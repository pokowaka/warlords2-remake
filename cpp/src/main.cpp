/**
 * @file
 * @brief Warlords II, in the original's own interface, on SDL2.
 *
 * A port of the Lua remake's front end (love2d/main.lua). The screen is
 * 640x480 and every rect in it comes from the game's own layout files:
 * DATA/JOIN.DAT names the dialog's controls and clickable regions, AREA.DAT
 * places the regions, BUTTON.DAT the controls, and FILE.DAT says which .PCK
 * each control is cut from. See docs/re/ui.md and docs/formats/screens.md.
 *
 *     warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window]
 *               [--size WxH] [--script FILE]
 *
 * --size opens a window of exactly W x H device pixels, to try a screen
 * other than this one (the Steam Deck's is 1280x800).
 *
 * Run from the repository root, or point --data at your copy of the game.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>

#include <SDL.h>

#include "front/app.hpp"
#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/menu.hpp"
#include "front/pckimage.hpp"
#include "front/prefs.hpp"
#include "front/screen.hpp"
#include "platform/coroutine.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "ui/kit.hpp"
#include "util/json.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/cues.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/pal.hpp"
#include "warlords/quest.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"
#include "warlords/uidata.hpp"

// The front end's shared state; its name is shared with ui/ and goes away
// when the front end is restructured.
App G;  // NOLINT(readability-identifier-length)

using w2::Army;
using w2::City;
using w2::Format;
using w2::kNone;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

static const int kTile = screen::kTile;
static const double kCursRate = 18.2 / 4;  ///< CURS.PCK frames a second.

/// Dice for what does not touch the game's own dice (front::Roll).
static std::mt19937 loose_dice(static_cast<unsigned>(time(nullptr)));

namespace front {
/** @brief A random roll for what does not touch the game's dice: 1..n. */
int Roll(int n) {
    return n <= 0 ? 1 : static_cast<int>(loose_dice() % static_cast<unsigned>(n)) + 1;
}
/** @brief Puts a line in the status bar. */
void Say(const std::string& message) {
    G.status = message;
}
}  // namespace front

using front::Say;

// ------------------------------------------------------------------ forwards

static void BeginGame();
static void ShowBanner(Side* side);
static void FinishRound(Side* side);
static void PlayComputers(Side* side);
static void RefreshControls();
static void MoveSelection(int x, int y);
static void StartAssault(int x, int y, const w2::Battle& result);
static void PresentVictory(City* city, const std::string& victor);
static void Keypressed(const std::string& key);
static void Select(int x, int y, Army* pick = nullptr);
static void AfterBanner();
static void RefreshRoute();
static void SyncSelection();
static bool MenuEnabled(const std::string& key);
static void MenuPick(const std::string& key);
static void ResumeComputer();
static std::map<std::string, std::function<void()>>& MenuDoes();

// ------------------------------------------------------------------ helpers

/** @brief The quad of terrain tile `tile` in its scenery sheet (96 a sheet, 16 a row). */
static gfx::Quad TileQuad(int tile) {
    int cell = tile % 96;
    return gfx::NewQuad((cell % 16) * kTile, (cell / 16) * kTile, kTile, kTile);
}
/** @brief The quad of road piece `road` in ROAD.PCK (13 a row). */
static gfx::Quad RoadQuad(int road) {
    return gfx::NewQuad((road % 13) * 48, (road / 13) * kTile, kTile, kTile);
}
/** @brief Sets the drawing colour to palette entry `colour`. */
static void PalColour(int colour) {
    kit::SetPal(colour);
}
/**
 * @brief Whether any of the named keys is held.
 *
 * Only the keys the game saw go down, and has not seen come up or lost with
 * the window's focus (keys::clear). Not SDL_GetModState: a modifier held as
 * the window loses focus (Ctrl+arrow to another Space, Cmd+Tab) stays "down"
 * there, and a phantom Ctrl or Alt turns every click into another command.
 */
static bool Held(std::initializer_list<const char*> key_names) {
    return keys::IsDown(key_names);
}
/** @brief A side index for the art: no owner (kNone) is the neutral's 8. */
static int OwnerOr8(int owner) {
    return owner == kNone ? 8 : owner;
}
/// --military-advisor: the setting forced on every game, or -1 for the game's own.
static int opt_military_advisor = -1;

// ------------------------------------------------------------------ loading

/** @brief Loads every picture, font and the main screen's layout from the data directory. */
static void LoadArt() {
    G.palette = w2::pal::Load(G.data_dir + "/TERRAIN0/WAR2.PAL");
    const auto& palette = G.palette;
    auto img = [&](const std::string& path, int key = pckimage::kNokey) {
        return pckimage::Load(G.data_dir + path, palette, key);
    };
    for (int i = 0; i < 2; i++) G.sheets[i] = img("/TERRAIN0/SCENERY" + std::to_string(i) + ".PCK");
    // The army sheets stand on 10, bright green -- all but the fifth side's,
    // keyed on its own ground, the colour of its corner.
    G.road_img = img("/TERRAIN0/ROAD.PCK", 1);
    for (int i = 0; i <= 8; i++)
        G.army_img[i] = img("/TERRAIN0/A" + std::to_string(i) + ".PCK", pckimage::kCorner);
    // the shadow sheet keys on white: it is two colours of ghost on white
    G.shadow_img = img("/TERRAIN0/ASHADOW.PCK", 15);
    G.marble = img("/PICS/MARBLE.PCK");
    G.big_army = img("/PICS/BIGARMY.PCK");
    G.city_pic = img("/PICS/CITY.PCK");
    G.war_pic = img("/PICS/WAR.PCK", 1);
    G.shield_img = img("/TERRAIN0/BSHIELD.PCK", 12);
    G.victory_pic = img("/PICS/VICTORY.PCK");
    G.hero_pic_m = img("/PICS/MHERO.PCK");
    G.hero_pic_f = img("/PICS/FHERO.PCK");
    // ATRANS2.PCK: map markers on a mask colour, 1
    G.atrans_shields = img("/TERRAIN0/ATRANS2.PCK", 1);
    // HIDDEN.PCK: the hidden map's edges; colour 1 is where the map shows through
    G.fog_img = img("/PICS/HIDDEN.PCK", 1);
    // CURS.PCK: the marching box round the selected stack; keyed on 0, its 1
    // shows white and its 4 black
    G.curs_img = pckimage::FromPixels(*w2::pck::Decode(G.data_dir + "/PICS/CURS.PCK"), palette,
                                      {{1, 15}, {4, 0}}, {0});
    // the rings, drawn opaque, as the original blits every bitmap it is given
    G.abits = img("/PICS/ABITS.PCK");
    // STAND.PCK: the twelve mouse pointers, drawn by the game itself
    G.pointer_img = img("/STAND.PCK", 10);
    G.screen = screen::Load(G.data_dir, palette, w2::uidata::kMainScreen, 0);
    // the player's own button shortcuts (ui/shortcuts)
    try {
        std::string shortcuts_json = prefs::Get("shortcuts");
        if (!shortcuts_json.empty()) {
            auto shortcut_list = w2::Json::Parse(shortcuts_json);
            for (size_t i = 0; i < shortcut_list.size(); i++)
                G.screen->ui.shortcuts[static_cast<int>(i)] =
                        static_cast<int>(shortcut_list[i].Integer());
        }
    } catch (...) {
    }
    G.font = Font::Load(G.data_dir, "TEXT", palette, 3);
    G.big_font = Font::Load(G.data_dir, "CHANCE17", palette, 3);
    G.title_font = Font::Load(G.data_dir, "CHANCE36", palette, 3);
    G.shields_img = img("/TERRAIN0/SHIELDS.PCK");
    G.city_back = img("/TERRAIN0/CITYBACK.PCK");
    G.search_pic = img("/PICS/SEARCH.PCK");
    G.temple_pic = img("/PICS/TEMPLE.PCK");
    // a promotion's (popups 16-18) and a medal's (popups 24-27) pictures
    for (int i = 2; i <= 4; i++) G.level_pic[i] = img("/PICS/LEVEL" + std::to_string(i) + ".PCK");
    for (int i = 0; i < 4; i++) G.medal_pic[i] = img("/PICS/MEDAL" + std::to_string(i) + ".PCK");
    G.scroll_pic = img("/PICS/SCROLL.PCK", 10);
}

// ------------------------------------------------------------------ camera

// The view is as many tiles as the map's rect holds at the map's zoom, and
// slides smoothly: G.cx, G.cy is the tile at its top-left corner.

/** @brief The view's width and height in tiles (fractional). */
static std::pair<double, double> ViewSize() {
    double tiles_per_ui = static_cast<double>(display::d.scale) / (kTile * G.zoom);
    return {G.map_rect.w * tiles_per_ui, G.map_rect.h * tiles_per_ui};
}

/** @brief The camera in device pixels, rounded to one. */
static std::pair<int, int> CamDev() {
    double tile_size = kTile * G.zoom;
    return {static_cast<int>(std::floor(G.cx * tile_size + 0.5)),
            static_cast<int>(std::floor(G.cy * tile_size + 0.5))};
}

/** @brief A UI point to the map position under it, in fractional tiles. */
static std::pair<double, double> UiToMap(double x, double y) {
    const auto& rect = G.map_rect;
    double scale = display::d.scale, tile_size = kTile * G.zoom;
    auto [cam_x, cam_y] = CamDev();
    return {(cam_x + (x - rect.x) * scale) / tile_size, (cam_y + (y - rect.y) * scale) / tile_size};
}

/** @brief A map position in fractional tiles to the UI point it is drawn at. */
std::pair<double, double> front::MapToUi(double tile_x, double tile_y) {
    const auto& rect = G.map_rect;
    double scale = display::d.scale, tile_size = kTile * G.zoom;
    auto [cam_x, cam_y] = CamDev();
    return {rect.x + (tile_x * tile_size - cam_x) / scale,
            rect.y + (tile_y * tile_size - cam_y) / scale};
}

/** @brief The tile at the middle of the view. */
std::pair<int, int> front::ViewCentre() {
    auto [view_w, view_h] = ViewSize();
    return {static_cast<int>(std::floor(G.cx + view_w / 2)),
            static_cast<int>(std::floor(G.cy + view_h / 2))};
}

/** @brief The tiles the view touches, clipped to the map: x0, y0, x1, y1. */
static std::array<int, 4> ViewTiles() {
    auto [view_w, view_h] = ViewSize();
    int map_w = G.g->map->width, map_h = G.g->map->height;
    return {std::max(0, static_cast<int>(std::floor(G.cx))),
            std::max(0, static_cast<int>(std::floor(G.cy))),
            std::min(map_w - 1, static_cast<int>(std::ceil(G.cx + view_w)) - 1),
            std::min(map_h - 1, static_cast<int>(std::ceil(G.cy + view_h)) - 1)};
}

/** @brief Whether the tile is among those drawn this frame (G.view). */
static bool Shown(int x, int y) {
    return x >= G.view[0] && y >= G.view[1] && x <= G.view[2] && y <= G.view[3];
}

/** @brief Whether the tile's centre lies inside the view. */
static bool InView(int x, int y) {
    auto [view_w, view_h] = ViewSize();
    return x + 0.5 > G.cx && x + 0.5 < G.cx + view_w && y + 0.5 > G.cy && y + 0.5 < G.cy + view_h;
}

/** @brief Keeps the view on the map; a map smaller than the view sits in its middle. */
static void ClampCamera() {
    if (!G.g) return;
    auto [view_w, view_h] = ViewSize();
    int map_w = G.g->map->width, map_h = G.g->map->height;
    G.cx = view_w >= map_w ? (map_w - view_w) / 2 : std::max(0.0, std::min(G.cx, map_w - view_w));
    G.cy = view_h >= map_h ? (map_h - view_h) / 2 : std::max(0.0, std::min(G.cy, map_h - view_h));
}

/** @brief Centres the view on the tile, as far as the map's edges allow. */
void front::CentreOn(int x, int y) {
    auto [view_w, view_h] = ViewSize();
    G.cx = x + 0.5 - view_w / 2;
    G.cy = y + 0.5 - view_h / 2;
    ClampCamera();
}
using front::CentreOn;

/** @brief The zoom the map may go to: up to two steps past the UI's biggest. */
static int MaxZoom() {
    return std::max(display::d.max_scale, display::d.scale) + 2;
}

/**
 * @brief Zooms the map to `zoom`, keeping the map under the UI point (x, y) put.
 * @param centre Zoom about the middle of the map's rect instead of (x, y).
 */
static void SetZoomAt(int zoom, double x, double y, bool centre) {
    zoom = std::max(1, std::min(MaxZoom(), zoom));
    if (zoom == G.zoom) return;
    prefs::Set("zoom", zoom);
    const auto& rect = G.map_rect;
    if (centre) {
        x = rect.x + rect.w / 2.0;
        y = rect.y + rect.h / 2.0;
    }
    auto [tile_x, tile_y] = UiToMap(x, y);
    G.zoom = zoom;
    double tiles_per_ui = static_cast<double>(display::d.scale) / (kTile * zoom);
    G.cx = tile_x - (x - rect.x) * tiles_per_ui;
    G.cy = tile_y - (y - rect.y) * tiles_per_ui;
    ClampCamera();
}
/** @brief Zooms the map about the middle of the view. */
void front::SetZoom(int zoom) {
    SetZoomAt(zoom, 0, 0, true);
}

/**
 * @brief Takes up a new layout of the screen's regions, keeping the view
 *        centred where it was.
 */
static void ApplyLayout(const layout::Layout& new_layout) {
    std::optional<std::pair<double, double>> mid;
    if (G.g && G.have_layout) {
        auto [view_w, view_h] = ViewSize();
        mid = std::make_pair(G.cx + view_w / 2, G.cy + view_h / 2);
    }
    G.layout = new_layout;
    G.have_layout = true;
    screen::Relayout(*G.screen, new_layout);
    auto reg = [](int region_id) {
        const auto* region = screen::FindRegion(*G.screen, region_id);
        return region ? layout::Rect{region->x, region->y, region->w, region->h, region->id}
                      : layout::Rect{};
    };
    G.map_rect = reg(screen::kMap);
    G.strat_rect = reg(screen::kStrategic);
    G.bar_rect = reg(screen::kBottombar);
    G.menu_rect = reg(screen::kMenubar);
    G.panel_rect = reg(screen::kMappanel);
    G.menu_layout = menu::MakeLayout(*G.font, new_layout.w,
                                     menu::WithZooms(display::d.max_short, MaxZoom(),
                                                     display::d.smooth_scale, display::d.shrink));
    G.zoom = std::max(1, std::min(MaxZoom(), G.zoom ? G.zoom : display::d.scale));
    if (mid) {
        auto [view_w, view_h] = ViewSize();
        G.cx = mid->first - view_w / 2;
        G.cy = mid->second - view_h / 2;
        ClampCamera();
    }
}

/**
 * @brief Sizes the frame and lays the screen out anew when its size changed.
 *
 * The start screens are the original's 640x480, centred; the game has the
 * whole screen, down to SHORT_H rows. Run each frame, and each input.
 */
void front::SyncLayout() {
    int width = layout::kW, height = layout::kH;
    if (!G.starting) std::tie(width, height) = display::UiSize();
    display::SetFrame(std::max(width, layout::kW),
                      std::max(height, G.starting ? layout::kH : layout::kShortH));
    if (!G.have_layout || G.layout.w != display::d.w || G.layout.h != display::d.h) {
        ApplyLayout(layout::Compute(display::d.w, display::d.h));
    }
}
using front::SyncLayout;

/** @brief Changes how the screen is used, keeping the view centred where it was. */
static void Relayout(const std::function<void()>& change) {
    std::optional<std::pair<double, double>> mid;
    if (G.g && !G.starting && G.have_layout) {
        auto [view_w, view_h] = ViewSize();
        mid = std::make_pair(G.cx + view_w / 2, G.cy + view_h / 2);
    }
    change();
    G.have_layout = false;
    SyncLayout();
    if (mid) {
        auto [view_w, view_h] = ViewSize();
        G.cx = mid->first - view_w / 2;
        G.cy = mid->second - view_h / 2;
        ClampCamera();
    }
}

/** @brief Chooses the interface's scale (display::Choose) and remembers it. */
static void SetUiScale(int scale) {
    Relayout([scale]() { display::Choose(scale); });
    if (display::d.chosen == display::kSmooth)
        prefs::Set("scale", "smooth");
    else
        prefs::Set("scale", display::d.chosen);
}

/** @brief Keeps the original's 640x480 (4:3) frame, or fits the screen, and remembers it. */
static void SetOriginalSize(bool enabled) {
    Relayout([enabled]() {
        if (enabled)
            display::d.wanted = std::make_pair(layout::kW, layout::kH);
        else
            display::d.wanted.reset();
    });
    prefs::Set("4:3", enabled ? "on" : "off");
}

/** @brief Runs in a window or full screen, and remembers it. */
static void SetScreen(bool window) {
    Relayout([window]() { display::SetWindowed(window); });
    prefs::Set("screen", window ? "window" : "full");
}

/** @brief Turns the CRT look on or off, and remembers it. */
static void ToggleCrt() {
    display::d.crt = !display::d.crt;
    prefs::Set("crt", display::d.crt ? "on" : "off");
}

/**
 * @brief Whether the computer plays with the classic AI: the first computer
 *        side's setting in the game under way, else the default.
 */
static bool ActiveClassicAi() {
    if (G.g && G.g->map && !G.starting) {
        for (const auto& side : G.g->map->sides) {
            if (side.in_use && side.computer) return side.classic_ai;
        }
        for (const auto& side : G.g->map->sides) {
            if (side.in_use) return side.classic_ai;
        }
    }
    return w2::ai::DefaultClassicAi();
}

/**
 * @brief Switches every side, and the default, to the classic or the modern
 *        AI, and remembers it.
 */
static void SetAiMode(bool classic) {
    w2::ai::SetDefaultClassicAi(classic);
    prefs::Set("ai", classic ? "classic" : "modern");
    if (G.g && G.g->map) {
        for (auto& side : G.g->map->sides) {
            side.classic_ai = classic;
            if (!side.ai) side.ai = std::make_shared<w2::AIData>(w2::ai::core::NewData());
            if (!classic) w2::ai::belief::Ensure(side);
        }
    }
}

/**
 * @brief The menu key of the interface's scale in use.
 *
 * A short one stays ticked on the start screens, which are drawn as with
 * none chosen.
 */
static std::string UiScaleKey() {
    const auto& state = display::d;
    if (state.chosen > state.max_scale && state.chosen <= state.max_short)
        return "ui scale " + std::to_string(state.chosen);
    return state.smooth ? "ui scale smooth" : "ui scale " + std::to_string(state.scale);
}

/** @brief Whether the menu item `key` shows a tick: the settings now in force. */
static bool MenuTicked(const std::string& key) {
    return key == "map zoom " + std::to_string(G.zoom) || key == UiScaleKey() ||
           key == std::string("screen ") + (display::d.windowed ? "window" : "full") ||
           (key == "screen 4:3" && display::d.wanted) ||
           (key == "screen fit" && !display::d.wanted) || (key == "screen crt" && display::d.crt) ||
           key == "music " + sound::Synth() ||
           key == (ActiveClassicAi() ? "ai classic" : "ai modern");
}

// ------------------------------------------------------------------ walking

// A stack walks one tile at a time, and the map shows where it is going. The
// destination is kept in the army record itself, so it outlives a walk that
// could not finish. The original marks the route: a ring on every tile but
// the last, plain while the stack can still reach it this turn and crossed
// once it cannot, and a ghost of the leading army on the last (docs/re/ui.md).
//
// In ASHADOW.PCK: the plain ring, and the crossed one for out of reach this
// turn; then where in its tile a ring, and the ghost, are drawn.
static const int kWalkRing[2] = {496, 31}, kWalkCrossed[2] = {496, 48};
static const int kWalkRingW = 16, kWalkRingH = 14;
static const int kWalkRingAt[2] = {16, 13}, kWalkGhostAt[2] = {4, 4};
static const double kWalkStep = 0.1;  ///< Seconds the walk spends on one tile.

/**
 * @brief Works out the route to the selection's destination, if it has one.
 *
 * A stack already standing on its destination forgets it.
 */
static void RefreshRoute() {
    G.route.reset();
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    auto target = sel.stack[0]->target;
    if (!target) return;
    if (sel.stack[0]->x == target->first && sel.stack[0]->y == target->second) {
        for (Army* army : sel.stack) army->target.reset();
        return;
    }
    G.route = move::PreviewRoute(*G.g, sel.stack, target->first, target->second);
}

/** @brief Gives every army in the stack the destination (x, y); kNone clears it. */
static void OrderTo(const std::vector<Army*>& stack, int x, int y) {
    for (Army* army : stack) {
        if (x == kNone)
            army->target.reset();
        else
            army->target = std::make_pair(x, y);
    }
}

/**
 * @brief Alt and a click: the route to (x, y) is drawn but not walked.
 *
 * A click on the stack's own tile clears its destination.
 */
static void PlanRoute(int x, int y) {
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    if (!game::Seen(*G.g, G.player->index, x, y)) return;
    if (sel.stack[0]->x == x && sel.stack[0]->y == y) {
        OrderTo(sel.stack, kNone, kNone);
        G.route.reset();
        return;
    }
    OrderTo(sel.stack, x, y);
    RefreshRoute();
    if (!G.route) sound::Effect("chord");
}

/**
 * @brief Plays the walk back a tile at a time, centring on the stack as it goes.
 * @param sea_before The stack was at sea as it set off, and is shown so to
 *        the end of the walk: the original puts it to sea or ashore only
 *        after its last step.
 */
