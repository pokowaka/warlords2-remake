/**
 * @file
 * @brief View > Army Bonus: every army type, in the side's fight order,
 * with its strength, moves, way of moving and bonus.
 *
 * Popup 0, (80, 60) 480x320: "Army Bonus" in font 1, a box raised with a
 * (4, 2) bevel and outlined in black, STACK.PCK's column heads, and six rows
 * 30 apart from y = 148: the army on a ring of the side's colour, name,
 * strength, moves, how it moves, and its bonus (BonusText, group 163).
 * Dialog 34: Done (490), 491/492 a row up and down and 493/494 six. Of the
 * 29 places only the first 27 are shown.
 */
#include <map>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/combat.hpp"

namespace armybonus {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0.
/// Dialog 34 and its controls: Done, a row up/down, six rows up/down.
const int kDialog = 34, kDone = 490, kUp = 491, kDown = 492, kPageUp = 493, kPageDown = 494;
const int kRows = 6;           ///< Rows shown at once.
const int kPlaces = 27;        ///< Places in the fight order shown (of 29).
const int kBonusGroup = 0xa3;  ///< STRING.DAT group 163: the bonus wordings.

/** @brief The Army Bonus table, scrolled a row or a page at a time. */
struct Bonus : kit::Modal {
    screen::View dialog_view;    ///< Dialog 34's controls and their states.
    std::map<int, int> by_rank;  ///< Place in the fight order -> army type id.
    int top_row = 0;             ///< The place shown in the first row.

    /** @brief Enables the scroll buttons that have somewhere to go. */
    void Refresh() {
        auto& states = dialog_view.state;
        states[kDone] = w2::uidata::kNormal;
        int up_state = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        int down_state = top_row + kRows < kPlaces ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kUp] = up_state;
        states[kPageUp] = up_state;
        states[kDown] = down_state;
        states[kPageDown] = down_state;
    }

    /** @brief Draws the frame, the column heads and the visible rows of army types. */
    void Draw() override {
        w2::Side* side = g_app.player;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0xa4, 0), 320, 62);
        kit::Bevel(124, 146, 434, 186, 4, 2);
        kit::SetPal(0);
        kit::Outline(125, 147, 432, 184);
        if (auto stack = g_app.screen->ArtFor(46)) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(stack->image, gfx::NewQuad(188, 0, 216, 22), 256, 125);
        }
        const Font& font = kit::GetFont(2);
        for (int i = 0; i < kRows; i++) {
            auto ranked = by_rank.find(top_row + i);
            const w2::ArmyType* type =
                    ranked != by_rank.end() ? g_app.g->types.ById(ranked->second) : nullptr;
            if (type) {
                int y = 148 + 30 * i;
                kit::Army(type->id, side->index, 128, y, side->index + 2);
                gfx::SetColor(1, 1, 1);
                font.Draw(type->name, 168, y + 5);
                font.Draw(w2::Fmt("%d", type->strength), 280, y + 5);
                font.Draw(w2::Fmt("%d", type->move), 328, y + 5);
                // The movement icon's x in ABITS: flies, woods and hills, woods, hills.
                int icon_x = -1;
                if (type->flies)
                    icon_x = 184;
                else if (type->woods_move && type->hills_move)
                    icon_x = 216;
                else if (type->woods_move)
                    icon_x = 248;
                else if (type->hills_move)
                    icon_x = 152;
                if (icon_x >= 0) {
                    gfx::SetColor(1, 1, 1);
                    gfx::Draw(g_app.abits, gfx::NewQuad(icon_x, 30, 32, 10), 352, y + 8);
                }
                gfx::SetColor(1, 1, 1);
                font.Draw(BonusText(*type), 400, y + 6);
            }
        }
        kit::DrawControls(dialog_view);
    }

    /** @brief Done closes; the arrows scroll by a row or by a page. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kDone) {
            kit::Pop(this);
            return;
        }
        if (control->id == kUp)
            top_row--;
        else if (control->id == kDown)
            top_row++;
        else if (control->id == kPageUp)
            top_row = std::max(0, top_row - kRows);
        else if (control->id == kPageDown)
            top_row = std::min(kPlaces - kRows, top_row + kRows);
        Refresh();
    }

    /** @brief Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief The first of the type's bonuses, as group 163 words it.
 *
 * An army at sea or a hero gets its own line instead; a hero's is his
 * command bonus. The checks go in the game's order, so a type with several
 * bonuses shows the first found. The indices are offsets into the type's
 * bonus bytes.
 * @param type The army type.
 * @param army The army itself, when there is one (at sea, a hero).
 */
std::string BonusText(const w2::ArmyType& type, const w2::Army* army) {
    const auto& bonus = type.bonus;
    auto say = [](int line, int value = 0) {
        return w2::Format(kit::Text(kBonusGroup, line), value);
    };
    if (army && army->at_sea) return say(1);
    if (army && army->Hero()) {
        int strength = std::min(9, army->strength + w2::combat::BattleItems(*army));
        return say(2, std::min(6, w2::combat::kHeroTable[std::max(0, strength)] +
                                          w2::combat::CommandItems(*army)));
    }
    if (bonus[52] == 2) return say(3);
    if (bonus[52] == 3) return say(4);
    if (bonus[48] != 0) return say(5, bonus[42]);
    if (bonus[46] != 0 && bonus[44] != 0 && bonus[42] != 0 && bonus[40] != 0)
        return say(16, bonus[40]);
    if (bonus[52] == 1) return say(6);
    // {bonus byte, group 163 line}, checked in this order.
    const int order[9][2] = {{50, 7},  {46, 8},  {44, 9},  {42, 10}, {40, 11},
                             {38, 12}, {36, 13}, {34, 14}, {32, 15}};
    for (auto& entry : order)
        if (bonus[entry[0]] != 0) return say(entry[1], bonus[entry[0]]);
    return say(0);
}

/** @brief Opens Army Bonus with the player's fight order. */
void Open() {
    auto dialog = std::make_shared<Bonus>();
    const auto& row = g_app.g->map->fight_order[g_app.player->index];
    for (int rank = 0; rank <= 28 && rank < static_cast<int>(row.size()); rank++)
        dialog->by_rank[row[rank]] = rank;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace armybonus
