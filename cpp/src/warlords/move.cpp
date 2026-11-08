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

struct Grids {
    std::map<int, std::vector<int>> by_side;
};

namespace move {

const char* const kTerrainNames[12] = {"road",      "bridge", "water", "shore", "forest", "hills",
                                       "mountains", "plain",  "marsh", "tower", "city",   "site"};

Mode StackMode(const Game& g, const Stack& stack) {
    bool at_sea = false, any_boat = false;
    bool all_fly = true, any_fly = false, all_non_hero_fly = true, hero_flight = false;
    bool woods = false, hills = false;
    for (Army* a : stack) {
        const ArmyType* t = g.types.ById(a->type);
        if (a->at_sea) at_sea = true;
        if (t->boat) any_boat = true;
        if (t->flies && !a->at_sea) any_fly = true;
        if (!t->flies || a->at_sea) {
            all_fly = false;
            if (a->type != armytype::kHero) all_non_hero_fly = false;
        }
        if (a->type == armytype::kHero) {
            for (Item* it : a->items)
                if (it->type == rules::kItemFlight) hero_flight = true;
        }
        if (t->woods_move) woods = true;
        if (t->hills_move) hills = true;
    }
    if (at_sea) return Mode{kLand, false, false, true};
    if (any_boat) return Mode{kBoat, woods, hills, false};
    if (hero_flight || all_fly || (all_non_hero_fly && any_fly))
        return Mode{kFlying, woods, hills, false};
    return Mode{kLand, woods, hills, false};
}

// 1555:109d walks round a city from its top left tile in these directions
static const int kPortTour[12] = {0, 2, 2, 4, 4, 4, 6, 6, 6, 0, 0, 0};

bool IsPort(const Game& g, const City& city) {
    int x = city.x, y = city.y;
    for (int d : kPortTour) {
        x += kDirs[d][0];
        y += kDirs[d][1];
        if (x < 0 || y < 0 || x >= g.map->width || y >= g.map->height) return false;
        int t = scn::TerrainAt(*g.map, x, y);
        if (t == kBridge || t == kWater || t == kShore) return true;
    }
    return false;
}

std::vector<int>& Grid(Game& g, int side_index) {
    if (!g.grids) g.grids = std::make_shared<Grids>();
    int key_s = side_index;  // NONE (-1) for nobody, as the Lua's -1
    auto it = g.grids->by_side.find(key_s);
    if (it != g.grids->by_side.end()) return it->second;

    const Map& m = *g.map;
    int w = m.width, h = m.height;
    std::vector<int> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            int terrain = scn::TerrainAt(m, x, y);
            int byte = scn::RoadAt(m, x, y) % 0x20 != 0 ? 1 : kCost[terrain];
            if (terrain == kBridge)
                byte |= kCrossF | kWaterF;
            else if (terrain == kWater || terrain == kShore)
                byte |= kWaterF;
            else if (terrain == kForest)
                byte |= kForestF;
            else if (terrain == kHills)
                byte |= kHillsF;
            if (m.crossing[i]) byte |= kCrossF;
            out[i] = byte;
        }
    }
    for (const City& c : m.cities) {
        // NONE is nobody, as the Lua's -1: no city is its own
        bool mine = side_index != kNone && c.owner_index == side_index && !c.razed;
        bool port = (mine || c.razed) && IsPort(g, c);
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int x = c.x + dx, y = c.y + dy;
                if (x < w && y < h) {
                    int i = y * w + x;
                    int byte = out[i];
                    if (c.razed) {
                        if (port) byte |= kCrossF | kWaterF;
                    } else {
                        byte |= kCityF;
                        if (mine) {
                            if (port) byte |= kCrossF | kWaterF;
                        } else {
                            byte = byte - (byte % 8);  // cost 0: impassable
                        }
                    }
                    out[i] = byte;
                }
            }
        }
    }
    return g.grids->by_side[key_s] = std::move(out);
}

