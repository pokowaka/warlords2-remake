/**
 * @file
 * @brief Rendering of the terrain map, fog of war, stacks, strategic map, bottom bar, and menus.
 */
#include <algorithm>
#include <cmath>

#include "absl/strings/str_format.h"

#include "front/internal.hpp"
#include "front/pckimage.hpp"
#include "front/prefs.hpp"
#include "nlohmann/json.hpp"
#include "platform/display.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/uidata.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

/** @brief Loads every picture, font and the main screen's layout from the data directory. */
absl::Status LoadArt() {
    const std::filesystem::path& root = g_app.data_dir;
    auto loaded_pal = w2::pal::Load(root / "TERRAIN0/WAR2.PAL");
    if (!loaded_pal.ok()) return loaded_pal.status();
    g_app.palette = *loaded_pal;
    const auto& palette = g_app.palette;
    auto load_into = [&](gfx::ImageP& dest, const std::string& rel_path,
                         int key = pckimage::kNokey) -> absl::Status {
        auto img = pckimage::Load(root / rel_path, palette, key);
        if (!img.ok()) return img.status();
        dest = std::move(*img);
        return absl::OkStatus();
    };
    for (int i = 0; i < 2; i++) {
        if (absl::Status status =
                    load_into(g_app.sheets[i], "TERRAIN0/SCENERY" + std::to_string(i) + ".PCK");
            !status.ok()) {
            return status;
        }
    }
    if (absl::Status status = load_into(g_app.road_img, "TERRAIN0/ROAD.PCK", 1); !status.ok())
        return status;
    for (int i = 0; i <= 8; i++) {
        if (absl::Status status =
                    load_into(g_app.army_img[i], "TERRAIN0/A" + std::to_string(i) + ".PCK",
                              pckimage::kCorner);
            !status.ok()) {
            return status;
        }
    }
    if (absl::Status status = load_into(g_app.shadow_img, "TERRAIN0/ASHADOW.PCK", 15); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.marble, "PICS/MARBLE.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.big_army, "PICS/BIGARMY.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.city_pic, "PICS/CITY.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.war_pic, "PICS/WAR.PCK", 1); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.shield_img, "TERRAIN0/BSHIELD.PCK", 12); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.victory_pic, "PICS/VICTORY.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.hero_pic_m, "PICS/MHERO.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.hero_pic_f, "PICS/FHERO.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.atrans_shields, "TERRAIN0/ATRANS2.PCK", 1);
        !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.fog_img, "PICS/HIDDEN.PCK", 1); !status.ok())
        return status;
    auto curs_px = w2::pck::Decode(root / "PICS/CURS.PCK");
    if (!curs_px.ok()) return curs_px.status();
    g_app.curs_img = pckimage::FromPixels(**curs_px, palette, {{1, 15}, {4, 0}}, {0});
    if (absl::Status status = load_into(g_app.abits, "PICS/ABITS.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(g_app.pointer_img, "STAND.PCK", 10); !status.ok())
        return status;
    auto loaded_screen = screen::Load(root, palette, w2::uidata::kMainScreen, 0);
    if (!loaded_screen.ok()) return loaded_screen.status();
    g_app.screen = std::move(*loaded_screen);
    std::string shortcuts_json = prefs::Get("shortcuts");
    if (!shortcuts_json.empty()) {
        auto shortcut_list =
                nlohmann::ordered_json::parse(shortcuts_json, nullptr, /*allow_exceptions=*/false);
        if (!shortcut_list.is_discarded() && shortcut_list.is_array()) {
            for (size_t i = 0; i < shortcut_list.size(); i++) {
                if (shortcut_list[i].is_number_integer()) {
                    g_app.screen->ui.shortcuts[static_cast<int>(i)] = shortcut_list[i].get<int>();
                }
            }
        }
    }
    auto text_font = Font::Load(root, "TEXT", palette, 3);
    if (!text_font.ok()) return text_font.status();
    g_app.font = std::move(*text_font);
    auto chance17 = Font::Load(root, "CHANCE17", palette, 3);
    if (!chance17.ok()) return chance17.status();
    g_app.big_font = std::move(*chance17);
    auto chance36 = Font::Load(root, "CHANCE36", palette, 3);
    if (!chance36.ok()) return chance36.status();
    g_app.title_font = std::move(*chance36);
    if (absl::Status status = load_into(g_app.shields_img, "TERRAIN0/SHIELDS.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.city_back, "TERRAIN0/CITYBACK.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.search_pic, "PICS/SEARCH.PCK"); !status.ok())
        return status;
    if (absl::Status status = load_into(g_app.temple_pic, "PICS/TEMPLE.PCK"); !status.ok())
        return status;
    for (int i = 2; i <= 4; i++) {
        if (absl::Status status =
                    load_into(g_app.level_pic[i], "PICS/LEVEL" + std::to_string(i) + ".PCK");
            !status.ok()) {
            return status;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (absl::Status status =
                    load_into(g_app.medal_pic[i], "PICS/MEDAL" + std::to_string(i) + ".PCK");
            !status.ok()) {
            return status;
        }
    }
    return load_into(g_app.scroll_pic, "PICS/SCROLL.PCK", 10);
}

// ------------------------------------------------------------------ drawing

/** @brief The army a stack shows: the one first in the fight order. */
static Army* TopArmy(const std::vector<Army*>& stack) {
    Army* best = nullptr;
    int best_rank = kNone;
    for (Army* army : stack) {
        int rank = g_app.g->map->fight_order[OwnerOr8(army->owner)][army->type];
        if (best_rank == kNone || rank > best_rank) {
            best = army;
            best_rank = rank;
        }
    }
    return best;
}

/// The army type a stack at sea shows: the Navy's boat.
static const int kNavy = 5;
/**
 * @brief The figure a stack shows: its top army's, or a boat when an army in
 *        it is at sea.
 */
static int StackFigure(const std::vector<Army*>& stack) {
    for (Army* army : stack)
        if (army->at_sea) return kNavy;
    Army* top = TopArmy(stack);
    return top ? top->type : kNone;
}

/**
 * @brief Draws the hidden map: over each unseen tile, black through its
 *        HIDDEN.PCK cell; cell 14, a tile with nothing seen round it, is black.
 */
static void DrawFog() {
    int viewer = game::ViewerSide(*g_app.g, g_app.player);
    if (g_app.g->map->options.hidden_map == 0 || viewer == kNone) return;
    for (int tile_y = g_app.view[1]; tile_y <= g_app.view[3]; tile_y++) {
        for (int tile_x = g_app.view[0]; tile_x <= g_app.view[2]; tile_x++) {
            int cell = game::FogCell(*g_app.g, viewer, tile_x, tile_y);
            if (cell == kNone) continue;
            int pixel_x = tile_x * kTile, pixel_y = tile_y * kTile;
            if (cell == game::kFogBlack) {
                gfx::SetColor(0, 0, 0);
                gfx::Rectangle(gfx::kFill, pixel_x, pixel_y, kTile, kTile);
            } else {
                gfx::SetColor(1, 1, 1);
                gfx::Draw(g_app.fog_img, gfx::NewQuad((cell % 7) * 48, (cell / 7) * 41, 40, 40),
                          pixel_x, pixel_y);
            }
        }
    }
    gfx::SetColor(1, 1, 1);
}

/// Where in the side's sheet each flag size, 1 to 4 armies, is cut from.
static const int kFlagY[4] = {29, 38, 47, 56};
/**
 * @brief Draws a stack on the map, at the tile whose top-left map pixel is
 *        (pixel_x, pixel_y).
 *
 * The top army's figure at (8, 7) in the tile; the flag pole, three 40-pixel
 * lines in colours 14, 13 and 14; and the flag, from the side's own sheet at
 * (464, y) 48 x 8, bigger the more armies stand there. More than four fly
 * the smallest 8 lower as well, and the flag for the rest at the top.
 */
static void DrawStack(int owner, int army_type, int count, int pixel_x, int pixel_y) {
    if (owner < 0 || owner > 8) owner = 8;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.army_img[owner], kit::ArmyQuad(army_type), pixel_x + 8, pixel_y + 7);
    PalColour(14);
    gfx::Rectangle(gfx::kFill, pixel_x + 2, pixel_y, 1, kTile);
    gfx::Rectangle(gfx::kFill, pixel_x + 4, pixel_y, 1, kTile);
    PalColour(13);
    gfx::Rectangle(gfx::kFill, pixel_x + 3, pixel_y, 1, kTile);
    gfx::SetColor(1, 1, 1);
    auto flag = [&](int flag_size, int y) {
        gfx::Draw(g_app.army_img[owner], gfx::NewQuad(464, kFlagY[flag_size - 1], 48, 8), pixel_x,
                  y);
    };
    if (count > 4) {
        flag(1, pixel_y + 8);
        count -= 4;
    }
    flag(std::max(1, std::min(count, 4)), pixel_y);
}

/**
 * @brief Draws the map, in map pixels: the caller has the transform and the
 *        scissor up.
 *
 * The terrain, roads, ports and items first, then the encampments, then the
 * stacks, the hidden map, the route, the walking stack and the selection box.
 */
static void DrawMap() {
    g_app.view = ViewTiles();
    const auto& view = g_app.view;
    std::map<int, std::vector<Army*>> armies_on;
    for (Army* army : g_app.g->armies) {
        if (!army->transit && army->x != kNone && army->x >= view[0] && army->x <= view[2] &&
            army->y >= view[1] && army->y <= view[3])
            armies_on[army->x + army->y * 1000].push_back(army);
    }
    // one item a tile: the last in item order (the original walks them from
    // the end)
    std::map<int, w2::Item*> items_on_tile;
    for (int i = static_cast<int>(g_app.g->map->items.size()) - 1; i >= 0; i--) {
        w2::Item& item = g_app.g->map->items[i];
        if (item.status == 1 && item.x != kNone && !items_on_tile.count(item.x + item.y * 1000))
            items_on_tile[item.x + item.y * 1000] = &item;
    }
    /** @brief A stack to draw, once the ground is down. */
    struct Standing {
        std::vector<Army*> stack;  ///< Its armies, not counting any walking.
        int x, y, sx, sy;          ///< Its tile, and the tile's top-left map pixel.
        bool hidden = false;       ///< In an encampment and not selected: not drawn.
    };
    std::vector<Standing> standing;
    for (int tile_y = view[1]; tile_y <= view[3]; tile_y++) {
        for (int tile_x = view[0]; tile_x <= view[2]; tile_x++) {
            int pixel_x = tile_x * kTile, pixel_y = tile_y * kTile;
            int tile = w2::scn::TileAt(*g_app.g->map, tile_x, tile_y);
            int sheet = std::min(1, tile / 96);
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.sheets[sheet], TileQuad(tile), pixel_x, pixel_y);
            int road = w2::scn::RoadAt(*g_app.g->map, tile_x, tile_y);
            if (road != 0) gfx::Draw(g_app.road_img, RoadQuad(road - 1), pixel_x, pixel_y);
            // a port -- a crossing, map word bit 15 -- takes the anchor, at the
            // tile's corner after the roads
            if (w2::scn::IsCrossing(*g_app.g->map, tile_x, tile_y))
                gfx::Draw(g_app.atrans_shields, gfx::NewQuad(0, 0, 32, 29), pixel_x, pixel_y);
            // items on the ground: a planted standard as its side's flag, army
            // cell 29; anything else as the bag
            auto here = items_on_tile.find(tile_x + tile_y * 1000);
            if (here != items_on_tile.end()) {
                w2::Item* item = here->second;
                if (item->planted && item->standard_of != kNone)
                    gfx::Draw(g_app.army_img[item->standard_of], kit::ArmyQuad(29), pixel_x,
                              pixel_y);
                else
                    gfx::Draw(g_app.atrans_shields, gfx::NewQuad(64, 0, 32, 29), pixel_x, pixel_y);
            }
            auto armies_here = armies_on.find(tile_x + tile_y * 1000);
            if (armies_here != armies_on.end()) {
                std::vector<Army*> stack;
                for (Army* army : armies_here->second)
                    if (!(g_app.walk && g_app.walk->armies.count(army))) stack.push_back(army);
                if (!stack.empty())
                    standing.push_back(Standing{stack, tile_x, tile_y, pixel_x, pixel_y});
            }
        }
    }
    // the ground first, then the encampments, then the stacks
    for (auto& stack_here : standing) {
        if (game::TowerAt(*g_app.g, stack_here.x, stack_here.y)) {
            int owner = OwnerOr8(stack_here.stack[0]->owner);
            if (owner == 15) owner = 8;
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.road_img, gfx::NewQuad(owner * 48 + 192, 40, 48, 40), stack_here.sx,
                      stack_here.sy);
            stack_here.hidden = !(g_app.selection && g_app.selection->x == stack_here.x &&
                                  g_app.selection->y == stack_here.y);
        }
    }
    // the selected stack's tile shows the lead of the group that moves
    Army* lead = g_app.selection && !g_app.selection->stack.empty()
                         ? TopArmy(g_app.selection->stack)
                         : nullptr;
    if (lead && g_app.walk && g_app.walk->armies.count(lead)) lead = nullptr;
    for (auto& stack_here : standing) {
        if (stack_here.hidden) continue;
        Army* top = TopArmy(stack_here.stack);
        int figure = StackFigure(stack_here.stack);
        if (lead && g_app.selection->x == stack_here.x && g_app.selection->y == stack_here.y) {
            top = lead;
            figure = lead->at_sea ? kNavy : lead->type;
        }
        DrawStack(OwnerOr8(top->owner), figure, static_cast<int>(stack_here.stack.size()),
                  stack_here.sx, stack_here.sy);
    }
    DrawFog();
    DrawRoute();
    // the stack itself, wherever the walk has got to
    const auto* walk_pos = WalkingAt();
    if (walk_pos) {
        Army* walker = g_app.walk->top;
        if (walker && Shown(walk_pos->first, walk_pos->second)) {
            std::vector<Army*> walking(g_app.walk->armies.begin(), g_app.walk->armies.end());
            Army* top = TopArmy(walking);
            int figure = g_app.walk->sea_before ? kNavy : (top ? top->type : kNone);
            DrawStack(OwnerOr8(walker->owner), figure, static_cast<int>(walking.size()),
                      walk_pos->first * kTile, walk_pos->second * kTile);
        }
    }
    // the selection box, which follows the walk
    if (g_app.selection) {
        int x = walk_pos ? walk_pos->first : g_app.selection->x;
        int y = walk_pos ? walk_pos->second : g_app.selection->y;
        if (Shown(x, y)) {
            gfx::SetColor(1, 1, 1);
            int base = g_app.selection->stack.size() > 1 ? 4 : 0;
            int frame = base + static_cast<int>(std::floor(w2::Now() * kCursRate)) % 4;
            gfx::Draw(g_app.curs_img, gfx::NewQuad((frame % 4) * 64, (frame / 4) * 40, 40, 40),
                      x * kTile, y * kTile);
        }
    }
}

