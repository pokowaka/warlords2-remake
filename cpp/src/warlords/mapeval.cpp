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

using namespace w2::move;   // the terrain type ids

constexpr int N = 1, E = 2, S = 4, W = 8;
constexpr int DX4[4] = {0, 1, 0, -1}, DY4[4] = {-1, 0, 1, 0};
constexpr int BIT4[4] = {N, E, S, W};
// What each road piece joins (docs/formats/map.md > .RD): piece n, n = 1..17.
constexpr int ROAD_SHAPE[18] = {0,     E | W,     N | S, N | E | S | W, E | S | W, N | S | W,
                                N | E | W, N | E | S, S | W, N | W,         N | E,     E | S,
                                W,     S,         E,     N,             E | W,     N | S};
const char* const COMPASS[8] = {"north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"};

// A city may not come closer than this to another: the tiles between the two
// footprints. The hand-made scenarios never go under 1 (see docs/mapeval.md).
constexpr int MIN_CITY_GAP = 1;
// A city this close (tiles between footprints) to another counts as crowded.
constexpr int CROWDED_GAP = 2;
// Two capitals may not be closer than this, in turns of travel. The hand-made
// scenarios never come under 2.5 (Erythea); this is about 60% of that.
constexpr double CAPITAL_GAP_TURNS = 1.5;
// A neutral city further than this from every capital is out in the wilds.
constexpr int REMOTE_TURNS = 5;

double quantile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double at = q * (double)(v.size() - 1);
  size_t lo = (size_t)std::floor(at), hi = (size_t)std::ceil(at);
  return v[lo] + (v[hi] - v[lo]) * (at - (double)lo);
}

/** The map, read once into flat grids the checks share. */
struct View {
  const Map& m;
  int w, h;
  std::vector<int> terrain, road;            // terrain type, road piece (0 none)
  std::vector<int> cityOf;                   // city index on each footprint tile, or -1
  std::vector<bool> site, crossing, port;    // port: a footprint tile of a port city

  explicit View(const Map& map) : m(map), w(map.width), h(map.height) {
    int n = w * h;
    terrain.resize(n);
    road.resize(n);
    cityOf.assign(n, -1);
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
      bool isPort = false;
      for (int dy = -1; dy <= 2 && !isPort; dy++)
        for (int dx = -1; dx <= 2 && !isPort; dx++) {
          if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1) continue;
          int t = terrainOr(c.x + dx, c.y + dy, -1);
          isPort = t == BRIDGE || t == WATER || t == SHORE;
        }
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
          if (!in(c.x + dx, c.y + dy)) continue;
          int i = (c.y + dy) * w + c.x + dx;
          cityOf[i] = c.index;
          port[i] = isPort;
        }
    }
    for (const Site& s : m.sites)
      if (in(s.x, s.y)) site[s.y * w + s.x] = true;
  }
  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  int terrainOr(int x, int y, int dflt) const { return in(x, y) ? terrain[y * w + x] : dflt; }
  bool sea(int i) const { return terrain[i] == WATER || terrain[i] == SHORE; }
  /** Road, or a bridge: what a road network is made of. */
  bool roadway(int i) const { return road[i] != 0 || terrain[i] == BRIDGE; }
  /** What a road may lead to: a city, a site, a signpost or a crossing. */
  bool destination(int i) const { return cityOf[i] >= 0 || site[i] || terrain[i] == TOWER || crossing[i]; }
  /** Is a destination on this tile or any of its 8 neighbours? */
  bool destinationNear(int x, int y) const {
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (in(x + dx, y + dy) && destination((y + dy) * w + x + dx)) return true;
    return false;
  }
};

struct Context {
  const View& v;
  Evaluation& e;
  void issue(const char* check, int x, int y, const std::string& detail) { e.issues.push_back({check, x, y, detail}); }
  void metric(const char* key, double value) { e.metrics.emplace_back(key, value); }
};

