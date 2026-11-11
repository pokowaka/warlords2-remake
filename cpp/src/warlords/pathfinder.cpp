/**
 * @file
 * @brief `Pathfinder` cost-grid cache, wavefront search, route trace, and road pathfinding.
 */
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

constexpr int kUnreached = 30000;
constexpr int kMaxBitsetWidth = 128;
constexpr int kWordsPerCol = 4;  // 4 * 64 = 256 max height for inline bitset

constexpr int8_t kNeighbourListStart[10] = {0, 9, 13, 19, 23, 29, 35, 39, 45, 49};
constexpr int8_t kNeighbourDx[] = {-1,  0,  1,   1,  1,   0,  -1,  -1, -10, 1, 1,   0,  -10, 1,
                                   1,   0,  -1,  -1, -10, 0,  -1,  -1, -10, 0, 1,   1,  1,   0,
                                   -10, -1, 0,   0,  -1,  -1, -10, 0,  1,   1, -10, -1, 0,   1,
                                   1,   -1, -10, -1, 0,   -1, -10, 0,  1,   0, -1,  -10};
constexpr int8_t kNeighbourDy[] = {-1,  -1, -1,  0,  1,   1, 1,   0,  -10, 0,  1,   1,  -10, 0,
                                   1,   1,  1,   0,  -10, 1, 1,   0,  -10, -1, -1,  0,  1,   1,
                                   -10, -1, -1,  1,  1,   0, -10, -1, -1,  0,  -10, -1, -1,  -1,
                                   0,   0,  -10, -1, -1,  0, -10, -1, 0,   1,  0,   -10};

const int kTraceOrder[8] = {0, 7, 1, 6, 2, 5, 3, 4};

using ColRowBitset = std::array<std::array<uint64_t, kWordsPerCol>, kMaxBitsetWidth>;

struct Query {
    int w;
    int h;
    int sx;
    int sy;
    int dx;
    int dy;
    std::vector<int>* grid;
    int mode;
    bool woods;
    bool hills;
    int penalty;
    std::vector<int>& dist;
    int ring = 0;
    bool roads = false;
    std::function<bool(int, int)> is_bridge;
    int flood = kNone;
    ColRowBitset active_rows{};
    int min_active_x = kMaxBitsetWidth;
    int max_active_x = -1;
    bool bitset_ready = false;
};

inline void SettleTile(Query& query, int x, int y, bool use_bitset, bool& spread_any, bool& going) {
    const int width = query.w;
    const int height = query.h;
    const int start_x = query.sx;
    const int start_y = query.sy;
    const int mode = query.mode;
    const bool hills = query.hills;
    const bool woods = query.woods;
    const int penalty = query.penalty;
    int* __restrict dist_ptr = query.dist.data();
    const int* __restrict grid_ptr = query.grid->data();

    int tile = y * width + x;
    int tile_dist = dist_ptr[tile];
    if (tile_dist >= 1) return;

    spread_any = true;
    int cur = grid_ptr[tile];
    int settled = -tile_dist;
    dist_ptr[tile] = settled;
    if (use_bitset && settled >= 1) {
        query.active_rows[static_cast<size_t>(x)][static_cast<size_t>(y >> 6)] &=
                ~(uint64_t{1} << (y & 63));
    }
    bool cur_water = move::Has(cur, move::kWaterF);
    bool cur_cross = move::Has(cur, move::kCrossF);
    int edge_class = query.roads ? 9 : 0;
    if (y == 0)
        edge_class = x == 0 ? 1 : (x == width - 1 ? 3 : 2);
    else if (y == height - 1)
        edge_class = x == 0 ? 6 : (x == width - 1 ? 8 : 7);
    else if (x == 0)
        edge_class = 4;
    else if (x == width - 1)
        edge_class = 5;
    int neighbour = kNeighbourListStart[edge_class];
    while (kNeighbourDx[neighbour] != -10) {
        int next_x = x + kNeighbourDx[neighbour];
        int next_y = y + kNeighbourDy[neighbour];
        neighbour++;
        int next_tile = next_y * width + next_x;
        int next_dist = dist_ptr[next_tile];
        if (next_dist == move::kShut) continue;
        int abs_next = next_dist < 0 ? -next_dist : next_dist;
        if (mode != move::kBoat && settled + 1 >= abs_next) continue;

        int next_byte = grid_ptr[next_tile];
        int step_cost = next_byte & 7;
        bool can_spread = true;
        bool next_water = move::Has(next_byte, move::kWaterF);
        bool next_cross = move::Has(next_byte, move::kCrossF);
        if (next_x == start_x && next_y == start_y) {
            if (mode == move::kLand && !cur_cross && !next_cross && next_water != cur_water)
                can_spread = false;
            step_cost = 1;
        } else if (mode == move::kFlying) {
            if (!((!next_water || next_cross) && step_cost != 0 && step_cost < 3)) {
                step_cost = 2;
            }
        } else if (mode == move::kBoat) {
            if (!next_water && !move::Has(next_byte, move::kCityF)) {
                dist_ptr[next_tile] = move::kShut;
                if (use_bitset) {
                    query.active_rows[static_cast<size_t>(next_x)]
                                     [static_cast<size_t>(next_y >> 6)] &=
                            ~(uint64_t{1} << (next_y & 63));
                }
                can_spread = false;
            }
        } else {
            if (!cur_cross && !next_cross && next_water != cur_water) {
                can_spread = false;
            } else {
                if (step_cost > 2 &&
                    ((hills && move::Has(next_byte, move::kHillsF)) ||
                     (woods && move::Has(next_byte, move::kForestF))))
                    step_cost = 2;
                if ((!cur_water || cur_cross) && next_water && !next_cross)
                    step_cost += penalty;
            }
        }
        if (can_spread && settled + step_cost < abs_next) {
            int updated = tile_dist - step_cost;
            dist_ptr[next_tile] = updated;
            if (use_bitset && updated < 1) {
                query.active_rows[static_cast<size_t>(next_x)]
                                 [static_cast<size_t>(next_y >> 6)] |=
                        (uint64_t{1} << (next_y & 63));
                if (next_x < query.min_active_x) query.min_active_x = next_x;
                if (next_x > query.max_active_x) query.max_active_x = next_x;
            }
        }
    }
    if (x == start_x && y == start_y && query.flood == kNone) going = false;
}