void Invalidate(Game& g) {
    g.grids.reset();
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
    for (Army* a : stack) least = std::min(least, a->moves);
    return least;
}

std::optional<Preview> PreviewRoute(Game& g, const Stack& stack, int x, int y) {
    if (stack.empty()) return std::nullopt;
    auto path = FindPath(g, stack, stack[0]->x, stack[0]->y, x, y);
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

// The search, as 1555:000a runs it: UNREACHED until the flood gets there,
// SHUT where the stack may never go, and then the distance from the
// destination -- negative while the tile still has to spread it further.
static const int kUnreached = 30000;

// Where the wavefront spreads from a tile, by where the tile lies: inside,
// or along one of the map's edges or corners (4125:00e2, 00f6, 0162)
static const int kNbFirst[10] = {0, 9, 13, 19, 23, 29, 35, 39, 45, 49};
static const int kNbDx[] = {-1,  0,  1,   1,  1,   0,  -1,  -1, -10, 1, 1,   0,  -10, 1,
                            1,   0,  -1,  -1, -10, 0,  -1,  -1, -10, 0, 1,   1,  1,   0,
                            -10, -1, 0,   0,  -1,  -1, -10, 0,  1,   1, -10, -1, 0,   1,
                            1,   -1, -10, -1, 0,   -1, -10, 0,  1,   0, -1,  -10};
static const int kNbDy[] = {-1,  -1, -1,  0,  1,   1, 1,   0,  -10, 0,  1,   1,  -10, 0,
                            1,   1,  1,   0,  -10, 1, 1,   0,  -10, -1, -1,  0,  1,   1,
                            -10, -1, -1,  1,  1,   0, -10, -1, -1,  0,  -10, -1, -1,  -1,
                            0,   0,  -10, -1, -1,  0, -10, -1, 0,   1,  0,   -10};

// the order path_trace tries the neighbours in (4125:0212)
static const int kTraceOrder[8] = {0, 7, 1, 6, 2, 5, 3, 4};

int Direction(int x1, int y1, int x2, int y2) {
    if (x1 == x2 && y1 == y2) return -1;
    if (x1 == x2) return y2 < y1 ? 0 : 4;
    if (y1 == y2) return x2 < x1 ? 6 : 2;
    if (x1 <= x2) return y2 < y1 ? 1 : 3;
    return y2 < y1 ? 7 : 5;
}

int Distance(int x1, int y1, int x2, int y2) {
    double ax = x1 - x2, ay = y1 - y2;
    return static_cast<int>(std::floor(std::sqrt(ax * ax + ay * ay)));
}

std::vector<int> Prepare(Game& g, const std::vector<int>& grid, int side_index, int mode, int dx,
                         int dy) {
    int w = g.map->width, h = g.map->height;
    Side* side = g.map->Side(side_index);
    bool fog = g.map->options.hidden_map != 0 && side && (!side->computer || !side->classic_ai);
    City* dest_city =
            (dx >= 0 && dy >= 0 && dx < w && dy < h) ? g.map->city_tile[dy * w + dx] : nullptr;
    bool dest_city_seen = false;
    if (dest_city && fog && side && side->computer && !side->classic_ai) {
        for (int oy = 0; oy <= 1; oy++)
            for (int ox = 0; ox <= 1; ox++)
                if (game::Seen(g, side_index, dest_city->x + ox, dest_city->y + oy))
                    dest_city_seen = true;
    }
    std::vector<int> dist(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int k = y * w + x;
            int b = grid[k];
            bool is_dest_city_tile = dest_city_seen && g.map->city_tile[k] == dest_city;
            bool hidden = fog && !is_dest_city_tile && !game::Seen(g, side_index, x, y);
            bool shut;
            if (mode == kLand)
                shut = b % 8 == 0 || (hidden && !(x == dx && y == dy));
            else if (mode == kBoat)
                shut = !Has(b, kWaterF) || (hidden && x != dx && y != dy);
            else
                shut = (b % 8 == 0 && Has(b, kCityF)) || (hidden && x != dx && y != dy);
            dist[k] = shut ? kShut : kUnreached;
        }
    }
    return dist;
}