// ---------------------------------------------------------------- cities

/** Tiles between two footprints along x and along y (0 when touching). */
std::pair<int, int> gaps(const City& a, const City& b) {
  int gx = std::max(0, std::abs(a.x - b.x) - 2), gy = std::max(0, std::abs(a.y - b.y) - 2);
  return {gx, gy};
}
bool overlap(const City& a, const City& b) { return std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2; }

void cities(Context& cx) {
  const Map& m = cx.v.m;
  const View& v = cx.v;
  int n = (int)m.cities.size();
  std::vector<double> nearestGap, nearestDist;
  int crowded = 0;
  std::map<std::string, int> names;
  for (const City& c : m.cities) {
    if (names[c.name]++ == 1) cx.issue("city-name", c.x, c.y, "two or more cities are called " + c.name);
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++) {
        int t = v.terrainOr(c.x + dx, c.y + dy, -1);
        if (t != CITY) {
          cx.issue("city-terrain", c.x + dx, c.y + dy,
                   c.name + "'s footprint stands on " + (t >= 0 ? TERRAIN_NAMES[t] : "the map's edge"));
        }
      }
    int best = std::numeric_limits<int>::max();
    double bestDist = std::numeric_limits<double>::max();
    for (const City& o : m.cities) {
      if (&o == &c) continue;
      auto [gx, gy] = gaps(c, o);
      int gap = overlap(c, o) ? -1 : std::max(gx, gy);
      best = std::min(best, gap);
      bestDist = std::min(bestDist, std::hypot(c.x - o.x, c.y - o.y));
      if (c.index < o.index && gap < MIN_CITY_GAP) {
        cx.issue("city-gap", c.x, c.y,
                 c.name + " and " + o.name + (gap < 0 ? " overlap" : " are " + std::to_string(gap) + " tile(s) apart"));
      }
    }
    if (n > 1) {
      nearestGap.push_back(best);
      nearestDist.push_back(bestDist);
      if (best <= CROWDED_GAP) crowded++;
    }
  }
  // Clark-Evans: the mean nearest-neighbour distance over what random points
  // on the same land would give; < 1 clustered, 1 random, > 1 evenly spread
  int land = 0;
  for (int i = 0; i < v.w * v.h; i++) land += !v.sea(i);
  double mean = 0;
  for (double d : nearestDist) mean += d;
  mean = nearestDist.empty() ? 0 : mean / (double)nearestDist.size();
  double expected = n > 0 && land > 0 ? 0.5 / std::sqrt((double)n / land) : 0;
  cx.metric("cities", n);
  cx.metric("cityGapMin", quantile(nearestGap, 0));
  cx.metric("cityGapP5", quantile(nearestGap, 0.05));
  cx.metric("cityGapMedian", quantile(nearestGap, 0.5));
  cx.metric("crowdedCityFrac", n ? (double)crowded / n : 0);
  cx.metric("clarkEvansR", expected > 0 ? mean / expected : 0);
}

// ---------------------------------------------------------------- roads

