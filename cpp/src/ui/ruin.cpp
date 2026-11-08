// View > Ruins (`.`): the city dialog's fifth mode, on a ruin or temple.
//
// 7204:0000 with mode 4 at the cursor takes the nearest site shown to the
// side (828e:06fd). Dialog 6 over popup 2 with only Done (192); the map with
// every site shown marked and this one boxed; and 7204:0cdf's case 4: the
// name, SPECBITS.PCK's picture, "Type: ..." and "Explored: ...", the map
// markers' legend, and the site's three lines of .SPC description.
#include <cmath>

#include "ui/dialogs.hpp"
#include "warlords/site.hpp"

namespace ruin {

namespace {
const kit::Rect kR{80, 60, 480, 312};  // popup 2
const kit::Rect kMap{80, 60, 224, 312};
const int kDialog = 6, kDone = 192;
const int kPictures[6][2] = {{0, 0}, {96, 0}, {192, 0}, {0, 63}, {96, 63}, {192, 63}};
const int kLegend[4][2] = {{112, 0}, {112, 10}, {112, 20}, {128, 0}};  // 4125:0f28
const int kLegendAt[4][2] = {{320, 188}, {432, 188}, {320, 208}, {432, 208}};

struct Ruin : kit::Modal {
    screen::View v;
    w2::Site* s = nullptr;
    void Draw() override {
        kit::Popup(kR);
        front::DrawStrategicMap(kMap.x, kMap.y);
        front::DrawSiteMarkers(kMap.x, kMap.y, s);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), s->name, 432, 62);
        kit::Bevel(326, 102, 100, 67, 4, 2);
        kit::SetPal(0);
        kit::Outline(327, 103, 98, 65);
        const int* pic = kPictures[s->content == w2::site::kTemple ? 0 : (s->index % 5) + 1];
        if (auto art = G.screen->ArtFor(6)) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(pic[0], pic[1], 96, 63), 328, 104);
        }
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        f.Draw(kit::Text(0x71, s->content), 432, 112);
        f.Draw(kit::Text(0x72, s->searched ? 1 : 0), 432, 136);
        kit::SetPal(2);
        gfx::Rectangle(gfx::kFill, 312, 180, 240, 48);
        kit::SetPal(0);
        kit::Outline(312, 180, 240, 48);
        for (int i = 0; i < 4; i++) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.atrans_shields, gfx::NewQuad(kLegend[i][0], kLegend[i][1], 16, 10),
                      kLegendAt[i][0], kLegendAt[i][1]);
            f.Draw(kit::Text(0x73, i), kLegendAt[i][0] + 16, kLegendAt[i][1] - 3);
        }
        auto it = G.g->map->site_text.find(s->index);
        if (it != G.g->map->site_text.end())
            for (int i = 0; i < 3; i++) f.Draw(it->second[i], 310, 259 + 20 * i);
        kit::DrawControls(v, hidden);
    }
    // a click on the map moves to the site nearest it (7204:1afa)
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (c && c->id == kDone) {
            kit::Pop(this);
            return;
        }
        if (!c && kMap.Contains(x, y)) {
            w2::Site* other = Nearest((x - kMap.x) / 2, (y - kMap.y) / 2);
            if (other && other != s) {
                kit::Pop(this);
                Open(other);
            }
        }
    }
    void Rightpressed(int x, int y, int sx, int sy) override {
        if (kMap.Contains(x, y)) infobox::ShowLines(sx, sy, kit::Text(0x7a, 2), kit::Text(0x7a, 3));
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

w2::Site* Nearest(int x, int y) {
    w2::Site* best = nullptr;
    int best_d = w2::kNone;
    int viewer = w2::game::ViewerSide(*G.g, G.player);
    for (auto& s : G.g->map->sites) {
        if (viewer == w2::kNone ||
            (w2::site::ShownTo(s, *G.player) && w2::game::Seen(*G.g, viewer, s.x, s.y))) {
            double dx = s.x - x, dy = s.y - y;
            int dd = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
            if (best_d == w2::kNone || dd < best_d) {
                best = &s;
                best_d = dd;
            }
        }
    }
    return best;
}

void Open(w2::Site* s) {
    if (!s) return;
    auto d = std::make_shared<Ruin>();
    d->s = s;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    for (auto& c : d->v.dialog.controls)
        if (c.id != kDone) d->hidden.insert(c.id);
    kit::Push(d);
}

}  // namespace ruin
