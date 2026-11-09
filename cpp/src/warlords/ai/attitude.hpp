/**
 * @file
 * @brief Diplomatic attitude and deal-threshold evaluation for computer-player strategies.
 */
#pragma once

#include <array>

#include "warlords/types.hpp"

namespace w2::ai {

/** @brief Per-opponent battle casualty counters contributing to diplomatic blood-feud grudges. */
struct BattleGrudge {
    std::array<int, 8> cities_lost{};    ///< Standing cities lost to each side.
    std::array<int, 8> heroes_killed{};  ///< Defending heroes slain by each side.
    std::array<int, 8> lost{};           ///< Defending stacks wiped out by each side.
};

/**
 * @brief Computes `ai_side`'s transparent diplomatic attitude and deal thresholds toward
 * `target_side` using fair-information visibility, personality traits, border pressure, and
 * `grudge` history.
 */
AttitudeBreakdown EvaluateSideProposal(const Game& game, SideId ai_side, SideId target_side,
                                       const SideBelief* belief, const BattleGrudge* grudge);

}  // namespace w2::ai
