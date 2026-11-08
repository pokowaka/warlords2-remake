#include "warlords/mapeval.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <regex>
#include <set>

#include "util/util.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/travel.hpp"

namespace w2::mapeval {

namespace {

using namespace w2::move;  // the terrain type ids

constexpr int kN = 1, kE = 2, kS = 4, kW = 8;
constexpr int kDX4[4] = {0, 1, 0, -1}, kDY4[4] = {-1, 0, 1, 0};
constexpr int kBiT4[4] = {kN, kE, kS, kW};
// What each road piece joins (docs/formats/map.md > .RD): piece n, n = 1..17.
constexpr int kRoadShape[18] = {0,
                                kE | kW,
                                kN | kS,
                                kN | kE | kS | kW,
                                kE | kS | kW,
                                kN | kS | kW,
                                kN | kE | kW,
                                kN | kE | kS,
                                kS | kW,
                                kN | kW,
                                kN | kE,
                                kE | kS,
                                kW,
                                kS,
                                kE,
                                kN,
                                kE | kW,
                                kN | kS};
const char* const kCompass[8] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};

// A city may not come closer than this to another: the tiles between the two
// footprints. The hand-made scenarios never go under 1 (see docs/mapeval.md).
constexpr int kMinCityGap = 1;
// A city this close (tiles between footprints) to another counts as crowded.
constexpr int kCrowdedGap = 2;
// Two capitals may not be closer than this, in turns of travel. The hand-made
// scenarios never come under 2.5 (Erythea); this is about 60% of that.
constexpr double kCapitalGapTurns = 1.5;
// A neutral city further than this from every capital is out in the wilds.
constexpr int kRemoteTurns = 5;

double Quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double at = q * static_cast<double>(v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(at)), hi = static_cast<size_t>(std::ceil(at));
    return v[lo] + (v[hi] - v[lo]) * (at - static_cast<double>(lo));
}

/** The map, read once into flat grids the checks share. */
struct View {
    const Map& m;
    int w, h;
    std::vector<int> terrain, road;          // terrain type, road piece (0 none)
    std::vector<int> city_of;                // city index on each footprint tile, or -1
    std::vector<bool> site, crossing, port;  // port: a footprint tile of a port city

    explicit View(const Map& map) : m(map), w(map.width), h(map.height) {
        int n = w * h;
        terrain.resize(n);
        road.resize(n);
        city_of.assign(n, -1);
        site.assign(n, false);
        crossing.assign(n, false);
        port.assign(n, false);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int i = y * w + x;
                terrain[i] = scn::terrainAt(m, x, y);
                int r = scn::roadAt(m, x, y) % 32;
                road[i] = r <= 17 ? r : 0;
                crossing[i] = scn::isCrossing(m, x, y);
            }
        }
        for (const City& c : m.cities) {
            bool is_port = false;
            for (int dy = -1; dy <= 2 && !is_port; dy++)
                for (int dx = -1; dx <= 2 && !is_port; dx++) {
                    if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1) continue;
                    int t = TerrainOr(c.x + dx, c.y + dy, -1);
                    is_port = t == BRIDGE || t == WATER || t == SHORE;
                }
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    if (!In(c.x + dx, c.y + dy)) continue;
                    int i = (c.y + dy) * w + c.x + dx;
                    city_of[i] = c.index;
                    port[i] = is_port;
                }
        }
        for (const Site& s : m.sites)
            if (In(s.x, s.y)) site[s.y * w + s.x] = true;
    }
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int TerrainOr(int x, int y, int dflt) const { return In(x, y) ? terrain[y * w + x] : dflt; }
    bool Sea(int i) const { return terrain[i] == WATER || terrain[i] == SHORE; }
    /** Road, or a bridge: what a road network is made of. */
    bool Roadway(int i) const { return road[i] != 0 || terrain[i] == BRIDGE; }
    /** What a road may lead to: a city, a site, a signpost or a crossing. */
    bool Destination(int i) const {
        return city_of[i] >= 0 || site[i] || terrain[i] == TOWER || crossing[i];
    }
    /** Is a destination on this tile or any of its 8 neighbours? */
    bool DestinationNear(int x, int y) const {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (In(x + dx, y + dy) && Destination((y + dy) * w + x + dx)) return true;
        return false;
    }
};