int Wavefront(Query& query, int pass) {
    int width = query.w, height = query.h;
    int start_x = query.sx, start_y = query.sy, dest_x = query.dx, dest_y = query.dy;
    const bool use_bitset = (width > 0 && width <= kMaxBitsetWidth && height > 0 &&
                             height <= kWordsPerCol * 64);
    if (use_bitset && !query.bitset_ready) {
        if (dest_x >= 0 && dest_x < width && dest_y >= 0 && dest_y < height &&
            query.dist[dest_y * width + dest_x] < 1) {
            query.active_rows[static_cast<size_t>(dest_x)][static_cast<size_t>(dest_y >> 6)] |=
                    (uint64_t{1} << (dest_y & 63));
            query.min_active_x = dest_x;
            query.max_active_x = dest_x;
        }
        query.bitset_ready = true;
    }

    int margin = query.flood != kNone ? query.flood : (pass == 0 ? 6 : 50);
    if (pass == 0 || query.flood != kNone) query.ring = 0;
    int low_x = std::min(start_x, dest_x), low_y = std::min(start_y, dest_y);
    int high_x = std::max(start_x, dest_x), high_y = std::max(start_y, dest_y);
    int bound_left = low_x < margin ? 0 : low_x - margin;
    int bound_top = low_y < margin ? 0 : low_y - margin;
    int bound_right = high_x + margin < width ? high_x + margin : width - 1;
    int bound_bottom = high_y + margin < height ? high_y + margin : height - 1;
    int found = 1;
    bool going = true;
    while (going) {
        if (query.flood != kNone && query.ring == query.flood) break;
        bool spread_any = false;
        int ring = query.ring;
        int sweep_left = std::max({dest_x - ring, 0, bound_left}),
            sweep_right = std::min({dest_x + ring, width - 1, bound_right});
        int sweep_top = std::max({dest_y - ring, 0, bound_top}),
            sweep_bottom = std::min({dest_y + ring, height - 1, bound_bottom});
        if (use_bitset && sweep_top <= sweep_bottom) {
            sweep_left = std::max(sweep_left, query.min_active_x);
            const int first_word = sweep_top >> 6;
            const int last_word = sweep_bottom >> 6;
            const uint64_t first_mask = ~uint64_t{0} << (sweep_top & 63);
            const int high_bit = sweep_bottom & 63;
            const uint64_t last_mask =
                    (high_bit == 63) ? ~uint64_t{0} : ((uint64_t{1} << (high_bit + 1)) - 1U);
            // Note: query.max_active_x may grow to x + 1 as column x expands rightward, so test
            // x <= std::min(sweep_right, query.max_active_x) dynamically!
            for (int x = sweep_left; x <= std::min(sweep_right, query.max_active_x); x++) {
                auto& col_words = query.active_rows[static_cast<size_t>(x)];
                for (int word_idx = first_word; word_idx <= last_word; ++word_idx) {
                    uint64_t valid_mask = ~uint64_t{0};
                    if (word_idx == first_word) valid_mask &= first_mask;
                    if (word_idx == last_word) valid_mask &= last_mask;
                    uint64_t cursor_mask = valid_mask;
                    while (true) {
                        uint64_t word = col_words[static_cast<size_t>(word_idx)] & cursor_mask;
                        if (word == 0) break;
                        int bit = std::countr_zero(word);
                        int y = (word_idx << 6) + bit;
                        cursor_mask = (bit == 63) ? uint64_t{0}
                                                  : (valid_mask & (~uint64_t{0} << (bit + 1)));
                        SettleTile(query, x, y, true, spread_any, going);
                    }
                }
            }
        } else if (!use_bitset) {
            for (int x = sweep_left; x <= sweep_right; x++) {
                for (int y = sweep_top; y <= sweep_bottom; y++) {
                    if (query.dist[y * width + x] < 1) {
                        SettleTile(query, x, y, false, spread_any, going);
                    }
                }
            }
        }
        query.ring = ring + 1;
        if (!spread_any) {
            going = false;
            found = 0;
        }
    }
    return found;
}

