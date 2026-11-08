// Travel cost over a map: how many movement points a land army needs to get
// from one place to every tile, under the game's own rules (docs/rules.md >
// Movement). Shared by the map evaluator (warlords/mapeval) and the random
// map generator's fair capital placement (warlords/randommap).
//
// The model is the walker's, slightly simplified: a tile costs its terrain's
// movement cost, any tile with a road on it costs 1 (so a road over the
// mountains is a pass), mountains without a road are shut, and an army
// changes between land and sea only at a crossing (a bridge, a marked tile,
// a port city), which ends its move -- counted here as a whole turn.
#pragma once

#include <climits>
#include <cstdint>
#include <vector>

namespace w2::travel {

/** Movement points in a turn: a Light Infantry's. */
constexpr int TURN = 12;
/** Going to sea or ashore ends the move: a turn's worth of points. */
constexpr int SEA_SWITCH = TURN;
/** A tile that cannot be reached. */
constexpr int UNREACHED = INT_MAX;

struct Ground {
  int w = 0, h = 0;
  std::vector<int> terrain;        // terrain type ids (move::ROAD .. move::SITE)
  std::vector<uint8_t> road;       // 1 where a road runs
  std::vector<uint8_t> crossing;   // 1 where land and sea may meet
};

/** Movement points from the nearest of `sources` (tile indices) to every
 *  tile, UNREACHED where it cannot be reached. */
std::vector<int> costs(const Ground& g, const std::vector<int>& sources);

}  // namespace w2::travel
