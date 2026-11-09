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
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace tileinfo {

namespace {

using PaintCallback = std::function<void(w2::UiContext&)>;
using ArmyStack = std::vector<w2::Army*>;

constexpr int kBoxWidth = 256;  ///< Width of the box.
constexpr int kBoxHeight = 75;  ///< Height of the box.

/** @brief Returns the dialog font coloured in `side`'s banner and edge palette indices. */
const Font& SideColours(const w2::UiContext& ctx, const w2::Side* side) {
    return kit::GetFont(ctx, 2).Colours(side->Colour(), side->Edge());
}

/** @brief Draws two centred lines inside `(box_x, box_y)`, colouring the first unless
 * `plain_first`. */
void DrawTwoLines(const w2::UiContext& ctx, int box_x, int box_y, const std::string& first,
                  const std::string& second, bool plain_first) {
    const Font& font = kit::GetFont(ctx, 2);
    kit::Centred(plain_first ? font : font.Colours(7, 0), first, box_x + kBoxWidth / 2 - 8,
                 box_y + 11);
    kit::Centred(font, second, box_x + kBoxWidth / 2 - 8, box_y + 35);
}

/** @brief Draws the armies on a visible tile side-by-side inside `(box_x, box_y)`. */
void PaintArmies(w2::UiContext& ctx, const ArmyStack& armies, int box_x, int box_y) {
    int count = static_cast<int>(armies.size());
    int x = (box_x + kBoxWidth / 2 - count * 12 - 16 + 4) / 8 * 8;
    for (int i = 0; i < count; i++) {
        kit::Army(ctx, armies[i]->TypeIndex(), armies[i]->OwnerIndex(), x + 24 * i, box_y + 16, 0);
    }
}

/** @brief Draws city ownership shields, capital marks, income, and defence inside `(box_x, box_y)`.
 */
void PaintCity(w2::UiContext& ctx, const w2::City* city, const w2::Side* player, int box_x,
               int box_y) {
    gfx::Renderer& ren = ctx.Renderer();
    const Font& font = kit::GetFont(ctx, 2);
    const w2::Map& map = ctx.ActiveGame().GetMap();
    const w2::Side* owner_side = !city->IsNeutral() ? &map.Sides()[city->OwnerIndex()] : player;
    kit::Centred(SideColours(ctx, owner_side), city->Name(), box_x + kBoxWidth / 2 - 8, box_y + 11);
    if (city->IsRazed()) {
        kit::Centred(font, "Razed!", box_x + kBoxWidth / 2 - 8, box_y + 35);
        return;
    }
    if (!city->IsNeutral() && city->OwnerIndex() < 8) {
        for (int x : {box_x + 24, box_x + kBoxWidth - 48}) {
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().ShieldsImg(),
                     gfx::NewQuad(city->OwnerIndex() * 40 + 24, 46, 16, 16), x, box_y + 10);
        }
    }
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(424, 0, 24, 11), box_x + 48, box_y + 36);
    font.Draw(absl::StrFormat("%d", city->Income()), box_x + 80, box_y + 33);
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(424, 11, 24, 11), box_x + 144, box_y + 36);
    font.Draw(absl::StrFormat("%d", city->Defence()), box_x + 176, box_y + 33);
    // a capital is marked on both sides with its side's cell of art 38
    const screen::Art* capital_marks = ctx.ControlArt(38);
    for (int i = 0; i < 8; i++) {
        if (map.Sides()[i].CapitalCity(map) == city && capital_marks) {
            for (int x : {box_x + 16, box_x + kBoxWidth - 56}) {
                ren.SetColor(1, 1, 1);
                ren.Draw(capital_marks->image, gfx::NewQuad(i * 32, 36, 32, 23), x, box_y + 10);
            }
        }
    }
}

/** @brief Draws a ruin or temple inspection summary inside `(box_x, box_y)`. */
void PaintSite(const w2::UiContext& ctx, const w2::Site* site, const w2::Side* player, int box_x,
               int box_y) {
    const Font& font = kit::GetFont(ctx, 2);
    kit::Centred(SideColours(ctx, player), site->name, box_x + kBoxWidth / 2 - 8, box_y + 11);
    if (site->content == w2::site::kTemple) {
        kit::Centred(font, "Blessings & Quests!", box_x + kBoxWidth / 2 - 8, box_y + 35);
    } else if (site->searched) {
        kit::Centred(font, "Explored!", box_x + kBoxWidth / 2 - 8, box_y + 33);
    } else {
        kit::Centred(font, "Unexplored!", box_x + kBoxWidth / 2 - 8, box_y + 35);
    }
}

/** @brief The box over the tile; any click, release or key takes it away. */
class TileBoxModal final : public kit::Modal {
  public:
    TileBoxModal(w2::UiContext& ctx, int box_x, int box_y, bool signpost_board, PaintCallback paint)
            : kit::Modal(ctx)
            , box_x_(box_x)
            , box_y_(box_y)
            , signpost_board_(signpost_board)
            , paint_(std::move(paint)) {
        SetIsInfobox(true);
    }