struct Context {
    const View& v;
    Evaluation& e;
    void AddIssue(const char* check, int x, int y, const std::string& detail) {
        e.issues.push_back({check, x, y, detail});
    }
    void Metric(const char* key, double value) { e.metrics.emplace_back(key, value); }
};

// ---------------------------------------------------------------- cities

/** Tiles between two footprints along x and along y (0 when touching). */
std::pair<int, int> Gaps(const City& a, const City& b) {
    int gx = std::max(0, std::abs(a.x - b.x) - 2), gy = std::max(0, std::abs(a.y - b.y) - 2);
    return {gx, gy};
}
bool Overlap(const City& a, const City& b) {
    return std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
}

void Cities(Context& cx) {
    const Map& m = cx.v.m;
    const View& v = cx.v;
    int n = static_cast<int>(m.cities.size());
    std::vector<double> nearest_gap, nearest_dist;
    int crowded = 0;
    std::map<std::string, int> names;
    for (const City& c : m.cities) {
        if (names[c.name]++ == 1)
            cx.AddIssue("city-name", c.x, c.y, "two or more cities are called " + c.name);
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) {
                int t = v.TerrainOr(c.x + dx, c.y + dy, -1);
                if (t != CITY) {
                    cx.AddIssue("city-terrain", c.x + dx, c.y + dy,
                                c.name + "'s footprint stands on " +
                                        (t >= 0 ? TERRAIN_NAMES[t] : "the map's edge"));
                }
            }
        int best = std::numeric_limits<int>::max();
        double best_dist = std::numeric_limits<double>::max();
        for (const City& o : m.cities) {
            if (&o == &c) continue;
            auto [gx, gy] = Gaps(c, o);
            int gap = Overlap(c, o) ? -1 : std::max(gx, gy);
            best = std::min(best, gap);
            best_dist = std::min(best_dist, std::hypot(c.x - o.x, c.y - o.y));
            if (c.index < o.index && gap < kMinCityGap) {
                cx.AddIssue("city-gap", c.x, c.y,
                            c.name + " and " + o.name +
                                    (gap < 0 ? " overlap"
                                             : " are " + std::to_string(gap) + " tile(s) apart"));
            }
        }
        if (n > 1) {
            nearest_gap.push_back(best);
            nearest_dist.push_back(best_dist);
            if (best <= kCrowdedGap) crowded++;
        }
    }
    // Clark-Evans: the mean nearest-neighbour distance over what random points
    // on the same land would give; < 1 clustered, 1 random, > 1 evenly spread
    int land = 0;
    for (int i = 0; i < v.w * v.h; i++) land += !v.Sea(i);
    double mean = 0;
    for (double d : nearest_dist) mean += d;
    mean = nearest_dist.empty() ? 0 : mean / static_cast<double>(nearest_dist.size());
    double expected = n > 0 && land > 0 ? 0.5 / std::sqrt(static_cast<double>(n) / land) : 0;
    cx.Metric("cities", n);
    cx.Metric("cityGapMin", Quantile(nearest_gap, 0));
    cx.Metric("cityGapP5", Quantile(nearest_gap, 0.05));
    cx.Metric("cityGapMedian", Quantile(nearest_gap, 0.5));
    cx.Metric("crowdedCityFrac", n ? static_cast<double>(crowded) / n : 0);
    cx.Metric("clarkEvansR", expected > 0 ? mean / expected : 0);
}

// ---------------------------------------------------------------- roads

