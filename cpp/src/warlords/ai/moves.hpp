/**
 * @file
 * @brief The computer players' movement phases.
 *
 * Move (standing orders), rescue, last rescue, move search, move explore and
 * specials, plus the explorer walk and the hero parties they use. See
 * docs/re/ai.md, "Movement phases".
 */
#pragma once

#include <vector>

#include "warlords/ai/core.hpp"
#include "warlords/types.hpp"

namespace w2::ai::moves {

/**
 * @brief A hero searches the site it has reached.
 *
 * At a temple the side may take a quest; at an allies site the new magical
 * allies are sent exploring when the map is hidden.
 * @param sel The hero's selected stack, or nullptr to search with the hero alone.
 * @return 0 when nothing was searched or the searcher is no hero, 2 when the
 *         hero searched (or the site was already empty).
 */
int SearchSite(Game& game, core::Sel* sel, Army* hero, Site& place);

/**
 * @brief Sends a hero and one flying companion off to a site or a rally city.
 *
 * The pair aims for the best unsearched, explored site (or, failing that, one
 * of the side's own cities, rally cities preferred) and walks there.
 * @param list The armies on the hero's tile to pick the pair from.
 * @return 0 when there is no hero (or nothing to search), 1 when there is a
 *         hero but no flier, 2 when the hero searched a site.
 */
int Party(Game& game, Side& side, std::vector<Army*> list);

/**
 * @brief Sends an army exploring: it takes steps while it has moves left.
 * @param home The city whose open neutral neighbour is the fallback target, or
 *             nullptr for none.
 */
void Explore(Game& game, Side& side, Army* army, City* home);

/** @brief The "move search" phase: walks the side's explorers, heroes last. */
void Search(Game& game, Side& side);

/** @brief The "move explore" phase: sends the side's marked heroes off in parties. */
void HeroParties(Game& game, Side& side);

/**
 * @brief The "move" phases (#1 and #2): walks every stack along its standing order.
 *
 * Stacks go nearest-first from a cursor that follows the last stack moved.
 */
void MoveAll(Game& game, Side& side);

/**
 * @brief Finds work for an idle army out in the field, or disbands it.
 *
 * It gathers the stack on its tile and sends it to the best city within reach
 * by the flood; weak or aimless stacks are disbanded.
 */
void SendIdle(Game& game, Side& side, Army* army);

/** @brief The "rescue" phase: idle armies out in the field get a new target. */
void Rescue(Game& game, Side& side);

/**
 * @brief The "last rescue" phase: armies out in the field that still have
 *        their full moves drop their orders and are sent anew.
 */
void LastRescue(Game& game, Side& side);

/** @brief The "specials" phase: heroes in the side's cities set off in parties. */
void Specials(Game& game, Side& side);

}  // namespace w2::ai::moves
