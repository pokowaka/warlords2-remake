#include "warlords/travel.hpp"

#include <functional>
#include <queue>
#include <utility>

#include "warlords/move.hpp"

namespace w2::travel {

/**
 * Dijkstra's algorithm over the tiles, from every source at once; the
 * queue holds (cost, tile) and stale entries are skipped as they come up.
 */
std::vector<int> Costs(const Ground& ground, const std::vector<int>& sources) {
    using namespace w2::move;
    const int tile_count = ground.w * ground.h;
    std::vector<int> dist(tile_count, kUnreached);
    auto is_sea = [&](int tile) {
        return ground.terrain[tile] == kWater || ground.terrain[tile] == kShore;
    };
    auto is_open = [&](int tile) {
        return ground.terrain[tile] != kMountains || ground.road[tile];
    };
    auto step_cost = [&](int tile) {
        if (ground.road[tile]) return 1;
        int terrain_cost = kCost[ground.terrain[tile]];
        return terrain_cost > 0 ? terrain_cost : 1;
    };
    using Entry = std::pair<int, int>;  // cost, tile
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
    for (int source : sources) {
        if (source < 0 || source >= tile_count || dist[source] == 0) continue;
        dist[source] = 0;
        frontier.push({0, source});
    }
    while (!frontier.empty()) {
        auto [cost, tile] = frontier.top();
        frontier.pop();
        if (cost != dist[tile]) continue;
        int x = tile % ground.w, y = tile / ground.w;
        for (int dir = 0; dir < 8; dir++) {
            int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
            if (next_x < 0 || next_y < 0 || next_x >= ground.w || next_y >= ground.h) continue;
            int next = next_y * ground.w + next_x;
            if (!is_open(next)) continue;
            int next_cost = cost + step_cost(next);
            if (is_sea(tile) != is_sea(next)) {
                if (!ground.crossing[tile] && !ground.crossing[next]) continue;
                next_cost += kSeaSwitch;
            }
            if (next_cost < dist[next]) {
                dist[next] = next_cost;
                frontier.push({next_cost, next});
            }
        }
    }
    return dist;
}

}  // namespace w2::travel