void Roads(Context& cx) {
    const View& v = cx.v;
    const Map& m = v.m;
    int w = v.w, h = v.h;
    int road_tiles = 0, pieces = 0, mismatched = 0, dead_ends = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (!v.road[i]) continue;
            pieces++;
            // the piece's own arms against what lies beyond them: an arm must meet
            // a roadway or a place, and a road piece pointing back needs an arm
            int shape = kRoadShape[v.road[i]], joined = 0;
            bool bad = false;
            for (int d = 0; d < 4; d++) {
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!v.In(nx, ny)) continue;
                int j = ny * w + nx;
                if (v.Roadway(j) || v.Destination(j)) joined |= kBiT4[d];
                bool back = v.road[j] && (kRoadShape[v.road[j]] & kBiT4[(d + 2) % 4]);
                if (back && !(shape & kBiT4[d])) bad = true;
            }
            if (shape & ~joined) bad = true;
            if (bad) mismatched++;
            for (int d = 0; d < 4; d++) {
                if (!(shape & kBiT4[d]) || (joined & kBiT4[d])) continue;
                if (v.DestinationNear(x, y)) continue;
                dead_ends++;
                cx.AddIssue("dead-end", x, y,
                            std::string("the road's ") + "NESW"[d] + " arm leads nowhere");
            }
        }
    }
    // the networks: 4-connected roads and bridges, and what each one reaches
    std::vector<int> comp(static_cast<size_t>(w) * h, -1);
    int comps = 0, stray = 0, spurs = 0, main_size = 0;
    std::vector<std::set<int>> comp_cities;
    for (int s = 0; s < w * h; s++) {
        if (!v.Roadway(s) || comp[s] >= 0) continue;
        std::set<int> reached;
        std::set<int> others;  // sites, signposts and crossings, by tile
        int size = 0;
        std::deque<int> q{s};
        comp[s] = comps;
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            size++;
            int x = i % w, y = i / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!v.In(x + dx, y + dy)) continue;
                    int j = (y + dy) * w + x + dx;
                    if (v.city_of[j] >= 0) reached.insert(v.city_of[j]);
                    if (v.site[j] || v.crossing[j] || v.terrain[j] == TOWER) others.insert(j);
                }
            for (int d = 0; d < 4; d++) {
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!v.In(nx, ny)) continue;
                int j = ny * w + nx;
                if (v.Roadway(j) && comp[j] < 0) {
                    comp[j] = comps;
                    q.push_back(j);
                }
            }
        }
        road_tiles += size;
        int ends = static_cast<int>(reached.size()) + static_cast<int>(others.size());
        if (ends == 0) {
            stray++;
            cx.AddIssue("road-to-nowhere", s % w, s / w,
                        "a road of " + std::to_string(size) + " tile(s) reaching no place");
        } else if (ends == 1) {
            spurs++;
            cx.AddIssue("spur-road", s % w, s / w,
                        "a road of " + std::to_string(size) + " tile(s) reaching one place only");
        }
        if (static_cast<int>(reached.size()) > main_size)
            main_size = static_cast<int>(reached.size());
        comp_cities.push_back(reached);
        comps++;
    }
    int n = static_cast<int>(m.cities.size());
    std::vector<bool> on_road(n, false);
    for (auto& r : comp_cities)
        for (int c : r) on_road[c] = true;
    int linked = static_cast<int>(std::count(on_road.begin(), on_road.end(), true));

    // detours: from each city along its roads to the first other cities met
    std::vector<double> detours;
    for (const City& c : m.cities) {
        std::vector<int> dist(static_cast<size_t>(w) * h, -1);
        std::deque<int> q;
        for (int dy = -1; dy <= 2; dy++)
            for (int dx = -1; dx <= 2; dx++) {
                int x = c.x + dx, y = c.y + dy;
                if (!v.In(x, y)) continue;
                int i = y * w + x;
                if (v.Roadway(i) && v.city_of[i] < 0) {
                    dist[i] = 1;
                    q.push_back(i);
                }
            }
        std::set<int> met;
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            int x = i % w, y = i / w;
            int other = -1;
            for (int dy = -1; dy <= 1 && other < 0; dy++)
                for (int dx = -1; dx <= 1 && other < 0; dx++)
                    if (v.In(x + dx, y + dy)) {
                        int k = v.city_of[(y + dy) * w + x + dx];
                        if (k >= 0 && k != c.index) other = k;
                    }
            if (other >= 0) {
                if (met.insert(other).second) {
                    auto [gx, gy] = Gaps(c, m.cities[other]);
                    detours.push_back(static_cast<double>(dist[i]) / std::max(1, gx + gy));
                }
                continue;  // a road through a city is two roads
            }
            for (int d = 0; d < 4; d++) {
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!v.In(nx, ny)) continue;
                int j = ny * w + nx;
                if (v.Roadway(j) && dist[j] < 0 && v.city_of[j] < 0) {
                    dist[j] = dist[i] + 1;
                    q.push_back(j);
                }
            }
        }
    }
    cx.Metric("roadTiles", road_tiles);
    cx.Metric("roadNetworks", comps);
    cx.Metric("roadsToNowhere", stray);
    cx.Metric("spurRoads", spurs);
    cx.Metric("spurRoadsPerCity", n ? static_cast<double>(spurs) / n : 0);
    cx.Metric("deadEnds", dead_ends);
    cx.Metric("deadEndsPer100Road", road_tiles ? 100.0 * dead_ends / road_tiles : 0);
    cx.Metric("roadShapeMismatchFrac", pieces ? static_cast<double>(mismatched) / pieces : 0);
    cx.Metric("cityRoadCoverage", n ? static_cast<double>(linked) / n : 0);
    cx.Metric("mainNetworkCityShare", n ? static_cast<double>(main_size) / n : 0);
    cx.Metric("roadTilesPerCity", n ? static_cast<double>(road_tiles) / n : 0);
    cx.Metric("roadDetourMedian", Quantile(detours, 0.5));
    cx.Metric("roadDetourP95", Quantile(detours, 0.95));
}