namespace {
struct Query {
    int w, h, sx, sy, dx, dy;
    std::vector<int>* grid;
    int mode;
    bool woods, hills;
    int penalty;
    std::vector<int> dist;
    int ring = 0;
    // the road builder (roadRoute): straight spreading, its own trace
    bool roads = false;
    std::function<bool(int, int)> is_bridge;
    int flood = kNone;  // a flood's range (flag 0x10), NONE for a route
};

// path_wavefront (1555:0373): sweeps squares ever wider round the
// destination; 6 tiles of margin on the first pass, 50 on the second.
int Wavefront(Query& q, int pass) {
    int w = q.w, h = q.h;
    int sx = q.sx, sy = q.sy, dx = q.dx, dy = q.dy;
    std::vector<int>& dist = q.dist;
    const std::vector<int>& grid = *q.grid;
    int mode = q.mode;
    // a flood (flag 0x10) spreads as far as its range, and as many rounds
    int margin = q.flood != kNone ? q.flood : (pass == 0 ? 6 : 50);
    if (pass == 0 || q.flood != kNone) q.ring = 0;
    int lox = std::min(sx, dx), loy = std::min(sy, dy);
    int hix = std::max(sx, dx), hiy = std::max(sy, dy);
    int bx0 = lox < margin ? 0 : lox - margin;
    int by0 = loy < margin ? 0 : loy - margin;
    int bx1 = hix + margin < w ? hix + margin : w - 1;
    int by1 = hiy + margin < h ? hiy + margin : h - 1;
    int found = 1;
    bool going = true;
    while (going) {
        if (q.flood != kNone && q.ring == q.flood) break;
        bool any = false;
        int r = q.ring;
        int x0 = std::max({dx - r, 0, bx0}), x1 = std::min({dx + r, w - 1, bx1});
        int y0 = std::max({dy - r, 0, by0}), y1 = std::min({dy + r, h - 1, by1});
        for (int x = x0; x <= x1; x++) {
            for (int y = y0; y <= y1; y++) {
                int k = y * w + x;
                int v = dist[k];
                if (v < 1) {
                    any = true;
                    int cur = grid[k];
                    int nd = -v;
                    dist[k] = nd;
                    bool cur_water = Has(cur, kWaterF), cur_cross = Has(cur, kCrossF);
                    // the road builder spreads straight only (4125:00e2, class 9)
                    int cls = q.roads ? 9 : 0;
                    if (y == 0)
                        cls = x == 0 ? 1 : (x == w - 1 ? 3 : 2);
                    else if (y == h - 1)
                        cls = x == 0 ? 6 : (x == w - 1 ? 8 : 7);
                    else if (x == 0)
                        cls = 4;
                    else if (x == w - 1)
                        cls = 5;
                    int i = kNbFirst[cls];
                    while (kNbDx[i] != -10) {
                        int nx = x + kNbDx[i], ny = y + kNbDy[i];
                        i++;
                        int nk = ny * w + nx;
                        int nv = dist[nk];
                        if (nv != kShut) {
                            int nb = grid[nk];
                            int c = nb % 8;
                            bool ok = true;
                            bool n_water = Has(nb, kWaterF), n_cross = Has(nb, kCrossF);
                            if (nx == sx && ny == sy) {
                                if (mode == kLand && !cur_cross && !n_cross && n_water != cur_water)
                                    ok = false;
                                c = 1;
                            } else if (mode == kFlying) {
                                if ((!n_water || n_cross) && c != 0 && c < 3) {
                                    // as dear as the tile, where that is 1 or 2
                                } else {
                                    c = 2;
                                }
                            } else if (mode == kBoat) {
                                if (!n_water && !Has(nb, kCityF)) {
                                    dist[nk] = kShut;
                                    ok = false;
                                }
                            } else {
                                if (!cur_cross && !n_cross && n_water != cur_water) {
                                    ok = false;
                                } else {
                                    if (c > 2 && ((q.hills && Has(nb, kHillsF)) ||
                                                  (q.woods && Has(nb, kForestF))))
                                        c = 2;
                                    if ((!cur_water || cur_cross) && n_water && !n_cross)
                                        c += q.penalty;
                                }
                            }
                            if (ok && nd + c < std::abs(nv)) dist[nk] = v - c;
                        }
                    }
                    if (x == sx && y == sy && q.flood == kNone) going = false;
                }
            }
        }
        q.ring = r + 1;
        if (!any) {
            going = false;
            found = 0;
        }
    }
    return found;
}

// path_trace (1555:117b): from the start, each step to whichever neighbour
// the flood put nearest the destination; at most 198 steps.
Path Trace(Query& q) {
    int w = q.w, h = q.h;
    const std::vector<int>& dist = q.dist;
    const std::vector<int>& grid = *q.grid;
    int x = q.sx, y = q.sy;
    int tx = q.dx, ty = q.dy;
    int d = std::abs(dist[y * w + x]);
    Path steps;
    while (!(x == tx && y == ty)) {
        int cur = grid[y * w + x];
        bool land = q.mode == kLand;
        bool cur_water = land && Has(cur, kWaterF);
        bool cur_cross = land && Has(cur, kCrossF);
        int toward = Direction(x, y, tx, ty);
        int bx = kNone, by = kNone;
        bool got = false;
        for (int turn : kTraceOrder) {
            int dir = (toward + turn) % 8;
            int nx = x + kDirs[dir][0], ny = y + kDirs[dir][1];
            bool inside = nx >= 0 && ny >= 0 && nx < w && ny < h;
            // the road builder steps diagonally only over water that is no bridge
            if (q.roads && dir % 2 == 1 &&
                !(inside && Has(grid[ny * w + nx], kWaterF) && !q.is_bridge(nx, ny)))
                continue;
            if (inside) {
                int v = dist[ny * w + nx];
                if (v != kUnreached && v != kShut) {
                    int nb = grid[ny * w + nx];
                    bool blocked = land && !cur_cross && !Has(nb, kCrossF) &&
                                   Has(nb, kWaterF) != cur_water;
                    // ... and takes a first neighbour no nearer than here
                    bool tie = q.roads && !got && std::abs(v) == d;
                    if (!blocked && (std::abs(v) < d || tie)) {
                        bx = nx;
                        by = ny;
                        got = true;
                        d = std::abs(v);
                    }
                }
            }
        }
        if (got && steps.size() < 198) {
            x = bx;
            y = by;
            steps.push_back(Step{x, y, 0});
        } else {
            tx = x;
            ty = y;
        }
    }
    return steps;
}

}  // namespace

