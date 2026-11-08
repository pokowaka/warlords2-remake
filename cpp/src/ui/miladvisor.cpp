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
    void draw() override {
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(kTitle, 0), 320, 67);
        if (auto art = G.screen->artFor(kPicture)) {
            gfx::setColor(1, 1, 1);
            gfx::draw(art->image, gfx::newQuad(0, 0, 128, 130), 256, 117);
        }
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::centred(f, hail, 320, 258);
        kit::centred(f, battle, 320, 278);
        kit::centred(f, verdict, 320, 298);
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (c && c->id == kOk) kit::pop(this);
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};
}  // namespace

void open(const std::vector<w2::Army*>* stack, int x, int y) {
    w2::Game& g = *G.g;
    if (g.map->options.militaryAdvisor == 0 || !stack || stack->empty()) return;
    auto l = w2::combat::lines(g, *stack, x, y);
    int wins = w2::combat::advise(g, l.attackers, l.defenders, x, y).second;
    auto d = std::make_shared<Advice>();
    d->v = kit::view(kDialog);
    d->view = &d->v;
    d->hail = kit::text(kHail, g.rng.dice(1, 5, -1));
    d->battle = kit::text(kBattle, g.rng.dice(1, 5, -1));
    d->verdict = kit::text(kVerdict, wins / 2);
    d->v.state[kOk] = w2::uidata::NORMAL;
    kit::push(d);
}

}  // namespace miladvisor