void roads(Context& cx) {
  const View& v = cx.v;
  const Map& m = v.m;
  int w = v.w, h = v.h;
  int roadTiles = 0, pieces = 0, mismatched = 0, deadEnds = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (!v.road[i]) continue;
      pieces++;
      // the piece's own arms against what lies beyond them: an arm must meet
      // a roadway or a place, and a road piece pointing back needs an arm
      int shape = ROAD_SHAPE[v.road[i]], joined = 0;
      bool bad = false;
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!v.in(nx, ny)) continue;
        int j = ny * w + nx;
        if (v.roadway(j) || v.destination(j)) joined |= BIT4[d];
        bool back = v.road[j] && (ROAD_SHAPE[v.road[j]] & BIT4[(d + 2) % 4]);
        if (back && !(shape & BIT4[d])) bad = true;
      }
      if (shape & ~joined) bad = true;
      if (bad) mismatched++;
      for (int d = 0; d < 4; d++) {
        if (!(shape & BIT4[d]) || (joined & BIT4[d])) continue;
        if (v.destinationNear(x, y)) continue;
        deadEnds++;
        cx.issue("dead-end", x, y, std::string("the road's ") + "NESW"[d] + " arm leads nowhere");
      }
    }
  }
  // the networks: 4-connected roads and bridges, and what each one reaches
  std::vector<int> comp(w * h, -1);
  int comps = 0, stray = 0, spurs = 0, mainSize = 0;
  std::vector<std::set<int>> compCities;
  for (int s = 0; s < w * h; s++) {
    if (!v.roadway(s) || comp[s] >= 0) continue;
    std::set<int> reached;
    std::set<int> others;   // sites, signposts and crossings, by tile
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
          if (!v.in(x + dx, y + dy)) continue;
          int j = (y + dy) * w + x + dx;
          if (v.cityOf[j] >= 0) reached.insert(v.cityOf[j]);
          if (v.site[j] || v.crossing[j] || v.terrain[j] == TOWER) others.insert(j);
        }
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!v.in(nx, ny)) continue;
        int j = ny * w + nx;
        if (v.roadway(j) && comp[j] < 0) { comp[j] = comps; q.push_back(j); }
      }
    }
    roadTiles += size;
    int ends = (int)reached.size() + (int)others.size();
    if (ends == 0) {
      stray++;
      cx.issue("road-to-nowhere", s % w, s / w, "a road of " + std::to_string(size) + " tile(s) reaching no place");
    } else if (ends == 1) {
      spurs++;
      cx.issue("spur-road", s % w, s / w, "a road of " + std::to_string(size) + " tile(s) reaching one place only");
    }
    if ((int)reached.size() > mainSize) mainSize = (int)reached.size();
    compCities.push_back(reached);
    comps++;
  }
  int n = (int)m.cities.size();
  std::vector<bool> onRoad(n, false);
  for (auto& r : compCities)
    for (int c : r) onRoad[c] = true;
  int linked = (int)std::count(onRoad.begin(), onRoad.end(), true);

  // detours: from each city along its roads to the first other cities met
  std::vector<double> detours;
  for (const City& c : m.cities) {
    std::vector<int> dist(w * h, -1);
    std::deque<int> q;
    for (int dy = -1; dy <= 2; dy++)
      for (int dx = -1; dx <= 2; dx++) {
        int x = c.x + dx, y = c.y + dy;
        if (!v.in(x, y)) continue;
        int i = y * w + x;
        if (v.roadway(i) && v.cityOf[i] < 0) { dist[i] = 1; q.push_back(i); }
      }
    std::set<int> met;
    while (!q.empty()) {
      int i = q.front();
      q.pop_front();
      int x = i % w, y = i / w;
      int other = -1;
      for (int dy = -1; dy <= 1 && other < 0; dy++)
        for (int dx = -1; dx <= 1 && other < 0; dx++)
          if (v.in(x + dx, y + dy)) {
            int k = v.cityOf[(y + dy) * w + x + dx];
            if (k >= 0 && k != c.index) other = k;
          }
      if (other >= 0) {
        if (met.insert(other).second) {
          auto [gx, gy] = gaps(c, m.cities[other]);
          detours.push_back((double)dist[i] / std::max(1, gx + gy));
        }
        continue;   // a road through a city is two roads
      }
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!v.in(nx, ny)) continue;
        int j = ny * w + nx;
        if (v.roadway(j) && dist[j] < 0 && v.cityOf[j] < 0) { dist[j] = dist[i] + 1; q.push_back(j); }
      }
    }
  }
  cx.metric("roadTiles", roadTiles);
  cx.metric("roadNetworks", comps);
  cx.metric("roadsToNowhere", stray);
  cx.metric("spurRoads", spurs);
  cx.metric("spurRoadsPerCity", n ? (double)spurs / n : 0);
  cx.metric("deadEnds", deadEnds);
  cx.metric("deadEndsPer100Road", roadTiles ? 100.0 * deadEnds / roadTiles : 0);
  cx.metric("roadShapeMismatchFrac", pieces ? (double)mismatched / pieces : 0);
  cx.metric("cityRoadCoverage", n ? (double)linked / n : 0);
  cx.metric("mainNetworkCityShare", n ? (double)mainSize / n : 0);
  cx.metric("roadTilesPerCity", n ? (double)roadTiles / n : 0);
  cx.metric("roadDetourMedian", quantile(detours, 0.5));
  cx.metric("roadDetourP95", quantile(detours, 0.95));
}

