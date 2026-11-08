/**
 * @file
 * @brief The figures behind the Report menu's five reports.
 *
 * Army, City, Gold, Production and Winning: this computes the numbers, the
 * drawing is in ui/reports.
 */
#pragma once

#include <array>

#include "warlords/types.hpp"

namespace w2::report {

/// The five reports, in the Report menu's order.
constexpr int kArmy = 0, kCity = 1, kGold = 2, kProduction = 3, kWinning = 4;

/**
 * @brief The Winning report's score for a side.
 *
 * (gold + 5 * income + upkeep + each city's income times defence) / 30,
 * kept within 1..500.
 */
int Score(const Game& game, const Side& side);

/** @brief One report's figures: a bar per side and the viewer's own result. */
struct Figures {
    std::array<int, 8> value{};  ///< The bar for each side.
    std::array<bool, 8> out{};   ///< Sides no longer in the game.
    int max = 0;                 ///< The top of the scale.
    int result = 0;              ///< The viewing side's figure (its rank, for Winning).
};

/**
 * @brief Computes one report's figures for the viewing side.
 * @param report_index One of kArmy, kCity, kGold, kProduction, kWinning.
 */
Figures ComputeFigures(const Game& game, const Side& side, int report_index);

}  // namespace w2::report
