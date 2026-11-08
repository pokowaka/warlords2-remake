#include "warlords/travel.hpp"

#include <functional>
#include <queue>
#include <utility>

#include "warlords/move.hpp"

namespace w2::travel {

std::vector<int> costs(const Ground& g, const std::vector<int>& sources) {
  using namespace w2::move;
  const int n = g.w * g.h;
  std::vector<int> dist(n, UNREACHED);
  auto sea = [&](int i) { return g.terrain[i] == WATER || g.terrain[i] == SHORE; };
  auto open = [&](int i) { return g.terrain[i] != MOUNTAINS || g.road[i]; };
  auto step = [&](int i) {
    if (g.road[i]) return 1;
    int c = COST[g.terrain[i]];
    return c > 0 ? c : 1;
  };
  using Item = std::pair<int, int>;   // cost, tile
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
  for (int s : sources) {
    if (s < 0 || s >= n || dist[s] == 0) continue;
    dist[s] = 0;
    q.push({0, s});
  }
  while (!q.empty()) {
    auto [d, i] = q.top();
    q.pop();
    if (d != dist[i]) continue;
    int x = i % g.w, y = i / g.w;
    for (int k = 0; k < 8; k++) {
      int nx = x + DIRS[k][0], ny = y + DIRS[k][1];
      if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) continue;
      int j = ny * g.w + nx;
      if (!open(j)) continue;
      int c = d + step(j);
      if (sea(i) != sea(j)) {
        if (!g.crossing[i] && !g.crossing[j]) continue;
        c += SEA_SWITCH;
      }
      if (c < dist[j]) {
        dist[j] = c;
        q.push({c, j});
      }
    }
  }
  return dist;
}

}  // namespace w2::travel
