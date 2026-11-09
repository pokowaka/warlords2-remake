#include "warlords/slots.hpp"

#include <algorithm>
#include <deque>
#include <map>

#include "util/util.hpp"

namespace w2::slots {

namespace {
/** @brief The fight order row for a side (neutrals use row 8), indexed by army type. */
const std::vector<int>& Row(const Game& game, int side) {
    return game.GetMap().FightOrder()[side == kNone ? 8 : side];
}

/** @brief Where the army in `slot` stands in its side's fight order. */
int Rank(const Game& game, const Slots& slots, int slot) {
    return Row(game, slots.side)[slots.army[slot]->TypeIndex()];
}

/** @brief Swaps two slots' armies and groups (not their marks, which are worked out afresh). */
void Swap(Slots& slots, int first, int second) {
    std::swap(slots.army[first], slots.army[second]);
    std::swap(slots.group[first], slots.group[second]);
    std::swap(slots.in_group[first], slots.in_group[second]);
}

/**
 * @brief Insertion-sorts the slots so each group is contiguous: by group, then
 * by fight order within it.
 */
void Order(Slots& slots, const Game& game) {
    for (int i = 1; i < slots.n; i++) {
        int position = i;
        while (position > 0 && (slots.group[position] < slots.group[position - 1] ||
                                (slots.group[position] == slots.group[position - 1] &&
                                 Rank(game, slots, position - 1) < Rank(game, slots, position)))) {
            Swap(slots, position, position - 1);
            position--;
        }
    }
}

/**
 * @brief Works out the marks, and which group is the one that moves.
 *
 * A group is active only when exactly its armies are the moving ones.
 */
void Remark(Slots& slots) {
    for (int i = 0; i < slots.n; i++)
        slots.mark[i] = (i > 0 && slots.group[i] == slots.group[i - 1]) ? kNomark : kCross;
    slots.active.reset();
    int head = -1;
    for (int i = 0; i < slots.n; i++)
        if (slots.in_group[i]) {
            head = i;
            break;
        }
    if (head < 0) return;
    for (int i = 0; i < slots.n; i++) {
        if (slots.in_group[i] != (slots.group[i] == slots.group[head])) return;
    }
    slots.mark[head] = kTick;
    slots.active = slots.group[head];
}

/** @brief Re-marks the slots after a change, re-sorting them first if `resort`. */
void Refresh(Slots& slots, const Game& game, bool resort = false) {
    if (resort) Order(slots, game);
    Remark(slots);
}
}  // namespace

std::set<Army*> Clicked(const Game& game, const std::vector<Army*>& armies, int side,
                        Army* anchor) {
    const auto& fight_order = Row(game, side);
    if (!anchor) {
        for (Army* army : armies) {
            if (!anchor || fight_order[army->TypeIndex()] > fight_order[anchor->TypeIndex()])
                anchor = army;
        }
    }
    if (!anchor) return {};
    int key = anchor->Group();
    std::set<Army*> picked = {anchor};
    if (key != kUngrouped) {
        for (Army* army : armies)
            if (army->Group() == key) picked.insert(army);
    }
    return picked;
}

Slots Build(const Game& game, const std::vector<Army*>& armies, int side,
            const std::set<Army*>* selected) {
    Slots slots;
    slots.n = std::min(static_cast<int>(armies.size()), kMax);
    slots.side = side;
    std::vector<Army*> list(armies.begin(), armies.begin() + slots.n);
    std::set<Army*> chosen = selected ? *selected : Clicked(game, list, side);

    // The original's order: the remembered groups first, and within a group the
    // army highest in the fight order.
    std::map<Army*, int> original_index;
    for (int i = 0; i < slots.n; i++) original_index[list[i]] = i;
    const auto& fight_order = Row(game, side);
    LuaSort(list, [&](Army* lhs, Army* rhs) {
        int lhs_group = lhs->Group(), rhs_group = rhs->Group();
        if (lhs_group != rhs_group) return lhs_group > rhs_group;
        int lhs_rank = fight_order[lhs->TypeIndex()], rhs_rank = fight_order[rhs->TypeIndex()];
        if (lhs_rank != rhs_rank) return lhs_rank > rhs_rank;
        return original_index[lhs] < original_index[rhs];
    });
    for (int i = 0; i < slots.n; i++) slots.army[i] = list[i];

    // Number the groups off down the list, as the original does: each run of
    // armies sharing a remembered group becomes one group, each ungrouped
    // army a group of its own.
    int group_number = -1;
    bool have_prev = false;
    int previous = 0;
    for (int i = 0; i < slots.n; i++) {
        int key = slots.army[i]->Group();
        if (!(have_prev && key == previous && key != kUngrouped && group_number >= 0))
            group_number++;
        slots.group[i] = group_number;
        slots.in_group[i] = chosen.count(slots.army[i]) > 0;
        previous = key;
        have_prev = true;
    }
    Refresh(slots, game);
    return slots;
}

void Toggle(Slots& slots, const Game& game, int slot) {
    if (slot < 0 || slot >= slots.n) return;
    if (!slots.in_group[slot]) {
        for (int j = 0; j < slots.n; j++) {
            if (slots.in_group[j]) {
                slots.group[slot] = slots.group[j];
                break;
            }
        }
        slots.in_group[slot] = true;
    } else {
        std::map<int, int> members;
        for (int j = 0; j < slots.n; j++) members[slots.group[j]]++;
        if (members[slots.group[slot]] != 1) {
            int free_group = 0;
            while (members.count(free_group) && members[free_group]) free_group++;
            slots.in_group[slot] = false;
            slots.group[slot] = free_group;
        }
    }
    Refresh(slots, game, true);
}

void PickGroup(Slots& slots, const Game& game, int slot) {
    if (slot < 0 || slot >= slots.n) return;
    int wanted_group = slots.group[slot];
    for (int j = 0; j < slots.n; j++) slots.in_group[j] = slots.group[j] == wanted_group;
    Refresh(slots, game);
}

void All(Slots& slots, const Game& game) {
    for (int i = 0; i < slots.n; i++) {
        slots.group[i] = 0;
        slots.in_group[i] = true;
    }
    Refresh(slots, game, true);
}

void Single(Slots& slots, const Game& game) {
    for (int i = 0; i < slots.n; i++) {
        slots.group[i] = i;
        slots.in_group[i] = false;
    }
    if (slots.n > 0) slots.in_group[0] = true;
    Refresh(slots, game, true);
}

std::vector<Army*> Selected(const Slots& slots) {
    std::vector<Army*> moving;
    for (int i = 0; i < slots.n; i++)
        if (slots.in_group[i]) moving.push_back(slots.army[i]);
    return moving;
}

int Moves(const Slots& slots) {
    int least = kNone;
    for (int i = 0; i < slots.n; i++) {
        if (slots.in_group[i]) {
            int army_moves = slots.army[i]->Moves();
            if (least == kNone || army_moves < least) least = army_moves;
        }
    }
    return least == kNone ? 0 : least;
}

bool Grouped(const Slots& slots) {
    if (slots.n == 0) return false;
    if (slots.n == 1) return true;
    for (int i = 0; i < slots.n; i++)
        if (!slots.in_group[i]) return false;
    return true;
}

std::optional<Slots> Keep(const Slots& slots, const Game& game, const std::vector<Army*>& armies) {
    std::set<Army*> was_moving;
    for (int i = 0; i < slots.n; i++)
        if (slots.in_group[i]) was_moving.insert(slots.army[i]);
    std::set<Army*> still_moving;
    for (Army* army : armies)
        if (was_moving.count(army)) still_moving.insert(army);
    if (armies.empty()) return std::nullopt;
    return Build(game, armies, slots.side, still_moving.empty() ? nullptr : &still_moving);
}

void Commit(Slots& slots, Game& game) {
    std::map<int, int> members;
    for (int i = 0; i < slots.n; i++) members[slots.group[i]]++;

    // The real group ids (2 and up) these armies already had, first come first
    // served, for the moving group to reuse.
    std::set<int> spare_set;
    std::deque<int> spare;
    for (int i = 0; i < slots.n; i++) {
        int army_group = slots.army[i]->Group();
        if (army_group > 1 && !spare_set.count(army_group)) {
            spare_set.insert(army_group);
            spare.push_back(army_group);
        }
    }
    /** @brief A group id for the moving group: a spare one, or the lowest unused from 2 up. */
    auto claim = [&]() {
        if (!spare.empty()) {
            int group_id = spare.front();
            spare.pop_front();
            return group_id;
        }
        std::set<int> used;
        for (Army* army : game.Armies()) used.insert(army->Group());
        int fresh = 2;
        while (used.count(fresh)) fresh++;
        return fresh;
    };

    std::map<int, int> group_ids;
    for (int i = 0; i < slots.n; i++) {
        int slot_group = slots.group[i];
        if (!group_ids.count(slot_group)) {
            if (members[slot_group] == 1)
                group_ids[slot_group] = kUngrouped;
            else if (slots.active && slot_group == *slots.active)
                group_ids[slot_group] = claim();
            else
                group_ids[slot_group] = 1;
        }
        slots.army[i]->SetGroup(group_ids[slot_group]);
    }
}

}  // namespace w2::slots
