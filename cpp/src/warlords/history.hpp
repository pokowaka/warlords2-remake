// The game's history: what the History menu plays back.
//
// docs/formats/history.md. Once a round, when the turn counter goes up
// (8065:17f6 -> 6d51:0d60), a record is taken of every side's gold, score
// and city count, every city's owner, and the round's deeds -- kept as they
// happen, two a side, a lower type counting for more (6d51:1244).
#pragma once

#include <string>

#include "warlords/types.hpp"

namespace w2::history {

using w2::DeedType;   // the kinds of deed, in types.hpp beside Deed

// A deed's first value where it is not a city, a side or an item:
constexpr int KILLED_IN_BATTLE = -1;   // HERO_KILLED: in the field, not in a city
constexpr int KILLED_SEARCHING = -2;   // HERO_KILLED: by a ruin's guardian
constexpr int WON_BY_NAME = -1;        // CITY_WON: the hero named in the deed took it
constexpr int FOUND_ALLIES = 100, FOUND_SAGE = 101, FOUND_GOLD = 102;   // FOUND: not an item

/** The history is recorded for the first 201 rounds only (6d51:0d60). */
constexpr int LAST_RECORDED_TURN = 201;
/** A round's record keeps at most ten deeds: every side's first, then
 *  second ones while there is room (6d51:132f). */
constexpr int MAX_DEEDS_PER_ROUND = 10;

/** Note a deed of a side's (6d51:1244). `name` is the hero's or the side's. */
void deed(Game& g, const Side* side, DeedType type, int v1, int v2, const std::string& name);

/** What History > Triumphs counts, by kind: a side's own row counts what it
 *  lost; its row for another side, what it killed of them (67cc:1b43-1e92). */
enum class Loss : int { ARMIES = 0, CREATURES = 1, HEROES = 2, NAVIES = 3, STANDARDS = 4 };
constexpr int LOSS_KINDS = 5;

/** An army of its owner's killed by `killer`'s; `boat` when it fought as a
 *  boat, at sea on water or shore (67cc:1aff counts those navies). */
void tally(Game& g, const Army* army, int killer, bool boat = false);
/** The count for side `me` against `opp`, of one kind. */
int triumph(const Game& g, int me, int opp, Loss kind);
/** The round's record (6d51:0d60), taken as the turn counter goes up. */
void record(Game& g);

}  // namespace w2::history
