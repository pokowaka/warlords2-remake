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
#include <memory>
#include <string>
#include <utility>

#include "ui/dialogs.hpp"

namespace resign {

namespace {

constexpr kit::Rect kPopupRect{160, 90, 320, 200};  ///< Popup 1.
constexpr int kDialog = 33;                         ///< The dialog with the buttons.
constexpr int kGracious = 487;                      ///< Resigns graciously.
constexpr int kResign = 488;                        ///< Resigns.
constexpr int kCancel = 489;                        ///< Thinks better of it.
constexpr int kTextGroup = 0xa5;  ///< STRING.DAT group with all the dialog's lines.

/** @brief Line `line` of the resign text group. */
std::string ResignLine(const w2::UiContext& ctx, int line) {
    return kit::Text(ctx, kTextGroup, line);
}

/** @brief Resigns the player: its cities burn and its armies go; then `after`. */
void Burn(w2::UiContext& ctx, kit::Done after) {
    w2::game::Resign(ctx.ActiveGame(), *ctx.Player());
    ctx.ClearSelection();
    ctx.GetViewport().StratDirty();
    if (after) after();
}

/** @brief Advances through the four-part gracious resignation prompt sequence. */
void GraciousStep(w2::UiContext& ctx, int step) {
    if (step < 3) {
        search::Say(ctx, ResignLine(ctx, 6 + step),
                    [&ctx, step]() { GraciousStep(ctx, step + 1); });
        return;
    }
    search::ShowMessage(ctx, ResignLine(ctx, 9), ResignLine(ctx, 10),
                        [&ctx]() { Burn(ctx, nullptr); });
}

/** @brief The Resign dialog: graciously, outright, or not at all. */
class ResignModal final : public kit::Modal {
  public:
    explicit ResignModal(w2::UiContext& ctx)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)) {
        SetDialogView(&dialog_view_);
        for (int control_id : {kGracious, kResign, kCancel}) {
            dialog_view_.state[control_id] = w2::uidata::kNormal;
        }
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), ResignLine(ctx, 0), 320, 92);
        const Font& font = kit::GetFont(ctx, 2);
        for (int i = 1; i <= 3; i++) {
            kit::Centred(font, ResignLine(ctx, i), 320, 120 + 20 * i);
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kCancel) {
            PopSelf();
        } else if (control->id == kGracious) {
            PopSelf();
            GraciousStep(ctx, 0);
        } else if (control->id == kResign) {
            PopSelf();
            Burn(ctx,
                 [&ctx]() { search::ShowMessage(ctx, ResignLine(ctx, 4), ResignLine(ctx, 5)); });
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View dialog_view_;  ///< Dialog 33's buttons.
};

}  // namespace

/**
 * @brief Opens Order > Resign for the player; does nothing when the side has
 * no city left.
 */
void Open(w2::UiContext& ctx) {
    // a side with no city left has nothing to resign
    if (w2::game::SideCities(ctx.ActiveGame(), *ctx.Player()).empty()) return;
    kit::Push(ctx, std::make_shared<ResignModal>(ctx));
}

}  // namespace resign
