/**
 * @file
 * @brief Travel cost over a map.
 *
 * How many movement points a land army needs to get from one place to every
 * tile, under the game's own rules (docs/rules.md > Movement). Shared by the
 * map evaluator (warlords/mapeval) and the random map generator's fair
 * capital placement (warlords/randommap).
 *
 * The model is the walker's, slightly simplified: a tile costs its terrain's
 * movement cost, any tile with a road on it costs 1 (so a road over the
 * mountains is a pass), mountains without a road are shut, and an army
 * changes between land and sea only at a crossing (a bridge, a marked tile,
 * a port city), which ends its move -- counted here as a whole turn.
 */
#pragma once

#include <climits>
#include <cstdint>
#include <vector>

namespace w2::travel {

/** @brief Movement points in a turn: a Light Infantry's. */
constexpr int kTurn = 12;
/** @brief Going to sea or ashore ends the move: a turn's worth of points. */
constexpr int kSeaSwitch = kTurn;
/** @brief The cost of a tile that cannot be reached. */
constexpr int kUnreached = INT_MAX;

/** @brief The map as travel sees it: terrain, roads and crossings per tile, row by row. */
struct Ground {
    int w = 0;                      ///< Width in tiles.
    int h = 0;                      ///< Height in tiles.
    std::vector<int> terrain;       ///< Terrain type ids (move::kRoad .. move::kSite).
    std::vector<uint8_t> road;      ///< 1 where a road runs.
    std::vector<uint8_t> crossing;  ///< 1 where land and sea may meet.
};

/**
 * @brief Movement points from the nearest of `sources` to every tile.
 * @param ground The map.
 * @param sources Tile indices (y * width + x) the costs are counted from.
 * @return A cost per tile; kUnreached where it cannot be reached.
 */
std::vector<int> Costs(const Ground& ground, const std::vector<int>& sources);

}  // namespace w2::travel
