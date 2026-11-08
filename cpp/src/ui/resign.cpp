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
    return kit::text(kS, i);
}

void Burn(kit::Done after) {
    w2::game::resign(*G.g, *G.player);
    G.selection.reset();
    front::stratDirty();
    if (after) after();
}

struct Resign : kit::Modal {
    screen::View v;
    void draw() override {
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), T(0), 320, 92);
        for (int i = 1; i <= 3; i++) kit::centred(kit::font(2), T(i), 320, 120 + 20 * i);
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kCancel) {
            kit::pop(this);
        } else if (c->id == kGracious) {
            kit::pop(this);
            search::say(T(6), []() {
                search::say(T(7), []() {
                    search::say(T(8),
                                []() { search::message(T(9), T(10), []() { Burn(nullptr); }); });
                });
            });
        } else if (c->id == kResign) {
            kit::pop(this);
            Burn([]() { search::message(T(4), T(5)); });
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};
}  // namespace

void open() {
    if (w2::game::sideCities(*G.g, *G.player).empty()) return;  // 4125:5dea
    auto d = std::make_shared<Resign>();
    d->v = kit::view(kDialog);
    d->view = &d->v;
    for (int id : {kGracious, kResign, kCancel}) d->v.state[id] = w2::uidata::NORMAL;
    kit::push(d);
}

}  // namespace resign