static void StartWalk(const std::vector<Army*>& armies,
                      const std::vector<std::pair<int, int>>& tiles, bool sea_before) {
    if (tiles.empty()) return;
    Walk walk;
    walk.sea_before = sea_before;
    walk.armies = std::set<Army*>(armies.begin(), armies.end());
    walk.tiles = tiles;
    walk.i = 0;
    walk.at = w2::Now();
    walk.top = armies.empty() ? nullptr : armies[0];
    G.walk = walk;
    CentreOn(tiles[0].first, tiles[0].second);
}

static void MoveAllStep();
static void FlushKeys();

/**
 * @brief Puts down a stack out of moves after its walk.
 *
 * It is done for the turn and put down, with nothing selected. A goal it has
 * reached is forgotten.
 */
static void PutDown(const std::vector<Army*>& stack) {
    Army* lead = stack.empty() ? nullptr : stack[0];
    bool arrived = lead && lead->target && lead->x == lead->target->first &&
                   lead->y == lead->target->second;
    for (Army* army : stack) {
        army->done = true;
        if (arrived) army->target.reset();
    }
    G.selection.reset();
    G.route.reset();
}

/**
 * @brief Moves the walk on by the time that has passed, and finishes it once
 *        its last tile has had its turn.
 *
 * A finished walk puts down a stack out of moves, and hands on to whoever
 * waits for it: the computer's turn, Move All, or the keys typed meanwhile.
 */
static void AdvanceWalk() {
    if (!G.walk) return;
    Walk& walk = *G.walk;
    double now = w2::Now();
    while (walk.i < walk.tiles.size() - 1 && now - walk.at >= kWalkStep) {
        walk.i++;
        walk.at += kWalkStep;
        CentreOn(walk.tiles[walk.i].first, walk.tiles[walk.i].second);
    }
    if (walk.i >= walk.tiles.size() - 1 && now - walk.at >= kWalkStep) {
        bool computer = walk.computer;
        auto down = walk.put_down;
        G.walk.reset();
        if (!down.empty()) PutDown(down);
        RefreshRoute();
        if (computer && G.ai_run) ResumeComputer();
        if (G.move_all) MoveAllStep();
        if (!G.walk && !G.move_all) FlushKeys();
    }
}

/** @brief The tile the walk has got to, or nullptr with no walk. */
static const std::pair<int, int>* WalkingAt() {
    return G.walk ? &G.walk->tiles[G.walk->i] : nullptr;
}

/** @brief Draws the selection's route: rings to the last tile, the lead's ghost on it. */
static void DrawRoute() {
    if (!G.route) return;
    const auto& route = *G.route;
    Army* ghost = G.selection && !G.selection->stack.empty() ? G.selection->stack[0] : nullptr;
    // while the walk plays out the route is drawn from where it has got to
    size_t first = G.walk ? G.walk->i + 1 : 1;
    for (size_t i = first; i <= route.path.size(); i++) {
        const auto& step = route.path[i - 1];
        if (!Shown(step.x, step.y)) continue;
        int pixel_x = step.x * kTile, pixel_y = step.y * kTile;
        gfx::SetColor(1, 1, 1);
        if (i == route.path.size()) {
            if (ghost)
                gfx::Draw(G.shadow_img, kit::ArmyQuad(ghost->type), pixel_x + kWalkGhostAt[0],
                          pixel_y + kWalkGhostAt[1]);
        } else {
            const int* src = static_cast<int>(i) <= route.reach ? kWalkRing : kWalkCrossed;
            gfx::Draw(G.shadow_img, gfx::NewQuad(src[0], src[1], kWalkRingW, kWalkRingH),
                      pixel_x + kWalkRingAt[0], pixel_y + kWalkRingAt[1]);
        }
    }
}

/** @brief A UI point to the map tile under it, or none outside the viewport. */
static std::optional<std::pair<int, int>> TileAtPoint(int point_x, int point_y) {
    const auto& rect = G.map_rect;
    if (point_x < rect.x || point_y < rect.y || point_x >= rect.x + rect.w ||
        point_y >= rect.y + rect.h)
        return std::nullopt;
    auto [tile_x, tile_y] = UiToMap(point_x, point_y);
    return std::make_pair(static_cast<int>(std::floor(tile_x)),
                          static_cast<int>(std::floor(tile_y)));
}

// ------------------------------------------------------------------ selection

/** @brief The player's armies on the tile, as many as a stack may hold. */
static std::vector<Army*> SelectableAt(int x, int y) {
    std::vector<Army*> out;
    for (Army* army : game::ArmiesAt(*G.g, x, y))
        if (army->owner == G.player->index && static_cast<int>(out.size()) < w2::rules::kMaxStack)
            out.push_back(army);
    return out;
}

/** @brief Picks up whatever the slots now say moves, and remembers the grouping. */
static void SyncSelection() {
    if (!G.selection) return;
    auto& sel = *G.selection;
    slots_mod::Commit(sel.slots, *G.g);
    sel.stack = slots_mod::Selected(sel.slots);
    // being in the group that moves puts an army back in the cycle and marks
    // it offered for this pass
    for (Army* army : sel.stack) {
        army->fortified = false;
        army->offered = true;
    }
}

/**
 * @brief Puts the slots back over a new list of armies, keeping the group.
 *
 * With none of them left the selection goes.
 */
static void Reslot(const std::vector<Army*>& armies) {
    if (!G.selection) return;
    auto kept = armies.empty() ? std::nullopt : slots_mod::Keep(G.selection->slots, *G.g, armies);
    if (!kept) {
        G.selection.reset();
        return;
    }
    G.selection->slots = *kept;
    G.selection->x = kept->army[0]->x;
    G.selection->y = kept->army[0]->y;
    SyncSelection();
}

/// "Alas! %d armies have been sunk" / "due to lack of a boat!" (STRING.DAT 168).
static const int kSunkGroup = 0xa8;

/**
 * @brief Drowns the player's armies stranded on water without a boat.
 *
 * On one tile, or with kNone on all of them. If any sank: a splash, the
 * selection put down, the view on the tile, and the message, `after` running
 * once it is read.
 *
 * @return Whether any did; if not, `after` runs at once.
 */
static bool DrownStranded(int x, int y, kit::Done after = nullptr) {
    int n = game::SinkStranded(*G.g, *G.player, x, y);
    if (n == 0) {
        if (after) after();
        return false;
    }
    sound::Effect("splash");
    G.selection.reset();
    G.route.reset();
    front::StratDirty();
    if (x != kNone) CentreOn(x, y);
    std::string line = n == 1 ? kit::Text(kSunkGroup, 2) : Format(kit::Text(kSunkGroup, 0), n);
    search::ShowMessage(line, kit::Text(kSunkGroup, 1), after);
    return true;
}

/**
 * @brief Selects the player's stack on the tile, or opens the city there.
 * @param pick The army clicked, which the slots select; nullptr for the
 *        usual choice.
 */
static void Select(int x, int y, Army* pick) {
    // picking a stack up on the water drowns whoever has no boat
    if (DrownStranded(x, y)) return;
    auto stack = SelectableAt(x, y);
    if (stack.empty()) {
        G.selection.reset();
        if (City* city = game::CityAt(*G.g, x, y)) front::OpenCity(city);
        return;
    }
    // a click selects one army, not the stack (docs/re/ui.md > The army slots)
    auto chosen = slots_mod::Clicked(*G.g, stack, G.player->index, pick);
    Selection sel;
    sel.x = x;
    sel.y = y;
    sel.slots = slots_mod::Build(*G.g, stack, G.player->index, &chosen);
    G.selection = sel;
    SyncSelection();
    RefreshRoute();
    // the tutorial on picking a stack up
    std::vector<std::string> moments = {"move"};
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            City* neighbour = game::CityAt(*G.g, x + dx, y + dy);
            if (neighbour && neighbour->owner_index != G.player->index && !neighbour->razed &&
                moments.size() == 1)
                moments.push_back("fight");
        }
    }
    Army* lead = G.selection->stack.empty() ? nullptr : G.selection->stack[0];
    if (lead && lead->Hero() && !G.g->map->site_at.empty() &&
        G.g->map->site_at[y * G.g->map->width + x])
        moments.push_back("search");
    tutorial::Chain(moments);
}
/** @brief Selects what is on the tile, as a click with the select pointer does. */
void front::SelectAt(int x, int y) {
    Select(x, y);
}

// ------------------------------------------------------------- the army cycle

// The five buttons above the pad walk you through your armies one stack at
// a time. The next army is the nearest of the eligible ones to where the
// cycle last stopped; one exactly there counts as far away.

/**
 * @brief Where a side's capital now stands: its own, or the city it moved
 *        its capital to while it does not hold its own.
 */
static std::pair<int, int> CapOf(const Side* side) {
    if (side->cap_now_x != kNone) return {side->cap_now_x, side->cap_now_y};
    if (side->capital) return {side->capital->x, side->capital->y};
    return {side->cap_x == kNone ? 0 : side->cap_x, side->cap_y == kNone ? 0 : side->cap_y};
}

/** @brief Starts the army cycle over from the player's capital. */
static void CycleReset() {
    G.cursor = CapOf(G.player);
}

/**
 * @brief How far the army is from where the cycle last stopped (smaller comes
 *        first); one standing exactly there counts as far away.
 */
static int CycleRank(const Army* army) {
    int distance = std::abs(army->x - G.cursor->first) + std::abs(army->y - G.cursor->second);
    return distance == 0 ? 9000 : distance;
}

/**
 * @brief The army the cycle offers next, and moves the cycle's cursor to it.
 *
 * Armies not yet offered this pass come first; once all have been, the pass
 * starts over. nullptr when every army has moved, is fortified or is done.
 */
static Army* NextArmy() {
    if (!G.cursor) CycleReset();
    Army* fresh = nullptr;
    Army* used = nullptr;
    for (Army* army : G.g->armies) {
        if (army->owner == G.player->index && !army->transit && !army->fortified && !army->done) {
            if (army->offered) {
                if (!used || CycleRank(army) < CycleRank(used)) used = army;
            } else if (!fresh || CycleRank(army) < CycleRank(fresh)) {
                fresh = army;
            }
        }
    }
    if (!fresh) {
        if (!used) return nullptr;
        for (Army* army : G.g->armies)
            if (army->owner == G.player->index) army->offered = false;
        fresh = used;
    }
    G.cursor = std::make_pair(fresh->x, fresh->y);
    return fresh;
}

/** @brief Selects the cycle's next army and centres on it, or says none is left. */
static void SelectNext() {
    Army* army = NextArmy();
    if (!army) {
        G.selection.reset();
        G.route.reset();
        Say("Every army has moved.");
        sound::Effect("chord");  // nothing left
        return;
    }
    sound::Effect("ding");
    Select(army->x, army->y, army);
    CentreOn(army->x, army->y);
}

/** @brief Takes the selection out of the cycle for the rest of this turn (control 175). */
static void QuitArmy() {
    if (!G.selection) {
        SelectNext();
        return;
    }
    for (Army* army : G.selection->stack) army->done = true;
    G.route.reset();
    SelectNext();
}

/** @brief Digs the selection in: out of the cycle until picked up again (control 176). */
static void Fortify() {
    if (!G.selection) {
        SelectNext();
        return;
    }
    for (Army* army : G.selection->stack) {
        army->fortified = true;
        army->target.reset();
    }
    G.route.reset();
    SelectNext();
}

/// A second click within 40 BIOS ticks of the last counts as a double click.
static const double kDoubleClick = 40 / 18.2;

/**
 * @brief A click with the select pointer on a tile.
 *
 * On the selected stack's own tile, soon after the last such click, it
 * groups the whole stack as Space does; otherwise it selects what is there.
 */
static void SelectClick(int tile_x, int tile_y) {
    double now = w2::Now(), last = G.last_select_click;
    G.last_select_click = now;
    if (G.selection && G.selection->x == tile_x && G.selection->y == tile_y && last >= 0 &&
        now - last < kDoubleClick) {
        slots_mod::All(G.selection->slots, *G.g);
        front::AfterSlotChange();
        RefreshControls();
        return;
    }
    Select(tile_x, tile_y);
}

/** @brief Puts the selection down (control 178). */
static void Deselect() {
    G.selection.reset();
    G.route.reset();
    G.walk.reset();
}

// ------------------------------------------------------------------ the turn

/**
 * @brief After the player's battle: the result's tutorial, and the selection
 *        re-slotted over its survivors.
 */
static void AfterBattle(const w2::Battle& result) {
    tutorial::Show("fresult");
    if (G.selection) {
        std::vector<Army*> alive;
        for (int i = 0; i < G.selection->slots.n; i++) {
            Army* army = G.selection->slots.army[i];
            if (!result.dead_by_army.count(army)) alive.push_back(army);
        }
        Reslot(alive);
    }
}

/// "War!" -- asked before attacking a side not at war (STRING.DAT group 140).
static const int kWarGroup = 0x8c;

/**
 * @brief A click with the sword or the dove: fights for the tile beside the
 *        stack.
 *
 * There is no walk, so the step's cost does not matter: one move left will
 * do. A side at peace -- or, for a city, one not at war -- must be declared
 * on first. The battle is decided first, shown, and only then takes effect.
 */
static void AttackTile(int x, int y) {
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    w2::Game& game_state = *G.g;
    int player_index = G.player->index;
    auto here = game::ArmiesAt(game_state, x, y);
    City* city = game::CityAt(game_state, x, y);
    if (city && city->razed) city = nullptr;
    int owner = !here.empty() ? here[0]->owner : (city ? city->owner_index : kNone);
    if (w2::diplomacy::MustDeclare(game_state, player_index, owner, city)) {
        input::Options options;
        options.title = kit::Text(kWarGroup, 0);
        for (int i = 1; i <= 4; i++) options.lines.push_back(kit::Text(kWarGroup, i));
        options.confirm = true;
        options.ok = [x, y, player_index, owner](const std::string&) {
            w2::diplomacy::AttackAnyway(*G.g, player_index, owner);
            AttackTile(x, y);
        };
        input::Open(options);
        return;
    }
    if (move::StackMoves(sel.stack) < 1) {
        Deselect();
        return;
    }
    if (owner == player_index || (!city && here.empty())) return;
    OrderTo(sel.stack, kNone, kNone);
    G.route.reset();
    game::ChargeAttack(game_state, sel.stack, x, y);
    // decided first, shown, and only then taken effect
    w2::Battle result = game::DecideAttack(game_state, sel.stack, x, y);
    StartAssault(x, y, result);
    G.assault->after = [result]() { AfterBattle(result); };
}

/**
 * @brief Walks the selection towards (x, y).
 *
 * Running into an enemy just stops the walk; attacking is the sword's
 * (AttackTile).
 */
static void MoveSelection(int x, int y) {
    if (!G.selection) return;
    auto& sel = *G.selection;
    OrderTo(sel.stack, x, y);
    G.route = move::PreviewRoute(*G.g, sel.stack, x, y);
    auto walk_result = move::MoveTo(*G.g, sel.stack, x, y);
    if (walk_result.stopped == "attack") walk_result.stopped = "blocked";
    if (walk_result.stopped == "no route") {
        Say("There is no way there.");
        sound::Effect("chord");
    } else if (walk_result.steps == 0) {
        Say("They cannot move: " + walk_result.stopped + ".");
    } else {
        auto walked = sel.stack;
        sel.x = sel.stack[0]->x;
        sel.y = sel.stack[0]->y;
        Reslot(SelectableAt(sel.x, sel.y));
        front::StratDirty();
        StartWalk(walked, walk_result.walked, walk_result.sea_before);
    }
    // a walk that leaves the stack out of moves ends its turn: done for the
    // turn and put down -- once the walk has played out, as the original
    // draws every step first
    if (walk_result.out_of_moves && G.selection) {
        if (G.walk)
            G.walk->put_down = G.selection->stack;
        else
            PutDown(G.selection->stack);
    }
    if (G.selection && !G.selection->stack.empty()) {
        G.selection->x = G.selection->stack[0]->x;
        G.selection->y = G.selection->stack[0]->y;
        if (!G.walk) CentreOn(G.selection->x, G.selection->y);
    }
    if (!G.walk) RefreshRoute();
}

// ------------------------------------------------------------------ the pointer

/**
 * @brief The twelve mouse pointers of STAND.PCK, in its order.
 *
 * The original decides the mouse pointer from what is under it and what is
 * selected, and a click on the map does whatever that pointer promises.
 */
enum Ptr {
    kArrow,   ///< Nothing to do here: menus, dialogs, the computer's turn.
    kView,    ///< Over the strategic map: moves the view.
    kBoat,    ///< Walks the selection onto the water.
    kCityP,   ///< Looks at a city.
    kHand,    ///< Nothing to do on this tile.
    kSelect,  ///< Selects the player's stack here.
    kWalk,    ///< Walks the selection here.
    kSiteP,   ///< Looks at a site (or a razed city).
    kAttack,  ///< Attacks the enemy here.
    kAdvise,  ///< Asks the military advisor about the enemy here.
    kPeace,   ///< The enemy here is not at war: attacking means declaring it.
    kAlt      ///< Alt held: plans a route without walking it.
};
/// Each pointer's hotspot, the same distance in from its left and top.
static const int kPtrHot[12] = {0, 6, 8, 6, 8, 8, 8, 8, 0, 8, 8, 0};

/**
 * @brief The pointer (Ptr) for the UI point (x, y): what a click there would do.
 */
static int PointerKind(int x, int y) {
    if (G.starting || G.over || G.banner || G.offer || G.assault || G.victory || G.ai_run ||
        G.open_menu >= 0 || kit::Top() || !G.player || G.player->computer) {
        return kArrow;
    }
    if (G.drag) return kHand;  // while it drags
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl"});
    if (G.strat_rect.Contains(x, y)) return alt ? kAlt : kView;
    auto tile = TileAtPoint(x, y);
    if (!tile) return G.panel_rect.Contains(x, y) ? kHand : kArrow;
    auto [tile_x, tile_y] = *tile;
    w2::Game& game_state = *G.g;
    int player_index = G.player->index;
    if (tile_x < 0 || tile_y < 0 || tile_x >= game_state.map->width ||
        tile_y >= game_state.map->height)
        return kHand;
    auto here = game::ArmiesAt(game_state, tile_x, tile_y);
    City* city = game::CityAt(game_state, tile_x, tile_y);
    bool armies = !here.empty();
    // the tile's owner nibble: whoever stands there, else the city's, else none
    int owner = armies ? here[0]->owner : kNone;
    if (owner == kNone) owner = city ? city->owner_index : kNone;
    if (owner == kNone) owner = w2::rules::kNeutral;
    int terrain = city ? (city->razed ? move::kSite : move::kCity)
                       : w2::scn::TerrainAt(*game_state.map, tile_x, tile_y);
    const std::vector<Army*>* sel =
            G.selection && !G.selection->stack.empty() ? &G.selection->stack : nullptr;
    int dist = 0, mode = kNone, under = move::kPlain;
    bool at_sea = false;
    if (sel) {
        int stack_x = (*sel)[0]->x, stack_y = (*sel)[0]->y;
        if (city && !city->razed) {
            int dx = std::max(0, std::max(city->x - stack_x, stack_x - (city->x + 1)));
            int dy = std::max(0, std::max(city->y - stack_y, stack_y - (city->y + 1)));
            dist = std::max(dx, dy);
        } else {
            dist = std::max(std::abs(tile_x - stack_x), std::abs(tile_y - stack_y));
        }
        mode = move::ModeOf(game_state, *sel);
        under = w2::scn::TerrainAt(*game_state.map, stack_x, stack_y);
        at_sea = (*sel)[0]->at_sea;
    }
    // any tile with a road on it costs 1, a mountain pass too
    bool road = w2::scn::RoadAt(*game_state.map, tile_x, tile_y) % 0x20 != 0;
    int cost = (sel && mode != move::kFlying && !road) ? move::kCost[terrain] : 1;
    if (!game::Seen(game_state, player_index, tile_x, tile_y) || cost <= 0) return kHand;
    auto walk = [&]() {
        return (mode != move::kFlying && (terrain == move::kWater || terrain == move::kShore))
                       ? kBoat
                       : kWalk;
    };
    auto look = [&]() {
        if (terrain == move::kCity) return kCityP;
        if (terrain == move::kSite) return kSiteP;
        return kHand;
    };
    static const std::set<int> kPtrOk = {move::kRoad, move::kBridge, move::kWater, move::kShore,
                                         move::kCity};
    bool enemy_near =
            sel && dist <= 1 && owner != player_index && (terrain == move::kCity || armies);
    if (Held({"lshift", "rshift"})) {
        if (enemy_near && game_state.map->options.military_advisor != 0) return kAdvise;
        return look();
    }
    if (armies && owner == player_index && !ctrl) {
        if (alt) return kAlt;
        if (!sel || dist < 1) return kSelect;
        return walk();
    }
    if (enemy_near &&
        !(at_sea && mode == move::kLand && under == move::kShore && !kPtrOk.count(terrain)) &&
        !(!at_sea && mode == move::kLand && terrain == move::kShore && !kPtrOk.count(under))) {
        if (game_state.map->options.diplomacy == 0 || owner == w2::rules::kNeutral) return kAttack;
        int relation = w2::diplomacy::State(game_state, player_index, owner);
        if (terrain == move::kCity) return relation == w2::diplomacy::kWar ? kAttack : kPeace;
        return relation != w2::diplomacy::kPeace ? kAttack : kPeace;
    }
    if (sel &&
        (owner == player_index || (owner == w2::rules::kNeutral && terrain != move::kCity))) {
        if (alt) return kAlt;
        if (dist >= 1 && ctrl && armies) return kSelect;
        return walk();
    }
    return look();
}

/**
 * @brief Draws the mouse pointer.
 *
 * Not the original's: over the black round the frame the pointer is the
 * arrow, drawn where the mouse really is rather than held to the frame's edge.
 */