// ---------------------------------------------------------------- reach

/** Every tile a stack can get to from (x, y): mountains are shut; it changes
 *  between land and sea only through a crossing (a bridge, a marked tile, a
 *  port city's footprint) -- docs/rules.md > Movement. Land only: no sea. */
std::vector<bool> reach(const View& v, int x0, int y0, bool landOnly) {
  int w = v.w, h = v.h;
  std::vector<bool> seen(w * h, false);
  // a road over mountains is a pass (18a9:0896): any road tile costs 1
  auto open = [&](int i) { return (v.terrain[i] != MOUNTAINS || v.road[i]) && !(landOnly && v.sea(i)); };
  auto crossing = [&](int i) { return v.crossing[i] || v.port[i] || v.terrain[i] == BRIDGE; };
  int s = y0 * w + x0;
  if (!v.in(x0, y0) || !open(s)) return seen;
  std::deque<int> q{s};
  seen[s] = true;
  while (!q.empty()) {
    int i = q.front();
    q.pop_front();
    int x = i % w, y = i / w;
    for (int d = 0; d < 8; d++) {
      int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
      if (!v.in(nx, ny)) continue;
      int j = ny * w + nx;
      if (seen[j] || !open(j)) continue;
      if (v.sea(i) != v.sea(j) && !crossing(i) && !crossing(j)) continue;
      seen[j] = true;
      q.push_back(j);
    }
  }
  return seen;
}

void reachability(Context& cx) {
  const View& v = cx.v;
  const Map& m = v.m;
  int w = v.w;
  std::vector<const City*> capitals;
  for (const Side& s : m.sides)
    if (s.capital) capitals.push_back(s.capital);
  double landShare = 0;
  std::set<int> lostCities, lostSites;
  for (const City* cap : capitals) {
    auto all = reach(v, cap->x, cap->y, false);
    auto land = reach(v, cap->x, cap->y, true);
    int byLand = 0;
    for (const City& c : m.cities) {
      if (land[c.y * w + c.x]) byLand++;
      if (!all[c.y * w + c.x] && lostCities.insert(c.index).second)
        cx.issue("unreachable", c.x, c.y, c.name + " cannot be reached from " + cap->name);
    }
    for (const Site& s : m.sites)
      if (!all[s.y * w + s.x] && lostSites.insert(s.index).second)
        cx.issue("unreachable", s.x, s.y, s.name + " cannot be reached from " + cap->name);
    landShare += m.cities.empty() ? 0 : (double)byLand / m.cities.size();
  }
  cx.metric("capitals", capitals.size());
  cx.metric("landReachShare", capitals.empty() ? 0 : landShare / capitals.size());
}

// ---------------------------------------------------------------- fairness

double gini(std::vector<double> v) {
  if (v.size() < 2) return 0;
  std::sort(v.begin(), v.end());
  double sum = 0, weighted = 0;
  for (size_t i = 0; i < v.size(); i++) {
    sum += v[i];
    weighted += (double)(i + 1) * v[i];
  }
  if (sum <= 0) return 0;
  double n = (double)v.size();
  return (2 * weighted) / (n * sum) - (n + 1) / n;
}

