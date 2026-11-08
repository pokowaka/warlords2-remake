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

using namespace w2::move;   // terrain type ids

constexpr int DX4[4] = {0, 1, 0, -1}, DY4[4] = {-1, 0, 1, 0};

double uniform(Rng& rng) { return rng.below(65536) / 65536.0; }

/** Perlin's gradient noise, its permutation drawn from the dice. */
class Noise {
 public:
  explicit Noise(Rng& rng) {
    std::vector<int> p(256);
    std::iota(p.begin(), p.end(), 0);
    rng.shuffle(p);
    for (int i = 0; i < 512; i++) perm_[i] = p[i & 255];
  }
  /** About -1..1. */
  double at(double x, double y) const {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    double xf = x - xi, yf = y - yi;
    int X = xi & 255, Y = yi & 255;
    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto grad = [&](int h, double dx, double dy) {
      switch (h & 7) {
        case 0: return dx + dy;
        case 1: return dx - dy;
        case 2: return -dx + dy;
        case 3: return -dx - dy;
        case 4: return dx;
        case 5: return -dx;
        case 6: return dy;
        default: return -dy;
      }
    };
    double u = fade(xf), v = fade(yf);
    int aa = perm_[perm_[X] + Y], ab = perm_[perm_[X] + Y + 1];
    int ba = perm_[perm_[X + 1] + Y], bb = perm_[perm_[X + 1] + Y + 1];
    double x1 = std::lerp(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
    double x2 = std::lerp(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
    return std::lerp(x1, x2, v);
  }
  /** Fractal Brownian motion: octaves of the noise, each twice as fine. */
  double fbm(double x, double y, int octaves = 5) const {
    double sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; o++) {
      sum += amp * at(x, y);
      norm += amp;
      amp *= 0.5;
      x *= 2.03;
      y *= 2.03;
    }
    return sum / norm;
  }

 private:
  std::array<int, 512> perm_{};
};

/** The value at quantile q (0..1) of the listed values. */
double cut(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  size_t k = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

struct Grid {
  int w, h;
  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  /** The 8-connected (or 4-connected) parts of the tiles that pass `test`. */
  std::vector<std::vector<int>> parts(const std::function<bool(int)>& test, bool eight = true) const {
    std::vector<int> seen(w * h, 0);
    std::vector<std::vector<int>> out;
    for (int s = 0; s < w * h; s++) {
      if (seen[s] || !test(s)) continue;
      std::vector<int> part{s};
      seen[s] = 1;
      for (size_t k = 0; k < part.size(); k++) {
        int i = part[k], x = i % w, y = i / w;
        for (int d = 0; d < (eight ? 8 : 4); d++) {
          int nx = x + (eight ? DIRS[d][0] : DX4[d]), ny = y + (eight ? DIRS[d][1] : DY4[d]);
          if (!in(nx, ny)) continue;
          int j = ny * w + nx;
          if (!seen[j] && test(j)) { seen[j] = 1; part.push_back(j); }
        }
      }
      out.push_back(std::move(part));
    }
    return out;
  }
  /** Steps (4-connected) from the nearest tile passing `from`. */
  std::vector<int> distance(const std::function<bool(int)>& from) const {
    std::vector<int> d(w * h, -1);
    std::deque<int> q;
    for (int i = 0; i < w * h; i++)
      if (from(i)) { d[i] = 0; q.push_back(i); }
    while (!q.empty()) {
      int i = q.front();
      q.pop_front();
      int x = i % w, y = i / w;
      for (int k = 0; k < 4; k++) {
        int nx = x + DX4[k], ny = y + DY4[k];
        if (!in(nx, ny)) continue;
        int j = ny * w + nx;
        if (d[j] < 0) { d[j] = d[i] + 1; q.push_back(j); }
      }
    }
    return d;
  }
};

double defaultLand(Shape s) {
  switch (s) {
    case Shape::Pangaea: return 0.60;
    case Shape::Islands: return 0.42;
    default: return 0.55;
  }
}

// ------------------------------------------------------------------ land

/** Step 1: where the land is (true) and its height (0..1 inland). */
std::vector<bool> land(const Settings& s, Rng& rng, const Grid& g, std::vector<double>& height) {
  int w = g.w, h = g.h, n = w * h;
  Noise base(rng), warp1(rng), warp2(rng), channel(rng);
  double f = s.shape == Shape::Islands ? 1.0 / 26 : 1.0 / 42;
  std::vector<double> e(n);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double px = x * f, py = y * f;
      // domain warping: the noise read where another noise points
      double qx = warp1.fbm(px + 5.2, py + 1.3, 4), qy = warp2.fbm(px + 1.7, py + 9.2, 4);
      double v = base.fbm(px + 1.6 * qx, py + 1.6 * qy, 5);
      // the sea closes in at the map's edges
      double edge = std::min({x, w - 1 - x, y, h - 1 - y}) / 14.0;
      edge = std::min(1.0, edge);
      v -= (1 - edge) * (1 - edge) * 0.9;
      if (s.shape == Shape::Continents) {
        // straits along the zero lines of a slow noise split the land
        double c = std::abs(channel.fbm(x / 70.0 + 3.1, y / 70.0 + 7.7, 3));
        if (c < 0.09) v -= 0.45 * (0.09 - c) / 0.09;
      } else if (s.shape == Shape::Pangaea) {
        double dx = (x - w / 2.0) / (w / 2.0), dy = (y - h / 2.0) / (h / 2.0);
        v += 0.45 * (1 - std::min(1.0, std::sqrt(dx * dx + dy * dy)));
      }
      e[y * w + x] = v;
    }
  double share = s.land >= 0 ? s.land : defaultLand(s.shape);
  double sea = cut(e, 1 - share);
  std::vector<bool> isLand(n);
  for (int i = 0; i < n; i++) isLand[i] = e[i] > sea;
  // smooth the coast so the tile set can draw it: a tile goes with most of
  // its neighbours
  for (int pass = 0; pass < 2; pass++) {
    auto next = isLand;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int k = 0;
        for (int d = 0; d < 8; d++) {
          int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
          k += g.in(nx, ny) && isLand[ny * w + nx];
        }
        if (k >= 5) next[y * w + x] = true;
        else if (k <= 3) next[y * w + x] = false;
      }
    isLand = next;
  }
  // no land on the map's outermost two rows
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) isLand[y * w + x] = false;
  // specks of land go under; small lakes fill in
  int minIsland = s.shape == Shape::Islands ? 40 : 70;
  for (auto& part : g.parts([&](int i) { return isLand[i]; }))
    if ((int)part.size() < minIsland)
      for (int i : part) isLand[i] = false;
  for (auto& part : g.parts([&](int i) { return !isLand[i]; }, false))
    if ((int)part.size() < 30)
      for (int i : part) isLand[i] = true;
  // the height inland: distance from the coast, roughened by the noise
  auto coast = g.distance([&](int i) { return !isLand[i]; });
  height.assign(n, 0);
  for (int i = 0; i < n; i++)
    if (isLand[i]) height[i] = 0.65 * std::min(1.0, coast[i] / 14.0) + 0.35 * (e[i] - sea) / std::max(1e-6, 1 - sea);
  return isLand;
}

