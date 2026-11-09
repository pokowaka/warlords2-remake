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
#include <memory>
#include <optional>
#include <string>

#include "ui/dialogs.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"

namespace signpost {

namespace {

using OptionalEditor = std::optional<input::Editor>;

constexpr kit::Rect kPopupRect{160, 90, 320, 200};  ///< Popup 1, the whole dialog.
constexpr int kDialog = 24;                         ///< BUTTON.DAT dialog number.
constexpr int kDone = 421;                          ///< The Done control.
constexpr int kLine1 = 422;                         ///< The first field's hit area.
constexpr int kLine2 = 423;                         ///< The second field's hit area.
constexpr int kFieldX[2] = {200, 200};              ///< Left edges of the two fields.
constexpr int kFieldY[2] = {190, 215};              ///< Top edges of the two fields.
constexpr int kFieldW = 240;                        ///< Width of a field.
constexpr int kFieldH = 22;                         ///< Height of a field.
constexpr int kMaxChars = 30;                       ///< The editor's character limit.
constexpr int kMaxWidth = 216;                      ///< The editor's width limit, in pixels.

/** @brief The signpost dialog: the sign's two lines, each editable. */
class SignModal final : public kit::Modal {
  public:
    SignModal(w2::UiContext& ctx, w2::Sign* sign)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), sign_(sign) {
        SetDialogView(&dialog_view_);
        for (int control_id : {kDone, kLine1, kLine2}) {
            dialog_view_.state[control_id] = w2::uidata::kNormal;
        }
    }

    /** @brief Draws the heading, the instructions and the two fields. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "A Signpost!", 320, 94);
        const Font& font = kit::GetFont(ctx, 2);
        kit::Centred(font, "Type the new message for", 320, 140);
        kit::Centred(font, "this signpost!", 320, 160);
        for (int i = 0; i < 2; i++) {
            const std::string& text =
                    (editing_ && edited_line_ == i) ? editing_->text : sign_->lines[i];
            kit::Field(ctx, kFieldX[i], kFieldY[i], kFieldW, kFieldH, text, &font);
            if (editing_ && edited_line_ == i) editing_->DrawCursor(ctx, kFieldX[i], kFieldY[i]);
        }
        // the fields are drawn above; their controls are only hit areas
        kit::ControlIdSet drawn_by_hand{kLine1, kLine2};
        kit::DrawControls(ctx, dialog_view_, drawn_by_hand);
    }

    /** @brief Keeps any line typed; Done closes, a field starts a new line. */
    void Mousepressed(int x, int y, int) override {
        Keep();
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kDone) {
            PopSelf();
        } else if (control->id == kLine1 || control->id == kLine2) {
            edited_line_ = control->id - kLine1;
            editing_ = input::Editor();
            editing_->max_chars = kMaxChars;
            editing_->max_width = kMaxWidth;
        }
    }

    /** @brief Keys go to the field being typed; else Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (editing_) {
            std::string outcome = editing_->Key(key);
            if (outcome == "keep") {
                Keep();
            } else if (outcome == "undo") {
                editing_.reset();
            }
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

    /** @brief Typed text goes to the field being typed. */
    void Textinput(const std::string& text) override {
        if (editing_) editing_->Input(Ctx(), text);
    }

    bool Typing() const override { return editing_.has_value(); }

  private:
    /** @brief Writes the line being typed onto the sign and stops editing. */
    void Keep() {
        if (editing_) {
            sign_->lines[edited_line_] = editing_->text;
            editing_.reset();
        }
    }

    screen::View dialog_view_;  ///< Dialog 24's controls.
    w2::Sign* sign_ = nullptr;  ///< The sign being rewritten.
    OptionalEditor editing_;    ///< The line being typed, if any.
    int edited_line_ = 0;       ///< Which line `editing_` replaces, 0 or 1.
};

}  // namespace

/** @brief Opens the signpost dialog when the stack stands on a sign. */
void Open(w2::UiContext& ctx, const ArmyStack& stack) {
    if (stack.empty()) return;
    w2::Army* first = stack[0];
    w2::Game& game = ctx.ActiveGame();
    if (w2::scn::TerrainAt(game.GetMap(), first->X(), first->Y()) != w2::move::kTower) return;
    w2::Sign* sign = w2::game::SignAt(game, first->X(), first->Y());
    if (!sign) return;
    kit::Push(ctx, std::make_shared<SignModal>(ctx, sign));
}

}  // namespace signpost
