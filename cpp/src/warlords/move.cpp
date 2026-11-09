#include "warlords/move.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <map>

#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

/** @brief The cost grids cached on the game state, one per side asked for. */
struct Grids {
    std::map<int, std::vector<int>> by_side;  ///< Keyed by side index; kNone for nobody.
};

namespace move {

const char* const kTerrainNames[12] = {"road",      "bridge", "water", "shore", "forest", "hills",
                                       "mountains", "plain",  "marsh", "tower", "city",   "site"};

Mode StackMode(const Game& game, const Stack& stack) {
    bool at_sea = false, any_boat = false;
    bool all_fly = true, any_fly = false, all_non_hero_fly = true, hero_flight = false;
    bool woods = false, hills = false;
    for (Army* army : stack) {
        const ArmyType* army_type = game.types.ById(army->type);
        if (army->at_sea) at_sea = true;
        if (army_type->boat) any_boat = true;
        if (army_type->flies && !army->at_sea) any_fly = true;
        if (!army_type->flies || army->at_sea) {
            all_fly = false;
            if (army->type != armytype::kHero) all_non_hero_fly = false;
        }
        if (army->type == armytype::kHero) {
            for (Item* item : army->items)
                if (item->type == rules::kItemFlight) hero_flight = true;
        }
        if (army_type->woods_move) woods = true;
        if (army_type->hills_move) hills = true;
    }
    if (at_sea) return Mode{kLand, false, false, true};
    if (any_boat) return Mode{kBoat, woods, hills, false};
    if (hero_flight || all_fly || (all_non_hero_fly && any_fly))
        return Mode{kFlying, woods, hills, false};
    return Mode{kLand, woods, hills, false};
}

/**
 * @brief The walk IsPort() takes round a city, as compass directions from its
 * top-left tile: it visits the twelve tiles bordering the 2x2 footprint.
 */
static const int kPortTour[12] = {0, 2, 2, 4, 4, 4, 6, 6, 6, 0, 0, 0};

bool IsPort(const Game& game, const City& city) {
    int x = city.x, y = city.y;
    for (int direction : kPortTour) {
        x += kDirs[direction][0];
        y += kDirs[direction][1];
        if (x < 0 || y < 0 || x >= game.map->width || y >= game.map->height) return false;
        int terrain = scn::TerrainAt(*game.map, x, y);
        if (terrain == kBridge || terrain == kWater || terrain == kShore) return true;
    }
    return false;
}

std::vector<int>& Grid(Game& game, int side_index) {
    if (!game.grids) game.grids = std::make_shared<Grids>();
    int cache_key = side_index;  // kNone (-1) for nobody, as the reference's -1
    auto cached = game.grids->by_side.find(cache_key);
    if (cached != game.grids->by_side.end()) return cached->second;

    const Map& map = *game.map;
    int width = map.width, height = map.height;
    std::vector<int> grid(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int tile = y * width + x;
            int terrain = scn::TerrainAt(map, x, y);
            int byte = scn::RoadAt(map, x, y) % 0x20 != 0 ? 1 : kCost[terrain];
            if (terrain == kBridge)
                byte |= kCrossF | kWaterF;
            else if (terrain == kWater || terrain == kShore)
                byte |= kWaterF;
            else if (terrain == kForest)
                byte |= kForestF;
            else if (terrain == kHills)
                byte |= kHillsF;
            if (map.crossing[tile]) byte |= kCrossF;
            grid[tile] = byte;
        }
    }
    for (const City& city : map.cities) {
        // kNone is nobody, as the reference's -1: no city is its own
        bool mine = side_index != kNone && city.owner_index == side_index && !city.razed;
        bool port = (mine || city.razed) && IsPort(game, city);
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int x = city.x + dx, y = city.y + dy;
                if (x < width && y < height) {
                    int tile = y * width + x;
                    int byte = grid[tile];
                    if (city.razed) {
                        if (port) byte |= kCrossF | kWaterF;
                    } else {
                        byte |= kCityF;
                        if (mine) {
                            if (port) byte |= kCrossF | kWaterF;
                        } else {
                            byte = byte - (byte % 8);  // cost 0: impassable
                        }
                    }
                    grid[tile] = byte;
                }
            }
        }
    }
    return game.grids->by_side[cache_key] = std::move(grid);
}

