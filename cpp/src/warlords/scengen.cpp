#include "warlords/scengen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <numeric>
#include <queue>

#include "warlords/move.hpp"

namespace w2::scengen {

namespace {

using namespace w2::move;  // terrain type ids

constexpr int kDX4[4] = {0, 1, 0, -1}, kDY4[4] = {-1, 0, 1, 0};

// The JavaScript and Lua ports make the same worlds from the same seed, byte
// for byte, so everything here keeps to arithmetic IEEE doubles give alike
// in all three: + - * / sqrt floor, no pow, hypot or lerp (whose last bits
// differ between libraries), sorts with no ties, and queues ordered by
// (cost, tile), so any heap pops them in the same order.

/** @brief Draws a uniform random number in [0, 1). */
double Uniform(Rng& rng) {
    return rng.Below(65536) / 65536.0;
}

/** @brief Linearly interpolates between `start` and `end` by `frac`. */
double Mix(double start, double end, double frac) {
    return start + frac * (end - start);
}

/** @brief Euclidean distance for offset (`dx`, `dy`). */
double Dist(double dx, double dy) {
    return std::sqrt(dx * dx + dy * dy);
}

/** @brief Perlin's gradient noise, its permutation drawn from the dice. */
class Noise {
  public:
    explicit Noise(Rng& rng) {
        std::vector<int> order(256);
        std::iota(order.begin(), order.end(), 0);
        rng.Shuffle(order);
        for (int i = 0; i < 512; i++) perm_[i] = order[i & 255];
    }

    /** @brief Evaluates 2D Perlin noise at (`x`, `y`), returning about -1..1. */
    double At(double x, double y) const {
        int grid_x = static_cast<int>(std::floor(x)), grid_y = static_cast<int>(std::floor(y));
        double frac_x = x - grid_x, frac_y = y - grid_y;
        int cell_x = grid_x & 255, cell_y = grid_y & 255;
        auto fade = [](double val) { return val * val * val * (val * (val * 6 - 15) + 10); };
        auto grad = [&](int hash, double dx, double dy) {
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
        };
        double fade_x = fade(frac_x), fade_y = fade(frac_y);
        int hash_aa = perm_[perm_[cell_x] + cell_y], hash_ab = perm_[perm_[cell_x] + cell_y + 1];
        int hash_ba = perm_[perm_[cell_x + 1] + cell_y],
            hash_bb = perm_[perm_[cell_x + 1] + cell_y + 1];
        double interp_top =
                Mix(grad(hash_aa, frac_x, frac_y), grad(hash_ba, frac_x - 1, frac_y), fade_x);
        double interp_bot = Mix(grad(hash_ab, frac_x, frac_y - 1),
                                grad(hash_bb, frac_x - 1, frac_y - 1), fade_x);
        return Mix(interp_top, interp_bot, fade_y);
    }

    /** @brief Fractal Brownian motion: octaves of the noise, each twice as fine. */
    double Fbm(double x, double y, int octaves = 5) const {
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

  private:
    std::array<int, 512> perm_{};  ///< Doubled 256-entry permutation table.
};

/** @brief The value at `quantile` (0..1) of the listed `values`. */
double Cut(std::vector<double> values, double quantile) {
    if (values.empty()) return 0;
    double pos = std::max(0.0, quantile * static_cast<double>(values.size() - 1));
    size_t idx = std::min(values.size() - 1, static_cast<size_t>(pos));
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return values[idx];
}

/** @brief Dimensions and connectivity helpers for a tile grid. */
struct Grid {
    int w;  ///< Width in tiles.
    int h;  ///< Height in tiles.