/// The side the strategic map's image was made for; -99 when it must be made anew.
static int strat_viewer = -99;

/**
 * @brief The strategic map as the original paints it anywhere it appears.
 *
 * Every city the viewer can see is an 8x8 shield. `mark` gets a white box;
 * `owners` is who held each city, for History (0xff a razed one). Only
 * `rows` of it show, from `scroll` rows down. In the modern UI an ally's
 * war target gets a box of its own.
 */
void DrawStrategicMap(int x, int y, const City* mark, bool no_cities,
                      const std::vector<int>* owners, int scroll, int rows) {
    int viewer = game::ViewerSide(*g_app.g, g_app.player);
    if (!g_app.strat_image || strat_viewer != viewer) {
        g_app.strat_image = screen::StrategicImage(*g_app.screen, *g_app.g, viewer);
        strat_viewer = viewer;
    }
    gfx::SetScissor(x, y, 224, rows);
    y -= scroll;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.strat_image, x, y);
    if (!no_cities) {
        auto& cities = g_app.g->map->cities;
        for (size_t i = 0; i < cities.size(); i++) {
            const City& city = cities[i];
            int owner = owners && i < owners->size() ? (*owners)[i] : kNone;
            bool gone = owners ? owner == 0xff : city.razed;
            if (!gone && game::Seen(*g_app.g, viewer, city.x, city.y)) {
                int side = owners ? owner : OwnerOr8(city.owner_index);
                gfx::Draw(g_app.atrans_shields, gfx::NewQuad(side * 16, 30, 8, 8),
                          x + city.x * 2 - 1, y + city.y * 2 - 1);
            }
        }
    }
    if (mark) {
        int mark_x = x + mark->x * 2 - 1, mark_y = y + mark->y * 2 - 1;
        PalColour(15);
        gfx::Rectangle(gfx::kFill, mark_x - 1, mark_y - 1, 10, 1);
        gfx::Rectangle(gfx::kFill, mark_x - 1, mark_y + 9, 10, 1);
        gfx::Rectangle(gfx::kFill, mark_x - 1, mark_y - 1, 1, 10);
        gfx::Rectangle(gfx::kFill, mark_x + 9, mark_y - 1, 1, 10);
    }
    // Highlight any Allied partner's broadcasted war target city on the strategic map
    if (g_app.modern_ui && !no_cities && g_app.player && viewer != kNone) {
        for (int ally = 0; ally < 8; ally++) {
            if (ally != g_app.player->index &&
                w2::diplomacy::IsAllied(*g_app.g, g_app.player->index, ally)) {
                int target_index = w2::diplomacy::AlliedTargetCity(*g_app.g, ally);
                if (const City* target = g_app.g->map->City(target_index)) {
                    if (!target->razed && game::Seen(*g_app.g, viewer, target->x, target->y)) {
                        int target_x = x + target->x * 2 - 1, target_y = y + target->y * 2 - 1;
                        PalColour(11);
                        gfx::Rectangle(gfx::kFill, target_x - 2, target_y - 2, 12, 1);
                        gfx::Rectangle(gfx::kFill, target_x - 2, target_y + 10, 12, 1);
                        gfx::Rectangle(gfx::kFill, target_x - 2, target_y - 2, 1, 12);
                        gfx::Rectangle(gfx::kFill, target_x + 10, target_y - 2, 1, 12);
                    }
                }
            }
        }
    }
    gfx::SetScissor();
}

