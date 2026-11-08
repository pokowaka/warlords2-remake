/**
 * @file
 * @brief View > Stack: the selected stack laid out at length, to be grouped
 * the way the bar under the map groups it.
 *
 * It works on a copy of the bar's slots and its clicks are the bar's: an army
 * (336-343) joins the group that moves or leaves it, a mark (344-351) makes
 * its group the one, Group (334) puts the lot together and Ungroup (335)
 * breaks it up. OK (332) keeps it, Cancel (333) throws it away.
 *
 * Popup 0, (80, 60) 480x320: STACK.PCK's column heads with the combat cap,
 * and eight rows 30 apart from (112, 90): the mark, the army on a ring of its
 * group's colour (its shadow when not moving), name, strength -- and for the
 * group that moves its strength in a fight here -- moves, how it moves and
 * its bonus.
 */
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/combat.hpp"
#include "warlords/rules.hpp"

namespace stackui {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0, the whole dialog.
const int kDialog = 18;                        ///< BUTTON.DAT dialog number.
const int kOk = 332;                           ///< OK: keep the grouping.
const int kCancel = 333;                       ///< Cancel: throw it away.
const int kGroup = 334;                        ///< Group: the whole stack moves.
const int kUngroup = 335;                      ///< Ungroup: break the stack up.
const int kArmy = 336;                         ///< First of the eight army rows.
const int kMark = 344;                         ///< First of the eight marks.
const int kRows = 8;                           ///< Rows: a stack holds up to eight.

/** @brief True when the army flies: its type does, or it is a hero with a flying item. */
bool Flies(const w2::Army* army, const w2::ArmyType* type) {
    if (type->flies) return true;
    if (!army->Hero()) return false;
    for (w2::Item* item : army->items)
        if (item->type == w2::rules::kItemFlight) return true;
    return false;
}

/** @brief The stack dialog, working on a copy of the bar's slots. */
struct Stack : kit::Modal {
    screen::View dialog_view;      ///< Dialog 18's controls.
    w2::slots::Slots stack_slots;  ///< The copy of the bar's slots being regrouped.
    /** @brief Enables the controls the stack's size allows. */
    void Refresh() {
        auto& states = dialog_view.state;
        states[kOk] = states[kCancel] = w2::uidata::kNormal;
        states[kGroup] = stack_slots.n > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kUngroup] = states[kGroup];
        for (int i = 0; i < kRows; i++) {
            states[kArmy + i] = i < stack_slots.n ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kMark + i] = states[kArmy + i];
        }
    }
    /** @brief Draws the column heads and a row for each army of the stack. */
    void Draw() override {
        w2::Side* side = g_app.player;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        if (auto head = g_app.screen->ArtFor(46))
            gfx::Draw(head->image, gfx::NewQuad(0, 0, 400, 23), 80, 62);
        const Font& font = kit::GetFont(2);
        font.Draw(w2::Fmt("(max +%d)", g_app.g->map->combat_cap), 480, 66);
        // draws a cell of ABITS at (x, y)
        auto abits = [](int cell_x, int cell_y, int width, int height, int x, int y) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.abits, gfx::NewQuad(cell_x, cell_y, width, height), x, y);
        };
        std::vector<w2::Army*> moving;
        for (int i = 0; i < stack_slots.n; i++)
            if (stack_slots.in_group[i]) moving.push_back(stack_slots.army[i]);
        w2::Army* first = stack_slots.n > 0 ? stack_slots.army[0] : nullptr;
        auto fight_strengths =
                first ? w2::combat::StackStrengths(*g_app.g, moving, first->x, first->y)
                      : std::map<const w2::Army*, int>{};
        for (int i = 0; i < kRows; i++) {
            int x = 112, y = 90 + 30 * i;
            if (i < stack_slots.n && stack_slots.army[i]) {
                w2::Army* army = stack_slots.army[i];
                const w2::ArmyType* type = g_app.g->types.ById(army->type);
                if (stack_slots.mark[i] != w2::slots::kNomark)
                    abits(448, stack_slots.mark[i] == w2::slots::kTick ? 16 : 0, 32, 16, x - 32,
                          y + 5);
                kit::Army(army->type, side->index, x, y,
                          (side->index + stack_slots.group[i]) % 8 + 2, !stack_slots.in_group[i]);
                gfx::SetColor(1, 1, 1);
                font.Draw(army->Hero() ? army->name : type->name, x + 48, y + 5);
                int strength = army->strength;
                if (army->Hero()) strength += w2::combat::BattleItems(*army);
                font.Draw(w2::Fmt("%d", std::min(9, strength)), x + 168, y + 5);
                auto fight_strength = fight_strengths.find(army);
                if (stack_slots.in_group[i] && fight_strength != fight_strengths.end())
                    font.Draw(w2::Fmt("(%d)", fight_strength->second), x + 184, y + 5);
                font.Draw(w2::Fmt("%d", army->moves), x + 232, y + 5);
                // the left of the ABITS cell showing how it moves; -1 for on foot
                int move_cell_x = -1;
                if (Flies(army, type))
                    move_cell_x = 184;
                else if (army->at_sea)
                    move_cell_x = 424;
                else if (type->woods_move && type->hills_move)
                    move_cell_x = 216;
                else if (type->woods_move)
                    move_cell_x = 248;
                else if (type->hills_move)
                    move_cell_x = 152;
                if (move_cell_x >= 0) abits(move_cell_x, 30, 32, 10, x + 256, y + 8);
                gfx::SetColor(1, 1, 1);
                font.Draw(armybonus::BonusText(*type, army), x + 304, y + 5);
            } else {
                kit::Army(w2::kNone, side->index, x, y, 1);
            }
        }
        // the rows and marks are drawn above, not as controls
        std::set<int> drawn_by_hand;
        for (int i = 0; i < kRows; i++) {
            drawn_by_hand.insert(kArmy + i);
            drawn_by_hand.insert(kMark + i);
        }
        kit::DrawControls(dialog_view, drawn_by_hand);
    }
    /** @brief Closes the dialog and gives the bar the new grouping. */
    void Keep() {
        w2::slots::Slots kept = stack_slots;
        kit::Pop(this);
        if (g_app.selection) g_app.selection->slots = kept;
        front::AfterSlotChange();
    }
    /** @brief OK, Cancel, Group, Ungroup, or a row's army or mark, as the bar does. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control || dialog_view.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == kOk) return Keep();
        if (control_id == kCancel) {
            kit::Pop(this);
            return;
        }
        if (control_id == kGroup)
            w2::slots::All(stack_slots, *g_app.g);
        else if (control_id == kUngroup)
            w2::slots::Single(stack_slots, *g_app.g);
        else if (control_id >= kArmy && control_id < kArmy + kRows)
            w2::slots::Toggle(stack_slots, *g_app.g, control_id - kArmy);
        else if (control_id >= kMark && control_id < kMark + kRows)
            w2::slots::PickGroup(stack_slots, *g_app.g, control_id - kMark);
        Refresh();
    }
    /** @brief The right button on a row (sub-ids 37-44): the army's info box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        int row = sub - 37;
        if (row >= 0 && row < stack_slots.n)
            infobox::Army(screen_x, screen_y, stack_slots.army[row]);
        else
            infobox::ShowLines(screen_x, screen_y, "Select Army", "Select armies when present");
        return true;
    }
    /** @brief Enter keeps the grouping, Escape throws it away. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Keep();
        else if (key == "escape")
            kit::Pop(this);
    }
};
}  // namespace

/** @brief Opens the stack dialog on the selected stack, if any. */
void Open() {
    if (!g_app.selection) return;
    auto dialog = std::make_shared<Stack>();
    dialog->stack_slots = g_app.selection->slots;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace stackui
