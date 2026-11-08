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
        auto self = kit::Top();
        (void)self;
        Options o = opts;
        std::string t = value;
        kit::Pop(this);
        if (ok) {
            if (o.ok) o.ok(t);
        } else if (o.cancelled)
            o.cancelled();
    }

    void Draw() override {
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), opts.title, 320, kTitleY);
        int n = static_cast<int>(opts.lines.size()) + (opts.confirm ? 0 : 2);
        auto ys = kRows.find(n);
        if (ys != kRows.end())
            for (size_t i = 0; i < opts.lines.size() && i < ys->second.size(); i++)
                kit::Centred(kit::GetFont(2), opts.lines[i], 320, ys->second[i]);
        if (opts.confirm) {
            std::set<int> h{kField};
            kit::DrawControls(v, h);
            return;
        }
        kit::Field(kBox.x, kBox.y, kBox.w, kBox.h, editing ? editing->text : value,
                   &kit::GetFont(2));
        if (editing) editing->DrawCursor(kBox.x, kBox.y);
        kit::DrawControls(v);
    }

    void Mousepressed(int x, int y, int) override {
        if (editing) {
            value = editing->text;
            editing.reset();
        }
        std::set<int> h;
        if (opts.confirm) h.insert(kField);
        auto c = kit::ControlAt(v, x, y, h);
        if (!c) return;
        if (c->id == kOk)
            Finish(true);
        else if (c->id == kCancel)
            Finish(false);
        else if (c->id == kField) {
            editing = Editor();
            editing->max_chars = opts.max_chars;
            editing->max_width = opts.max_width;
        }
    }

    void Keypressed(const std::string& key) override {
        if (editing) {
            std::string r = editing->Key(key);
            if (r == "keep") {
                value = editing->text;
                editing.reset();
            } else if (r == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter")
            Finish(true);
        else if (key == "escape" && v.StateOf(kCancel) != w2::uidata::kDisabled)
            Finish(false);
    }

    void Textinput(const std::string& t) override {
        if (editing) editing->Input(t);
    }
    bool Typing() const override { return editing.has_value(); }
};
}  // namespace

std::string Editor::Key(const std::string& k) {
    if (k == "return" || k == "kpenter") return "keep";
    if (k == "escape") return "undo";
    if (k == "backspace" && !text.empty()) text.pop_back();
    return "";
}

void Editor::Input(const std::string& t) {
    for (unsigned char b : t) {
        if (b >= 0x20 && b < 0x7b && static_cast<int>(text.size()) < max_chars - 1 &&
            kit::GetFont(2).Width(text) < max_width)
            text += static_cast<char>(b);
    }
}

void Editor::DrawCursor(double x, double y) const {
    const Font& f = kit::GetFont(2);
    if (static_cast<long>(std::floor(kit::Now() / kBlink)) % 2 == 0)
        kit::SetPal(9);
    else
        gfx::SetColor(1, 1, 1);
    f.Draw("`", x + f.Width(text) + 5, y + 2);
}

void Open(const Options& opts) {
    auto d = std::make_shared<Dialog>();
    d->opts = opts;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->value = opts.text;
    d->v.state[kOk] = w2::uidata::kNormal;
    d->v.state[kCancel] = opts.cancel ? w2::uidata::kNormal : w2::uidata::kDisabled;
    kit::Push(d);
}

}  // namespace input
