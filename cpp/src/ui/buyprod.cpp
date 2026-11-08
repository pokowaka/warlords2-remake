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
#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace buyprod {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 350};  ///< Popup 11.
const int kDialog = 23;                        ///< The dialog's BUTTON.DAT controls.
/// Done, the first of the four slot controls, the first of the type controls.
const int kDone = 396, kSlotFirst = 397, kTypeFirst = 401;
const int kStringGroup = 0x70;  ///< STRING.DAT group 112: the dialog's words.

/** @brief The Build Production dialog for one city. */
struct BuyDialog : kit::Modal {
    w2::City* city = nullptr;                ///< The city buying.
    kit::Done done;                          ///< Runs once the dialog has closed.
    screen::View dialog_view;                ///< Dialog 23's controls and their states.
    std::vector<const w2::ArmyType*> types;  ///< The types for sale, in control order.
    int slot = 0;                            ///< The production slot being bought into, 0-3.

    /** @brief Enables the slot buttons once all four are full, and the types the side can buy. */
    void Refresh() {
        auto& states = dialog_view.state;
        states[kDone] = w2::uidata::kNormal;
        bool full = city->slots.size() >= 4;
        for (int i = 0; i < 4; i++)
            states[kSlotFirst + i] = full ? w2::uidata::kNormal : w2::uidata::kDisabled;
        for (size_t type_index = 0; type_index < types.size(); type_index++)
            states[kTypeFirst + static_cast<int>(type_index)] =
                    w2::game::CannotBuy(*g_app.g, *g_app.player, *city, *types[type_index]).empty()
                            ? w2::uidata::kNormal
                            : w2::uidata::kDisabled;
    }

    /** @brief Puts the production back in price order, closes, then runs `done`. */
    void Close() {
        w2::game::SortProduction(*city);
        auto done_copy = done;
        kit::Pop(this);
        if (done_copy) done_copy();
    }

    /** @brief Buys the type at `type_index` into the current slot, if the side can. */
    void Buy(int type_index) {
        if (type_index < 0 || type_index >= static_cast<int>(types.size())) return;
        const w2::ArmyType* type = types[type_index];
        if (!w2::game::CannotBuy(*g_app.g, *g_app.player, *city, *type).empty()) return;
        w2::game::BuyProduction(*g_app.g, *g_app.player, *city, slot, type->id);
        // On to the next empty slot, if there is one.
        if (slot < 3 && slot + 1 >= static_cast<int>(city->slots.size())) slot++;
        Refresh();
    }

    /** @brief Draws the title, the city's slots, the gold and the types for sale. */
    void Draw() override {
        int side = g_app.player->index;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kStringGroup, 0), 320, 64);
        kit::Shield(side, 88, 64);
        kit::Shield(side, 512, 64);
        const Font& body = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(body, w2::Format(kit::Text(kStringGroup, 1), g_app.player->name, city->name),
                     320, 104);
        kit::Centred(body, w2::Format(kit::Text(kStringGroup, 2), g_app.player->gold), 352, 365);
        kit::Centred(body, kit::Text(kStringGroup, 3), 176, 348);
        for (int i = 0; i < 4; i++) {
            bool have = i < static_cast<int>(city->slots.size());
            int x = 104 + i * 40;
            kit::Army(have ? city->slots[i].type : w2::kNone, side, x, 370, have ? side + 2 : 1);
            int frame_x = x - 2, frame_y = 368;
            if (slot == i) {
                kit::SetPal(0);
                kit::Outline(frame_x, frame_y, 37, 35);
                kit::SetPal(9);
                kit::Outline(frame_x - 1, frame_y - 1, 37, 35);
            } else {
                kit::SetPal(3);
                kit::Outline(frame_x, frame_y, 37, 35);
                kit::Outline(frame_x - 1, frame_y - 1, 37, 35);
            }
        }
        for (size_t type_index = 0; type_index < types.size(); type_index++) {
            int col = static_cast<int>(type_index) % 4, row = static_cast<int>(type_index) / 4;
            int x = 88 + col * 120, y = 136 + row * 31;
            kit::Army(types[type_index]->id, side, x, y, 1,
                      !w2::game::CannotBuy(*g_app.g, *g_app.player, *city, *types[type_index])
                               .empty());
            gfx::SetColor(1, 1, 1);
            body.Draw(w2::Fmt("%d gp", types[type_index]->price), x + 40, y + 6);
        }
        kit::DrawControls(dialog_view);
    }

    /** @brief Done closes, a slot control picks the slot, a type control buys it. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kDone)
            Close();
        else if (control->id >= kSlotFirst && control->id < kSlotFirst + 4)
            slot = control->id - kSlotFirst;
        else if (control->id >= kTypeFirst)
            Buy(control->id - kTypeFirst);
    }

    /** @brief The right button on a type for sale (help sub-ids 5-24): its army-type box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        int type_index = sub - 5;
        if (type_index < 0 || type_index >= static_cast<int>(types.size())) return false;
        const w2::ArmyType* type = types[type_index];
        infobox::ArmyType(screen_x, screen_y, type->id,
                          w2::Slot{type->id, type->name, type->strength, type->time, type->cost,
                                   type->move, type->price});
        return true;
    }

    /** @brief Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }
};
}  // namespace

/**
 * @brief Opens Build Production for `city`, buying into its first free slot.
 * @param after Runs once the dialog has closed.
 */
void Open(w2::City* city, kit::Done after) {
    auto dialog = std::make_shared<BuyDialog>();
    dialog->city = city;
    dialog->done = after;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->types = w2::game::BuyableTypes(*g_app.g);
    dialog->slot = w2::game::BuySlot(*city);
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace buyprod
