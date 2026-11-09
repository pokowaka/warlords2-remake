#include "warlords/mapeval.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <regex>
#include <set>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/travel.hpp"

namespace w2::mapeval {

namespace {

using namespace w2::move;  // the terrain type ids

/// The four compass directions as bits, as a road piece's arms are counted.
constexpr int kNorth = 1, kEast = 2, kSouth = 4, kWest = 8;
/// The four orthogonal steps, north first and clockwise.
constexpr int kDX4[4] = {0, 1, 0, -1}, kDY4[4] = {-1, 0, 1, 0};
/// The arm bit for each of the four steps in kDX4/kDY4.
constexpr int kArmBit4[4] = {kNorth, kEast, kSouth, kWest};
/// What each road piece joins (docs/formats/map.md > .RD): piece n, n = 1..17.
constexpr int kRoadShape[18] = {0,
                                kEast | kWest,
                                kNorth | kSouth,
                                kNorth | kEast | kSouth | kWest,
                                kEast | kSouth | kWest,
                                kNorth | kSouth | kWest,
                                kNorth | kEast | kWest,
                                kNorth | kEast | kSouth,
                                kSouth | kWest,
                                kNorth | kWest,
                                kNorth | kEast,
                                kEast | kSouth,
                                kWest,
                                kSouth,
                                kEast,
                                kNorth,
                                kEast | kWest,
                                kNorth | kSouth};
/// The directions a signpost names, by the number Compass() gives.
const char* const kCompass[8] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};

/// A city may not come closer than this to another: the tiles between the two
/// footprints. The hand-made scenarios never go under 1 (see docs/mapeval.md).
constexpr int kMinCityGap = 1;
/// A city this close (tiles between footprints) to another counts as crowded.
constexpr int kCrowdedGap = 2;
/// Two capitals may not be closer than this, in turns of travel. The hand-made
/// scenarios never come under 2.5 (Erythea); this is about 60% of that.
constexpr double kCapitalGapTurns = 1.5;
/// A neutral city further than this from every capital is out in the wilds.
constexpr int kRemoteTurns = 5;

/**
 * @brief The value at `quantile` (0..1) of the values, interpolating
 *        between the two nearest; 0 for no values.
 */
double Quantile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    double position = quantile * static_cast<double>(values.size() - 1);
    size_t below = static_cast<size_t>(std::floor(position)),
           above = static_cast<size_t>(std::ceil(position));
    return values[below] +
           (values[above] - values[below]) * (position - static_cast<double>(below));
}

/** @brief The map, read once into flat grids the checks share. */
struct View {
    const Map& map;              ///< The map being evaluated.
    int width;                   ///< The map's width in tiles.
    int height;                  ///< The map's height in tiles.
    std::vector<int> terrain;    ///< Terrain type of each tile.
    std::vector<int> road;       ///< Road piece on each tile, 0 for none.
    std::vector<int> city_of;    ///< City index on each footprint tile, or -1.
    std::vector<bool> site;      ///< A site stands on the tile.
    std::vector<bool> crossing;  ///< The tile is marked as a crossing.
    std::vector<bool> port;      ///< The tile is part of a port city's footprint.

    explicit View(const Map& scenario_map)
            : map(scenario_map), width(scenario_map.width), height(scenario_map.height) {
        int tile_count = width * height;
        terrain.resize(tile_count);
        road.resize(tile_count);
        city_of.assign(tile_count, -1);
        site.assign(tile_count, false);
        crossing.assign(tile_count, false);
        port.assign(tile_count, false);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int tile = y * width + x;
                terrain[tile] = scn::TerrainAt(map, x, y);
                int piece = scn::RoadAt(map, x, y) % 32;
                road[tile] = piece <= 17 ? piece : 0;
                crossing[tile] = scn::IsCrossing(map, x, y);
            }
        }
        for (const City& city : map.cities) {
            bool is_port = false;
            for (int dy = -1; dy <= 2 && !is_port; dy++)
                for (int dx = -1; dx <= 2 && !is_port; dx++) {
                    if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1) continue;
                    int around = TerrainOr(city.X() + dx, city.Y() + dy, -1);
                    is_port = around == kBridge || around == kWater || around == kShore;
                }
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    if (!In(city.X() + dx, city.Y() + dy)) continue;
                    int tile = (city.Y() + dy) * width + city.X() + dx;
                    city_of[tile] = city.Index();
                    port[tile] = is_port;
                }
        }
        for (const Site& map_site : map.sites)
            if (In(map_site.x, map_site.y)) site[map_site.y * width + map_site.x] = true;
    }
    /** @brief Is (x, y) on the map? */
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    /** @brief The terrain at (x, y), or `fallback` off the map. */
    int TerrainOr(int x, int y, int fallback) const {
        return In(x, y) ? terrain[y * width + x] : fallback;
    }
    /** @brief Is the tile water or shore? */
    bool Sea(int tile) const { return terrain[tile] == kWater || terrain[tile] == kShore; }
    /** @brief Road, or a bridge: what a road network is made of. */
    bool Roadway(int tile) const { return road[tile] != 0 || terrain[tile] == kBridge; }
    /** @brief What a road may lead to: a city, a site, a signpost or a crossing. */
    bool Destination(int tile) const {
        return city_of[tile] >= 0 || site[tile] || terrain[tile] == kTower || crossing[tile];
    }
    /** @brief Is a destination on this tile or any of its 8 neighbours? */
    bool DestinationNear(int x, int y) const {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (In(x + dx, y + dy) && Destination((y + dy) * width + x + dx)) return true;
        return false;
    }
};