move::Path Trace(Query& query) {
    int width = query.w, height = query.h;
    const std::vector<int>& dist = query.dist;
    const std::vector<int>& grid = *query.grid;
    int x = query.sx, y = query.sy;
    int target_x = query.dx, target_y = query.dy;
    int best_dist = std::abs(dist[y * width + x]);
    move::Path steps;
    while (!(x == target_x && y == target_y)) {
        int cur = grid[y * width + x];
        bool land = query.mode == move::kLand;
        bool cur_water = land && move::Has(cur, move::kWaterF);
        bool cur_cross = land && move::Has(cur, move::kCrossF);
        int toward = move::Direction(x, y, target_x, target_y);
        int best_x = kNone, best_y = kNone;
        bool got = false;
        for (int turn : kTraceOrder) {
            int dir = (toward + turn) % 8;
            int next_x = x + move::kDirs[dir][0], next_y = y + move::kDirs[dir][1];
            bool inside = next_x >= 0 && next_y >= 0 && next_x < width && next_y < height;
            if (query.roads && dir % 2 == 1 &&
                !(inside && move::Has(grid[next_y * width + next_x], move::kWaterF) &&
                  !query.is_bridge(next_x, next_y)))
                continue;
            if (inside) {
                int next_dist = dist[next_y * width + next_x];
                if (next_dist != kUnreached && next_dist != move::kShut) {
                    int next_byte = grid[next_y * width + next_x];
                    bool blocked = land && !cur_cross && !move::Has(next_byte, move::kCrossF) &&
                                   move::Has(next_byte, move::kWaterF) != cur_water;
                    bool tie = query.roads && !got && std::abs(next_dist) == best_dist;
                    if (!blocked && (std::abs(next_dist) < best_dist || tie)) {
                        best_x = next_x;
                        best_y = next_y;
                        got = true;
                        best_dist = std::abs(next_dist);
                    }
                }
            }
        }
        if (got && steps.size() < 198) {
            x = best_x;
            y = best_y;
            steps.push_back(move::Step{x, y, 0});
        } else {
            target_x = x;
            target_y = y;
        }
    }
    return steps;
}

bool SingleStep(const Game& game, int mode, int start_x, int start_y, int dest_x, int dest_y) {
    auto wet = [](int terrain) { return terrain == move::kWater || terrain == move::kShore; };
    const Map& map = game.GetMap();
    int from_terrain = scn::TerrainAt(map, start_x, start_y);
    int to_terrain = scn::TerrainAt(map, dest_x, dest_y);
    if (mode != move::kFlying) {
        if (wet(from_terrain) != wet(to_terrain)) return false;
        if (move::kCost[to_terrain] == 0) return false;
    }
    return true;
}

std::vector<std::pair<int, int>> OpenCityAt(Game& game, std::vector<int>& grid, int dest_x,
                                            int dest_y) {
    Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    std::vector<std::pair<int, int>> restore;
    int goal = dest_y * width + dest_x;
    int goal_byte = grid[goal];
    City* city = map.CityAtFootprint(dest_x, dest_y);
    if (city && goal_byte % 8 == 0 && move::Has(goal_byte, move::kCityF)) {
        bool port = move::IsPort(game, *city);
        for (int ox = 0; ox <= 1; ox++) {
            for (int oy = 0; oy <= 1; oy++) {
                int x = city->X() + ox, y = city->Y() + oy;
                if (x < width && y < height) {
                    int tile = y * width + x;
                    restore.emplace_back(tile, grid[tile]);
                    int byte = grid[tile] - grid[tile] % 8 + move::kCost[move::kCity];
                    if (port) byte |= move::kCrossF | move::kWaterF;
                    grid[tile] = byte;
                }
            }
        }
    }
    return restore;
}

int RouteFlags(int mode, bool woods, bool hills, bool at_sea) {
    if (at_sea) return 8;
    if (mode != move::kLand) return 0;
    return (hills ? 1 : 0) | (woods ? 2 : 0);
}

bool StepDir(int& x, int& y, int dir, int width, int height) {
    int next_x = x + move::kDirs[dir][0], next_y = y + move::kDirs[dir][1];
    if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height) return false;
    x = next_x;
    y = next_y;
    return true;
}

int DirOf(int dx, int dy) {
    for (int dir = 0; dir < 8; dir++) {
        if (move::kDirs[dir][0] == dx && move::kDirs[dir][1] == dy) return dir;
    }
    return 0;
}

std::optional<std::vector<int>> CacheHit(const Game& game,
                                         const std::optional<move::CachedRoute>& entry, int start_x,
                                         int start_y, int dest_x, int dest_y, int mode, int flags) {
    if (!entry || entry->dx != dest_x || entry->dy != dest_y || entry->mode != mode ||
        entry->flags != flags)
        return std::nullopt;
    const Map& map = game.GetMap();
    int x = entry->sx, y = entry->sy;
    for (size_t i = 0; i < entry->dirs.size(); i++) {
        if (x == start_x && y == start_y)
            return std::vector<int>(entry->dirs.begin() + i, entry->dirs.end());
        if (!StepDir(x, y, entry->dirs[i], map.Width(), map.Height())) return std::nullopt;
    }
    return std::nullopt;
}

