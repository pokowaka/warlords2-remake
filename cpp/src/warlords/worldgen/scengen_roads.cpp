/**
 * @file
 * @brief Perlin noise, grid connectivity, and A* and MST road planning implementations for
 *        `w2::scengen`.
 */
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <numeric>
#include <queue>

#include "absl/log/absl_check.h"

#include "warlords/move.hpp"
#include "warlords/worldgen/scengen_impl.hpp"

namespace w2::scengen {

namespace {

using namespace w2::move;  // Terrain type ids.

/** @brief Quintic Perlin fade curve `6t^5 - 15t^4 + 10t^3`. */
double Fade(double val) {
    return val * val * val * (val * (val * 6 - 15) + 10);
}

/** @brief Evaluates one of 8 Perlin 2D gradient directions selected by `hash & 7`. */
double Grad(int hash, double dx, double dy) {
    switch (hash & 7) {
    case 0:
        return dx + dy;
    case 1:
        return dx - dy;
    case 2:
        return -dx + dy;
    case 3:
        return -dx - dy;
    case 4:
        return dx;
    case 5:
        return -dx;
    case 6:
        return dy;
    default:
        return -dy;
    }
}

/** @brief Admissible Manhattan A* heuristic scaled below minimum road-step cost (`0.3`..`0.5`).
 */
double Heuristic(int tile, int width, int to_x, int to_y) {
    return 0.5 * (std::abs(tile % width - to_x) + std::abs(tile / width - to_y));
}

/**
 * @brief Small cross-track penalty nudging A* routes toward staircases along the straight chord
 *        rather than single L-bends.
 */
double CrossTrackPenalty(int tile, int width, int from_x, int from_y, double line_x, double line_y,
                         double len) {
    return 0.08 * std::abs((tile % width - from_x) * line_y - (tile / width - from_y) * line_x) /
           len;
}

/** @brief Disjoint-set lookup with path compression. */
int FindRoot(std::vector<int>& root, int city) {
    int cur = city;
    while (root[cur] != cur) cur = root[cur];
    while (root[city] != cur) {
        int next = root[city];
        root[city] = cur;
        city = next;
    }
    return cur;
}

}  // namespace

// The JavaScript and Lua ports make the same worlds from the same seed, byte
// for byte, so everything here keeps to arithmetic IEEE doubles give alike in
// all three: + - * / sqrt floor, no pow, hypot or lerp (whose last bits differ
// between libraries), sorts with no ties, and queues ordered by (cost, tile).

double Uniform(Rng& rng) {
    return rng.Below(65536) / 65536.0;
}

double Mix(double start, double end, double frac) {
    return start + frac * (end - start);
}

double Dist(double dx, double dy) {
    return std::sqrt(dx * dx + dy * dy);
}

double Cut(std::vector<double> values, double quantile) {
    if (values.empty()) return 0;
    double pos = std::max(0.0, quantile * static_cast<double>(values.size() - 1));
    size_t idx = std::min(values.size() - 1, static_cast<size_t>(pos));
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return values[idx];
}

// ------------------------------------------------------------------ Noise

Noise::Noise(Rng& rng) {
    std::vector<int> order(256);
    std::iota(order.begin(), order.end(), 0);
    rng.Shuffle(order);
    for (int i = 0; i < 512; i++) perm_[i] = order[i & 255];
}

double Noise::At(double x, double y) const {
    int grid_x = static_cast<int>(std::floor(x)), grid_y = static_cast<int>(std::floor(y));
    double frac_x = x - grid_x, frac_y = y - grid_y;
    int cell_x = grid_x & 255, cell_y = grid_y & 255;
    double fade_x = Fade(frac_x), fade_y = Fade(frac_y);
    int hash_aa = perm_[perm_[cell_x] + cell_y], hash_ab = perm_[perm_[cell_x] + cell_y + 1];
    int hash_ba = perm_[perm_[cell_x + 1] + cell_y],
        hash_bb = perm_[perm_[cell_x + 1] + cell_y + 1];
    double interp_top =
            Mix(Grad(hash_aa, frac_x, frac_y), Grad(hash_ba, frac_x - 1, frac_y), fade_x);
    double interp_bot =
            Mix(Grad(hash_ab, frac_x, frac_y - 1), Grad(hash_bb, frac_x - 1, frac_y - 1), fade_x);
    return Mix(interp_top, interp_bot, fade_y);
}

double Noise::Fbm(double x, double y, int octaves) const {
    double sum = 0, amp = 1, norm = 0;
    for (int oct = 0; oct < octaves; oct++) {
        sum += amp * At(x, y);
        norm += amp;
        amp *= 0.5;
        x *= 2.03;
        y *= 2.03;
    }
    return sum / norm;
}

// ------------------------------------------------------------------ Grid

Grid::Grid(int width, int height) : width_(width), height_(height) {
    ABSL_DCHECK_GT(width_, 0);
    ABSL_DCHECK_GT(height_, 0);
}

std::vector<TileComponent> Grid::Parts(const TilePredicate& test, bool eight) const {
    int total = TileCount();
    std::vector<int> seen(static_cast<size_t>(total), 0);
    std::vector<TileComponent> out;
    for (int start = 0; start < total; start++) {
        if (seen[start] || !test(start)) continue;
        TileComponent part{start};
        seen[start] = 1;
        for (size_t idx = 0; idx < part.size(); idx++) {
            int tile = part[idx], x = tile % width_, y = tile / width_;
            for (int dir = 0; dir < (eight ? 8 : 4); dir++) {
                int next_x = x + (eight ? kDirs[dir][0] : kDX4[dir]),
                    next_y = y + (eight ? kDirs[dir][1] : kDY4[dir]);
                if (!In(next_x, next_y)) continue;
                int next = next_y * width_ + next_x;
                if (!seen[next] && test(next)) {
                    seen[next] = 1;
                    part.push_back(next);
                }
            }
        }
        out.push_back(std::move(part));
    }
    return out;
}

std::vector<int> Grid::Distance(const TilePredicate& from) const {
    int total = TileCount();
    std::vector<int> dist(static_cast<size_t>(total), -1);
    std::deque<int> queue;
    for (int i = 0; i < total; i++)
        if (from(i)) {
            dist[i] = 0;
            queue.push_back(i);
        }
    while (!queue.empty()) {
        int tile = queue.front();
        queue.pop_front();
        int x = tile % width_, y = tile / width_;
        for (int dir = 0; dir < 4; dir++) {
            int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
            if (!In(next_x, next_y)) continue;
            int next = next_y * width_ + next_x;
            if (dist[next] < 0) {
                dist[next] = dist[tile] + 1;
                queue.push_back(next);
            }
        }
    }
    return dist;
}

// ------------------------------------------------------------------ RoadPlanner

RoadPlanner::RoadPlanner(absl::Span<const uint8_t> terrain, int width, int height,
                         absl::Span<const TileCoords> cities, absl::Span<const Span> spans)
        : terrain_(terrain)
        , cities_(cities)
        , spans_(spans)
        , grid_(width, height)
        , leap_(static_cast<size_t>(grid_.TileCount()))
        , door_(cities.size())
        , door_of_(static_cast<size_t>(grid_.TileCount()), -1)
        , road_(static_cast<size_t>(grid_.TileCount()), 0) {
    ABSL_DCHECK_EQ(terrain_.size(), static_cast<size_t>(grid_.TileCount()));
    for (size_t idx = 0; idx < spans_.size(); idx++) {
        const auto& span = spans_[idx];
        int start_x = span.ew ? span.x - 1 : span.x, start_y = span.ew ? span.y : span.y - 1;
        int end_x = span.ew ? span.x + 2 : span.x, end_y = span.ew ? span.y : span.y + 2;
        if (!grid_.In(start_x, start_y) || !grid_.In(end_x, end_y)) continue;
        int start_tile = start_y * width + start_x, end_tile = end_y * width + end_x;
        leap_[start_tile].push_back({end_tile, static_cast<int>(idx)});
        leap_[end_tile].push_back({start_tile, static_cast<int>(idx)});
    }
    for (size_t city = 0; city < cities_.size(); city++) {
        door_[city] = CityDoors(static_cast<int>(city));
        for (int tile : door_[city]) door_of_[tile] = static_cast<int>(city);
    }
}

double RoadPlanner::BuildCost(int tile) const {
    switch (terrain_[tile]) {
    case kPlain:
        return 1;
    case kForest:
        return 2;
    case kHills:
        return 3;
    case kMarsh:
        return 5;
    case kMountains:
        return 9;
    default:
        return -1;  // Water, shore, bridges, cities, sites, signposts.
    }
}

std::vector<int> RoadPlanner::CityDoors(int city_idx) const {
    auto [x, y] = cities_[city_idx];
    int width = grid_.Width();
    std::vector<int> door_tiles;
    const int ring[8][2] = {{0, -1}, {1, -1}, {2, 0}, {2, 1}, {1, 2}, {0, 2}, {-1, 1}, {-1, 0}};
    for (const auto& off : ring) {
        int dx = x + off[0], dy = y + off[1];
        if (grid_.In(dx, dy) && BuildCost(dy * width + dx) > 0 &&
            terrain_[dy * width + dx] != kMountains)
            door_tiles.push_back(dy * width + dx);
    }
    return door_tiles;
}

double RoadPlanner::Route(int from_city, int to_city, std::vector<int>* tiles,
                          std::vector<int>* used) const {
    if (door_[from_city].empty() || door_[to_city].empty()) return -1;
    int width = grid_.Width(), tile_count = grid_.TileCount();
    auto [to_x, to_y] = cities_[to_city];
    auto [from_x, from_y] = cities_[from_city];
    double line_x = to_x - from_x, line_y = to_y - from_y;
    double len = std::max(1.0, Dist(line_x, line_y));

    std::vector<double> cost(tile_count, 1e18);
    std::vector<int> from(tile_count, -1), via(tile_count, -1);
    using QueueItem = std::pair<double, int>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
    for (int tile : door_[from_city]) {
        cost[tile] = road_[tile] ? 0.3 : BuildCost(tile);
        queue.push({cost[tile] + Heuristic(tile, width, to_x, to_y), tile});
    }
    int end = -1;
    while (!queue.empty()) {
        auto [est, tile] = queue.top();
        queue.pop();
        if (est - Heuristic(tile, width, to_x, to_y) > cost[tile] + 1e-9) continue;
        if (door_of_[tile] == to_city) {
            end = tile;
            break;
        }
        int x = tile % width, y = tile / width;
        for (int dir = 0; dir < 4; dir++) {
            int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
            if (!grid_.In(next_x, next_y)) continue;
            int next = next_y * width + next_x;
            double step_cost = BuildCost(next);
            if (step_cost < 0) continue;
            double next_cost =
                    cost[tile] +
                    (road_[next] ? 0.3
                                 : step_cost + CrossTrackPenalty(next, width, from_x, from_y,
                                                                 line_x, line_y, len));
            if (next_cost < cost[next]) {
                cost[next] = next_cost;
                from[next] = tile;
                via[next] = -1;
                queue.push({next_cost + Heuristic(next, width, to_x, to_y), next});
            }
        }
        for (const auto& [next, span] : leap_[tile]) {
            double next_cost = cost[tile] + 4;
            if (next_cost < cost[next]) {
                cost[next] = next_cost;
                from[next] = tile;
                via[next] = span;
                queue.push({next_cost + Heuristic(next, width, to_x, to_y), next});
            }
        }
    }
    if (end < 0) return -1;
    if (tiles) {
        for (int tile = end; tile >= 0; tile = from[tile]) {
            tiles->push_back(tile);
            if (used && via[tile] >= 0) used->push_back(via[tile]);
        }
    }
    return cost[end];
}

double RoadPlanner::TreeCost(const std::vector<std::vector<TreeEdge>>& tree, int from_city,
                             int to_city) {
    std::vector<double> dist(tree.size(), -1);
    std::deque<int> queue{from_city};
    dist[from_city] = 0;
    while (!queue.empty()) {
        int cur = queue.front();
        queue.pop_front();
        for (const auto& [next, edge_cost] : tree[cur])
            if (dist[next] < 0) {
                dist[next] = dist[cur] + edge_cost;
                queue.push_back(next);
            }
    }
    return dist[to_city];
}

bool RoadPlanner::CheaperLink(const Link& lhs, const Link& rhs) {
    if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
    return lhs.city_a != rhs.city_a ? lhs.city_a < rhs.city_a : lhs.city_b < rhs.city_b;
}

Roads RoadPlanner::Plan(Rng& rng) {
    std::vector<Link> links;
    int city_count = static_cast<int>(cities_.size());
    for (int city_a = 0; city_a < city_count; city_a++) {
        std::vector<std::pair<double, int>> near;
        for (int city_b = 0; city_b < city_count; city_b++)
            if (city_b != city_a)
                near.push_back({Dist(cities_[city_a].first - cities_[city_b].first,
                                     cities_[city_a].second - cities_[city_b].second),
                                city_b});
        std::sort(near.begin(), near.end());
        for (int idx = 0; idx < 4 && idx < static_cast<int>(near.size()); idx++) {
            int city_b = near[idx].second;
            if (near[idx].first > 26 || city_b < city_a) continue;
            double road_cost = Route(city_a, city_b, nullptr, nullptr);
            if (road_cost > 0) links.push_back({road_cost, city_a, city_b});
        }
    }
    std::sort(links.begin(), links.end(), CheaperLink);

    // Build a Kruskal minimum spanning tree, then add shortcuts where the tree detours badly.
    std::vector<int> root(city_count);
    std::iota(root.begin(), root.end(), 0);
    std::vector<std::vector<TreeEdge>> tree(city_count);
    std::vector<Link> chosen;
    for (const auto& link : links) {
        int root_a = FindRoot(root, link.city_a), root_b = FindRoot(root, link.city_b);
        if (root_a == root_b || link.cost > 34) continue;
        root[root_a] = root_b;
        chosen.push_back(link);
        tree[link.city_a].push_back({link.city_b, link.cost});
        tree[link.city_b].push_back({link.city_a, link.cost});
    }
    for (const auto& link : links) {
        double detour = TreeCost(tree, link.city_a, link.city_b);
        if (detour > 2.2 * link.cost && link.cost <= 24 && Uniform(rng) < 0.7) {
            chosen.push_back(link);
            tree[link.city_a].push_back({link.city_b, link.cost});
            tree[link.city_b].push_back({link.city_a, link.cost});
        }
    }

    // Lay chosen links cheapest first so later links merge into roads already laid.
    std::sort(chosen.begin(), chosen.end(), CheaperLink);
    std::vector<uint8_t> bridged(spans_.size(), 0);
    for (const auto& link : chosen) {
        std::vector<int> tiles, used;
        if (Route(link.city_a, link.city_b, &tiles, &used) < 0) continue;
        for (int tile : tiles) road_[tile] = 1;
        for (int span_idx : used) bridged[span_idx] = 1;
    }

    Roads out;
    out.road = std::move(road_);
    for (size_t idx = 0; idx < spans_.size(); idx++)
        if (bridged[idx]) out.bridges.push_back(static_cast<int>(idx));
    return out;
}

Roads PlanRoads(const std::vector<uint8_t>& terrain, int width, int height,
                const std::vector<std::pair<int, int>>& cities, const std::vector<Span>& spans,
                Rng& rng) {
    RoadPlanner planner(terrain, width, height, cities, spans);
    return planner.Plan(rng);
}

}  // namespace w2::scengen
