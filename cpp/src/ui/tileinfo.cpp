/**
 * @file
 * @brief The right button on the map: what is on a tile (button 2).
 *
 * Only on a tile the side has seen, and gone when the button comes up. The
 * box is 256x75, centred on the tile, with POPUP.PCK behind it through its
 * mask. What it shows, first that applies: a stack the side may see, side by
 * side -- an enemy in a tower only "Tower / Very hard to conquer!"; a city
 * with its shields, income and defence; a site; a signpost over POPUP2.PCK;
 * else the terrain.
 */
#include <cmath>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace tileinfo {

namespace {
const int kBoxWidth = 256;  ///< Width of the box.
const int kBoxHeight = 75;  ///< Height of the box.

/** @brief The box over the tile; any click, release or key takes it away. */
struct TileBox : kit::Modal {
    int box_x = 0;                ///< Left edge, in the dialogs' frame.
    int box_y = 0;                ///< Top edge, in the dialogs' frame.
    bool signpost_board = false;  ///< POPUP2.PCK, the signpost's board, behind.
    std::function<void()> paint;  ///< Draws what is on the tile over the board.
    /** @brief Draws the board, then what is on the tile. */
    void Draw() override {
        auto board = infobox::BoardImage(signpost_board ? infobox::kPopuP2 : infobox::kLines);
        gfx::SetColor(1, 1, 1);
        if (board) gfx::Draw(board, gfx::NewQuad(0, 0, kBoxWidth, kBoxHeight), box_x, box_y);
        gfx::SetColor(1, 1, 1);
        paint();
    }
    bool Mousereleased(int, int, int) override {
        kit::Pop(this);
        return true;
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};
}  // namespace

/**
 * @brief Shows what is on map tile (tile_x, tile_y) in a box over it.
 *
 * The box is kept on the screen and its centre snapped to 8 pixels across.
 */
void Open(int tile_x, int tile_y) {
    w2::Game& game = *g_app.g;
    int viewer = w2::game::ViewerSide(game, g_app.player);
    if (!w2::game::Seen(game, viewer, tile_x, tile_y)) return;
    const auto& screen_layout = g_app.layout;
    auto [exact_centre_x, exact_centre_y] = front::MapToUi(tile_x + 0.5, tile_y + 0.5);
    int centre_x = static_cast<int>(std::floor(exact_centre_x)),
        centre_y = static_cast<int>(std::floor(exact_centre_y));
    centre_x = (centre_x + 4) / 8 * 8;
    centre_x = std::max(kBoxWidth / 2, std::min(screen_layout.w - kBoxWidth / 2, centre_x));
    centre_y = std::max(kBoxHeight / 2, std::min(screen_layout.h - 2 - kBoxHeight / 2, centre_y));
    int box_x = centre_x - kBoxWidth / 2 - screen_layout.dialog.x,
        box_y = centre_y - kBoxHeight / 2 - screen_layout.dialog.y;
    w2::Side* player = g_app.player;
    auto box = std::make_shared<TileBox>();
    box->box_x = box_x;
    box->box_y = box_y;

    auto armies = w2::game::ArmiesAt(game, tile_x, tile_y);
    int owner = armies.empty() ? w2::kNone : armies[0]->OwnerIndex();
    bool tower = w2::game::TowerAt(game, tile_x, tile_y);
    w2::City* city = w2::game::CityAt(game, tile_x, tile_y);
    w2::Site* site = game.map->site_at.empty()
                             ? nullptr
                             : game.map->site_at[tile_y * game.map->width + tile_x];
    int terrain = w2::scn::TerrainAt(*game.map, tile_x, tile_y);
    // the text font in a side's colours
    auto colours = [](const w2::Side* side) -> const Font& {
        return kit::GetFont(2).Colours(side->Colour(), side->Edge());
    };
    // two centred lines; the first in colours 7 and 0 unless `plain_first`
    auto two_lines = [box_x, box_y](const std::string& first, const std::string& second,
                                    bool plain_first) {
        const Font& font = kit::GetFont(2);
        kit::Centred(plain_first ? font : font.Colours(7, 0), first, box_x + kBoxWidth / 2 - 8,
                     box_y + 11);
        kit::Centred(font, second, box_x + kBoxWidth / 2 - 8, box_y + 35);
    };

    if (!armies.empty() && (viewer == w2::kNone || game.map->options.view_enemies != 0 ||
                            owner == player->Index() || tower)) {
        if (viewer != w2::kNone && owner != player->Index() && tower &&
            game.map->options.view_enemies == 0) {
            box->paint = [two_lines]() {
                two_lines(kit::Text(0x82, 0), kit::Text(0x82, 1), false);
            };
        } else {
            box->paint = [armies, box_x, box_y]() {
                int count = static_cast<int>(armies.size());
                int x = (box_x + kBoxWidth / 2 - count * 12 - 16 + 4) / 8 * 8;
                for (int i = 0; i < count; i++)
                    kit::Army(armies[i]->TypeIndex(), armies[i]->OwnerIndex(), x + 24 * i,
                              box_y + 16, 0);
            };
        }
    } else if (city) {
        box->paint = [city, player, box_x, box_y, colours]() {
            const Font& font = kit::GetFont(2);
            const w2::Side* owner_side =
                    !city->IsNeutral() ? &g_app.g->map->sides[city->OwnerIndex()] : player;
            kit::Centred(colours(owner_side), city->Name(), box_x + kBoxWidth / 2 - 8, box_y + 11);
            if (city->IsRazed()) {
                kit::Centred(font, "Razed!", box_x + kBoxWidth / 2 - 8, box_y + 35);
                return;
            }
            if (!city->IsNeutral() && city->OwnerIndex() < 8) {
                for (int x : {box_x + 24, box_x + kBoxWidth - 48}) {
                    gfx::SetColor(1, 1, 1);
                    gfx::Draw(g_app.shields_img,
                              gfx::NewQuad(city->OwnerIndex() * 40 + 24, 46, 16, 16), x,
                              box_y + 10);
                }
            }
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.abits, gfx::NewQuad(424, 0, 24, 11), box_x + 48, box_y + 36);
            font.Draw(absl::StrFormat("%d", city->Income()), box_x + 80, box_y + 33);
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.abits, gfx::NewQuad(424, 11, 24, 11), box_x + 144, box_y + 36);
            font.Draw(absl::StrFormat("%d", city->Defence()), box_x + 176, box_y + 33);
            // a capital is marked on both sides with its side's cell of art 38
            auto capital_marks = g_app.screen->ArtFor(38);
            for (int i = 0; i < 8; i++) {
                if (g_app.g->map->sides[i].CapitalCity(*g_app.g->map) == city && capital_marks) {
                    for (int x : {box_x + 16, box_x + kBoxWidth - 56}) {
                        gfx::SetColor(1, 1, 1);
                        gfx::Draw(capital_marks->image, gfx::NewQuad(i * 32, 36, 32, 23), x,
                                  box_y + 10);
                    }
                }
            }
        };
    } else if (site) {
        box->paint = [site, player, box_x, box_y, colours]() {
            const Font& font = kit::GetFont(2);
            kit::Centred(colours(player), site->name, box_x + kBoxWidth / 2 - 8, box_y + 11);
            if (site->content == w2::site::kTemple)
                kit::Centred(font, "Blessings & Quests!", box_x + kBoxWidth / 2 - 8, box_y + 35);
            else if (site->searched)
                kit::Centred(font, "Explored!", box_x + kBoxWidth / 2 - 8, box_y + 33);
            else
                kit::Centred(font, "Unexplored!", box_x + kBoxWidth / 2 - 8, box_y + 35);
        };
    } else if (terrain == w2::move::kTower && w2::game::SignAt(game, tile_x, tile_y)) {
        w2::Sign* sign = w2::game::SignAt(game, tile_x, tile_y);
        box->signpost_board = true;
        box->paint = [sign, two_lines]() { two_lines(sign->lines[0], sign->lines[1], true); };
    } else {
        bool road = w2::scn::RoadAt(*game.map, tile_x, tile_y) % 32 != 0;
        bool port = game.map->crossing[tile_y * game.map->width + tile_x];
        if (port) {
            box->paint = [two_lines]() {
                two_lines("Port", "A way for armies to put to sea", false);
            };
        } else {
            // a road is told as line 0, else the terrain's own lines
            int terrain_line = road ? 0 : terrain;
            box->paint = [terrain_line, two_lines]() {
                two_lines(kit::Text(0x80, terrain_line), kit::Text(0x81, terrain_line), false);
            };
        }
    }
    box->infobox = true;
    kit::Push(box);
}

}  // namespace tileinfo
