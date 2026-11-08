/**
 * @file
 * @brief The rules of Warlords II, as read out of the original game.
 *
 * Every number here has a citation in docs/rules.md. The functions are pure:
 * they take state and dice, and return values. Turn order and mutation live
 * in game.cpp.
 */
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "warlords/types.hpp"

namespace w2::rules {

/** @brief Deliberate faults in the original, reproduced by default. */
struct Bugs {
    /// Post-battle hero experience checks the *attacker's* type array when
    /// deciding whether a surviving defender was a hero. Used by
    /// hero::BattleExperience.
    bool hero_experience_reads_attacker_types = true;
};
extern Bugs bugs;  ///< The faults in force; tests may switch them off.

constexpr int kNeutral = 15;         ///< The owner byte used for neutral cities.
constexpr int kMaxStack = 8;         ///< Armies on one tile.
constexpr int kProductionSlots = 4;  ///< Army types a city can build.
constexpr int kMaxMove = 99;         ///< Movement points are capped here at turn start.
constexpr int kMoveCarry = 2;        ///< Unused moves carried into the next turn, at most.
constexpr int kSeaMoves = 20;        ///< An army at sea gets this instead of its maximum.
constexpr int kSeaMinUpkeep = 4;     ///< The least upkeep an army at sea costs.
constexpr int kMaxHeroXp = 60;       ///< The most experience a hero can hold.

/** @brief A production purpose: weights on an army type's time, strength and move. */
struct Purpose {
    int time;  ///< Weight on a short build time.
    int str;   ///< Weight on strength.
    int move;  ///< Weight on movement.
};
/** @brief The weights of a production purpose (0-6; out-of-range values are clamped). */
const Purpose& PurposeOf(int purpose);

/// Garrison level (0-3) -> production purpose.
constexpr int kGarrisonPurpose[4] = {1, 6, 2, 3};

/// Item effects (docs/rules.md > Item types).
constexpr int kItemBattle = 1, kItemCommand = 2;
constexpr int kItemFlight = 5, kItemDoubleMove = 6, kItemGoldPerCity = 7;
constexpr int kItemStandard = 8;

/** @brief A city's defence: 1 below three production types, otherwise 2. */
inline int CityDefence(int n_types) {
    return n_types >= 3 ? 2 : 1;
}

/**
 * @brief Builds a city's production slots the way the original sets them up.
 *
 * ARMYTYPE stats, a random nudge per stat, then sorted by purchase price.
 */
std::vector<Slot> CitySlots(const std::vector<int>& produce_ids, const Types& types, Rng& rng);

/**
 * @brief The slot a city would build for a purpose, or kNone (its index).
 *
 * Scanned last to first with a strict >, so ties go to the later slot.
 */
int BestSlot(const std::vector<Slot>& slots, int purpose, const Types& types, bool side_bonus);

/** @brief The garrison level a city starts with; std::nullopt means "no garrison". */
std::optional<int> GarrisonLevel(bool owned, int neutral_cities, Rng& rng);

/** @brief A new army's stats, from the city slot that built it. */
Army ArmyFromSlot(const Slot& slot, bool enhanced);

}  // namespace w2::rules