    /** @brief Returns true if (`x`, `y`) lies within the grid bounds. */
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    /** @brief The 8-connected (or 4-connected) parts of the tiles that pass `test`. */
    std::vector<std::vector<int>> Parts(const std::function<bool(int)>& test,
                                        bool eight = true) const {
        std::vector<int> seen(static_cast<size_t>(w) * h, 0);
        std::vector<std::vector<int>> out;
        for (int start = 0; start < w * h; start++) {
            if (seen[start] || !test(start)) continue;
            std::vector<int> part{start};
            seen[start] = 1;
            for (size_t idx = 0; idx < part.size(); idx++) {
                int tile = part[idx], x = tile % w, y = tile / w;
                for (int dir = 0; dir < (eight ? 8 : 4); dir++) {
                    int next_x = x + (eight ? kDirs[dir][0] : kDX4[dir]),
                        next_y = y + (eight ? kDirs[dir][1] : kDY4[dir]);
                    if (!In(next_x, next_y)) continue;
                    int next = next_y * w + next_x;
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

    /** @brief Steps (4-connected) from the nearest tile passing `from`. */
    std::vector<int> Distance(const std::function<bool(int)>& from) const {
        std::vector<int> dist(static_cast<size_t>(w) * h, -1);
        std::deque<int> queue;
        for (int i = 0; i < w * h; i++)
            if (from(i)) {
                dist[i] = 0;
                queue.push_back(i);
            }
        while (!queue.empty()) {
            int tile = queue.front();
            queue.pop_front();
            int x = tile % w, y = tile / w;
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!In(next_x, next_y)) continue;
                int next = next_y * w + next_x;
                if (dist[next] < 0) {
                    dist[next] = dist[tile] + 1;
                    queue.push_back(next);
                }
            }
        }
        return dist;
    }
};

/** @brief Returns the default target land share for `shape`. */
double DefaultLand(Shape shape) {
    switch (shape) {
    case Shape::kPangaea:
        return 0.60;
    case Shape::kIslands:
        return 0.42;
    default:
        return 0.55;
    }
}

// ------------------------------------------------------------------ land

/** @brief Step 1: where the land is (true) and its height (0..1 inland). */
std::vector<bool> Land(const Settings& settings, Rng& rng, const Grid& grid,
                       std::vector<double>& height) {
    int width = grid.w, height_tiles = grid.h, tile_count = width * height_tiles;
    Noise base(rng), warp1(rng), warp2(rng), channel(rng);
    double freq = settings.shape == Shape::kIslands ? 1.0 / 26 : 1.0 / 42;
    std::vector<double> elev(tile_count);
    for (int y = 0; y < height_tiles; y++)
        for (int x = 0; x < width; x++) {
            double sample_x = x * freq, sample_y = y * freq;
            // domain warping: the noise read where another noise points
            double warp_x = warp1.Fbm(sample_x + 5.2, sample_y + 1.3, 4),
                   warp_y = warp2.Fbm(sample_x + 1.7, sample_y + 9.2, 4);
            double val = base.Fbm(sample_x + 1.6 * warp_x, sample_y + 1.6 * warp_y, 5);
            // the sea closes in at the map's edges
            double edge = std::min({x, width - 1 - x, y, height_tiles - 1 - y}) / 14.0;
            edge = std::min(1.0, edge);
            val -= (1 - edge) * (1 - edge) * 0.9;
            if (settings.shape == Shape::kContinents) {
                // straits along the zero lines of a slow noise split the land
                double chan = std::abs(channel.Fbm(x / 70.0 + 3.1, y / 70.0 + 7.7, 3));
                if (chan < 0.09) val -= 0.45 * (0.09 - chan) / 0.09;
            } else if (settings.shape == Shape::kPangaea) {
                double dx = (x - width / 2.0) / (width / 2.0),
                       dy = (y - height_tiles / 2.0) / (height_tiles / 2.0);
                val += 0.45 * (1 - std::min(1.0, std::sqrt(dx * dx + dy * dy)));
            }
            elev[y * width + x] = val;
        }
    double share = settings.land >= 0 ? settings.land : DefaultLand(settings.shape);
    double sea = Cut(elev, 1 - share);
    std::vector<bool> is_land(tile_count);
    for (int i = 0; i < tile_count; i++) is_land[i] = elev[i] > sea;
    // smooth the coast so the tile set can draw it: a tile goes with most of
    // its neighbours
    for (int pass = 0; pass < 2; pass++) {
        auto next = is_land;
        for (int y = 0; y < height_tiles; y++)
            for (int x = 0; x < width; x++) {
                int land_neighbors = 0;
                for (int dir = 0; dir < 8; dir++) {
                    int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
                    land_neighbors += grid.In(next_x, next_y) && is_land[next_y * width + next_x];
                }
                if (land_neighbors >= 5)
                    next[y * width + x] = true;
                else if (land_neighbors <= 3)
                    next[y * width + x] = false;
            }
        is_land = next;
    }
    // no land on the map's outermost two rows
    for (int y = 0; y < height_tiles; y++)
        for (int x = 0; x < width; x++)
            if (x < 2 || y < 2 || x >= width - 2 || y >= height_tiles - 2)
                is_land[y * width + x] = false;
    // specks of land go under; small lakes fill in
    int min_island = settings.shape == Shape::kIslands ? 40 : 70;
    for (auto& part : grid.Parts([&](int tile) { return is_land[tile]; }))
        if (static_cast<int>(part.size()) < min_island)
            for (int tile : part) is_land[tile] = false;
    for (auto& part : grid.Parts([&](int tile) { return !is_land[tile]; }, false))
        if (static_cast<int>(part.size()) < 30)
            for (int tile : part) is_land[tile] = true;
    // the height inland: distance from the coast, roughened by the noise
    auto coast = grid.Distance([&](int tile) { return !is_land[tile]; });
    height.assign(tile_count, 0);
    for (int i = 0; i < tile_count; i++)
        if (is_land[i])
            height[i] = 0.65 * std::min(1.0, coast[i] / 14.0) +
                        0.35 * (elev[i] - sea) / std::max(1e-6, 1 - sea);
    return is_land;
}

// ------------------------------------------------------------------ relief

/**
 * @brief Step 2: ranges along ridged noise, hills round them, and a pass into
 *        every pocket of ground the mountains would wall off.
 */
void Relief(const Settings& settings, Rng& rng, const Grid& grid, std::vector<uint8_t>& terrain,
            const std::vector<double>& height) {
    int width = grid.w, tile_count = width * grid.h;
    Noise ridges(rng), lumps(rng);
    auto coast = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    std::vector<double> score(tile_count, -1), eligible;
    for (int i = 0; i < tile_count; i++) {
        if (terrain[i] == kWater) continue;
        int x = i % width, y = i / width;
        double ridge = 1 - std::abs(ridges.Fbm(x / 30.0, y / 30.0, 4));  // ridged: high along lines
        score[i] = ridge * ridge + 0.35 * height[i];
        if (coast[i] >= 2) eligible.push_back(score[i]);
    }
    int land_tiles = 0;
    for (int i = 0; i < tile_count; i++) land_tiles += terrain[i] != kWater;
    // the crests first, at a third of the share, then the ranges widened
    // round them onto the next-highest ground, so they are thick enough to
    // draw as ranges rather than lines
    double m_share =
            std::min(0.9, settings.mountains * land_tiles / std::max<size_t>(1, eligible.size()));
    double crest = Cut(eligible, 1 - m_share / 3), flank = Cut(eligible, 1 - m_share * 1.6);
    for (int i = 0; i < tile_count; i++)
        if (score[i] >= crest && coast[i] >= 2) terrain[i] = kMountains;
    for (int pass = 0; pass < 2; pass++) {
        auto next = terrain;
        for (int i = 0; i < tile_count; i++) {
            if (terrain[i] == kWater || terrain[i] == kMountains || coast[i] < 2 ||
                score[i] < flank)
                continue;
            int x = i % width, y = i / width;
            for (int dir = 0; dir < 8; dir++) {
                int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
                if (grid.In(next_x, next_y) && terrain[next_y * width + next_x] == kMountains) {
                    next[i] = kMountains;
                    break;
                }
            }
        }
        terrain = next;
    }
    // hills: the foothills and lumps of their own
    std::vector<double> hscore(tile_count, -1), hill_scores;
    auto near_mount = grid.Distance([&](int tile) { return terrain[tile] == kMountains; });
    for (int i = 0; i < tile_count; i++) {
        if (terrain[i] != kPlain) continue;
        int x = i % width, y = i / width;
        double lump = lumps.Fbm(x / 13.0 + 11.3, y / 13.0 + 2.9, 3);
        double foot = near_mount[i] >= 0 && near_mount[i] <= 2 ? 0.6 - 0.2 * near_mount[i] : 0;
        hscore[i] = lump + foot + 0.2 * score[i];
        hill_scores.push_back(hscore[i]);
    }
    double h_cut =
            Cut(hill_scores, 1 - std::min(0.9, settings.hills * land_tiles /
                                                       std::max<size_t>(1, hill_scores.size())));
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] == kPlain && hscore[i] >= h_cut) terrain[i] = kHills;
    // passes: on each landmass, every pocket of open ground the mountains cut
    // off gets the cheapest way through them to the landmass's largest one
    for (auto& mass : grid.Parts([&](int tile) { return terrain[tile] != kWater; })) {
        std::vector<uint8_t> on_mass(tile_count, 0);
        for (int tile : mass) on_mass[tile] = 1;
        auto pockets =
                grid.Parts([&](int tile) { return on_mass[tile] && terrain[tile] != kMountains; });
        if (pockets.size() < 2) continue;
        std::sort(pockets.begin(), pockets.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.size() != rhs.size() ? lhs.size() > rhs.size() : lhs[0] < rhs[0];
        });
        // 0-1 search from the largest: a mountain tile costs 1, open ground 0
        std::vector<int> cost(tile_count, 1 << 30), from(tile_count, -1);
        std::deque<int> queue;
        for (int tile : pockets[0]) {
            cost[tile] = 0;
            queue.push_back(tile);
        }
        while (!queue.empty()) {
            int tile = queue.front();
            queue.pop_front();
            int x = tile % width, y = tile / width;
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!grid.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                if (!on_mass[next]) continue;
                int next_cost = cost[tile] + (terrain[next] == kMountains);
                if (next_cost < cost[next]) {
                    cost[next] = next_cost;
                    from[next] = tile;
                    if (terrain[next] == kMountains)
                        queue.push_back(next);
                    else
                        queue.push_front(next);
                }
            }
        }
        for (size_t idx = 1; idx < pockets.size(); idx++)
            for (int tile = pockets[idx][0]; tile >= 0 && cost[tile] > 0; tile = from[tile])
                if (terrain[tile] == kMountains) terrain[tile] = kHills;
    }
}

// ------------------------------------------------------------------ rivers

/**
 * @brief Step 3: rivers from the feet of the hills down to the sea (or into
 *        another river), along the cheapest path over a noise field, so they
 *        wind along its valleys. Two tiles wide, as the tile set draws rivers
 *        best.
 */
void Rivers(const Settings& settings, Rng& rng, const Grid& grid, std::vector<uint8_t>& terrain) {
    int width = grid.w, tile_count = width * grid.h;
    Noise wander(rng);
    auto coast = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    auto mount = grid.Distance(
            [&](int tile) { return terrain[tile] == kMountains || terrain[tile] == kHills; });
    // sources: well inland, by the high ground, apart from each other
    std::vector<int> sources;
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] != kWater && terrain[i] != kMountains && terrain[i] != kHills &&
            coast[i] >= 6 && mount[i] >= 1 && mount[i] <= 2)
            sources.push_back(i);
    rng.Shuffle(sources);
    int want = settings.rivers;
    std::vector<uint8_t> river(tile_count, 0);
    std::vector<int> started;
    // the way down: the cheapest path to the sea, a tile costing more where
    // the noise is high, so the river follows the noise's valleys and winds
    std::vector<double> cost(tile_count);
    for (int i = 0; i < tile_count; i++) {
        double val = 0.5 + 0.5 * wander.Fbm((i % width) / 12.0 + 3.3, (i / width) / 12.0 + 1.1, 3);
        cost[i] = 1 + 14 * val * val;
    }
    for (int src : sources) {
        if (static_cast<int>(started.size()) >= want) break;
        bool apart = true;
        for (int other : started)
            if (Dist(other % width - src % width, other / width - src / width) < 22) apart = false;
        if (!apart) continue;
        std::vector<double> best(tile_count, 1e18);
        std::vector<int> from(tile_count, -1);
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        best[src] = 0;
        queue.push({0, src});
        int end = -1;
        while (!queue.empty()) {
            auto [cur_cost, tile] = queue.top();
            queue.pop();
            if (cur_cost > best[tile]) continue;
            if (terrain[tile] == kWater || river[tile]) {
                end = tile;
                break;
            }
            int x = tile % width, y = tile / width;
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!grid.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                if (terrain[next] == kMountains || terrain[next] == kHills) continue;
                double next_cost = cur_cost + cost[next];
                if (next_cost < best[next]) {
                    best[next] = next_cost;
                    from[next] = tile;
                    queue.push({next_cost, next});
                }
            }
        }
        if (end < 0) continue;
        std::vector<int> path;
        for (int tile = from[end]; tile >= 0; tile = from[tile]) path.push_back(tile);
        std::reverse(path.begin(), path.end());
        if (path.size() < 12) continue;
        started.push_back(src);
        for (size_t idx = 0; idx < path.size(); idx++) {
            int tile = path[idx], x = tile % width, y = tile / width;
            river[tile] = 1;
            // the second tile: beside it, across the way the river runs
            int adj = idx + 1 < path.size() ? path[idx + 1] : (idx > 0 ? path[idx - 1] : tile);
            bool along_x = std::abs(adj - tile) == 1;
            int off_x = along_x ? 0 : 1, off_y = along_x ? 1 : 0;
            if (grid.In(x + off_x, y + off_y) &&
                terrain[(y + off_y) * width + x + off_x] != kMountains)
                river[(y + off_y) * width + x + off_x] = 1;
        }
    }
    for (int i = 0; i < tile_count; i++)
        if (river[i]) terrain[i] = kWater;
}