/**
 * @brief Every site shown to the side, marked on a strategic map at (x, y).
 *
 * A temple, a searched site, a rich one and the rest each have their own
 * marker; `highlight` gets a white outline.
 */
void DrawSiteMarkers(int x, int y, const w2::Site* highlight) {
    int viewer = game::ViewerSide(*g_app.g, g_app.player);
    gfx::SetScissor(x, y, 224, 312);
    for (auto& site : g_app.g->map->sites) {
        if (!(viewer == kNone || (w2::site::ShownTo(site, *g_app.player) &&
                                  game::Seen(*g_app.g, viewer, site.x, site.y))))
            continue;
        int src_x, src_y;
        if (site.content == w2::site::kTemple) {
            src_x = 112;
            src_y = 20;
        } else if (site.searched) {
            src_x = 112;
            src_y = 10;
        } else if (site.rich) {
            src_x = 128;
            src_y = 0;
        } else {
            src_x = 112;
            src_y = 0;
        }
        int marker_x = (site.x * 2 - 1 + 4) / 8 * 8;
        int marker_y = site.y * 2 - 1;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.atrans_shields, gfx::NewQuad(src_x, src_y, 16, 10), x + marker_x,
                  y + marker_y);
        if (&site == highlight) {
            kit::SetPal(15);
            kit::Outline(x + marker_x, y + marker_y, 12, 10);
        }
    }
    gfx::SetScissor();
}