void PrepareInto(const Game& game, const std::vector<int>& grid, int side_index, int mode,
                 int dest_x, int dest_y, std::vector<int>& dist) {
    const Map& map = game.GetMap();
    const int width = map.Width();
    const int height = map.Height();
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
    dist.resize(total);

    const Side* side = map.SideAt(side_index);
    const bool fog = map.GameOptions().hidden_map != 0 && side &&
                     (!side->IsComputerControlled() || !side->IsClassicAi());
    const int* grid_ptr = grid.data();
    int* dist_ptr = dist.data();
    if (!fog) {
        if (mode == move::kLand) {
            for (size_t tile = 0; tile < total; ++tile) {
                dist_ptr[tile] = (grid_ptr[tile] % 8 == 0) ? move::kShut : kUnreached;
            }
        } else if (mode == move::kBoat) {
            for (size_t tile = 0; tile < total; ++tile) {
                dist_ptr[tile] = !move::Has(grid_ptr[tile], move::kWaterF) ? move::kShut
                                                                           : kUnreached;
            }
        } else {
            for (size_t tile = 0; tile < total; ++tile) {
                int byte = grid_ptr[tile];
                dist_ptr[tile] = (byte % 8 == 0 && move::Has(byte, move::kCityF)) ? move::kShut
                                                                                  : kUnreached;
            }
        }
        return;
    }

    const game::SeenMasks seen = game::BuildSeenMasks(game, side_index);
    const City* dest_city = map.CityAtFootprint(dest_x, dest_y);
    bool dest_city_seen = false;
    if (dest_city && side->IsComputerControlled() && !side->IsClassicAi()) {
        for (int oy = 0; oy <= 1; oy++) {
            for (int ox = 0; ox <= 1; ox++) {
                if (game::IsSeenAt(seen, dest_city->X() + ox, dest_city->Y() + oy)) {
                    dest_city_seen = true;
                }
            }
        }
    }

    if (seen.count == 1 && !seen.all_visible) {
        const uint8_t* mask0 = seen.masks[0];
        if (mode == move::kLand) {
            for (int y = 0; y < height; y++) {
                size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = 0; x < width; x++) {
                    size_t tile = row + static_cast<size_t>(x);
                    bool hidden = (mask0[tile] == 0);
                    bool shut = (grid_ptr[tile] % 8 == 0) ||
                                (hidden && !(x == dest_x && y == dest_y));
                    dist_ptr[tile] = shut ? move::kShut : kUnreached;
                }
            }
        } else if (mode == move::kBoat) {
            for (int y = 0; y < height; y++) {
                size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = 0; x < width; x++) {
                    size_t tile = row + static_cast<size_t>(x);
                    bool hidden = (mask0[tile] == 0);
                    bool shut = !move::Has(grid_ptr[tile], move::kWaterF) ||
                                (hidden && x != dest_x && y != dest_y);
                    dist_ptr[tile] = shut ? move::kShut : kUnreached;
                }
            }
        } else {
            for (int y = 0; y < height; y++) {
                size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = 0; x < width; x++) {
                    size_t tile = row + static_cast<size_t>(x);
                    int byte = grid_ptr[tile];
                    bool hidden = (mask0[tile] == 0);
                    bool shut = (byte % 8 == 0 && move::Has(byte, move::kCityF)) ||
                                (hidden && x != dest_x && y != dest_y);
                    dist_ptr[tile] = shut ? move::kShut : kUnreached;
                }
            }
        }
    } else {
        for (int y = 0; y < height; y++) {
            size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
            for (int x = 0; x < width; x++) {
                size_t tile = row + static_cast<size_t>(x);
                int byte = grid_ptr[tile];
                bool hidden = !game::IsSeenIndex(seen, tile);
                bool shut;
                if (mode == move::kLand) {
                    shut = byte % 8 == 0 || (hidden && !(x == dest_x && y == dest_y));
                } else if (mode == move::kBoat) {
                    shut = !move::Has(byte, move::kWaterF) ||
                           (hidden && x != dest_x && y != dest_y);
                } else {
                    shut = (byte % 8 == 0 && move::Has(byte, move::kCityF)) ||
                           (hidden && x != dest_x && y != dest_y);
                }
                dist_ptr[tile] = shut ? move::kShut : kUnreached;
            }
        }
    }

    if (dest_city_seen) {
        for (int oy = 0; oy <= 1; oy++) {
            for (int ox = 0; ox <= 1; ox++) {
                int x = dest_city->X() + ox;
                int y = dest_city->Y() + oy;
                if (map.InBounds(x, y) && map.CityAtFootprint(x, y) == dest_city) {
                    size_t tile = static_cast<size_t>(y) * static_cast<size_t>(width) +
                                  static_cast<size_t>(x);
                    int byte = grid_ptr[tile];
                    bool shut;
                    if (mode == move::kLand) {
                        shut = byte % 8 == 0;
                    } else if (mode == move::kBoat) {
                        shut = !move::Has(byte, move::kWaterF);
                    } else {
                        shut = (byte % 8 == 0 && move::Has(byte, move::kCityF));
                    }
                    dist_ptr[tile] = shut ? move::kShut : kUnreached;
                }
            }
        }
    }
}