// ------------------------------------------------------------------ growth

/** @brief Step 4: forest where it is wet, marsh where it is wet and low. */
void Growth(const Settings& settings, Rng& rng, const Grid& grid, std::vector<uint8_t>& terrain,
            const std::vector<double>& height) {
    int width = grid.w, tile_count = width * grid.h;
    Noise wet(rng);
    auto water = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    std::vector<double> moisture(tile_count, -9), moistures;
    int land_tiles = 0;
    for (int i = 0; i < tile_count; i++) {
        if (terrain[i] == kWater) continue;
        land_tiles++;
        if (terrain[i] != kPlain) continue;
        int x = i % width, y = i / width;
        moisture[i] = wet.Fbm(x / 26.0 + 4.4, y / 26.0 + 8.8, 4) +
                      (water[i] <= 3 ? 0.08 * (4 - water[i]) : 0);
        moistures.push_back(moisture[i]);
    }
    if (moistures.empty()) return;
    double f_cut =
            Cut(moistures, 1 - std::min(0.9, settings.forest * land_tiles / moistures.size()));
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] == kPlain && moisture[i] >= f_cut) terrain[i] = kForest;
    // marsh: a few small patches on the wettest low ground by the water
    std::vector<double> low;
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] == kPlain && water[i] <= 2) low.push_back(moisture[i] - height[i]);
    double m_cut = Cut(low, 0.97);
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] == kPlain && water[i] <= 2 && moisture[i] - height[i] >= m_cut)
            terrain[i] = kMarsh;
}