// ------------------------------------------------------------------ relief

/** Step 2: ranges along ridged noise, hills round them, and a pass into
 *  every pocket of ground the mountains would wall off. */
void relief(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t, const std::vector<double>& height) {
  int w = g.w, n = w * g.h;
  Noise ridges(rng), lumps(rng);
  auto coast = g.distance([&](int i) { return t[i] == WATER; });
  std::vector<double> score(n, -1), eligible;
  for (int i = 0; i < n; i++) {
    if (t[i] == WATER) continue;
    int x = i % w, y = i / w;
    double r = 1 - std::abs(ridges.fbm(x / 30.0, y / 30.0, 4));   // ridged: high along lines
    score[i] = r * r + 0.35 * height[i];
    if (coast[i] >= 2) eligible.push_back(score[i]);
  }
  int landTiles = 0;
  for (int i = 0; i < n; i++) landTiles += t[i] != WATER;
  // the crests first, at a third of the share, then the ranges widened
  // round them onto the next-highest ground, so they are thick enough to
  // draw as ranges rather than lines
  double mShare = std::min(0.9, s.mountains * landTiles / std::max<size_t>(1, eligible.size()));
  double crest = cut(eligible, 1 - mShare / 3), flank = cut(eligible, 1 - mShare * 1.6);
  for (int i = 0; i < n; i++)
    if (score[i] >= crest && coast[i] >= 2) t[i] = MOUNTAINS;
  for (int pass = 0; pass < 2; pass++) {
    auto next = t;
    for (int i = 0; i < n; i++) {
      if (t[i] == WATER || t[i] == MOUNTAINS || coast[i] < 2 || score[i] < flank) continue;
      int x = i % w, y = i / w;
      for (int d = 0; d < 8; d++) {
        int nx = x + DIRS[d][0], ny = y + DIRS[d][1];
        if (g.in(nx, ny) && t[ny * w + nx] == MOUNTAINS) { next[i] = MOUNTAINS; break; }
      }
    }
    t = next;
  }
  // hills: the foothills and lumps of their own
  std::vector<double> hscore(n, -1), hs;
  auto nearMount = g.distance([&](int i) { return t[i] == MOUNTAINS; });
  for (int i = 0; i < n; i++) {
    if (t[i] != PLAIN) continue;
    int x = i % w, y = i / w;
    double l = lumps.fbm(x / 13.0 + 11.3, y / 13.0 + 2.9, 3);
    double foot = nearMount[i] >= 0 && nearMount[i] <= 2 ? 0.6 - 0.2 * nearMount[i] : 0;
    hscore[i] = l + foot + 0.2 * score[i];
    hs.push_back(hscore[i]);
  }
  double hCut = cut(hs, 1 - std::min(0.9, s.hills * landTiles / std::max<size_t>(1, hs.size())));
  for (int i = 0; i < n; i++)
    if (t[i] == PLAIN && hscore[i] >= hCut) t[i] = HILLS;
  // passes: on each landmass, every pocket of open ground the mountains cut
  // off gets the cheapest way through them to the landmass's largest one
  for (auto& mass : g.parts([&](int i) { return t[i] != WATER; })) {
    std::vector<uint8_t> onMass(n, 0);
    for (int i : mass) onMass[i] = 1;
    auto pockets = g.parts([&](int i) { return onMass[i] && t[i] != MOUNTAINS; });
    if (pockets.size() < 2) continue;
    std::sort(pockets.begin(), pockets.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    // 0-1 search from the largest: a mountain tile costs 1, open ground 0
    std::vector<int> cost(n, 1 << 30), from(n, -1);
    std::deque<int> q;
    for (int i : pockets[0]) { cost[i] = 0; q.push_back(i); }
    while (!q.empty()) {
      int i = q.front();
      q.pop_front();
      int x = i % w, y = i / w;
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        int j = ny * w + nx;
        if (!onMass[j]) continue;
        int c = cost[i] + (t[j] == MOUNTAINS);
        if (c < cost[j]) {
          cost[j] = c;
          from[j] = i;
          if (t[j] == MOUNTAINS) q.push_back(j); else q.push_front(j);
        }
      }
    }
    for (size_t p = 1; p < pockets.size(); p++)
      for (int i = pockets[p][0]; i >= 0 && cost[i] > 0; i = from[i])
        if (t[i] == MOUNTAINS) t[i] = HILLS;
  }
}