std::optional<std::vector<std::pair<int, int>>> RoadRoute(const RoadGround& m, const int costs[12],
                                                          int w, int h, int sx, int sy, int dx,
                                                          int dy) {
    if (dx < 0 || dy < 0 || dx >= w || dy >= h) return std::nullopt;
    std::vector<int> grd(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int t = m.terrain(x, y);
            int byte = m.road(x, y) ? 1 : costs[t];
            if (t == kBridge)
                byte |= kCrossF | kWaterF;
            else if (t == kWater || t == kShore)
                byte |= kWaterF;
            else if (t == kForest)
                byte |= kForestF;
            else if (t == kHills)
                byte |= kHillsF;
            else if (t == kCity)
                byte = kCityF;  // nobody's city is player 14's
            if (m.crossing(x, y)) byte |= kCrossF;
            grd[y * w + x] = byte;
        }
    }
    int to = m.terrain(dx, dy);
    Query q;
    q.w = w;
    q.h = h;
    q.sx = sx;
    q.sy = sy;
    q.dx = dx;
    q.dy = dy;
    q.grid = &grd;
    q.mode = kLand;
    q.woods = q.hills = false;
    q.penalty = to == kWater || to == kShore ? kWaterPenaltyTo : kWaterPenalty;
    q.dist.resize(static_cast<size_t>(w) * h);
    for (int k = 0; k < w * h; k++) q.dist[k] = grd[k] % 8 == 0 ? kShut : kUnreached;
    q.roads = true;
    q.is_bridge = [&m](int x, int y) { return m.terrain(x, y) == kBridge; };
    int goal = dy * w + dx;
    if (q.dist[goal] == kShut && !Has(grd[goal], kCityF)) return std::nullopt;
    q.dist[goal] = -1;
    if (Wavefront(q, 0) != 1) return std::nullopt;
    std::vector<std::pair<int, int>> out;
    for (const Step& st : Trace(q)) out.emplace_back(st.x, st.y);
    return out;
}