/** Each side's start, by travel cost from its capital: how near its rivals
 *  are, and what it can expand into -- the neutral cities it reaches before
 *  anyone else does (a Voronoi diagram by travel cost), their income, and
 *  how many lie within a few turns. */
void fairness(Context& cx) {
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
        if (v.in(c.x + dx, c.y + dy)) t.push_back((c.y + dy) * w + c.x + dx);
    return t;
  };
  auto costTo = [&](const std::vector<int>& field, const City& c) {
    int best = travel::UNREACHED;
    for (int t : footprint(c)) best = std::min(best, field[t]);
    return best;
  };
  std::vector<const City*> caps;
  for (const Side& s : m.sides)
    if (s.capital) caps.push_back(s.capital);
  int k = (int)caps.size();
  if (k < 2) return;
  std::vector<std::vector<int>> field(k);
  for (int i = 0; i < k; i++) field[i] = travel::costs(ground, footprint(*caps[i]));

  // the nearest rival capital, in turns
  std::vector<double> gaps;
  for (int i = 0; i < k; i++) {
    int best = travel::UNREACHED;
    for (int j = 0; j < k; j++)
      if (j != i) best = std::min(best, costTo(field[i], *caps[j]));
    if (best == travel::UNREACHED) continue;
    double turns = (double)best / travel::TURN;
    gaps.push_back(turns);
    for (int j = i + 1; j < k; j++) {
      int c = costTo(field[i], *caps[j]);
      if (c != travel::UNREACHED && (double)c / travel::TURN < CAPITAL_GAP_TURNS)
        cx.issue("capital-gap", caps[i]->x, caps[i]->y,
                 caps[i]->name + " and " + caps[j]->name + " are " +
                     fmt("%.1f", (double)c / travel::TURN) + " turns apart");
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
      int d = costTo(field[i], c);
      if (d < best) { second = best; best = d; who = i; }
      else if (d < second) second = d;
      if (d <= 2 * travel::TURN) near2[i]++;
      if (d <= 4 * travel::TURN) near4[i]++;
    }
    if (who < 0) continue;
    cities[who]++;
    income[who] += c.income;
    if (best > REMOTE_TURNS * travel::TURN) remote++;
    // a city nobody else can contest: the next side is twice as far, and
    // at least two turns further
    if (second == travel::UNREACHED || (second >= 2 * best && second - best >= 2 * travel::TURN)) hinterland[who]++;
  }
  double fair = neutral > 0 ? (double)neutral / k : 1;
  cx.metric("capitalGapMinTurns", gaps.empty() ? 0 : quantile(gaps, 0));
  cx.metric("capitalGapMedianTurns", quantile(gaps, 0.5));
  cx.metric("expansionMinShare", *std::min_element(cities.begin(), cities.end()) / fair);
  cx.metric("expansionMaxShare", *std::max_element(cities.begin(), cities.end()) / fair);
  cx.metric("expansionGini", gini(cities));
  cx.metric("incomeGini", gini(income));
  cx.metric("near2MinShare", *std::min_element(near2.begin(), near2.end()) / fair);
  cx.metric("near4MinShare", *std::min_element(near4.begin(), near4.end()) / fair);
  cx.metric("hinterlandMaxShare", *std::max_element(hinterland.begin(), hinterland.end()) / fair);
  cx.metric("remoteCityFrac", neutral ? (double)remote / neutral : 0);
}

// ---------------------------------------------------------------- terrain

