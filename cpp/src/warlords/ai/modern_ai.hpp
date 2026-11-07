// Clean-slate 100% learnable Fair-Information AI turn engine.
//
// Architectural Role: Executes a computer player's turn using human-equivalent
// fair observations (SideBelief) and the 4-head differentiable PolicyNetwork
// (ml_policy). Rules code computes only normalized physical observables and
// executes legal actions without strategic heuristics, fake feature injections,
// or actuator vetoes. Supports stochastic rollout recording via RolloutSampler
// for Proximal Policy Optimization (PPO) self-play training.
#pragma once

#include <functional>
#include <vector>

#include "warlords/ai/ml_policy.hpp"
#include "warlords/types.hpp"

namespace w2::ai::modern {

using PauseCallback = std::function<void(const char*)>;

/**
 * Plays a full turn for `side` using `customPolicy` (or activePolicy() when null)
 * under strict human-equivalent fog-of-war and stack visibility rules.
 *
 * @param g Active game state.
 * @param side Computer side whose turn is executing.
 * @param pause Optional UI pacing hook invoked between turn phases.
 * @param customPolicy Optional policy network override (used during arena evaluation/training).
 * @param sampler Optional stochastic rollout sampler & trajectory recorder for PPO training.
 */
void playTurn(Game& g, Side& side, const PauseCallback& pause = nullptr,
              const ml::PolicyNetwork* customPolicy = nullptr,
              ml::RolloutSampler* sampler = nullptr);

/**
 * The mission options the Modern AI would weigh for one army group: one entry
 * per (target, coalition size) option, as MissionNet sees them. Used to label
 * another player's moves for imitation learning.
 */
struct MissionSnapshot {
  std::vector<ml::MissionInVec> candidates;
  std::vector<float> logMult;
  std::vector<int> kind;        // TargetKind per option (0 hold, 1 neutral city, 2 enemy city, 3 enemy stack,
                                // 4 ruin/sage, 5 temple/item, 6 fog frontier, 7 reinforce own city)
  std::vector<int> cityIndex;   // the target city per option, or NONE
  std::vector<int> tx, ty;      // the target tile per option
  std::vector<int> stackSize;   // coalition size per option
  std::uintptr_t seed = 0;      // the group's first army (Game::pool address)
  std::vector<std::uintptr_t> group;   // every army of the group
  int startX = 0, startY = 0, startCity = NONE;
};

/**
 * Builds, without moving anything, the first mission decision the Modern AI
 * would face for every army group of `side` this turn (fair information).
 */
std::vector<MissionSnapshot> snapshotMissions(Game& g, Side& side, const ml::PolicyNetwork& policy);

/**
 * Selects which owned city a newly hired hero should appear in, scored by the
 * neural policy's economy head.
 */
City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* customPolicy = nullptr);

}  // namespace w2::ai::modern
