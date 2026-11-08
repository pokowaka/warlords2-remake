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

#include "ui/dialogs.hpp"

namespace choose {

namespace {
const kit::Rect kPopupRect{96, 50, 200, 200};  ///< Popup 3.
const kit::Rect kBox{106, 70, 180, 110};       ///< The sunk box the names are listed in.
const int kDialog = 2;                         ///< The dialog's BUTTON.DAT controls.
/// The first row control, page and row scrolling, OK and Cancel.
const int kRow = 114, kPageUp = 119, kPageDown = 120, kUp = 121, kDown = 122, kOk = 123,
          kCancel = 124;
const int kRows = 5;  ///< Rows shown at once.

/** @brief The chooser: a scrolling list of names with one chosen. */
struct Dialog : kit::Modal {
    std::string title;               ///< Shown above the list.
    std::vector<std::string> names;  ///< The entries to choose from.
    std::function<void(int)> after;  ///< Gets the chosen entry, or w2::kNone, on closing.
    screen::View dialog_view;        ///< Dialog 2's controls and their states.
    int top_row = 0;                 ///< The entry shown in the first row.
    int chosen_row = 0;              ///< The chosen entry's row on screen, 0-4.

    /** @brief The number of entries. */
    int Count() const { return static_cast<int>(names.size()); }

    /** @brief Enables the rows and the arrows that can scroll; hides the arrows when all fit. */
    void Refresh() {
        auto& states = dialog_view.state;
        for (int i = 0; i < kRows; i++) states[kRow + i] = w2::uidata::kNormal;
        states[kOk] = w2::uidata::kNormal;
        states[kCancel] = w2::uidata::kNormal;
        hidden.clear();
        if (Count() > kRows) {
            int up_state = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            int down_state =
                    top_row + kRows - 1 < Count() - 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kPageUp] = up_state;
            states[kUp] = up_state;
            states[kPageDown] = down_state;
            states[kDown] = down_state;
        } else {
            for (int control_id : {kPageUp, kPageDown, kUp, kDown}) hidden.insert(control_id);
        }
    }

    /** @brief The chosen entry's index, or w2::kNone when the chosen row is empty. */
    int Chosen() const { return top_row + chosen_row < Count() ? top_row + chosen_row : w2::kNone; }

    /** @brief Closes, then hands `entry` to `after`. */
    void Close(int entry) {
        auto after_copy = after;
        kit::Pop(this);
        if (after_copy) after_copy(entry);
    }

    /** @brief Draws the frame, the title, the visible names and the controls. */
    void Draw() override {
        kit::Popup(kPopupRect);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, title, 196, 52);
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, kBox.x, kBox.y, kBox.w, kBox.h);
        kit::Bevel(kBox.x, kBox.y, kBox.w, kBox.h, 4, 2);
        gfx::SetColor(1, 1, 1);
        for (int i = 0; i < kRows; i++) {
            if (top_row + i < Count())
                font.Colours(i == chosen_row ? 15 : 2, 0)
                        .Draw(names[top_row + i], 112, 74 + 20 * i);
        }
        kit::DrawControls(dialog_view, hidden);
    }

    /**
     * @brief A row control chooses its entry, the arrows scroll, OK and Cancel close.
     *
     * Scrolling a row keeps the same entry chosen where it can.
     */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control) return;
        int control_id = control->id;
        if (control_id >= kRow && control_id < kRow + kRows) {
            if (top_row + control_id - kRow < Count()) chosen_row = control_id - kRow;
        } else if (control_id == kUp) {
            top_row--;
            chosen_row = std::min(chosen_row + 1, kRows - 1);
        } else if (control_id == kDown) {
            top_row++;
            chosen_row = std::max(chosen_row - 1, 0);
        } else if (control_id == kPageUp) {
            top_row -= std::min(kRows, top_row);
        } else if (control_id == kPageDown) {
            top_row += std::min(kRows, Count() - (top_row + kRows - 1) - 1);
        } else if (control_id == kOk) {
            return Close(Chosen());
        } else if (control_id == kCancel) {
            return Close(w2::kNone);
        }
        Refresh();
    }

    /** @brief Enter takes the chosen entry, Escape none. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Close(Chosen());
        else if (key == "escape")
            Close(w2::kNone);
    }
};
}  // namespace

/**
 * @brief Opens the chooser with entry `start` chosen, scrolled so it shows.
 * @param after Gets the chosen entry's index, or w2::kNone, once it closes.
 */
void Open(const std::string& title, const std::vector<std::string>& names, int start,
          std::function<void(int)> after) {
    auto dialog = std::make_shared<Dialog>();
    dialog->title = title;
    dialog->names = names;
    dialog->after = after;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->chosen_row = std::max(0, start);
    while (dialog->chosen_row >= kRows) {
        dialog->chosen_row--;
        dialog->top_row++;
    }
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace choose