/// How many rows down the main screen's strategic map is scrolled (FollowView).
int strat_scroll = 0;

/**
 * @brief Scrolls the main screen's strategic map to keep the view's box in
 *        its middle.
 *
 * Not the original's: on a short screen the strategic map's frame shows only
 * some of it, so that a click towards an edge brings more of the world in
 * from that side.
 */
static void FollowView() {
    const auto& rect = g_app.strat_rect;
    int full = g_app.g->map->height * 2;
    auto [view_w, view_h] = ViewSize();
    // a whole tile at a time
    int scroll = static_cast<int>(std::lround(g_app.cy + view_h / 2 - rect.h / 4.0)) * 2;
    strat_scroll = std::clamp(scroll, 0, std::max(0, full - rect.h));
}

/**
 * @brief Draws the main screen's strategic map with the view's box on it:
 *        whatever the view covers, to the device pixel.
 */
static void DrawStrategic() {
    const auto& rect = g_app.strat_rect;
    FollowView();
    DrawStrategicMap(rect.x, rect.y, nullptr, false, nullptr, strat_scroll, rect.h);
    gfx::SetScissor(rect.x, rect.y, rect.w, rect.h);
    // the view box: whatever the view covers, to the device pixel
    PalColour(15);
    double scale = display::state.scale;
    auto snap = [scale](double value) { return std::floor(value * scale + 0.5) / scale; };
    auto [view_w, view_h] = ViewSize();
    double box_x = rect.x + snap(g_app.cx * 2), box_y = rect.y - strat_scroll + snap(g_app.cy * 2);
    double box_w = snap(view_w * 2), box_h = snap(view_h * 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y, box_w, 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y + box_h - 2, box_w, 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y, 2, box_h);
    gfx::Rectangle(gfx::kFill, box_x + box_w - 2, box_y, 2, box_h);
    gfx::SetScissor();
}

/** @brief Has the strategic map's image made anew before it is next drawn. */
void StratDirty() {
    g_app.strat_image.reset();
    strat_viewer = -99;
}

/**
 * @brief Paints the four configurable buttons a second time, each with the
 *        icon of the item assigned to it.
 *
 * From MENUBUTT.PCK at the rect the assigned item carries in UDB.DAT; with
 * no art, the item's name cut to six letters.
 */
static void DrawShortcutIcons() {
    auto& main_ui = g_app.screen->ui;
    auto art = g_app.screen->ArtFor(w2::uidata::kShortcutBitmap);
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        auto control = screen::FindControl(*g_app.screen, w2::uidata::kShortcutFirst + i);
        auto shortcut = main_ui.shortcuts.find(i);
        if (!control || shortcut == main_ui.shortcuts.end()) continue;
        auto item = main_ui.shortcut_items.find(shortcut->second);
        if (item == main_ui.shortcut_items.end()) continue;
        if (art) {
            auto src = item->second.src[g_app.screen->StateOf(control->id)];
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(src.x, src.y, item->second.w, item->second.h),
                      control->x, control->y);
        } else {
            std::string short_name = item->second.name.substr(0, 6);
            gfx::SetColor(1, 1, 1);
            g_app.font->Draw(
                    short_name,
                    control->x + std::max(1, (control->w - g_app.font->Width(short_name)) / 2),
                    control->y + (control->h - g_app.font->line_height) / 2);
        }
    }
}

