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

#include "ui/dialogs.hpp"
#include "warlords/site.hpp"

namespace ruin {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2, the whole dialog.
const kit::Rect kMap{80, 60, 224, 312};        ///< The strategic map on its left.
const int kDialog = 6;                         ///< BUTTON.DAT dialog number.
const int kDone = 192;                         ///< The Done control.
/// SPECBITS.PCK cells (left, top), 96x63: the temple first, then five ruins.
const int kPictures[6][2] = {{0, 0}, {96, 0}, {192, 0}, {0, 63}, {96, 63}, {192, 63}};
/// The legend's four map markers: their cells in the shield sheet.
const int kLegend[4][2] = {{112, 0}, {112, 10}, {112, 20}, {128, 0}};
/// Where each legend marker is drawn; its line of text follows it.
const int kLegendAt[4][2] = {{320, 188}, {432, 188}, {320, 208}, {432, 208}};

/**
 * @brief The site dialog: one ruin or temple, with the map of all the sites.
 */
struct Ruin : kit::Modal {
    screen::View dialog_view;  ///< The dialog's controls; only Done is shown.
    w2::Site* site = nullptr;  ///< The ruin or temple shown.
    /** @brief Draws the map, the site's picture, its state, legend and text. */
    void Draw() override {
        kit::Popup(kPopupRect);
        front::DrawStrategicMap(kMap.x, kMap.y);
        front::DrawSiteMarkers(kMap.x, kMap.y, site);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), site->name, 432, 62);
        kit::Bevel(326, 102, 100, 67, 4, 2);
        kit::SetPal(0);
        kit::Outline(327, 103, 98, 65);
        const int* pic = kPictures[site->content == w2::site::kTemple ? 0 : (site->index % 5) + 1];
        if (auto art = g_app.screen->ArtFor(6)) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(pic[0], pic[1], 96, 63), 328, 104);
        }
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        font.Draw(kit::Text(0x71, site->content), 432, 112);
        font.Draw(kit::Text(0x72, site->searched ? 1 : 0), 432, 136);
        kit::SetPal(2);
        gfx::Rectangle(gfx::kFill, 312, 180, 240, 48);
        kit::SetPal(0);
        kit::Outline(312, 180, 240, 48);
        for (int i = 0; i < 4; i++) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.atrans_shields, gfx::NewQuad(kLegend[i][0], kLegend[i][1], 16, 10),
                      kLegendAt[i][0], kLegendAt[i][1]);
            font.Draw(kit::Text(0x73, i), kLegendAt[i][0] + 16, kLegendAt[i][1] - 3);
        }
        auto description = g_app.g->map->site_text.find(site->index);
        if (description != g_app.g->map->site_text.end())
            for (int i = 0; i < 3; i++) font.Draw(description->second[i], 310, 259 + 20 * i);
        kit::DrawControls(dialog_view, hidden);
    }
    /** @brief Done closes; a click on the map moves to the site nearest it. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (control && control->id == kDone) {
            kit::Pop(this);
            return;
        }
        if (!control && kMap.Contains(x, y)) {
            w2::Site* other = Nearest((x - kMap.x) / 2, (y - kMap.y) / 2);
            if (other && other != site) {
                kit::Pop(this);
                Open(other);
            }
        }
    }
    /** @brief The right button on the map explains it. */
    void Rightpressed(int x, int y, int screen_x, int screen_y) override {
        if (kMap.Contains(x, y))
            infobox::ShowLines(screen_x, screen_y, kit::Text(0x7a, 2), kit::Text(0x7a, 3));
    }
    /** @brief Enter or Escape closes the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief The site nearest a map tile among those the viewing side has seen.
 *
 * With no viewing side every site counts. Distances are whole tiles, rounded
 * down; the first site at the least distance wins.
 */
w2::Site* Nearest(int x, int y) {
    w2::Site* best = nullptr;
    int best_distance = w2::kNone;
    int viewer = w2::game::ViewerSide(*g_app.g, g_app.player);
    for (auto& site : g_app.g->map->sites) {
        if (viewer == w2::kNone || (w2::site::ShownTo(site, *g_app.player) &&
                                    w2::game::Seen(*g_app.g, viewer, site.x, site.y))) {
            double dx = site.x - x, dy = site.y - y;
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
void Open(w2::Site* site) {
    if (!site) return;
    auto dialog = std::make_shared<Ruin>();
    dialog->site = site;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kDone] = w2::uidata::kNormal;
    for (auto& control : dialog->dialog_view.dialog.controls)
        if (control.id != kDone) dialog->hidden.insert(control.id);
    kit::Push(dialog);
}

}  // namespace ruin