// ------------------------------------------------------------------ cities, sites

/** @brief Returns true if a 2x2 city footprint at (`x`, `y`) lies on plain or forest within bounds.
 */
bool FootprintOk(const Grid& grid, const std::vector<uint8_t>& terrain, int x, int y) {
    if (x < 3 || y < 3 || x + 1 >= grid.w - 3 || y + 1 >= grid.h - 3) return false;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            int terr = terrain[(y + dy) * grid.w + x + dx];
            if (terr != kPlain && terr != kForest) return false;
        }
    return true;
}

/**
 * @brief Best-candidate sampling (Mitchell): each new point the best of a few
 *        random candidates, the best being the furthest from the points already
 *        placed, plus `bonus`. Gives an even spread with no clumps.
 */
std::vector<std::pair<int, int>> Spread(Rng& rng, const std::vector<int>& spots, int count,
                                        int width, const std::vector<std::pair<int, int>>& avoid,
                                        double min_dist, const std::function<double(int)>& bonus,
                                        int candidates) {
    std::vector<std::pair<int, int>> out;
    if (spots.empty()) return out;
    auto nearest = [&](int x, int y) {
        double best = 1e9;
        for (auto& [avoid_x, avoid_y] : out) best = std::min(best, Dist(avoid_x - x, avoid_y - y));
        for (auto& [avoid_x, avoid_y] : avoid)
            best = std::min(best, Dist(avoid_x - x, avoid_y - y));
        return best;
    };
    for (int idx = 0; idx < count; idx++) {
        int pick = -1;
        double pick_score = -1e9;
        // `candidates` that keep their distance; on crowded land it may take
        // many draws to find them
        for (int tries = 0, found = 0; found < candidates && tries < 600; tries++) {
            int spot = spots[rng.Below(static_cast<int>(spots.size()))];
            int x = spot % width, y = spot / width;
            double dist = nearest(x, y);
            if (dist < min_dist) continue;
            found++;
            double score = std::min(dist, 12.0) + bonus(spot);
            if (score > pick_score) {
                pick_score = score;
                pick = spot;
            }
        }
        if (pick < 0) continue;
        out.push_back({pick % width, pick / width});
    }
    return out;
}

