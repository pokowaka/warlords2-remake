// Clean-slate 100% learnable AI turn engine.
//
// Architectural Role: Executes a computer player's turn using human-equivalent
// fair observations (SideBelief) and the 4-head neural PolicyNetwork (ml_policy).
// All strategic, operational, tactical, and economic choices—garrison retention,
// marginal stack assembly, AttackNow vs StageNear siege synchronization, upkeep
// disbanding, production buying, vector routing, capture spoils, and diplomacy—are
// determined by neural utility evaluation without hardcoded strategic thresholds.
#pragma once

#include <functional>
#include <vector>

#include "warlords/ai/ml_policy.hpp"
#include "warlords/types.hpp"

namespace w2::ai::modern {

/**
 * Plays a full turn for `side` using the given `policy` network (or activePolicy()
 * by default) under strict human-equivalent fog-of-war and stack visibility rules.
 */
void playTurn(Game& g, Side& side, const std::function<void(const char*)>& pause = nullptr,
              const ml::PolicyNetwork* customPolicy = nullptr);

/**
 * Selects which owned city a newly hired hero should appear in, scored by the
 * neural policy's mission/economy evaluation.
 */
City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* customPolicy = nullptr);

}  // namespace w2::ai::modern