/**
 * @brief Draws the turn and the shields of the sides still in it, at the
 *        right of the menu bar; the side whose turn it is is boxed.
 */
static void DrawTurnStrip() {
    PalColour(15);
    gfx::Rectangle(gfx::kFill, 437, 0, 202, 17);
    std::vector<Side*> alive;
    for (Side* side : g_app.g->sides)
        if (side->alive) alive.push_back(side);
    int count = static_cast<int>(alive.size());
    std::string turn = absl::StrFormat("Turn %d", g_app.g->turn);
    const Font& font = g_app.big_font->Colours(0, 15);
    font.Draw(turn, (8 - count) * 16 + 508 - font.Width(turn), 0);
    const Side* active_side = (g_app.ai_side && !HumansLeft()) ? g_app.ai_side : g_app.player;
    for (int i = 0; i < count; i++) {
        int x = (8 - count + i) * 16 + 512;
        if (alive[i] == active_side) {
            PalColour(0);
            gfx::Rectangle(gfx::kFill, x - 3, 1, 17, 15);
        }
        int side_index = alive[i]->index;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.atrans_shields,
                  gfx::NewQuad(112 + (side_index / 4) * 16, 94 + (side_index % 4) * 14, 16, 14), x,
                  2);
    }
}

/**
 * @brief Draws the menu bar, and the open menu's dropdown.
 *
 * White, the titles in TEXT, black. An open title and the row under the
 * pointer are XORed with colour 7; a greyed item's glyph is colour 3. The
 * dropdown is white with a black outline and a shadow.
 */
static void DrawMenuBar() {
    PalColour(15);
    gfx::Rectangle(gfx::kFill, 0, 0, g_app.layout.w, menu::kBarH);
    for (size_t i = 0; i < g_app.menu_layout.size(); i++) {
        const auto& menu_entry = g_app.menu_layout[i];
        bool lit = static_cast<int>(i) == g_app.open_menu;
        if (lit) {
            PalColour(8);
            gfx::Rectangle(gfx::kFill, menu_entry.x, 0, menu_entry.w, menu::kBarH);
        }
        gfx::SetColor(1, 1, 1);
        g_app.font->Colours(lit ? 7 : 0, lit ? 8 : 15)
                .Draw(menu_entry.title, menu_entry.x + 2, menu::kBarY);
    }
    if (!g_app.starting && g_app.g) {
        gfx::Push();
        gfx::Translate(g_app.layout.ex, 0);
        DrawTurnStrip();
        gfx::Pop();
    }
    if (g_app.open_menu < 0 || g_app.open_menu >= static_cast<int>(g_app.menu_layout.size()))
        return;
    const auto& drop = g_app.menu_layout[g_app.open_menu].drop;
    PalColour(15);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y, drop.w - 1, drop.h - 1);
    PalColour(0);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y + drop.h - 2, drop.w - 2, 1);
    gfx::Rectangle(gfx::kFill, drop.x + 2, drop.y + drop.h - 1, drop.w - 2, 1);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y, 1, drop.h - 2);
    gfx::Rectangle(gfx::kFill, drop.x + drop.w - 2, drop.y, 1, drop.h - 1);
    gfx::Rectangle(gfx::kFill, drop.x + drop.w - 1, drop.y + 1, 1, drop.h - 1);
    const menu::Row* hover =
            menu::RowAt(g_app.menu_layout, g_app.open_menu, g_app.mouse_x, g_app.mouse_y);
    for (const auto& row : drop.rows) {
        if (row.label == "-") {
            PalColour(0);
            gfx::Rectangle(gfx::kFill, row.x + 1, row.y - 1, drop.w - 2, 1);
            continue;
        }
        std::string key = row.key.empty() ? row.act : row.key;
        bool grey = !MenuEnabled(key);
        bool lit = &row == hover && !grey;
        if (lit) {
            PalColour(8);
            gfx::Rectangle(gfx::kFill, row.x + 1, row.y, drop.w - 3, row.h);
        }
        int glyph = grey ? 3 : (lit ? 7 : 0);
        const Font& font = g_app.font->Colours(glyph, lit ? 8 : 15);
        gfx::SetColor(1, 1, 1);
        font.Draw(row.label, row.x + 3, row.y);
        if (!row.key.empty()) font.Draw(row.key, row.x + drop.key_col, row.y);
        // the setting in use (not the original's): a diamond in the key column
        if (MenuTicked(key)) {
            PalColour(glyph);
            int centre_x = row.x + drop.key_col + 2, centre_y = row.y + row.h / 2;
            for (int j = -2; j <= 2; j++) {
                int half = 2 - std::abs(j);
                gfx::Rectangle(gfx::kFill, centre_x - half, centre_y + j, half * 2 + 1, 1);
            }
        }
    }
}

// The bottom bar's eight army slots and the mark under each are real
// controls -- ids 224-231 the slots, 232-239 the marks, 240/241 the Grp
// button -- drawn from ABITS.PCK and the army sheets at the rects the
// original uses (docs/re/ui.md > The army slots).
static const int kSlotX = 24, kSlotY = 405, kSlotStep = 40;
static const int kSlotMovesX = 8, kSlotMovesY = 31;
static const int kMarkY = 449, kMarkW = 32, kMarkH = 16;

/** @brief Draws the ABITS.PCK cell at (src_x, src_y), width x height, at (x, y). */
static void DrawAbits(int src_x, int src_y, int width, int height, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.abits, gfx::NewQuad(src_x, src_y, width, height), x, y);
}

/** @brief Draws a number in ABITS's own 8x8 digits, always two of them ("%02d"). */
static void DrawDigits(int number, int x, int y) {
    std::string text = absl::StrFormat("%02d", std::max(0, std::min(99, number)));
    for (size_t i = 0; i < text.size(); i++)
        DrawAbits(64 + (text[i] - '0') * 8, 30, 8, 8, x + static_cast<int>(i) * 8, y);
}

