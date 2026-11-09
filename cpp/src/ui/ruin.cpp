/**
 * @file
 * @brief View > Ruins (`.`): the city dialog's fifth mode, on a ruin or temple.
 *
 * The city dialog opened in mode 4 at the cursor takes the nearest site shown
 * to the side. It is dialog 6 over popup 2 with only Done (192); the map with
 * every site shown marked and this one boxed; and the city dialog's info page
 * for mode 4: the name, SPECBITS.PCK's picture, "Type: ..." and
 * "Explored: ...", the map markers' legend, and the site's three lines of
 * .SPC description.
 */
#include <cmath>
#include <memory>
#include <string>

#include "ui/dialogs.hpp"
#include "warlords/site.hpp"

namespace ruin {

namespace {

constexpr kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2, the whole dialog.
constexpr kit::Rect kMap{80, 60, 224, 312};        ///< The strategic map on its left.
constexpr int kDialog = 6;                         ///< BUTTON.DAT dialog number.
constexpr int kDone = 192;                         ///< The Done control.
/// SPECBITS.PCK cells (left, top), 96x63: the temple first, then five ruins.
constexpr int kPictures[6][2] = {{0, 0}, {96, 0}, {192, 0}, {0, 63}, {96, 63}, {192, 63}};
/// The legend's four map markers: their cells in the shield sheet.
constexpr int kLegend[4][2] = {{112, 0}, {112, 10}, {112, 20}, {128, 0}};
/// Where each legend marker is drawn; its line of text follows it.
constexpr int kLegendAt[4][2] = {{320, 188}, {432, 188}, {320, 208}, {432, 208}};

/**
 * @brief The site dialog: one ruin or temple, with the map of all the sites.
 */
class RuinModal final : public kit::Modal {
  public:
    RuinModal(w2::UiContext& ctx, w2::Site* site)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), site_(site) {
        SetDialogView(&dialog_view_);
        dialog_view_.state[kDone] = w2::uidata::kNormal;
        for (const auto& control : dialog_view_.dialog.controls) {
            if (control.id != kDone) HiddenControls().insert(control.id);
        }
    }

    /** @brief Draws the map, the site's picture, its state, legend and text. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kPopupRect);
        ctx.Actions().DrawStrategicMap(kMap.x, kMap.y);
        ctx.Actions().DrawSiteMarkers(kMap.x, kMap.y, site_);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), site_->name, 432, 62);
        kit::Bevel(ctx, 326, 102, 100, 67, 4, 2);
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 327, 103, 98, 65);
        const int* pic =
                kPictures[site_->content == w2::site::kTemple ? 0 : (site_->index % 5) + 1];
        if (const screen::Art* art = ctx.ControlArt(6)) {
            ren.SetColor(1, 1, 1);
            ren.Draw(art->image, gfx::NewQuad(pic[0], pic[1], 96, 63), 328, 104);
        }
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        font.Draw(kit::Text(ctx, 0x71, site_->content), 432, 112);
        font.Draw(kit::Text(ctx, 0x72, site_->searched ? 1 : 0), 432, 136);
        kit::SetPal(ctx, 2);
        ren.Rectangle(gfx::kFill, 312, 180, 240, 48);
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 312, 180, 240, 48);
        for (int i = 0; i < 4; i++) {
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().AtransShields(),
                     gfx::NewQuad(kLegend[i][0], kLegend[i][1], 16, 10), kLegendAt[i][0],
                     kLegendAt[i][1]);
            font.Draw(kit::Text(ctx, 0x73, i), kLegendAt[i][0] + 16, kLegendAt[i][1] - 3);
        }
        const auto* description = ctx.ActiveGame().GetMap().SiteDescription(site_->index);
        if (description) {
            for (int i = 0; i < 3; i++) font.Draw((*description)[i], 310, 259 + 20 * i);
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief Done closes; a click on the map moves to the site nearest it. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (control && control->id == kDone) {
            PopSelf();
            return;
        }
        if (!control && kMap.Contains(x, y)) {
            w2::Site* other = Nearest(ctx, (x - kMap.x) / 2, (y - kMap.y) / 2);
            if (other && other != site_) {
                PopSelf();
                Open(ctx, other);
            }
        }
    }

    /** @brief The right button on the map explains it. */
    void Rightpressed(int x, int y, int screen_x, int screen_y) override {
        w2::UiContext& ctx = Ctx();
        if (kMap.Contains(x, y)) {
            infobox::ShowLines(ctx, screen_x, screen_y, kit::Text(ctx, 0x7a, 2),
                               kit::Text(ctx, 0x7a, 3));
        }
    }

    /** @brief Enter or Escape closes the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View dialog_view_;  ///< The dialog's controls; only Done is shown.
    w2::Site* site_ = nullptr;  ///< The ruin or temple shown.
};

}  // namespace

/**
 * @brief The site nearest a map tile among those the viewing side has seen.
 *
 * With no viewing side every site counts. Distances are whole tiles, rounded
 * down; the first site at the least distance wins.
 */
w2::Site* Nearest(w2::UiContext& ctx, int x, int y) {
    w2::Site* best = nullptr;
    int best_distance = w2::kNone;
    w2::Game& game = ctx.ActiveGame();
    w2::Side* player = ctx.Player();
    int viewer = w2::game::ViewerSide(game, player);
    for (auto& site : game.GetMap().Sites()) {
        if (viewer == w2::kNone ||
            (w2::site::ShownTo(site, *player) && w2::game::Seen(game, viewer, site.x, site.y))) {
            double dx = site.x - x;
            double dy = site.y - y;
            int distance = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
            if (best_distance == w2::kNone || distance < best_distance) {
                best = &site;
                best_distance = distance;
            }
        }
    }
    return best;
}

/** @brief Opens the site dialog on `site`; nothing when there is none. */
void Open(w2::UiContext& ctx, w2::Site* site) {
    if (!site) return;
    kit::Push(ctx, std::make_shared<RuinModal>(ctx, site));
}

}  // namespace ruin
