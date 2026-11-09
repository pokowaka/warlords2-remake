/**
 * @file
 * @brief Clean-slate learnable fair-information AI turn engine.
 *
 * Executes a computer player's turn using human-equivalent fair observations
 * (SideBelief) and the differentiable PolicyNetwork (ml_policy). Rules code
 * computes only normalized physical observables and executes legal actions
 * without strategic heuristics, fake feature injections, or actuator vetoes.
 * Supports stochastic rollout recording via RolloutSampler for Proximal Policy
 * Optimization (PPO) self-play training.
 */
#pragma once

#include <functional>
#include <vector>

#include "warlords/ai/ml_policy.hpp"
#include "warlords/model/computer_player.hpp"
#include "warlords/types.hpp"

namespace w2::ai::modern {

/**
 * @brief Diagnostic record of what MissionNet believed when it sent a stack on the
 * mission that just triggered a battle.
 */
struct MissionFight {
    int side_idx = kNone;        ///< Side index of the attacking stack.
    int target_kind = 0;         ///< TargetKind index of the chosen mission.
    bool at_target = false;      ///< True when the battle is the chosen mission target.
    float decided_win = 0.0F;    ///< target[10] (`win_now`) at mission-decision time.
    float decided_turns = 0.0F;  ///< `turns_to_reach` at mission-decision time.
    int decided_size = 0;        ///< Coalition size at mission-decision time.
    int fight_size = 0;          ///< Attackers that actually entered the battle.
};

/** @brief Callback invoked after each battle initiated by `ExecuteStackMission`. */
using MissionFightCallback = std::function<void(Game&, const MissionFight&, const Battle&)>;

/** @brief Registers or clears the diagnostic callback invoked after mission-initiated battles. */
void SetMissionFightCallback(MissionFightCallback callback);

/** @brief Invokes the registered `MissionFightCallback` if one is active. */
void NotifyMissionFight(Game& game, const MissionFight& fight, const Battle& battle);

/**
 * @brief Plays a full turn for `side` using `custom_policy` (or ActivePolicy() when null)
 * under strict human-equivalent fog-of-war and stack visibility rules.
 *
 * @param game Active game state.
 * @param side Computer side whose turn is executing.
 * @param events Optional UI pacing and turn-event observer.
 * @param custom_policy Optional policy network override (used during arena evaluation/training).
 * @param sampler Optional stochastic rollout sampler and trajectory recorder for PPO training.
 */
void PlayTurn(Game& game, Side& side, IAiTurnEvents* events = nullptr,
              const ml::PolicyNetwork* custom_policy = nullptr,
              ml::RolloutSampler* sampler = nullptr);

/**
 * @brief The mission options the Modern AI would weigh for one army group: one entry
 * per (target, coalition size) option, as MissionNet sees them.
 *
 * Used to label another player's moves for imitation learning.
 */
struct MissionSnapshot {
    std::vector<ml::MissionInVec> candidates;  ///< MissionNet input per option.
    std::vector<float> log_mult;               ///< Log multiplicity prior per option.
    std::vector<int> kind;              ///< TargetKind per option (0 hold .. 7 reinforce own city).
    std::vector<int> city_index;        ///< Target city index per option, or kNone.
    std::vector<int> tx, ty;            ///< Target tile coordinates per option.
    std::vector<int> stack_size;        ///< Coalition size per option.
    std::uintptr_t seed = 0;            ///< The group's first army (Game::pool address).
    std::vector<std::uintptr_t> group;  ///< Every army in the group.
    int start_x = 0, start_y = 0;       ///< Group starting tile coordinates.
    int start_city = kNone;             ///< Owned city index at the start tile, or kNone.
};

/**
 * @brief Builds, without moving anything, the first mission decision the Modern AI
 * would face for every army group of `side` this turn (fair information).
 */
std::vector<MissionSnapshot> SnapshotMissions(Game& game, Side& side,
                                              const ml::PolicyNetwork& policy);

/**
 * @brief Selects which owned city a newly hired hero should appear in.
 */
City* SelectHeroCity(Game& game, Side& side, City* default_city,
                     const ml::PolicyNetwork* custom_policy = nullptr);

}  // namespace w2::ai::modern