// ---------------------------------------------------------------- reach

/** Every tile a stack can get to from (x, y): mountains are shut; it changes
 *  between land and sea only through a crossing (a bridge, a marked tile, a
 *  port city's footprint) -- docs/rules.md > Movement. Land only: no sea. */
std::vector<bool> Reach(const View& v, int x0, int y0, bool land_only) {
    int w = v.w, h = v.h;
    std::vector<bool> seen(static_cast<size_t>(w) * h, false);
    // a road over mountains is a pass (18a9:0896): any road tile costs 1
    auto open = [&](int i) {
        return (v.terrain[i] != MOUNTAINS || v.road[i]) && !(land_only && v.Sea(i));
    };
    auto crossing = [&](int i) { return v.crossing[i] || v.port[i] || v.terrain[i] == BRIDGE; };
    int s = y0 * w + x0;
    if (!v.In(x0, y0) || !open(s)) return seen;
    std::deque<int> q{s};
    seen[s] = true;
    while (!q.empty()) {
        int i = q.front();
        q.pop_front();
        int x = i % w, y = i / w;
        for (int d = 0; d < 8; d++) {
            int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
            if (!v.In(nx, ny)) continue;
            int j = ny * w + nx;
            if (seen[j] || !open(j)) continue;
            if (v.Sea(i) != v.Sea(j) && !crossing(i) && !crossing(j)) continue;
            seen[j] = true;
            q.push_back(j);
        }
    }
    return seen;
}

void Reachability(Context& cx) {
    const View& v = cx.v;
    const Map& m = v.m;
    int w = v.w;
    std::vector<const City*> capitals;
    for (const Side& s : m.sides)
        if (s.capital) capitals.push_back(s.capital);
    double land_share = 0;
    std::set<int> lost_cities, lost_sites;
    for (const City* cap : capitals) {
        auto all = Reach(v, cap->x, cap->y, false);
        auto land = Reach(v, cap->x, cap->y, true);
        int by_land = 0;
        for (const City& c : m.cities) {
            if (land[c.y * w + c.x]) by_land++;
            if (!all[c.y * w + c.x] && lost_cities.insert(c.index).second)
                cx.AddIssue("unreachable", c.x, c.y,
                            c.name + " cannot be reached from " + cap->name);
        }
        for (const Site& s : m.sites)
            if (!all[s.y * w + s.x] && lost_sites.insert(s.index).second)
                cx.AddIssue("unreachable", s.x, s.y,
                            s.name + " cannot be reached from " + cap->name);
        land_share += m.cities.empty() ? 0 : static_cast<double>(by_land) / m.cities.size();
    }
    cx.Metric("capitals", capitals.size());
    cx.Metric("landReachShare", capitals.empty() ? 0 : land_share / capitals.size());
}