void ShutVisibleHostileStacks(const Game& game, int side, int start_x, int start_y, int dest_x,
                              int dest_y, std::vector<int>& dist) {
    if (side == kNone) return;
    const Map& map = game.GetMap();
    int width = map.Width();
    const game::SeenMasks seen = game::BuildSeenMasks(game, side);
    for (const Army* army : game.Armies()) {
        if (army->InTransit() || !map.InBounds(army->X(), army->Y())) continue;
        if (army->OwnedBy(side)) continue;
        if ((army->X() == start_x && army->Y() == start_y) ||
            (army->X() == dest_x && army->Y() == dest_y))
            continue;
        const City* city_tile = map.CityAtFootprint(army->X(), army->Y());
        if (city_tile && !city_tile->IsRazed()) continue;
        if (!game::IsSeenAt(seen, army->X(), army->Y())) continue;
        if (diplomacy::MayAttack(game, side, army->OwnerIndex())) {
            dist[static_cast<size_t>(army->Y()) * static_cast<size_t>(width) +
                 static_cast<size_t>(army->X())] = move::kShut;
        }
    }
}

}  // namespace

void Pathfinder::Invalidate() {
    grids_.clear();
    flood_cache_.clear();
}

void Pathfinder::InvalidateSide(SideId side_id) {
    grids_.erase(side_id.Value());
    flood_cache_.clear();
}

void Pathfinder::ClearRouteCache() {
    for (auto& slot : route_slots_) slot.reset();
    flood_cache_.clear();
}

std::vector<int>& Pathfinder::GridFor(Game& game, int side_index) {
    auto cached = grids_.find(side_index);
    if (cached != grids_.end()) return cached->second;

    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    std::vector<int> grid(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int tile = y * width + x;
            int terrain = scn::TerrainAt(map, x, y);
            int byte = scn::RoadAt(map, x, y) % 0x20 != 0 ? 1 : move::kCost[terrain];
            if (terrain == move::kBridge)
                byte |= move::kCrossF | move::kWaterF;
            else if (terrain == move::kWater || terrain == move::kShore)
                byte |= move::kWaterF;
            else if (terrain == move::kForest)
                byte |= move::kForestF;
            else if (terrain == move::kHills)
                byte |= move::kHillsF;
            if (map.IsCrossing(x, y)) byte |= move::kCrossF;
            grid[tile] = byte;
        }
    }
    for (const City& city : map.Cities()) {
        bool mine = side_index != kNone && city.OwnedBy(side_index) && !city.IsRazed();
        bool port = (mine || city.IsRazed()) && move::IsPort(game, city);
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int x = city.X() + dx, y = city.Y() + dy;
                if (x < width && y < height) {
                    int tile = y * width + x;
                    int byte = grid[tile];
                    if (city.IsRazed()) {
                        if (port) byte |= move::kCrossF | move::kWaterF;
                    } else {
                        byte |= move::kCityF;
                        if (mine) {
                            if (port) byte |= move::kCrossF | move::kWaterF;
                        } else {
                            byte = byte - (byte % 8);
                        }
                    }
                    grid[tile] = byte;
                }
            }
        }
    }
    auto [inserted, was_new] = grids_.insert_or_assign(side_index, std::move(grid));
    (void)was_new;
    return inserted->second;
}

Pathfinder::OptionalDirList Pathfinder::FindMatchingCachedDirs(const Game& game, int start_x,
                                                               int start_y, int dest_x, int dest_y,
                                                               int mode, int flags,
                                                               int distance) const {
    if (auto dirs = CacheHit(game, route_slots_[0], start_x, start_y, dest_x, dest_y, mode, flags))
        return dirs;
    for (int i = 1; distance > 14 && i < move::kCacheSlots; i++) {
        if (auto dirs =
                    CacheHit(game, route_slots_[i], start_x, start_y, dest_x, dest_y, mode, flags))
            return dirs;
    }
    return std::nullopt;
}

void Pathfinder::StoreCachedRoute(int distance, int start_x, int start_y, int dest_x, int dest_y,
                                  int mode, int flags, const DirList& dirs) {
    int slot = -1;
    if (distance < 15) {
        slot = 0;
    } else {
        int shortest = -1, least = 10000;
        for (int i = 1; i < move::kCacheSlots; i++) {
            const auto& entry = route_slots_[i];
            if (!entry) {
                slot = i;
                break;
            }
            int entry_distance = move::Distance(entry->dx, entry->dy, entry->sx, entry->sy);
            if (entry_distance < least) {
                shortest = i;
                least = entry_distance;
            }
        }
        if (slot == -1 && shortest != -1 && least < distance) slot = shortest;
    }
    if (slot != -1) {
        route_slots_[slot] = move::CachedRoute{mode, flags, start_x, start_y, dest_x, dest_y, dirs};
    }
}