namespace {

// path_single_step (1555:020e): a destination one tile away is simply stepped
// to, unless a land or boat stack would cross between land and water, or the
// ground there is impassable.
bool SingleStep(const Game& g, int mode, int sx, int sy, int dx, int dy) {
    auto wet = [](int t) { return t == kWater || t == kShore; };
    int from = scn::TerrainAt(*g.map, sx, sy), to = scn::TerrainAt(*g.map, dx, dy);
    if (mode != kFlying) {
        if (wet(from) != wet(to)) return false;
        if (kCost[to] == 0) return false;
    }
    return true;
}
}  // namespace

namespace {
// A stack may always path *to* a city, just never through one it does not
// own (path_mark_cities, 1555:0bde): when (x, y) is a city tile the grid
// shuts, its four tiles are opened. Returns what to put back.
std::vector<std::pair<int, int>> OpenCityAt(Game& g, std::vector<int>& grd, int x0, int y0) {
    int w = g.map->width, h = g.map->height;
    std::vector<std::pair<int, int>> restore;
    int goal = y0 * w + x0;
    int goal_byte = grd[goal];
    City* city = g.map->city_tile[goal];
    if (city && goal_byte % 8 == 0 && Has(goal_byte, kCityF)) {
        bool port = IsPort(g, *city);
        for (int ox = 0; ox <= 1; ox++) {
            for (int oy = 0; oy <= 1; oy++) {
                int x = city->x + ox, y = city->y + oy;
                if (x < w && y < h) {
                    int k = y * w + x;
                    restore.emplace_back(k, grd[k]);
                    int byte = grd[k] - grd[k] % 8 + kCost[kCity];
                    if (port) byte |= kCrossF | kWaterF;
                    grd[k] = byte;
                }
            }
        }
    }
    return restore;
}

// The request's flags (stack_movement_mode, 1c8c:0529): 8 at sea, else for
// a land stack 1 for the hills bonus and 2 for the woods'.
int RouteFlags(int mode, bool woods, bool hills, bool at_sea) {
    if (at_sea) return 8;
    if (mode != kLand) return 0;
    return (hills ? 1 : 0) | (woods ? 2 : 0);
}

bool StepDir(int& x, int& y, int dir, int w, int h) {
    int nx = x + kDirs[dir][0], ny = y + kDirs[dir][1];
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return false;
    x = nx;
    y = ny;
    return true;
}

int DirOf(int dx, int dy) {
    for (int d = 0; d < 8; d++)
        if (kDirs[d][0] == dx && kDirs[d][1] == dy) return d;
    return 0;
}

// A cached route the request can use (1555:1595 / 165c): same destination,
// mode and flags, with the start somewhere on it before its end. Returns the
// rest of it as directions, or none.
std::optional<std::vector<int>> CacheHit(const Game& g, const std::optional<CachedRoute>& e, int sx,
                                         int sy, int dx, int dy, int mode, int flags) {
    if (!e || e->dx != dx || e->dy != dy || e->mode != mode || e->flags != flags)
        return std::nullopt;
    int x = e->sx, y = e->sy;
    for (size_t i = 0; i < e->dirs.size(); i++) {
        if (x == sx && y == sy) return std::vector<int>(e->dirs.begin() + i, e->dirs.end());
        if (!StepDir(x, y, e->dirs[i], g.map->width, g.map->height)) return std::nullopt;
    }
    return std::nullopt;
}

// Keep a route found (1555:16f0): a short one in slot 0; a long one in the
// first free slot of 1-19, or over the one whose own route is shortest when
// that is shorter than this one.
void CacheStore(Game& g, int d, int sx, int sy, int dx, int dy, int mode, int flags,
                const std::vector<int>& dirs) {
    auto& c = g.path_cache->slots;
    int slot = -1;
    if (d < 15) {
        slot = 0;
    } else {
        int shortest = -1, least = 10000;
        for (int i = 1; i < kCacheSlots; i++) {
            const auto& e = c[i];
            if (!e) {
                slot = i;
                break;
            }
            int ed = Distance(e->dx, e->dy, e->sx, e->sy);
            if (ed < least) {
                shortest = i;
                least = ed;
            }
        }
        if (slot == -1 && shortest != -1 && least < d) slot = shortest;
    }
    if (slot != -1) c[slot] = CachedRoute{mode, flags, sx, sy, dx, dy, dirs};
}
}  // namespace

