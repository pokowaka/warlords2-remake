/**
 * @file
 * @brief The computer players' assault groups.
 *
 * A group has a rally city where a strike force gathers, up to four member
 * cities building for it, and six enemy cities to take. Picking whom to
 * attack is here too. See docs/re/ai.md > Assault groups.
 */
#pragma once

#include <array>
#include <vector>

#include "warlords/ai/core.hpp"
#include "warlords/types.hpp"

namespace w2::ai::groups {

/**
 * @brief Is a city free to serve a new group?
 *
 * Not while it is an active group's rally city; yes when it is stopped or
 * building, and, with `members`, when it is already a member.
 */
bool Free(Game& game, Side& side, const City& city, bool members);
/** @brief Is a city an active group's rally city? */
bool IsRally(Game& game, Side& side, const City& city);
/** @brief Cancels a group: its rally and member cities stop, its slot is emptied. */
void Cancel(Game& game, Side& side, int group_index);
/** @brief The gold sacking a city would bring: half the price of each production slot but the
 * first. */
int SackValue(const Game& game, const City& city);
/** @brief Pillages a city the side has taken. */
void Pillage(Game& game, Side& side, City& city);
/** @brief Sacks a city the side has taken; pillages it when there is nothing to sack. */
void Sack(Game& game, Side& side, City& city);
/**
 * @brief Razes a city the side has taken, or sacks it when it is worth 400.
 *
 * Without `force`, a city with more than two of the side's cities near it
 * is spared.
 *
 * @return The stack's next city to go for, or null.
 */
City* Raze(Game& game, Side& side, City& city, bool force, core::Sel* sel = nullptr);
/**
 * @brief Early vengeance: before turn 10, a side with the trait sacks (when
 *        worth 200) or razes a city it has just taken from a human.
 * @param previous_owner Who held the city before.
 * @return Whether it took its vengeance.
 */
bool EarlyVengeance(Game& game, int side_index, City& city, int previous_owner);
/**
 * @brief Looks over what the side's cities can build for the groups.
 *
 * Marks the cities that build fliers, strong fast armies and fast armies,
 * and sets each active group's wanted size and speed from that.
 */
void Prepare(Game& game, Side& side);
/**
 * @brief Sends the group's stack at a city and deals with what it takes.
 * @return 1 the city was taken and dealt with, 3 the march ended otherwise.
 */
int March(Game& game, Side& side, int group_index, std::vector<Army*> list, City* city);
/** @brief Follows up from the cities the group took: quiet ones send their armies to the rally. */
void FollowUp(Game& game, Side& side, int group_index);
/** @brief The assault phase: every active group takes its turn. */
void Assault(Game& game, Side& side);
/** @brief Which side to attack, or kNone. */
int PickEnemy(Game& game, Side& side);
/** @brief For I am the Greatest: which computer sides are at war with a human. */
std::array<bool, 8> FightingHumans(const Game& game);
/** @brief The "assault XX" phase: start a new group when there is room for one. */
void AssaultXx(Game& game, Side& side);

}  // namespace w2::ai::groups