void terrain(Context& cx) {
  const View& v = cx.v;
  const Map& m = v.m;
  int w = v.w, h = v.h, n = w * h;
  int land = 0, coast = 0;
  for (int i = 0; i < n; i++) {
    if (v.sea(i)) continue;
    land++;
    int x = i % w, y = i / w;
    for (int d = 0; d < 4; d++) {
      int t = v.terrainOr(x + DX4[d], y + DY4[d], WATER);
      if (t == WATER || t == SHORE) { coast++; break; }
    }
  }
  // landmasses: 8-connected land, crossing bridges, and the cities on each
  std::vector<int> mass(n, -1);
  std::vector<int> massCities;
  for (int s = 0; s < n; s++) {
    if (v.sea(s) || mass[s] >= 0) continue;
    int id = (int)massCities.size();
    massCities.push_back(0);
    std::deque<int> q{s};
    mass[s] = id;
    while (!q.empty()) {
      int i = q.front();
      q.pop_front();
      int x = i % w, y = i / w;
      for (int d = 0; d < 8; d++) {
        int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
        if (!v.in(nx, ny)) continue;
        int j = ny * w + nx;
        bool through = !v.sea(j) || v.terrain[j] == BRIDGE;
        if (through && mass[j] < 0) { mass[j] = id; q.push_back(j); }
      }
    }
  }
  for (const City& c : m.cities)
    if (v.in(c.x, c.y) && mass[c.y * w + c.x] >= 0) massCities[mass[c.y * w + c.x]]++;
  int inhabited = 0, biggest = 0;
  for (int k : massCities) { inhabited += k > 0; biggest = std::max(biggest, k); }

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
          if (!v.in(nx, ny)) continue;
          int j = ny * w + nx;
          if (v.terrain[j] == type && !seen[j]) { seen[j] = true; q.push_back(j); }
        }
      }
      sizes.push_back(size);
      total += size;
      if (size == 1) lone++;
    }
    median = quantile(sizes, 0.5);
  };
  int lone = 0, total = 0;
  double forest = 0, hills = 0, mountains = 0, marsh = 0;
  patches(FOREST, forest, lone, total);
  patches(HILLS, hills, lone, total);
  patches(MOUNTAINS, mountains, lone, total);
  patches(MARSH, marsh, lone, total);

  cx.metric("landFrac", (double)land / n);
  cx.metric("coastComplexity", land ? coast / std::sqrt((double)land) : 0);
  cx.metric("inhabitedLandmasses", inhabited);
  cx.metric("biggestLandmassCityShare", m.cities.empty() ? 0 : (double)biggest / m.cities.size());
  cx.metric("forestPatchMedian", forest);
  cx.metric("hillsPatchMedian", hills);
  cx.metric("mountainPatchMedian", mountains);
  cx.metric("marshPatchMedian", marsh);
  cx.metric("speckleFrac", total ? (double)lone / total : 0);
}

// ---------------------------------------------------------------- signs, ports

/** 828e:0b51, as the generator gives a signpost's direction. */
int compass(int x1, int y1, int x2, int y2) {
  if (x1 == x2) return y1 < y2 ? 4 : 0;
  if (y1 == y2) return x1 < x2 ? 2 : 6;
  if (x2 < x1 && y2 < y1) return 7;
  if (x2 < x1 && y1 < y2) return 5;
  if (x1 < x2 && y2 < y1) return 1;
  return 3;
}

