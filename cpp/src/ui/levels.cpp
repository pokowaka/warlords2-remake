// Hero > Levels (auto_ui_hero_levels, 7563:1652).
//
// Popup 0, (80, 60) 480x320, and dialog 28: Done (469). "Hero Levels" in
// font 1, the column heads in the side's colours at y = 105, and a row per
// hero, 30 apart from y = 128, highest level first: the hero on a ring --
// its side's colour at the top level, grey below -- its name, its title, its
// experience, what the next level needs, its strength and its moves.
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"

namespace levels {

namespace {
const kit::Rect kR{80, 60, 480, 320};  // popup 0
const int kDialog = 28, kDone = 469;
const int kS = 0x68, kSMale = 0x63, kSFemale = 0x64;

int Needs(int level) {
    return level == 1 ? 15 : level == 2 ? 30 : level == 3 ? 60 : w2::NONE;
}

struct Levels : kit::Modal {
    screen::View v;
    std::vector<w2::Army*> rows;
    void draw() override {
        w2::Side* side = G.player;
        const Font& f = kit::font(2);
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(kS, 0), 320, 63);
        const Font& head = f.colours(side->colour, side->edge);
        head.draw(kit::text(kS, 1), 128, 105);
        head.draw(kit::text(kS, 2), 272, 105);
        const int xs[4] = {392, 440, 488, 536};
        for (int k = 0; k < 4; k++) kit::centred(head, kit::text(kS, k + 3), xs[k], 105);
        for (size_t i = 0; i < rows.size(); i++) {
            w2::Army* h = rows[i];
            int y = 128 + 30 * static_cast<int>(i);
            int level = h->level ? h->level : 1;
            kit::army(w2::armytype::HERO, side->index, 88, y, level == 4 ? side->index + 2 : 1);
            gfx::setColor(1, 1, 1);
            f.draw(h->name, 128, y + 6);
            f.draw(kit::text(h->female ? kSFemale : kSMale, level - 1), 272, y + 6);
            kit::centred(f, w2::fmt("%d", h->experience), 392, y + 6);
            kit::centred(f, Needs(level) != w2::NONE ? w2::fmt("%d", Needs(level)) : "-", 440,
                         y + 6);
            kit::centred(f, w2::fmt("%d", h->strength), 488, y + 6);
            kit::centred(f, w2::fmt("%d", h->maxMoves), 536, y + 6);
        }
        for (int i = static_cast<int>(rows.size()); i < 6; i++)
            kit::army(w2::NONE, side->index, 88, 128 + 30 * i, 1);
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (c && c->id == kDone) kit::pop(this);
    }
    void keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::pop(this);
    }
};
}  // namespace

void open() {
    auto d = std::make_shared<Levels>();
    d->v = kit::view(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::NORMAL;
    for (int level = 4; level >= 1; level--) {
        for (int i = static_cast<int>(G.g->armies.size()) - 1; i >= 0; i--) {
            w2::Army* a = G.g->armies[i];
            if (a->hero() && a->owner == G.player->index && (a->level ? a->level : 1) == level)
                d->rows.push_back(a);
        }
    }
    kit::push(d);
}

}  // namespace levels
