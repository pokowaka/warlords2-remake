#include "warlords/report.hpp"

#include <algorithm>

#include "warlords/game.hpp"

namespace w2::report {

int Score(const Game& game, const Side& side) {
    int cities = 0;
    for (City* city : game::SideCities(game, side)) cities += city->Defence() * city->Income();
    int score =
            (side.Gold() + game::Income(game, side) * 5 + game::Upkeep(game, side) + cities) / 30;
    return std::max(1, std::min(500, score));
}

Figures ComputeFigures(const Game& game, const Side& side, int report_index) {
    const Map& map = game.GetMap();
    Figures figures;
    for (int i = 0; i < 8; i++) {
        const Side& other = map.Sides()[i];
        figures.out[i] = !other.IsAlive();
        figures.value[i] = 0;
    }
    if (report_index == kProduction) {
        figures.result = static_cast<int>(game.TurnNews(side.Id()).produced.size());
        return figures;
    }
    if (report_index == kArmy) {
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() >= 0 && army->OwnerIndex() < 8)
                figures.value[army->OwnerIndex()]++;
        }
    } else if (report_index == kCity) {
        for (int i = 0; i < 8; i++)
            figures.value[i] = static_cast<int>(game::SideCities(game, map.Sides()[i]).size());
    } else if (report_index == kGold) {
        for (int i = 0; i < 8; i++) figures.value[i] = map.Sides()[i].Gold();
    } else if (report_index == kWinning) {
        int scores[8];
        for (int i = 0; i < 8; i++) {
            const Side& other = map.Sides()[i];
            scores[i] = other.InUse() ? Score(game, other) : 1;
        }
        int rank = 0;
        for (int i = 0; i < 8; i++)
            if (scores[side.Index()] < scores[i]) rank++;
        for (int i = 0; i < 8; i++) figures.value[i] = scores[i] / 5;
        figures.max = 100;
        figures.result = rank;
        return figures;
    }
    for (int i = 0; i < 8; i++) figures.max = std::max(figures.max, figures.value[i]);
    if (figures.max > 0 && figures.max / 2 == (figures.max - 1) / 2) figures.max++;
    figures.result = figures.value[side.Index()];
    return figures;
}

}  // namespace w2::report
