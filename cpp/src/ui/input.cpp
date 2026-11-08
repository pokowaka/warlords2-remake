// The original's one text-entry dialog, used for renaming a city and for
// anything else that asks for a line of text (7b4c:0000).
//
// Popup 1, (160, 90) 320x200: the title in font 1 centred on x = 320 at
// y = 92, the prompt in font 2, and the field at (184, 197) 256x20 over
// dialog 5: OK (189), Cancel (190) and the field's own hit area (191).
// Clicking the field starts an edit from an EMPTY line (7b4c:03a6); a
// character from 0x20 to 0x7a is taken while the line is short and narrow
// enough; Backspace takes one back; Enter keeps it, Escape puts the old text
// back. The cursor is a "`", the font's own, blinking between colours 15 and
// 9. The same dialog asks yes-or-no questions (7b4c:0088): no field.
#include <cmath>
#include <map>

#include "ui/dialogs.hpp"

namespace input {

namespace {
const kit::Rect kR{160, 90, 320, 200};  // popup 1
const int kDialog = 5;
const int kOk = 189, kCancel = 190, kField = 191;
const int kTitleY = 92;
// Where the prompt goes, by how many lines there are (4125:2316). The
// text-entry form counts two more lines than it has, for the field.
const std::map<int, std::vector<int>> kRows = {
    {1, {150}}, {2, {150, 173}}, {3, {150, 173, 196}}, {4, {140, 163, 186, 209}}};
const kit::Rect kBox{184, 197, 256, 20};
const double kBlink = 0.25;

struct Dialog : kit::Modal {
    Options opts;
    screen::View v;
    std::string value;
    std::optional<Editor> editing;

    void Finish(bool ok) {
        auto self = kit::top();
        (void)self;
        Options o = opts;
        std::string t = value;
        kit::pop(this);
        if (ok) {
            if (o.ok) o.ok(t);
        } else if (o.cancelled)
            o.cancelled();
    }

    void draw() override {
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), opts.title, 320, kTitleY);
        int n = static_cast<int>(opts.lines.size()) + (opts.confirm ? 0 : 2);
        auto ys = kRows.find(n);
        if (ys != kRows.end())
            for (size_t i = 0; i < opts.lines.size() && i < ys->second.size(); i++)
                kit::centred(kit::font(2), opts.lines[i], 320, ys->second[i]);
        if (opts.confirm) {
            std::set<int> h{kField};
            kit::drawControls(v, h);
            return;
        }
        kit::field(kBox.x, kBox.y, kBox.w, kBox.h, editing ? editing->text : value, &kit::font(2));
        if (editing) editing->drawCursor(kBox.x, kBox.y);
        kit::drawControls(v);
    }

    void mousepressed(int x, int y, int) override {
        if (editing) {
            value = editing->text;
            editing.reset();
        }
        std::set<int> h;
        if (opts.confirm) h.insert(kField);
        auto c = kit::controlAt(v, x, y, h);
        if (!c) return;
        if (c->id == kOk)
            Finish(true);
        else if (c->id == kCancel)
            Finish(false);
        else if (c->id == kField) {
            editing = Editor();
            editing->maxChars = opts.maxChars;
            editing->maxWidth = opts.maxWidth;
        }
    }

    void keypressed(const std::string& key) override {
        if (editing) {
            std::string r = editing->key(key);
            if (r == "keep") {
                value = editing->text;
                editing.reset();
            } else if (r == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter")
            Finish(true);
        else if (key == "escape" && v.stateOf(kCancel) != w2::uidata::DISABLED)
            Finish(false);
    }

    void textinput(const std::string& t) override {
        if (editing) editing->input(t);
    }
    bool typing() const override { return editing.has_value(); }
};
}  // namespace

std::string Editor::key(const std::string& k) {
    if (k == "return" || k == "kpenter") return "keep";
    if (k == "escape") return "undo";
    if (k == "backspace" && !text.empty()) text.pop_back();
    return "";
}

void Editor::input(const std::string& t) {
    for (unsigned char b : t) {
        if (b >= 0x20 && b < 0x7b && static_cast<int>(text.size()) < maxChars - 1 &&
            kit::font(2).width(text) < maxWidth)
            text += static_cast<char>(b);
    }
}

void Editor::drawCursor(double x, double y) const {
    const Font& f = kit::font(2);
    if (static_cast<long>(std::floor(kit::now() / kBlink)) % 2 == 0)
        kit::setPal(9);
    else
        gfx::setColor(1, 1, 1);
    f.draw("`", x + f.width(text) + 5, y + 2);
}

void open(const Options& opts) {
    auto d = std::make_shared<Dialog>();
    d->opts = opts;
    d->v = kit::view(kDialog);
    d->view = &d->v;
    d->value = opts.text;
    d->v.state[kOk] = w2::uidata::NORMAL;
    d->v.state[kCancel] = opts.cancel ? w2::uidata::NORMAL : w2::uidata::DISABLED;
    kit::push(d);
}

}  // namespace input