void Invalidate(Game& game) {
    game.grids.reset();
}

std::optional<int> StepCost(int from_byte, int to_byte, int mode, bool woods, bool hills,
                            int penalty) {
    int cost = to_byte % 8;
    bool to_water = Has(to_byte, kWaterF), from_water = Has(from_byte, kWaterF);
    bool to_cross = Has(to_byte, kCrossF);
    if (mode == kFlying) {
        if (cost == 0 && Has(to_byte, kCityF)) return std::nullopt;
        if (to_water && !to_cross) return 2;
        if (cost == 0 || cost > 2) return 2;
        return cost;
    }
    if (mode == kBoat) {
        if (!to_water && !Has(to_byte, kCityF)) return std::nullopt;
        if (cost == 0) return std::nullopt;
        return cost;
    }
    if (cost == 0) return std::nullopt;
    if (to_water != from_water && !to_cross && !Has(from_byte, kCrossF)) return std::nullopt;
    if (cost > 2) {
        if (woods && Has(to_byte, kForestF)) cost = 2;
        if (hills && Has(to_byte, kHillsF)) cost = 2;
    }
    if ((!from_water || Has(from_byte, kCrossF)) && to_water && !to_cross) cost += penalty;
    return cost;
}

int WalkCost(int byte, int mode, bool woods, bool hills) {
    int cost = byte % 8;
    if (mode == kFlying) {
        if (Has(byte, kWaterF) && !Has(byte, kCrossF)) return 2;
        if (cost == 0 || cost > 2) return 2;
        return cost;
    }
    if (mode == kLand && cost > 2) {
        if (woods && Has(byte, kForestF)) cost = 2;
        if (hills && Has(byte, kHillsF)) cost = 2;
    }
    return cost;
}

bool CrossesShore(int byte, bool at_sea) {
    if (Has(byte, kCrossF)) return false;
    return Has(byte, kWaterF) != at_sea;
}

int StackMoves(const Stack& stack) {
    if (stack.empty()) return 0;
    int least = INT_MAX;
    for (Army* army : stack) least = std::min(least, army->moves);
    return least;
}

std::optional<Preview> PreviewRoute(Game& game, const Stack& stack, int x, int y) {
    if (stack.empty()) return std::nullopt;
    auto path = FindPath(game, stack, stack[0]->x, stack[0]->y, x, y);
    if (!path || path->empty()) return std::nullopt;
    int left = StackMoves(stack), reach = 0;
    for (size_t i = 0; i < path->size(); i++) {
        const Step& step = (*path)[i];
        if (step.cost > left) break;
        left -= step.cost;
        reach = static_cast<int>(i) + 1;
    }
    return Preview{*path, reach};
}

/**
 * @brief A tile the search has not reached yet.
 *
 * The search's distance grid holds this until the flood gets there, kShut
 * where the stack may never go, and then the distance from the destination --
 * negative while the tile still has to spread it further.
 */
static const int kUnreached = 30000;

/**
 * @brief Where the wavefront spreads from a tile, by where the tile lies.
 *
 * kNeighbourListStart gives, for each class of tile, where its list of
 * neighbour offsets starts in kNeighbourDx / kNeighbourDy; a list ends at
 * -10. The classes: 0 inside the map (all eight neighbours), 1 top-left
 * corner, 2 top edge, 3 top-right corner, 4 left edge, 5 right edge,
 * 6 bottom-left corner, 7 bottom edge, 8 bottom-right corner, and 9 inside
 * the map spreading straight only (the road builder).
 */
static const int kNeighbourListStart[10] = {0, 9, 13, 19, 23, 29, 35, 39, 45, 49};
/** @brief The x offsets of the neighbour lists; see kNeighbourListStart. */
static const int kNeighbourDx[] = {-1,  0,  1,   1,  1,   0,  -1,  -1, -10, 1, 1,   0,  -10, 1,
                                   1,   0,  -1,  -1, -10, 0,  -1,  -1, -10, 0, 1,   1,  1,   0,
                                   -10, -1, 0,   0,  -1,  -1, -10, 0,  1,   1, -10, -1, 0,   1,
                                   1,   -1, -10, -1, 0,   -1, -10, 0,  1,   0, -1,  -10};