/** @brief What every check works with: the map's view and the evaluation it fills in. */
struct Context {
    const View& view;        ///< The map, read into grids.
    Evaluation& evaluation;  ///< Where the issues and metrics go.
    /** @brief Records a hard- or soft-rule failure on a tile. */
    void AddIssue(const char* check, int x, int y, const std::string& detail) {
        evaluation.issues.push_back({check, x, y, detail});
    }
    /** @brief Records a named number. */
    void Metric(const char* key, double value) { evaluation.metrics.emplace_back(key, value); }
};

// ---------------------------------------------------------------- cities

/** @brief Tiles between two footprints along x and along y (0 when touching). */
std::pair<int, int> Gaps(const City& first, const City& second) {
    int gap_x = std::max(0, std::abs(first.X() - second.X()) - 2),
        gap_y = std::max(0, std::abs(first.Y() - second.Y()) - 2);
    return {gap_x, gap_y};
}
/** @brief Do two cities' 2x2 footprints share a tile? */
bool Overlap(const City& first, const City& second) {
    return std::abs(first.X() - second.X()) < 2 && std::abs(first.Y() - second.Y()) < 2;
}

/**
 * @brief The cities: their footprints, names and spacing, and how evenly
 *        they spread over the land.
 */
void Cities(Context& context) {
    const Map& map = context.view.map;
    const View& view = context.view;
    int city_count = static_cast<int>(map.cities.size());
    std::vector<double> nearest_gap, nearest_dist;
    int crowded = 0;
    std::map<std::string, int> names;
    for (const City& city : map.cities) {
        if (names[city.Name()]++ == 1)
            context.AddIssue("city-name", city.X(), city.Y(),
                             "two or more cities are called " + city.Name());
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) {
                int terrain = view.TerrainOr(city.X() + dx, city.Y() + dy, -1);
                if (terrain != kCity) {
                    context.AddIssue(
                            "city-terrain", city.X() + dx, city.Y() + dy,
                            city.Name() + "'s footprint stands on " +
                                    (terrain >= 0 ? kTerrainNames[terrain] : "the map's edge"));
                }
            }
        int best = std::numeric_limits<int>::max();
        double best_dist = std::numeric_limits<double>::max();
        for (const City& other : map.cities) {
            if (&other == &city) continue;
            auto [gap_x, gap_y] = Gaps(city, other);
            int gap = Overlap(city, other) ? -1 : std::max(gap_x, gap_y);
            best = std::min(best, gap);
            best_dist = std::min(best_dist, std::hypot(city.X() - other.X(), city.Y() - other.Y()));
            if (city.Index() < other.Index() && gap < kMinCityGap) {
                context.AddIssue(
                        "city-gap", city.X(), city.Y(),
                        city.Name() + " and " + other.Name() +
                                (gap < 0 ? " overlap"
                                         : " are " + std::to_string(gap) + " tile(s) apart"));
            }
        }
        if (city_count > 1) {
            nearest_gap.push_back(best);
            nearest_dist.push_back(best_dist);
            if (best <= kCrowdedGap) crowded++;
        }
    }
    // Clark-Evans: the mean nearest-neighbour distance over what random points
    // on the same land would give; < 1 clustered, 1 random, > 1 evenly spread
    int land = 0;
    for (int i = 0; i < view.width * view.height; i++) land += !view.Sea(i);
    double mean = 0;
    for (double dist : nearest_dist) mean += dist;
    mean = nearest_dist.empty() ? 0 : mean / static_cast<double>(nearest_dist.size());
    double expected = city_count > 0 && land > 0
                              ? 0.5 / std::sqrt(static_cast<double>(city_count) / land)
                              : 0;
    context.Metric("cities", city_count);
    context.Metric("cityGapMin", Quantile(nearest_gap, 0));
    context.Metric("cityGapP5", Quantile(nearest_gap, 0.05));
    context.Metric("cityGapMedian", Quantile(nearest_gap, 0.5));
    context.Metric("crowdedCityFrac", city_count ? static_cast<double>(crowded) / city_count : 0);
    context.Metric("clarkEvansR", expected > 0 ? mean / expected : 0);
}