// ---------------------------------------------------------------- fairness

double Gini(std::vector<double> v) {
    if (v.size() < 2) return 0;
    std::sort(v.begin(), v.end());
    double sum = 0, weighted = 0;
    for (size_t i = 0; i < v.size(); i++) {
        sum += v[i];
        weighted += static_cast<double>(i + 1) * v[i];
    }
    if (sum <= 0) return 0;
    double n = static_cast<double>(v.size());
    return (2 * weighted) / (n * sum) - (n + 1) / n;
}

/** Each side's start, by travel cost from its capital: how near its rivals
 *  are, and what it can expand into -- the neutral cities it reaches before
 *  anyone else does (a Voronoi diagram by travel cost), their income, and
 *  how many lie within a few turns. */
void Fairness(Context& cx) {
    const View& v = cx.v;
    const Map& m = v.m;
    int w = v.w, n = w * v.h;
    travel::Ground ground;
    ground.w = w;
    ground.h = v.h;
    ground.terrain = v.terrain;
    ground.road.resize(n);
    ground.crossing.resize(n);
    for (int i = 0; i < n; i++) {
        ground.road[i] = v.road[i] != 0;
        ground.crossing[i] = v.crossing[i] || v.port[i] || v.terrain[i] == BRIDGE;
    }
    auto footprint = [&](const City& c) {
        std::vector<int> t;
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                if (v.In(c.x + dx, c.y + dy)) t.push_back((c.y + dy) * w + c.x + dx);
        return t;
    };
    auto cost_to = [&](const std::vector<int>& field, const City& c) {
        int best = travel::UNREACHED;
        for (int t : footprint(c)) best = std::min(best, field[t]);
        return best;
    };
    std::vector<const City*> caps;
    for (const Side& s : m.sides)
        if (s.capital) caps.push_back(s.capital);
    int k = static_cast<int>(caps.size());
    if (k < 2) return;
    std::vector<std::vector<int>> field(k);
    for (int i = 0; i < k; i++) field[i] = travel::costs(ground, footprint(*caps[i]));

    // the nearest rival capital, in turns
    std::vector<double> gaps;
    for (int i = 0; i < k; i++) {
        int best = travel::UNREACHED;
        for (int j = 0; j < k; j++)
            if (j != i) best = std::min(best, cost_to(field[i], *caps[j]));
        if (best == travel::UNREACHED) continue;
        double turns = static_cast<double>(best) / travel::TURN;
        gaps.push_back(turns);
        for (int j = i + 1; j < k; j++) {
            int c = cost_to(field[i], *caps[j]);
            if (c != travel::UNREACHED && static_cast<double>(c) / travel::TURN < kCapitalGapTurns)
                cx.AddIssue("capital-gap", caps[i]->x, caps[i]->y,
                            caps[i]->name + " and " + caps[j]->name + " are " +
                                    fmt("%.1f", static_cast<double>(c) / travel::TURN) +
                                    " turns apart");
        }
    }
    // the neutral cities each side reaches first, and those within reach
    std::vector<double> cities(k, 0), income(k, 0), near2(k, 0), near4(k, 0), hinterland(k, 0);
    int neutral = 0, remote = 0;
    for (const City& c : m.cities) {
        if (std::find(caps.begin(), caps.end(), &c) != caps.end()) continue;
        neutral++;
        int best = travel::UNREACHED, second = travel::UNREACHED, who = -1;
        for (int i = 0; i < k; i++) {
            int d = cost_to(field[i], c);
            if (d < best) {
                second = best;
                best = d;
                who = i;
            } else if (d < second)
                second = d;
            if (d <= 2 * travel::TURN) near2[i]++;
            if (d <= 4 * travel::TURN) near4[i]++;
        }
        if (who < 0) continue;
        cities[who]++;
        income[who] += c.income;
        if (best > kRemoteTurns * travel::TURN) remote++;
        // a city nobody else can contest: the next side is twice as far, and
        // at least two turns further
        if (second == travel::UNREACHED ||
            (second >= 2 * best && second - best >= 2 * travel::TURN))
            hinterland[who]++;
    }
    double fair = neutral > 0 ? static_cast<double>(neutral) / k : 1;
    cx.Metric("capitalGapMinTurns", gaps.empty() ? 0 : Quantile(gaps, 0));
    cx.Metric("capitalGapMedianTurns", Quantile(gaps, 0.5));
    cx.Metric("expansionMinShare", *std::min_element(cities.begin(), cities.end()) / fair);
    cx.Metric("expansionMaxShare", *std::max_element(cities.begin(), cities.end()) / fair);
    cx.Metric("expansionGini", Gini(cities));
    cx.Metric("incomeGini", Gini(income));
    cx.Metric("near2MinShare", *std::min_element(near2.begin(), near2.end()) / fair);
    cx.Metric("near4MinShare", *std::min_element(near4.begin(), near4.end()) / fair);
    cx.Metric("hinterlandMaxShare", *std::max_element(hinterland.begin(), hinterland.end()) / fair);
    cx.Metric("remoteCityFrac", neutral ? static_cast<double>(remote) / neutral : 0);
}