std::optional<move::Path> Pathfinder::FindPath(Game& game, const move::Stack& stack, int start_x,
                                               int start_y, int dest_x, int dest_y) {
    if (start_x == dest_x && start_y == dest_y) return move::Path{};
    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    if (!map.InBounds(dest_x, dest_y)) return std::nullopt;

    int side = stack.empty() ? kNone : stack[0]->OwnerIndex();
    move::Mode movement = move::StackMode(game, stack);
    std::vector<int>& grid = GridFor(game, side);
    int flags = RouteFlags(movement.mode, movement.woods, movement.hills, movement.at_sea);
    int distance = move::Distance(dest_x, dest_y, start_x, start_y);

    std::optional<move::Path> path;
    OptionalDirList dirs = FindMatchingCachedDirs(game, start_x, start_y, dest_x, dest_y,
                                                  movement.mode, flags, distance);
    if (dirs) {
        path = move::Path{};
        int x = start_x, y = start_y;
        for (int dir : *dirs) {
            StepDir(x, y, dir, width, height);
            path->push_back(move::Step{x, y, 0});
        }
    } else if (distance == 1 && SingleStep(game, movement.mode, start_x, start_y, dest_x, dest_y)) {
        path = move::Path{move::Step{dest_x, dest_y, 0}};
    } else {
        int goal = dest_y * width + dest_x;
        auto restore = OpenCityAt(game, grid, dest_x, dest_y);
        int penalty = 0;
        if (movement.mode == move::kLand) {
            int dest_terrain = scn::TerrainAt(map, dest_x, dest_y);
            penalty = (dest_terrain == move::kWater || dest_terrain == move::kShore)
                              ? move::kWaterPenaltyTo
                              : move::kWaterPenalty;
        }
        PrepareInto(game, grid, side, movement.mode, dest_x, dest_y, scratch_dist_);
        Query query{width,
                    height,
                    start_x,
                    start_y,
                    dest_x,
                    dest_y,
                    &grid,
                    movement.mode,
                    movement.woods,
                    movement.hills,
                    penalty,
                    scratch_dist_,
                    0,
                    false,
                    nullptr};
        int found = 0;
        if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
            query.dist[goal] = -1;
            for (int pass = 0; pass <= 1; pass++) {
                if (found == 0) found = Wavefront(query, pass);
            }
        }
        if (found == 1) {
            path = Trace(query);
            std::vector<int> route_dirs;
            int prev_x = start_x, prev_y = start_y;
            for (const move::Step& step : *path) {
                route_dirs.push_back(DirOf(step.x - prev_x, step.y - prev_y));
                prev_x = step.x;
                prev_y = step.y;
            }
            StoreCachedRoute(distance, start_x, start_y, dest_x, dest_y, movement.mode, flags,
                             route_dirs);
        }
        for (auto& [tile, byte] : restore) grid[tile] = byte;
    }

    if (path) {
        bool ashore = false;
        for (move::Step& step : *path) {
            int byte = grid[step.y * width + step.x];
            if (ashore) {
                step.cost = move::kPastShore;
            } else {
                step.cost = move::WalkCost(byte, movement.mode, movement.woods, movement.hills);
                ashore = movement.mode == move::kLand && move::CrossesShore(byte, movement.at_sea);
            }
        }
    }
    if (path && path->empty()) path.reset();
    return path;
}

std::optional<move::Path> Pathfinder::FindPathClear(Game& game, const move::Stack& stack,
                                                    int start_x, int start_y, int dest_x,
                                                    int dest_y) {
    if (start_x == dest_x && start_y == dest_y) return move::Path{};
    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    if (!map.InBounds(dest_x, dest_y)) return std::nullopt;

    int side = stack.empty() ? kNone : stack[0]->OwnerIndex();
    move::Mode movement = move::StackMode(game, stack);
    std::vector<int>& grid = GridFor(game, side);
    int distance = move::Distance(dest_x, dest_y, start_x, start_y);

    std::optional<move::Path> path;
    if (distance == 1 && SingleStep(game, movement.mode, start_x, start_y, dest_x, dest_y)) {
        path = move::Path{move::Step{dest_x, dest_y, 0}};
    } else {
        int goal = dest_y * width + dest_x;
        auto restore = OpenCityAt(game, grid, dest_x, dest_y);
        int penalty = 0;
        if (movement.mode == move::kLand) {
            int dest_terrain = scn::TerrainAt(map, dest_x, dest_y);
            penalty = (dest_terrain == move::kWater || dest_terrain == move::kShore)
                              ? move::kWaterPenaltyTo
                              : move::kWaterPenalty;
        }
        int bound_x0 = std::max(0, std::min(start_x, dest_x) - 6);
        int bound_x1 = std::min(width - 1, std::max(start_x, dest_x) + 6);
        int bound_y0 = std::max(0, std::min(start_y, dest_y) - 6);
        int bound_y1 = std::min(height - 1, std::max(start_y, dest_y) + 6);
        const Side* side_ptr = map.SideAt(side);
        bool fog = map.GameOptions().hidden_map != 0 && side_ptr &&
                   (!side_ptr->IsComputerControlled() || !side_ptr->IsClassicAi());
        const game::SeenMasks seen = fog ? game::BuildSeenMasks(game, side) : game::SeenMasks{};
        const City* dest_city = map.CityAtFootprint(dest_x, dest_y);
        bool dest_city_seen = false;
        if (dest_city && fog && side_ptr && side_ptr->IsComputerControlled() &&
            !side_ptr->IsClassicAi()) {
            for (int oy = 0; oy <= 1; oy++) {
                for (int ox = 0; ox <= 1; ox++) {
                    if (game::IsSeenAt(seen, dest_city->X() + ox, dest_city->Y() + oy))
                        dest_city_seen = true;
                }
            }
        }
        scratch_dist_.assign(static_cast<size_t>(width) * static_cast<size_t>(height), move::kShut);
        for (int y = bound_y0; y <= bound_y1; y++) {
            for (int x = bound_x0; x <= bound_x1; x++) {
                int tile = y * width + x;
                int byte = grid[tile];
                bool is_dest_city_tile = dest_city_seen && map.CityAtFootprint(x, y) == dest_city;
                bool hidden = fog && !is_dest_city_tile && !game::IsSeenAt(seen, x, y);
                bool shut = false;
                if (movement.mode == move::kLand) {
                    bool embarks_water = !movement.at_sea && move::Has(byte, move::kWaterF) &&
                                         !move::Has(byte, move::kCrossF) &&
                                         !(x == dest_x && y == dest_y);
                    shut = byte % 8 == 0 || embarks_water ||
                           (hidden && !(x == dest_x && y == dest_y));
                } else if (movement.mode == move::kBoat) {
                    shut = !move::Has(byte, move::kWaterF) ||
                           (hidden && x != dest_x && y != dest_y);
                } else {
                    shut = (byte % 8 == 0 && move::Has(byte, move::kCityF)) ||
                           (hidden && x != dest_x && y != dest_y);
                }
                scratch_dist_[tile] = shut ? move::kShut : kUnreached;
            }
        }
        Query query{width, height,        start_x,        start_y,        dest_x,  dest_y,
                    &grid, movement.mode, movement.woods, movement.hills, penalty, scratch_dist_,
                    0,     false,         nullptr};
        ShutVisibleHostileStacks(game, side, start_x, start_y, dest_x, dest_y, query.dist);
        int found = 0;
        if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
            query.dist[goal] = -1;
            found = Wavefront(query, 0);
        }
        if (found == 1) {
            path = Trace(query);
        }
        for (auto& [tile, byte] : restore) grid[tile] = byte;
    }

    if (path) {
        bool ashore = false;
        for (move::Step& step : *path) {
            int byte = grid[step.y * width + step.x];
            if (ashore) {
                step.cost = move::kPastShore;
            } else {
                step.cost = move::WalkCost(byte, movement.mode, movement.woods, movement.hills);
                ashore = movement.mode == move::kLand && move::CrossesShore(byte, movement.at_sea);
            }
        }
    }
    if (path && path->empty()) path.reset();
    return path;
}

