// Order > Resign (7721:150d).
//
// Popup 1, (160, 90) 320x200: "Resign!" (group 165) in font 1 centred on
// (320, 92), and three lines of font 2. Dialog 33: 487 resigns graciously,
// 488 resigns, 489 thinks better of it (default and cancel both). Either way
// the side's cities burn and its armies go (7721:1608). The gracious way is
// asked three times first; the other is told afterwards what has burned.
#include "ui/dialogs.hpp"

namespace resign {

namespace {
const kit::Rect kR{160, 90, 320, 200};  // popup 1
const int kDialog = 33, kGracious = 487, kResign = 488, kCancel = 489;
const int kS = 0xa5;
std::string T(int i) {
    return kit::Text(kS, i);
}

void Burn(kit::Done after) {
    w2::game::Resign(*G.g, *G.player);
    G.selection.reset();
    front::StratDirty();
    if (after) after();
}

struct Resign : kit::Modal {
    screen::View v;
    void Draw() override {
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), T(0), 320, 92);
        for (int i = 1; i <= 3; i++) kit::Centred(kit::GetFont(2), T(i), 320, 120 + 20 * i);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kCancel) {
            kit::Pop(this);
        } else if (c->id == kGracious) {
            kit::Pop(this);
            search::Say(T(6), []() {
                search::Say(T(7), []() {
                    search::Say(T(8), []() {
                        search::ShowMessage(T(9), T(10), []() { Burn(nullptr); });
                    });
                });
            });
        } else if (c->id == kResign) {
            kit::Pop(this);
            Burn([]() { search::ShowMessage(T(4), T(5)); });
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

void Open() {
    if (w2::game::SideCities(*G.g, *G.player).empty()) return;  // 4125:5dea
    auto d = std::make_shared<Resign>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    for (int id : {kGracious, kResign, kCancel}) d->v.state[id] = w2::uidata::kNormal;
    kit::Push(d);
}

}  // namespace resign
