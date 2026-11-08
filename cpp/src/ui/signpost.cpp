// Order > Signpost (540d:01a4): rewrite the sign the selected stack stands on.
//
// Only on a tile of terrain 9 that has a sign. Popup 1, (160, 90) 320x200:
// "A Signpost!" in font 1, two lines of instruction in font 2, and the
// sign's two lines in fields at (200, 190) and (200, 215), 240x22, over
// dialog 24: Done (421) and the two fields' hit areas (422, 423). Clicking a
// field types a new line into it, up to 29 characters and 216 pixels.
#include "ui/dialogs.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"

namespace signpost {

namespace {
const kit::Rect kR{160, 90, 320, 200};  // popup 1
const int kDialog = 24, kDone = 421, kLinE1 = 422, kLinE2 = 423;
const int kFieldX[2] = {200, 200}, kFieldY[2] = {190, 215};
const int kFieldW = 240, kFieldH = 22;
const int kMaxChars = 30, kMaxWidth = 216;

struct Sign : kit::Modal {
    screen::View v;
    w2::Sign* sign = nullptr;
    std::optional<input::Editor> editing;
    int line = 0;
    void Keep() {
        if (editing) {
            sign->lines[line] = editing->text;
            editing.reset();
        }
    }
    void draw() override {
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), "A Signpost!", 320, 94);  // 4125:03eb
        kit::centred(kit::font(2), "Type the new message for", 320, 140);
        kit::centred(kit::font(2), "this signpost!", 320, 160);
        for (int i = 0; i < 2; i++) {
            const std::string& text = (editing && line == i) ? editing->text : sign->lines[i];
            kit::field(kFieldX[i], kFieldY[i], kFieldW, kFieldH, text, &kit::font(2));
            if (editing && line == i) editing->drawCursor(kFieldX[i], kFieldY[i]);
        }
        std::set<int> h{kLinE1, kLinE2};
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        Keep();
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kDone) {
            kit::pop(this);
        } else if (c->id == kLinE1 || c->id == kLinE2) {
            line = c->id - kLinE1;
            editing = input::Editor();
            editing->maxChars = kMaxChars;
            editing->maxWidth = kMaxWidth;
        }
    }
    void keypressed(const std::string& key) override {
        if (editing) {
            std::string r = editing->key(key);
            if (r == "keep")
                Keep();
            else if (r == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
    void textinput(const std::string& t) override {
        if (editing) editing->input(t);
    }
    bool typing() const override { return editing.has_value(); }
};
}  // namespace

void open(const std::vector<w2::Army*>& stack) {
    if (stack.empty()) return;
    w2::Army* a = stack[0];
    if (w2::scn::terrainAt(*G.g->map, a->x, a->y) != w2::move::TOWER) return;
    w2::Sign* sign = w2::game::signAt(*G.g, a->x, a->y);
    if (!sign) return;
    auto d = std::make_shared<Sign>();
    d->sign = sign;
    d->v = kit::view(kDialog);
    d->view = &d->v;
    for (int id : {kDone, kLinE1, kLinE2}) d->v.state[id] = w2::uidata::NORMAL;
    kit::push(d);
}

}  // namespace signpost
