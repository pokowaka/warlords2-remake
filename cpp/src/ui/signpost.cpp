/**
 * @file
 * @brief Order > Signpost: rewrite the sign the selected stack stands on.
 *
 * Only on a tile of terrain 9 that has a sign. Popup 1, (160, 90) 320x200:
 * "A Signpost!" in font 1, two lines of instruction in font 2, and the
 * sign's two lines in fields at (200, 190) and (200, 215), 240x22, over
 * dialog 24: Done (421) and the two fields' hit areas (422, 423). Clicking a
 * field types a new line into it, up to 29 characters and 216 pixels.
 */
#include "ui/dialogs.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"

namespace signpost {

namespace {
const kit::Rect kPopupRect{160, 90, 320, 200};  ///< Popup 1, the whole dialog.
const int kDialog = 24;                         ///< BUTTON.DAT dialog number.
const int kDone = 421;                          ///< The Done control.
const int kLine1 = 422;                         ///< The first field's hit area.
const int kLine2 = 423;                         ///< The second field's hit area.
const int kFieldX[2] = {200, 200};              ///< Left edges of the two fields.
const int kFieldY[2] = {190, 215};              ///< Top edges of the two fields.
const int kFieldW = 240;                        ///< Width of a field.
const int kFieldH = 22;                         ///< Height of a field.
const int kMaxChars = 30;                       ///< The editor's character limit.
const int kMaxWidth = 216;                      ///< The editor's width limit, in pixels.

/** @brief The signpost dialog: the sign's two lines, each editable. */
struct Sign : kit::Modal {
    screen::View dialog_view;              ///< Dialog 24's controls.
    w2::Sign* sign = nullptr;              ///< The sign being rewritten.
    std::optional<input::Editor> editing;  ///< The line being typed, if any.
    int edited_line = 0;                   ///< Which line `editing` replaces, 0 or 1.
    /** @brief Writes the line being typed onto the sign and stops editing. */
    void Keep() {
        if (editing) {
            sign->lines[edited_line] = editing->text;
            editing.reset();
        }
    }
    /** @brief Draws the heading, the instructions and the two fields. */
    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "A Signpost!", 320, 94);
        kit::Centred(kit::GetFont(2), "Type the new message for", 320, 140);
        kit::Centred(kit::GetFont(2), "this signpost!", 320, 160);
        for (int i = 0; i < 2; i++) {
            const std::string& text =
                    (editing && edited_line == i) ? editing->text : sign->lines[i];
            kit::Field(kFieldX[i], kFieldY[i], kFieldW, kFieldH, text, &kit::GetFont(2));
            if (editing && edited_line == i) editing->DrawCursor(kFieldX[i], kFieldY[i]);
        }
        // the fields are drawn above; their controls are only hit areas
        std::set<int> drawn_by_hand{kLine1, kLine2};
        kit::DrawControls(dialog_view, drawn_by_hand);
    }
    /** @brief Keeps any line typed; Done closes, a field starts a new line. */
    void Mousepressed(int x, int y, int) override {
        Keep();
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kDone) {
            kit::Pop(this);
        } else if (control->id == kLine1 || control->id == kLine2) {
            edited_line = control->id - kLine1;
            editing = input::Editor();
            editing->max_chars = kMaxChars;
            editing->max_width = kMaxWidth;
        }
    }
    /** @brief Keys go to the field being typed; else Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (editing) {
            std::string outcome = editing->Key(key);
            if (outcome == "keep")
                Keep();
            else if (outcome == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
    /** @brief Typed text goes to the field being typed. */
    void Textinput(const std::string& text) override {
        if (editing) editing->Input(text);
    }
    bool Typing() const override { return editing.has_value(); }
};
}  // namespace

/** @brief Opens the signpost dialog when the stack stands on a sign. */
void Open(const std::vector<w2::Army*>& stack) {
    if (stack.empty()) return;
    w2::Army* first = stack[0];
    if (w2::scn::TerrainAt(*g_app.g->map, first->x, first->y) != w2::move::kTower) return;
    w2::Sign* sign = w2::game::SignAt(*g_app.g, first->x, first->y);
    if (!sign) return;
    auto dialog = std::make_shared<Sign>();
    dialog->sign = sign;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    for (int control_id : {kDone, kLine1, kLine2})
        dialog->dialog_view.state[control_id] = w2::uidata::kNormal;
    kit::Push(dialog);
}

}  // namespace signpost
