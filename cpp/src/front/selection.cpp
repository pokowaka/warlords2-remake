/**
 * @file
 * @brief Stack selection, bottom-bar slot grouping, stranded drowning, and the army cycle.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/uidata.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

// ------------------------------------------------------------------ selection

/** @brief The player's armies on the tile, as many as a stack may hold. */
std::vector<Army*> SelectableAt(int x, int y) {
    std::vector<Army*> out;
    for (Army* army : game::ArmiesAt(*g_app.g, x, y))
        if (army->OwnerIndex() == g_app.player->Index() &&
            static_cast<int>(out.size()) < w2::rules::kMaxStack)
            out.push_back(army);
    return out;
}

/** @brief Picks up whatever the slots now say moves, and remembers the grouping. */
void SyncSelection() {
    if (!g_app.selection) return;
    auto& sel = *g_app.selection;
    slots_mod::Commit(sel.slots, *g_app.g);
    sel.stack = slots_mod::Selected(sel.slots);
    // being in the group that moves puts an army back in the cycle and marks
    // it offered for this pass
    for (Army* army : sel.stack) {
        army->SetFortified(false);
        army->SetOffered(true);
    }
}

/**
 * @brief Puts the slots back over a new list of armies, keeping the group.
 *
 * With none of them left the selection goes.
 */
void Reslot(const std::vector<Army*>& armies) {
    if (!g_app.selection) return;
    auto kept = armies.empty() ? std::nullopt
                               : slots_mod::Keep(g_app.selection->slots, *g_app.g, armies);
    if (!kept) {
        g_app.selection.reset();
        return;
    }
    g_app.selection->slots = *kept;
    g_app.selection->x = kept->army[0]->X();
    g_app.selection->y = kept->army[0]->Y();
    SyncSelection();
}

/// "Alas! %d armies have been sunk" / "due to lack of a boat!" (STRING.DAT 168).
static const int kSunkGroup = 0xa8;

/**
 * @brief Drowns the player's armies stranded on water without a boat.
 *
 * On one tile, or with kNone on all of them. If any sank: a splash, the
 * selection put down, the view on the tile, and the message, `after` running
 * once it is read.
 *
 * @return Whether any did; if not, `after` runs at once.
 */
bool DrownStranded(int x, int y, kit::Done after) {
    int sunk = game::SinkStranded(*g_app.g, *g_app.player, x, y);
    if (sunk == 0) {
        if (after) after();
        return false;
    }
    sound::Effect("splash");
    g_app.selection.reset();
    g_app.route.reset();
    StratDirty();
    if (x != kNone) CentreOn(x, y);
    std::string line =
            sunk == 1 ? kit::Text(kSunkGroup, 2) : Format(kit::Text(kSunkGroup, 0), sunk);
    search::ShowMessage(line, kit::Text(kSunkGroup, 1), after);
    return true;
}

/**
 * @brief Selects the player's stack on the tile, or opens the city there.
 * @param pick The army clicked, which the slots select; nullptr for the
 *        usual choice.
 */
void Select(int x, int y, Army* pick) {
    // picking a stack up on the water drowns whoever has no boat
    if (DrownStranded(x, y)) return;
    auto stack = SelectableAt(x, y);
    if (stack.empty()) {
        g_app.selection.reset();
        if (City* city = game::CityAt(*g_app.g, x, y)) front::OpenCity(city);
        return;
    }
    // a click selects one army, not the stack (docs/re/ui.md > The army slots)
    auto chosen = slots_mod::Clicked(*g_app.g, stack, g_app.player->Index(), pick);
    Selection sel;
    sel.x = x;
    sel.y = y;
    sel.slots = slots_mod::Build(*g_app.g, stack, g_app.player->Index(), &chosen);
    g_app.selection = sel;
    SyncSelection();
    RefreshRoute();
    // the tutorial on picking a stack up
    std::vector<std::string> moments = {"move"};
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            City* neighbour = game::CityAt(*g_app.g, x + dx, y + dy);
            if (neighbour && neighbour->OwnerIndex() != g_app.player->Index() &&
                !neighbour->IsRazed() && moments.size() == 1)
                moments.push_back("fight");
        }
    }
    Army* lead = g_app.selection->stack.empty() ? nullptr : g_app.selection->stack[0];
    if (lead && lead->Hero() && g_app.g->GetMap().SiteAt(x, y)) moments.push_back("search");
    tutorial::Chain(moments);
}
/** @brief Selects what is on the tile, as a click with the select pointer does. */
void SelectAt(int x, int y) {
    Select(x, y);
}