// ---------------------------------------------------------------- terrain

void Terrain(Context& cx) {
    const View& v = cx.v;
    const Map& m = v.m;
    int w = v.w, h = v.h, n = w * h;
    int land = 0, coast = 0;
    for (int i = 0; i < n; i++) {
        if (v.Sea(i)) continue;
        land++;
        int x = i % w, y = i / w;
        for (int d = 0; d < 4; d++) {
            int t = v.TerrainOr(x + kDX4[d], y + kDY4[d], WATER);
            if (t == WATER || t == SHORE) {
                coast++;
                break;
            }
        }
    }
    // landmasses: 8-connected land, crossing bridges, and the cities on each
    std::vector<int> mass(n, -1);
    std::vector<int> mass_cities;
    for (int s = 0; s < n; s++) {
        if (v.Sea(s) || mass[s] >= 0) continue;
        int id = static_cast<int>(mass_cities.size());
        mass_cities.push_back(0);
        std::deque<int> q{s};
        mass[s] = id;
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            int x = i % w, y = i / w;
            for (int d = 0; d < 8; d++) {
                int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
                if (!v.In(nx, ny)) continue;
                int j = ny * w + nx;
                bool through = !v.Sea(j) || v.terrain[j] == BRIDGE;
                if (through && mass[j] < 0) {
                    mass[j] = id;
                    q.push_back(j);
                }
            }
        }
    }
    for (const City& c : m.cities)
        if (v.In(c.x, c.y) && mass[c.y * w + c.x] >= 0) mass_cities[mass[c.y * w + c.x]]++;
    int inhabited = 0, biggest = 0;
    for (int k : mass_cities) {
        inhabited += k > 0;
        biggest = std::max(biggest, k);
    }

    // patches: how lumpy forest, hills, mountains and marsh are
    auto patches = [&](int type, double& median, int& lone, int& total) {
        std::vector<bool> seen(n, false);
        std::vector<double> sizes;
        for (int s = 0; s < n; s++) {
            if (v.terrain[s] != type || seen[s]) continue;
            int size = 0;
            std::deque<int> q{s};
            seen[s] = true;
            while (!q.empty()) {
                int i = q.front();
                q.pop_front();
                size++;
                int x = i % w, y = i / w;
                for (int d = 0; d < 8; d++) {
                    int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
                    if (!v.In(nx, ny)) continue;
                    int j = ny * w + nx;
                    if (v.terrain[j] == type && !seen[j]) {
                        seen[j] = true;
                        q.push_back(j);
                    }
                }
            }
            sizes.push_back(size);
            total += size;
            if (size == 1) lone++;
        }
        median = Quantile(sizes, 0.5);
    };
    int lone = 0, total = 0;
    double forest = 0, hills = 0, mountains = 0, marsh = 0;
    patches(FOREST, forest, lone, total);
    patches(HILLS, hills, lone, total);
    patches(MOUNTAINS, mountains, lone, total);
    patches(MARSH, marsh, lone, total);

    cx.Metric("landFrac", static_cast<double>(land) / n);
    cx.Metric("coastComplexity", land ? coast / std::sqrt(static_cast<double>(land)) : 0);
    cx.Metric("inhabitedLandmasses", inhabited);
    cx.Metric("biggestLandmassCityShare",
              m.cities.empty() ? 0 : static_cast<double>(biggest) / m.cities.size());
    cx.Metric("forestPatchMedian", forest);
    cx.Metric("hillsPatchMedian", hills);
    cx.Metric("mountainPatchMedian", mountains);
    cx.Metric("marshPatchMedian", marsh);
    cx.Metric("speckleFrac", total ? static_cast<double>(lone) / total : 0);
}