std::optional<move::Path> Pathfinder::FindPathNoCache(Game& game, const move::Stack& stack,
                                                      int start_x, int start_y, int dest_x,
                                                      int dest_y) {
    if (start_x == dest_x && start_y == dest_y) return move::Path{};
    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    if (!map.InBounds(dest_x, dest_y)) return std::nullopt;

    int side = stack.empty() ? kNone : stack[0]->OwnerIndex();
    move::Mode movement = move::StackMode(game, stack);
    std::vector<int>& grid = GridFor(game, side);
    int distance = move::Distance(dest_x, dest_y, start_x, start_y);

    std::optional<move::Path> path;
    if (distance == 1 && SingleStep(game, movement.mode, start_x, start_y, dest_x, dest_y)) {
        path = move::Path{move::Step{dest_x, dest_y, 0}};
    } else {
        int goal = dest_y * width + dest_x;
        auto restore = OpenCityAt(game, grid, dest_x, dest_y);
        int penalty = 0;
        if (movement.mode == move::kLand) {
            int dest_terrain = scn::TerrainAt(map, dest_x, dest_y);
            penalty = (dest_terrain == move::kWater || dest_terrain == move::kShore)
                              ? move::kWaterPenaltyTo
                              : move::kWaterPenalty;
        }
        PrepareInto(game, grid, side, movement.mode, dest_x, dest_y, scratch_dist_);
        Query query{width,
                    height,
                    start_x,
                    start_y,
                    dest_x,
                    dest_y,
                    &grid,
                    movement.mode,
                    movement.woods,
                    movement.hills,
                    penalty,
                    scratch_dist_,
                    0,
                    false,
                    nullptr};
        int found = 0;
        if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
            query.dist[goal] = -1;
            for (int pass = 0; pass <= 1; pass++) {
                if (found == 0) found = Wavefront(query, pass);
            }
        }
        if (found == 1) {
            path = Trace(query);
        }
        for (auto& [tile, byte] : restore) grid[tile] = byte;
    }

    if (path) {
        bool ashore = false;
        for (move::Step& step : *path) {
            int byte = grid[step.y * width + step.x];
            if (ashore) {
                step.cost = move::kPastShore;
            } else {
                step.cost = move::WalkCost(byte, movement.mode, movement.woods, movement.hills);
                ashore = movement.mode == move::kLand && move::CrossesShore(byte, movement.at_sea);
            }
        }
    }
    if (path && path->empty()) path.reset();
    return path;
}

