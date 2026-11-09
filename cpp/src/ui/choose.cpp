/**
 * @file
 * @brief The game's list chooser: pick one of a list of names.
 *
 * Popup 3, (96, 50) 200x200, with dialog 2: the title in font 2 centred on
 * (196, 52), a colour-3 box sunk with a (4, 2) bevel, five names 20 apart,
 * the chosen one in colour 15 and the rest in colour 2. Controls 114-118
 * choose a row, 121/122 scroll a row and 119/120 five, hidden when the list
 * fits; OK (123) hands the chosen entry over, Cancel (124) none.
 */
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "ui/dialogs.hpp"

namespace choose {

namespace {
constexpr kit::Rect kPopupRect{96, 50, 200, 200};  ///< Popup 3.
constexpr kit::Rect kBox{106, 70, 180, 110};       ///< The sunk box the names are listed in.
constexpr int kDialog = 2;                         ///< The dialog's BUTTON.DAT controls.
/// The first row control, page and row scrolling, OK and Cancel.
constexpr int kRow = 114, kPageUp = 119, kPageDown = 120, kUp = 121, kDown = 122, kOk = 123,
              kCancel = 124;
constexpr int kRows = 5;  ///< Rows shown at once.

/** @brief The chooser: a scrolling list of names with one chosen. */
class Dialog final : public kit::Modal {
  public:
    Dialog(w2::UiContext& ctx, std::string title, ChoiceList names, int start, PickCallback after)
            : kit::Modal(ctx)
            , title_(std::move(title))
            , names_(std::move(names))
            , after_(std::move(after))
            , dialog_view_(kit::View(ctx, kDialog))
            , chosen_row_(std::max(0, start)) {
        SetDialogView(&dialog_view_);
        while (chosen_row_ >= kRows) {
            chosen_row_--;
            top_row_++;
        }
        Refresh();
    }

    /** @brief Draws the frame, the title, the visible names and the controls. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kPopupRect);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, title_, 196, 52);
        kit::SetPal(ctx, 3);
        ren.Rectangle(gfx::kFill, kBox.x, kBox.y, kBox.w, kBox.h);
        kit::Bevel(ctx, kBox.x, kBox.y, kBox.w, kBox.h, 4, 2);
        ren.SetColor(1, 1, 1);
        for (int i = 0; i < kRows; i++) {
            if (top_row_ + i < Count()) {
                font.Colours(i == chosen_row_ ? 15 : 2, 0)
                        .Draw(names_[top_row_ + i], 112, 74 + 20 * i);
            }
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /**
     * @brief A row control chooses its entry, the arrows scroll, OK and Cancel close.
     *
     * Scrolling a row keeps the same entry chosen where it can.
     */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control) return;
        int control_id = control->id;
        if (control_id >= kRow && control_id < kRow + kRows) {
            if (top_row_ + control_id - kRow < Count()) chosen_row_ = control_id - kRow;
        } else if (control_id == kUp) {
            top_row_--;
            chosen_row_ = std::min(chosen_row_ + 1, kRows - 1);
        } else if (control_id == kDown) {
            top_row_++;
            chosen_row_ = std::max(chosen_row_ - 1, 0);
        } else if (control_id == kPageUp) {
            top_row_ -= std::min(kRows, top_row_);
        } else if (control_id == kPageDown) {
            top_row_ += std::min(kRows, Count() - (top_row_ + kRows - 1) - 1);
        } else if (control_id == kOk) {
            return Close(Chosen());
        } else if (control_id == kCancel) {
            return Close(w2::kNone);
        }
        Refresh();
    }

    /** @brief Enter takes the chosen entry, Escape none. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Close(Chosen());
        } else if (key == "escape") {
            Close(w2::kNone);
        }
    }

  private:
    /** @brief The number of entries. */
    int Count() const { return static_cast<int>(names_.size()); }

    /** @brief Enables the rows and the arrows that can scroll; hides the arrows when all fit. */
    void Refresh() {
        auto& states = dialog_view_.state;
        for (int i = 0; i < kRows; i++) states[kRow + i] = w2::uidata::kNormal;
        states[kOk] = w2::uidata::kNormal;
        states[kCancel] = w2::uidata::kNormal;
        HiddenControls().clear();
        if (Count() > kRows) {
            int up_state = top_row_ > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            int down_state = top_row_ + kRows - 1 < Count() - 1 ? w2::uidata::kNormal
                                                                : w2::uidata::kDisabled;
            states[kPageUp] = up_state;
            states[kUp] = up_state;
            states[kPageDown] = down_state;
            states[kDown] = down_state;
        } else {
            for (int control_id : {kPageUp, kPageDown, kUp, kDown}) {
                HiddenControls().insert(control_id);
            }
        }
    }

    /** @brief The chosen entry's index, or w2::kNone when the chosen row is empty. */
    int Chosen() const {
        return top_row_ + chosen_row_ < Count() ? top_row_ + chosen_row_ : w2::kNone;
    }

    /** @brief Closes, then hands `entry` to `after_`. */
    void Close(int entry) {
        PickCallback after_copy = std::move(after_);
        PopSelf();
        if (after_copy) after_copy(entry);
    }

    std::string title_;         ///< Shown above the list.
    ChoiceList names_;          ///< The entries to choose from.
    PickCallback after_;        ///< Gets the chosen entry, or w2::kNone, on closing.
    screen::View dialog_view_;  ///< Dialog 2's controls and their states.
    int top_row_ = 0;           ///< The entry shown in the first row.
    int chosen_row_ = 0;        ///< The chosen entry's row on screen, 0-4.
};
}  // namespace

/**
 * @brief Opens the chooser with entry `start` chosen, scrolled so it shows.
 * @param after Gets the chosen entry's index, or w2::kNone, once it closes.
 */
void Open(w2::UiContext& ctx, const std::string& title, const ChoiceList& names, int start,
          PickCallback after) {
    kit::Push(ctx, std::make_shared<Dialog>(ctx, title, names, start, std::move(after)));
}

}  // namespace choose