static void DrawPointer() {
    if (display::PointerAway()) return;
    int kind = display::d.off ? kArrow : PointerKind(G.mouse_x, G.mouse_y);
    auto [x, y] = display::d.off ? *display::d.off : std::make_pair(G.mouse_x, G.mouse_y);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.pointer_img, gfx::NewQuad(kind * 16, 0, 16, 16), x - kPtrHot[kind],
              y - kPtrHot[kind]);
}

// The start-of-turn banner: popup 6, (160, 60) 320x312 -- exactly CITY.PCK
// -- framed in the side's own colour, with the side's name and "Turn %d"
// over it. The original then blocks until any input arrives.
static const kit::Rect kBanner{160, 60, 320, 312};
static const int kBannerNameY = 85, kBannerTurnY = 130;

/** @brief Raises the start-of-turn banner for the side, with its fanfare. */
static void ShowBanner(Side* side) {
    G.banner = Banner{side->name, G.g->turn, side->colour, side->edge};
    sound::Effect("turn");  // the fanfare, TURN.8SN
}

// The hero offer is popup 2 -- (80, 60) 480x312, MARBLE.PCK cropped -- with
// dialog 12's controls laid on it, every number from the original's own
// hero offer.
static const kit::Rect kHeroPopup{80, 60, 480, 312};
static const int kHeroDialog = 12;
static const int kHeroMapX = 80, kHeroMapY = 60;
static const int kHeroPicX = 320, kHeroPicY = 110, kHeroPicW = 224, kHeroPicH = 170;
static const int kHeroTitleY = 63, kHeroCentre = 432;
static const int kHeroLineY[4] = {190, 210, 230, 250};
static const int kMaleLabel[2] = {376, 315}, kFemaleLabel[2] = {480, 315};
static const int kHeroOk = 287, kHeroCancel = 288, kHeroField = 289, kHeroMale = 290,
                 kHeroFemale = 291;
static const int kHeroTitleGroup = 0x5f, kHeroLineGroup = 0x61, kHeroSexGroup = 0x62;
/// What a hired hero's allies are announced with (STRING.DAT group 96).
static const int kAllyGroup = 0x60;
static const size_t kHeroNameMax = 19;

static bool PresentOffer(Side* side);
static void AfterOffer();
/// The name field clicked: the Steam Deck's on-screen keyboard comes up for it.
static bool offer_typing = false;

/**
 * @brief Whether the maps stay blank until the banner is clicked away.
 *
 * Where the map is hidden and more than one human plays, the main map and
 * the strategic map stay blank as a human's turn opens, until the banner is
 * clicked away: each side has its own hidden map, and whoever is still at
 * the screen is not to see the next one's. The original shares one seen bit
 * among its humans and only holds back the re-centring.
 */
static bool HotseatHidden() {
    if (G.g->map->options.hidden_map == 0) return false;
    int humans = 0;
    for (auto& side : G.g->map->sides)
        if (side.in_use && side.alive && !side.computer) humans++;
    return humans > 1;
}

/** @brief Closes the banner, and only then lets the turn's first dialog through. */
static void DismissBanner() {
    G.banner.reset();
    if (G.map_after_banner) CentreOn(CapOf(G.player).first, CapOf(G.player).second);
    G.map_after_banner = false;
    // whoever is left on the water without a boat drowns, and only then does
    // the advisor have his say; the turn's opening then puts on the hero's
    // music or the turn's
    DrownStranded(kNone, kNone, AfterBanner);
}

/**
 * @brief The turn's opening once the banner is gone: the advisor, the
 *        promotions, the music, the tutorial, then the hero offer.
 */
static void AfterBanner() {
    int says = sound::SpeechOn() ? w2::cues::AdvisorCue(*G.g, *G.player, front::Roll) : kNone;
    // then the heroes promoted, each in its own popup, and the hero offer
    // with its music, or the turn's
    advisor::Say(says, []() {
        auto promoted = G.player->promoted;
        G.player->promoted.clear();
        honours::Promotions(promoted, []() {
            sound::Music(G.player->hero_offer ? w2::cues::kHero : w2::cues::kPlay);
            std::vector<std::string> moments;
            if (G.g->turn == 2) moments.push_back("turn2");
            if (G.player->hero_offer) moments.push_back("hero");
            tutorial::Chain(moments, []() {
                if (!PresentOffer(G.player)) AfterOffer();
            });
        });
    });
}

/**
 * @brief The four lines over the hero offer's picture.
 *
 * As the original assembles them: strings 0-3 for the free hero of turn 1,
 * 4-7 for one that asks a price, and the same again from 8 for a heroine.
 */
static std::vector<std::string> HeroLines(const w2::HeroOffer& offer, bool female) {
    int first = female ? 8 : 0;
    auto line = [first](int i) { return kit::Text(kHeroLineGroup, first + i); };
    if (offer.first) return {line(0), line(1), line(2), Format(line(3), offer.city->name)};
    return {Format(line(4), offer.city->name), Format(line(5), offer.price),
            Format(line(6), G.player->gold), line(7)};
}

/**
 * @brief Opens the side's hero offer, if it has one.
 * @return Whether it had one.
 */
static bool PresentOffer(Side* side) {
    if (!side->hero_offer) return false;
    if (!G.hero_view) G.hero_view = screen::MakeView(*G.screen, kHeroDialog);
    G.offer = side->hero_offer;
    // the name and sex were rolled with the offer; the checkboxes only change
    // the wording and the picture
    G.offer_female = G.offer->female;
    G.offer_name = G.offer->name.empty() ? "Hero" : G.offer->name;
    G.hero_view->state[kHeroCancel] = G.offer->first ? kDisabled : kNormal;
    G.hero_view->state[kHeroOk] = kNormal;
    return true;
}

/** @brief Takes the offer: the hero joins under the name and sex now in the dialog. */
static void AcceptOffer() {
    auto offer = G.offer;
    offer->name = G.offer_name;
    offer->female = G.offer_female;
    auto hired = w2::hero::Recruit(*G.g, *G.player, *offer);
    Army* hero = hired.hero;
    G.offer.reset();
    G.player->hero_offer.reset();
    CentreOn(hero->x, hero->y);
    if (hired.count > 0) {
        // "And the hero brings 1 ally!" / "... %d allies!": the number rolled,
        // placed or not
        std::string line = hired.count == 1 ? kit::Text(kAllyGroup, 0)
                                            : Format(kit::Text(kAllyGroup, 1), hired.count);
        search::Say(line, []() { AfterOffer(); });
        return;
    }
    AfterOffer();
}

/**
 * @brief The turn's opening, once any hero offer is settled: the cities
 *        build -- or on turn 1 the capital's dialog opens in Production.
 */
static void AfterOffer() {
    if (G.player->computer) return;
    game::HumanProduction(*G.g, *G.player);
    front::StratDirty();
    auto cap = CapOf(G.player);
    City* city = game::CityAt(*G.g, cap.first, cap.second);
    if (G.g->turn == 1 && city) {
        city::Open(city, city::kProduction);
        return;
    }
    if (G.modern_ui && !G.player->missives.empty()) {
        diplomacyui::PollMissives();
    } else {
        G.player->missives.clear();
    }
}

/** @brief Turns the hero offer down. */
static void RefuseOffer() {
    G.offer.reset();
    G.player->hero_offer.reset();
    AfterOffer();
}

// ------------------------------------------------------------ the computer

// The computer's turns run on a coroutine (the Lua remake's), so the screen
// keeps being drawn while they are played: it gives a frame back after every
// side's turn, and stops after each walk worth showing. A turn is shown when
// its side is observed or no human is left; a walk only if the player has
// seen any of it. Holding Shift or Alt between turns opens Settings; any
// other key or a click runs the rest of the round through unwatched.

static Coroutine* ai_co = nullptr;  ///< The turn being played, from inside it.
static Side* ai_result = nullptr;   ///< Where the round ended.

/** @brief Whether any human is still in the game. */
static bool HumansLeft() {
    return G.g && game::HasActiveHuman(*G.g);
}

/** @brief Whether the side's turn is shown: it is observed, or no human is left. */
static bool ShownSide(const Side* side) {
    return side && (side->observe || !HumansLeft());
}

/**
 * @brief A line in the status bar during the computer's turn, and the time
 *        the original then waits.
 * @param ticks How long to wait, in BIOS ticks; none when running through.
 */
static void Status(const std::string& text, int ticks) {
    if (G.ai_status) G.ai_status->text = text;
    if (ticks > 0 && !G.ai_skip && ai_co) {
        Yield pause;
        pause.what = "pause";
        pause.ticks = ticks;
        ai_co->YieldValue(pause);
    }
}

/** @brief The computer's progress bar after each phase of its turn, in percent. */
static int ProgressOf(const std::string& phase) {
    static const std::map<std::string, int> kProgress = {
        {"diplomacy", 10},  {"move hero", 15}, {"move search", 20}, {"move explore", 25},
        {"assault", 30},    {"move #1", 40},   {"rescue", 45},      {"evaluate", 50},
        {"clean city", 55}, {"neutral", 60},   {"missions", 60},    {"move #2", 65},
        {"assault XX", 70}, {"specials", 75},  {"rebuilding", 80},  {"last rescue", 85},
        {"production", 90}, {"economy", 92},   {"vectoring", 95}};
    auto it = kProgress.find(phase);
    return it == kProgress.end() ? (G.ai_status ? G.ai_status->progress : 0) : it->second;
}

/**
 * @brief Hooks the computer's walks and battles, so the screen can show them
 *        as the turn plays.
 */
static void InstallHooks() {
    w2::ai::hooks.on_walk = [](w2::Game& game_state, const std::vector<Army*>& stack,
                               const move::WalkResult& walk_result) {
        if (!ai_co || G.ai_skip || !ShownSide(G.ai_side)) return;
        bool seen = !HumansLeft();
        for (auto& tile : walk_result.walked) {
            if (seen) break;
            if (game::Seen(game_state, G.player->index, tile.first, tile.second)) seen = true;
        }
        if (seen) {
            Yield walk_step;
            walk_step.what = "walk";
            walk_step.armies = stack;
            walk_step.tiles = walk_result.walked;
            walk_step.sea_before = walk_result.sea_before;
            ai_co->YieldValue(walk_step);
        }
    };
    // A computer's battle: who is attacked goes up in the status bar, and
    // then how it went, five ticks each -- when the attacker's turn is
    // watched or the defender is a human.
    w2::ai::hooks.on_fight = [](w2::Game& game_state, const std::vector<Army*>&, int x, int y,
                                w2::Battle& result) {
        if (!ai_co || !G.ai_status) return;
        int defender = kNone;
        if (result.lines.city)
            defender = result.lines.city->owner_index;
        else if (!result.lines.defenders.empty())
            defender = result.lines.defenders[0]->owner;
        Side* def_side =
                defender != kNone && defender < 8 ? &game_state.map->sides[defender] : nullptr;
        bool human = def_side && !def_side->computer;
        if (!(ShownSide(G.ai_side) || human)) return;
        std::vector<Side*> humans;
        for (Side* side : game_state.sides)
            if (side->alive && !side->computer) humans.push_back(side);
        if (game_state.map->options.hidden_map != 0 && humans.size() == 1 &&
            !game::Seen(game_state, humans[0]->index, x, y))
            return;
        auto text_of = [](int grp) { return kit::Text(grp, 0); };
        Army* attacking_hero = nullptr;
        for (Army* army : result.lines.attackers)
            if (army->Hero()) {
                attacking_hero = army;
                break;
            }
        std::string attacker_name = G.ai_side->name;
        std::string outcome;
        if (!result.won)
            outcome = Format(text_of(0x98), attacker_name);
        else if (attacking_hero)
            outcome = Format(text_of(0x8e), attacking_hero->name);
        else
            outcome = Format(text_of(0x96), attacker_name);
        bool hidden_many = game_state.map->options.hidden_map != 0 && humans.size() > 1;
        bool window = !hidden_many && ((ShownSide(G.ai_side) && def_side) || human);
        bool cloud = !hidden_many;
        // the original slides the view box to the battle and centres the map
        // on it, Observe or not -- a human attacked gets to see where
        if (!hidden_many) {
            CentreOn(x, y);
            front::StratDirty();
        }
        Status(def_side ? Format(text_of(0x94), def_side->name) : text_of(0x95), 5);
        if (cloud && !G.ai_skip) {
            StartAssault(x, y, result);
            G.assault->computer = true;
            G.assault->window = window;
            G.assault->outcome = outcome;
            G.assault->message = {outcome};
            Yield assault_step;
            assault_step.what = "assault";
            ai_co->YieldValue(assault_step);
            if (window) return;
        }
        Status(outcome, 5);
    };
    w2::ai::hooks.on_spoils = nullptr;
}

/**
 * @brief The coroutine's body: plays computer sides from `side` on, until a
 *        human's turn or the end, yielding to the screen as it goes.
 *
 * Leaves the side whose turn comes next, or nullptr at the end, in
 * ai_result.
 */
static void ComputerTurns(Coroutine& coroutine, Side* side) {
    ai_co = &coroutine;
    auto yield_what = [&coroutine](const char* what) {
        Yield step;
        step.what = what;
        coroutine.YieldValue(step);
    };
    // What the round's end found, as the original tells it inside the
    // computer's turns: each side put out in a box, or -- with no human
    // playing -- in the status bar for 100 ticks, then the last human's fall
    // in two boxes. The rest waits for the turn that follows (ui/ending.cpp).
    auto announce = [&coroutine, &yield_what]() {
        w2::Ending& round_end = G.g->ending;
        if (round_end.told) return;
        round_end.told = true;
        auto fallen = round_end.fallen;
        bool no_humans = round_end.no_humans;
        for (auto& fall : fallen) {
            std::string text = ending::FallenText(fall);
            if (fall.boxed) {
                Yield step;
                step.what = "fallen";
                step.text = text;
                coroutine.YieldValue(step);
            } else {
                Status(text, 100);
            }
        }
        if (no_humans) yield_what("nohumans");
    };
    announce();  // a human's turn may have ended the round
    while (side && side->computer) {
        G.ai_side = side;
        // each computer turn opens with a song that plays once
        sound::Music(w2::cues::kComputer, G.g->sides);
        G.ai_status = AiStatus{side, side->name, 5};
        yield_what("progress");
        if (ShownSide(side)) {
            auto news = side->diplo_news;
            for (auto& news_line : news) Status(news_line, 20);
        }
        side->diplo_news.clear();
        w2::ai::PlayTurn(*G.g, *side, [&](const char* phase) {
            G.ai_status->progress = ProgressOf(phase);
            yield_what("progress");
        });
        G.ai_status->progress = 100;
        yield_what("progress");
        side = game::EndTurn(*G.g);
        announce();
        yield_what("turn");
    }
    ai_result = side;
}

/**
 * @brief Runs the computer's coroutine on to its next yield, and acts on
 *        what it yielded: a pause, a walk, a message, or the round's end.
 */
static void ResumeComputer() {
    if (!G.ai_run) return;
    G.ai_wait = false;
    bool more = G.ai_run->Resume();
    if (!more) {
        G.ai_run.reset();
        ai_co = nullptr;
        G.ai_skip = false;
        G.ai_side = nullptr;
        FinishRound(ai_result);
        return;
    }
    front::StratDirty();
    const Yield& step = G.ai_run->Value();
    if (step.what == "pause") {
        G.ai_resume_at = w2::Now() + step.ticks / 18.2;
        G.ai_wait = true;
    } else if (step.what == "walk") {
        StartWalk(step.armies, step.tiles, step.sea_before);
        if (G.walk)
            G.walk->computer = true;
        else
            G.ai_wait = true;
    } else if (step.what == "nohumans") {
        // the last human is gone, and the war goes on
        auto text_of = [](int line) { return kit::Text(0xd, line); };
        search::ShowMessage(text_of(0), text_of(1), [text_of]() {
            search::ShowMessage(text_of(2), text_of(3), []() { G.ai_wait = true; });
        });
    } else if (step.what == "fallen") {
        search::Say(step.text, []() { G.ai_wait = true; });
    } else {
        // a turn done, or a battle up: go on at the next frame free of it
        G.ai_wait = true;
    }
}

/** @brief Called every frame: carries the computer on once nothing is in its way. */
static void StepComputer() {
    if (!G.ai_run || !G.ai_wait || G.walk || G.assault || kit::Top() || G.open_menu >= 0) return;
    if (G.ai_resume_at > 0) {
        if (w2::Now() < G.ai_resume_at && !G.ai_skip) return;
        G.ai_resume_at = 0;
    }
    // only a Shift or Alt pressed since the round began: the Alt of Alt-E
    // is still down as the computer starts
    bool mods = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    if (!mods) G.ai_mods_held = false;
    if (mods && !G.ai_mods_held) {
        G.ai_mods_held = true;
        G.ai_skip = false;
        settings::Open();
        return;
    }
    // several steps a frame while running through unwatched
    double until = w2::Now() + 0.03;
    do {
        ResumeComputer();
    } while (G.ai_skip && G.ai_run && G.ai_wait && !G.walk && !G.assault && !kit::Top() &&
             G.ai_resume_at == 0 && w2::Now() < until);
}

/** @brief Plays computer sides from `side` on, until a human's turn or the end. */
static void PlayComputers(Side* side) {
    G.ai_mods_held = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    ai_result = nullptr;
    G.ai_run = std::make_unique<Coroutine>(
            [side](Coroutine& coroutine) { ComputerTurns(coroutine, side); });
    ResumeComputer();
}
/** @brief Plays the computer sides from the side whose turn it is. */
void front::PlayComputer() {
    PlayComputers(G.g->side);
}

/** @brief End Turn: hands over to the next side, computer or human. */
static void EndTurn() {
    // End Turn does nothing once the game is won: the winner stays on the
    // turn, to look the kingdom over
    if (G.g->won) return;
    // It puts the stack down, and its route with it: the next player sees no
    // other side's plans
    G.selection.reset();
    G.route.reset();
    Side* side = game::EndTurn(*G.g);
    if (side && side->computer) return PlayComputers(side);
    FinishRound(side);
}

/**
 * @brief The round is over: the next human side takes the keyboard, or with
 *        none the game is over.
 */
static void FinishRound(Side* side) {
    if (!side) {
        G.ai_status.reset();
        G.over = true;
        ending::Over(G.g->ending);
        return;
    }
    G.player = side;
    G.ai_status.reset();  // the bar is the player's again
    front::StratDirty();
    CycleReset();
    // the original centres on the side's capital -- but where the map is
    // hidden and more than one human plays, only once the banner is gone,
    // and until then neither map is shown
    G.map_after_banner = HotseatHidden();
    if (!G.map_after_banner) CentreOn(CapOf(side).first, CapOf(side).second);
    ending::Show(G.g->ending, [side]() { ShowBanner(side); });
}

/** @brief Opens the city's dialog. */
void front::OpenCity(City* city) {
    city::Open(city);
}
/** @brief Opens the quest dialog. */
void front::OpenQuest() {
    questui::Open();
}

/**
 * @brief Takes up a game just loaded: its military advisor setting, a clean
 *        screen, and the view on its current side's capital.
 */
void front::TakeLoaded(std::unique_ptr<w2::Game> loaded) {
    G.g = std::move(loaded);
    if (opt_military_advisor >= 0) {
        G.g->map->options.military_advisor = opt_military_advisor;
    } else if (G.g->map->options.military_advisor == 0) {
        G.g->map->options.military_advisor = 1;
    }
    G.selection.reset();
    G.route.reset();
    G.walk.reset();
    G.over = false;
    G.starting = false;
    G.map_after_banner = false;
    front::StratDirty();
    G.player = G.g->sides[G.g->current];
    CentreOn(CapOf(G.player).first, CapOf(G.player).second);
    sound::Music(w2::cues::kPlay);
}

// ------------------------------------------------------------------ drawing