// ------------------------------------------------------------------ rivers

/** Step 3: rivers from the feet of the hills down to the sea (or into
 *  another river), along the cheapest path over a noise field, so they wind
 *  along its valleys. Two tiles wide, as the tile set draws rivers best. */
void rivers(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t) {
  int w = g.w, n = w * g.h;
  Noise wander(rng);
  auto coast = g.distance([&](int i) { return t[i] == WATER; });
  auto mount = g.distance([&](int i) { return t[i] == MOUNTAINS || t[i] == HILLS; });
  // sources: well inland, by the high ground, apart from each other
  std::vector<int> sources;
  for (int i = 0; i < n; i++)
    if (t[i] != WATER && t[i] != MOUNTAINS && t[i] != HILLS && coast[i] >= 6 && mount[i] >= 1 && mount[i] <= 2)
      sources.push_back(i);
  rng.shuffle(sources);
  int want = s.rivers;
  std::vector<uint8_t> river(n, 0);
  std::vector<int> started;
  // the way down: the cheapest path to the sea, a tile costing more where
  // the noise is high, so the river follows the noise's valleys and winds
  std::vector<double> cost(n);
  for (int i = 0; i < n; i++) cost[i] = 1 + 14 * std::pow(0.5 + 0.5 * wander.fbm((i % w) / 12.0 + 3.3, (i / w) / 12.0 + 1.1, 3), 2);
  for (int src : sources) {
    if ((int)started.size() >= want) break;
    bool apart = true;
    for (int o : started)
      if (std::hypot(o % w - src % w, o / w - src / w) < 22) apart = false;
    if (!apart) continue;
    std::vector<double> best(n, 1e18);
    std::vector<int> from(n, -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    best[src] = 0;
    q.push({0, src});
    int end = -1;
    while (!q.empty()) {
      auto [c, i] = q.top();
      q.pop();
      if (c > best[i]) continue;
      if (t[i] == WATER || river[i]) { end = i; break; }
      int x = i % w, y = i / w;
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        int j = ny * w + nx;
        if (t[j] == MOUNTAINS || t[j] == HILLS) continue;
        double nc = c + cost[j];
        if (nc < best[j]) { best[j] = nc; from[j] = i; q.push({nc, j}); }
      }
    }
    if (end < 0) continue;
    std::vector<int> path;
    for (int i = from[end]; i >= 0; i = from[i]) path.push_back(i);
    std::reverse(path.begin(), path.end());
    if (path.size() < 12) continue;
    started.push_back(src);
    for (size_t k = 0; k < path.size(); k++) {
      int i = path[k], x = i % w, y = i / w;
      river[i] = 1;
      // the second tile: beside it, across the way the river runs
      int j = k + 1 < path.size() ? path[k + 1] : (k > 0 ? path[k - 1] : i);
      bool alongX = std::abs(j - i) == 1;
      int ox = alongX ? 0 : 1, oy = alongX ? 1 : 0;
      if (g.in(x + ox, y + oy) && t[(y + oy) * w + x + ox] != MOUNTAINS) river[(y + oy) * w + x + ox] = 1;
    }
  }
  for (int i = 0; i < n; i++)
    if (river[i]) t[i] = WATER;
}

