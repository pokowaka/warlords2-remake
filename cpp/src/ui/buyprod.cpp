/**
 * @file
 * @brief Build Production: buying a new army type for a city.
 *
 * The city dialog's Build Prod button (control 204) pushes popup 11 --
 * (80, 60) 480x350, marble -- and dialog 23 over it: the title between the
 * side's shields, "The %s city of %s", every type with a price four to a
 * row -- ghosted when the city already has it or the side cannot pay --
 * "Currently Producing" with the city's four slots, and the gold. The slot
 * being bought into is framed in colours 0 and 9, the others in 3. Control
 * 396 is Done, 397-400 pick the slot (live only once all four are full), 401
 * on are the types. Done puts the list back in price order.
 */
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace buyprod {

namespace {
constexpr kit::Rect kPopupRect{80, 60, 480, 350};  ///< Popup 11.
constexpr int kDialog = 23;                        ///< The dialog's BUTTON.DAT controls.
/// Done, the first of the four slot controls, the first of the type controls.
constexpr int kDone = 396, kSlotFirst = 397, kTypeFirst = 401;
constexpr int kStringGroup = 0x70;  ///< STRING.DAT group 112: the dialog's words.

/** @brief The Build Production dialog for one city. */
class BuyDialog final : public kit::Modal {
  public:
    BuyDialog(w2::UiContext& ctx, w2::City* target_city, kit::Done after)
            : kit::Modal(ctx)
            , city_(target_city)
            , done_(std::move(after))
            , dialog_view_(kit::View(ctx, kDialog))
            , types_(w2::game::BuyableTypes(ctx.ActiveGame()))
            , slot_(w2::game::BuySlot(*target_city)) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the title, the city's slots, the gold and the types for sale. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Side& player = *ctx.Player();
        int side = player.Index();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kStringGroup, 0), 320, 64);
        kit::Shield(ctx, side, 88, 64);
        kit::Shield(ctx, side, 512, 64);
        const Font& body = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(body,
                     w2::Format(kit::Text(ctx, kStringGroup, 1), player.Name(), city_->Name()), 320,
                     104);
        kit::Centred(body, w2::Format(kit::Text(ctx, kStringGroup, 2), player.Gold()), 352, 365);
        kit::Centred(body, kit::Text(ctx, kStringGroup, 3), 176, 348);
        for (int i = 0; i < 4; i++) {
            bool have = i < city_->SlotCount();
            int x = 104 + i * 40;
            kit::Army(ctx, have ? city_->SlotAt(i).type : w2::kNone, side, x, 370,
                      have ? side + 2 : 1);
            int frame_x = x - 2, frame_y = 368;
            if (slot_ == i) {
                kit::SetPal(ctx, 0);
                kit::Outline(ctx, frame_x, frame_y, 37, 35);
                kit::SetPal(ctx, 9);
                kit::Outline(ctx, frame_x - 1, frame_y - 1, 37, 35);
            } else {
                kit::SetPal(ctx, 3);
                kit::Outline(ctx, frame_x, frame_y, 37, 35);
                kit::Outline(ctx, frame_x - 1, frame_y - 1, 37, 35);
            }
        }
        for (size_t type_index = 0; type_index < types_.size(); type_index++) {
            int col = static_cast<int>(type_index) % 4;
            int row = static_cast<int>(type_index) / 4;
            int x = 88 + col * 120, y = 136 + row * 31;
            kit::Army(ctx, types_[type_index]->id, side, x, y, 1,
                      !w2::game::CannotBuy(ctx.ActiveGame(), player, *city_, *types_[type_index])
                               .empty());
            ren.SetColor(1, 1, 1);
            body.Draw(absl::StrFormat("%d gp", types_[type_index]->price), x + 40, y + 6);
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    /** @brief Done closes, a slot control picks the slot, a type control buys it. */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kDone) {
            Close();
        } else if (control->id >= kSlotFirst && control->id < kSlotFirst + 4) {
            slot_ = control->id - kSlotFirst;
        } else if (control->id >= kTypeFirst) {
            Buy(control->id - kTypeFirst);
        }
    }

    /** @brief The right button on a type for sale (help sub-ids 5-24): its army-type box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        int type_index = sub - 5;
        if (type_index < 0 || type_index >= static_cast<int>(types_.size())) return false;
        const w2::ArmyType* type = types_[type_index];
        infobox::ArmyType(Ctx(), screen_x, screen_y, type->id,
                          w2::Slot{type->id, type->name, type->strength, type->time, type->cost,
                                   type->move, type->price});
        return true;
    }

    /** @brief Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }

  private:
    using ArmyTypeList = std::vector<const w2::ArmyType*>;

    /** @brief Enables the slot buttons once all four are full, and the types the side can buy. */
    void Refresh() {
        w2::UiContext& ctx = Ctx();
        auto& states = dialog_view_.state;
        states[kDone] = w2::uidata::kNormal;
        bool full = city_->SlotCount() >= 4;
        for (int i = 0; i < 4; i++) {
            states[kSlotFirst + i] = full ? w2::uidata::kNormal : w2::uidata::kDisabled;
        }
        for (size_t type_index = 0; type_index < types_.size(); type_index++) {
            states[kTypeFirst + static_cast<int>(type_index)] =
                    w2::game::CannotBuy(ctx.ActiveGame(), *ctx.Player(), *city_,
                                        *types_[type_index])
                                    .empty()
                            ? w2::uidata::kNormal
                            : w2::uidata::kDisabled;
        }
    }

    /** @brief Puts the production back in price order, closes, then runs `done_`. */
    void Close() {
        w2::game::SortProduction(*city_);
        kit::Done done_copy = std::move(done_);
        PopSelf();
        if (done_copy) done_copy();
    }

    /** @brief Buys the type at `type_index` into the current slot, if the side can. */
    void Buy(int type_index) {
        if (type_index < 0 || type_index >= static_cast<int>(types_.size())) return;
        w2::UiContext& ctx = Ctx();
        const w2::ArmyType* type = types_[type_index];
        if (!w2::game::CannotBuy(ctx.ActiveGame(), *ctx.Player(), *city_, *type).empty()) return;
        w2::game::BuyProduction(ctx.ActiveGame(), *ctx.Player(), *city_, slot_, type->id);
        // On to the next empty slot, if there is one.
        if (slot_ < 3 && slot_ + 1 >= city_->SlotCount()) slot_++;
        Refresh();
    }

    w2::City* city_ = nullptr;  ///< The city buying.
    kit::Done done_;            ///< Runs once the dialog has closed.
    screen::View dialog_view_;  ///< Dialog 23's controls and their states.
    ArmyTypeList types_;        ///< The types for sale, in control order.
    int slot_ = 0;              ///< The production slot being bought into, 0-3.
};
}  // namespace

/**
 * @brief Opens Build Production for `target_city`, buying into its first free slot.
 * @param after Runs once the dialog has closed.
 */
void Open(w2::UiContext& ctx, w2::City* target_city, kit::Done after) {
    kit::Push(ctx, std::make_shared<BuyDialog>(ctx, target_city, std::move(after)));
}

}  // namespace buyprod
