/**
 * @file
 * @brief The computer players' hero phase.
 *
 * Each hero weighs a quest temple, a site, an item lying about and an enemy
 * city -- 100 - distance + 1d20 each -- and goes for the best, twice at
 * most.
 */
#pragma once

#include "warlords/types.hpp"

namespace w2::ai::heroes {

/** @brief The hero phase: every hero of the side picks a goal and goes for it. */
void Phase(Game& game, Side& side);

}  // namespace w2::ai::heroes