/** @brief The y offsets of the neighbour lists; see kNeighbourListStart. */
static const int kNeighbourDy[] = {-1,  -1, -1,  0,  1,   1, 1,   0,  -10, 0,  1,   1,  -10, 0,
                                   1,   1,  1,   0,  -10, 1, 1,   0,  -10, -1, -1,  0,  1,   1,
                                   -10, -1, -1,  1,  1,   0, -10, -1, -1,  0,  -10, -1, -1,  -1,
                                   0,   0,  -10, -1, -1,  0, -10, -1, 0,   1,  0,   -10};

/**
 * @brief The order the trace tries the neighbours in, as turns from the
 * direction straight at the destination: straight, then 45, 90 and 135
 * degrees to either side (left first), then straight away.
 */
static const int kTraceOrder[8] = {0, 7, 1, 6, 2, 5, 3, 4};

int Direction(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x && from_y == to_y) return -1;
    if (from_x == to_x) return to_y < from_y ? 0 : 4;
    if (from_y == to_y) return to_x < from_x ? 6 : 2;
    if (from_x <= to_x) return to_y < from_y ? 1 : 3;
    return to_y < from_y ? 7 : 5;
}

int Distance(int from_x, int from_y, int to_x, int to_y) {
    double delta_x = from_x - to_x, delta_y = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(delta_x * delta_x + delta_y * delta_y)));
}

