// The figures behind the Report menu's five reports: Army, City, Gold,
// Production and Winning (6ef3:02fb). The drawing is ui/reports.
#pragma once

#include <array>

#include "warlords/types.hpp"

namespace w2::report {

constexpr int kArmy = 0, kCity = 1, kGold = 2, kProduction = 3, kWinning = 4;

/** The Winning report's score for a side: (gold + 5 income + upkeep + each
 *  city's income times defence) / 30, within 1..500. */
int Score(const Game& g, const Side& side);

struct Figures {
    std::array<int, 8> value{};
    std::array<bool, 8> out{};
    int max = 0;
    int result = 0;
};
/** One report's figures. */
Figures ComputeFigures(const Game& g, const Side& side, int n);

}  // namespace w2::report
