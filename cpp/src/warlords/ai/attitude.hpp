/**
 * @file
 * @brief Diplomatic attitude and deal-threshold evaluation for computer-player strategies.
 */
#pragma once

#include <array>

#include "warlords/types.hpp"

namespace w2::ai {

/** @brief Per-opponent battle and treaty counters contributing to relational grudges. */
struct BattleGrudge {
    std::array<int, 8> cities_lost{};       ///< Garrisoned cities lost in battle to each side.
    std::array<int, 8> cities_stolen{};     ///< Ungarrisoned cities seized by each side.
    std::array<int, 8> heroes_killed{};     ///< Defending heroes slain by each side.
    std::array<int, 8> lost{};              ///< Defending stacks wiped out by each side.
    std::array<int, 8> cities_razed_by{};   ///< Claimed/owned cities razed to ruins by each side.
    std::array<int, 8> truces_broken_by{};  ///< Active truces/alliances broken by each side.
    std::array<int, 8> tribute_received{};  ///< Cumulative gold tribute paid to us by each side.
    std::array<int, 8> last_grudge_turn{};  ///< Most recent turn each side wronged us.
};

/**
 * @brief Computes a smooth `[0, 1]` blood-feud intensity toward `rival_idx` from `grudge` and
 * recency relative to `current_turn`.
 */
float ComputeVendettaScore(const BattleGrudge& grudge, int rival_idx, int current_turn);

/**
 * @brief Computes `ai_side`'s transparent diplomatic attitude and deal thresholds toward
 * `target_side` using fair-information visibility, personality traits, border pressure, and
 * `grudge` history.
 */
AttitudeBreakdown EvaluateSideProposal(const Game& game, SideId ai_side, SideId target_side,
                                       const SideBelief* belief, const BattleGrudge* grudge);

}  // namespace w2::ai
