/**
 * @file
 * @brief The computer players' diplomacy: each turn the side sets every
 *        proposal afresh. See docs/re/ai.md > Diplomacy.
 */
#pragma once

#include "warlords/types.hpp"

namespace w2::ai::diplo {

/**
 * @brief May the side go for this city?
 *
 * A neutral city or one of a side at war, yes. One of a side it is uneasy
 * with, or at peace with while it is at war with nobody, only when the side
 * is bold; otherwise no.
 */
bool CanAttack(Game& game, Side& side, const City& city);
/**
 * @brief Computer solidarity: does the side spare `other` because `other` is
 *        at war with a human who is in solidarity too?
 */
bool Spared(Game& game, Side& side, int other);
/**
 * @brief The diplomacy phase: claim a city, then set every proposal afresh
 *        from grudges, threats, the enemy picked, the assault groups, who
 *        holds too much of the world, solidarity and I am the Greatest.
 */
void Phase(Game& game, Side& side);

}  // namespace w2::ai::diplo
