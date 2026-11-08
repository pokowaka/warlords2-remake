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
//
//   Frozen: a fixed Modern AI checkpoint (e.g. the installed weights) playing
//   near-greedily. It does not learn, so the learner cannot co-adapt with it;
//   it exploits empty cities the way the real Modern AI does. Its side must
//   have classic_ai == false.
#pragma once

#include "warlords/ai/ml_policy.hpp"
#include "warlords/game.hpp"

namespace w2::opponents {

/** @brief The personality a training opponent plays with. */
enum class Kind {
    kClassic,  ///< The 1993 Classic AI, unchanged.
    kRaider,   ///< Takes reachable undefended enemy cities, then plays Classic.
    kFrozen,   ///< A fixed Modern AI checkpoint that does not learn.
};

/**
 * @brief Plays one turn for a training opponent of the given kind.
 * @param game The game in progress.
 * @param side The side whose turn it is.
 * @param kind The opponent's personality.
 * @param frozen The network a Frozen opponent plays; required for kFrozen.
 */
void PlayTurn(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen = nullptr);

/**
 * @brief The Raider's pre-phase alone: captures reachable undefended enemy
 *        cities.
 * @return How many cities it took.
 */
int RaidEmptyCities(Game& game, Side& side);

}  // namespace w2::opponents
