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
#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace fightorder {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 350};  ///< Popup 11.
const int kDialog = 26;                        ///< The dialog's BUTTON.DAT controls.
/// OK, Cancel, Default, a place earlier, a place later, the first place.
const int kOk = 425, kCancel = 426, kDefault = 427, kEarlier = 428, kLater = 429, kPlace = 430;
const int kPlaces = 27;         ///< Places shown and chosen among.
const int kStringGroup = 0x7f;  ///< STRING.DAT group 127: the dialog's words.
const int kNeutral = 8;         ///< The neutral side's row: the default order.

/**
 * @brief The Fight Order dialog, editing the player's row in place.
 *
 * The row maps each army type id to its place; `backup` restores it on
 * Cancel.
 */
struct Order : kit::Modal {
    screen::View dialog_view;  ///< Dialog 26's controls and their states.
    std::vector<int> backup;   ///< The row as it was on opening.
    int chosen = -1;           ///< The chosen place, or -1 for none.

    /** @brief The player's fight-order row: army type id -> place. */
    std::vector<int>& Row() { return g_app.g->map->fight_order[g_app.player->index]; }

    /** @brief The army type id at place `rank`, or w2::kNone. */
    int TypeAt(int rank) {
        auto& row = Row();
        for (int type_id = 0; type_id <= 28 && type_id < static_cast<int>(row.size()); type_id++)
            if (row[type_id] == rank) return type_id;
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
        auto& states = dialog_view.state;
        states[kOk] = states[kCancel] = states[kDefault] = w2::uidata::kNormal;
        states[kEarlier] = chosen > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kLater] =
                (chosen >= 0 && chosen < kPlaces - 1) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        for (int i = 0; i < kPlaces; i++) states[kPlace + i] = w2::uidata::kNormal;
    }

    /** @brief Closes, keeping the new order or putting the old one back. */
    void Close(bool keep) {
        if (!keep) Row() = backup;
        kit::Pop(this);
    }

    /** @brief Draws the title, the help lines and the 27 places with their armies. */
    void Draw() override {
        w2::Side* side = g_app.player;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kStringGroup, 0), 320, 64);
        kit::Shield(side->index, 88, 64);
        kit::Shield(side->index, 512, 64);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, w2::Format(kit::Text(kStringGroup, 1), side->name), 320, 104);
        kit::Centred(font, kit::Text(kStringGroup, 2), 320, 348);
        kit::Centred(font, kit::Text(kStringGroup, 3), 320, 368);
        kit::Centred(font, kit::Text(kStringGroup, 4), 320, 388);
        for (int i = 0; i < kPlaces; i++) {
            int x = 120 * (i % 4), y = 31 * (i / 4);
            int type_id = TypeAt(i);
            if (type_id != w2::kNone)
                kit::Army(type_id, side->index, 88 + x, 128 + y, i == chosen ? side->index + 2 : 1);
            gfx::SetColor(1, 1, 1);
            font.Draw(absl::StrFormat("%d.", i + 1), 128 + x, 134 + y);
        }
        // The place controls are clicked but not drawn: the armies stand for them.
        std::set<int> place_controls;
        for (int i = 0; i < kPlaces; i++) place_controls.insert(kPlace + i);
        kit::DrawControls(dialog_view, place_controls);
    }

    /** @brief OK, Cancel, Default, Earlier, Later, or a place chosen, let go or swapped. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == kOk) return Close(true);
        if (control_id == kCancel) return Close(false);
        if (control_id == kDefault) {
            Row() = g_app.g->map->fight_order[kNeutral];
            chosen = -1;
        } else if (control_id == kEarlier && chosen > 0) {
            Swap(chosen, chosen - 1);
            chosen--;
        } else if (control_id == kLater && chosen >= 0 && chosen < kPlaces - 1) {
            Swap(chosen, chosen + 1);
            chosen++;
        } else if (control_id >= kPlace && control_id < kPlace + kPlaces) {
            int place = control_id - kPlace;
            if (chosen == place)
                chosen = -1;
            else if (chosen < 0)
                chosen = place;
            else {
                Swap(chosen, place);
                chosen = -1;
            }
        }
        Refresh();
    }

    /** @brief The right button on a place (help sub-ids 45-71): its army-type box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        int type_id = TypeAt(sub - 45);
        const w2::ArmyType* type = type_id != w2::kNone ? g_app.g->types.ById(type_id) : nullptr;
        if (!type) return false;
        infobox::ArmyType(screen_x, screen_y, type_id,
                          w2::Slot{type->id, type->name, type->strength, type->time, type->cost,
                                   type->move, type->price},
                          g_app.player->index);
        return true;
    }

    /** @brief Enter keeps the new order, Escape puts the old one back. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Close(true);
        else if (key == "escape")
            Close(false);
    }
};
}  // namespace

/** @brief Opens Fight Order, keeping a copy of the row for Cancel. */
void Open() {
    auto dialog = std::make_shared<Order>();
    dialog->backup = g_app.g->map->fight_order[g_app.player->index];
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace fightorder