void signsAndPorts(Context& cx) {
  const View& v = cx.v;
  const Map& m = v.m;
  static const std::regex LEAGUES("^\\s*(\\d+) leagues (\\w+)\\s*$");
  int pointing = 0;
  for (const Sign& s : m.signs) {
    if (v.terrainOr(s.x, s.y, -1) != TOWER)
      cx.issue("signpost", s.x, s.y, "the signpost stands on no signpost tile");
    std::smatch g;
    if (!std::regex_match(s.lines[1], g, LEAGUES)) continue;
    std::vector<const City*> named;   // names may repeat; any of them will do
    for (const City& c : m.cities)
      if (trim(s.lines[0]) == c.name) named.push_back(&c);
    if (named.empty()) {
      cx.issue("signpost", s.x, s.y, "points to \"" + s.lines[0] + "\", which is no city");
      continue;
    }
    pointing++;
    int said = (int)std::stol(g[1]);
    // any tile of the footprint will do: designers may aim at the middle
    bool way = false, far = false;
    for (const City* city : named)
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
          int cxx = city->x + dx, cyy = city->y + dy;
          bool w = COMPASS[compass(s.x, s.y, cxx, cyy)] == g[2];
          int truth = 2 * (int)std::floor(std::hypot(s.x - cxx, s.y - cyy));
          bool f = std::abs(said - truth) <= std::max(4, truth / 4);
          way = way || w;
          far = far || f;
        }
    const std::string& name = named[0]->name;
    if (!way) cx.issue("signpost", s.x, s.y, "says " + std::string(g[2]) + " for " + name);
    if (!far) cx.issue("signpost", s.x, s.y, "says " + std::string(g[1]) + " leagues to " + name);
  }
  int crossings = 0;
  for (int y = 0; y < v.h; y++)
    for (int x = 0; x < v.w; x++) {
      if (!v.crossing[y * v.w + x]) continue;
      crossings++;
      bool sea = false, land = false;
      for (int d = 0; d < 8; d++) {
        int t = v.terrainOr(x + DIRS[d][0], y + DIRS[d][1], -1);
        if (t < 0) continue;
        (t == WATER || t == SHORE ? sea : land) = true;
      }
      if (!sea || !land) cx.issue("port", x, y, "a crossing that does not join land and sea");
    }
  cx.metric("signs", m.signs.size());
  cx.metric("pointingSigns", pointing);
  cx.metric("crossings", crossings);
}

}  // namespace

double Evaluation::metric(const std::string& key, double dflt) const {
  for (auto& [k, v] : metrics)
    if (k == key) return v;
  return dflt;
}

int Evaluation::count(const std::string& check) const {
  return (int)std::count_if(issues.begin(), issues.end(), [&](const Issue& i) { return i.check == check; });
}

const std::vector<std::string>& checks() {
  static const std::vector<std::string> c = {"city-terrain", "city-gap", "city-name", "dead-end", "road-to-nowhere",
                                             "spur-road",    "unreachable", "capital-gap", "signpost", "port"};
  return c;
}

bool isHard(const std::string& check) { return check != "spur-road"; }

bool Evaluation::passed() const {
  return std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return isHard(i.check); });
}

const std::vector<std::string>& scoredMetrics() {
  static const std::vector<std::string> s = {
      "cityGapMin",        "cityGapP5",          "cityGapMedian",       "crowdedCityFrac",
      "clarkEvansR",       "spurRoadsPerCity",   "roadShapeMismatchFrac", "cityRoadCoverage",
      "mainNetworkCityShare", "roadTilesPerCity", "roadDetourMedian",    "roadDetourP95",
      "landReachShare",    "landFrac",           "coastComplexity",     "biggestLandmassCityShare",
      "forestPatchMedian", "hillsPatchMedian",   "mountainPatchMedian", "speckleFrac",
      "capitalGapMinTurns", "capitalGapMedianTurns", "expansionMinShare", "expansionMaxShare",
      "expansionGini",     "incomeGini",         "near4MinShare",       "hinterlandMaxShare",
      "remoteCityFrac"};
  return s;
}

Evaluation evaluate(const Map& map, const std::string& name) {
  Evaluation e;
  e.name = name;
  View v(map);
  Context cx{v, e};
  cities(cx);
  roads(cx);
  reachability(cx);
  fairness(cx);
  terrain(cx);
  signsAndPorts(cx);
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
    r.mean /= (double)maps.size();
    for (const Evaluation& e : maps) r.sd += std::pow(e.metric(k) - r.mean, 2);
    r.sd = std::sqrt(r.sd / (double)maps.size());
    ref[k] = r;
  }
  return ref;
}

Score score(const Evaluation& e, const Reference& ref) {
  Score s;
  for (auto& [k, r] : ref) {
    double x = e.metric(k), out = 0;
    if (x < r.min) out = r.min - x;
    else if (x > r.max) out = x - r.max;
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
