// The single-line text-entry and confirmation dialog, used for renaming a city
// and for yes-or-no prompts.
//
// Popup 1, (160, 90) 320x200: the title in font 1 centred on x = 320 at
// y = 92, the prompt in font 2, and the field at (184, 197) 256x20 over
// dialog 5: OK (189), Cancel (190) and the field's own hit area (191).
// Clicking the field starts an edit from an empty line; printable ASCII from
// 0x20 to 0x7a is accepted while the line is short and narrow enough;
// Backspace deletes a character; Enter keeps it, Escape restores the old text.
// The cursor is a "`" in the font, blinking between colours 15 and 9.
// In confirmation mode (`confirm = true`), the text field is hidden.
#include <cmath>
#include <map>

#include "ui/dialogs.hpp"

namespace input {

namespace {
const kit::Rect kPopupRect{160, 90, 320, 200};  // popup 1
const int kDialog = 5;
const int kOk = 189, kCancel = 190, kField = 191;
const int kTitleY = 92;
// Prompt row y-positions by line count. The text-entry form counts two extra
// lines for the input field.
const std::map<int, std::vector<int>> kRows = {
    {1, {150}}, {2, {150, 173}}, {3, {150, 173, 196}}, {4, {140, 163, 186, 209}}};
const kit::Rect kBox{184, 197, 256, 20};
const double kBlink = 0.25;

/**
 * @brief Modal implementation for text input and confirmation prompts.
 */
struct Dialog : kit::Modal {
    Options opts;                   ///< Dialog options and callbacks.
    screen::View dialog_view;       ///< Dialog 5 control states.
    std::string value;              ///< Committed text value.
    std::optional<Editor> editing;  ///< Active editor while typing into the field.

    /** @brief Closes the dialog and invokes the appropriate callback. */
    void Finish(bool confirmed) {
        auto self = kit::Top();
        (void)self;
        Options options = opts;
        std::string text_value = value;
        kit::Pop(this);
        if (confirmed) {
            if (options.ok) options.ok(text_value);
        } else if (options.cancelled) {
            options.cancelled();
        }
    }

    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), opts.title, 320, kTitleY);
        int line_count = static_cast<int>(opts.lines.size()) + (opts.confirm ? 0 : 2);
        auto row_it = kRows.find(line_count);
        if (row_it != kRows.end())
            for (size_t i = 0; i < opts.lines.size() && i < row_it->second.size(); i++)
                kit::Centred(kit::GetFont(2), opts.lines[i], 320, row_it->second[i]);
        if (opts.confirm) {
            std::set<int> hidden_ids{kField};
            kit::DrawControls(dialog_view, hidden_ids);
            return;
        }
        kit::Field(kBox.x, kBox.y, kBox.w, kBox.h, editing ? editing->text : value,
                   &kit::GetFont(2));
        if (editing) editing->DrawCursor(kBox.x, kBox.y);
        kit::DrawControls(dialog_view);
    }

    void Mousepressed(int x, int y, int) override {
        if (editing) {
            value = editing->text;
            editing.reset();
        }
        std::set<int> hidden_ids;
        if (opts.confirm) hidden_ids.insert(kField);
        auto control = kit::ControlAt(dialog_view, x, y, hidden_ids);
        if (!control) return;
        if (control->id == kOk)
            Finish(true);
        else if (control->id == kCancel)
            Finish(false);
        else if (control->id == kField) {
            editing = Editor();
            editing->max_chars = opts.max_chars;
            editing->max_width = opts.max_width;
        }
    }

    void Keypressed(const std::string& key) override {
        if (editing) {
            std::string action = editing->Key(key);
            if (action == "keep") {
                value = editing->text;
                editing.reset();
            } else if (action == "undo") {
                editing.reset();
            }
            return;
        }
        if (key == "return" || key == "kpenter")
            Finish(true);
        else if (key == "escape" && dialog_view.StateOf(kCancel) != w2::uidata::kDisabled)
            Finish(false);
    }

    void Textinput(const std::string& typed) override {
        if (editing) editing->Input(typed);
    }
    bool Typing() const override { return editing.has_value(); }
};
}  // namespace

std::string Editor::Key(const std::string& key) {
    if (key == "return" || key == "kpenter") return "keep";
    if (key == "escape") return "undo";
    if (key == "backspace" && !text.empty()) text.pop_back();
    return "";
}

void Editor::Input(const std::string& typed) {
    for (unsigned char byte : typed) {
        if (byte >= 0x20 && byte < 0x7b && static_cast<int>(text.size()) < max_chars - 1 &&
            kit::GetFont(2).Width(text) < max_width)
            text += static_cast<char>(byte);
    }
}

void Editor::DrawCursor(double x, double y) const {
    const Font& font = kit::GetFont(2);
    if (static_cast<long>(std::floor(kit::Now() / kBlink)) % 2 == 0)
        kit::SetPal(9);
    else
        gfx::SetColor(1, 1, 1);
    font.Draw("`", x + font.Width(text) + 5, y + 2);
}

void Open(const Options& opts) {
    auto dialog = std::make_shared<Dialog>();
    dialog->opts = opts;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->value = opts.text;
    dialog->dialog_view.state[kOk] = w2::uidata::kNormal;
    dialog->dialog_view.state[kCancel] = opts.cancel ? w2::uidata::kNormal : w2::uidata::kDisabled;
    kit::Push(dialog);
}

}  // namespace input
