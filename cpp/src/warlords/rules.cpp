#include "warlords/rules.hpp"

#include <algorithm>
#include <cstdlib>

#include "util/util.hpp"

namespace w2::rules {

Bugs bugs;

const Purpose& PurposeOf(int purpose) {
    // Indexed by purpose; purposes 3, 4 and 5 weigh alike, 4 only takes fliers.
    static const Purpose kTable[7] = {
        {0, 0, 0},   {10, 4, 1},   // quick and cheap
        {10, 10, 1},               // balanced
        {5, 10, 1},                // strongest
        {5, 10, 1},                // flying types only
        {5, 10, 1},  {10, 1, 10},  // fastest
    };
    return kTable[std::clamp(purpose, 0, 6)];
}

std::vector<Slot> CitySlots(const std::vector<int>& produce_ids, const Types& types, Rng& rng) {
    std::vector<Slot> slots;
    for (int type_id : produce_ids) {
        const ArmyType* army_type = types.ById(type_id);
        int strength = army_type->strength, time = army_type->time, cost = army_type->cost,
            move = army_type->move;
        if (rng.Chance(10)) {
            if (rng.Chance(60))
                strength = std::min(9, strength + 1);
            else
                strength = std::max(1, strength - 1);
        }
        if (rng.Chance(20)) {
            int roll = rng.Dice(1, 100, 0);
            move += roll < 10 ? 4 : roll < 60 ? 2 : roll < 95 ? -2 : -4;
            move = std::max(2, move);
        }
        move = std::max(6, move);
        if (rng.Chance(10)) {
            int quarter = cost / 4;
            cost += rng.Chance(60) ? -quarter : quarter;
        }
        if (rng.Chance(10)) {
            time = rng.Chance(60) ? std::max(1, time - 1) : time + 1;
        }
        slots.push_back(Slot{type_id, army_type->name, strength, time, cost, move,
                             std::abs(army_type->price)});
    }
    LuaSort(slots, [](const Slot& left, const Slot& right) { return left.price < right.price; });
    return slots;
}

int BestSlot(const std::vector<Slot>& slots, int purpose, const Types& types, bool side_bonus) {
    const Purpose& weights = rules::PurposeOf(purpose);
    int best = kNone, best_score = 0;
    for (int i = static_cast<int>(slots.size()) - 1; i >= 0; i--) {
        const Slot& slot = slots[i];
        const ArmyType* army_type = types.ById(slot.type);
        if (purpose != 4 || army_type->flies) {
            int str = std::min(9, slot.strength + (side_bonus ? 2 : 0));
            if (army_type->siege) str += 2;
            int time = slot.time + ((str < 3 && purpose != 6) ? 1 : 0);
            int score = (10 - std::min(10, time)) * weights.time + str * weights.str +
                        (slot.move * weights.move) / 2;
            if (score > best_score) {
                best = i;
                best_score = score;
            }
        }
    }
    return best;
}

std::optional<int> GarrisonLevel(bool owned, int neutral_cities, Rng& rng) {
    if (owned) return 3;
    if (neutral_cities <= 0) return std::nullopt;
    return std::min(3, rng.Dice(1, 4, 0) + neutral_cities - 2);
}

Army ArmyFromSlot(const Slot& slot, bool enhanced) {
    Army army;
    army.type = slot.type;
    army.name = slot.name;
    army.strength = enhanced ? std::min(9, slot.strength + 2) : slot.strength;
    army.max_moves = slot.move;
    army.upkeep = slot.cost / 2;
    return army;
}

}  // namespace w2::rules