/**
 * @brief The ring a slot's army sits in: each group a colour of its own,
 *        counted round from the current player's.
 */
static int GroupRing(int group) {
    return (g_app.player->index + group) % 8 + 1;
}

/**
 * @brief Draws the bottom bar with a stack selected: its eight slots, the
 *        group's movement, the Grp button and the way the group travels.
 */
static void DrawArmySlots() {
    const slots_mod::Slots* slots = g_app.selection ? &g_app.selection->slots : nullptr;
    for (int i = 0; i < kSlotCount; i++) {
        int x = kSlotX + i * kSlotStep;
        Army* army = slots && i < slots->n ? slots->army[i] : nullptr;
        gfx::SetColor(1, 1, 1);
        int ring = army ? GroupRing(slots->group[i]) : 0;
        gfx::Draw(g_app.abits, gfx::NewQuad(ring * 32, 0, 32, 30), x, kSlotY);
        if (army) {
            // the armies not moving with the group are drawn as ghosts
            gfx::Draw(slots->in_group[i] ? g_app.army_img[g_app.player->index] : g_app.shadow_img,
                      kit::ArmyQuad(army->type), x, kSlotY);
            DrawDigits(army->moves, x + kSlotMovesX, kSlotY + kSlotMovesY);
            if (slots->mark[i] != slots_mod::kNomark)
                DrawAbits(448, slots->mark[i] == slots_mod::kTick ? 16 : 0, kMarkW, kMarkH, x,
                          kMarkY);
        }
    }
    // "Group Move", the group's movement, and the Grp button
    DrawAbits(0, 30, 32, 8, 344, 420);
    DrawAbits(32, 30, 32, 8, 344, 428);
    DrawDigits(slots ? slots_mod::Moves(*slots) : 0, 352, 436);
    bool green = slots && slots_mod::Grouped(*slots);
    DrawAbits(288, green ? 19 : 0, 32, 19, 344, 447);
    // the way the moving group travels
    std::vector<Army*> moving;
    if (slots)
        for (int i = 0; i < slots->n; i++)
            if (slots->in_group[i]) moving.push_back(slots->army[i]);
    int icon = kNone;
    if (!moving.empty()) {
        bool sea = true, woods = false, hills = false;
        for (Army* army : moving) {
            const w2::ArmyType* army_type = g_app.g->types.ById(army->type);
            if (army_type->woods_move) woods = true;
            if (army_type->hills_move) hills = true;
            if (!army->at_sea) sea = false;
        }
        if (move::ModeOf(*g_app.g, moving) == move::kFlying)
            icon = 184;
        else if (sea)
            icon = 424;
        else if (woods && hills)
            icon = 216;
        else if (woods)
            icon = 248;
        else if (hills)
            icon = 152;
    }
    if (icon != kNone) {
        DrawAbits(icon, 30, 32, 10, 344, 407);
    } else {
        PalColour(3);
        gfx::Rectangle(gfx::kFill, 344, 407, 32, 10);
    }
}

/**
 * @brief One of the four cells of the side's standing on the bottom bar.
 *
 * With nothing selected the bar shows the side's standing: cities, treasury,
 * income and upkeep, four 40 x 20 cells of ABITS.PCK each with a number
 * beside it.
 */
struct StatusCell {
    int sx, sy, x, tx;  ///< The cell's place in ABITS.PCK, its x, and the number's x.
    const char* fmt;    ///< How the number is written.
};
static const StatusCell kStatusCells[4] = {{344, 0, 32, 72, "%d"},
                                           {344, 20, 120, 144, "%dgp"},
                                           {384, 0, 200, 232, "%dgp"},
                                           {384, 20, 280, 320, "%dgp"}};
static const int kStatusY = 425;

/** @brief The number in standing cell `cell`: cities, gold, income or upkeep. */
static int StatusValue(int cell) {
    switch (cell) {
    case 0:
        return static_cast<int>(game::SideCities(*g_app.g, *g_app.player).size());
    case 1:
        return g_app.player->gold;
    case 2:
        return game::Income(*g_app.g, *g_app.player);
    default:
        return game::Upkeep(*g_app.g, *g_app.player);
    }
}

/** @brief Draws the side's standing on the bottom bar, nothing being selected. */
static void DrawStatus() {
    for (int cell = 0; cell < 4; cell++) {
        const auto& status_cell = kStatusCells[cell];
        DrawAbits(status_cell.sx, status_cell.sy, 40, 20, status_cell.x, kStatusY);
        g_app.big_font->Draw(w2::Format(status_cell.fmt, StatusValue(cell)), status_cell.tx,
                             kStatusY);
    }
}

/**
 * @brief Draws a computer's turn in the status bar.
 *
 * A line centred on (196, 415) in the side's colours, a black frame at
 * (36, 442) 320x22, and the first progress / 5 * 16 pixels of the side's
 * MOVEBAR<n>.PCK at (32, 444).
 */
static void DrawComputerStatus() {
    const AiStatus& status = *g_app.ai_status;
    Side* side = status.side;
    gfx::SetColor(1, 1, 1);
    kit::Centred(g_app.big_font->Colours(side->colour, side->edge), status.text, 196, 415);
    PalColour(0);
    kit::Outline(36, 442, 320, 22);
    int bar_width = status.progress / 5 * 16;
    if (bar_width > 0) {
        int side_index = side->index;
        if (!g_app.move_bars.count(side_index)) {
            auto bar = pckimage::Load(
                    g_app.data_dir / "TERRAIN0" / ("MOVEBAR" + std::to_string(side_index) + ".PCK"),
                    g_app.palette, pckimage::kCorner);
            g_app.move_bars[side_index] = bar.ok() ? std::move(*bar) : nullptr;
        }
        gfx::SetColor(1, 1, 1);
        if (g_app.move_bars[side_index])
            gfx::Draw(g_app.move_bars[side_index], gfx::NewQuad(0, 0, std::min(bar_width, 320), 18),
                      32, 444);
    }
}

