// Training opponents for the Modern AI (`w2arena`, `w2aidiag`).
//
// These are *opponents*, never part of the Modern AI: rule-based personalities
// that probe one weakness each, so the learner meets the punishment often and
// discovers its own answer. They run on top of the 1993 Classic AI.
//
//   Raider: before the Classic AI plays its turn, it sends its weakest armies
//   to walk into every enemy city left without a single defender that they can
//   reach this turn (omniscient, like the Classic AI itself), the learner's
//   cities first.
#pragma once

#include "warlords/game.hpp"

namespace w2::opponents {

enum class Kind { Classic, Raider };

/** Plays one turn for a training opponent of the given kind. */
void playTurn(Game& g, Side& side, Kind kind);

/** The Raider's pre-phase alone: capture reachable undefended enemy cities.
 *  Returns how many cities it took. */
int raidEmptyCities(Game& g, Side& side);

}  // namespace w2::opponents
