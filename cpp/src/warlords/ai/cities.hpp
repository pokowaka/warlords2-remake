// The computer players' city phases: evaluate, clean city, neutral, quick
// attack, update hide, rebuilding, production and vectoring.
// See docs/re/ai.md for the turn pipeline and the city roles.
#pragma once

#include <map>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2::ai::cities {

/**
 * @brief The side's own cities, last first.
 *
 * Walked last first because the original walks the city table that way, and
 * the order decides which city acts first.
 */
std::vector<City*> Own(const Game& game, const Side& side);

/** @brief A neutral city among another city's six neighbours. */
struct NeutralNeighbour {
    City* city;  ///< The neutral city.
    int dist;    ///< Its land-path distance from the city it neighbours.
    int slot;    ///< Its place in the neighbour list, from 1.
};

/**
 * @brief The neutral cities among a city's neighbours worth going for.
 *
 * Skips the side's quest city and razed cities. Once one is found, those
 * beyond the first two neighbours count only when nearer than 20.
 */
std::vector<NeutralNeighbour> NeutralNeighbours(Game& game, Side& side, const City& city);

/**
 * @brief The distance from (x, y) to the nearest army the side can see.
 *
 * Counts armies of any other side, or only those of side `of_side` when it is
 * given. Armies in transit and ownerless ones don't count.
 * @return The distance, or 1000 when there is none.
 */
int NearestArmy(const Game& game, const Side& side, int x, int y, int of_side = kNone);

/**
 * @brief Can this city build something worth having?
 *
 * The bigger the side's empire, the stronger the type must be: any type up to
 * 7 cities, strength 2 or more from 8, 3 or more from 12.
 * @return {good, the best slot's index}; the slot is kNone when it has none.
 */
std::pair<bool, int> BuildsWell(Game& game, Side& side, const City& city);

/** @brief Is a city part way through building something (not just started)? */
bool Building(const City& city);

/**
 * @brief Vectors a city's production to `dest`, or clears it.
 *
 * Only a city that is building keeps a vector; any other has it cleared.
 */
void Vector(Game& game, City& city, const City* dest);

/**
 * @brief Clears "not seen yet" for cities the side has seen round.
 *
 * Only with Hidden Map on: a city counts as seen once any tile of the 4x4
 * block round it has been seen.
 */
void ClearUnseen(Game& game, Side& side);

/**
 * @brief The evaluate phase: recounts the cities and settles new cities' roles.
 *
 * Housekeeping, not scoring: marks which cities can build fliers, recounts
 * own, enemy, neutral and unseen cities, bumps how long the side has held each
 * city and clears the role of cities it no longer holds. A just-taken city
 * becomes kNearNeutral when it has neutral neighbours, else kBuilding.
 * @param side_index Whose cities count as own.
 */
void Evaluate(Game& game, Side& side, int side_index);

/**
 * @brief The garrison a city wants, by who holds its neighbours.
 * @return 8 at war with a neighbour's owner, else 4 for two or more foreign
 *     neighbours, 3 for one and 2 for none.
 */
int Wanted(Game& game, const Side& side, const City& city);

/**
 * @brief Looks over a city's armies and stands them where they belong.
 *
 * Disbands foreign armies found there, hands items round between heroes,
 * settles the city's role by its garrison, may disband the surplus of a broke
 * side, sends fliers exploring, and stands the keepers on the city's first
 * tile and the rest eight to a tile on the others. Skipped when three or
 * more of its armies have orders (unless it is a rally city), or, without
 * `force`, when nothing has changed and a one-in-six roll fails.
 * @return Always true.
 */
bool Garrison(Game& game, Side& side, City& city, bool force);

/** @brief The clean city phase: checks the garrison of every city not yet cleaned. */
void Clean(Game& game, Side& side);

/**
 * @brief The first neutral neighbour fewer than three stacks are heading for.
 * @param heading Stacks on their way, by city index.
 * @return The city, or null.
 */
City* OpenNeutral(Game& game, Side& side, const City& city, std::map<int, int>& heading);

/**
 * @brief The neutral phase: sends idle armies at neutral neighbours.
 *
 * Up to ten passes; the first covers every city, later ones only cities just
 * taken in the pass before. Stops when a pass does nothing.
 */
void Neutral(Game& game, Side& side);

/**
 * @brief The quick attack phase: strong garrisons strike at weak neighbours.
 *
 * Not for a cautious side.
 */
void QuickAttack(Game& game, Side& side);

/**
 * @brief The update hide phase: refreshes what the side has seen.
 *
 * A city sending explorers goes back to kNearNeutral once one of its neutral
 * neighbours has been seen.
 */
void UpdateHide(Game& game, Side& side);

/**
 * @brief The rebuilding phase: buys a new production type into a city.
 *
 * Only with at least 500 gold, and when the side has more than four heroes
 * or few cities for its heroes; the type bought depends on whether the side
 * has over 2000 gold.
 */
void Rebuild(Game& game, Side& side);

/**
 * @brief The production phase: picks what each city builds, by its role.
 *
 * Also picks the assault groups' member cities. Stops altogether when the
 * side is short of gold and its income is below its upkeep.
 */
void Production(Game& game, Side& side);

/**
 * @brief The vectoring phase: member cities vector production to their rally city.
 *
 * For every active group whose rally city the side still holds.
 */
void Vectoring(Game& game, Side& side);

}  // namespace w2::ai::cities