// ---------------------------------------------------------------- signs, ports

/** 828e:0b51, as the generator gives a signpost's direction. */
int Compass(int x1, int y1, int x2, int y2) {
    if (x1 == x2) return y1 < y2 ? 4 : 0;
    if (y1 == y2) return x1 < x2 ? 2 : 6;
    if (x2 < x1 && y2 < y1) return 7;
    if (x2 < x1 && y1 < y2) return 5;
    if (x1 < x2 && y2 < y1) return 1;
    return 3;
}

void SignsAndPorts(Context& cx) {
    const View& v = cx.v;
    const Map& m = v.m;
    static const std::regex kLeagues("^\\s*(\\d+) leagues (\\w+)\\s*$");
    int pointing = 0;
    for (const Sign& s : m.signs) {
        if (v.TerrainOr(s.x, s.y, -1) != TOWER)
            cx.AddIssue("signpost", s.x, s.y, "the signpost stands on no signpost tile");
        std::smatch g;
        if (!std::regex_match(s.lines[1], g, kLeagues)) continue;
        std::vector<const City*> named;  // names may repeat; any of them will do
        for (const City& c : m.cities)
            if (trim(s.lines[0]) == c.name) named.push_back(&c);
        if (named.empty()) {
            cx.AddIssue("signpost", s.x, s.y, "points to \"" + s.lines[0] + "\", which is no city");
            continue;
        }
        pointing++;
        int said = static_cast<int>(std::stol(g[1]));
        // any tile of the footprint will do: designers may aim at the middle
        bool way = false, far = false;
        for (const City* city : named)
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    int cxx = city->x + dx, cyy = city->y + dy;
                    bool w = kCompass[Compass(s.x, s.y, cxx, cyy)] == g[2];
                    int truth = 2 * static_cast<int>(std::floor(std::hypot(s.x - cxx, s.y - cyy)));
                    bool f = std::abs(said - truth) <= std::max(4, truth / 4);
                    way = way || w;
                    far = far || f;
                }
        const std::string& name = named[0]->name;
        if (!way) cx.AddIssue("signpost", s.x, s.y, "says " + std::string(g[2]) + " for " + name);
        if (!far)
            cx.AddIssue("signpost", s.x, s.y, "says " + std::string(g[1]) + " leagues to " + name);
    }
    int crossings = 0;
    for (int y = 0; y < v.h; y++)
        for (int x = 0; x < v.w; x++) {
            if (!v.crossing[y * v.w + x]) continue;
            crossings++;
            bool sea = false, land = false;
            for (int d = 0; d < 8; d++) {
                int t = v.TerrainOr(x + DIRS[d][0], y + DIRS[d][1], -1);
                if (t < 0) continue;
                (t == WATER || t == SHORE ? sea : land) = true;
            }
            if (!sea || !land)
                cx.AddIssue("port", x, y, "a crossing that does not join land and sea");
        }
    cx.Metric("signs", m.signs.size());
    cx.Metric("pointingSigns", pointing);
    cx.Metric("crossings", crossings);
}

}  // namespace

double Evaluation::metric(const std::string& key, double dflt) const {
    for (auto& [k, v] : metrics)
        if (k == key) return v;
    return dflt;
}

int Evaluation::count(const std::string& check) const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(),
                                          [&](const Issue& i) { return i.check == check; }));
}

const std::vector<std::string>& checks() {
    static const std::vector<std::string> kC = {
        "city-terrain", "city-gap",    "city-name",   "dead-end", "road-to-nowhere",
        "spur-road",    "unreachable", "capital-gap", "signpost", "port"};
    return kC;
}

bool isHard(const std::string& check) {
    return check != "spur-road";
}

