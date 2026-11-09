/**
 * @file
 * @brief What a pillage or a sack took (auto_ui_pillage_sack_raze), shown
 * over the spoils dialog and closed, the two together, by any key or click.
 *
 * Popup 12, (176, 60) 288x300, marble.
 */
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace spoils {

using w2::Format;

namespace {

constexpr kit::Rect kPopupRect{176, 60, 288, 300};  ///< Popup 12, the whole box.
constexpr int kTitle = 0x44;  ///< STRING.DAT group: the title, then the list's headings.
constexpr int kLine = 0x45;   ///< STRING.DAT group: the city pillaged or sacked.
constexpr int kGold = 0x46;   ///< STRING.DAT group: the gold taken.
constexpr int kLost = 0x47;   ///< STRING.DAT group: how many production slots went.
constexpr int kLeft = 0x48;   ///< STRING.DAT group: how many are left.

/** @brief The spoils box: the city, the gold, and the production sold for it. */
class SpoilsModal final : public kit::Modal {
  public:
    SpoilsModal(w2::UiContext& ctx, bool sacked, w2::City* city, int gold, LostSlotList lost,
                int left, kit::Done after)
            : kit::Modal(ctx)
            , sacked_(sacked)
            , city_(city)
            , gold_(gold)
            , left_(left)
            , lost_(std::move(lost))
            , after_(std::move(after)) {}

    /** @brief Draws the title, the totals and up to three things lost. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        int which = sacked_ ? 1 : 0;
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kTitle, which), 320, 64);
        const Font& font = kit::GetFont(ctx, 2);
        kit::Centred(font, Format(kit::Text(ctx, kLine, which), city_->Name()), 320, 115);
        kit::Centred(font, Format(kit::Text(ctx, kGold, 0), gold_), 320, 135);
        kit::Centred(font,
                     Format(kit::Text(ctx, kLost, lost_.size() == 1 ? 0 : 1),
                            static_cast<int>(lost_.size())),
                     320, 165);
        kit::Centred(font, Format(kit::Text(ctx, kLeft, left_ == 1 ? 0 : 1), left_), 320, 185);
        kit::Bevel(ctx, 184, 220, 272, 130, 4, 2);
        kit::Bevel(ctx, 185, 221, 270, 128, 2, 4);
        ren.SetColor(1, 1, 1);
        font.Draw(kit::Text(ctx, kTitle, 2), 192, 230);
        font.Draw(kit::Text(ctx, kTitle, 3), 368, 230);
        for (int i = 0; i <= 2; i++) {
            int y = 250 + 30 * i;
            bool have = i < static_cast<int>(lost_.size());
            kit::Army(ctx, have ? lost_[i].first : w2::kNone, ctx.Player()->Index(), 208, y, 1);
            if (have) {
                ren.SetColor(1, 1, 1);
                const w2::ArmyType* type = ctx.ActiveGame().ArmyTypes().ById(lost_[i].first);
                font.Draw(type ? type->name : "", 248, y + 6);
                font.Draw(absl::StrFormat("%d gp", lost_[i].second), 368, y + 6);
            }
        }
    }

    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }

  private:
    /** @brief Closes the box, then runs `after_`. */
    void Close() {
        kit::Done after_copy = std::move(after_);
        PopSelf();
        if (after_copy) after_copy();
    }

    bool sacked_ = false;       ///< A sack; else a pillage.
    w2::City* city_ = nullptr;  ///< The city taken.
    int gold_ = 0;              ///< The gold the spoils brought.
    int left_ = 0;              ///< How many production slots the city has left.
    LostSlotList lost_;         ///< The production lost: army type id, gold got.
    kit::Done after_;           ///< Runs once the box is closed.
};

}  // namespace

/** @brief Opens the spoils box; `after` runs when it is closed. */
void Open(w2::UiContext& ctx, bool sacked, w2::City* city, int gold, const LostSlotList& lost,
          int left, kit::Done after) {
    kit::Push(ctx,
              std::make_shared<SpoilsModal>(ctx, sacked, city, gold, lost, left, std::move(after)));
}

}  // namespace spoils