std::vector<int> Prepare(Game& game, const std::vector<int>& grid, int side_index, int mode,
                         int dest_x, int dest_y) {
    int width = game.map->width, height = game.map->height;
    Side* side = game.map->Side(side_index);
    bool fog = game.map->options.hidden_map != 0 && side && (!side->computer || !side->classic_ai);
    City* dest_city = (dest_x >= 0 && dest_y >= 0 && dest_x < width && dest_y < height)
                              ? game.map->city_tile[dest_y * width + dest_x]
                              : nullptr;
    bool dest_city_seen = false;
    if (dest_city && fog && side && side->computer && !side->classic_ai) {
        for (int oy = 0; oy <= 1; oy++)
            for (int ox = 0; ox <= 1; ox++)
                if (game::Seen(game, side_index, dest_city->x + ox, dest_city->y + oy))
                    dest_city_seen = true;
    }
    std::vector<int> dist(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int tile = y * width + x;
            int byte = grid[tile];
            bool is_dest_city_tile = dest_city_seen && game.map->city_tile[tile] == dest_city;
            bool hidden = fog && !is_dest_city_tile && !game::Seen(game, side_index, x, y);
            bool shut;
            // Matches the reference: a boat or a flier may enter a hidden tile
            // anywhere in the destination's row or column.
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

namespace {
/** @brief One run of the search: what it searches over and how far it has got. */
struct Query {
    int w;                   ///< The map's width.
    int h;                   ///< The map's height.
    int sx;                  ///< The start, where the trace begins.
    int sy;                  ///< The start, where the trace begins.
    int dx;                  ///< The destination, where the wavefront begins.
    int dy;                  ///< The destination, where the wavefront begins.
    std::vector<int>* grid;  ///< The cost grid searched over.
    int mode;                ///< The stack mode.
    bool woods;              ///< The woods move bonus.
    bool hills;              ///< The hills move bonus.
    int penalty;             ///< The water charge for a land stack (kWaterPenalty or ...To).
    std::vector<int> dist;   ///< The distance grid the wavefront fills.
    int ring = 0;            ///< The next square the wavefront sweeps, as its radius.
    bool roads = false;      ///< The road builder: straight spreading, its own trace.
    std::function<bool(int, int)> is_bridge;  ///< For the road builder: is (x, y) a bridge?
    int flood = kNone;                        ///< A flood's range (flag 0x10), kNone for a route.
};

/**
 * @brief The wavefront: sweeps squares ever wider round the destination,
 * each tile passing its distance on to its neighbours.
 *
 * Only tiles inside the rectangle round start and destination, grown by a
 * margin of 6 tiles on the first pass and 50 on the second, spread at all.
 * See docs/rules.md > The search.
 *
 * @return 1 when the start was reached, 0 when a sweep spread nothing.
 */
int Wavefront(Query& query, int pass) {
    int width = query.w, height = query.h;
    int start_x = query.sx, start_y = query.sy, dest_x = query.dx, dest_y = query.dy;
    std::vector<int>& dist = query.dist;
    const std::vector<int>& grid = *query.grid;
    int mode = query.mode;
    // a flood (flag 0x10) spreads as far as its range, and as many rounds
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
                    bool cur_water = Has(cur, kWaterF), cur_cross = Has(cur, kCrossF);
                    // the road builder spreads straight only (neighbour class 9)
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
                        int next_x = x + kNeighbourDx[neighbour],
                            next_y = y + kNeighbourDy[neighbour];
                        neighbour++;
                        int next_tile = next_y * width + next_x;
                        int next_dist = dist[next_tile];
                        if (next_dist != kShut) {
                            int next_byte = grid[next_tile];
                            int step_cost = next_byte % 8;
                            bool can_spread = true;
                            bool next_water = Has(next_byte, kWaterF),
                                 next_cross = Has(next_byte, kCrossF);
                            if (next_x == start_x && next_y == start_y) {
                                if (mode == kLand && !cur_cross && !next_cross &&
                                    next_water != cur_water)
                                    can_spread = false;
                                step_cost = 1;
                            } else if (mode == kFlying) {
                                if ((!next_water || next_cross) && step_cost != 0 &&
                                    step_cost < 3) {
                                    // as dear as the tile, where that is 1 or 2
                                } else {
                                    step_cost = 2;
                                }
                            } else if (mode == kBoat) {
                                if (!next_water && !Has(next_byte, kCityF)) {
                                    dist[next_tile] = kShut;
                                    can_spread = false;
                                }
                            } else {
                                if (!cur_cross && !next_cross && next_water != cur_water) {
                                    can_spread = false;
                                } else {
                                    if (step_cost > 2 &&
                                        ((query.hills && Has(next_byte, kHillsF)) ||
                                         (query.woods && Has(next_byte, kForestF))))
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

/**
 * @brief The trace: from the start, each step to whichever neighbour the
 * flood put nearest the destination; at most 198 steps.
 */
Path Trace(Query& query) {
    int width = query.w, height = query.h;
    const std::vector<int>& dist = query.dist;
    const std::vector<int>& grid = *query.grid;
    int x = query.sx, y = query.sy;
    int target_x = query.dx, target_y = query.dy;
    int best_dist = std::abs(dist[y * width + x]);
    Path steps;
    while (!(x == target_x && y == target_y)) {
        int cur = grid[y * width + x];
        bool land = query.mode == kLand;
        bool cur_water = land && Has(cur, kWaterF);
        bool cur_cross = land && Has(cur, kCrossF);
        int toward = Direction(x, y, target_x, target_y);
        int best_x = kNone, best_y = kNone;
        bool got = false;
        for (int turn : kTraceOrder) {
            int dir = (toward + turn) % 8;
            int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
            bool inside = next_x >= 0 && next_y >= 0 && next_x < width && next_y < height;
            // the road builder steps diagonally only over water that is no bridge
            if (query.roads && dir % 2 == 1 &&
                !(inside && Has(grid[next_y * width + next_x], kWaterF) &&
                  !query.is_bridge(next_x, next_y)))
                continue;
            if (inside) {
                int next_dist = dist[next_y * width + next_x];
                if (next_dist != kUnreached && next_dist != kShut) {
                    int next_byte = grid[next_y * width + next_x];
                    bool blocked = land && !cur_cross && !Has(next_byte, kCrossF) &&
                                   Has(next_byte, kWaterF) != cur_water;
                    // ... and takes a first neighbour no nearer than here
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
            steps.push_back(Step{x, y, 0});
        } else {
            target_x = x;
            target_y = y;
        }
    }
    return steps;
}

}  // namespace

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
                byte = kCityF;  // nobody's city is player 14's
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

namespace {

/**
 * @brief Whether a destination one tile away is simply stepped to: it is,
 * unless a land or boat stack would cross between land and water, or the
 * ground there is impassable.
 */
bool SingleStep(const Game& game, int mode, int start_x, int start_y, int dest_x, int dest_y) {
    auto wet = [](int terrain) { return terrain == kWater || terrain == kShore; };
    int from_terrain = scn::TerrainAt(*game.map, start_x, start_y),
        to_terrain = scn::TerrainAt(*game.map, dest_x, dest_y);
    if (mode != kFlying) {
        if (wet(from_terrain) != wet(to_terrain)) return false;
        if (kCost[to_terrain] == 0) return false;
    }
    return true;
}
}  // namespace

namespace {
/**
 * @brief Opens a city to path to.
 *
 * A stack may always path *to* a city, just never through one it does not
 * own: when (dest_x, dest_y) is a city tile the grid shuts, its four tiles
 * are opened at the city cost (and made a crossing for a port).
 *
 * @return The tiles changed and their old bytes, to put back.
 */
std::vector<std::pair<int, int>> OpenCityAt(Game& game, std::vector<int>& grid, int dest_x,
                                            int dest_y) {
    int width = game.map->width, height = game.map->height;
    std::vector<std::pair<int, int>> restore;
    int goal = dest_y * width + dest_x;
    int goal_byte = grid[goal];
    City* city = game.map->city_tile[goal];
    if (city && goal_byte % 8 == 0 && Has(goal_byte, kCityF)) {
        bool port = IsPort(game, *city);
        for (int ox = 0; ox <= 1; ox++) {
            for (int oy = 0; oy <= 1; oy++) {
                int x = city->x + ox, y = city->y + oy;
                if (x < width && y < height) {
                    int tile = y * width + x;
                    restore.emplace_back(tile, grid[tile]);
                    int byte = grid[tile] - grid[tile] % 8 + kCost[kCity];
                    if (port) byte |= kCrossF | kWaterF;
                    grid[tile] = byte;
                }
            }
        }
    }
    return restore;
}

/**
 * @brief The request's flags, which a cached route must match: 8 at sea,
 * else for a land stack 1 for the hills bonus and 2 for the woods'.
 */
int RouteFlags(int mode, bool woods, bool hills, bool at_sea) {
    if (at_sea) return 8;
    if (mode != kLand) return 0;
    return (hills ? 1 : 0) | (woods ? 2 : 0);
}

/** @brief Moves (x, y) one step in a compass direction, unless that leaves the map. */
bool StepDir(int& x, int& y, int dir, int width, int height) {
    int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
    if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height) return false;
    x = next_x;
    y = next_y;
    return true;
}

/** @brief The compass direction of a one-tile step, or 0 when it is none. */
int DirOf(int dx, int dy) {
    for (int dir = 0; dir < 8; dir++)
        if (kDirs[dir][0] == dx && kDirs[dir][1] == dy) return dir;
    return 0;
}

/**
 * @brief A cached route the request can use: same destination, mode and
 * flags, with the start somewhere on it before its end.
 * @return The rest of it as directions, or none.
 */
std::optional<std::vector<int>> CacheHit(const Game& game, const std::optional<CachedRoute>& entry,
                                         int start_x, int start_y, int dest_x, int dest_y, int mode,
                                         int flags) {
    if (!entry || entry->dx != dest_x || entry->dy != dest_y || entry->mode != mode ||
        entry->flags != flags)
        return std::nullopt;
    int x = entry->sx, y = entry->sy;
    for (size_t i = 0; i < entry->dirs.size(); i++) {
        if (x == start_x && y == start_y)
            return std::vector<int>(entry->dirs.begin() + i, entry->dirs.end());
        if (!StepDir(x, y, entry->dirs[i], game.map->width, game.map->height)) return std::nullopt;
    }
    return std::nullopt;
}

/**
 * @brief Keeps a route found: a short one in slot 0; a long one in the first
 * free slot of 1-19, or over the one whose own route is shortest when that is
 * shorter than this one.
 */
void CacheStore(Game& game, int distance, int start_x, int start_y, int dest_x, int dest_y,
                int mode, int flags, const std::vector<int>& dirs) {
    auto& slots = game.path_cache->slots;
    int slot = -1;
    if (distance < 15) {
        slot = 0;
    } else {
        int shortest = -1, least = 10000;
        for (int i = 1; i < kCacheSlots; i++) {
            const auto& entry = slots[i];
            if (!entry) {
                slot = i;
                break;
            }
            int entry_distance = Distance(entry->dx, entry->dy, entry->sx, entry->sy);
            if (entry_distance < least) {
                shortest = i;
                least = entry_distance;
            }
        }
        if (slot == -1 && shortest != -1 && least < distance) slot = shortest;
    }
    if (slot != -1) slots[slot] = CachedRoute{mode, flags, start_x, start_y, dest_x, dest_y, dirs};
}
}  // namespace

std::shared_ptr<PathCache> NewPathCache() {
    return std::make_shared<PathCache>();
}

std::optional<Path> FindPath(Game& game, const Stack& stack, int start_x, int start_y, int dest_x,
                             int dest_y) {
    if (start_x == dest_x && start_y == dest_y) return Path{};
    int width = game.map->width, height = game.map->height;
    if (dest_x < 0 || dest_y < 0 || dest_x >= width || dest_y >= height) return std::nullopt;

    int side = stack.empty() ? kNone : stack[0]->owner;
    Mode movement = StackMode(game, stack);
    std::vector<int>& grid = Grid(game, side);
    int flags = RouteFlags(movement.mode, movement.woods, movement.hills, movement.at_sea);
    int distance = Distance(dest_x, dest_y, start_x, start_y);

    std::optional<Path> path;
    std::optional<std::vector<int>> dirs;
    if (game.path_cache) {
        dirs = CacheHit(game, game.path_cache->slots[0], start_x, start_y, dest_x, dest_y,
                        movement.mode, flags);
        for (int i = 1; !dirs && distance > 14 && i < kCacheSlots; i++) {
            dirs = CacheHit(game, game.path_cache->slots[i], start_x, start_y, dest_x, dest_y,
                            movement.mode, flags);
        }
    }
    if (dirs) {
        path = Path{};
        int x = start_x, y = start_y;
        for (int dir : *dirs) {
            StepDir(x, y, dir, width, height);
            path->push_back(Step{x, y, 0});
        }
    } else if (distance == 1 && SingleStep(game, movement.mode, start_x, start_y, dest_x, dest_y)) {
        path = Path{Step{dest_x, dest_y, 0}};
    } else {
        int goal = dest_y * width + dest_x;
        auto restore = OpenCityAt(game, grid, dest_x, dest_y);
        int penalty = 0;
        if (movement.mode == kLand) {
            int dest_terrain = scn::TerrainAt(*game.map, dest_x, dest_y);
            penalty = (dest_terrain == kWater || dest_terrain == kShore) ? kWaterPenaltyTo
                                                                         : kWaterPenalty;
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
                    Prepare(game, grid, side, movement.mode, dest_x, dest_y),
                    0,
                    false,
                    nullptr};
        int found = 0;
        if (query.dist[goal] != kShut || Has(grid[goal], kCityF)) {
            query.dist[goal] = -1;
            for (int pass = 0; pass <= 1; pass++) {
                if (found == 0) found = Wavefront(query, pass);
            }
        }
        if (found == 1) {
            path = Trace(query);
            if (game.path_cache) {
                std::vector<int> route_dirs;
                int prev_x = start_x, prev_y = start_y;
                for (const Step& step : *path) {
                    route_dirs.push_back(DirOf(step.x - prev_x, step.y - prev_y));
                    prev_x = step.x;
                    prev_y = step.y;
                }
                CacheStore(game, distance, start_x, start_y, dest_x, dest_y, movement.mode, flags,
                           route_dirs);
            }
        }
        for (auto& [tile, byte] : restore) grid[tile] = byte;
    }

    // what the walk spends on each step; see docs/rules.md > Going to sea
    if (path) {
        bool ashore = false;
        for (Step& step : *path) {
            int byte = grid[step.y * width + step.x];
            if (ashore) {
                step.cost = kPastShore;
            } else {
                step.cost = WalkCost(byte, movement.mode, movement.woods, movement.hills);
                ashore = movement.mode == kLand && CrossesShore(byte, movement.at_sea);
            }
        }
    }
    if (path && path->empty()) path.reset();
    return path;
}

std::vector<int> Flood(Game& game, int grid_side, int x, int y, int range, int mode, bool woods,
                       bool hills, int fog_side) {
    int width = game.map->width, height = game.map->height;
    std::vector<int>& grid = Grid(game, grid_side);
    int goal = y * width + x;
    auto restore = OpenCityAt(game, grid, x, y);
    int penalty = 0;
    if (mode == kLand) {
        int terrain = scn::TerrainAt(*game.map, x, y);
        penalty = (terrain == kWater || terrain == kShore) ? kWaterPenaltyTo : kWaterPenalty;
    }
    Query query{width, height, x,      y,     x,       y,
                &grid, mode,   woods,  hills, penalty, Prepare(game, grid, fog_side, mode, x, y),
                0,     false,  nullptr};
    query.flood = range;
    if (query.dist[goal] != kShut || Has(grid[goal], kCityF)) {
        query.dist[goal] = -1;
        if (Wavefront(query, 0) == 0) Wavefront(query, 1);
    }
    for (auto& [tile, byte] : restore) grid[tile] = byte;
    return query.dist;
}

std::string SettleSea(Game& game, const Stack& stack, int x, int y, int mode, bool was_at_sea) {
    if (mode != kLand) return "";
    int terrain = scn::TerrainAt(*game.map, x, y);
    if (!was_at_sea) {
        if (terrain != kWater && terrain != kShore) return "";
        bool flier = false, walker = false;
        for (Army* army : stack) {
            if (game.types.ById(army->type)->flies)
                flier = true;
            else if (army->type != armytype::kHero)
                walker = true;
        }
        if (walker) flier = false;
        std::string change;
        for (Army* army : stack) {
            if (!game.types.ById(army->type)->flies && (army->type != armytype::kHero || !flier)) {
                army->at_sea = true;
                change = "to sea";
            }
        }
        return change;
    } else if (terrain != kWater && terrain != kShore && terrain != kBridge) {
        for (Army* army : stack) army->at_sea = false;
        return "ashore";
    }
    return "";
}

std::string_view WalkStopName(WalkStop stop) {
    switch (stop) {
    case WalkStop::kNone:
        return "";
    case WalkStop::kArrived:
        return "arrived";
    case WalkStop::kOutOfMoves:
        return "out of moves";
    case WalkStop::kAttack:
        return "attack";
    case WalkStop::kAtPeace:
        return "at peace";
    case WalkStop::kBlocked:
        return "blocked";
    case WalkStop::kNoRoute:
        return "no route";
    }
    return "";
}

namespace {
/**
 * @brief Whether the walk leaves the stack out of moves.
 *
 * With `moves_left` the moves it has left: below 2; or, having walked,
 * stopped by neither an enemy city nor a stack at war, below the cost of the
 * route **from its start** to the step after where it stopped (the step-cost
 * totals keep 255 at most; none past the route's end). A human's stack out
 * of moves is done for the turn and put down. See docs/rules.md > Moving a
 * stack.
 *
 * @param landed The steps to where it stopped.
 */
bool OutOfMoves(const Stack& stack, const Path& path, int landed, WalkStop stopped) {
    int moves_left = StackMoves(stack);
    if (moves_left < 2) return true;
    if (landed == 0 || stopped == WalkStop::kAttack || stopped == WalkStop::kAtPeace) return false;
    if (landed >= static_cast<int>(path.size())) return false;
    int total = 0;
    for (int i = 0; i <= landed; i++) total += path[i].cost;
    if (total >= 255) return false;
    return moves_left < total;
}

/** @brief Walks `stack` along `path`, stopping where the rules say to stop. */
WalkResult WalkPath(Game& game, const Stack& stack, const Path& path) {
    int side = stack.empty() ? kNone : stack[0]->owner;
    int left = StackMoves(stack);
    WalkResult result;
    result.stopped = WalkStop::kArrived;
    for (Army* army : stack)
        if (army->at_sea) result.sea_before = true;
    int last_ok = 0, cumulative = 0;
    std::map<int, int> cost_to;

    for (int i = 1; i <= static_cast<int>(path.size()); i++) {
        const Step& step = path[i - 1];
        // a step is taken while what the walk has cost so far fits in the moves
        // left: there is no floor, so one move still buys a road
        if (step.cost > left) {
            result.stopped = WalkStop::kOutOfMoves;
            break;
        }
        City* city = game.map->city_tile[step.y * game.map->width + step.x];
        auto here = game::ArmiesAt(game, step.x, step.y);
        if (city && !city->razed && city->owner_index != side) {
            if (!diplomacy::MayAttack(game, side, city->owner_index)) {
                result.stopped = WalkStop::kAtPeace;
                break;
            }
            result.stopped = WalkStop::kAttack;
            result.attack = Attack{step.x, step.y, city};
            break;
        }
        bool theirs = !here.empty() && here[0]->owner != side;
        if (theirs && diplomacy::MayAttack(game, side, here[0]->owner)) {
            result.stopped = WalkStop::kAttack;
            result.attack = Attack{step.x, step.y, nullptr};
            break;
        }
        left -= step.cost;
        cumulative += step.cost;
        // the stack may only stop on a tile of its own where it fits; other
        // steps -- a side at peace's stack among them -- are passed over
        if (!theirs && static_cast<int>(here.size() + stack.size()) <= rules::kMaxStack) {
            last_ok = i;
            cost_to[i] = cumulative;
        }
    }

    if (last_ok == 0) {
        if (result.stopped == WalkStop::kArrived) result.stopped = WalkStop::kBlocked;
        result.out_of_moves = OutOfMoves(stack, path, 0, result.stopped);
        return result;
    }

    int cost = cost_to[last_ok];
    const Step& dest = path[last_ok - 1];
    Mode movement = StackMode(game, stack);
    for (Army* army : stack) {
        game.armies.Relocate(army, TilePos(dest.x, dest.y));
        army->moves = std::max(0, army->moves - cost);
    }
    std::string change = SettleSea(game, stack, dest.x, dest.y, movement.mode, movement.at_sea);
    if (!change.empty()) {
        for (Army* army : stack) {
            if (army->at_sea == (change == "to sea")) army->moves = 0;
        }
        result.stopped = WalkStop::kOutOfMoves;
        result.attack.reset();
    }

    // walking uncovers the map as it goes
    if (game.map->options.hidden_map != 0 && side != kNone) {
        bool flying = ModeOf(game, stack) == kFlying;
        int found = 0;
        for (int i = 0; i < static_cast<int>(path.size()); i++) {
            if (i == last_ok - 1) break;
            found += game::Reveal(game, side, path[i].x, path[i].y, flying);
        }
        found += game::Reveal(game, side, dest.x, dest.y, flying);
        if (found > 0) Invalidate(game);
    }

    result.steps = last_ok;
    result.spent = cost;
    for (int i = 0; i < last_ok; i++) result.walked.emplace_back(path[i].x, path[i].y);
    if (last_ok < static_cast<int>(path.size()) && result.stopped == WalkStop::kArrived)
        result.stopped = WalkStop::kBlocked;
    // going to sea or coming ashore ends the move as running out does
    result.out_of_moves = !change.empty() || OutOfMoves(stack, path, last_ok, result.stopped);
    return result;
}
}  // namespace

WalkResult Walk(Game& game, const Stack& stack, const Path& path) {
    game::TidyTowers(game);
    WalkResult result = WalkPath(game, stack, path);
    game::TidyTowers(game);
    return result;
}

WalkResult MoveTo(Game& game, const Stack& stack, int x, int y) {
    WalkResult result;
    if (stack.empty()) {
        result.stopped = WalkStop::kBlocked;
        return result;
    }
    auto path = FindPath(game, stack, stack[0]->x, stack[0]->y, x, y);
    if (!path) {
        result.stopped = WalkStop::kNoRoute;
        return result;
    }
    if (path->empty()) {
        result.stopped = WalkStop::kArrived;
        return result;
    }
    return Walk(game, stack, *path);
}

}  // namespace move
}  // namespace w2