// ---------------------------------------------------------------- roads

/**
 * @brief The roads: pieces whose arms meet nothing, networks that reach no
 *        place or only one, how many cities the roads join, and how far a
 *        road detours between neighbouring cities.
 */
void Roads(Context& context) {
    const View& view = context.view;
    const Map& map = view.map;
    int width = view.width, height = view.height;
    int road_tiles = 0, pieces = 0, mismatched = 0, dead_ends = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int tile = y * width + x;
            if (!view.road[tile]) continue;
            pieces++;
            // the piece's own arms against what lies beyond them: an arm must meet
            // a roadway or a place, and a road piece pointing back needs an arm
            int shape = kRoadShape[view.road[tile]], joined = 0;
            bool bad = false;
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!view.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                if (view.Roadway(next) || view.Destination(next)) joined |= kArmBit4[dir];
                bool back =
                        view.road[next] && (kRoadShape[view.road[next]] & kArmBit4[(dir + 2) % 4]);
                if (back && !(shape & kArmBit4[dir])) bad = true;
            }
            if (shape & ~joined) bad = true;
            if (bad) mismatched++;
            for (int dir = 0; dir < 4; dir++) {
                if (!(shape & kArmBit4[dir]) || (joined & kArmBit4[dir])) continue;
                if (view.DestinationNear(x, y)) continue;
                dead_ends++;
                context.AddIssue("dead-end", x, y,
                                 std::string("the road's ") + "NESW"[dir] + " arm leads nowhere");
            }
        }
    }
    // the networks: 4-connected roads and bridges, and what each one reaches
    std::vector<int> network_of(static_cast<size_t>(width) * height, -1);
    int networks = 0, stray = 0, spurs = 0, main_size = 0;
    std::vector<std::set<int>> network_cities;
    for (int start = 0; start < width * height; start++) {
        if (!view.Roadway(start) || network_of[start] >= 0) continue;
        std::set<int> reached;
        std::set<int> others;  // sites, signposts and crossings, by tile
        int size = 0;
        std::deque<int> frontier{start};
        network_of[start] = networks;
        while (!frontier.empty()) {
            int tile = frontier.front();
            frontier.pop_front();
            size++;
            int x = tile % width, y = tile / width;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!view.In(x + dx, y + dy)) continue;
                    int around = (y + dy) * width + x + dx;
                    if (view.city_of[around] >= 0) reached.insert(view.city_of[around]);
                    if (view.site[around] || view.crossing[around] ||
                        view.terrain[around] == kTower)
                        others.insert(around);
                }
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!view.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                if (view.Roadway(next) && network_of[next] < 0) {
                    network_of[next] = networks;
                    frontier.push_back(next);
                }
            }
        }
        road_tiles += size;
        int ends = static_cast<int>(reached.size()) + static_cast<int>(others.size());
        if (ends == 0) {
            stray++;
            context.AddIssue("road-to-nowhere", start % width, start / width,
                             "a road of " + std::to_string(size) + " tile(s) reaching no place");
        } else if (ends == 1) {
            spurs++;
            context.AddIssue(
                    "spur-road", start % width, start / width,
                    "a road of " + std::to_string(size) + " tile(s) reaching one place only");
        }
        if (static_cast<int>(reached.size()) > main_size)
            main_size = static_cast<int>(reached.size());
        network_cities.push_back(reached);
        networks++;
    }
    int city_count = static_cast<int>(map.cities.size());
    std::vector<bool> on_road(city_count, false);
    for (auto& cities_reached : network_cities)
        for (int city_index : cities_reached) on_road[city_index] = true;
    int linked = static_cast<int>(std::count(on_road.begin(), on_road.end(), true));

    // detours: from each city along its roads to the first other cities met
    std::vector<double> detours;
    for (const City& city : map.cities) {
        std::vector<int> dist(static_cast<size_t>(width) * height, -1);
        std::deque<int> frontier;
        for (int dy = -1; dy <= 2; dy++)
            for (int dx = -1; dx <= 2; dx++) {
                int x = city.X() + dx, y = city.Y() + dy;
                if (!view.In(x, y)) continue;
                int tile = y * width + x;
                if (view.Roadway(tile) && view.city_of[tile] < 0) {
                    dist[tile] = 1;
                    frontier.push_back(tile);
                }
            }
        std::set<int> met;
        while (!frontier.empty()) {
            int tile = frontier.front();
            frontier.pop_front();
            int x = tile % width, y = tile / width;
            int other = -1;
            for (int dy = -1; dy <= 1 && other < 0; dy++)
                for (int dx = -1; dx <= 1 && other < 0; dx++)
                    if (view.In(x + dx, y + dy)) {
                        int near_city = view.city_of[(y + dy) * width + x + dx];
                        if (near_city >= 0 && near_city != city.Index()) other = near_city;
                    }
            if (other >= 0) {
                if (met.insert(other).second) {
                    auto [gap_x, gap_y] = Gaps(city, map.cities[other]);
                    detours.push_back(static_cast<double>(dist[tile]) / std::max(1, gap_x + gap_y));
                }
                continue;  // a road through a city is two roads
            }
            for (int dir = 0; dir < 4; dir++) {
                int next_x = x + kDX4[dir], next_y = y + kDY4[dir];
                if (!view.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                if (view.Roadway(next) && dist[next] < 0 && view.city_of[next] < 0) {
                    dist[next] = dist[tile] + 1;
                    frontier.push_back(next);
                }
            }
        }
    }
    context.Metric("roadTiles", road_tiles);
    context.Metric("roadNetworks", networks);
    context.Metric("roadsToNowhere", stray);
    context.Metric("spurRoads", spurs);
    context.Metric("spurRoadsPerCity", city_count ? static_cast<double>(spurs) / city_count : 0);
    context.Metric("deadEnds", dead_ends);
    context.Metric("deadEndsPer100Road", road_tiles ? 100.0 * dead_ends / road_tiles : 0);
    context.Metric("roadShapeMismatchFrac", pieces ? static_cast<double>(mismatched) / pieces : 0);
    context.Metric("cityRoadCoverage", city_count ? static_cast<double>(linked) / city_count : 0);
    context.Metric("mainNetworkCityShare",
                   city_count ? static_cast<double>(main_size) / city_count : 0);
    context.Metric("roadTilesPerCity",
                   city_count ? static_cast<double>(road_tiles) / city_count : 0);
    context.Metric("roadDetourMedian", Quantile(detours, 0.5));
    context.Metric("roadDetourP95", Quantile(detours, 0.95));
}