bool Evaluation::passed() const {
    return std::none_of(issues.begin(), issues.end(),
                        [](const Issue& i) { return isHard(i.check); });
}

const std::vector<std::string>& scoredMetrics() {
    static const std::vector<std::string> kScoredMetrics = {"cityGapMin",
                                                            "cityGapP5",
                                                            "cityGapMedian",
                                                            "crowdedCityFrac",
                                                            "clarkEvansR",
                                                            "spurRoadsPerCity",
                                                            "roadShapeMismatchFrac",
                                                            "cityRoadCoverage",
                                                            "mainNetworkCityShare",
                                                            "roadTilesPerCity",
                                                            "roadDetourMedian",
                                                            "roadDetourP95",
                                                            "landReachShare",
                                                            "landFrac",
                                                            "coastComplexity",
                                                            "biggestLandmassCityShare",
                                                            "forestPatchMedian",
                                                            "hillsPatchMedian",
                                                            "mountainPatchMedian",
                                                            "speckleFrac",
                                                            "capitalGapMinTurns",
                                                            "capitalGapMedianTurns",
                                                            "expansionMinShare",
                                                            "expansionMaxShare",
                                                            "expansionGini",
                                                            "incomeGini",
                                                            "near4MinShare",
                                                            "hinterlandMaxShare",
                                                            "remoteCityFrac"};
    return kScoredMetrics;
}

Evaluation evaluate(const Map& map, const std::string& name) {
    Evaluation e;
    e.name = name;
    View v(map);
    Context cx{v, e};
    Cities(cx);
    Roads(cx);
    Reachability(cx);
    Fairness(cx);
    Terrain(cx);
    SignsAndPorts(cx);
    return e;
}

Reference reference(const std::vector<Evaluation>& maps) {
    Reference ref;
    if (maps.empty()) return ref;
    for (const std::string& k : scoredMetrics()) {
        Range r;
        r.min = std::numeric_limits<double>::max();
        r.max = std::numeric_limits<double>::lowest();
        for (const Evaluation& e : maps) {
            double x = e.metric(k);
            r.min = std::min(r.min, x);
            r.max = std::max(r.max, x);
            r.mean += x;
        }
        r.mean /= static_cast<double>(maps.size());
        for (const Evaluation& e : maps) r.sd += std::pow(e.metric(k) - r.mean, 2);
        r.sd = std::sqrt(r.sd / static_cast<double>(maps.size()));
        ref[k] = r;
    }
    return ref;
}

Score score(const Evaluation& e, const Reference& ref) {
    Score s;
    for (auto& [k, r] : ref) {
        double x = e.metric(k), out = 0;
        if (x < r.min)
            out = r.min - x;
        else if (x > r.max)
            out = x - r.max;
        if (out <= 0) continue;
        // in standard deviations; a metric the designers never varied is measured
        // against a tenth of its size instead
        double unit = std::max({r.sd, 0.1 * std::abs(r.mean), 1e-3});
        double dev = out / unit;
        s.penalty += dev;
        s.deviations.emplace_back(k, x < r.min ? -dev : dev);
    }
    return s;
}

Json toJson(const Evaluation& e, const Score* s) {
    Json j = Json::object();
    j.set("name", e.name);
    j.set("passed", e.passed());
    Json counts = Json::object();
    for (const std::string& c : checks()) counts.set(c, e.count(c));
    j.set("checks", counts);
    Json metrics = Json::object();
    for (auto& [k, v] : e.metrics) metrics.set(k, v);
    j.set("metrics", metrics);
    Json issues = Json::array();
    for (const Issue& i : e.issues) {
        Json o = Json::object();
        o.set("check", i.check);
        o.set("x", i.x);
        o.set("y", i.y);
        o.set("detail", i.detail);
        issues.push(o);
    }
    j.set("issues", issues);
    if (s) {
        Json sc = Json::object();
        sc.set("penalty", s->penalty);
        Json dev = Json::object();
        for (auto& [k, v] : s->deviations) dev.set(k, v);
        sc.set("deviations", dev);
        j.set("score", sc);
    }
    return j;
}

}  // namespace w2::mapeval
