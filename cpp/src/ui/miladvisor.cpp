// The Military Advisor (military_advisor, 67cc:1f19): Shift and a click on an
// enemy beside the selected stack asks how the fight would go.
//
// Popup 12, (176, 60) 288x300, with dialog 25's one button, 424. "Advisor!"
// in font 1, ADVISOR.PCK at (256, 117), and in font 2 a random line of group
// 124, one of group 125, and the verdict -- group 126's line wins / 2 out of
// 19 battles fought in secret, on the game's own dice as the original's are.
#include "ui/dialogs.hpp"
#include "warlords/combat.hpp"

namespace miladvisor {

namespace {
const kit::Rect kR{176, 60, 288, 300};  // popup 12
const int kDialog = 25, kOk = 424;
const int kTitle = 0x7b, kHail = 0x7c, kBattle = 0x7d, kVerdict = 0x7e;
const int kPicture = 48;  // ADVISOR.PCK

struct Advice : kit::Modal {
    screen::View v;
    std::string hail, battle, verdict;
    void Draw() override {
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kTitle, 0), 320, 67);
        if (auto art = G.screen->ArtFor(kPicture)) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(0, 0, 128, 130), 256, 117);
        }
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f, hail, 320, 258);
        kit::Centred(f, battle, 320, 278);
        kit::Centred(f, verdict, 320, 298);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (c && c->id == kOk) kit::Pop(this);
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

void Open(const std::vector<w2::Army*>* stack, int x, int y) {
    w2::Game& g = *G.g;
    if (g.map->options.military_advisor == 0 || !stack || stack->empty()) return;
    auto l = w2::combat::BattleLines(g, *stack, x, y);
    int wins = w2::combat::Advise(g, l.attackers, l.defenders, x, y).second;
    auto d = std::make_shared<Advice>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->hail = kit::Text(kHail, g.rng.Dice(1, 5, -1));
    d->battle = kit::Text(kBattle, g.rng.Dice(1, 5, -1));
    d->verdict = kit::Text(kVerdict, wins / 2);
    d->v.state[kOk] = w2::uidata::kNormal;
    kit::Push(d);
}

}  // namespace miladvisor
