#include "warlords/report.hpp"

#include <algorithm>

#include "warlords/game.hpp"

namespace w2::report {

int Score(const Game& game, const Side& side) {
    int cities = 0;
    for (City* city : game::SideCities(game, side)) cities += city->defence * city->income;
    int score = (side.gold + game::Income(game, side) * 5 + game::Upkeep(game, side) + cities) / 30;
    return std::max(1, std::min(500, score));
}

Figures ComputeFigures(const Game& game, const Side& side, int report_index) {
    Figures figures;
    for (int i = 0; i < 8; i++) {
        const Side& other = game.map->sides[i];
        figures.out[i] = !other.alive;
        figures.value[i] = 0;
    }
    if (report_index == kProduction) {
        figures.result = static_cast<int>(side.produced.size());
        return figures;
    }
    if (report_index == kArmy) {
        for (Army* army : game.armies) {
            if (army->owner >= 0 && army->owner < 8) figures.value[army->owner]++;
        }
    } else if (report_index == kCity) {
        for (int i = 0; i < 8; i++)
            figures.value[i] = static_cast<int>(game::SideCities(game, game.map->sides[i]).size());
    } else if (report_index == kGold) {
        for (int i = 0; i < 8; i++) figures.value[i] = game.map->sides[i].gold;
    } else if (report_index == kWinning) {
        int scores[8];
        for (int i = 0; i < 8; i++) {
            const Side& other = game.map->sides[i];
            scores[i] = other.in_use ? Score(game, other) : 1;
        }
        int rank = 0;
        for (int i = 0; i < 8; i++)
            if (scores[side.index] < scores[i]) rank++;
        for (int i = 0; i < 8; i++) figures.value[i] = scores[i] / 5;
        figures.max = 100;
        figures.result = rank;
        return figures;
    }
    for (int i = 0; i < 8; i++) figures.max = std::max(figures.max, figures.value[i]);
    // the top of the scale is the largest figure made even
    if (figures.max > 0 && figures.max / 2 == (figures.max - 1) / 2) figures.max++;
    figures.result = figures.value[side.index];
    return figures;
}

}  // namespace w2::report