// ------------------------------------------------------------------ growth

/** Step 4: forest where it is wet, marsh where it is wet and low. */
void growth(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t, const std::vector<double>& height) {
  int w = g.w, n = w * g.h;
  Noise wet(rng);
  auto water = g.distance([&](int i) { return t[i] == WATER; });
  std::vector<double> m(n, -9), ms;
  int landTiles = 0;
  for (int i = 0; i < n; i++) {
    if (t[i] == WATER) continue;
    landTiles++;
    if (t[i] != PLAIN) continue;
    int x = i % w, y = i / w;
    m[i] = wet.fbm(x / 26.0 + 4.4, y / 26.0 + 8.8, 4) + (water[i] <= 3 ? 0.08 * (4 - water[i]) : 0);
    ms.push_back(m[i]);
  }
  if (ms.empty()) return;
  double fCut = cut(ms, 1 - std::min(0.9, s.forest * landTiles / ms.size()));
  for (int i = 0; i < n; i++)
    if (t[i] == PLAIN && m[i] >= fCut) t[i] = FOREST;
  // marsh: a few small patches on the wettest low ground by the water
  std::vector<double> low;
  for (int i = 0; i < n; i++)
    if (t[i] == PLAIN && water[i] <= 2) low.push_back(m[i] - height[i]);
  double mCut = cut(low, 0.97);
  for (int i = 0; i < n; i++)
    if (t[i] == PLAIN && water[i] <= 2 && m[i] - height[i] >= mCut) t[i] = MARSH;
}

// ------------------------------------------------------------------ cities, sites

