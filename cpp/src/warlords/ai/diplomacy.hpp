// The computer players' diplomacy (ai_phase_diplomacy, 558d:0000): each turn
// the side sets every proposal afresh. docs/re/ai.md > Diplomacy.
#pragma once

#include "warlords/types.hpp"

namespace w2::ai::diplo {

/** May the side go for this city (558d:0851)? */
bool CanAttack(Game& g, Side& side, const City& c);
/** Solidarity with a side (558d:0a6e). */
bool Spared(Game& g, Side& side, int other);
/** diplomacy (ai_phase_diplomacy, 558d:0000). */
void Phase(Game& g, Side& side);

}  // namespace w2::ai::diplo