// ---------------------------------------------------------------- reach

/**
 * @brief Every tile a stack can get to from (start_x, start_y).
 *
 * Mountains are shut; it changes between land and sea only through a
 * crossing (a bridge, a marked tile, a port city's footprint) --
 * docs/rules.md > Movement. With `land_only`, the sea is shut too.
 */
std::vector<bool> Reach(const View& view, int start_x, int start_y, bool land_only) {
    int width = view.width, height = view.height;
    std::vector<bool> seen(static_cast<size_t>(width) * height, false);
    // a road over mountains is a pass: any road tile costs 1
    auto open = [&](int tile) {
        return (view.terrain[tile] != kMountains || view.road[tile]) &&
               !(land_only && view.Sea(tile));
    };
    auto crossing = [&](int tile) {
        return view.crossing[tile] || view.port[tile] || view.terrain[tile] == kBridge;
    };
    int start = start_y * width + start_x;
    if (!view.In(start_x, start_y) || !open(start)) return seen;
    std::deque<int> frontier{start};
    seen[start] = true;
    while (!frontier.empty()) {
        int tile = frontier.front();
        frontier.pop_front();
        int x = tile % width, y = tile / width;
        for (int dir = 0; dir < 8; dir++) {
            int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
            if (!view.In(next_x, next_y)) continue;
            int next = next_y * width + next_x;
            if (seen[next] || !open(next)) continue;
            if (view.Sea(tile) != view.Sea(next) && !crossing(tile) && !crossing(next)) continue;
            seen[next] = true;
            frontier.push_back(next);
        }
    }
    return seen;
}

/**
 * @brief Can every capital reach every city and site, and what share of the
 *        cities can it reach without taking to the sea?
 */
