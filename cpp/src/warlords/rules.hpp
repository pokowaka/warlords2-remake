// The rules of Warlords II, as read out of WARLORD2.EXE.
//
// Every number here has a citation in docs/rules.md. The functions are pure:
// they take state and dice, and return values. Turn order and mutation live in
// game.cpp.
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "warlords/types.hpp"

namespace w2::rules {

/** Deliberate faults in the original, reproduced by default. */
struct Bugs {
    // Post-battle hero experience checks the *attacker's* type array when
    // deciding whether a surviving defender was a hero. Used by
    // hero::battleExperience.
    bool hero_experience_reads_attacker_types = true;
};
extern Bugs bugs;

constexpr int kNeutral = 15;         // the owner byte used for neutral cities
constexpr int kMaxStack = 8;         // armies on one tile
constexpr int kProductionSlots = 4;  // army types a city can build
constexpr int kMaxMove = 99;         // movement points are capped here at turn start
constexpr int kMoveCarry = 2;        // unused moves carried into the next turn, at most
constexpr int kSeaMoves = 20;        // an army at sea gets this instead of its maximum
constexpr int kSeaMinUpkeep = 4;
constexpr int kMaxHeroXp = 60;

// Production purposes: weights on (time, strength, move).
struct Purpose {
    int time, str, move;
};
const Purpose& PurposeOf(int p);

// Garrison level (0-3) -> production purpose. DS:0d60.
constexpr int kGarrisonPurpose[4] = {1, 6, 2, 3};

// Item effects (docs/rules.md > Item types).
constexpr int kItemBattle = 1, kItemCommand = 2;
constexpr int kItemFlight = 5, kItemDoubleMove = 6, kItemGoldPerCity = 7;
constexpr int kItemStandard = 8;

/** A city's defence: 1 below three production types, otherwise 2. */
inline int CityDefence(int n_types) {
    return n_types >= 3 ? 2 : 1;
}

/** Build a city's production slots the way setup_city_production does:
 *  ARMYTYPE stats, a random nudge per stat, then sorted by purchase price. */
std::vector<Slot> CitySlots(const std::vector<int>& produce_ids, const Types& types, Rng& rng);

/** The slot a city would build for a purpose, or none (its index).
 *  best_production_for: scanned last to first with a strict >, so ties go to
 *  the later slot. */
int BestSlot(const std::vector<Slot>& slots, int purpose, const Types& types, bool side_bonus);

/** The garrison level a city starts with; none means "no garrison". */
std::optional<int> GarrisonLevel(bool owned, int neutral_cities, Rng& rng);

/** A new army's stats, from the city slot that built it. */
Army ArmyFromSlot(const Slot& slot, bool enhanced);

}  // namespace w2::rules