// ------------------------------------------------------------- the army cycle

// The five buttons above the pad walk you through your armies one stack at
// a time. The next army is the nearest of the eligible ones to where the
// cycle last stopped; one exactly there counts as far away.

/**
 * @brief Where a side's capital now stands: its own, or the city it moved
 *        its capital to while it does not hold its own.
 */
std::pair<int, int> CapOf(const Side* side) {
    if (side->CurrentCapitalPos()) return side->CurrentCapitalPos()->ToPair();
    if (g_app.g && g_app.g->HasMap()) {
        if (const City* cap = side->CapitalCity(g_app.g->GetMap())) return {cap->X(), cap->Y()};
    }
    w2::TilePos orig = side->OriginCapitalPos();
    return {orig.x == kNone ? 0 : orig.x, orig.y == kNone ? 0 : orig.y};
}

/** @brief Starts the army cycle over from the player's capital. */
void CycleReset() {
    g_app.cursor = CapOf(g_app.player);
}

/**
 * @brief How far the army is from where the cycle last stopped (smaller comes
 *        first); one standing exactly there counts as far away.
 */
static int CycleRank(const Army* army) {
    int distance =
            std::abs(army->X() - g_app.cursor->first) + std::abs(army->Y() - g_app.cursor->second);
    return distance == 0 ? 9000 : distance;
}

/**
 * @brief The army the cycle offers next, and moves the cycle's cursor to it.
 *
 * Armies not yet offered this pass come first; once all have been, the pass
 * starts over. nullptr when every army has moved, is fortified or is done.
 */
static Army* NextArmy() {
    if (!g_app.cursor) CycleReset();
    Army* fresh = nullptr;
    Army* used = nullptr;
    for (Army* army : g_app.g->Armies()) {
        if (army->OwnerIndex() == g_app.player->Index() && !army->InTransit() &&
            !army->IsFortified() && !army->IsDone()) {
            if (army->IsOffered()) {
                if (!used || CycleRank(army) < CycleRank(used)) used = army;
            } else if (!fresh || CycleRank(army) < CycleRank(fresh)) {
                fresh = army;
            }
        }
    }
    if (!fresh) {
        if (!used) return nullptr;
        for (Army* army : g_app.g->Armies())
            if (army->OwnerIndex() == g_app.player->Index()) army->SetOffered(false);
        fresh = used;
    }
    g_app.cursor = std::make_pair(fresh->X(), fresh->Y());
    return fresh;
}

/** @brief Selects the cycle's next army and centres on it, or says none is left. */
void SelectNext() {
    Army* army = NextArmy();
    if (!army) {
        g_app.selection.reset();
        g_app.route.reset();
        Say("Every army has moved.");
        sound::Effect("chord");  // nothing left
        return;
    }
    sound::Effect("ding");
    Select(army->X(), army->Y(), army);
    CentreOn(army->X(), army->Y());
}

/** @brief Takes the selection out of the cycle for the rest of this turn (control 175). */
void QuitArmy() {
    if (!g_app.selection) {
        SelectNext();
        return;
    }
    for (Army* army : g_app.selection->stack) army->SetDone(true);
    g_app.route.reset();
    SelectNext();
}

/** @brief Digs the selection in: out of the cycle until picked up again (control 176). */
void Fortify() {
    if (!g_app.selection) {
        SelectNext();
        return;
    }
    for (Army* army : g_app.selection->stack) {
        army->SetFortified(true);
        army->ClearTarget();
    }
    g_app.route.reset();
    SelectNext();
}

/// A second click within 40 BIOS ticks of the last counts as a double click.
static const double kDoubleClick = 40 / 18.2;

/**
 * @brief A click with the select pointer on a tile.
 *
 * On the selected stack's own tile, soon after the last such click, it
 * groups the whole stack as Space does; otherwise it selects what is there.
 */
void SelectClick(int tile_x, int tile_y) {
    double now = w2::Now(), last = g_app.last_select_click;
    g_app.last_select_click = now;
    if (g_app.selection && g_app.selection->x == tile_x && g_app.selection->y == tile_y &&
        last >= 0 && now - last < kDoubleClick) {
        slots_mod::All(g_app.selection->slots, *g_app.g);
        AfterSlotChange();
        RefreshControls();
        return;
    }
    Select(tile_x, tile_y);
}

/** @brief Puts the selection down (control 178). */
void Deselect() {
    g_app.selection.reset();
    g_app.route.reset();
    g_app.walk.reset();
}

}  // namespace front