/**
 * @brief The right button on the bottom bar: an information box about the
 *        slot or the standing cell under the pointer.
 */
void BarInfo(int x, int y) {
    int bar_x = x - g_app.layout.bar.x;
    if (g_app.ai_status) return;
    if (g_app.selection) {
        if (bar_x < 336) {
            int slot = (bar_x - 16) / 40;
            if (bar_x >= 16 && slot < g_app.selection->slots.n)
                infobox::Army(x, y, g_app.selection->slots.army[slot]);
            else
                infobox::ShowLines(x, y, "Select Army", "Select armies when present");
        } else {
            infobox::ShowLines(x, y, "Group/Ungroup", "Manipulate all armies");
        }
        return;
    }
    static const char* const kHelp[4][2] = {{"Number of Cities", "You have %d cities!"},
                                            {"Your Treasury", "You have %d gold!"},
                                            {"Your Income", "You earn %d gold!"},
                                            {"Your Upkeep", "You pay %d gold!"}};
    int cell = (bar_x - 16) / 90;
    if (bar_x >= 16 && cell >= 0 && cell < 4)
        infobox::ShowLines(x, y, kHelp[cell][0], w2::Format(kHelp[cell][1], StatusValue(cell)));
}

/** @brief Draws the bottom bar: the computer's turn, the army slots, or the standing. */
static void DrawBottomBar() {
    // The original blits MARBLE.PCK over the bar, (16, 403) 360x66 from (0, 60).
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.marble, gfx::NewQuad(0, 60, 360, 66), 16, 403);
    if (g_app.ai_status)
        DrawComputerStatus();
    else if (g_app.selection)
        DrawArmySlots();
    else
        DrawStatus();
}

// The city dialog's Vector mode paints the map its own way: a marker on each
// of the side's cities from ATRANS2.PCK's row at y = 94, and lines for the
// vectors.
static const int kVmarkX[7] = {0, 32, 48, 16, 64, 80, 96};

/** @brief Draws a 1-pixel Bresenham line from (`start_x`, `start_y`) to (`end_x`, `end_y`), both
 * ends included. */
static void LinePixels(int start_x, int start_y, int end_x, int end_y) {
    int dx = std::abs(end_x - start_x), dy = -std::abs(end_y - start_y);
    int step_x = start_x < end_x ? 1 : -1, step_y = start_y < end_y ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx::Rectangle(gfx::kFill, start_x, start_y, 1, 1);
        if (start_x == end_x && start_y == end_y) break;
        int err2 = 2 * err;
        if (err2 >= dy) {
            err += dy;
            start_x += step_x;
        }
        if (err2 <= dx) {
            err += dx;
            start_y += step_y;
        }
    }
}

void DrawVectorMap(int x, int y, City* city, int filter, bool see_all) {
    if (!g_app.strat_image)
        g_app.strat_image = screen::StrategicImage(*g_app.screen, *g_app.g, g_app.player->index);
    gfx::SetScissor(x, y, 224, 312);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.strat_image, x, y);
    auto marker = [&](const City* target_city, int mark_idx) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.atrans_shields, gfx::NewQuad(kVmarkX[mark_idx], 94, 16, 10),
                  x + target_city->x * 2 - 2, y + target_city->y * 2 - 1);
    };
    auto line = [&](const City* from_city, const City* to_city, int colour) {
        PalColour(colour);
        LinePixels(x + from_city->x * 2 + 2, y + from_city->y * 2 + 2, x + to_city->x * 2 + 2,
                   y + to_city->y * 2 + 2);
    };
    auto mine = game::SideCities(*g_app.g, *g_app.player);
    auto building = [](const City* target_city) { return game::Producing(*target_city) ? 0 : 3; };
    auto [std_x, std_y] = game::StandardAt(*g_app.g, *g_app.player);
    auto to_standard = [&, flag_x = std_x, flag_y = std_y](const City* src_city, int colour) {
        PalColour(colour);
        LinePixels(x + src_city->x * 2 + 2, y + src_city->y * 2 + 2, x + flag_x * 2,
                   y + flag_y * 2);
    };
    if (see_all) {
        std::set<const City*> done;
        for (City* owned_city : mine) {
            if (done.count(owned_city)) continue;
            done.insert(owned_city);
            auto incoming = game::VectoredTo(*g_app.g, owned_city);
            int mark_idx = building(owned_city);
            if (owned_city->vector_to != kNone) mark_idx = 2;
            if (!incoming.empty()) mark_idx = game::Producing(*owned_city) ? 1 : 5;
            marker(owned_city, mark_idx);
            City* dest = owned_city->vector_to >= 0 ? g_app.g->map->City(owned_city->vector_to)
                                                    : nullptr;
            if (dest) {
                done.insert(dest);
                marker(dest, game::Producing(*dest) ? 1 : 5);
                line(dest, owned_city, 7);
            } else if (owned_city->vector_to == game::kStandard && std_x != kNone) {
                to_standard(owned_city, 8);
            }
            for (City* src : incoming) {
                done.insert(src);
                marker(src, 2);
                line(src, owned_city, 8);
            }
        }
        PalColour(15);
        int box_x = x + (city ? city->x : -10) * 2 - 3;
        int box_y = y + (city ? city->y : -10) * 2 - 2;
        gfx::Rectangle(gfx::kFill, box_x, box_y, 11, 1);
        gfx::Rectangle(gfx::kFill, box_x, box_y + 11, 11, 1);
        gfx::Rectangle(gfx::kFill, box_x, box_y, 1, 11);
        gfx::Rectangle(gfx::kFill, box_x + 11, box_y, 1, 11);
    } else if (city) {
        for (City* owned_city : mine) {
            int vec_count = static_cast<int>(game::VectoredTo(*g_app.g, owned_city).size());
            bool full =
                    (filter == -1 && vec_count + 1 > game::kMaxVectoredTo) ||
                    (filter >= 0 && filter != kNone && vec_count + filter > game::kMaxVectoredTo);
            if (owned_city == city || !full) {
                int mark_idx = building(owned_city);
                if (owned_city == city) mark_idx = game::Producing(*owned_city) ? 4 : 6;
                marker(owned_city, mark_idx);
            }
        }
        City* dest = city->vector_to >= 0 ? g_app.g->map->City(city->vector_to) : nullptr;
        if (dest) {
            marker(dest, game::Producing(*dest) ? 1 : 5);
            line(city, dest, 7);
        } else if (city->vector_to == game::kStandard && std_x != kNone) {
            to_standard(city, 7);
        }
        for (City* src : game::VectoredTo(*g_app.g, city)) {
            marker(src, 2);
            line(src, city, 8);
        }
    }
    if (std_x != kNone) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.atrans_shields, gfx::NewQuad(96, 15, 16, 15),
                  x + std::max(0, (std_x * 2 - 2) / 8 * 8), y + std::max(0, std_y * 2 - 6));
    }
    gfx::SetScissor();
}