    /** @brief Draws the board, then what is on the tile. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        gfx::ImageP board =
                infobox::BoardImage(ctx, signpost_board_ ? infobox::kPopuP2 : infobox::kLines);
        ren.SetColor(1, 1, 1);
        if (board) ren.Draw(board, gfx::NewQuad(0, 0, kBoxWidth, kBoxHeight), box_x_, box_y_);
        ren.SetColor(1, 1, 1);
        paint_(ctx);
    }

    bool Mousereleased(int, int, int) override {
        PopSelf();
        return true;
    }

    void Mousepressed(int, int, int) override { PopSelf(); }
    void Keypressed(const std::string&) override { PopSelf(); }

  private:
    int box_x_ = 0;                ///< Left edge, in the dialogs' frame.
    int box_y_ = 0;                ///< Top edge, in the dialogs' frame.
    bool signpost_board_ = false;  ///< POPUP2.PCK, the signpost's board, behind.
    PaintCallback paint_;          ///< Draws what is on the tile over the board.
};

}  // namespace

/**
 * @brief Shows what is on map tile (tile_x, tile_y) in a box over it.
 *
 * The box is kept on the screen and its centre snapped to 8 pixels across.
 */
void Open(w2::UiContext& ctx, int tile_x, int tile_y) {
    w2::Game& game = ctx.ActiveGame();
    w2::Side* player = ctx.Player();
    int viewer = w2::game::ViewerSide(game, player);
    if (!w2::game::Seen(game, viewer, tile_x, tile_y)) return;
    const auto& screen_layout = ctx.GetViewport().GetLayout();
    auto [exact_centre_x, exact_centre_y] = ctx.MapToUi(tile_x + 0.5, tile_y + 0.5);
    int centre_x = static_cast<int>(std::floor(exact_centre_x));
    int centre_y = static_cast<int>(std::floor(exact_centre_y));
    centre_x = (centre_x + 4) / 8 * 8;
    centre_x = std::max(kBoxWidth / 2, std::min(screen_layout.w - kBoxWidth / 2, centre_x));
    centre_y = std::max(kBoxHeight / 2, std::min(screen_layout.h - 2 - kBoxHeight / 2, centre_y));
    int box_x = centre_x - kBoxWidth / 2 - screen_layout.dialog.x;
    int box_y = centre_y - kBoxHeight / 2 - screen_layout.dialog.y;

    ArmyStack armies = w2::game::ArmiesAt(game, tile_x, tile_y);
    int owner = armies.empty() ? w2::kNone : armies[0]->OwnerIndex();
    bool tower = w2::game::TowerAt(game, tile_x, tile_y);
    w2::City* city = w2::game::CityAt(game, tile_x, tile_y);
    w2::Map& map = game.GetMap();
    w2::Site* site = map.SiteAt(tile_x, tile_y);
    int terrain = w2::scn::TerrainAt(map, tile_x, tile_y);

    bool signpost_board = false;
    PaintCallback paint;

    if (!armies.empty() && (viewer == w2::kNone || map.GameOptions().view_enemies != 0 ||
                            owner == player->Index() || tower)) {
        if (viewer != w2::kNone && owner != player->Index() && tower &&
            map.GameOptions().view_enemies == 0) {
            paint = [box_x, box_y](w2::UiContext& draw_ctx) {
                DrawTwoLines(draw_ctx, box_x, box_y, kit::Text(draw_ctx, 0x82, 0),
                             kit::Text(draw_ctx, 0x82, 1), false);
            };
        } else {
            paint = [armies = std::move(armies), box_x, box_y](w2::UiContext& draw_ctx) {
                PaintArmies(draw_ctx, armies, box_x, box_y);
            };
        }
    } else if (city) {
        paint = [city, player, box_x, box_y](w2::UiContext& draw_ctx) {
            PaintCity(draw_ctx, city, player, box_x, box_y);
        };
    } else if (site) {
        paint = [site, player, box_x, box_y](w2::UiContext& draw_ctx) {
            PaintSite(draw_ctx, site, player, box_x, box_y);
        };
    } else if (terrain == w2::move::kTower && w2::game::SignAt(game, tile_x, tile_y)) {
        w2::Sign* sign = w2::game::SignAt(game, tile_x, tile_y);
        signpost_board = true;
        paint = [sign, box_x, box_y](w2::UiContext& draw_ctx) {
            DrawTwoLines(draw_ctx, box_x, box_y, sign->lines[0], sign->lines[1], true);
        };
    } else {
        bool road = w2::scn::RoadAt(map, tile_x, tile_y) % 32 != 0;
        bool port = map.IsCrossing(tile_x, tile_y);
        if (port) {
            paint = [box_x, box_y](w2::UiContext& draw_ctx) {
                DrawTwoLines(draw_ctx, box_x, box_y, "Port", "A way for armies to put to sea",
                             false);
            };
        } else {
            // a road is told as line 0, else the terrain's own lines
            int terrain_line = road ? 0 : terrain;
            paint = [terrain_line, box_x, box_y](w2::UiContext& draw_ctx) {
                DrawTwoLines(draw_ctx, box_x, box_y, kit::Text(draw_ctx, 0x80, terrain_line),
                             kit::Text(draw_ctx, 0x81, terrain_line), false);
            };
        }
    }

    kit::Push(ctx,
              std::make_shared<TileBoxModal>(ctx, box_x, box_y, signpost_board, std::move(paint)));
}

}  // namespace tileinfo