// the words a world's name is made of
const char* const kAdjectives[] = {
    "Shattered", "Sundered", "Burning", "Frozen",  "Whispering", "Forgotten", "Gilded",  "Crimson",
    "Iron",      "Silver",   "Hollow",  "Endless", "Drowned",    "Ashen",     "Emerald", "Howling",
    "Sunken",    "Twilight", "Broken",  "Haunted", "Golden",     "Bitter",    "Wild",    "Lost"};
const char* const kFirst[] = {"Thorn",  "Dusk",   "Ember", "Raven", "Storm", "Frost",
                              "Gold",   "Wolf",   "Mist",  "Drake", "Shade", "Iron",
                              "Bright", "Grim",   "Moon",  "Star",  "Oak",   "Black",
                              "Red",    "Winter", "Wyrm",  "Ash",   "Salt",  "Hawk"};
const char* const kSecond[] = {"mere",  "hold",  "vale", "fell",  "wick", "haven", "march", "moor",
                               "crest", "reach", "gard", "shire", "fall", "wood",  "spire", "ford"};
const char* const kNouns[3][8] = {{"Kingdoms", "Marches", "Dominions", "Realms", "Baronies",
                                   "Lands", "Reaches", "Shires"},  // continents
                                  {"Realm", "Expanse", "Heartland", "Empire", "Dominion", "Wilds",
                                   "Steppe", "Highlands"},  // pangaea
                                  {"Isles", "Archipelago", "Atolls", "Shoals", "Skerries",
                                   "Islands", "Straits", "Sea Lords"}};  // islands
const char* const kLines[] = {"Eight warlords, one throne",    "Where empires rise and fall",
                              "Dragons stir beyond the hills", "Gold, steel and treachery",
                              "A land waiting for a king",     "Fortune favours the bold",
                              "Old ruins, older magic",        "Heroes wanted. Apply within",
                              "The war begins at dawn",        "Every road leads to war",
                              "Crowns are won, not given",     "Banners over every hill",
                              "Beware the dark between towns", "Here be monsters (and loot)",
                              "Many cities, one Warlord",      "Sharpen your swords"};