bool footprintOk(const Grid& g, const std::vector<uint8_t>& t, int x, int y) {
  if (x < 3 || y < 3 || x + 1 >= g.w - 3 || y + 1 >= g.h - 3) return false;
  for (int dy = 0; dy <= 1; dy++)
    for (int dx = 0; dx <= 1; dx++) {
      int k = t[(y + dy) * g.w + x + dx];
      if (k != PLAIN && k != FOREST) return false;
    }
  return true;
}

/** Best-candidate sampling (Mitchell): each new point the best of a few
 *  random candidates, the best being the furthest from the points already
 *  placed, plus `bonus`. Gives an even spread with no clumps. */
std::vector<std::pair<int, int>> spread(Rng& rng, const std::vector<int>& spots, int count, int w,
                                        const std::vector<std::pair<int, int>>& avoid, double minDist,
                                        const std::function<double(int)>& bonus, int candidates) {
  std::vector<std::pair<int, int>> out;
  if (spots.empty()) return out;
  auto nearest = [&](int x, int y) {
    double best = 1e9;
    for (auto& [ax, ay] : out) best = std::min(best, std::hypot(ax - x, ay - y));
    for (auto& [ax, ay] : avoid) best = std::min(best, std::hypot(ax - x, ay - y));
    return best;
  };
  for (int k = 0; k < count; k++) {
    int pick = -1;
    double pickScore = -1e9;
    // `candidates` that keep their distance; on crowded land it may take
    // many draws to find them
    for (int c = 0, found = 0; found < candidates && c < 600; c++) {
      int s = spots[rng.below((int)spots.size())];
      int x = s % w, y = s / w;
      double d = nearest(x, y);
      if (d < minDist) continue;
      found++;
      double score = std::min(d, 12.0) + bonus(s);
      if (score > pickScore) { pickScore = score; pick = s; }
    }
    if (pick < 0) continue;
    out.push_back({pick % w, pick / w});
  }
  return out;
}

}  // namespace

bool parseShape(const std::string& s, Shape& out) {
  if (s == "continents") out = Shape::Continents;
  else if (s == "pangaea") out = Shape::Pangaea;
  else if (s == "islands") out = Shape::Islands;
  else return false;
  return true;
}

const char* shapeName(Shape s) {
  switch (s) {
    case Shape::Pangaea: return "pangaea";
    case Shape::Islands: return "islands";
    default: return "continents";
  }
}

World makeWorld(const Settings& s, Rng& rng, int w, int h) {
  Grid g{w, h};
  int n = w * h;
  World world;
  std::vector<double> height;
  auto isLand = land(s, rng, g, height);
  auto& t = world.terrain;
  t.assign(n, WATER);
  for (int i = 0; i < n; i++)
    if (isLand[i]) t[i] = PLAIN;
  relief(s, rng, g, t, height);
  rivers(s, rng, g, t);
  growth(s, rng, g, t, height);

  // cities: on open ground with room round them, rather by water
  auto water = g.distance([&](int i) { return t[i] == WATER; });
  std::vector<int> spots;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (footprintOk(g, t, x, y)) spots.push_back(y * w + x);
  int want = std::clamp(s.cities, 8, 100);
  world.cities = spread(rng, spots, want, w, {}, 2.9, [&](int i) {
    int d = water[i];
    return d >= 0 && d <= 2 ? 0.6 : 0.0;
  }, 4);
  for (auto& [x, y] : world.cities)
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++) t[(y + dy) * w + x + dx] = CITY;

  // sites: in the wilds, on ground an army can walk to from a city
  auto open = g.parts([&](int i) { return t[i] != WATER && t[i] != MOUNTAINS; });
  std::vector<int> part(n, -1);
  std::vector<bool> settled(open.size(), false);
  for (size_t k = 0; k < open.size(); k++)
    for (int i : open[k]) part[i] = (int)k;
  for (auto& [x, y] : world.cities)
    if (part[y * w + x] >= 0) settled[part[y * w + x]] = true;
  std::vector<int> wild;
  for (int i = 0; i < n; i++) {
    int x = i % w, y = i / w;
    if (t[i] != PLAIN || part[i] < 0 || !settled[part[i]]) continue;
    if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) continue;
    bool clear = true;
    for (int dy = -2; dy <= 2 && clear; dy++)
      for (int dx = -2; dx <= 2 && clear; dx++)
        if (g.in(x + dx, y + dy) && t[(y + dy) * w + x + dx] == CITY) clear = false;
    if (clear) wild.push_back(i);
  }
  std::vector<std::pair<int, int>> avoid;
  for (auto& [x, y] : world.cities) avoid.push_back({x, y});
  world.sites = spread(rng, wild, s.sites, w, avoid, 3.0, [](int) { return 0.0; }, 20);
  for (auto& [x, y] : world.sites) t[y * w + x] = SITE;
  return world;
}

