/**
 * @file
 * @brief Order > Resign.
 *
 * Popup 1, (160, 90) 320x200: "Resign!" (group 165) in font 1 centred on
 * (320, 92), and three lines of font 2. Dialog 33: 487 resigns graciously,
 * 488 resigns, 489 thinks better of it (default and cancel both). Either way
 * the side's cities burn and its armies go. The gracious way is asked three
 * times first; the other is told afterwards what has burned.
 */
#include "ui/dialogs.hpp"

namespace resign {

namespace {
const kit::Rect kPopupRect{160, 90, 320, 200};  ///< Popup 1.
const int kDialog = 33,                         ///< The dialog with the buttons.
        kGracious = 487,                        ///< Resigns graciously.
        kResign = 488,                          ///< Resigns.
        kCancel = 489;                          ///< Thinks better of it.
const int kTextGroup = 0xa5;                    ///< STRING.DAT group with all the dialog's lines.
/** @brief Line `line` of the resign text group. */
std::string ResignLine(int line) {
    return kit::Text(kTextGroup, line);
}

/** @brief Resigns the player: its cities burn and its armies go; then `after`. */
void Burn(kit::Done after) {
    w2::game::Resign(*g_app.g, *g_app.player);
    g_app.selection.reset();
    front::StratDirty();
    if (after) after();
}

/** @brief The Resign dialog: graciously, outright, or not at all. */
struct Resign : kit::Modal {
    screen::View dialog_view;  ///< Dialog 33's buttons.
    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), ResignLine(0), 320, 92);
        for (int i = 1; i <= 3; i++)
            kit::Centred(kit::GetFont(2), ResignLine(i), 320, 120 + 20 * i);
        kit::DrawControls(dialog_view);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kCancel) {
            kit::Pop(this);
        } else if (control->id == kGracious) {
            kit::Pop(this);
            search::Say(ResignLine(6), []() {
                search::Say(ResignLine(7), []() {
                    search::Say(ResignLine(8), []() {
                        search::ShowMessage(ResignLine(9), ResignLine(10), []() { Burn(nullptr); });
                    });
                });
            });
        } else if (control->id == kResign) {
            kit::Pop(this);
            Burn([]() { search::ShowMessage(ResignLine(4), ResignLine(5)); });
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief Opens Order > Resign for the player; does nothing when the side has
 * no city left.
 */
void Open() {
    // a side with no city left has nothing to resign
    if (w2::game::SideCities(*g_app.g, *g_app.player).empty()) return;
    auto dialog = std::make_shared<Resign>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    for (int control_id : {kGracious, kResign, kCancel})
        dialog->dialog_view.state[control_id] = w2::uidata::kNormal;
    kit::Push(dialog);
}

}  // namespace resign