/** @brief Picks a uniform random entry from a fixed array. */
template <class T, size_t N>
const char* PickWord(Rng& rng, const T (&list)[N]) {
    return list[rng.Below(static_cast<int>(N))];
}

// How far each letter moves the pen in CHANCE17, the font New Scenario's
// list draws names in (CHANCE17.FIN), and the widest name the list's box
// holds: "The Frozen Steppe", 140 pixels; the shipped names reach 130.
constexpr int kUpperWidth[26] = {15, 16, 13, 17, 15, 18, 15, 16, 12, 14, 15, 14, 18,
                                 16, 15, 15, 15, 16, 14, 13, 16, 17, 24, 16, 13, 14};
constexpr int kLowerWidth[26] = {7, 6, 7, 9, 6, 8, 6, 7, 4, 7,  7, 4, 12,
                                 8, 7, 8, 6, 6, 6, 5, 9, 9, 14, 9, 9, 8};
constexpr int kNameWidth = 140, kNameChars = 20;

/** @brief Pixel width of `text` in the CHANCE17 scenario-list font. */
int NameWidth(const std::string& text) {
    int width = 0;
    for (char chr : text) {
        if (chr >= 'A' && chr <= 'Z')
            width += kUpperWidth[chr - 'A'];
        else if (chr >= 'a' && chr <= 'z')
            width += kLowerWidth[chr - 'a'];
        else if (chr == ' ')
            width += 7;
        else if (chr == '\'')
            width += 3;
        else
            width += 16;
    }
    return width;
}

/** @brief Returns true if `text` fits in the scenario selection list box. */
bool Fits(const std::string& text) {
    return static_cast<int>(text.size()) <= kNameChars && NameWidth(text) <= kNameWidth;
}

}  // namespace

Title MakeTitle(Shape shape, long seed) {
    Rng rng(static_cast<double>(seed) +
            1000003.0);  // its own dice: naming never touches the world's
    const auto& nouns = kNouns[shape == Shape::kPangaea ? 1 : shape == Shape::kIslands ? 2 : 0];
    Title title;
    std::string noun;
    for (int tries = 0; tries < 50; tries++) {
        std::string place = PickWord(rng, kFirst);  // two statements: the dice in a fixed order
        place += PickWord(rng, kSecond);
        noun = PickWord(rng, nouns);
        std::string adj = PickWord(rng, kAdjectives);
        int form = rng.Below(3);
        if (form == 0)
            title.name = "The " + adj + " " + noun;
        else if (form == 1)
            title.name = noun + " of " + place;
        else
            title.name = place + " " + noun;
        if (Fits(title.name)) break;
    }
    if (!Fits(title.name)) title.name = "The " + noun;  // fits whatever the noun
    title.text = PickWord(rng, kLines);
    return title;
}

bool ParseShape(const std::string& text, Shape& out) {
    if (text == "continents")
        out = Shape::kContinents;
    else if (text == "pangaea")
        out = Shape::kPangaea;
    else if (text == "islands")
        out = Shape::kIslands;
    else
        return false;
    return true;
}

const char* ShapeName(Shape shape) {
    switch (shape) {
    case Shape::kPangaea:
        return "pangaea";
    case Shape::kIslands:
        return "islands";
    default:
        return "continents";
    }
}

World MakeWorld(const Settings& settings, Rng& rng, int width, int height) {
    Grid grid{width, height};
    int tile_count = width * height;
    World world;
    std::vector<double> elev_height;
    auto is_land = Land(settings, rng, grid, elev_height);
    auto& terrain = world.terrain;
    terrain.assign(tile_count, kWater);
    for (int i = 0; i < tile_count; i++)
        if (is_land[i]) terrain[i] = kPlain;
    Relief(settings, rng, grid, terrain, elev_height);
    Rivers(settings, rng, grid, terrain);
    Growth(settings, rng, grid, terrain, elev_height);

    // cities: on open ground with room round them, rather by water
    auto water = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    std::vector<int> spots;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (FootprintOk(grid, terrain, x, y)) spots.push_back(y * width + x);
    int want = std::clamp(settings.cities, 8, 100);
    world.cities = Spread(
            rng, spots, want, width, {}, 2.9,
            [&](int tile) {
                int dist = water[tile];
                return dist >= 0 && dist <= 2 ? 0.6 : 0.0;
            },
            4);
    for (auto& [x, y] : world.cities)
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) terrain[(y + dy) * width + x + dx] = kCity;

    // sites: in the wilds, on ground an army can walk to from a city
    auto open = grid.Parts(
            [&](int tile) { return terrain[tile] != kWater && terrain[tile] != kMountains; });
    std::vector<int> part(tile_count, -1);
    std::vector<bool> settled(open.size(), false);
    for (size_t idx = 0; idx < open.size(); idx++)
        for (int tile : open[idx]) part[tile] = static_cast<int>(idx);
    for (auto& [x, y] : world.cities)
        if (part[y * width + x] >= 0) settled[part[y * width + x]] = true;
    std::vector<int> wild;
    for (int i = 0; i < tile_count; i++) {
        int x = i % width, y = i / width;
        if (terrain[i] != kPlain || part[i] < 0 || !settled[part[i]]) continue;
        if (x < 3 || y < 3 || x >= width - 3 || y >= height - 3) continue;
        bool clear = true;
        for (int dy = -2; dy <= 2 && clear; dy++)
            for (int dx = -2; dx <= 2 && clear; dx++)
                if (grid.In(x + dx, y + dy) && terrain[(y + dy) * width + x + dx] == kCity)
                    clear = false;
        if (clear) wild.push_back(i);
    }
    std::vector<std::pair<int, int>> avoid;
    for (auto& [x, y] : world.cities) avoid.push_back({x, y});
    world.sites = Spread(rng, wild, settings.sites, width, avoid, 3.0, [](int) { return 0.0; }, 20);
    for (auto& [x, y] : world.sites) terrain[y * width + x] = kSite;
    return world;
}