// ------------------------------------------------------------------ roads

Roads planRoads(const std::vector<uint8_t>& terrain, int w, int h, const std::vector<std::pair<int, int>>& cities,
                const std::vector<Span>& spans, Rng& rng) {
  Grid g{w, h};
  int n = w * h;
  Roads out;
  out.road.assign(n, 0);
  // the cost of building a road over each kind of ground
  auto build = [&](int i) -> double {
    switch (terrain[i]) {
      case PLAIN: return 1;
      case FOREST: return 2;
      case HILLS: return 3;
      case MARSH: return 5;
      case MOUNTAINS: return 9;
      default: return -1;   // water, shore, bridges, cities, sites, signposts
    }
  };
  // a bridge span: from the ground before it to the ground after it
  std::vector<std::vector<std::pair<int, int>>> leap(n);   // tile -> (tile beyond, span)
  for (size_t k = 0; k < spans.size(); k++) {
    auto& sp = spans[k];
    int ax = sp.ew ? sp.x - 1 : sp.x, ay = sp.ew ? sp.y : sp.y - 1;
    int bx = sp.ew ? sp.x + 2 : sp.x, by = sp.ew ? sp.y : sp.y + 2;
    if (!g.in(ax, ay) || !g.in(bx, by)) continue;
    int a = ay * w + ax, b = by * w + bx;
    leap[a].push_back({b, (int)k});
    leap[b].push_back({a, (int)k});
  }
  // the tiles a road may leave a city from: the eight beside its footprint
  auto doors = [&](int c) {
    auto [x, y] = cities[c];
    std::vector<int> d;
    const int ring[8][2] = {{0, -1}, {1, -1}, {2, 0}, {2, 1}, {1, 2}, {0, 2}, {-1, 1}, {-1, 0}};
    for (auto& r : ring) {
      int dx = x + r[0], dy = y + r[1];
      if (g.in(dx, dy) && build(dy * w + dx) > 0 && terrain[dy * w + dx] != MOUNTAINS) d.push_back(dy * w + dx);
    }
    return d;
  };
  std::vector<std::vector<int>> door(cities.size());
  std::vector<int> doorOf(n, -1);
  for (size_t c = 0; c < cities.size(); c++) {
    door[c] = doors((int)c);
    for (int i : door[c]) doorOf[i] = (int)c;
  }
  // A*: the cheapest road from any door of a to any door of b, existing
  // roads cheap so new ones join them; returns its cost and tiles
  auto route = [&](int a, int b, std::vector<int>* tiles, std::vector<int>* used) -> double {
    if (door[a].empty() || door[b].empty()) return -1;
    auto [bx, by] = cities[b];
    auto [ax0, ay0] = cities[a];
    auto guess = [&](int i) { return 0.5 * (std::abs(i % w - bx) + std::abs(i / w - by)); };
    // a little extra the further a tile is off the straight line, so a road
    // runs as a staircase along it rather than as one long L
    double lx = bx - ax0, ly = by - ay0, len = std::max(1.0, std::hypot(lx, ly));
    auto off = [&](int i) { return 0.08 * std::abs((i % w - ax0) * ly - (i / w - ay0) * lx) / len; };
    std::vector<double> cost(n, 1e18);
    std::vector<int> from(n, -1), via(n, -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (int i : door[a]) {
      cost[i] = out.road[i] ? 0.3 : build(i);
      q.push({cost[i] + guess(i), i});
    }
    int end = -1;
    while (!q.empty()) {
      auto [f, i] = q.top();
      q.pop();
      if (f - guess(i) > cost[i] + 1e-9) continue;
      if (doorOf[i] == b) { end = i; break; }
      int x = i % w, y = i / w;
      auto relax = [&](int j, double c, int span) {
        if (c < cost[j]) {
          cost[j] = c;
          from[j] = i;
          via[j] = span;
          q.push({c + guess(j), j});
        }
      };
      for (int d = 0; d < 4; d++) {
        int nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        int j = ny * w + nx;
        double c = build(j);
        if (c < 0) continue;
        relax(j, cost[i] + (out.road[j] ? 0.3 : c + off(j)), -1);
      }
      for (auto& [j, span] : leap[i]) relax(j, cost[i] + 4, span);
    }
    if (end < 0) return -1;
    if (tiles) {
      for (int i = end; i >= 0; i = from[i]) {
        tiles->push_back(i);
        if (used && via[i] >= 0) used->push_back(via[i]);
      }
    }
    return cost[end];
  };
  // the links worth having: each city's nearest few, by travel cost
  struct Link { double cost; int a, b; };
  std::vector<Link> links;
  int m = (int)cities.size();
  for (int a = 0; a < m; a++) {
    std::vector<std::pair<double, int>> near;
    for (int b = 0; b < m; b++)
      if (b != a) near.push_back({std::hypot(cities[a].first - cities[b].first, cities[a].second - cities[b].second), b});
    std::sort(near.begin(), near.end());
    for (int k = 0; k < 4 && k < (int)near.size(); k++) {
      int b = near[k].second;
      if (near[k].first > 26 || b < a) continue;
      double c = route(a, b, nullptr, nullptr);
      if (c > 0) links.push_back({c, a, b});
    }
  }
  std::sort(links.begin(), links.end(), [](auto& p, auto& q) { return p.cost < q.cost; });
  // the minimum spanning tree (Kruskal), then a few links where the tree
  // would send a traveller the long way round
  std::vector<int> root(m);
  std::iota(root.begin(), root.end(), 0);
  std::function<int(int)> find = [&](int i) { return root[i] == i ? i : root[i] = find(root[i]); };
  std::vector<std::vector<std::pair<int, double>>> tree(m);
  std::vector<Link> chosen;
  for (auto& l : links) {
    int ra = find(l.a), rb = find(l.b);
    if (ra == rb || l.cost > 34) continue;
    root[ra] = rb;
    chosen.push_back(l);
    tree[l.a].push_back({l.b, l.cost});
    tree[l.b].push_back({l.a, l.cost});
  }
  auto treeCost = [&](int a, int b) {
    std::vector<double> d(m, -1);
    std::deque<int> q{a};
    d[a] = 0;
    while (!q.empty()) {
      int i = q.front();
      q.pop_front();
      for (auto& [j, c] : tree[i])
        if (d[j] < 0) { d[j] = d[i] + c; q.push_back(j); }
    }
    return d[b];
  };
  for (auto& l : links) {
    double t = treeCost(l.a, l.b);
    if (t > 2.2 * l.cost && l.cost <= 24 && uniform(rng) < 0.7) {
      chosen.push_back(l);
      tree[l.a].push_back({l.b, l.cost});
      tree[l.b].push_back({l.a, l.cost});
    }
  }
  // lay them, cheapest first, each joining what is there already
  std::sort(chosen.begin(), chosen.end(), [](auto& p, auto& q) { return p.cost < q.cost; });
  std::vector<uint8_t> bridged(spans.size(), 0);
  for (auto& l : chosen) {
    std::vector<int> tiles, used;
    if (route(l.a, l.b, &tiles, &used) < 0) continue;
    for (int i : tiles) out.road[i] = 1;
    for (int k : used) bridged[k] = 1;
  }
  for (size_t k = 0; k < spans.size(); k++)
    if (bridged[k]) out.bridges.push_back((int)k);
  return out;
}

}  // namespace w2::scengen