std::shared_ptr<PathCache> NewPathCache() {
    return std::make_shared<PathCache>();
}

std::optional<Path> FindPath(Game& g, const Stack& stack, int sx, int sy, int dx, int dy) {
    if (sx == dx && sy == dy) return Path{};
    int w = g.map->width, h = g.map->height;
    if (dx < 0 || dy < 0 || dx >= w || dy >= h) return std::nullopt;

    int side = stack.empty() ? kNone : stack[0]->owner;
    Mode m = StackMode(g, stack);
    std::vector<int>& grd = Grid(g, side);
    int flags = RouteFlags(m.mode, m.woods, m.hills, m.at_sea);
    int d = Distance(dx, dy, sx, sy);

    std::optional<Path> path;
    std::optional<std::vector<int>> dirs;
    if (g.path_cache) {
        dirs = CacheHit(g, g.path_cache->slots[0], sx, sy, dx, dy, m.mode, flags);
        for (int i = 1; !dirs && d > 14 && i < kCacheSlots; i++) {
            dirs = CacheHit(g, g.path_cache->slots[i], sx, sy, dx, dy, m.mode, flags);
        }
    }
    if (dirs) {
        path = Path{};
        int x = sx, y = sy;
        for (int dir : *dirs) {
            StepDir(x, y, dir, w, h);
            path->push_back(Step{x, y, 0});
        }
    } else if (d == 1 && SingleStep(g, m.mode, sx, sy, dx, dy)) {
        path = Path{Step{dx, dy, 0}};
    } else {
        int goal = dy * w + dx;
        auto restore = OpenCityAt(g, grd, dx, dy);
        int penalty = 0;
        if (m.mode == kLand) {
            int t = scn::TerrainAt(*g.map, dx, dy);
            penalty = (t == kWater || t == kShore) ? kWaterPenaltyTo : kWaterPenalty;
        }
        Query q{w,    h,      sx,      sy,      dx,      dy,
                &grd, m.mode, m.woods, m.hills, penalty, Prepare(g, grd, side, m.mode, dx, dy),
                0,    false,  nullptr};
        int found = 0;
        if (q.dist[goal] != kShut || Has(grd[goal], kCityF)) {
            q.dist[goal] = -1;
            for (int pass = 0; pass <= 1; pass++) {
                if (found == 0) found = Wavefront(q, pass);
            }
        }
        if (found == 1) {
            path = Trace(q);
            if (g.path_cache) {
                std::vector<int> ds;
                int px = sx, py = sy;
                for (const Step& st : *path) {
                    ds.push_back(DirOf(st.x - px, st.y - py));
                    px = st.x;
                    py = st.y;
                }
                CacheStore(g, d, sx, sy, dx, dy, m.mode, flags, ds);
            }
        }
        for (auto& [k, byte] : restore) grd[k] = byte;
    }

    // what the walk spends on each step (path_step_costs, 1555:18be)
    if (path) {
        bool ashore = false;
        for (Step& step : *path) {
            int byte = grd[step.y * w + step.x];
            if (ashore) {
                step.cost = kPastShore;
            } else {
                step.cost = WalkCost(byte, m.mode, m.woods, m.hills);
                ashore = m.mode == kLand && CrossesShore(byte, m.at_sea);
            }
        }
    }
    if (path && path->empty()) path.reset();
    return path;
}

