/**
 * @file
 * @brief Order > Fight Order: the order the side's armies fight in.
 *
 * Popup 11, (80, 60) 480x350: "Fighting Order" between the side's shields,
 * "Order of combat for %s", three lines of help, and the 27 places four to a
 * row, the chosen one on the side's ring. Dialog 26: OK (425), Cancel (426),
 * Default (427, the neutral row back), 428/429 move the chosen army a place
 * earlier or later, and 430-456 the places. A click on a place chooses it;
 * on the chosen one, lets it go; on another, swaps the two. Cancel puts back
 * the copy taken on opening.
 */
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace fightorder {

namespace {
constexpr kit::Rect kPopupRect{80, 60, 480, 350};  ///< Popup 11.
constexpr int kDialog = 26;                        ///< The dialog's BUTTON.DAT controls.
/// OK, Cancel, Default, a place earlier, a place later, the first place.
constexpr int kOk = 425, kCancel = 426, kDefault = 427, kEarlier = 428, kLater = 429, kPlace = 430;
constexpr int kPlaces = 27;         ///< Places shown and chosen among.
constexpr int kStringGroup = 0x7f;  ///< STRING.DAT group 127: the dialog's words.
constexpr int kNeutral = 8;         ///< The neutral side's row: the default order.

/**
 * @brief The Fight Order dialog, editing the player's row in place.
 *
 * The row maps each army type id to its place; `backup_` restores it on
 * Cancel.
 */
class Order final : public kit::Modal {
  public:
    explicit Order(w2::UiContext& ctx)
            : kit::Modal(ctx)
            , dialog_view_(kit::View(ctx, kDialog))
            , backup_(ctx.ActiveGame().GetMap().FightOrder()[ctx.Player()->Index()]) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the title, the help lines and the 27 places with their armies. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Side* side = ctx.Player();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kStringGroup, 0), 320, 64);
        kit::Shield(ctx, side->Index(), 88, 64);
        kit::Shield(ctx, side->Index(), 512, 64);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, w2::Format(kit::Text(ctx, kStringGroup, 1), side->Name()), 320, 104);
        kit::Centred(font, kit::Text(ctx, kStringGroup, 2), 320, 348);
        kit::Centred(font, kit::Text(ctx, kStringGroup, 3), 320, 368);
        kit::Centred(font, kit::Text(ctx, kStringGroup, 4), 320, 388);
        for (int i = 0; i < kPlaces; i++) {
            int x = 120 * (i % 4), y = 31 * (i / 4);
            int type_id = TypeAt(i);
            if (type_id != w2::kNone) {
                kit::Army(ctx, type_id, side->Index(), 88 + x, 128 + y,
                          i == chosen_ ? side->Index() + 2 : 1);
            }
            ren.SetColor(1, 1, 1);
            font.Draw(absl::StrFormat("%d.", i + 1), 128 + x, 134 + y);
        }
        // The place controls are clicked but not drawn: the armies stand for them.
        kit::ControlIdSet place_controls;
        for (int i = 0; i < kPlaces; i++) place_controls.insert(kPlace + i);
        kit::DrawControls(ctx, dialog_view_, place_controls);
    }

    /** @brief OK, Cancel, Default, Earlier, Later, or a place chosen, let go or swapped. */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == kOk) return Close(true);
        if (control_id == kCancel) return Close(false);
        if (control_id == kDefault) {
            Row() = Ctx().ActiveGame().GetMap().FightOrder()[kNeutral];
            chosen_ = -1;
        } else if (control_id == kEarlier && chosen_ > 0) {
            Swap(chosen_, chosen_ - 1);
            chosen_--;
        } else if (control_id == kLater && chosen_ >= 0 && chosen_ < kPlaces - 1) {
            Swap(chosen_, chosen_ + 1);
            chosen_++;
        } else if (control_id >= kPlace && control_id < kPlace + kPlaces) {
            int place = control_id - kPlace;
            if (chosen_ == place) {
                chosen_ = -1;
            } else if (chosen_ < 0) {
                chosen_ = place;
            } else {
                Swap(chosen_, place);
                chosen_ = -1;
            }
        }
        Refresh();
    }

    /** @brief The right button on a place (help sub-ids 45-71): its army-type box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        w2::UiContext& ctx = Ctx();
        int type_id = TypeAt(sub - 45);
        const w2::ArmyType* type =
                type_id != w2::kNone ? ctx.ActiveGame().ArmyTypes().ById(type_id) : nullptr;
        if (!type) return false;
        infobox::ArmyType(ctx, screen_x, screen_y, type_id,
                          w2::Slot{type->id, type->name, type->strength, type->time, type->cost,
                                   type->move, type->price},
                          ctx.Player()->Index());
        return true;
    }

    /** @brief Enter keeps the new order, Escape puts the old one back. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Close(true);
        } else if (key == "escape") {
            Close(false);
        }
    }

  private:
    using RankRow = std::vector<int>;

    /** @brief The player's fight-order row: army type id -> place. */
    RankRow& Row() {
        return Ctx().ActiveGame().GetMap().MutableFightOrder()[Ctx().Player()->Index()];
    }

    /** @brief The army type id at place `rank`, or w2::kNone. */
    int TypeAt(int rank) {
        const auto& row = Row();
        for (int type_id = 0; type_id <= 28 && type_id < static_cast<int>(row.size()); type_id++) {
            if (row[type_id] == rank) return type_id;
        }
        return w2::kNone;
    }

    /** @brief Swaps the types at places `first_rank` and `second_rank`. */
    void Swap(int first_rank, int second_rank) {
        int first_type = TypeAt(first_rank), second_type = TypeAt(second_rank);
        if (first_type != w2::kNone) Row()[first_type] = second_rank;
        if (second_type != w2::kNone) Row()[second_type] = first_rank;
    }

    /** @brief Enables Earlier and Later when the chosen place can move that way. */
    void Refresh() {
        auto& states = dialog_view_.state;
        states[kOk] = states[kCancel] = states[kDefault] = w2::uidata::kNormal;
        states[kEarlier] = chosen_ > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kLater] = (chosen_ >= 0 && chosen_ < kPlaces - 1) ? w2::uidata::kNormal
                                                                 : w2::uidata::kDisabled;
        for (int i = 0; i < kPlaces; i++) states[kPlace + i] = w2::uidata::kNormal;
    }

    /** @brief Closes, keeping the new order or putting the old one back. */
    void Close(bool keep) {
        if (!keep) Row() = backup_;
        PopSelf();
    }

    screen::View dialog_view_;  ///< Dialog 26's controls and their states.
    RankRow backup_;            ///< The row as it was on opening.
    int chosen_ = -1;           ///< The chosen place, or -1 for none.
};
}  // namespace

/** @brief Opens Fight Order, keeping a copy of the row for Cancel. */
void Open(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<Order>(ctx));
}

}  // namespace fightorder