void Reachability(Context& context) {
    const View& view = context.view;
    const Map& map = view.map;
    int width = view.width;
    std::vector<const City*> capitals;
    for (const Side& side : map.sides)
        if (const City* cap = side.CapitalCity(map)) capitals.push_back(cap);
    double land_share = 0;
    std::set<int> lost_cities, lost_sites;
    for (const City* capital : capitals) {
        auto all = Reach(view, capital->X(), capital->Y(), false);
        auto land = Reach(view, capital->X(), capital->Y(), true);
        int by_land = 0;
        for (const City& city : map.cities) {
            if (land[city.Y() * width + city.X()]) by_land++;
            if (!all[city.Y() * width + city.X()] && lost_cities.insert(city.Index()).second)
                context.AddIssue("unreachable", city.X(), city.Y(),
                                 city.Name() + " cannot be reached from " + capital->Name());
        }
        for (const Site& site : map.sites)
            if (!all[site.y * width + site.x] && lost_sites.insert(site.index).second)
                context.AddIssue("unreachable", site.x, site.y,
                                 site.name + " cannot be reached from " + capital->Name());
        land_share += map.cities.empty() ? 0 : static_cast<double>(by_land) / map.cities.size();
    }
    context.Metric("capitals", capitals.size());
    context.Metric("landReachShare", capitals.empty() ? 0 : land_share / capitals.size());
}

// ---------------------------------------------------------------- fairness

/** @brief The Gini coefficient of the values: 0 all equal, towards 1 all in one. */
double Gini(std::vector<double> values) {
    if (values.size() < 2) return 0;
    std::sort(values.begin(), values.end());
    double sum = 0, weighted = 0;
    for (size_t i = 0; i < values.size(); i++) {
        sum += values[i];
        weighted += static_cast<double>(i + 1) * values[i];
    }
    if (sum <= 0) return 0;
    double count = static_cast<double>(values.size());
    return (2 * weighted) / (count * sum) - (count + 1) / count;
}

/**
 * @brief Each side's start, by travel cost from its capital.
 *
 * How near its rivals are, and what it can expand into: the neutral cities
 * it reaches before anyone else does (a Voronoi diagram by travel cost),
 * their income, and how many lie within a few turns.
 */
void Fairness(Context& context) {
    const View& view = context.view;
    const Map& map = view.map;
    int width = view.width, tile_count = width * view.height;
    travel::Ground ground;
    ground.w = width;
    ground.h = view.height;
    ground.terrain = view.terrain;
    ground.road.resize(tile_count);
    ground.crossing.resize(tile_count);
    for (int i = 0; i < tile_count; i++) {
        ground.road[i] = view.road[i] != 0;
        ground.crossing[i] = view.crossing[i] || view.port[i] || view.terrain[i] == kBridge;
    }
    auto footprint = [&](const City& city) {
        std::vector<int> tiles;
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                if (view.In(city.X() + dx, city.Y() + dy))
                    tiles.push_back((city.Y() + dy) * width + city.X() + dx);
        return tiles;
    };
    auto cost_to = [&](const std::vector<int>& costs, const City& city) {
        int best = travel::kUnreached;
        for (int tile : footprint(city)) best = std::min(best, costs[tile]);
        return best;
    };
    std::vector<const City*> capitals;
    for (const Side& side : map.sides)
        if (const City* cap = side.CapitalCity(map)) capitals.push_back(cap);
    int capital_count = static_cast<int>(capitals.size());
    if (capital_count < 2) return;
    std::vector<std::vector<int>> cost_fields(capital_count);
    for (int i = 0; i < capital_count; i++)
        cost_fields[i] = travel::Costs(ground, footprint(*capitals[i]));

    // the nearest rival capital, in turns
    std::vector<double> gaps;
    for (int i = 0; i < capital_count; i++) {
        int best = travel::kUnreached;
        for (int j = 0; j < capital_count; j++)
            if (j != i) best = std::min(best, cost_to(cost_fields[i], *capitals[j]));
        if (best == travel::kUnreached) continue;
        double turns = static_cast<double>(best) / travel::kTurn;
        gaps.push_back(turns);
        for (int j = i + 1; j < capital_count; j++) {
            int cost = cost_to(cost_fields[i], *capitals[j]);
            if (cost != travel::kUnreached &&
                static_cast<double>(cost) / travel::kTurn < kCapitalGapTurns)
                context.AddIssue("capital-gap", capitals[i]->X(), capitals[i]->Y(),
                                 absl::StrFormat("%s and %s are %.1f turns apart",
                                                 capitals[i]->Name(), capitals[j]->Name(),
                                                 static_cast<double>(cost) / travel::kTurn));
        }
    }
    // the neutral cities each side reaches first, and those within reach
    std::vector<double> cities(capital_count, 0), income(capital_count, 0), near2(capital_count, 0),
            near4(capital_count, 0), hinterland(capital_count, 0);
    int neutral = 0, remote = 0;
    for (const City& city : map.cities) {
        if (std::find(capitals.begin(), capitals.end(), &city) != capitals.end()) continue;
        neutral++;
        int best = travel::kUnreached, second = travel::kUnreached, who = -1;
        for (int i = 0; i < capital_count; i++) {
            int cost = cost_to(cost_fields[i], city);
            if (cost < best) {
                second = best;
                best = cost;
                who = i;
            } else if (cost < second)
                second = cost;
            if (cost <= 2 * travel::kTurn) near2[i]++;
            if (cost <= 4 * travel::kTurn) near4[i]++;
        }
        if (who < 0) continue;
        cities[who]++;
        income[who] += city.Income();
        if (best > kRemoteTurns * travel::kTurn) remote++;
        // a city nobody else can contest: the next side is twice as far, and
        // at least two turns further
        if (second == travel::kUnreached ||
            (second >= 2 * best && second - best >= 2 * travel::kTurn))
            hinterland[who]++;
    }
    double fair = neutral > 0 ? static_cast<double>(neutral) / capital_count : 1;
    context.Metric("capitalGapMinTurns", gaps.empty() ? 0 : Quantile(gaps, 0));
    context.Metric("capitalGapMedianTurns", Quantile(gaps, 0.5));
    context.Metric("expansionMinShare", *std::min_element(cities.begin(), cities.end()) / fair);
    context.Metric("expansionMaxShare", *std::max_element(cities.begin(), cities.end()) / fair);
    context.Metric("expansionGini", Gini(cities));
    context.Metric("incomeGini", Gini(income));
    context.Metric("near2MinShare", *std::min_element(near2.begin(), near2.end()) / fair);
    context.Metric("near4MinShare", *std::min_element(near4.begin(), near4.end()) / fair);
    context.Metric("hinterlandMaxShare",
                   *std::max_element(hinterland.begin(), hinterland.end()) / fair);
    context.Metric("remoteCityFrac", neutral ? static_cast<double>(remote) / neutral : 0);
}