std::vector<int> Pathfinder::Flood(Game& game, int grid_side, int x, int y, int range, int mode,
                                   bool woods, bool hills, int fog_side) {
    const Map& map = game.GetMap();
    const Side* fog_side_ptr = map.SideAt(fog_side);
    const bool fog_active = map.GameOptions().hidden_map != 0 && fog_side_ptr &&
                            (!fog_side_ptr->IsComputerControlled() || !fog_side_ptr->IsClassicAi());
    uint8_t allies_mask = 0;
    if (fog_active && fog_side >= 0 && fog_side < 8) {
        const DiplomacyMatrix& diplo = game.Diplomacy();
        SideId sid(fog_side);
        for (int ally = 0; ally < 8; ++ally) {
            if (ally != fog_side && diplo.AlliedFlag(sid, SideId(ally))) {
                allies_mask |= static_cast<uint8_t>(1U << ally);
            }
        }
    }
    uint8_t key_flags = static_cast<uint8_t>(
            (woods ? 1 : 0) | (hills ? 2 : 0) |
            (fog_side_ptr && fog_side_ptr->IsComputerControlled() ? 4 : 0) |
            (fog_side_ptr && fog_side_ptr->IsClassicAi() ? 8 : 0));
    FloodCacheKey key{grid_side,
                      x,
                      y,
                      range,
                      static_cast<uint8_t>(mode),
                      key_flags,
                      allies_mask,
                      fog_side,
                      fog_active ? game.Explored().Version() : uint64_t{0}};
    if (auto cached = flood_cache_.find(key); cached != flood_cache_.end()) {
        return cached->second;
    }

    int width = map.Width(), height = map.Height();
    std::vector<int>& grid = GridFor(game, grid_side);
    int goal = y * width + x;
    auto restore = OpenCityAt(game, grid, x, y);
    int penalty = 0;
    if (mode == move::kLand) {
        int terrain = scn::TerrainAt(map, x, y);
        penalty = (terrain == move::kWater || terrain == move::kShore) ? move::kWaterPenaltyTo
                                                                       : move::kWaterPenalty;
    }
    std::vector<int> dist;
    PrepareInto(game, grid, fog_side, mode, x, y, dist);
    Query query{width, height, x,       y,
                x,     y,      &grid,   mode,
                woods, hills,  penalty, dist,
                0,     false,  nullptr, range};
    if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
        query.dist[goal] = -1;
        if (Wavefront(query, 0) == 0) Wavefront(query, 1);
    }
    for (auto& [tile, byte] : restore) grid[tile] = byte;
    if (flood_cache_.size() >= 64) flood_cache_.clear();
    flood_cache_.emplace(key, dist);
    return dist;
}

std::vector<int> Pathfinder::FloodClear(Game& game, int grid_side, int x, int y, int range,
                                        int mode, bool woods, bool hills, int fog_side) {
    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    std::vector<int>& grid = GridFor(game, grid_side);
    int goal = y * width + x;
    auto restore = OpenCityAt(game, grid, x, y);
    int penalty = 0;
    if (mode == move::kLand) {
        int terrain = scn::TerrainAt(map, x, y);
        penalty = (terrain == move::kWater || terrain == move::kShore) ? move::kWaterPenaltyTo
                                                                       : move::kWaterPenalty;
    }
    std::vector<int> dist;
    PrepareInto(game, grid, fog_side, mode, x, y, dist);
    Query query{width, height, x,       y,
                x,     y,      &grid,   mode,
                woods, hills,  penalty, dist,
                0,     false,  nullptr, range};
    ShutVisibleHostileStacks(game, grid_side, x, y, kNone, kNone, query.dist);
    if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
        query.dist[goal] = -1;
        if (Wavefront(query, 0) == 0) Wavefront(query, 1);
    }
    for (auto& [tile, byte] : restore) grid[tile] = byte;
    return dist;
}

namespace move {

std::vector<int> Prepare(Game& game, const std::vector<int>& grid, int side_index, int mode,
                         int dest_x, int dest_y) {
    std::vector<int> dist;
    PrepareInto(game, grid, side_index, mode, dest_x, dest_y, dist);
    return dist;
}

std::optional<std::vector<std::pair<int, int>>> RoadRoute(const RoadGround& ground,
                                                          const int costs[12], int width,
                                                          int height, int start_x, int start_y,
                                                          int dest_x, int dest_y) {
    if (dest_x < 0 || dest_y < 0 || dest_x >= width || dest_y >= height) return std::nullopt;
    std::vector<int> grid(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int terrain = ground.terrain(x, y);
            int byte = ground.road(x, y) ? 1 : costs[terrain];
            if (terrain == kBridge)
                byte |= kCrossF | kWaterF;
            else if (terrain == kWater || terrain == kShore)
                byte |= kWaterF;
            else if (terrain == kForest)
                byte |= kForestF;
            else if (terrain == kHills)
                byte |= kHillsF;
            else if (terrain == kCity)
                byte = kCityF;
            if (ground.crossing(x, y)) byte |= kCrossF;
            grid[y * width + x] = byte;
        }
    }
    int dest_terrain = ground.terrain(dest_x, dest_y);
    std::vector<int> dist(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int tile = 0; tile < width * height; tile++)
        dist[tile] = grid[tile] % 8 == 0 ? kShut : kUnreached;
    int penalty =
            dest_terrain == kWater || dest_terrain == kShore ? kWaterPenaltyTo : kWaterPenalty;
    Query query{width,
                height,
                start_x,
                start_y,
                dest_x,
                dest_y,
                &grid,
                kLand,
                false,
                false,
                penalty,
                dist,
                0,
                true,
                [&ground](int x, int y) { return ground.terrain(x, y) == kBridge; }};
    int goal = dest_y * width + dest_x;
    if (query.dist[goal] == kShut && !Has(grid[goal], kCityF)) return std::nullopt;
    query.dist[goal] = -1;
    if (Wavefront(query, 0) != 1) return std::nullopt;
    std::vector<std::pair<int, int>> route;
    for (const Step& step : Trace(query)) route.emplace_back(step.x, step.y);
    return route;
}

}  // namespace move
}  // namespace w2
