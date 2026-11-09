/**
 * @file
 * @brief Computer player facade, frontend turn hooks, and default strategy registration.
 *
 * Bridges the nineteen-phase 1993 Classic AI (`ClassicAiPlayer`) and the fair-information
 * neural-policy AI (`ModernMlPlayer`) through `Side::computer_player`.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "warlords/move.hpp"
#include "warlords/types.hpp"

namespace w2 {
struct AIData;
struct Battle;
}  // namespace w2

namespace w2::ai {

/** @brief Callbacks the front end registers to animate computer turns. */
struct Hooks {
    /// Invoked after a computer stack walks one or more steps.
    std::function<void(Game&, const std::vector<Army*>&, const move::WalkResult&)> on_walk;
    /// Invoked when a computer stack initiates combat at a tile.
    std::function<void(Game&, const std::vector<Army*>&, int, int, Battle&)> on_fight;
    /// Invoked when a computer side razes, sacks, or pillages a city.
    std::function<void(Game&, Side&, City&, const std::string&)> on_spoils;
};
extern Hooks hooks;  ///< Active front-end hooks; empty callbacks are skipped.

/**
 * @brief Returns whether newly initialized sides default to `ClassicAiPlayer` (`true`, for 1993
 * parity tests) or `ModernMlPlayer` (`false`, in the application).
 */
bool DefaultClassicAi();

/** @brief Sets the process-wide default returned by `DefaultClassicAi()`. */
void SetDefaultClassicAi(bool classic);

/**
 * @brief Instantiates a `ClassicAiPlayer` (`classic == true`) or `ModernMlPlayer` (`false`) for
 * `side` on `game` without advancing `game.rng`.
 */
std::shared_ptr<IComputerPlayer> MakeComputerPlayer(bool classic, Game& game, Side& side);

/**
 * @brief Ensures `side` is backed by a `ClassicAiPlayer` (`classic == true`) or `ModernMlPlayer`
 * (`classic == false`), preserving existing strategy instances when the mode already matches.
 */
void SetSideClassicAi(Game& game, Side& side, bool classic);

/** @brief Initializes `side` with a `ClassicAiPlayer` populated from level defaults and its card.
 */
AIData& InitSide(Game& game, Side& side);

/** @brief Initializes computer strategies, city claims, and diplomacy scores for a new game. */
void StartGame(Game& game);

/**
 * @brief Executes one computer turn on `side` via `side.computer_player`.
 * @param pause Optional callback invoked between phases with the phase's name.
 */
void PlayTurn(Game& game, Side& side, const std::function<void(const char*)>& pause = nullptr);

/** @brief Reports a completed computer stack walk to `hooks.on_walk`. */
void Walked(Game& game, const std::vector<Army*>& stack, const move::WalkResult& walk);

/** @brief Selects the owned city where a newly hired computer hero appears. */
City* HeroCity(Game& game, Side& side, City* fallback);

/**
 * @brief Applies computer quest-capture spoils when `stack` conquers `city`.
 * @return True if `city` was razed or sacked.
 */
bool QuestCapture(Game& game, Side& side, City& city, const std::vector<Army*>& stack);

/** @brief Records combat casualties and fair-information observations for battle participants. */
void RecordBattle(Game& game, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile);

}  // namespace w2::ai
