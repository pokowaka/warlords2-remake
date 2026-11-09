/**
 * @file
 * @brief `Pathfinder` cost-grid cache, wavefront search, route trace, and road pathfinding.
 */
#include <algorithm>
#include <cmath>
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

const int kNeighbourListStart[10] = {0, 9, 13, 19, 23, 29, 35, 39, 45, 49};
const int kNeighbourDx[] = {-1,  0,  1,   1,  1,   0,  -1,  -1, -10, 1, 1,   0,  -10, 1,
                            1,   0,  -1,  -1, -10, 0,  -1,  -1, -10, 0, 1,   1,  1,   0,
                            -10, -1, 0,   0,  -1,  -1, -10, 0,  1,   1, -10, -1, 0,   1,
                            1,   -1, -10, -1, 0,   -1, -10, 0,  1,   0, -1,  -10};
const int kNeighbourDy[] = {-1,  -1, -1,  0,  1,   1, 1,   0,  -10, 0,  1,   1,  -10, 0,
                            1,   1,  1,   0,  -10, 1, 1,   0,  -10, -1, -1,  0,  1,   1,
                            -10, -1, -1,  1,  1,   0, -10, -1, -1,  0,  -10, -1, -1,  -1,
                            0,   0,  -10, -1, -1,  0, -10, -1, 0,   1,  0,   -10};

const int kTraceOrder[8] = {0, 7, 1, 6, 2, 5, 3, 4};

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
    std::vector<int> dist;
    int ring = 0;
    bool roads = false;
    std::function<bool(int, int)> is_bridge;
    int flood = kNone;
};