// ---------------------------------------------------------------- terrain

/**
 * @brief The ground: how much is land, how ragged its coast, how the cities
 *        share out over the landmasses, and how lumpy the forest, hills,
 *        mountains and marsh are.
 */
void Terrain(Context& context) {
    const View& view = context.view;
    const Map& map = view.map;
    int width = view.width, height = view.height, tile_count = width * height;
    int land = 0, coast = 0;
    for (int i = 0; i < tile_count; i++) {
        if (view.Sea(i)) continue;
        land++;
        int x = i % width, y = i / width;
        for (int dir = 0; dir < 4; dir++) {
            int neighbour = view.TerrainOr(x + kDX4[dir], y + kDY4[dir], kWater);
            if (neighbour == kWater || neighbour == kShore) {
                coast++;
                break;
            }
        }
    }
    // landmasses: 8-connected land, crossing bridges, and the cities on each
    std::vector<int> mass(tile_count, -1);
    std::vector<int> mass_cities;
    for (int start = 0; start < tile_count; start++) {
        if (view.Sea(start) || mass[start] >= 0) continue;
        int mass_id = static_cast<int>(mass_cities.size());
        mass_cities.push_back(0);
        std::deque<int> frontier{start};
        mass[start] = mass_id;
        while (!frontier.empty()) {
            int tile = frontier.front();
            frontier.pop_front();
            int x = tile % width, y = tile / width;
            for (int dir = 0; dir < 8; dir++) {
                int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
                if (!view.In(next_x, next_y)) continue;
                int next = next_y * width + next_x;
                bool through = !view.Sea(next) || view.terrain[next] == kBridge;
                if (through && mass[next] < 0) {
                    mass[next] = mass_id;
                    frontier.push_back(next);
                }
            }
        }
    }
    for (const City& city : map.cities)
        if (view.In(city.X(), city.Y()) && mass[city.Y() * width + city.X()] >= 0)
            mass_cities[mass[city.Y() * width + city.X()]]++;
    int inhabited = 0, biggest = 0;
    for (int cities_on_mass : mass_cities) {
        inhabited += cities_on_mass > 0;
        biggest = std::max(biggest, cities_on_mass);
    }

    // patches: how lumpy forest, hills, mountains and marsh are
    auto patches = [&](int type, double& median, int& lone, int& total) {
        std::vector<bool> seen(tile_count, false);
        std::vector<double> sizes;
        for (int start = 0; start < tile_count; start++) {
            if (view.terrain[start] != type || seen[start]) continue;
            int size = 0;
            std::deque<int> frontier{start};
            seen[start] = true;
            while (!frontier.empty()) {
                int tile = frontier.front();
                frontier.pop_front();
                size++;
                int x = tile % width, y = tile / width;
                for (int dir = 0; dir < 8; dir++) {
                    int next_x = x + kDirs[dir][0], next_y = y + kDirs[dir][1];
                    if (!view.In(next_x, next_y)) continue;
                    int next = next_y * width + next_x;
                    if (view.terrain[next] == type && !seen[next]) {
                        seen[next] = true;
                        frontier.push_back(next);
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
    patches(kForest, forest, lone, total);
    patches(kHills, hills, lone, total);
    patches(kMountains, mountains, lone, total);
    patches(kMarsh, marsh, lone, total);

    context.Metric("landFrac", static_cast<double>(land) / tile_count);
    context.Metric("coastComplexity", land ? coast / std::sqrt(static_cast<double>(land)) : 0);
    context.Metric("inhabitedLandmasses", inhabited);
    context.Metric("biggestLandmassCityShare",
                   map.cities.empty() ? 0 : static_cast<double>(biggest) / map.cities.size());
    context.Metric("forestPatchMedian", forest);
    context.Metric("hillsPatchMedian", hills);
    context.Metric("mountainPatchMedian", mountains);
    context.Metric("marshPatchMedian", marsh);
    context.Metric("speckleFrac", total ? static_cast<double>(lone) / total : 0);
}

// ---------------------------------------------------------------- signs, ports

/**
 * @brief The direction from one tile to another as the generator gives a
 *        signpost's (an index into kCompass).
 */
int Compass(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x) return from_y < to_y ? 4 : 0;
    if (from_y == to_y) return from_x < to_x ? 2 : 6;
    if (to_x < from_x && to_y < from_y) return 7;
    if (to_x < from_x && from_y < to_y) return 5;
    if (from_x < to_x && to_y < from_y) return 1;
    return 3;
}

/**
 * @brief Signposts stand on signpost tiles and point the right way the
 *        right distance to a real city; crossings join land and sea.
 */
void SignsAndPorts(Context& context) {
    const View& view = context.view;
    const Map& map = view.map;
    static const std::regex kLeagues("^\\s*(\\d+) leagues (\\w+)\\s*$");
    int pointing = 0;
    for (const Sign& sign : map.signs) {
        if (view.TerrainOr(sign.x, sign.y, -1) != kTower)
            context.AddIssue("signpost", sign.x, sign.y, "the signpost stands on no signpost tile");
        std::smatch match;
        if (!std::regex_match(sign.lines[1], match, kLeagues)) continue;
        std::vector<const City*> named;  // names may repeat; any of them will do
        for (const City& city : map.cities)
            if (Trim(sign.lines[0]) == city.Name()) named.push_back(&city);
        if (named.empty()) {
            context.AddIssue("signpost", sign.x, sign.y,
                             "points to \"" + sign.lines[0] + "\", which is no city");
            continue;
        }
        pointing++;
        int said = static_cast<int>(std::stol(match[1]));
        // any tile of the footprint will do: designers may aim at the middle
        bool way = false, far = false;
        for (const City* city : named)
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    int city_x = city->X() + dx, city_y = city->Y() + dy;
                    bool right_way = kCompass[Compass(sign.x, sign.y, city_x, city_y)] == match[2];
                    int truth = 2 * static_cast<int>(std::floor(
                                            std::hypot(sign.x - city_x, sign.y - city_y)));
                    bool right_distance = std::abs(said - truth) <= std::max(4, truth / 4);
                    way = way || right_way;
                    far = far || right_distance;
                }
        const std::string& name = named[0]->Name();
        if (!way)
            context.AddIssue("signpost", sign.x, sign.y,
                             "says " + std::string(match[2]) + " for " + name);
        if (!far)
            context.AddIssue("signpost", sign.x, sign.y,
                             "says " + std::string(match[1]) + " leagues to " + name);
    }
    int crossings = 0;
    for (int y = 0; y < view.height; y++)
        for (int x = 0; x < view.width; x++) {
            if (!view.crossing[y * view.width + x]) continue;
            crossings++;
            bool sea = false, land = false;
            for (int dir = 0; dir < 8; dir++) {
                int neighbour = view.TerrainOr(x + kDirs[dir][0], y + kDirs[dir][1], -1);
                if (neighbour < 0) continue;
                (neighbour == kWater || neighbour == kShore ? sea : land) = true;
            }
            if (!sea || !land)
                context.AddIssue("port", x, y, "a crossing that does not join land and sea");
        }
    context.Metric("signs", map.signs.size());
    context.Metric("pointingSigns", pointing);
    context.Metric("crossings", crossings);
}

}  // namespace

double Evaluation::Metric(const std::string& key, double fallback) const {
    for (auto& [metric_key, value] : metrics)
        if (metric_key == key) return value;
    return fallback;
}

int Evaluation::Count(const std::string& check) const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [&](const Issue& issue) {
        return issue.check == check;
    }));
}

