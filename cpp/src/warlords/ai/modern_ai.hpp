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
 * Selects which owned city a newly hired hero should appear in, scored by the
 * neural policy's economy head.
 */
City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* customPolicy = nullptr);

}  // namespace w2::ai::modern
