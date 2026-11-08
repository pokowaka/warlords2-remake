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
    return level == 1 ? 15 : level == 2 ? 30 : level == 3 ? 60 : w2::kNone;
}

struct Levels : kit::Modal {
    screen::View v;
    std::vector<w2::Army*> rows;
    void Draw() override {
        w2::Side* side = G.player;
        const Font& f = kit::GetFont(2);
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kS, 0), 320, 63);
        const Font& head = f.Colours(side->colour, side->edge);
        head.Draw(kit::Text(kS, 1), 128, 105);
        head.Draw(kit::Text(kS, 2), 272, 105);
        const int xs[4] = {392, 440, 488, 536};
        for (int k = 0; k < 4; k++) kit::Centred(head, kit::Text(kS, k + 3), xs[k], 105);
        for (size_t i = 0; i < rows.size(); i++) {
            w2::Army* h = rows[i];
            int y = 128 + 30 * static_cast<int>(i);
            int level = h->level ? h->level : 1;
            kit::Army(w2::armytype::kHero, side->index, 88, y, level == 4 ? side->index + 2 : 1);
            gfx::SetColor(1, 1, 1);
            f.Draw(h->name, 128, y + 6);
            f.Draw(kit::Text(h->female ? kSFemale : kSMale, level - 1), 272, y + 6);
            kit::Centred(f, w2::Fmt("%d", h->experience), 392, y + 6);
            kit::Centred(f, Needs(level) != w2::kNone ? w2::Fmt("%d", Needs(level)) : "-", 440,
                         y + 6);
            kit::Centred(f, w2::Fmt("%d", h->strength), 488, y + 6);
            kit::Centred(f, w2::Fmt("%d", h->max_moves), 536, y + 6);
        }
        for (int i = static_cast<int>(rows.size()); i < 6; i++)
            kit::Army(w2::kNone, side->index, 88, 128 + 30 * i, 1);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (c && c->id == kDone) kit::Pop(this);
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::Pop(this);
    }
};
}  // namespace

void Open() {
    auto d = std::make_shared<Levels>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    for (int level = 4; level >= 1; level--) {
        for (int i = static_cast<int>(G.g->armies.size()) - 1; i >= 0; i--) {
            w2::Army* a = G.g->armies[i];
            if (a->Hero() && a->owner == G.player->index && (a->level ? a->level : 1) == level)
                d->rows.push_back(a);
        }
    }
    kit::Push(d);
}

}  // namespace levels