// ------------------------------------------------------------------ roads

Roads PlanRoads(const std::vector<uint8_t>& terrain, int width, int height,
                const std::vector<std::pair<int, int>>& cities, const std::vector<Span>& spans,
                Rng& rng) {
    Grid grid{width, height};
    int tile_count = width * height;
    Roads out;
    out.road.assign(tile_count, 0);
    // the cost of building a road over each kind of ground
    auto build = [&](int tile) -> double {
        switch (terrain[tile]) {
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
            return -1;  // water, shore, bridges, cities, sites, signposts
        }
    };
    // a bridge span: from the ground before it to the ground after it
    std::vector<std::vector<std::pair<int, int>>> leap(tile_count);  // tile -> (tile beyond, span)
    for (size_t idx = 0; idx < spans.size(); idx++) {
        const auto& span = spans[idx];
        int start_x = span.ew ? span.x - 1 : span.x, start_y = span.ew ? span.y : span.y - 1;
        int end_x = span.ew ? span.x + 2 : span.x, end_y = span.ew ? span.y : span.y + 2;
        if (!grid.In(start_x, start_y) || !grid.In(end_x, end_y)) continue;
        int start_tile = start_y * width + start_x, end_tile = end_y * width + end_x;
        leap[start_tile].push_back({end_tile, static_cast<int>(idx)});
        leap[end_tile].push_back({start_tile, static_cast<int>(idx)});
    }
    // the tiles a road may leave a city from: the eight beside its footprint
    auto doors = [&](int city) {
        auto [x, y] = cities[city];
        std::vector<int> door_tiles;
        const int ring[8][2] = {{0, -1}, {1, -1}, {2, 0}, {2, 1}, {1, 2}, {0, 2}, {-1, 1}, {-1, 0}};
        for (const auto& off : ring) {
            int dx = x + off[0], dy = y + off[1];
            if (grid.In(dx, dy) && build(dy * width + dx) > 0 &&
                terrain[dy * width + dx] != kMountains)
                door_tiles.push_back(dy * width + dx);
        }
        return door_tiles;
    };
    std::vector<std::vector<int>> door(cities.size());
    std::vector<int> door_of(tile_count, -1);
    for (size_t city = 0; city < cities.size(); city++) {
        door[city] = doors(static_cast<int>(city));
        for (int tile : door[city]) door_of[tile] = static_cast<int>(city);
    }
    // A*: the cheapest road from any door of from_city to any door of to_city,
    // existing roads cheap so new ones join them; returns its cost and tiles
    auto route = [&](int from_city, int to_city, std::vector<int>* tiles,
                     std::vector<int>* used) -> double {
        if (door[from_city].empty() || door[to_city].empty()) return -1;
        auto [to_x, to_y] = cities[to_city];
        auto [from_x, from_y] = cities[from_city];
        auto guess = [&](int tile) {
            return 0.5 * (std::abs(tile % width - to_x) + std::abs(tile / width - to_y));
        };
        // a little extra the further a tile is off the straight line, so a road
        // runs as a staircase along it rather than as one long L
        double line_x = to_x - from_x, line_y = to_y - from_y,
               len = std::max(1.0, Dist(line_x, line_y));
        auto off = [&](int tile) {
            return 0.08 *
                   std::abs((tile % width - from_x) * line_y - (tile / width - from_y) * line_x) /
                   len;
        };
        std::vector<double> cost(tile_count, 1e18);
        std::vector<int> from(tile_count, -1), via(tile_count, -1);
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        for (int tile : door[from_city]) {
            cost[tile] = out.road[tile] ? 0.3 : build(tile);
            queue.push({cost[tile] + guess(tile), tile});
        }
        int end = -1;
        while (!queue.empty()) {
            auto [est, tile] = queue.top();
            queue.pop();
            if (est - guess(tile) > cost[tile] + 1e-9) continue;
            if (door_of[tile] == to_city) {
                end = tile;
                break;
            }
            int x = tile % width, y = tile / width;
            auto relax = [&](int next, double next_cost, int span) {
                if (next_cost < cost[next]) {
                    cost[next] = next_cost;
                    from[next] = tile;
                    via[next] = span;
                    queue.push({next_cost + guess(next), next});
                }
            };
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!grid.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                double step_cost = build(next);
                if (step_cost < 0) continue;
                relax(next, cost[tile] + (out.road[next] ? 0.3 : step_cost + off(next)), -1);
            }
            for (auto& [next, span] : leap[tile]) relax(next, cost[tile] + 4, span);
        }
        if (end < 0) return -1;
        if (tiles) {
            for (int tile = end; tile >= 0; tile = from[tile]) {
                tiles->push_back(tile);
                if (used && via[tile] >= 0) used->push_back(via[tile]);
            }
        }
        return cost[end];
    };
    // the links worth having: each city's nearest few, by travel cost
    struct Link {
        double cost;  ///< Route construction cost.
        int a;        ///< First city index.
        int b;        ///< Second city index.
    };
    std::vector<Link> links;
    int city_count = static_cast<int>(cities.size());
    for (int city_a = 0; city_a < city_count; city_a++) {
        std::vector<std::pair<double, int>> near;
        for (int city_b = 0; city_b < city_count; city_b++)
            if (city_b != city_a)
                near.push_back({Dist(cities[city_a].first - cities[city_b].first,
                                     cities[city_a].second - cities[city_b].second),
                                city_b});
        std::sort(near.begin(), near.end());
        for (int idx = 0; idx < 4 && idx < static_cast<int>(near.size()); idx++) {
            int city_b = near[idx].second;
            if (near[idx].first > 26 || city_b < city_a) continue;
            double road_cost = route(city_a, city_b, nullptr, nullptr);
            if (road_cost > 0) links.push_back({road_cost, city_a, city_b});
        }
    }
    auto cheaper = [](const Link& lhs, const Link& rhs) {
        if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
        return lhs.a != rhs.a ? lhs.a < rhs.a : lhs.b < rhs.b;
    };
    std::sort(links.begin(), links.end(), cheaper);
    // the minimum spanning tree (Kruskal), then a few links where the tree
    // would send a traveller the long way round
    std::vector<int> root(city_count);
    std::iota(root.begin(), root.end(), 0);
    std::function<int(int)> find = [&](int city) {
        return root[city] == city ? city : root[city] = find(root[city]);
    };
    std::vector<std::vector<std::pair<int, double>>> tree(city_count);
    std::vector<Link> chosen;
    for (auto& link : links) {
        int root_a = find(link.a), root_b = find(link.b);
        if (root_a == root_b || link.cost > 34) continue;
        root[root_a] = root_b;
        chosen.push_back(link);
        tree[link.a].push_back({link.b, link.cost});
        tree[link.b].push_back({link.a, link.cost});
    }
    auto tree_cost = [&](int from_city, int to_city) {
        std::vector<double> dist(city_count, -1);
        std::deque<int> queue{from_city};
        dist[from_city] = 0;
        while (!queue.empty()) {
            int cur = queue.front();
            queue.pop_front();
            for (auto& [next, edge_cost] : tree[cur])
                if (dist[next] < 0) {
                    dist[next] = dist[cur] + edge_cost;
                    queue.push_back(next);
                }
        }
        return dist[to_city];
    };
    for (auto& link : links) {
        double detour = tree_cost(link.a, link.b);
        if (detour > 2.2 * link.cost && link.cost <= 24 && Uniform(rng) < 0.7) {
            chosen.push_back(link);
            tree[link.a].push_back({link.b, link.cost});
            tree[link.b].push_back({link.a, link.cost});
        }
    }
    // lay them, cheapest first, each joining what is there already
    std::sort(chosen.begin(), chosen.end(), cheaper);
    std::vector<uint8_t> bridged(spans.size(), 0);
    for (auto& link : chosen) {
        std::vector<int> tiles, used;
        if (route(link.a, link.b, &tiles, &used) < 0) continue;
        for (int tile : tiles) out.road[tile] = 1;
        for (int span_idx : used) bridged[span_idx] = 1;
    }
    for (size_t idx = 0; idx < spans.size(); idx++)
        if (bridged[idx]) out.bridges.push_back(static_cast<int>(idx));
    return out;
}

}  // namespace w2::scengen