std::vector<int> Flood(Game& g, int grid_side, int x, int y, int range, int mode, bool woods,
                       bool hills, int fog_side) {
    int w = g.map->width, h = g.map->height;
    std::vector<int>& grd = Grid(g, grid_side);
    int goal = y * w + x;
    auto restore = OpenCityAt(g, grd, x, y);
    int penalty = 0;
    if (mode == kLand) {
        int t = scn::TerrainAt(*g.map, x, y);
        penalty = (t == kWater || t == kShore) ? kWaterPenaltyTo : kWaterPenalty;
    }
    Query q{w,    h,     x,      y,     x,       y,
            &grd, mode,  woods,  hills, penalty, Prepare(g, grd, fog_side, mode, x, y),
            0,    false, nullptr};
    q.flood = range;
    if (q.dist[goal] != kShut || Has(grd[goal], kCityF)) {
        q.dist[goal] = -1;
        if (Wavefront(q, 0) == 0) Wavefront(q, 1);
    }
    for (auto& [k, byte] : restore) grd[k] = byte;
    return q.dist;
}

std::string SettleSea(Game& g, const Stack& stack, int x, int y, int mode, bool was_at_sea) {
    if (mode != kLand) return "";
    int t = scn::TerrainAt(*g.map, x, y);
    if (!was_at_sea) {
        if (t != kWater && t != kShore) return "";
        bool flier = false, walker = false;
        for (Army* a : stack) {
            if (g.types.ById(a->type)->flies)
                flier = true;
            else if (a->type != armytype::kHero)
                walker = true;
        }
        if (walker) flier = false;
        std::string change;
        for (Army* a : stack) {
            if (!g.types.ById(a->type)->flies && (a->type != armytype::kHero || !flier)) {
                a->at_sea = true;
                change = "to sea";
            }
        }
        return change;
    } else if (t != kWater && t != kShore && t != kBridge) {
        for (Army* a : stack) a->at_sea = false;
        return "ashore";
    }
    return "";
}