int Wavefront(Query& query, int pass) {
    int width = query.w, height = query.h;
    int start_x = query.sx, start_y = query.sy, dest_x = query.dx, dest_y = query.dy;
    std::vector<int>& dist = query.dist;
    const std::vector<int>& grid = *query.grid;
    int mode = query.mode;
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
        for (int x = sweep_left; x <= sweep_right; x++) {
            for (int y = sweep_top; y <= sweep_bottom; y++) {
                int tile = y * width + x;
                int tile_dist = dist[tile];
                if (tile_dist < 1) {
                    spread_any = true;
                    int cur = grid[tile];
                    int settled = -tile_dist;
                    dist[tile] = settled;
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
                        int next_dist = dist[next_tile];
                        if (next_dist != move::kShut) {
                            int next_byte = grid[next_tile];
                            int step_cost = next_byte % 8;
                            bool can_spread = true;
                            bool next_water = move::Has(next_byte, move::kWaterF);
                            bool next_cross = move::Has(next_byte, move::kCrossF);
                            if (next_x == start_x && next_y == start_y) {
                                if (mode == move::kLand && !cur_cross && !next_cross &&
                                    next_water != cur_water)
                                    can_spread = false;
                                step_cost = 1;
                            } else if (mode == move::kFlying) {
                                if (!((!next_water || next_cross) && step_cost != 0 &&
                                      step_cost < 3)) {
                                    step_cost = 2;
                                }
                            } else if (mode == move::kBoat) {
                                if (!next_water && !move::Has(next_byte, move::kCityF)) {
                                    dist[next_tile] = move::kShut;
                                    can_spread = false;
                                }
                            } else {
                                if (!cur_cross && !next_cross && next_water != cur_water) {
                                    can_spread = false;
                                } else {
                                    if (step_cost > 2 &&
                                        ((query.hills && move::Has(next_byte, move::kHillsF)) ||
                                         (query.woods && move::Has(next_byte, move::kForestF))))
                                        step_cost = 2;
                                    if ((!cur_water || cur_cross) && next_water && !next_cross)
                                        step_cost += query.penalty;
                                }
                            }
                            if (can_spread && settled + step_cost < std::abs(next_dist))
                                dist[next_tile] = tile_dist - step_cost;
                        }
                    }
                    if (x == start_x && y == start_y && query.flood == kNone) going = false;
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

void ShutVisibleHostileStacks(const Game& game, int side, int start_x, int start_y, int dest_x,
                              int dest_y, std::vector<int>& dist) {
    if (side == kNone) return;
    const Map& map = game.GetMap();
    int width = map.Width();
    for (const Army* army : game.Armies()) {
        if (army->InTransit() || !map.InBounds(army->X(), army->Y())) continue;
        if (army->OwnedBy(side)) continue;
        if ((army->X() == start_x && army->Y() == start_y) ||
            (army->X() == dest_x && army->Y() == dest_y))
            continue;
        const City* city_tile = map.CityAtFootprint(army->X(), army->Y());
        if (city_tile && !city_tile->IsRazed()) continue;
        if (!game::Seen(game, side, army->X(), army->Y())) continue;
        if (diplomacy::MayAttack(game, side, army->OwnerIndex())) {
            dist[static_cast<size_t>(army->Y()) * width + army->X()] = move::kShut;
        }
    }
}

}  // namespace

void Pathfinder::Invalidate() {
    grids_.clear();
}

void Pathfinder::InvalidateSide(SideId side_id) {
    grids_.erase(side_id.Value());
}

void Pathfinder::ClearRouteCache() {
    for (auto& slot : route_slots_) slot.reset();
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
                    move::Prepare(game, grid, side, movement.mode, dest_x, dest_y),
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
        const City* dest_city = map.CityAtFootprint(dest_x, dest_y);
        bool dest_city_seen = false;
        if (dest_city && fog && side_ptr && side_ptr->IsComputerControlled() &&
            !side_ptr->IsClassicAi()) {
            for (int oy = 0; oy <= 1; oy++) {
                for (int ox = 0; ox <= 1; ox++) {
                    if (game::Seen(game, side, dest_city->X() + ox, dest_city->Y() + oy))
                        dest_city_seen = true;
                }
            }
        }
        std::vector<int> dist(static_cast<size_t>(width) * height, move::kShut);
        for (int y = bound_y0; y <= bound_y1; y++) {
            for (int x = bound_x0; x <= bound_x1; x++) {
                int tile = y * width + x;
                int byte = grid[tile];
                bool is_dest_city_tile = dest_city_seen && map.CityAtFootprint(x, y) == dest_city;
                bool hidden = fog && !is_dest_city_tile && !game::Seen(game, side, x, y);
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
                dist[tile] = shut ? move::kShut : kUnreached;
            }
        }
        Query query{width, height,        start_x,        start_y,        dest_x,  dest_y,
                    &grid, movement.mode, movement.woods, movement.hills, penalty, std::move(dist),
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
                    move::Prepare(game, grid, side, movement.mode, dest_x, dest_y),
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
    Query query{width, height, x,       y,
                x,     y,      &grid,   mode,
                woods, hills,  penalty, move::Prepare(game, grid, fog_side, mode, x, y),
                0,     false,  nullptr, range};
    if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
        query.dist[goal] = -1;
        if (Wavefront(query, 0) == 0) Wavefront(query, 1);
    }
    for (auto& [tile, byte] : restore) grid[tile] = byte;
    return query.dist;
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
    Query query{width, height, x,       y,
                x,     y,      &grid,   mode,
                woods, hills,  penalty, move::Prepare(game, grid, fog_side, mode, x, y),
                0,     false,  nullptr, range};
    ShutVisibleHostileStacks(game, grid_side, x, y, kNone, kNone, query.dist);
    if (query.dist[goal] != move::kShut || move::Has(grid[goal], move::kCityF)) {
        query.dist[goal] = -1;
        if (Wavefront(query, 0) == 0) Wavefront(query, 1);
    }
    for (auto& [tile, byte] : restore) grid[tile] = byte;
    return query.dist;
}

namespace move {

std::vector<int> Prepare(Game& game, const std::vector<int>& grid, int side_index, int mode,
                         int dest_x, int dest_y) {
    const Map& map = game.GetMap();
    int width = map.Width(), height = map.Height();
    const Side* side = map.SideAt(side_index);
    bool fog = map.GameOptions().hidden_map != 0 && side &&
               (!side->IsComputerControlled() || !side->IsClassicAi());
    const City* dest_city = map.CityAtFootprint(dest_x, dest_y);
    bool dest_city_seen = false;
    if (dest_city && fog && side && side->IsComputerControlled() && !side->IsClassicAi()) {
        for (int oy = 0; oy <= 1; oy++) {
            for (int ox = 0; ox <= 1; ox++) {
                if (game::Seen(game, side_index, dest_city->X() + ox, dest_city->Y() + oy))
                    dest_city_seen = true;
            }
        }
    }
    std::vector<int> dist(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int tile = y * width + x;
            int byte = grid[tile];
            bool is_dest_city_tile = dest_city_seen && map.CityAtFootprint(x, y) == dest_city;
            bool hidden = fog && !is_dest_city_tile && !game::Seen(game, side_index, x, y);
            bool shut;
            if (mode == kLand)
                shut = byte % 8 == 0 || (hidden && !(x == dest_x && y == dest_y));
            else if (mode == kBoat)
                shut = !Has(byte, kWaterF) || (hidden && x != dest_x && y != dest_y);
            else
                shut = (byte % 8 == 0 && Has(byte, kCityF)) ||
                       (hidden && x != dest_x && y != dest_y);
            dist[tile] = shut ? kShut : kUnreached;
        }
    }
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
    Query query;
    query.w = width;
    query.h = height;
    query.sx = start_x;
    query.sy = start_y;
    query.dx = dest_x;
    query.dy = dest_y;
    query.grid = &grid;
    query.mode = kLand;
    query.woods = query.hills = false;
    query.penalty =
            dest_terrain == kWater || dest_terrain == kShore ? kWaterPenaltyTo : kWaterPenalty;
    query.dist.resize(static_cast<size_t>(width) * height);
    for (int tile = 0; tile < width * height; tile++)
        query.dist[tile] = grid[tile] % 8 == 0 ? kShut : kUnreached;
    query.roads = true;
    query.is_bridge = [&ground](int x, int y) { return ground.terrain(x, y) == kBridge; };
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
