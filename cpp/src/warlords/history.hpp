/**
 * @file
 * @brief The game's history: what the History menu plays back.
 *
 * See docs/formats/history.md. Once a round, when the turn counter goes up,
 * a record is taken of every side's gold, score and city count, every city's
 * owner, and the round's deeds -- kept as they happen, two a side, a lower
 * type counting for more.
 */
#pragma once

#include <string>

#include "warlords/types.hpp"

namespace w2::history {

using w2::DeedType;  // the kinds of deed, in types.hpp beside Deed

// A deed's first value where it is not a city, a side or an item:
constexpr int kKilledInBattle = -1;   ///< kHeroKilled: in the field, not in a city.
constexpr int kKilledSearching = -2;  ///< kHeroKilled: by a ruin's guardian.
constexpr int kWonByName = -1;        ///< kCityWon: the hero named in the deed took it.
/// kFound: allies, a sage or gold rather than an item.
constexpr int kFoundAllies = 100, kFoundSage = 101, kFoundGold = 102;

/** @brief The history is recorded for the first 201 rounds only. */
constexpr int kLastRecordedTurn = 201;
/**
 * @brief A round's record keeps at most ten deeds.
 *
 * Every side's first, then second ones while there is room.
 */
constexpr int kMaxDeedsPerRound = 10;

/**
 * @brief Notes a deed of a side's for this round's record.
 *
 * A side keeps its two most important deeds of the round; a new one replaces
 * the less important of the two when it counts for more.
 * @param subject The deed's first value (see DeedType).
 * @param object The deed's second value (see DeedType).
 * @param name The hero's or the side's name.
 */
void RecordDeed(Game& game, const Side* side, DeedType type, int subject, int object,
                const std::string& name);

/**
 * @brief What History > Triumphs counts, by kind.
 *
 * A side's own row counts what it lost; its row for another side, what it
 * killed of them.
 */
enum class Loss : int {
    kArmies = 0,     ///< Ordinary armies.
    kCreatures = 1,  ///< Armies of a creature type.
    kHeroes = 2,     ///< Heroes.
    kNavies = 3,     ///< Armies that fought as boats.
    kStandards = 4,  ///< Standards carried by the armies lost.
};
constexpr int kLossKinds = 5;  ///< How many kinds of Loss there are.

/**
 * @brief Counts an army of its owner's killed by `killer`'s.
 * @param boat True when it fought as a boat, at sea on water or shore; those
 *        count as navies too.
 */
void Tally(Game& game, const Army* army, int killer, bool boat = false);
/** @brief The count for `side` against `opponent`, of one kind. */
int Triumph(const Game& game, int side, int opponent, Loss kind);
/** @brief Takes the round's record, as the turn counter goes up. */
void Record(Game& game);

}  // namespace w2::history