namespace {
// Does the walk leave the stack out of moves (walk_path's status 2,
// 1a8b:09d1)? With `m` the moves it has left: m below 2; or, having walked,
// stopped by neither an enemy city nor a stack at war, m below the cost of
// the route **from its start** to the step after where it stopped (the
// table path_step_costs, 1555:18be, keeps, 255 at most; none past the
// route's end). A human's stack out of moves is done for the turn and put
// down (1c8c:01fd). `landed` counts the steps to where it stopped.
bool OutOfMoves(const Stack& stack, const Path& path, int landed, const std::string& stopped) {
    int m = StackMoves(stack);
    if (m < 2) return true;
    if (landed == 0 || stopped == "attack" || stopped == "at peace") return false;
    if (landed >= static_cast<int>(path.size())) return false;
    int total = 0;
    for (int i = 0; i <= landed; i++) total += path[i].cost;
    if (total >= 255) return false;
    return m < total;
}

// Walk `stack` along `path`, stopping where the rules say to stop.
WalkResult WalkPath(Game& g, const Stack& stack, const Path& path) {
    int side = stack.empty() ? kNone : stack[0]->owner;
    int left = StackMoves(stack);
    WalkResult result;
    result.stopped = "arrived";
    for (Army* a : stack)
        if (a->at_sea) result.sea_before = true;
    int last_ok = 0, cumulative = 0;
    std::map<int, int> cost_to;

    for (int i = 1; i <= static_cast<int>(path.size()); i++) {
        const Step& step = path[i - 1];
        // a step is taken while what the walk has cost so far fits in the moves
        // left (1555:18be): there is no floor, so one move still buys a road
        if (step.cost > left) {
            result.stopped = "out of moves";
            break;
        }
        City* city = g.map->city_tile[step.y * g.map->width + step.x];
        auto here = game::ArmiesAt(g, step.x, step.y);
        if (city && !city->razed && city->owner_index != side) {
            if (!diplomacy::MayAttack(g, side, city->owner_index)) {
                result.stopped = "at peace";
                break;
            }
            result.stopped = "attack";
            result.attack = Attack{step.x, step.y, city};
            break;
        }
        bool theirs = !here.empty() && here[0]->owner != side;
        if (theirs && diplomacy::MayAttack(g, side, here[0]->owner)) {
            result.stopped = "attack";
            result.attack = Attack{step.x, step.y, nullptr};
            break;
        }
        left -= step.cost;
        cumulative += step.cost;
        // the stack may only stop on a tile of its own where it fits; other
        // steps -- a side at peace's stack among them -- are passed over
        // (1a8b:07f9)
        if (!theirs && static_cast<int>(here.size() + stack.size()) <= rules::kMaxStack) {
            last_ok = i;
            cost_to[i] = cumulative;
        }
    }

    if (last_ok == 0) {
        if (result.stopped == "arrived") result.stopped = "blocked";
        result.out_of_moves = OutOfMoves(stack, path, 0, result.stopped);
        return result;
    }

    int cost = cost_to[last_ok];
    const Step& dest = path[last_ok - 1];
    Mode m = StackMode(g, stack);
    for (Army* a : stack) {
        a->x = dest.x;
        a->y = dest.y;
        a->moves = std::max(0, a->moves - cost);
    }
    std::string change = SettleSea(g, stack, dest.x, dest.y, m.mode, m.at_sea);
    if (!change.empty()) {
        for (Army* a : stack) {
            if (a->at_sea == (change == "to sea")) a->moves = 0;
        }
        result.stopped = "out of moves";
        result.attack.reset();
    }

    // walking uncovers the map as it goes
    if (g.map->options.hidden_map != 0 && side != kNone) {
        bool flying = ModeOf(g, stack) == kFlying;
        int found = 0;
        for (int i = 0; i < static_cast<int>(path.size()); i++) {
            if (i == last_ok - 1) break;
            found += game::Reveal(g, side, path[i].x, path[i].y, flying);
        }
        found += game::Reveal(g, side, dest.x, dest.y, flying);
        if (found > 0) Invalidate(g);
    }

    result.steps = last_ok;
    result.spent = cost;
    for (int i = 0; i < last_ok; i++) result.walked.emplace_back(path[i].x, path[i].y);
    if (last_ok < static_cast<int>(path.size()) && result.stopped == "arrived")
        result.stopped = "blocked";
    // going to sea or coming ashore ends the move as running out does
    result.out_of_moves = !change.empty() || OutOfMoves(stack, path, last_ok, result.stopped);
    return result;
}
}  // namespace

WalkResult Walk(Game& g, const Stack& stack, const Path& path) {
    game::TidyTowers(g);
    WalkResult result = WalkPath(g, stack, path);
    game::TidyTowers(g);
    return result;
}

WalkResult MoveTo(Game& g, const Stack& stack, int x, int y) {
    WalkResult r;
    if (stack.empty()) {
        r.stopped = "blocked";
        return r;
    }
    auto path = FindPath(g, stack, stack[0]->x, stack[0]->y, x, y);
    if (!path) {
        r.stopped = "no route";
        return r;
    }
    if (path->empty()) {
        r.stopped = "arrived";
        return r;
    }
    return Walk(g, stack, *path);
}

}  // namespace move
}  // namespace w2
