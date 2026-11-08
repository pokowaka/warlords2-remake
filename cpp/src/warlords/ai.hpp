/**
 * @file
 * @brief The computer players.
 *
 * A port of WARLORD2.EXE's AI (docs/re/ai.md). Every side has a block of AI
 * data (ai/core) filled at game start from its level and its character card
 * (docs/formats/crd.md); each computer turn runs the original's nineteen
 * phases in the original's order.
 *
 *   ai/core       data, neighbours, the selection, flood, battles, walk
 *   ai/cities     evaluate, garrisons, neutrals, production, vectoring
 *   ai/groups     the assault groups and picking an enemy
 *   ai/moves      standing orders, rescue, explorers, hero parties
 *   ai/heroes     the hero phase
 *   ai/diplomacy  proposals
 *
 * The Lua runs a computer turn as a coroutine, so the front end can show each
 * walk and battle as it happens. Here the turn is plain code, and the hooks
 * are where the front end shows things: it runs the turn on a thread of its
 * own and has each hook wait for the showing to finish (front/aithread).
 * With no hooks set the turn simply plays through.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "warlords/move.hpp"
#include "warlords/types.hpp"

namespace w2 {
struct Battle;
}

namespace w2::ai {

/** @brief The callbacks the front end listens to a computer turn with. */
struct Hooks {
    /// A computer stack has walked.
    std::function<void(Game&, const std::vector<Army*>&, const move::WalkResult&)> on_walk;
    /// A computer stack fights at the given tile.
    std::function<void(Game&, const std::vector<Army*>&, int, int, Battle&)> on_fight;
    /// A computer side has razed, sacked or pillaged a city; the string says which.
    std::function<void(Game&, Side&, City&, const std::string&)> on_spoils;
};
extern Hooks hooks;  ///< The front end's hooks; empty ones are skipped.

/**
 * @brief Whether newly initialized computer sides default to the 1993
 * Classic AI (true, for the parity tests) or the learnable ML AI (false, in
 * the app).
 */
bool DefaultClassicAi();
/** @brief Sets what DefaultClassicAi() returns. */
void SetDefaultClassicAi(bool classic);

/** @brief Gives a side its AI data at game start, from its level and card. */
AIData& InitSide(Game& game, Side& side);
/** @brief Sets the computer players up for a new game. */
void StartGame(Game& game);
/**
 * @brief Plays one computer turn, phase by phase.
 * @param pause If given, called between phases with the phase's name.
 */
void PlayTurn(Game& game, Side& side, const std::function<void(const char*)>& pause = nullptr);
/** @brief Every walk a computer stack makes is reported here once it is made. */
void Walked(Game& game, const std::vector<Army*>& stack, const move::WalkResult& walk);
/** @brief Where a hired hero appears for a computer; `fallback` when nowhere is better. */
City* HeroCity(Game& game, Side& side, City* fallback);
/**
 * @brief A computer's quest hero has taken a city.
 * @return True when it razed or sacked, so the capture is not also taken for
 *         an occupation.
 */
bool QuestCapture(Game& game, Side& side, City& city, const std::vector<Army*>& stack);
/** @brief Records a battle fought on a side's tile in the defender's AI data. */
void RecordBattle(Game& game, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile);

}  // namespace w2::ai