const std::vector<std::string>& Checks() {
    static const std::vector<std::string> kChecks = {
        "city-terrain", "city-gap",    "city-name",   "dead-end", "road-to-nowhere",
        "spur-road",    "unreachable", "capital-gap", "signpost", "port"};
    return kChecks;
}

bool IsHard(const std::string& check) {
    return check != "spur-road";
}

bool Evaluation::Passed() const {
    return std::none_of(issues.begin(), issues.end(),
                        [](const Issue& issue) { return IsHard(issue.check); });
}

const std::vector<std::string>& ScoredMetrics() {
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

Evaluation Evaluate(const Map& map, const std::string& name) {
    Evaluation evaluation;
    evaluation.name = name;
    View view(map);
    Context context{view, evaluation};
    Cities(context);
    Roads(context);
    Reachability(context);
    Fairness(context);
    Terrain(context);
    SignsAndPorts(context);
    return evaluation;
}

Reference MakeReference(const std::vector<Evaluation>& maps) {
    Reference ref;
    if (maps.empty()) return ref;
    for (const std::string& key : ScoredMetrics()) {
        Range range;
        range.min = std::numeric_limits<double>::max();
        range.max = std::numeric_limits<double>::lowest();
        for (const Evaluation& evaluation : maps) {
            double value = evaluation.Metric(key);
            range.min = std::min(range.min, value);
            range.max = std::max(range.max, value);
            range.mean += value;
        }
        range.mean /= static_cast<double>(maps.size());
        for (const Evaluation& evaluation : maps)
            range.sd += std::pow(evaluation.Metric(key) - range.mean, 2);
        range.sd = std::sqrt(range.sd / static_cast<double>(maps.size()));
        ref[key] = range;
    }
    return ref;
}

Score ScoreAgainst(const Evaluation& evaluation, const Reference& ref) {
    Score score;
    for (auto& [key, range] : ref) {
        double value = evaluation.Metric(key), outside = 0;
        if (value < range.min)
            outside = range.min - value;
        else if (value > range.max)
            outside = value - range.max;
        if (outside <= 0) continue;
        // in standard deviations; a metric the designers never varied is measured
        // against a tenth of its size instead
        double unit = std::max({range.sd, 0.1 * std::abs(range.mean), 1e-3});
        double deviation = outside / unit;
        score.penalty += deviation;
        score.deviations.emplace_back(key, value < range.min ? -deviation : deviation);
    }
    return score;
}

nlohmann::ordered_json ToJson(const Evaluation& evaluation, const Score* score) {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    json["name"] = evaluation.name;
    json["passed"] = evaluation.Passed();
    nlohmann::ordered_json counts = nlohmann::ordered_json::object();
    for (const std::string& check : Checks()) counts[check] = evaluation.Count(check);
    json["checks"] = std::move(counts);
    nlohmann::ordered_json metrics = nlohmann::ordered_json::object();
    for (const auto& [key, value] : evaluation.metrics) metrics[key] = value;
    json["metrics"] = std::move(metrics);
    nlohmann::ordered_json issues = nlohmann::ordered_json::array();
    for (const Issue& issue : evaluation.issues) {
        nlohmann::ordered_json entry = nlohmann::ordered_json::object();
        entry["check"] = issue.check;
        entry["x"] = issue.x;
        entry["y"] = issue.y;
        entry["detail"] = issue.detail;
        issues.push_back(std::move(entry));
    }
    json["issues"] = std::move(issues);
    if (score) {
        nlohmann::ordered_json score_json = nlohmann::ordered_json::object();
        score_json["penalty"] = score->penalty;
        nlohmann::ordered_json deviations = nlohmann::ordered_json::object();
        for (const auto& [key, value] : score->deviations) deviations[key] = value;
        score_json["deviations"] = std::move(deviations);
        json["score"] = std::move(score_json);
    }
    return json;
}

}  // namespace w2::mapeval