// The figure is (96, 0) outlined in white everywhere but the hero dialog,
// which draws the other heroes with (96, 15), dark.
void DrawHeroFigure(int x, int y, int tile_x, int tile_y, bool dark) {
    gfx::SetScissor(x, y, 224, 312);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.atrans_shields, gfx::NewQuad(96, dark ? 15 : 0, 16, 15),
              x + std::max(0, (tile_x * 2 - 2) / 8 * 8), y + std::max(0, tile_y * 2 - 6));
    gfx::SetScissor();
}

void DrawStrategicPanel(int x, int y, const City* city) {
    DrawStrategicMap(x, y, city);
}

/** @brief Draws the start-of-turn banner with CITY.PCK framed in the side's colours. */

/** @brief Runs `draw` translated into the popups' 640x480 frame centred on the screen. */
static void InDialogFrame(const std::function<void()>& draw) {
    gfx::Push();
    gfx::Translate(g_app.layout.dialog.x, g_app.layout.dialog.y);
    draw();
    gfx::Pop();
}

/** @brief Runs `draw` translated by `offset` for one of the fixed screen groups. */
static void InGroup(const layout::Offset& offset, const std::function<void()>& draw) {
    gfx::Push();
    gfx::Translate(offset.x, offset.y);
    draw();
    gfx::Pop();
}

/** @brief Draws all open modal dialogs in stack order. */
static void DrawModals() {
    // a dialog may push another while drawing is under way: a copy is drawn
    auto modals = g_app.modals;
    for (auto& modal : modals) modal->Draw();
}

/** @brief Updates sound playback and the top modal dialog each frame. */
void Update() {
    sound::Update();
    if (auto top = kit::TopShared()) top->Update();
}

/**
 * @brief Renders one frame: chrome in UI pixels, map at its own zoom, then dialogs and pointer.
 */
void Draw() {
    gfx::Begin();
    gfx::Clear(0, 0, 0);
    SyncLayout();
    StepComputer();
    display::PushUi();
    if (g_app.starting || !g_app.g) {
        DrawModals();
        DrawMenuBar();
        DrawPointer();
        gfx::Pop();
        return;
    }
    AdvanceWalk();
    // a quest that ended is told once the screen is the player's again
    if (g_app.player && g_app.player->quest_news &&
        !(g_app.over || g_app.banner || g_app.offer || g_app.assault || g_app.victory ||
          g_app.victory_under || g_app.walk || g_app.move_all || g_app.ai_run ||
          g_app.open_menu >= 0 || kit::Top())) {
        questnews::Poll();
    }
    RefreshControls();
    screen::DrawBackground(*g_app.screen, g_app.layout);
    const auto& map_r = g_app.map_rect;
    if (g_app.map_after_banner) {
        // a hotseat turn not yet taken up: both maps blank (hotseatHidden)
        const auto& strat_r = g_app.strat_rect;
        gfx::SetColor(0, 0, 0);
        gfx::Rectangle(gfx::kFill, map_r.x, map_r.y, map_r.w, map_r.h);
        gfx::Rectangle(gfx::kFill, strat_r.x, strat_r.y, strat_r.w, strat_r.h);
        gfx::SetColor(1, 1, 1);
    } else {
        gfx::SetScissor(map_r.x, map_r.y, map_r.w, map_r.h);
        auto [cam_x, cam_y] = CamDev();
        display::PushMap(map_r.x, map_r.y, cam_x, cam_y, g_app.zoom);
        DrawMap();
        if (g_app.assault && g_app.assault->phase == "cloud") DrawCloud();
        gfx::Pop();
        gfx::SetScissor();
        DrawStrategic();
    }
    // Refill the control panel with marble before the controls go on.
    InGroup(g_app.layout.panel, []() {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.marble, gfx::NewQuad(0, 0, 224, 114), 400, 355);
    });
    screen::DrawControls(*g_app.screen);
    DrawShortcutIcons();
    InGroup(g_app.layout.bar, DrawBottomBar);
    InDialogFrame([]() {
        if (g_app.victory_under) DrawVictory(*g_app.victory_under);
        DrawModals();
    });
    DrawMenuBar();
    // the turn opens with the banner over the offer, and is dismissed first
    InDialogFrame([]() {
        if (g_app.offer) DrawHeroOffer();
        if (g_app.assault) DrawAssault();
        if (g_app.victory) DrawVictory(*g_app.victory);
        if (g_app.banner) DrawBanner();
    });
    DrawPointer();
    gfx::Pop();
}

}  // namespace front
