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
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/combat.hpp"
#include "warlords/rules.hpp"

namespace stackui {

namespace {

using StrengthMap = std::map<const w2::Army*, int>;

constexpr kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0, the whole dialog.
constexpr int kDialog = 18;                        ///< BUTTON.DAT dialog number.
constexpr int kOk = 332;                           ///< OK: keep the grouping.
constexpr int kCancel = 333;                       ///< Cancel: throw it away.
constexpr int kGroup = 334;                        ///< Group: the whole stack moves.
constexpr int kUngroup = 335;                      ///< Ungroup: break the stack up.
constexpr int kArmy = 336;                         ///< First of the eight army rows.
constexpr int kMark = 344;                         ///< First of the eight marks.
constexpr int kRows = 8;                           ///< Rows: a stack holds up to eight.

/** @brief True when the army flies: its type does, or it is a hero with a flying item. */
bool Flies(const w2::Army* army, const w2::ArmyType* type) {
    if (type->flies) return true;
    if (!army->Hero()) return false;
    for (w2::Item* item : army->Items()) {
        if (item->type == w2::rules::kItemFlight) return true;
    }
    return false;
}

/** @brief The stack dialog, working on a copy of the bar's slots. */
class StackModal final : public kit::Modal {
  public:
    StackModal(w2::UiContext& ctx, const w2::slots::Slots& initial_slots)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), stack_slots_(initial_slots) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the column heads and a row for each army of the stack. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Side* side = ctx.Player();
        w2::Game& game = ctx.ActiveGame();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        if (const screen::Art* head = ctx.ControlArt(46)) {
            ren.Draw(head->image, gfx::NewQuad(0, 0, 400, 23), 80, 62);
        }
        const Font& font = kit::GetFont(ctx, 2);
        font.Draw(absl::StrFormat("(max +%d)", game.GetMap().CombatCap()), 480, 66);
        // draws a cell of ABITS at (x, y)
        auto abits = [&ren, &ctx](int cell_x, int cell_y, int width, int height, int x, int y) {
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(cell_x, cell_y, width, height), x, y);
        };
        std::vector<w2::Army*> moving;
        for (int i = 0; i < stack_slots_.n; i++) {
            if (stack_slots_.in_group[i]) moving.push_back(stack_slots_.army[i]);
        }
        w2::Army* first = stack_slots_.n > 0 ? stack_slots_.army[0] : nullptr;
        StrengthMap fight_strengths =
                first ? w2::combat::StackStrengths(game, moving, first->X(), first->Y())
                      : StrengthMap{};
        for (int i = 0; i < kRows; i++) {
            int x = 112;
            int y = 90 + 30 * i;
            if (i < stack_slots_.n && stack_slots_.army[i]) {
                w2::Army* army = stack_slots_.army[i];
                const w2::ArmyType* type = game.ArmyTypes().ById(army->Type());
                if (stack_slots_.mark[i] != w2::slots::kNomark) {
                    abits(448, stack_slots_.mark[i] == w2::slots::kTick ? 16 : 0, 32, 16, x - 32,
                          y + 5);
                }
                kit::Army(ctx, army->TypeIndex(), side->Index(), x, y,
                          (side->Index() + stack_slots_.group[i]) % 8 + 2,
                          !stack_slots_.in_group[i]);
                ren.SetColor(1, 1, 1);
                font.Draw(army->Hero() ? army->Name() : type->name, x + 48, y + 5);
                int strength = army->Strength();
                if (army->Hero()) strength += w2::combat::BattleItems(*army);
                font.Draw(absl::StrFormat("%d", std::min(9, strength)), x + 168, y + 5);
                auto fight_strength = fight_strengths.find(army);
                if (stack_slots_.in_group[i] && fight_strength != fight_strengths.end()) {
                    font.Draw(absl::StrFormat("(%d)", fight_strength->second), x + 184, y + 5);
                }
                font.Draw(absl::StrFormat("%d", army->Moves()), x + 232, y + 5);
                // the left of the ABITS cell showing how it moves; -1 for on foot
                int move_cell_x = -1;
                if (Flies(army, type)) {
                    move_cell_x = 184;
                } else if (army->IsAtSea()) {
                    move_cell_x = 424;
                } else if (type->woods_move && type->hills_move) {
                    move_cell_x = 216;
                } else if (type->woods_move) {
                    move_cell_x = 248;
                } else if (type->hills_move) {
                    move_cell_x = 152;
                }
                if (move_cell_x >= 0) abits(move_cell_x, 30, 32, 10, x + 256, y + 8);
                ren.SetColor(1, 1, 1);
                font.Draw(armybonus::BonusText(ctx, *type, army), x + 304, y + 5);
            } else {
                kit::Army(ctx, w2::kNone, side->Index(), x, y, 1);
            }
        }
        // the rows and marks are drawn above, not as controls
        kit::ControlIdSet drawn_by_hand;
        for (int i = 0; i < kRows; i++) {
            drawn_by_hand.insert(kArmy + i);
            drawn_by_hand.insert(kMark + i);
        }
        kit::DrawControls(ctx, dialog_view_, drawn_by_hand);
    }

    /** @brief OK, Cancel, Group, Ungroup, or a row's army or mark, as the bar does. */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control || dialog_view_.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == kOk) {
            Keep();
            return;
        }
        if (control_id == kCancel) {
            PopSelf();
            return;
        }
        w2::Game& game = Ctx().ActiveGame();
        if (control_id == kGroup) {
            w2::slots::All(stack_slots_, game);
        } else if (control_id == kUngroup) {
            w2::slots::Single(stack_slots_, game);
        } else if (control_id >= kArmy && control_id < kArmy + kRows) {
            w2::slots::Toggle(stack_slots_, game, control_id - kArmy);
        } else if (control_id >= kMark && control_id < kMark + kRows) {
            w2::slots::PickGroup(stack_slots_, game, control_id - kMark);
        }
        Refresh();
    }

    /** @brief The right button on a row (sub-ids 37-44): the army's info box. */
    bool Info(int sub, int screen_x, int screen_y) override {
        w2::UiContext& ctx = Ctx();
        int row = sub - 37;
        if (row >= 0 && row < stack_slots_.n) {
            infobox::Army(ctx, screen_x, screen_y, stack_slots_.army[row]);
        } else {
            infobox::ShowLines(ctx, screen_x, screen_y, "Select Army",
                               "Select armies when present");
        }
        return true;
    }

    /** @brief Enter keeps the grouping, Escape throws it away. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Keep();
        } else if (key == "escape") {
            PopSelf();
        }
    }

  private:
    /** @brief Enables the controls the stack's size allows. */
    void Refresh() {
        auto& states = dialog_view_.state;
        states[kOk] = states[kCancel] = w2::uidata::kNormal;
        states[kGroup] = stack_slots_.n > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kUngroup] = states[kGroup];
        for (int i = 0; i < kRows; i++) {
            states[kArmy + i] = i < stack_slots_.n ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kMark + i] = states[kArmy + i];
        }
    }

    /** @brief Closes the dialog and gives the bar the new grouping. */
    void Keep() {
        w2::UiContext& ctx = Ctx();
        w2::slots::Slots kept = stack_slots_;
        PopSelf();
        if (w2::Selection* sel = ctx.ActiveSelection()) sel->slots = kept;
        ctx.Actions().AfterSlotChange();
    }

    screen::View dialog_view_;      ///< Dialog 18's controls.
    w2::slots::Slots stack_slots_;  ///< The copy of the bar's slots being regrouped.
};

}  // namespace

/** @brief Opens the stack dialog on the selected stack, if any. */
void Open(w2::UiContext& ctx) {
    const w2::Selection* sel = ctx.ActiveSelection();
    if (!sel) return;
    kit::Push(ctx, std::make_shared<StackModal>(ctx, sel->slots));
}

}  // namespace stackui