/** @brief The army a stack shows: the one first in the fight order. */
static Army* TopArmy(const std::vector<Army*>& stack) {
    Army* best = nullptr;
    int best_rank = kNone;
    for (Army* army : stack) {
        int rank = G.g->map->fight_order[OwnerOr8(army->owner)][army->type];
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
    int viewer = game::ViewerSide(*G.g, G.player);
    if (G.g->map->options.hidden_map == 0 || viewer == kNone) return;
    for (int tile_y = G.view[1]; tile_y <= G.view[3]; tile_y++) {
        for (int tile_x = G.view[0]; tile_x <= G.view[2]; tile_x++) {
            int cell = game::FogCell(*G.g, viewer, tile_x, tile_y);
            if (cell == kNone) continue;
            int pixel_x = tile_x * kTile, pixel_y = tile_y * kTile;
            if (cell == game::kFogBlack) {
                gfx::SetColor(0, 0, 0);
                gfx::Rectangle(gfx::kFill, pixel_x, pixel_y, kTile, kTile);
            } else {
                gfx::SetColor(1, 1, 1);
                gfx::Draw(G.fog_img, gfx::NewQuad((cell % 7) * 48, (cell / 7) * 41, 40, 40),
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
    gfx::Draw(G.army_img[owner], kit::ArmyQuad(army_type), pixel_x + 8, pixel_y + 7);
    PalColour(14);
    gfx::Rectangle(gfx::kFill, pixel_x + 2, pixel_y, 1, kTile);
    gfx::Rectangle(gfx::kFill, pixel_x + 4, pixel_y, 1, kTile);
    PalColour(13);
    gfx::Rectangle(gfx::kFill, pixel_x + 3, pixel_y, 1, kTile);
    gfx::SetColor(1, 1, 1);
    auto flag = [&](int flag_size, int y) {
        gfx::Draw(G.army_img[owner], gfx::NewQuad(464, kFlagY[flag_size - 1], 48, 8), pixel_x, y);
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
    G.view = ViewTiles();
    const auto& view = G.view;
    std::map<int, std::vector<Army*>> armies_on;
    for (Army* army : G.g->armies) {
        if (!army->transit && army->x != kNone && army->x >= view[0] && army->x <= view[2] &&
            army->y >= view[1] && army->y <= view[3])
            armies_on[army->x + army->y * 1000].push_back(army);
    }
    // one item a tile: the last in item order (the original walks them from
    // the end)
    std::map<int, w2::Item*> items_on_tile;
    for (int i = static_cast<int>(G.g->map->items.size()) - 1; i >= 0; i--) {
        w2::Item& item = G.g->map->items[i];
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
            int tile = w2::scn::TileAt(*G.g->map, tile_x, tile_y);
            int sheet = std::min(1, tile / 96);
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.sheets[sheet], TileQuad(tile), pixel_x, pixel_y);
            int road = w2::scn::RoadAt(*G.g->map, tile_x, tile_y);
            if (road != 0) gfx::Draw(G.road_img, RoadQuad(road - 1), pixel_x, pixel_y);
            // a port -- a crossing, map word bit 15 -- takes the anchor, at the
            // tile's corner after the roads
            if (w2::scn::IsCrossing(*G.g->map, tile_x, tile_y))
                gfx::Draw(G.atrans_shields, gfx::NewQuad(0, 0, 32, 29), pixel_x, pixel_y);
            // items on the ground: a planted standard as its side's flag, army
            // cell 29; anything else as the bag
            auto here = items_on_tile.find(tile_x + tile_y * 1000);
            if (here != items_on_tile.end()) {
                w2::Item* item = here->second;
                if (item->planted && item->standard_of != kNone)
                    gfx::Draw(G.army_img[item->standard_of], kit::ArmyQuad(29), pixel_x, pixel_y);
                else
                    gfx::Draw(G.atrans_shields, gfx::NewQuad(64, 0, 32, 29), pixel_x, pixel_y);
            }
            auto armies_here = armies_on.find(tile_x + tile_y * 1000);
            if (armies_here != armies_on.end()) {
                std::vector<Army*> stack;
                for (Army* army : armies_here->second)
                    if (!(G.walk && G.walk->armies.count(army))) stack.push_back(army);
                if (!stack.empty())
                    standing.push_back(Standing{stack, tile_x, tile_y, pixel_x, pixel_y});
            }
        }
    }
    // the ground first, then the encampments, then the stacks
    for (auto& stack_here : standing) {
        if (game::TowerAt(*G.g, stack_here.x, stack_here.y)) {
            int owner = OwnerOr8(stack_here.stack[0]->owner);
            if (owner == 15) owner = 8;
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.road_img, gfx::NewQuad(owner * 48 + 192, 40, 48, 40), stack_here.sx,
                      stack_here.sy);
            stack_here.hidden = !(G.selection && G.selection->x == stack_here.x &&
                                  G.selection->y == stack_here.y);
        }
    }
    // the selected stack's tile shows the lead of the group that moves
    Army* lead = G.selection && !G.selection->stack.empty() ? TopArmy(G.selection->stack) : nullptr;
    if (lead && G.walk && G.walk->armies.count(lead)) lead = nullptr;
    for (auto& stack_here : standing) {
        if (stack_here.hidden) continue;
        Army* top = TopArmy(stack_here.stack);
        int figure = StackFigure(stack_here.stack);
        if (lead && G.selection->x == stack_here.x && G.selection->y == stack_here.y) {
            top = lead;
            figure = lead->at_sea ? kNavy : lead->type;
        }
        DrawStack(OwnerOr8(top->owner), figure, static_cast<int>(stack_here.stack.size()),
                  stack_here.sx, stack_here.sy);
    }
    DrawFog();
    DrawRoute();
    // the stack itself, wherever the walk has got to
    const auto* at = WalkingAt();
    if (at) {
        Army* walker = G.walk->top;
        if (walker && Shown(at->first, at->second)) {
            std::vector<Army*> walking(G.walk->armies.begin(), G.walk->armies.end());
            Army* top = TopArmy(walking);
            int figure = G.walk->sea_before ? kNavy : (top ? top->type : kNone);
            DrawStack(OwnerOr8(walker->owner), figure, static_cast<int>(walking.size()),
                      at->first * kTile, at->second * kTile);
        }
    }
    // the selection box, which follows the walk
    if (G.selection) {
        int x = at ? at->first : G.selection->x, y = at ? at->second : G.selection->y;
        if (Shown(x, y)) {
            gfx::SetColor(1, 1, 1);
            int base = G.selection->stack.size() > 1 ? 4 : 0;
            int frame = base + static_cast<int>(std::floor(w2::Now() * kCursRate)) % 4;
            gfx::Draw(G.curs_img, gfx::NewQuad((frame % 4) * 64, (frame / 4) * 40, 40, 40),
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
void front::DrawStrategicMap(int x, int y, const City* mark, bool no_cities,
                             const std::vector<int>* owners, int scroll, int rows) {
    int viewer = game::ViewerSide(*G.g, G.player);
    if (!G.strat_image || strat_viewer != viewer) {
        G.strat_image = screen::StrategicImage(*G.screen, *G.g, viewer);
        strat_viewer = viewer;
    }
    gfx::SetScissor(x, y, 224, rows);
    y -= scroll;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.strat_image, x, y);
    if (!no_cities) {
        auto& cities = G.g->map->cities;
        for (size_t i = 0; i < cities.size(); i++) {
            const City& city = cities[i];
            int owner = owners && i < owners->size() ? (*owners)[i] : kNone;
            bool gone = owners ? owner == 0xff : city.razed;
            if (!gone && game::Seen(*G.g, viewer, city.x, city.y)) {
                int side = owners ? owner : OwnerOr8(city.owner_index);
                gfx::Draw(G.atrans_shields, gfx::NewQuad(side * 16, 30, 8, 8), x + city.x * 2 - 1,
                          y + city.y * 2 - 1);
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
    if (G.modern_ui && !no_cities && G.player && viewer != kNone) {
        for (int ally = 0; ally < 8; ally++) {
            if (ally != G.player->index && w2::diplomacy::IsAllied(*G.g, G.player->index, ally)) {
                int target_index = w2::diplomacy::AlliedTargetCity(*G.g, ally);
                if (const City* target = G.g->map->City(target_index)) {
                    if (!target->razed && game::Seen(*G.g, viewer, target->x, target->y)) {
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
void front::DrawSiteMarkers(int x, int y, const w2::Site* highlight) {
    int viewer = game::ViewerSide(*G.g, G.player);
    gfx::SetScissor(x, y, 224, 312);
    for (auto& site : G.g->map->sites) {
        if (!(viewer == kNone ||
              (w2::site::ShownTo(site, *G.player) && game::Seen(*G.g, viewer, site.x, site.y))))
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
        gfx::Draw(G.atrans_shields, gfx::NewQuad(src_x, src_y, 16, 10), x + marker_x, y + marker_y);
        if (&site == highlight) {
            kit::SetPal(15);
            kit::Outline(x + marker_x, y + marker_y, 12, 10);
        }
    }
    gfx::SetScissor();
}

/// How many rows down the main screen's strategic map is scrolled (FollowView).
static int strat_scroll = 0;

/**
 * @brief Scrolls the main screen's strategic map to keep the view's box in
 *        its middle.
 *
 * Not the original's: on a short screen the strategic map's frame shows only
 * some of it, so that a click towards an edge brings more of the world in
 * from that side.
 */
static void FollowView() {
    const auto& rect = G.strat_rect;
    int full = G.g->map->height * 2;
    auto [view_w, view_h] = ViewSize();
    // a whole tile at a time
    int scroll = static_cast<int>(std::lround(G.cy + view_h / 2 - rect.h / 4.0)) * 2;
    strat_scroll = std::clamp(scroll, 0, std::max(0, full - rect.h));
}

/**
 * @brief Draws the main screen's strategic map with the view's box on it:
 *        whatever the view covers, to the device pixel.
 */
static void DrawStrategic() {
    const auto& rect = G.strat_rect;
    FollowView();
    front::DrawStrategicMap(rect.x, rect.y, nullptr, false, nullptr, strat_scroll, rect.h);
    gfx::SetScissor(rect.x, rect.y, rect.w, rect.h);
    // the view box: whatever the view covers, to the device pixel
    PalColour(15);
    double scale = display::d.scale;
    auto snap = [scale](double value) { return std::floor(value * scale + 0.5) / scale; };
    auto [view_w, view_h] = ViewSize();
    double box_x = rect.x + snap(G.cx * 2), box_y = rect.y - strat_scroll + snap(G.cy * 2);
    double box_w = snap(view_w * 2), box_h = snap(view_h * 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y, box_w, 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y + box_h - 2, box_w, 2);
    gfx::Rectangle(gfx::kFill, box_x, box_y, 2, box_h);
    gfx::Rectangle(gfx::kFill, box_x + box_w - 2, box_y, 2, box_h);
    gfx::SetScissor();
}

/** @brief Has the strategic map's image made anew before it is next drawn. */
void front::StratDirty() {
    G.strat_image.reset();
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
    auto& main_ui = G.screen->ui;
    auto art = G.screen->ArtFor(w2::uidata::kShortcutBitmap);
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        auto control = screen::FindControl(*G.screen, w2::uidata::kShortcutFirst + i);
        auto shortcut = main_ui.shortcuts.find(i);
        if (!control || shortcut == main_ui.shortcuts.end()) continue;
        auto item = main_ui.shortcut_items.find(shortcut->second);
        if (item == main_ui.shortcut_items.end()) continue;
        if (art) {
            auto src = item->second.src[G.screen->StateOf(control->id)];
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(src.x, src.y, item->second.w, item->second.h),
                      control->x, control->y);
        } else {
            std::string short_name = item->second.name.substr(0, 6);
            gfx::SetColor(1, 1, 1);
            G.font->Draw(short_name,
                         control->x + std::max(1, (control->w - G.font->Width(short_name)) / 2),
                         control->y + (control->h - G.font->line_height) / 2);
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
    for (Side* side : G.g->sides)
        if (side->alive) alive.push_back(side);
    int n = static_cast<int>(alive.size());
    std::string turn = w2::Fmt("Turn %d", G.g->turn);
    const Font& font = G.big_font->Colours(0, 15);
    font.Draw(turn, (8 - n) * 16 + 508 - font.Width(turn), 0);
    const Side* active_side = (G.ai_side && !HumansLeft()) ? G.ai_side : G.player;
    for (int i = 0; i < n; i++) {
        int x = (8 - n + i) * 16 + 512;
        if (alive[i] == active_side) {
            PalColour(0);
            gfx::Rectangle(gfx::kFill, x - 3, 1, 17, 15);
        }
        int side_index = alive[i]->index;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.atrans_shields,
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
    gfx::Rectangle(gfx::kFill, 0, 0, G.layout.w, menu::kBarH);
    for (size_t i = 0; i < G.menu_layout.size(); i++) {
        const auto& menu_entry = G.menu_layout[i];
        bool lit = static_cast<int>(i) == G.open_menu;
        if (lit) {
            PalColour(8);
            gfx::Rectangle(gfx::kFill, menu_entry.x, 0, menu_entry.w, menu::kBarH);
        }
        gfx::SetColor(1, 1, 1);
        G.font->Colours(lit ? 7 : 0, lit ? 8 : 15)
                .Draw(menu_entry.title, menu_entry.x + 2, menu::kBarY);
    }
    if (!G.starting && G.g) {
        gfx::Push();
        gfx::Translate(G.layout.ex, 0);
        DrawTurnStrip();
        gfx::Pop();
    }
    if (G.open_menu < 0 || G.open_menu >= static_cast<int>(G.menu_layout.size())) return;
    const auto& drop = G.menu_layout[G.open_menu].drop;
    PalColour(15);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y, drop.w - 1, drop.h - 1);
    PalColour(0);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y + drop.h - 2, drop.w - 2, 1);
    gfx::Rectangle(gfx::kFill, drop.x + 2, drop.y + drop.h - 1, drop.w - 2, 1);
    gfx::Rectangle(gfx::kFill, drop.x, drop.y, 1, drop.h - 2);
    gfx::Rectangle(gfx::kFill, drop.x + drop.w - 2, drop.y, 1, drop.h - 1);
    gfx::Rectangle(gfx::kFill, drop.x + drop.w - 1, drop.y + 1, 1, drop.h - 1);
    const menu::Row* hover = menu::RowAt(G.menu_layout, G.open_menu, G.mouse_x, G.mouse_y);
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
        const Font& font = G.font->Colours(glyph, lit ? 8 : 15);
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
static const int kSlotFirst = 224, kBarFirst = 232, kSlotCount = 8;
static const int kGrpAll = 240, kGrpNone = 241;
static const int kSlotX = 24, kSlotY = 405, kSlotStep = 40;
static const int kSlotMovesX = 8, kSlotMovesY = 31;
static const int kMarkY = 449, kMarkW = 32, kMarkH = 16;

/** @brief Draws the ABITS.PCK cell at (src_x, src_y), width x height, at (x, y). */
static void DrawAbits(int src_x, int src_y, int width, int height, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.abits, gfx::NewQuad(src_x, src_y, width, height), x, y);
}

/** @brief Draws a number in ABITS's own 8x8 digits, always two of them ("%02d"). */
static void DrawDigits(int number, int x, int y) {
    std::string text = w2::Fmt("%02d", std::max(0, std::min(99, number)));
    for (size_t i = 0; i < text.size(); i++)
        DrawAbits(64 + (text[i] - '0') * 8, 30, 8, 8, x + static_cast<int>(i) * 8, y);
}

/**
 * @brief The ring a slot's army sits in: each group a colour of its own,
 *        counted round from the current player's.
 */
static int GroupRing(int group) {
    return (G.player->index + group) % 8 + 1;
}

/**
 * @brief Draws the bottom bar with a stack selected: its eight slots, the
 *        group's movement, the Grp button and the way the group travels.
 */
static void DrawArmySlots() {
    const slots_mod::Slots* slots = G.selection ? &G.selection->slots : nullptr;
    for (int i = 0; i < kSlotCount; i++) {
        int x = kSlotX + i * kSlotStep;
        Army* army = slots && i < slots->n ? slots->army[i] : nullptr;
        gfx::SetColor(1, 1, 1);
        int ring = army ? GroupRing(slots->group[i]) : 0;
        gfx::Draw(G.abits, gfx::NewQuad(ring * 32, 0, 32, 30), x, kSlotY);
        if (army) {
            // the armies not moving with the group are drawn as ghosts
            gfx::Draw(slots->in_group[i] ? G.army_img[G.player->index] : G.shadow_img,
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
            const w2::ArmyType* army_type = G.g->types.ById(army->type);
            if (army_type->woods_move) woods = true;
            if (army_type->hills_move) hills = true;
            if (!army->at_sea) sea = false;
        }
        if (move::ModeOf(*G.g, moving) == move::kFlying)
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
        return static_cast<int>(game::SideCities(*G.g, *G.player).size());
    case 1:
        return G.player->gold;
    case 2:
        return game::Income(*G.g, *G.player);
    default:
        return game::Upkeep(*G.g, *G.player);
    }
}

/** @brief Draws the side's standing on the bottom bar, nothing being selected. */
static void DrawStatus() {
    for (int cell = 0; cell < 4; cell++) {
        const auto& status_cell = kStatusCells[cell];
        DrawAbits(status_cell.sx, status_cell.sy, 40, 20, status_cell.x, kStatusY);
        G.big_font->Draw(w2::Format(status_cell.fmt, StatusValue(cell)), status_cell.tx, kStatusY);
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
    const AiStatus& status = *G.ai_status;
    Side* side = status.side;
    gfx::SetColor(1, 1, 1);
    kit::Centred(G.big_font->Colours(side->colour, side->edge), status.text, 196, 415);
    PalColour(0);
    kit::Outline(36, 442, 320, 22);
    int bar_width = status.progress / 5 * 16;
    if (bar_width > 0) {
        int side_index = side->index;
        if (!G.move_bars.count(side_index)) {
            try {
                G.move_bars[side_index] = pckimage::Load(
                        G.data_dir + "/TERRAIN0/MOVEBAR" + std::to_string(side_index) + ".PCK",
                        G.palette, pckimage::kCorner);
            } catch (...) {
                G.move_bars[side_index] = nullptr;
            }
        }
        gfx::SetColor(1, 1, 1);
        if (G.move_bars[side_index])
            gfx::Draw(G.move_bars[side_index], gfx::NewQuad(0, 0, std::min(bar_width, 320), 18), 32,
                      444);
    }
}

/**
 * @brief The right button on the bottom bar: an information box about the
 *        slot or the standing cell under the pointer.
 */
static void BarInfo(int x, int y) {
    int bar_x = x - G.layout.bar.x;
    if (G.ai_status) return;
    if (G.selection) {
        if (bar_x < 336) {
            int slot = (bar_x - 16) / 40;
            if (bar_x >= 16 && slot < G.selection->slots.n)
                infobox::Army(x, y, G.selection->slots.army[slot]);
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
    gfx::Draw(G.marble, gfx::NewQuad(0, 60, 360, 66), 16, 403);
    if (G.ai_status)
        DrawComputerStatus();
    else if (G.selection)
        DrawArmySlots();
    else
        DrawStatus();
}

// The city dialog's Vector mode paints the map its own way (834b:08df, and
// 834b:1817 for See All): a marker on each of the side's cities from
// ATRANS2.PCK's row at y = 94 (4125:2c76), and lines for the vectors.
static const int kVmarkX[7] = {0, 32, 48, 16, 64, 80, 96};

// 2133:00b8 draws a plain Bresenham line, both ends included
static void LinePixels(int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx::Rectangle(gfx::kFill, x0, y0, 1, 1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void front::DrawVectorMap(int x, int y, City* city, int filter, bool see_all) {
    if (!G.strat_image) G.strat_image = screen::StrategicImage(*G.screen, *G.g, G.player->index);
    gfx::SetScissor(x, y, 224, 312);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.strat_image, x, y);
    auto marker = [&](const City* c, int k) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.atrans_shields, gfx::NewQuad(kVmarkX[k], 94, 16, 10), x + c->x * 2 - 2,
                  y + c->y * 2 - 1);
    };
    auto line = [&](const City* a, const City* b, int colour) {
        PalColour(colour);
        LinePixels(x + a->x * 2 + 2, y + a->y * 2 + 2, x + b->x * 2 + 2, y + b->y * 2 + 2);
    };
    auto mine = game::SideCities(*G.g, *G.player);
    auto building = [](const City* c) { return game::Producing(*c) ? 0 : 3; };
    auto [sx, sy] = game::StandardAt(*G.g, *G.player);
    auto to_standard = [&, sx = sx, sy = sy](const City* c, int colour) {
        PalColour(colour);
        LinePixels(x + c->x * 2 + 2, y + c->y * 2 + 2, x + sx * 2, y + sy * 2);
    };
    if (see_all) {
        std::set<const City*> done;
        for (City* c : mine) {
            if (done.count(c)) continue;
            done.insert(c);
            auto incoming = game::VectoredTo(*G.g, c);
            int k = building(c);
            if (c->vector_to != kNone) k = 2;
            if (!incoming.empty()) k = game::Producing(*c) ? 1 : 5;
            marker(c, k);
            City* dest = c->vector_to >= 0 ? G.g->map->City(c->vector_to) : nullptr;
            if (dest) {
                done.insert(dest);
                marker(dest, game::Producing(*dest) ? 1 : 5);
                line(dest, c, 7);
            } else if (c->vector_to == game::kStandard && sx != kNone) {
                to_standard(c, 8);
            }
            for (City* src : incoming) {
                done.insert(src);
                marker(src, 2);
                line(src, c, 8);
            }
        }
        PalColour(15);
        int bx = x + (city ? city->x : -10) * 2 - 3, by = y + (city ? city->y : -10) * 2 - 2;
        gfx::Rectangle(gfx::kFill, bx, by, 11, 1);
        gfx::Rectangle(gfx::kFill, bx, by + 11, 11, 1);
        gfx::Rectangle(gfx::kFill, bx, by, 1, 11);
        gfx::Rectangle(gfx::kFill, bx + 11, by, 1, 11);
    } else if (city) {
        for (City* c : mine) {
            int n = static_cast<int>(game::VectoredTo(*G.g, c).size());
            bool full = (filter == -1 && n + 1 > game::kMaxVectoredTo) ||
                        (filter >= 0 && filter != kNone && n + filter > game::kMaxVectoredTo);
            if (c == city || !full) {
                int k = building(c);
                if (c == city) k = game::Producing(*c) ? 4 : 6;
                marker(c, k);
            }
        }
        City* dest = city->vector_to >= 0 ? G.g->map->City(city->vector_to) : nullptr;
        if (dest) {
            marker(dest, game::Producing(*dest) ? 1 : 5);
            line(city, dest, 7);
        } else if (city->vector_to == game::kStandard && sx != kNone) {
            to_standard(city, 7);
        }
        for (City* src : game::VectoredTo(*G.g, city)) {
            marker(src, 2);
            line(src, city, 8);
        }
    }
    if (sx != kNone) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.atrans_shields, gfx::NewQuad(96, 15, 16, 15),
                  x + std::max(0, (sx * 2 - 2) / 8 * 8), y + std::max(0, sy * 2 - 6));
    }
    gfx::SetScissor();
}

// The figure is 834b:1f5f's third argument, a rect from 4125:2cae: 1, (96, 0)
// outlined in white, everywhere but the hero dialog, which draws the other
// heroes with 0, (96, 15), dark (6c1b:10ac).
void front::DrawHeroFigure(int x, int y, int tx, int ty, bool dark) {
    gfx::SetScissor(x, y, 224, 312);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.atrans_shields, gfx::NewQuad(96, dark ? 15 : 0, 16, 15),
              x + std::max(0, (tx * 2 - 2) / 8 * 8), y + std::max(0, ty * 2 - 6));
    gfx::SetScissor();
}

void front::DrawStrategicPanel(int x, int y, const City* city) {
    DrawStrategicMap(x, y, city);
}

// The start-of-turn banner: CITY.PCK framed in the side's colour, the frame
// painted into the picture (54f6:0000).
static void DrawBanner() {
    const Banner& b = *G.banner;
    const auto& popup_rect = kBanner;
    auto fill = [](int x, int y, int w, int h) { gfx::Rectangle(gfx::kFill, x, y, w, h); };
    kit::PopupFrame(popup_rect);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.city_pic, popup_rect.x, popup_rect.y);
    auto outline = [&](int x, int y, int w, int h) {
        fill(popup_rect.x + x, popup_rect.y + y, w, 1);
        fill(popup_rect.x + x, popup_rect.y + y + h - 1, w, 1);
        fill(popup_rect.x + x, popup_rect.y + y, 1, h);
        fill(popup_rect.x + x + w - 1, popup_rect.y + y, 1, h);
    };
    PalColour(b.edge);
    outline(0, 0, popup_rect.w, popup_rect.h);
    PalColour(b.colour);
    fill(popup_rect.x + 1, popup_rect.y + 1, popup_rect.w - 2, 9);
    fill(popup_rect.x + 1, popup_rect.y + 1, 9, popup_rect.h - 2);
    fill(popup_rect.x + 1, popup_rect.y + popup_rect.h - 10, popup_rect.w - 2, 9);
    fill(popup_rect.x + popup_rect.w - 10, popup_rect.y + 1, 9, popup_rect.h - 2);
    PalColour(b.edge);
    outline(10, 10, popup_rect.w - 20, popup_rect.h - 20);
    gfx::SetColor(1, 1, 1);
    kit::Centred(*G.title_font, b.name, 320, kBannerNameY);
    kit::Centred(*G.title_font, w2::Fmt("Turn %d", b.turn), 320, kBannerTurnY);
}

// The hero offer: the marble, the map with where the hero would appear,
// the portrait, four lines, the name field and the two checkboxes.
static void DrawHeroOffer() {
    const auto& popup_rect = kHeroPopup;
    const w2::HeroOffer& b = *G.offer;
    kit::PopupFrame(popup_rect);
    gfx::SetColor(1, 1, 1);
    gfx::SetScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::Draw(G.marble, popup_rect.x, popup_rect.y);
    gfx::SetScissor();
    front::DrawStrategicMap(kHeroMapX, kHeroMapY);
    gfx::SetScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::SetColor(1, 1, 1);
    int fx = std::max(0, (b.city->x * 2 - 2) / 8 * 8);
    int fy = std::max(0, b.city->y * 2 - 6);
    gfx::Draw(G.atrans_shields, gfx::NewQuad(96, 0, 16, 15), kHeroMapX + fx, kHeroMapY + fy);
    gfx::Draw(G.offer_female ? G.hero_pic_f : G.hero_pic_m, kHeroPicX, kHeroPicY);
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, kHeroPicX - 0.5, kHeroPicY - 0.5, kHeroPicW + 1, kHeroPicH + 1);
    gfx::SetScissor();
    auto centred = [](const Font& f, const std::string& s, int y) {
        gfx::SetColor(1, 1, 1);
        f.Draw(s, kHeroCentre - f.Width(s) / 2, y);
    };
    centred(*G.title_font, kit::Text(kHeroTitleGroup, 0), kHeroTitleY);
    auto lines = HeroLines(b, G.offer_female);
    for (int i = 0; i < 4; i++) {
        // font 2 in 15 with a colour-14 outline (78a8:06ae(2, 15, 14, 3))
        if (!lines[i].empty()) centred(G.big_font->Colours(15, 14), lines[i], kHeroLineY[i]);
    }
    if (auto field = screen::DialogControl(*G.hero_view, kHeroField)) {
        gfx::SetColor(0, 0, 0);
        kit::Outline(field->x - 2, field->y - 2, field->w + 4, field->h + 4);
        kit::Field(field->x, field->y, field->w, field->h, G.offer_name, G.big_font.get());
    }
    auto label = [](const int* at, const std::string& s) {
        gfx::SetColor(1, 1, 1);
        G.big_font->Draw(s, at[0] - G.big_font->Width(s), at[1]);
    };
    label(kMaleLabel, kit::Text(kHeroSexGroup, 0));
    label(kFemaleLabel, kit::Text(kHeroSexGroup, 1));
    auto box = [](int id, bool on) {
        auto c = screen::DialogControl(*G.hero_view, id);
        if (!c) return;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.abits, gfx::NewQuad(320, on ? 0 : 20, 24, 20), c->x, c->y);
    };
    box(kHeroMale, !G.offer_female);
    box(kHeroFemale, G.offer_female);
    screen::DrawDialogControls(*G.screen, *G.hero_view);
}

// ------------------------------------------------------------------ the assault

// A city is not walked into, it is assaulted, and a stack always fights from
// one of the eight tiles around it. What the player sees, in order: the fire
// cloud from WAR.PCK over the tile (67cc:1836), the battle window with both
// lines drawn up (6a35:0160), the fight played back one army at a time in
// the order combat_resolve logged (6a35:0094), how it ended (6a35:04c5), and
// if a city fell, what is to be done with it (63fa:0000). The fight is
// decided before any of it is drawn (docs/re/ui.md > The assault).
static const kit::Rect kAsWindow{160, 60, 320, 312};  // popup 8
static const int kAsCloudW = 128, kAsCloudH = 120;    // 4125:0cfa
static const int kAsShieldW = 32, kAsShieldH = 36;
static const int kAsShieldDef[2] = {176, 86}, kAsShieldAtk[2] = {176, 246};
static const std::vector<int> kAsRows = {86, 116, 146, 176};  // the defender's rows
static const int kAsAtkRow = 246;
static const int kAsXEven[8] = {216, 248, 280, 312, 344, 376, 408, 440};
static const int kAsXOdd[7] = {232, 264, 296, 328, 360, 392, 424};
static const int kAsSea[4] = {0, 162, 32, 18}, kAsSeaDrop = 10;  // WAR.PCK's water
static const int kAsTextY = 298, kAsTextStep = 20;               // 4125:4366
static const double kAsCloudTime = 0.7;
// one casualty (6a35:0094): 5, 3 and 5 BIOS ticks; once hurried, 2 and 3
static const double kAsFellTime = 13 / 18.2, kAsFellFast = 5 / 18.2;
static const double kAsBlastTime = 5 / 18.2, kAsBlastFast = 2 / 18.2;
static const int kAsFled = 141, kAsWonCityHero = 142, kAsWonCity = 143, kAsWonHero = 144,
                 kAsWon = 145, kAsLost = 146, kAsLoot = 147;
static const int kAsVText = 65, kAsVWho = 66, kAsVWhere = 67;
static const kit::Rect kAsVPopup{160, 90, 320, 200};  // popup 7, exactly VICTORY.PCK
static const int kAsVDialog = 11;
static const int kAsOccupy = 285, kAsPillage = 283, kAsSack = 286, kAsRaze = 284;
static const int kAsVTitleY = 93;
static const int kAsVLineY[4] = {140, 160, 180, 200};

// Where slot k (from 1) of a line of n sits across the window (6a35:0160).
static int SlotX(int n, int k) {
    if (n >= 8) return kAsXEven[k - 1];
    int from = 4 - (n + 1) / 2;
    return n % 2 == 1 ? kAsXOdd[from + k - 1] : kAsXEven[from + k - 1];
}

// A whole line laid out: rows of eight from the top, the last centred.
static std::vector<std::pair<int, int>> LineSlots(int n, const std::vector<int>& rows) {
    std::vector<std::pair<int, int>> out(n);
    int i = 1;
    size_t row = 0;
    while (n - i + 1 >= 8 && row + 1 < rows.size()) {
        for (int k = 1; k <= 8; k++) out[i + k - 2] = {kAsXEven[k - 1], rows[row]};
        i += 8;
        row++;
    }
    int y = row < rows.size() ? rows[row] : rows.back();
    for (int k = 1; k <= n - i + 1; k++) out[i + k - 2] = {SlotX(n - i + 1, k), y};
    return out;
}

// The name the spoils dialog calls the victor by: the hero who led the
// assault, else the best army in the line.
static std::string VictorName(const std::vector<Army*>& line) {
    for (Army* a : line)
        if (a->Hero() && !a->name.empty()) return a->name;
    const w2::ArmyType* rec = line.empty() ? nullptr : G.g->types.ById(line[0]->type);
    return rec ? rec->name : "Your armies";
}

// How the fight ended, in the original's own words.
static std::vector<std::string> OutcomeLines(const w2::Battle& result, const City* city,
                                             bool fled) {
    std::vector<std::string> out;
    if (!result.won) {
        out.push_back(kit::Text(kAsLost, 0));
        return out;
    }
    std::string hero_name;
    for (Army* a : result.lines.attackers)
        if (a->Hero() && !a->name.empty()) {
            hero_name = a->name;
            break;
        }
    if (city) {
        if (fled) out.push_back(kit::Text(kAsFled, G.g->rng.Dice(1, 4, -1)));
        if (!hero_name.empty())
            out.push_back(Format(kit::Text(kAsWonCityHero, 0), hero_name));
        else
            out.push_back(kit::Text(kAsWonCity, 0));
    } else if (!hero_name.empty()) {
        out.push_back(Format(kit::Text(kAsWonHero, 0), hero_name));
        out.push_back(kit::Text(kAsWonHero, 1));
    } else {
        out.push_back(kit::Text(kAsWon, 0));
    }
    if (result.loot > 0) out.push_back(Format(kit::Text(kAsLoot, 0), result.loot));
    return out;
}

// Begin showing an assault; the fight is already decided.
static void StartAssault(int x, int y, const w2::Battle& result) {
    const auto& lines = result.lines;
    bool fled = lines.defenders.empty();
    Assault a;
    a.x = x;
    a.y = y;
    a.result = result;
    a.def = lines.defenders;
    a.atk = lines.attackers;
    a.def_slots = LineSlots(static_cast<int>(lines.defenders.size()), kAsRows);
    a.atk_slots = LineSlots(static_cast<int>(lines.attackers.size()), {kAsAtkRow});
    a.def_side = lines.defenders.empty() ? 8 : OwnerOr8(lines.defenders[0]->owner);
    a.atk_side = lines.attackers.empty() ? 8 : OwnerOr8(lines.attackers[0]->owner);
    a.phase = "cloud";
    a.at = w2::Now();
    // on a human's turn 67cc:1836 holds the cloud while WAR.8SN plays
    a.cloud_time = (G.g->side && !G.g->side->computer) ? sound::Effect("war") : 0;
    a.victor = VictorName(lines.attackers);
    a.message = OutcomeLines(result, lines.city, fled);
    a.def_fell.resize(a.def.size());
    a.atk_fell.resize(a.atk.size());
    G.assault = a;
}
void front::StartAssault(int x, int y, const w2::Battle& result) {
    ::StartAssault(x, y, result);
}

// Close the battle window and let the fight take effect (67cc:0a6b).
void front::CloseAssault() {
    if (!G.assault) return;
    Assault a = std::move(*G.assault);
    G.assault.reset();
    game::ApplyAttack(*G.g, a.result);
    G.medal = a.result.medal;
    front::StratDirty();
    if (a.after) a.after();
    // a computer's assault is the coroutine's: it carries on
}

// Carry the playback on by the clock.
static void AdvanceAssault() {
    Assault& a = *G.assault;
    double t = w2::Now();
    if (a.phase == "cloud") {
        if (t - a.at >= std::max(kAsCloudTime, a.cloud_time)) {
            // a computer's battle nobody is to see: the cloud was all of it
            if (a.computer && !a.window) {
                front::CloseAssault();
                return;
            }
            a.phase = "battle";
            a.at = t;
            // 67cc:124a waits five ticks on the drawn-up lines before the fight
            if (a.computer) a.at = t + 5 / 18.2;
        }
        return;
    }
    if (a.phase == "over" && a.computer) {
        if (a.close_at == 0) {
            a.close_at = t + 15 / 18.2;
            if (G.ai_status) G.ai_status->text = a.outcome;
        } else if (t >= a.close_at) {
            front::CloseAssault();
        }
        return;
    }
    if (a.phase != "battle") return;
    double each = a.fast ? kAsFellFast : kAsFellTime;
    const auto& log = a.result.log;
    while (a.step < log.size() && t >= a.at) {
        a.step++;
        Fell fell{a.at, a.fast ? kAsBlastFast : kAsBlastTime};
        if (log[a.step - 1] == 1) {
            if (a.atk_down < static_cast<int>(a.atk_fell.size())) a.atk_fell[a.atk_down] = fell;
            a.atk_down++;
        } else {
            if (a.def_down < static_cast<int>(a.def_fell.size())) a.def_fell[a.def_down] = fell;
            a.def_down++;
        }
        // 7dda:0181: a blow for each, until Space hurries the fight on
        if (!a.fast) sound::Effect(log[a.step - 1] == 1 ? "army" : "army2");
        a.at += each;
    }
    if (a.step >= log.size() && t >= a.at) a.phase = "over";
}

// A key or a click: run the playback through, and when it is through, close
// the window and ask what is to be done with the city.
static void PressAssault() {
    if (!G.assault) return;
    Assault& a = *G.assault;
    if (a.phase != "over") {
        a.fast = true;
        a.at = std::min(a.at, w2::Now() + kAsFellFast);
        AdvanceAssault();
        return;
    }
    City* city = a.result.captured;
    bool computer = a.computer;
    std::string victor = a.victor;
    // captured is set once applied: apply, then ask
    front::CloseAssault();
    Army* medal = G.medal;
    G.medal = nullptr;
    if (!computer) {
        // the fight took effect in closeAssault; see whether a city fell
        if (G.selection && !G.selection->stack.empty()) {
            City* c = game::CityAt(*G.g, G.selection->stack[0]->x, G.selection->stack[0]->y);
            if (c && c->owner_index == G.player->index) city = c;
        }
        // a medal is told before the city's fate is asked (67cc:0000)
        auto then = [city, victor]() {
            if (city) PresentVictory(city, victor);
        };
        if (medal && G.g->Alive(medal))
            honours::Medal(medal, then);
        else
            then();
    }
}

// The cloud over the tile, drawn with the map, in map pixels.
static void DrawCloud() {
    const Assault& a = *G.assault;
    int sx = a.x * kTile + (kTile - kAsCloudW) / 2;
    int sy = a.y * kTile + (kTile - kAsCloudH) / 2;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.war_pic, gfx::NewQuad(0, 0, kAsCloudW, kAsCloudH), sx, sy);
}

// One line of armies, struck off from the front as they fall.
static void DrawBattleLine(const std::vector<Army*>& armies,
                           const std::vector<std::pair<int, int>>& slots, int side, int down,
                           const std::vector<Fell>& fell) {
    if (side < 0 || side > 8) side = 8;
    double t = w2::Now();
    for (size_t i = 0; i < armies.size() && i < slots.size(); i++) {
        bool gone = static_cast<int>(i) < down && i < fell.size() && t >= fell[i].at + fell[i].burn;
        if (gone) continue;
        auto at = slots[i];
        gfx::SetColor(1, 1, 1);
        if (armies[i]->at_sea)
            gfx::Draw(G.war_pic, gfx::NewQuad(kAsSea[0], kAsSea[1], kAsSea[2], kAsSea[3]), at.first,
                      at.second + kAsSeaDrop);
        gfx::Draw(G.army_img[side], kit::ArmyQuad(armies[i]->type), at.first, at.second);
        if (static_cast<int>(i) < down)
            gfx::Draw(G.atrans_shields, gfx::NewQuad(32, 0, 32, 29), at.first, at.second);
    }
}

static void DrawBattle() {
    const Assault& a = *G.assault;
    const auto& popup_rect = kAsWindow;
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::Rectangle(gfx::kFill, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::Rectangle(gfx::kFill, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.marble, gfx::NewQuad(0, 0, popup_rect.w, popup_rect.h), popup_rect.x, popup_rect.y);
    auto shield = [](int side, const int* at) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.shield_img, gfx::NewQuad(side * kAsShieldW, 0, kAsShieldW, kAsShieldH), at[0],
                  at[1]);
    };
    shield(a.def_side, kAsShieldDef);
    shield(a.atk_side, kAsShieldAtk);
    DrawBattleLine(a.def, a.def_slots, a.def_side, a.def_down, a.def_fell);
    DrawBattleLine(a.atk, a.atk_slots, a.atk_side, a.atk_down, a.atk_fell);
    if (a.phase == "over") {
        int y = kAsTextY;
        gfx::SetColor(1, 1, 1);
        for (auto& line : a.message) {
            kit::Centred(*G.big_font, line, 320, y);  // 6a35:04c5, on x = 320
            y += kAsTextStep;
        }
    }
}

static void DrawAssault() {
    AdvanceAssault();
    if (!G.assault) return;
    if (G.assault->phase != "cloud") DrawBattle();
}

// Dialog 11 behind popup 7 -- exactly VICTORY.PCK -- with four buttons.
// Pillage needs a production type to strip and sack two (63fa:0000).
static void PresentVictory(City* city, const std::string& victor) {
    if (!G.victory_view) G.victory_view = screen::MakeView(*G.screen, kAsVDialog);
    Victory v;
    v.city = city;
    v.who = Format(kit::Text(kAsVWho, G.g->rng.Dice(1, 4, -1)), victor);
    v.where = Format(kit::Text(kAsVWhere, G.g->rng.Dice(1, 3, -1)), city->name);
    G.victory = v;
    auto& st = G.victory_view->state;
    st[kAsOccupy] = kNormal;
    st[kAsRaze] = kNormal;
    // Pillage and Sack by the gold they would bring (63fa:0834, 63fa:07d0)
    int pillage_gold =
            city->slots.empty() ? 0 : std::abs(G.g->types.ById(city->slots.back().type)->price) / 2;
    int sack_gold = w2::ai::groups::SackValue(*G.g, *city);
    st[kAsPillage] = pillage_gold != 0 ? kNormal : kDisabled;
    st[kAsSack] = sack_gold != 0 ? kNormal : kDisabled;
    // Enter is Occupy -- unless the stack's hero is on a quest this answers:
    // Raze for a raze quest on this city, Sack (or Pillage) for gold (63fa:0000)
    G.victory->enter = kAsOccupy;
    auto q = G.player->quest;
    Army* hero = nullptr;
    if (G.selection) {
        for (Army* a : G.selection->stack)
            if (a->type == w2::armytype::kHero) {
                hero = a;
                break;
            }
    }
    if (G.g->map->options.quests != 0 && q && hero && hero == q->hero) {
        if (q->type == w2::quest::kRaze && q->city == city)
            G.victory->enter = kAsRaze;
        else if (q->type == w2::quest::kPillageGold) {
            if (sack_gold != 0)
                G.victory->enter = kAsSack;
            else if (pillage_gold != 0)
                G.victory->enter = kAsPillage;
        }
    }
}
void front::PresentVictory(City* city, const std::string& victor) {
    ::PresentVictory(city, victor);
}

static void DrawVictory(const Victory& v) {
    const auto& popup_rect = kAsVPopup;
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::Rectangle(gfx::kFill, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::Rectangle(gfx::kFill, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.victory_pic, popup_rect.x, popup_rect.y);
    kit::Centred(*G.title_font, kit::Text(kAsVText, 0), 320, kAsVTitleY);
    kit::Centred(*G.big_font, v.who, 320, kAsVLineY[0]);
    kit::Centred(*G.big_font, v.where, 320, kAsVLineY[1]);
    kit::Centred(*G.big_font, kit::Text(kAsVText, 1), 320, kAsVLineY[2]);
    kit::Centred(*G.big_font, kit::Text(kAsVText, 2), 320, kAsVLineY[3]);
    screen::DrawDialogControls(*G.screen, *G.victory_view);
}

// What the player chose to do with the city they have just taken (63fa).
static void TakeCity(int what) {
    if (!G.victory) return;
    Victory v = *G.victory;
    City* c = v.city;
    std::vector<Army*> stack = G.selection ? G.selection->stack : std::vector<Army*>{};
    G.victory.reset();
    auto done = []() {
        G.victory_under.reset();
        front::StratDirty();
        RefreshControls();
    };
    if (what == kAsPillage || what == kAsSack) {
        bool sacked = what == kAsSack;
        auto spoils = sacked ? game::Sack(*G.g, *G.player, *c, stack)
                             : game::Pillage(*G.g, *G.player, *c, stack);
        G.victory_under = v;
        spoils::Open(sacked, c, spoils.gold, spoils.lost, static_cast<int>(c->slots.size()), done);
    } else if (what == kAsRaze) {
        G.victory_under = v;
        search::Say(c->name + " is in ruins!", [c, stack, done]() {  // 4125:0a56
            game::Raze(*G.g, *G.player, *c, stack);
            done();
        });
    } else {
        // quest_check(4): a quest done is its reward instead of the city
        auto news = game::Occupy(*G.g, *G.player, *c, stack);
        auto production = [c]() { city::Open(c, city::kProduction); };
        if (news) {
            G.player->quest_news.reset();
            if (!news->failed.empty())
                questnews::Show(news, production);
            else
                questnews::Show(news);
        } else {
            production();
        }
    }
}

// Draw in the popups' 640x480 frame, centred on the screen.
static void InDialogFrame(const std::function<void()>& draw) {
    gfx::Push();
    gfx::Translate(G.layout.dialog.x, G.layout.dialog.y);
    draw();
    gfx::Pop();
}

// Draw one of the fixed groups of the original screen where it now is.
static void InGroup(const layout::Offset& o, const std::function<void()>& draw) {
    gfx::Push();
    gfx::Translate(o.x, o.y);
    draw();
    gfx::Pop();
}

static void DrawModals() {
    // a dialog may push another while drawing is under way: a copy is drawn
    auto modals = G.modals;
    for (auto& d : modals) d->Draw();
}

// The samples queue and the advisor blinks by the clock.
static void Update() {
    sound::Update();
    if (auto top = kit::TopShared()) top->Update();
}

// One frame: the chrome in UI pixels, the map in map pixels at its own
// zoom, then the dialogs and the pointer over both.
static void Draw() {
    gfx::Begin();
    gfx::Clear(0, 0, 0);
    SyncLayout();
    StepComputer();
    display::PushUi();
    if (G.starting || !G.g) {
        DrawModals();
        DrawMenuBar();
        DrawPointer();
        gfx::Pop();
        return;
    }
    AdvanceWalk();
    // a quest that ended is told once the screen is the player's again
    if (G.player && G.player->quest_news &&
        !(G.over || G.banner || G.offer || G.assault || G.victory || G.victory_under || G.walk ||
          G.move_all || G.ai_run || G.open_menu >= 0 || kit::Top())) {
        questnews::Poll();
    }
    RefreshControls();
    screen::DrawBackground(*G.screen, G.layout);
    const auto& r = G.map_rect;
    if (G.map_after_banner) {
        // a hotseat turn not yet taken up: both maps blank (hotseatHidden)
        const auto& s = G.strat_rect;
        gfx::SetColor(0, 0, 0);
        gfx::Rectangle(gfx::kFill, r.x, r.y, r.w, r.h);
        gfx::Rectangle(gfx::kFill, s.x, s.y, s.w, s.h);
        gfx::SetColor(1, 1, 1);
    } else {
        gfx::SetScissor(r.x, r.y, r.w, r.h);
        auto [cx, cy] = CamDev();
        display::PushMap(r.x, r.y, cx, cy, G.zoom);
        DrawMap();
        if (G.assault && G.assault->phase == "cloud") DrawCloud();
        gfx::Pop();
        gfx::SetScissor();
        DrawStrategic();
    }
    // 8065:0a9d refills the control panel with marble before the controls go on
    InGroup(G.layout.panel, []() {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 224, 114), 400, 355);
    });
    screen::DrawControls(*G.screen);
    DrawShortcutIcons();
    InGroup(G.layout.bar, DrawBottomBar);
    InDialogFrame([]() {
        if (G.victory_under) DrawVictory(*G.victory_under);
        DrawModals();
    });
    DrawMenuBar();
    // the turn opens with the banner over the offer, and is dismissed first
    InDialogFrame([]() {
        if (G.offer) DrawHeroOffer();
        if (G.assault) DrawAssault();
        if (G.victory) DrawVictory(*G.victory);
        if (G.banner) DrawBanner();
    });
    DrawPointer();
    gfx::Pop();
}

// ------------------------------------------------------------------ input

// 1a8b:04c8 reads no input while a stack walks, and Move All walks one stack
// after another. A click meanwhile is lost; a key waits in the keyboard's
// buffer, fifteen of them, and is read once it is all over.
static bool BusyWalking() {
    return ((G.walk && !G.walk->computer) || G.move_all) && !kit::Top();
}

static void FlushKeys() {
    auto ks = G.key_buffer;
    G.key_buffer.clear();
    for (auto& k : ks) Keypressed(k);
}

// While the computer plays: a key hurries a battle, or runs the rest of the
// round through unwatched. True when it took the input.
static bool ComputerInput(const std::string& k) {
    if (!(G.ai_run && !kit::Top() && G.open_menu < 0)) return false;
    if (G.assault && G.assault->computer) {
        PressAssault();
        return true;
    }
    // Allow opening the F10 AI Brain Inspector (under --modern-ui), panning, or adjusting zoom
    // while spectating AI turns
    if (G.modern_ui && k == "f10") {
        diplomacyui::OpenBrainInspector(G.ai_side ? G.ai_side->index : kNone);
        return true;
    }
    if (k == "kp+" || k == "pageup" || k == "=") {
        front::SetZoom(G.zoom + 1);
        return true;
    }
    if (k == "kp-" || k == "pagedown" || k == "-") {
        front::SetZoom(G.zoom - 1);
        return true;
    }
    if (k == "up") {
        G.cy -= 1;
        ClampCamera();
        return true;
    }
    if (k == "down") {
        G.cy += 1;
        ClampCamera();
        return true;
    }
    if (k == "left") {
        G.cx -= 1;
        ClampCamera();
        return true;
    }
    if (k == "right") {
        G.cx += 1;
        ClampCamera();
        return true;
    }
    // Shift and Alt are held to reach Settings, not pressed to skip
    if (w2::EndsWith(k, "shift") || w2::EndsWith(k, "alt") || w2::EndsWith(k, "ctrl") ||
        w2::EndsWith(k, "gui")) {
        return true;
    }
    if (!HumansLeft()) {
        // In AI-vs-AI spectator mode, let Space toggle fast-forward and allow read-only spectator
        // dialogs/quit
        if (k == "space") {
            G.ai_skip = !G.ai_skip;
            if (G.ai_skip && G.walk && G.walk->computer) {
                G.walk.reset();
                ResumeComputer();
            }
            return true;
        }
        if (k == "escape") {
            settings::Open();
            return true;
        }
        if (Held({"lctrl", "rctrl", "lgui", "rgui"}) && k == "q") return false;
        if (k == "c" || k == "." || k == "a" || k == "k" || k == "g" || k == "n" || k == "w" ||
            k == "d" || k == "h" || k == "e" || k == "j" || k == "y" || k == "l" || k == "?") {
            return false;
        }
        return true;
    }
    G.ai_skip = true;
    if (G.walk && G.walk->computer) {
        G.walk.reset();
        ResumeComputer();
    }
    return true;
}

// A control's id, minus 100, indexes the 397-entry jump table at 17be:0b0c:
// that is what turns a click into an action (docs/re/ui.md > Commands).
static std::map<int, std::function<void()>>& Actions();

static void Mousepressed(int x, int y, int button) {
    if (BusyWalking()) return;
    SyncLayout();
    int fx = x - G.layout.dialog.x, fy = y - G.layout.dialog.y;
    if (G.ai_run && !kit::Top()) {
        if (G.assault && G.assault->computer) {
            PressAssault();
            return;
        }
        if (!HumansLeft()) {
            // AI-vs-AI spectator mouse controls: menu bar, right-click tile/stack inspect, minimap
            // jump, and map drag
            int hit = menu::TitleAt(G.menu_layout, x, y);
            if (hit >= 0) {
                G.open_menu = G.open_menu == hit ? -1 : hit;
                return;
            }
            if (G.open_menu >= 0) {
                const menu::Row* row = menu::RowAt(G.menu_layout, G.open_menu, x, y);
                G.open_menu = -1;
                std::string rk = row ? (row->key.empty() ? row->act : row->key) : "";
                if (!rk.empty() && MenuEnabled(rk)) MenuPick(rk);
                return;
            }
            if (G.g && G.screen) {
                if (auto r = screen::RegionAt(*G.screen, x, y)) {
                    if (r->id == screen::kMap) {
                        if (button == 2) {
                            if (auto tile = TileAtPoint(x, y))
                                tileinfo::Open(tile->first, tile->second);
                            return;
                        }
                        if (button == 1) {
                            G.drag = Drag{G.cx, G.cy, 0, 0};
                            return;
                        }
                    } else if (r->id == screen::kStrategic && button == 1) {
                        int tx = (x - r->x) / 2, ty = (y - r->y + strat_scroll) / 2;
                        CentreOn(tx, ty);
                        return;
                    }
                }
            }
            return;
        }
        if (ComputerInput("")) return;
    }
    // 7ecb:0142: the banner eats the click that dismisses it
    if (G.banner) {
        DismissBanner();
        return;
    }
    if (G.assault) {
        PressAssault();
        return;
    }
    // the right button is not a click on a dialog's button (18a9:012c)
    if ((G.victory || G.offer) && button == 2) return;
    if (G.victory) {
        auto c = screen::DialogControlAt(*G.victory_view, fx, fy);
        if (c && G.victory_view->StateOf(c->id) != kDisabled) TakeCity(c->id);
        return;
    }
    if (G.offer) {
        auto c = screen::DialogControlAt(*G.hero_view, fx, fy);
        if (!c) return;
        if (c->id == kHeroField)
            offer_typing = true;
        else if (c->id == kHeroMale)
            G.offer_female = false;
        else if (c->id == kHeroFemale)
            G.offer_female = true;
        else if (c->id == kHeroOk)
            AcceptOffer();
        else if (c->id == kHeroCancel && !G.offer->first)
            RefuseOffer();
        return;
    }
    // on the start screens the menu bar stays live over them (7f77:0200)
    auto top = kit::TopShared();
    if (top && top->menu_bar && (G.open_menu >= 0 || menu::TitleAt(G.menu_layout, x, y) >= 0))
        top = nullptr;
    if (top) {
        // the right button on a dialog: a control's help, or the dialog's regions
        if (button == 2 && top->view) {
            auto c = infobox::ControlAt(*top->view, fx, fy, &top->hidden);
            if (!(c && infobox::ExplainControl(x, y, c->id, top.get())))
                top->Rightpressed(fx, fy, x, y);
            return;
        }
        top->Mousepressed(fx, fy, button);
        return;
    }
    if (G.over) return;
    // the menu bar takes precedence over everything beneath it, the start
    // screens' too, with no game yet
    int hit = menu::TitleAt(G.menu_layout, x, y);
    if (hit >= 0) {
        G.open_menu = G.open_menu == hit ? -1 : hit;
        return;
    }
    if (G.open_menu >= 0) {
        const menu::Row* row = menu::RowAt(G.menu_layout, G.open_menu, x, y);
        G.open_menu = -1;
        std::string k = row ? (row->key.empty() ? row->act : row->key) : "";
        if (!k.empty() && MenuEnabled(k)) MenuPick(k);
        return;
    }
    if (!G.g) return;
    auto c = screen::ControlAt(*G.screen, x, y);
    if (c && button == 2) {
        if (infobox::ExplainControl(x, y, c->id)) return;
    } else if (c) {
        if (G.screen->StateOf(c->id) == kDisabled) return;
        G.pressed = c->id;
        G.screen->state[c->id] = kActive;
        return;
    }
    // the hand drags the map (740d:00e4 -> 8065:0e04)
    if (button == 1 && PointerKind(x, y) == kHand) {
        G.drag = Drag{G.cx, G.cy, 0, 0};
        return;
    }
    auto r = screen::RegionAt(*G.screen, x, y);
    if (!r) return;
    if (r->id == screen::kMap) {
        auto tile = TileAtPoint(x, y);
        if (!tile) return;
        auto [tx, ty] = *tile;
        if (button == 2) {
            tileinfo::Open(tx, ty);
            return;
        }
        // the left button does what the pointer shows (740d:00ce)
        int k = PointerKind(x, y);
        if (k == kWalk || k == kBoat)
            MoveSelection(tx, ty);
        else if (k == kAttack || k == kPeace)
            AttackTile(tx, ty);
        else if (k == kCityP || k == kSiteP) {
            if (City* city = game::CityAt(*G.g, tx, ty)) front::OpenCity(city);
        } else if (k == kAdvise) {
            miladvisor::Open(G.selection ? &G.selection->stack : nullptr, tx, ty);
        } else if (k == kSelect) {
            SelectClick(tx, ty);
        } else if (k == kAlt) {
            PlanRoute(tx, ty);
            RefreshControls();
        }
    } else if (r->id == screen::kStrategic) {
        int tx = (x - r->x) / 2, ty = (y - r->y + strat_scroll) / 2;
        if (button != 1) return;
        if (Held({"lalt", "ralt"})) {
            PlanRoute(tx, ty);
            RefreshControls();
        } else {
            CentreOn(tx, ty);
        }
    } else if (r->id == screen::kBottombar && button == 2) {
        BarInfo(x, y);
    } else if (r->id == screen::kMappanel && button == 2) {
        infobox::ShowLines(x, y, "- Drag Screen -", "Move mouse to drag the screen");
    }
}

void front::AfterSlotChange() {
    SyncSelection();
}

// Order > Move All (1c8c:04c4): every army of the side with a destination
// walks there in turn, each walk played out before the next.
static void MoveAllStep() {
    while (G.move_all) {
        if (G.walk) return;
        Army* lead = nullptr;
        for (Army* a : G.g->armies) {
            if (a->owner == G.player->index && a->target && !a->transit &&
                !G.move_all->seen.count(a)) {
                lead = a;
                break;
            }
        }
        if (!lead) {
            if (G.move_all->moved == 0) Say("Nothing is under orders.");
            G.move_all.reset();
            RefreshControls();
            return;
        }
        G.move_all->seen.insert(lead);
        Select(lead->x, lead->y, lead);
        if (G.selection) {
            for (Army* a : G.selection->stack) G.move_all->seen.insert(a);
            // selecting a stack already where it was sent lifts the order
            // (refreshRoute), so there may be nothing left to walk to
            if (lead->target) {
                auto target = *lead->target;
                MoveSelection(target.first, target.second);
                if (G.move_all) G.move_all->moved++;
            }
        }
    }
}

static void MoveAll() {
    G.move_all = App::MoveAll{};
    if (G.selection && !G.selection->stack.empty() && G.selection->stack[0]->target) {
        auto t = *G.selection->stack[0]->target;
        for (Army* a : G.selection->stack) G.move_all->seen.insert(a);
        MoveSelection(t.first, t.second);
        if (G.move_all) G.move_all->moved = 1;
    }
    MoveAllStep();
}

// Hero > Search (6536:0000) with the selected stack.
static void DoSearch() {
    if (!G.selection) return;
    int x = G.selection->x, y = G.selection->y;
    search::Open(G.selection->stack);
    Reslot(SelectableAt(x, y));
    front::StratDirty();
}

// The key of each menu item UDB.DAT lets a button carry (545c:00aa).
static std::string ShortcutKey(int n) {
    static const std::map<int, std::string> kKey = {
        {507, "m"}, {508, "q"}, {510, "a"}, {511, "k"}, {512, "g"}, {513, "n"}, {514, "w"},
        {516, "="}, {517, ","}, {518, "f"}, {521, "z"}, {524, "b"}, {525, "c"}, {526, "p"},
        {527, "v"}, {528, "."}, {529, "s"}, {530, "h"}, {531, "e"}, {534, "l"}, {535, "alt E"}};
    auto sc = G.screen->ui.shortcuts.find(n);
    if (sc == G.screen->ui.shortcuts.end()) return "";
    auto k = kKey.find(sc->second);
    return k == kKey.end() ? "" : k->second;
}

static std::map<int, std::function<void()>>& Actions() {
    static std::map<int, std::function<void()>> action_table;
    if (!action_table.empty()) return action_table;
    for (int i = 0; i < kSlotCount; i++) {
        action_table[kSlotFirst + i] = [i]() {
            if (!G.selection) return;
            slots_mod::Toggle(G.selection->slots, *G.g, i);
            front::AfterSlotChange();
        };
        action_table[kBarFirst + i] = [i]() {
            if (!G.selection) return;
            slots_mod::PickGroup(G.selection->slots, *G.g, i);
            front::AfterSlotChange();
        };
    }
    action_table[kGrpAll] = []() {
        if (!G.selection) return;
        auto& s = G.selection->slots;
        if (slots_mod::Grouped(s))
            slots_mod::Single(s, *G.g);
        else
            slots_mod::All(s, *G.g);
        front::AfterSlotChange();
    };
    action_table[kGrpNone] = action_table[kGrpAll];
    // The five buttons above the pad, 173-178: walk on, next, done, defend,
    // deselect (8065:0ec9 / 0ed7 / 0ef4 / 0f26).
    action_table[173] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto t = G.selection->stack[0]->target;
        if (!t) return;
        MoveSelection(t->first, t->second);
    };
    action_table[174] = SelectNext;
    action_table[175] = QuitArmy;
    action_table[176] = Fortify;
    action_table[178] = Deselect;
    // The 3x3 pad, 320-327 (8611:0723), clockwise from north: it moves the view.
    static const int kPad[8][2] = {{0, -1}, {1, -1}, {1, 0},  {1, 1},
                                   {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
    for (int i = 0; i < 8; i++) {
        action_table[320 + i] = [i]() {
            G.cx += kPad[i][0];
            G.cy += kPad[i][1];
            ClampCamera();
        };
    }
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        action_table[w2::uidata::kShortcutFirst + i] = [i]() {
            std::string k = ShortcutKey(i);
            if (!k.empty() && MenuEnabled(k)) MenuDoes()[k]();
        };
    }
    // the "?" (8065:104e): the mouse's help page, then the keys'
    action_table[188] = []() {
        help::Open(
                "HELP\\HMOUSE.GFX",
                []() { help::Open("HELP\\HKEYS.GFX", nullptr, &help::kPopuP4); }, &help::kPopuP4);
    };
    // 186 (8065:0f3f) looks at where the stack is going, and back
    action_table[186] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto& sel = *G.selection;
        auto t = sel.stack[0]->target;
        double cx = G.cx, cy = G.cy;
        if (t && InView(sel.x, sel.y)) {
            CentreOn(t->first, t->second);
            if (G.cx == cx && G.cy == cy) CentreOn(sel.x, sel.y);
        } else {
            CentreOn(sel.x, sel.y);
        }
    };
    // 187 (8065:0fe9) forgets the destination
    action_table[187] = []() {
        if (!G.selection || G.selection->stack.empty() || !G.selection->stack[0]->target) return;
        for (Army* a : G.selection->stack) a->target.reset();
        G.route.reset();
    };
    // 183-185 are one button in three faces: Diplomatic Action (484e:0346)
    for (int id = 183; id <= 185; id++) action_table[id] = diplomacyui::Action;
    // the pad's centre, 177, shares its handler with Home (8065:0f02)
    action_table[177] = []() {
        if (G.selection) CentreOn(G.selection->x, G.selection->y);
    };
    return action_table;
}

// Which buttons are live (8065:0174, the original's own refresh). A control
// with no handler here is greyed too.
static void RefreshControls() {
    auto& st = G.screen->state;
    // under the banner and the hero offer every control is still greyed
    if (G.banner || G.offer) {
        for (auto& [id, s] : st) s = kDisabled;
        return;
    }
    auto& action_table = Actions();
    auto set = [&](int id, bool live) {
        if (!st.count(id)) return;
        if (!(live && action_table.count(id)))
            st[id] = kDisabled;
        else if (id == G.pressed)
            st[id] = kActive;
        else
            st[id] = kNormal;
    };
    const Selection* sel = G.selection ? &*G.selection : nullptr;
    bool walk_on =
            sel && G.route &&
            static_cast<int>(G.route->path.size()) > (G.walk ? static_cast<int>(G.walk->i) + 1 : 0);
    set(173, walk_on);
    bool more = false;
    for (Army* a : G.g->armies)
        if (a->owner == G.player->index && !a->transit && !a->fortified && !a->done) {
            more = true;
            break;
        }
    set(174, more);
    set(175, more && sel);
    set(176, sel);
    set(177, sel);
    set(178, sel);
    set(186, sel);
    set(187, sel && !sel->stack.empty() && sel->stack[0]->target);
    set(188, true);
    for (int i = 0; i < kSlotCount; i++) {
        bool live = sel && i < sel->slots.n;
        set(kSlotFirst + i, live);
        set(kBarFirst + i, live);
    }
    bool grouped = sel && slots_mod::Grouped(sel->slots);
    set(240, sel && !grouped);
    set(241, sel && grouped);
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        std::string k = ShortcutKey(i);
        set(w2::uidata::kShortcutFirst + i, !k.empty() && MenuEnabled(k));
    }
    for (int i = 0; i < 8; i++) set(320 + i, true);
    int face = diplomacyui::ButtonFor(*G.g, G.player->index);
    for (int id = 183; id <= 185; id++) set(id, id == face);
}

// The drag: the map goes the way the mouse does, by exactly as far.
static void Mousemoved(double dx, double dy) {
    if (!G.drag) return;
    G.drag->dx += dx;
    G.drag->dy += dy;
    double k = static_cast<double>(display::d.scale) / (kTile * G.zoom);
    G.cx = G.drag->cx - G.drag->dx * k;
    G.cy = G.drag->cy - G.drag->dy * k;
    ClampCamera();
}

// The wheel zooms the map about the tile under the pointer.
static void Wheelmoved(int wy) {
    SyncLayout();
    if (G.starting || !G.g || G.over || kit::Top() || G.banner || G.offer || G.victory || wy == 0)
        return;
    if (G.map_rect.Contains(G.mouse_x, G.mouse_y))
        SetZoomAt(G.zoom + (wy > 0 ? 1 : -1), G.mouse_x, G.mouse_y, false);
    else
        SetZoomAt(G.zoom + (wy > 0 ? 1 : -1), 0, 0, true);
}

static void Mousereleased(int x, int y, int button) {
    SyncLayout();
    if (G.drag) {
        G.drag.reset();
        if (G.pressed == kNone) return;
    }
    if (auto top = kit::TopShared()) {
        if (top->Mousereleased(x - G.layout.dialog.x, y - G.layout.dialog.y, button)) return;
    }
    int id = G.pressed;
    if (id == kNone) return;
    G.pressed = kNone;
    G.screen->state[id] = kNormal;
    auto c = screen::ControlAt(*G.screen, x, y);
    if (!c || c->id != id) return;  // released off the button
    auto& action_table = Actions();
    auto act = action_table.find(id);
    if (act != action_table.end()) act->second();
    if (G.g) RefreshControls();
}

// The city nearest the view's centre, then the city dialog in that mode.
static void ViewCity(int mode) {
    auto [cx, cy] = front::ViewCentre();
    int viewer = game::ViewerSide(*G.g, G.player);
    City* best = nullptr;
    int best_d = kNone;
    for (auto& c : G.g->map->cities) {
        bool ok = game::Seen(*G.g, viewer, c.x, c.y) &&
                  (mode == city::kInfo || c.owner_index == G.player->index);
        if (ok) {
            int d = std::max(std::abs(c.x - cx), std::abs(c.y - cy));
            if (best_d == kNone || d < best_d) {
                best = &c;
                best_d = d;
            }
        }
    }
    if (best) city::Open(best, mode);
}

void front::Quit() {
    G.modals.clear();
    G.ai_run.reset();
    ai_co = nullptr;
    G.ai_status.reset();
    G.assault.reset();
    G.walk.reset();
    G.move_all.reset();
    G.banner.reset();
    G.offer.reset();
    G.victory.reset();
    G.victory_under.reset();
    G.over = false;
    front::OpenStart();
}

// Each menu accelerator, as far as this engine can honour it.
static std::map<std::string, std::function<void()>>& MenuDoes() {
    static std::map<std::string, std::function<void()>> menu_actions;
    if (!menu_actions.empty()) return menu_actions;
    menu_actions["alt E"] = EndTurn;
    menu_actions["m"] = MoveAll;
    // Game > Quit (7721:0000) and New game (7721:019d) ask first
    menu_actions["^Q"] = []() {
        input::Options o;
        o.title = kit::Text(0x2c, 0);
        for (int i = 1; i <= 4; i++) o.lines.push_back(kit::Text(0x2c, i));
        o.confirm = true;
        o.ok = [](const std::string&) {
            // 7721:0072: "Farewell, Warlord!" -- and the game closes
            advisor::Say(w2::cues::kQuit, []() { G.quit_requested = true; });
        };
        input::Open(o);
    };
    menu_actions["alt N"] = []() {
        input::Options o;
        o.title = kit::Text(0x2d, 0);
        for (int i = 1; i <= 4; i++) o.lines.push_back(kit::Text(0x2d, i));
        o.confirm = true;
        o.ok = [](const std::string&) {
            G.selection.reset();
            G.banner.reset();
            G.offer.reset();
            front::OpenStart();
        };
        input::Open(o);
    };
    menu_actions["alt S"] = savegame::Save;
    menu_actions["alt L"] = []() {
        savegame::Load([](std::unique_ptr<w2::Game> g) {
            if (G.starting) {
                G.modals.clear();
                G.starting = false;
            }
            front::TakeLoaded(std::move(g));
        });
    };
    menu_actions["z"] = DoSearch;
    menu_actions["c"] = []() { ViewCity(city::kInfo); };
    menu_actions["b"] = []() { ViewCity(city::kCity); };
    menu_actions["p"] = []() { ViewCity(city::kProduction); };
    menu_actions["v"] = []() { ViewCity(city::kVector); };
    // Hero > Plant Flag (7563:09f7)
    menu_actions["f"] = []() {
        if (!G.selection) return;
        for (Army* a : G.selection->stack) {
            if (a->Hero() && game::PlantFlag(*G.g, *G.player, a)) {
                front::StratDirty();
                return;
            }
        }
    };
    // Order > Disband (1b62:06bf)
    menu_actions["q"] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto stack = G.selection->stack;
        bool heroes = false;
        for (Army* a : stack)
            if (a->Hero()) heroes = true;
        input::Options o;
        o.title = "Disband";  // 4125:2bf8
        o.confirm = true;
        o.lines = {"Are you sure you", "want to disband this", "group?",
                   heroes ? "It contains heroes" : ""};
        o.ok = [stack](const std::string&) {
            game::Disband(*G.g, *G.player, stack);
            G.selection.reset();
            front::StratDirty();
        };
        input::Open(o);
    };
    menu_actions["i"] = fightorder::Open;
    menu_actions["r"] = resign::Open;
    menu_actions["."] = []() {
        auto [cx, cy] = front::ViewCentre();
        ruin::Open(ruin::Nearest(cx, cy));
    };
    menu_actions["s"] = stackui::Open;
    menu_actions["o"] = armybonus::Open;
    menu_actions["t"] = items::Open;
    menu_actions["x"] = []() {
        if (G.selection) signpost::Open(G.selection->stack);
    };
    menu_actions["h"] = []() { historyui::Open(0); };
    menu_actions["e"] = []() { historyui::Open(1); };
    menu_actions["j"] = []() { historyui::Open(2); };
    menu_actions["y"] = []() { historyui::Open(3); };
    menu_actions["alt U"] = shortcuts::Open;
    menu_actions["alt X"] = []() { settings::Open(); };
    menu_actions["?"] = about::Open;
    menu_actions["l"] = historyui::Triumphs;
    menu_actions["d"] = diplomacyui::Open;
    menu_actions["="] = questui::Open;
    menu_actions["u"] = levels::Open;
    menu_actions[","] = heroinfo::Open;
    menu_actions["a"] = []() { reports::Open(0); };
    menu_actions["k"] = []() { reports::Open(1); };
    menu_actions["g"] = []() { reports::Open(2); };
    menu_actions["n"] = []() { reports::Open(3); };
    menu_actions["w"] = []() { reports::Open(4); };
    for (int n = 1; n <= 16; n++) {
        menu_actions["map zoom " + std::to_string(n)] = [n]() { front::SetZoom(n); };
        menu_actions["ui scale " + std::to_string(n)] = [n]() { SetUiScale(n); };
    }
    menu_actions["ui scale smooth"] = []() { SetUiScale(display::kSmooth); };
    menu_actions["screen full"] = []() { SetScreen(false); };
    menu_actions["screen window"] = []() { SetScreen(true); };
    menu_actions["screen 4:3"] = []() { SetOriginalSize(true); };
    menu_actions["screen fit"] = []() { SetOriginalSize(false); };
    menu_actions["screen crt"] = ToggleCrt;
    for (const char* s : {"fm", "mt32", "sc55"}) {
        std::string synth = s;
        menu_actions["music " + synth] = [synth]() { sound::SetSynth(synth); };
    }
    menu_actions["ai modern"] = []() { SetAiMode(false); };
    menu_actions["ai classic"] = []() { SetAiMode(true); };
    return menu_actions;
}

static void MenuPick(const std::string& k) {
    auto& menu_actions = MenuDoes();
    auto it = menu_actions.find(k);
    if (it != menu_actions.end()) it->second();
}

// What greys a menu item (8065:0519, the table at 4125:1798).
static bool MenuEnabled(const std::string& k) {
    auto& menu_actions = MenuDoes();
    if (k.empty() || !menu_actions.count(k)) return false;
    bool music = w2::StartsWith(k, "music ");
    bool ai_opt = w2::StartsWith(k, "ai ");
    if (music && !sound::SynthAvailable(k.substr(6))) return false;
    // 7f77:0200: on the start screens every menu is greyed but Quit and Load
    if (G.starting || !G.g) {
        return k == "^Q" || (k == "alt L" && savegame::Used() > 0) ||
               w2::StartsWith(k, "ui scale ") || w2::StartsWith(k, "screen ") || music || ai_opt;
    }
    // While AI-vs-AI spectating is running, only enable read-only spectator reports, display/audio
    // settings, and quit
    if (G.ai_run && !HumansLeft()) {
        if (k == "h" || k == "e" || k == "j" || k == "y") return G.g->turn != 1;
        if (k == "d") return G.g->map->options.diplomacy != 0;
        return k == "^Q" || k == "alt X" || k == "?" || k == "c" || k == "." || k == "a" ||
               k == "k" || k == "g" || k == "n" || k == "w" || k == "l" ||
               w2::StartsWith(k, "map zoom ") || w2::StartsWith(k, "ui scale ") ||
               w2::StartsWith(k, "screen ") || music || ai_opt;
    }
    w2::Game& g = *G.g;
    auto side_has_hero = [&]() {
        for (Army* a : g.armies)
            if (a->owner == G.player->index && a->Hero()) return true;
        return false;
    };
    auto selected = []() { return G.selection && !G.selection->stack.empty(); };
    auto side_has_cities = [&]() {
        for (auto& c : g.map->cities)
            if (c.owner_index == G.player->index && !c.razed) return true;
        return false;
    };
    auto can_search = [&]() {
        if (!selected()) return false;
        Army* lead = G.selection->stack[0];
        w2::Site* s =
                g.map->site_at.empty() ? nullptr : g.map->site_at[lead->y * g.map->width + lead->x];
        if (!s || s->searched) return false;
        return lead->Hero() || s->type == w2::site::kTemple;
    };
    auto can_plant_flag = [&]() {
        if (!selected()) return false;
        Army* lead = G.selection->stack[0];
        if (!lead->Hero()) return false;
        if (G.player->index >= static_cast<int>(g.map->items.size())) return false;
        w2::Item* std = &g.map->items[G.player->index];
        if (std::find(lead->items.begin(), lead->items.end(), std) == lead->items.end())
            return false;
        int t = w2::scn::TerrainAt(*g.map, lead->x, lead->y);
        if (t == move::kWater || t == move::kShore || t == move::kCity || t == move::kSite)
            return false;
        for (auto& it : g.map->items)
            if (it.planted && it.x == lead->x && it.y == lead->y) return false;
        return true;
    };
    if (k == "alt L") return savegame::Used() > 0;
    if (k == "q" || k == "s") return selected();
    if (k == "x")
        return selected() && w2::scn::TerrainAt(*g.map, G.selection->stack[0]->x,
                                                G.selection->stack[0]->y) == move::kTower;
    if (k == "r" || k == "b" || k == "p" || k == "v") return side_has_cities();
    if (k == "d") return g.map->options.diplomacy != 0;
    if (k == "," || k == "u") return side_has_hero();
    if (k == "f") return can_plant_flag();
    if (k == "z") return can_search();
    if (k == "h" || k == "e" || k == "j" || k == "y") return g.turn != 1;
    if (k == "alt E") return !g.won;
    return true;
}
bool front::MenuEnabled(const std::string& k) {
    return ::MenuEnabled(k);
}

static void Keypressed(const std::string& k) {
    if (BusyWalking()) {
        if (G.key_buffer.size() < 15) G.key_buffer.push_back(k);
        return;
    }
    SyncLayout();
    if (ComputerInput(k)) return;
    if (G.banner) {
        DismissBanner();
        return;
    }
    if (G.assault) {
        PressAssault();
        return;
    }
    if (G.victory) {
        // Occupy heads both the default and the cancel lists
        if (k == "return" || k == "kpenter" || k == "escape") TakeCity(G.victory->enter);
        return;
    }
    if (G.offer) {
        if (k == "return" || k == "kpenter")
            AcceptOffer();
        else if (k == "backspace" && !G.offer_name.empty())
            G.offer_name.pop_back();
        else if (k == "escape" && !G.offer->first)
            RefuseOffer();
        return;
    }
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl", "lgui", "rgui"}),
         shift = Held({"lshift", "rshift"});
    auto top = kit::TopShared();
    if (top) {
        if (top->menu_bar) {
            std::string accel;
            if (ctrl && k == "q")
                accel = "^Q";
            else if (alt && k.size() == 1)
                accel = "alt " + w2::Upper(k);
            if (!accel.empty()) {
                G.open_menu = -1;
                if (MenuEnabled(accel)) MenuDoes()[accel]();
                return;
            }
        }
        if (G.open_menu >= 0) {
            G.open_menu = -1;
            return;
        }
        top->Keypressed(k);
        return;
    }
    if (G.open_menu >= 0) {
        G.open_menu = -1;
        return;
    }
    if (G.over || !G.g) return;
    // the main screen's keys (17be:0064, 17be:0444)
    auto press = [](int id) {
        auto& action_table = Actions();
        if (G.screen->StateOf(id) != kDisabled && action_table.count(id)) action_table[id]();
        RefreshControls();
    };
    std::string digit;
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9')
        digit = k;
    else if (k.size() == 3 && w2::StartsWith(k, "kp") && k[2] >= '0' && k[2] <= '9')
        digit = k.substr(2);
    if (k == "return" || k == "kpenter")
        press(174);
    else if (k == "kp+" || k == "pageup" || k == "=")
        front::SetZoom(G.zoom + 1);
    else if (k == "kp-" || k == "pagedown" || k == "-")
        front::SetZoom(G.zoom - 1);
    else if (k == "escape")
        press(175);
    else if (k == "space")
        press(240);  // group the stack (89e0:0a55)
    else if (k == "tab")
        press(186);
    else if (k == "backspace")
        press(187);
    else if (k == "home")
        press(177);
    else if (k == "end")
        press(173);  // walk on along the route (17be:0318)
    else if (k == "delete")
        Deselect();  // 1b62:08b3, as 178 is (17be:02f1)
    else if (digit == "5") {
        if (G.selection) CentreOn(G.selection->x, G.selection->y);
    } else if (!digit.empty()) {
        // 1-9 step the stack the way the numeric pad lies (1c8c:0197)
        static const std::map<char, std::pair<int, int>> kStep = {
            {'8', {0, -1}}, {'9', {1, -1}}, {'6', {1, 0}},  {'3', {1, 1}},
            {'2', {0, 1}},  {'1', {-1, 1}}, {'4', {-1, 0}}, {'7', {-1, -1}}};
        auto st = kStep.find(digit[0]);
        if (st != kStep.end() && G.selection) {
            for (Army* a : G.selection->stack) a->target.reset();
            MoveSelection(G.selection->x + st->second.first, G.selection->y + st->second.second);
        }
    } else if (k == "up")
        press(320);
    else if (k == "right")
        press(322);
    else if (k == "down")
        press(324);
    else if (k == "left")
        press(326);
    else if (k == "f5") {
        // not the original's: quick save and load
        if (savegame::WriteSlot("quick", *G.g))
            Say("Saved.");
        else
            Say("Could not save.");
    } else if (k == "f9") {
        if (auto loaded = savegame::ReadSlot("quick")) front::TakeLoaded(std::move(loaded));
    } else if (G.modern_ui && k == "f10") {
        diplomacyui::OpenBrainInspector();
    } else {
        std::string accel;
        if (ctrl && k == "q")
            accel = "^Q";
        else if (alt && k.size() == 1)
            accel = "alt " + w2::Upper(k);
        else if (shift && k == "/")
            accel = "?";
        else if (k.size() == 1)
            accel = k;
        // a key is its menu item, and does nothing while the item is greyed
        if (!accel.empty() && MenuEnabled(accel)) MenuDoes()[accel]();
    }
}

// Typing into the hero's name field, or a dialog's.
static void Textinput(const std::string& text) {
    auto top = kit::TopShared();
    if (top && !G.offer) {
        top->Textinput(text);
        return;
    }
    if (!G.offer) return;
    static const std::regex kOk("^[A-Za-z0-9\\s'\\-.]+$");
    if (G.offer_name.size() < kHeroNameMax && std::regex_match(text, kOk)) G.offer_name += text;
}

// ------------------------------------------------------------------ controller

// Not the original's: a game controller plays as the mouse and a few keys,
// so that the Steam Deck plays with Steam's gamepad layout in Gaming Mode
// (cpp/linux/README.md has the table). The right stick is the pointer, and
// the right trackpad with it, as that layout makes the trackpad the right
// stick; the left stick slides the map. The touchscreen and a trackpad set
// to the mouse go on working beside them.
static const double kPointerSpeed = 1.2;  // the frame's height a second, the stick all the way over
static const double kSlideSpeed = 600;    // UI pixels a second, likewise

static double last_x = 0, last_y = 0;           // the mouse, in window points, as last seen
static std::vector<std::pair<int, int>> warps;  // where the mouse was put, its motion not yet back
static double pad_x = 0, pad_y = 0;             // the pointer as the stick has it, in UI pixels
static int pad_left = 0, pad_right = 0;         // the controls holding each mouse button down
static int typing_now = 0;  // text input: 0 off, 1 on, 2 on with the Deck's keyboard up

// The pointer to a UI point, dragging the map if a drag is on, and the
// mouse along with it, so that a trackpad or a mouse goes on from there.
static void PointTo(double x, double y) {
    x = std::clamp(x, 0.0, display::d.w - 0.001);
    y = std::clamp(y, 0.0, display::d.h - 0.001);
    double dx = x - pad_x, dy = y - pad_y;
    pad_x = x;
    pad_y = y;
    display::d.mouse_inside = true;
    display::d.off.reset();
    Mousemoved(dx, dy);
    if (static_cast<int>(x) == G.mouse_x && static_cast<int>(y) == G.mouse_y) return;
    G.mouse_x = static_cast<int>(x);
    G.mouse_y = static_cast<int>(y);
    SDL_Window* win = display::Window();
    if (win && SDL_GetKeyboardFocus() == win) {
        auto [wx, wy] = display::FromUi(G.mouse_x + 0.5, G.mouse_y + 0.5);
        last_x = std::round(wx);
        last_y = std::round(wy);
        if (warps.size() >= 16) warps.erase(warps.begin());
        warps.push_back({static_cast<int>(last_x), static_cast<int>(last_y)});
        SDL_WarpMouseInWindow(win, static_cast<int>(last_x), static_cast<int>(last_y));
    }
}

// A mouse button, held while any of the controls for it is.
static void PadMouse(int& held, int button, bool down) {
    if (down && held++ == 0)
        Mousepressed(G.mouse_x, G.mouse_y, button);
    else if (!down && held > 0 && --held == 0)
        Mousereleased(G.mouse_x, G.mouse_y, button);
}

static void PadKey(const std::string& k, bool down) {
    if (!down) {
        keys::Release(k);
        return;
    }
    keys::Press(k);
    Keypressed(k);
}

static void PadButton(int b, bool down) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:
    case pad::kRightTrigger:
        PadMouse(pad_left, 1, down);
        break;
    case SDL_CONTROLLER_BUTTON_X:
    case pad::kLeftTrigger:
        PadMouse(pad_right, 2, down);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        PadKey("escape", down);
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        PadKey("return", down);
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        if (down) Wheelmoved(-1);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        if (down) Wheelmoved(1);
        break;
    // the D-pad steps the stack, as 8 2 4 6 do
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        PadKey("8", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        PadKey("2", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        PadKey("4", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        PadKey("6", down);
        break;
    // held, as the keys are held for a Shift-click or an Alt-click
    case SDL_CONTROLLER_BUTTON_BACK:
        down ? keys::Press("lshift") : keys::Release("lshift");
        break;
    case SDL_CONTROLLER_BUTTON_START:
        down ? keys::Press("lalt") : keys::Release("lalt");
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        PadKey("space", down);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        PadKey("home", down);
        break;
    }
}

// A pad gone, or the window left: nothing stays held.
static void PadLetGo() {
    if (pad_left) Mousereleased(G.mouse_x, G.mouse_y, 1);
    if (pad_right) Mousereleased(G.mouse_x, G.mouse_y, 2);
    pad_left = pad_right = 0;
}

// Once a frame: the triggers, the pointer and the map's slide, by the
// time gone since the last.
static void PadUpdate() {
    static Uint64 then = SDL_GetPerformanceCounter();
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = std::min(0.1, static_cast<double>(now - then) / SDL_GetPerformanceFrequency());
    then = now;
    int b;
    bool down;
    while (pad::Trigger(b, down)) PadButton(b, down);
    // the speed goes as the square of the push: slow for aiming, quick across
    auto s = pad::ReadSticks();
    if (s.rx != 0 || s.ry != 0) {
        // the mouse or a touch moved it since
        if (static_cast<int>(pad_x) != G.mouse_x || static_cast<int>(pad_y) != G.mouse_y) {
            pad_x = G.mouse_x + 0.5;
            pad_y = G.mouse_y + 0.5;
        }
        double v = kPointerSpeed * display::d.h * std::hypot(s.rx, s.ry) * dt;
        PointTo(pad_x + s.rx * v, pad_y + s.ry * v);
    }
    if ((s.lx != 0 || s.ly != 0) && !(G.starting || !G.g || G.over || kit::Top() || G.banner ||
                                      G.offer || G.victory || G.drag)) {
        double k = kSlideSpeed * std::hypot(s.lx, s.ly) * dt * display::d.scale / (kTile * G.zoom);
        G.cx += s.lx * k;
        G.cy += s.ly * k;
        ClampCamera();
    }
}

// Text input is on while a field is being typed into, and only then: on the
// Steam Deck, SDL brings Steam's on-screen keyboard up with it. The hero's
// name is typed with no click on its field first, so the keyboard waits for
// one.
static void SyncTyping() {
    if (!G.offer) offer_typing = false;
    auto top = kit::Top();
    int want = G.offer ? (offer_typing ? 2 : 1) : top && top->Typing() ? 2 : 0;
    if (want == typing_now) return;
    if (want == 0 || typing_now == 2) SDL_StopTextInput();
    typing_now = want;
    if (want == 0) return;
    // as SDL starts it at first: text, but no keyboard, unless asked for
    const char* hint = SDL_GetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    SDL_StartTextInput();
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, nullptr);
}

// ------------------------------------------------------------------ starting

// A fresh game: its first turn, the view on the capital.
static void BeginGame() {
    Side* first = game::Begin(*G.g);
    if (!first) return;
    // a computer that opens the game plays to the screen of the human who
    // follows it, on that human's hidden map
    G.player = first->computer ? game::NextHuman(*G.g) : first;
    if (!G.player) G.player = first;
    G.cursor = CapOf(G.player);
    G.selection.reset();
    CentreOn(CapOf(G.player).first, CapOf(G.player).second);
    // the banner is a human's (8cc6:0259)
    if (!first->computer) {
        G.map_after_banner = HotseatHidden();
        ShowBanner(G.player);
    }
}

struct Script {
    std::vector<std::string> lines;
    size_t at = 0;
    int wait = 0;
};
static std::optional<Script> script;

void front::OpenStart() {
    G.starting = true;
    G.have_layout = false;
    sound::Music(w2::cues::kTitle);
    start::Open(
            [](const std::string& dir, const w2::game::NewGameOptions& opts) {
                auto start_game = [dir, opts]() {
                    G.starting = false;
                    G.have_layout = false;
                    G.scenario = dir;
                    w2::game::NewGameOptions o = opts;
                    o.seed = static_cast<double>(time(nullptr) % 1000000007);
                    G.seed = o.seed;
                    if (opt_military_advisor >= 0) {
                        for (auto& [k, v] : o.options)
                            if (k == "militaryAdvisor") v = opt_military_advisor;
                    }
                    G.g = game::NewGame(G.data_dir, dir, o);
                    G.selection.reset();
                    G.over = false;
                    front::StratDirty();
                    SyncLayout();
                    BeginGame();
                    if (G.g->side && G.g->side->computer) {
                        // a game with no human in it says so as it begins (8065:00f1)
                        bool human = false;
                        for (Side* s : G.g->sides)
                            if (!s->computer) human = true;
                        if (human)
                            PlayComputers(G.g->side);
                        else
                            search::ShowMessage(kit::Text(0xe, 0), kit::Text(0xe, 1),
                                                []() { PlayComputers(G.g->side); });
                    }
                };
                if (!script && sound::SpeechOn()) {
                    sound::Music(w2::cues::kBegin);
                    advisor::Say(w2::cues::kBeginWar, start_game);
                } else {
                    start_game();
                }
            },
            [](std::unique_ptr<w2::Game> g) {
                G.starting = false;
                G.have_layout = false;
                SyncLayout();
                front::TakeLoaded(std::move(g));
            });
}

// ------------------------------------------------------------------ scripts

// A script drives the game for testing: one command a line, the coordinates
// UI pixels.
//   wait N          let N frames go by
//   key NAME        press and release a key, by LÖVE's name for it
//   hold NAME / release NAME
//   click X Y [B]   press and release a mouse button (1 left, 2 right)
//   move X Y        move the pointer
//   text STRING     type
//   shot FILE       write the frame as a .bmp
//   glass FILE      write the window as the next frame shows it, the CRT's
//                   tube and all, as a .bmp
//   pad NAME        press and release a controller's button, by SDL's name
//                   for it (a, b, dpup, leftshoulder, lefttrigger ...)
//   padhold NAME / padrelease NAME
//   stick LX LY RX RY   hold the sticks there, -1..1; all 0 lets go
//   eval            print the game's state (turn, side, modals)
//   select TX TY    pick up the stack on a tile
//   attack TX TY    send the selection at a tile, fighting if it must
//   near            list the cities nearest the selection not the player's
//   greet           the advisor's greeting, as the start screens open
//   sdlclick X Y    a left click on UI point (X, Y) sent through SDL's event
//                   queue, as a real one arrives
//   quit
static void StepScript() {
    if (!script) return;
    if (script->wait > 0) {
        script->wait--;
        return;
    }
    while (script->at < script->lines.size()) {
        std::istringstream in(script->lines[script->at++]);
        std::string cmd;
        in >> cmd;
        if (cmd.empty() || cmd[0] == '#') continue;
        if (cmd == "wait") {
            in >> script->wait;
            return;
        } else if (cmd == "key") {
            std::string k;
            in >> k;
            keys::Press(k);
            Keypressed(k);
            keys::Release(k);
        } else if (cmd == "hold") {
            std::string k;
            in >> k;
            keys::Press(k);
        } else if (cmd == "release") {
            std::string k;
            in >> k;
            keys::Release(k);
        } else if (cmd == "click") {
            int x, y, b = 1;
            in >> x >> y;
            if (!(in >> b)) b = 1;
            G.mouse_x = x;
            G.mouse_y = y;
            display::d.mouse_inside = true;
            display::d.off.reset();
            Mousepressed(x, y, b);
            Mousereleased(x, y, b);
        } else if (cmd == "move") {
            in >> G.mouse_x >> G.mouse_y;
            display::d.mouse_inside = true;
            display::d.off.reset();
        } else if (cmd == "pad" || cmd == "padhold" || cmd == "padrelease") {
            std::string k;
            in >> k;
            int b = pad::FromName(k);
            if (b < 0) continue;
            if (cmd != "padrelease") PadButton(b, true);
            if (cmd != "padhold") PadButton(b, false);
        } else if (cmd == "stick") {
            pad::Sticks st;
            in >> st.lx >> st.ly >> st.rx >> st.ry;
            pad::Script(st);
        } else if (cmd == "text") {
            std::string t;
            std::getline(in, t);
            Textinput(w2::Trim(t));
        } else if (cmd == "shot") {
            std::string f;
            in >> f;
            gfx::SaveShot(f);
        } else if (cmd == "glass") {
            in >> display::d.glass_shot;
        } else if (cmd == "eval") {
            printf("turn=%d player=%s modals=%d banner=%d offer=%d ai=%d over=%d walk=%d "
                   "assault=%d status=%s\n",
                   G.g ? G.g->turn : 0, G.player ? G.player->name.c_str() : "-",
                   static_cast<int>(G.modals.size()), static_cast<bool>(G.banner),
                   static_cast<bool>(G.offer), static_cast<bool>(G.ai_run), G.over,
                   static_cast<bool>(G.walk), static_cast<bool>(G.assault),
                   G.ai_status ? G.ai_status->text.c_str() : "");
            if (G.g && G.g->map)
                printf("  options: milAdv=%d diplo=%d hidden=%d viewEn=%d\n",
                       G.g->map->options.military_advisor, G.g->map->options.diplomacy,
                       G.g->map->options.hidden_map, G.g->map->options.view_enemies);
            if (G.g && G.player && G.player->capital)
                printf("  cam=%.2f,%.2f zoom=%d map=%d,%d,%d,%d capital=%d,%d pointer=%d,%d\n",
                       G.cx, G.cy, G.zoom, G.map_rect.x, G.map_rect.y, G.map_rect.w, G.map_rect.h,
                       G.player->capital->x, G.player->capital->y, G.mouse_x, G.mouse_y);
            fflush(stdout);
        } else if (cmd == "opt") {
            std::string o;
            int v;
            in >> o >> v;
            if (G.g && G.g->map) {
                if (int* f = G.g->map->options.Field(o)) *f = v;
            }
        } else if (cmd == "load") {
            std::string s;
            in >> s;
            while (kit::Top()) kit::Pop(kit::Top());
            if (auto loaded = savegame::ReadSlot(s)) {
                front::TakeLoaded(std::move(loaded));
                SyncLayout();
            }
        } else if (cmd == "centre") {
            int tx, ty;
            in >> tx >> ty;
            CentreOn(tx, ty);
        } else if (cmd == "pointer") {
            int x, y;
            in >> x >> y;
            printf("pointer(%d,%d)=%d\n", x, y, PointerKind(x, y));
            fflush(stdout);
        } else if (cmd == "tileptr") {
            int tx, ty;
            in >> tx >> ty;
            int px = G.map_rect.x +
                     static_cast<int>((tx + 0.5 - G.cx) * kTile * G.zoom / display::d.scale);
            int py = G.map_rect.y +
                     static_cast<int>((ty + 0.5 - G.cy) * kTile * G.zoom / display::d.scale);
            printf("tileptr(%d,%d)=%d\n", tx, ty, PointerKind(px, py));
            fflush(stdout);
        } else if (cmd == "spawn" && G.g) {
            int tx, ty, owner, type;
            in >> tx >> ty >> owner >> type;
            auto a = std::make_unique<Army>();
            a->x = tx;
            a->y = ty;
            a->owner = owner;
            a->type = type;
            a->moves = 10;
            G.g->armies.push_back(a.get());
            G.g->pool.push_back(std::move(a));
        } else if (cmd == "advise") {
            int x, y;
            in >> x >> y;
            miladvisor::Open(G.selection ? &G.selection->stack : nullptr, x, y);
        } else if (cmd == "select") {
            int x, y;
            in >> x >> y;
            Select(x, y);
        } else if (cmd == "attack") {
            int x, y;
            in >> x >> y;
            AttackTile(x, y);
        } else if (cmd == "near" && G.g && G.selection) {
            std::vector<std::pair<int, City*>> found;
            for (auto& c : G.g->map->cities) {
                if (c.owner_index != G.player->index && !c.razed)
                    found.push_back({std::max(std::abs(c.x - G.selection->x),
                                              std::abs(c.y - G.selection->y)),
                                     &c});
            }
            std::sort(found.begin(), found.end(),
                      [](auto& a, auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < found.size() && i < 3; i++)
                printf("  %s at %d,%d owner %d, %d away\n", found[i].second->name.c_str(),
                       found[i].second->x, found[i].second->y, found[i].second->owner_index,
                       found[i].first);
            fflush(stdout);
        } else if (cmd == "greet") {
            advisor::Say(w2::cues::kGreet);
        } else if (cmd == "sdlclick") {
            // a click on UI point (x, y) as SDL delivers one, in window points,
            // through the event loop
            int ux, uy;
            in >> ux >> uy;
            auto [wx, wy] = display::FromUi(ux, uy);
            int x = static_cast<int>(wx), y = static_cast<int>(wy);
            for (Uint32 type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
                SDL_Event ev{};
                ev.type = type;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                ev.button.x = x;
                ev.button.y = y;
                SDL_PushEvent(&ev);
            }
        } else if (cmd == "quit") {
            G.quit_requested = true;
            return;
        }
    }
}

// ------------------------------------------------------------------ main

// Why the game cannot start, on stderr and in a box: on Windows the game has
// no console to print to.
static int Fail(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", msg.c_str(), nullptr);
    return 1;
}

int main(int argc, char** argv) {
    std::string scenario, script_file, weights_file;
    double seed = 0;
    int size_w = 0, size_h = 0;
    bool windowed = prefs::Get("screen") == "window";
    bool use_classic_ai = prefs::Get("ai") == "classic";
    bool force_diplomacy = false;
    bool force_hidden_map = false;
    bool spectate_all_ai = false;
    G.modern_ui = false;
    G.data_dir = "original";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data")
            G.data_dir = next();
        else if (a == "--scenario")
            scenario = w2::Upper(next());
        else if (a == "--seed")
            seed = atof(next().c_str());
        else if (a == "--window")
            windowed = true;
        else if (a == "--fullscreen")
            windowed = false;
        else if (a == "--script")
            script_file = next();
        else if (a == "--size")
            sscanf(next().c_str(), "%dx%d", &size_w, &size_h);
        else if (a == "--classic-ai")
            use_classic_ai = true;
        else if (a == "--modern-ai")
            use_classic_ai = false;
        else if (a == "--modern-ui")
            G.modern_ui = true;
        else if (a == "--classic-ui")
            G.modern_ui = false;
        else if (a == "--weights")
            weights_file = next();
        else if (a == "--diplomacy")
            force_diplomacy = true;
        else if (a == "--hidden-map")
            force_hidden_map = true;
        else if (a == "--military-advisor")
            opt_military_advisor = 1;
        else if (a == "--no-military-advisor")
            opt_military_advisor = 0;
        else if (a == "--spectate" || a == "--all-ai")
            spectate_all_ai = true;
    }
    // Modern AI by default; its trained weights come from --weights or ai_weights.json
    w2::ai::SetDefaultClassicAi(use_classic_ai);
    std::string weights_path = !weights_file.empty() ? weights_file
                               : w2::FileExists("cpp/ai_weights.json")
                                       ? std::string("cpp/ai_weights.json")
                               : w2::FileExists("ai_weights.json") ? std::string("ai_weights.json")
                                                                   : std::string();
    if (weights_path.empty() || !w2::ai::ml::ActivePolicy().LoadJson(weights_path)) {
        fprintf(stderr,
                "warning: Modern AI weights %s%s%s could not be loaded (missing, or trained for a "
                "different network "
                "size); the Modern AI will play with untrained random weights. Retrain with "
                "cpp/test/train.sh or "
                "pass --weights PATH.\n",
                weights_path.empty() ? "(none found)" : "'", weights_path.c_str(),
                weights_path.empty() ? "" : "'");
    }
    if (!w2::FileExists(G.data_dir + "/TERRAIN0/ARMYTYPE.DAT"))
        return Fail("cannot find the game's files in '" + G.data_dir +
                    "': run from the repository root, or pass --data DIR");
    pad::Hints();
    // Windows: the window sized in points and drawn in pixels, as on a Mac,
    // rather than Windows stretching a blurred 100% picture
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        return Fail(std::string("SDL: ") + SDL_GetError());
    }
    SDL_Window* win = display::Open("Warlords II", windowed, size_w, size_h);
    if (!win) {
        return Fail(std::string("SDL window: ") + SDL_GetError());
    }
    // OpenGL, where the CRT screen's tube is a shader (platform/crt.hpp); SDL
    // goes on down its list where there is none, and SDL_RENDER_DRIVER wins
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_Renderer* ren =
            SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        return Fail(std::string("SDL renderer: ") + SDL_GetError());
    }
    gfx::Init(ren);
    SDL_ShowCursor(SDL_DISABLE);
    // SDL starts with text input on; it waits here for a field (syncTyping)
    SDL_StopTextInput();
    if (!script_file.empty()) {
        Script s;
        std::ifstream in(script_file);
        std::string line;
        while (std::getline(in, line)) s.lines.push_back(line);
        script = s;
    }
    try {
        LoadArt();
        InstallHooks();
        auto ui = w2::uidata::Load(G.data_dir);
        sound::Init(G.data_dir, ui.files);
        // the screen: its real pixels, the UI's scale, and the map's zoom
        if (prefs::Get("4:3") == "on") display::d.wanted = std::make_pair(layout::kW, layout::kH);
        display::d.crt = prefs::Get("crt") == "on";
        display::d.chosen =
                prefs::Get("scale") == "smooth" ? display::kSmooth : prefs::GetInt("scale", 0);
        display::Measure();
        G.zoom = prefs::GetInt("zoom", display::d.scale);
        if (scenario.empty()) {
            front::OpenStart();
            // 7f77:0000 greets the player the first time round
            if (script_file.empty()) advisor::Say(w2::cues::kGreet);
        } else {
            G.starting = false;
            G.scenario = scenario;
            game::NewGameOptions o;
            o.seed = seed != 0 ? seed : static_cast<double>(time(nullptr) % 1000000007);
            static const char* const kOptionKeys[10] = {
                "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
                "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
            int default_options[10] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
            if (auto opt_bytes = w2::ReadFile(G.data_dir + "/DATA/OPTIONS.DAT")) {
                for (size_t i = 0; i <= 9 && 2 * i + 2 <= opt_bytes->size(); i++) {
                    default_options[i] = w2::U16(*opt_bytes, 2 * i);
                }
            }
            for (int i = 0; i < 10; i++) {
                o.options.emplace_back(kOptionKeys[i], default_options[i]);
            }
            if (force_diplomacy) {
                for (auto& [k, v] : o.options)
                    if (k == "diplomacy") v = 1;
            }
            if (force_hidden_map) {
                for (auto& [k, v] : o.options) {
                    if (k == "hiddenMap") v = 1;
                    if (k == "viewEnemies") v = 0;
                }
            }
            if (opt_military_advisor >= 0) {
                for (auto& [k, v] : o.options)
                    if (k == "militaryAdvisor") v = opt_military_advisor;
            }
            if (scenario == w2::randommap::kDir) {
                // RANDOM: a random world with the start menu's first settings, made
                // from the seed, in place of the last one the original left on disk
                w2::Rng r(o.seed);
                w2::randommap::Options ro;
                ro.data_dir = G.data_dir;
                ro.rng = &r;
                w2::randommap::Install(G.data_dir, w2::randommap::GenerateNow(ro));
            }
            G.g = game::NewGame(G.data_dir, scenario, o);
            for (size_t i = 0; i < G.g->sides.size(); i++)
                G.g->sides[i]->computer = spectate_all_ai || (i > 0);
            auto start_war = [=]() {
                SyncLayout();
                BeginGame();
                if (G.g->side && G.g->side->computer) PlayComputers(G.g->side);
            };
            if (script_file.empty() && !spectate_all_ai && sound::SpeechOn()) {
                sound::Music(w2::cues::kBegin);
                advisor::Say(w2::cues::kBeginWar, start_war);
            } else {
                start_war();
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "could not start: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", e.what(), win);
        return 1;
    }

    {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        last_x = mx;
        last_y = my;
        display::d.mouse_inside = (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        display::d.off = display::OffFrame(mx, my);
        std::tie(G.mouse_x, G.mouse_y) = display::ToUi(mx, my);
        pad_x = G.mouse_x + 0.5;
        pad_y = G.mouse_y + 0.5;
    }
    while (!G.quit_requested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            try {
                switch (e.type) {
                case SDL_QUIT:
                    G.quit_requested = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        display::Measure();
                        G.have_layout = false;
                    } else if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
                        display::d.mouse_inside = false;
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        keys::Clear();
                        PadLetGo();
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        SDL_ShowCursor(SDL_DISABLE);  // the game draws its own pointer
                    }
                    break;
                case SDL_KEYDOWN: {
                    std::string k = keys::LoveName(e.key.keysym.sym);
                    if (k.empty()) break;
                    // the Lua remake leaves LÖVE's key repeat off
                    if (e.key.repeat) break;
                    keys::Press(k);
                    Keypressed(k);
                    break;
                }
                case SDL_KEYUP: {
                    std::string k = keys::LoveName(e.key.keysym.sym);
                    if (!k.empty()) keys::Release(k);
                    break;
                }
                case SDL_TEXTINPUT:
                    if (!keys::IsDown({"lalt", "ralt", "lctrl", "rctrl", "lgui", "rgui"}))
                        Textinput(w2::Utf8ToLatin1(e.text.text));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    // a click on a window that has not got the focus takes it (see
                    // display::open): else the pointer moves only while dragging
                    if (e.type == SDL_MOUSEBUTTONDOWN &&
                        !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
                        SDL_RaiseWindow(win);
                    }
                    auto [x, y] = display::ToUi(e.button.x, e.button.y);
                    G.mouse_x = x;
                    G.mouse_y = y;
                    int b = e.button.button == SDL_BUTTON_RIGHT    ? 2
                            : e.button.button == SDL_BUTTON_MIDDLE ? 3
                                                                   : 1;
                    if (e.type == SDL_MOUSEBUTTONDOWN)
                        Mousepressed(x, y, b);
                    else
                        Mousereleased(x, y, b);
                    break;
                }
                case SDL_MOUSEMOTION: {
                    // a warp's own motion comes back late, when the stick has gone on
                    auto w = std::find(warps.begin(), warps.end(),
                                       std::make_pair(e.motion.x, e.motion.y));
                    if (w != warps.end()) {
                        warps.erase(warps.begin(), w + 1);
                        break;
                    }
                    auto [x, y] = display::ToUi(e.motion.x, e.motion.y);
                    G.mouse_x = x;
                    G.mouse_y = y;
                    display::d.mouse_inside = true;
                    display::d.off = display::OffFrame(e.motion.x, e.motion.y);
                    double k = display::d.dpi / display::d.scale;
                    Mousemoved((e.motion.x - last_x) * k, (e.motion.y - last_y) * k);
                    last_x = e.motion.x;
                    last_y = e.motion.y;
                    break;
                }
                case SDL_MOUSEWHEEL:
                    if (e.wheel.y != 0) Wheelmoved(e.wheel.y > 0 ? 1 : -1);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                case SDL_CONTROLLERDEVICEREMOVED:
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int b;
                    bool down;
                    if (pad::TranslateEvent(e, b, down))
                        PadButton(b, down);
                    else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                        PadLetGo();
                    break;
                }
                }
            } catch (const std::exception& ex) {
                fprintf(stderr, "error: %s\n", ex.what());
            }
        }
        // Where the mouse is, asked rather than waited for. On some Macs (SDL 3
        // under sdl2-compat) the window holds both focuses and still SDL sends
        // the mouse's moves only while a button is held -- a bare SDL program
        // shows it too -- so the pointer would follow only a drag. So while the
        // window has the focus, the pointer follows wherever the mouse is; a
        // move SDL did send has already brought lastX/lastY up to date.
        if (!script && SDL_GetKeyboardFocus() == win) {
            int gx, gy, wx, wy, ww, wh;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_GetWindowPosition(win, &wx, &wy);
            SDL_GetWindowSize(win, &ww, &wh);
            int mx = gx - wx, my = gy - wy;
            if (mx >= 0 && my >= 0 && mx < ww && my < wh &&
                (mx != static_cast<int>(last_x) || my != static_cast<int>(last_y))) {
                auto [x, y] = display::ToUi(mx, my);
                G.mouse_x = x;
                G.mouse_y = y;
                display::d.mouse_inside = true;
                display::d.off = display::OffFrame(mx, my);
                double k = display::d.dpi / display::d.scale;
                Mousemoved((mx - last_x) * k, (my - last_y) * k);
                last_x = mx;
                last_y = my;
            }
        }
        try {
            display::d.min_h = G.starting || !G.g ? layout::kH : layout::kShortH;
            display::Measure();
            display::BeginFrame();
            PadUpdate();
            SyncTyping();
            StepScript();
            Update();
            Draw();
        } catch (const std::exception& ex) {
            fprintf(stderr, "error: %s\n", ex.what());
        }
        display::Present();
    }
    G.ai_run.reset();
    G.modals.clear();
    sound::Shutdown();
    gfx::Shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
