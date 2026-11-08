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

double Uniform(Rng& rng) {
    return rng.Below(65536) / 65536.0;
}
double Mix(double a, double b, double t) {
    return a + t * (b - a);
}
double Dist(double dx, double dy) {
    return std::sqrt(dx * dx + dy * dy);
}

/** Perlin's gradient noise, its permutation drawn from the dice. */
class Noise {
  public:
    explicit Noise(Rng& rng) {
        std::vector<int> p(256);
        std::iota(p.begin(), p.end(), 0);
        rng.Shuffle(p);
        for (int i = 0; i < 512; i++) perm_[i] = p[i & 255];
    }
    /** About -1..1. */
    double At(double x, double y) const {
        int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
        double xf = x - xi, yf = y - yi;
        int cell_x = xi & 255, cell_y = yi & 255;
        auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
        auto grad = [&](int h, double dx, double dy) {
            switch (h & 7) {
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
        double u = fade(xf), v = fade(yf);
        int aa = perm_[perm_[cell_x] + cell_y], ab = perm_[perm_[cell_x] + cell_y + 1];
        int ba = perm_[perm_[cell_x + 1] + cell_y], bb = perm_[perm_[cell_x + 1] + cell_y + 1];
        double x1 = Mix(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
        double x2 = Mix(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
        return Mix(x1, x2, v);
    }
    /** Fractal Brownian motion: octaves of the noise, each twice as fine. */
    double Fbm(double x, double y, int octaves = 5) const {
        double sum = 0, amp = 1, norm = 0;
        for (int o = 0; o < octaves; o++) {
            sum += amp * At(x, y);
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
double Cut(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    double at = std::max(
            0.0, q * static_cast<double>(v.size() - 1));  // a share above 1 asks for the lowest
    size_t k = std::min(v.size() - 1, static_cast<size_t>(at));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct Grid {
    int w, h;
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    /** The 8-connected (or 4-connected) parts of the tiles that pass `test`. */
    std::vector<std::vector<int>> Parts(const std::function<bool(int)>& test,
                                        bool eight = true) const {
        std::vector<int> seen(static_cast<size_t>(w) * h, 0);
        std::vector<std::vector<int>> out;
        for (int s = 0; s < w * h; s++) {
            if (seen[s] || !test(s)) continue;
            std::vector<int> part{s};
            seen[s] = 1;
            for (size_t k = 0; k < part.size(); k++) {
                int i = part[k], x = i % w, y = i / w;
                for (int d = 0; d < (eight ? 8 : 4); d++) {
                    int nx = x + (eight ? kDirs[d][0] : kDX4[d]),
                        ny = y + (eight ? kDirs[d][1] : kDY4[d]);
                    if (!In(nx, ny)) continue;
                    int j = ny * w + nx;
                    if (!seen[j] && test(j)) {
                        seen[j] = 1;
                        part.push_back(j);
                    }
                }
            }
            out.push_back(std::move(part));
        }
        return out;
    }
    /** Steps (4-connected) from the nearest tile passing `from`. */
    std::vector<int> Distance(const std::function<bool(int)>& from) const {
        std::vector<int> d(static_cast<size_t>(w) * h, -1);
        std::deque<int> q;
        for (int i = 0; i < w * h; i++)
            if (from(i)) {
                d[i] = 0;
                q.push_back(i);
            }
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            int x = i % w, y = i / w;
            for (int k = 0; k < 4; k++) {
                int nx = x + kDX4[k], ny = y + kDY4[k];
                if (!In(nx, ny)) continue;
                int j = ny * w + nx;
                if (d[j] < 0) {
                    d[j] = d[i] + 1;
                    q.push_back(j);
                }
            }
        }
        return d;
    }
};

double DefaultLand(Shape s) {
    switch (s) {
    case Shape::kPangaea:
        return 0.60;
    case Shape::kIslands:
        return 0.42;
    default:
        return 0.55;
    }
}

// ------------------------------------------------------------------ land

/** Step 1: where the land is (true) and its height (0..1 inland). */
std::vector<bool> Land(const Settings& s, Rng& rng, const Grid& g, std::vector<double>& height) {
    int w = g.w, h = g.h, n = w * h;
    Noise base(rng), warp1(rng), warp2(rng), channel(rng);
    double f = s.shape == Shape::kIslands ? 1.0 / 26 : 1.0 / 42;
    std::vector<double> e(n);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double px = x * f, py = y * f;
            // domain warping: the noise read where another noise points
            double qx = warp1.Fbm(px + 5.2, py + 1.3, 4), qy = warp2.Fbm(px + 1.7, py + 9.2, 4);
            double v = base.Fbm(px + 1.6 * qx, py + 1.6 * qy, 5);
            // the sea closes in at the map's edges
            double edge = std::min({x, w - 1 - x, y, h - 1 - y}) / 14.0;
            edge = std::min(1.0, edge);
            v -= (1 - edge) * (1 - edge) * 0.9;
            if (s.shape == Shape::kContinents) {
                // straits along the zero lines of a slow noise split the land
                double c = std::abs(channel.Fbm(x / 70.0 + 3.1, y / 70.0 + 7.7, 3));
                if (c < 0.09) v -= 0.45 * (0.09 - c) / 0.09;
            } else if (s.shape == Shape::kPangaea) {
                double dx = (x - w / 2.0) / (w / 2.0), dy = (y - h / 2.0) / (h / 2.0);
                v += 0.45 * (1 - std::min(1.0, std::sqrt(dx * dx + dy * dy)));
            }
            e[y * w + x] = v;
        }
    double share = s.land >= 0 ? s.land : DefaultLand(s.shape);
    double sea = Cut(e, 1 - share);
    std::vector<bool> is_land(n);
    for (int i = 0; i < n; i++) is_land[i] = e[i] > sea;
    // smooth the coast so the tile set can draw it: a tile goes with most of
    // its neighbours
    for (int pass = 0; pass < 2; pass++) {
        auto next = is_land;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int k = 0;
                for (int d = 0; d < 8; d++) {
                    int nx = x + kDirs[d][0], ny = y + kDirs[d][1];
                    k += g.In(nx, ny) && is_land[ny * w + nx];
                }
                if (k >= 5)
                    next[y * w + x] = true;
                else if (k <= 3)
                    next[y * w + x] = false;
            }
        is_land = next;
    }
    // no land on the map's outermost two rows
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) is_land[y * w + x] = false;
    // specks of land go under; small lakes fill in
    int min_island = s.shape == Shape::kIslands ? 40 : 70;
    for (auto& part : g.Parts([&](int i) { return is_land[i]; }))
        if (static_cast<int>(part.size()) < min_island)
            for (int i : part) is_land[i] = false;
    for (auto& part : g.Parts([&](int i) { return !is_land[i]; }, false))
        if (static_cast<int>(part.size()) < 30)
            for (int i : part) is_land[i] = true;
    // the height inland: distance from the coast, roughened by the noise
    auto coast = g.Distance([&](int i) { return !is_land[i]; });
    height.assign(n, 0);
    for (int i = 0; i < n; i++)
        if (is_land[i])
            height[i] = 0.65 * std::min(1.0, coast[i] / 14.0) +
                        0.35 * (e[i] - sea) / std::max(1e-6, 1 - sea);
    return is_land;
}

// ------------------------------------------------------------------ relief

/** Step 2: ranges along ridged noise, hills round them, and a pass into
 *  every pocket of ground the mountains would wall off. */
void Relief(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t,
            const std::vector<double>& height) {
    int w = g.w, n = w * g.h;
    Noise ridges(rng), lumps(rng);
    auto coast = g.Distance([&](int i) { return t[i] == kWater; });
    std::vector<double> score(n, -1), eligible;
    for (int i = 0; i < n; i++) {
        if (t[i] == kWater) continue;
        int x = i % w, y = i / w;
        double r = 1 - std::abs(ridges.Fbm(x / 30.0, y / 30.0, 4));  // ridged: high along lines
        score[i] = r * r + 0.35 * height[i];
        if (coast[i] >= 2) eligible.push_back(score[i]);
    }
    int land_tiles = 0;
    for (int i = 0; i < n; i++) land_tiles += t[i] != kWater;
    // the crests first, at a third of the share, then the ranges widened
    // round them onto the next-highest ground, so they are thick enough to
    // draw as ranges rather than lines
    double m_share = std::min(0.9, s.mountains * land_tiles / std::max<size_t>(1, eligible.size()));
    double crest = Cut(eligible, 1 - m_share / 3), flank = Cut(eligible, 1 - m_share * 1.6);
    for (int i = 0; i < n; i++)
        if (score[i] >= crest && coast[i] >= 2) t[i] = kMountains;
    for (int pass = 0; pass < 2; pass++) {
        auto next = t;
        for (int i = 0; i < n; i++) {
            if (t[i] == kWater || t[i] == kMountains || coast[i] < 2 || score[i] < flank) continue;
            int x = i % w, y = i / w;
            for (int d = 0; d < 8; d++) {
                int nx = x + kDirs[d][0], ny = y + kDirs[d][1];
                if (g.In(nx, ny) && t[ny * w + nx] == kMountains) {
                    next[i] = kMountains;
                    break;
                }
            }
        }
        t = next;
    }
    // hills: the foothills and lumps of their own
    std::vector<double> hscore(n, -1), hs;
    auto near_mount = g.Distance([&](int i) { return t[i] == kMountains; });
    for (int i = 0; i < n; i++) {
        if (t[i] != kPlain) continue;
        int x = i % w, y = i / w;
        double l = lumps.Fbm(x / 13.0 + 11.3, y / 13.0 + 2.9, 3);
        double foot = near_mount[i] >= 0 && near_mount[i] <= 2 ? 0.6 - 0.2 * near_mount[i] : 0;
        hscore[i] = l + foot + 0.2 * score[i];
        hs.push_back(hscore[i]);
    }
    double h_cut =
            Cut(hs, 1 - std::min(0.9, s.hills * land_tiles / std::max<size_t>(1, hs.size())));
    for (int i = 0; i < n; i++)
        if (t[i] == kPlain && hscore[i] >= h_cut) t[i] = kHills;
    // passes: on each landmass, every pocket of open ground the mountains cut
    // off gets the cheapest way through them to the landmass's largest one
    for (auto& mass : g.Parts([&](int i) { return t[i] != kWater; })) {
        std::vector<uint8_t> on_mass(n, 0);
        for (int i : mass) on_mass[i] = 1;
        auto pockets = g.Parts([&](int i) { return on_mass[i] && t[i] != kMountains; });
        if (pockets.size() < 2) continue;
        std::sort(pockets.begin(), pockets.end(), [](auto& a, auto& b) {
            return a.size() != b.size() ? a.size() > b.size() : a[0] < b[0];
        });
        // 0-1 search from the largest: a mountain tile costs 1, open ground 0
        std::vector<int> cost(n, 1 << 30), from(n, -1);
        std::deque<int> q;
        for (int i : pockets[0]) {
            cost[i] = 0;
            q.push_back(i);
        }
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            int x = i % w, y = i / w;
            for (int d = 0; d < 4; d++) {
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!g.In(nx, ny)) continue;
                int j = ny * w + nx;
                if (!on_mass[j]) continue;
                int c = cost[i] + (t[j] == kMountains);
                if (c < cost[j]) {
                    cost[j] = c;
                    from[j] = i;
                    if (t[j] == kMountains)
                        q.push_back(j);
                    else
                        q.push_front(j);
                }
            }
        }
        for (size_t p = 1; p < pockets.size(); p++)
            for (int i = pockets[p][0]; i >= 0 && cost[i] > 0; i = from[i])
                if (t[i] == kMountains) t[i] = kHills;
    }
}

// ------------------------------------------------------------------ rivers

/** Step 3: rivers from the feet of the hills down to the sea (or into
 *  another river), along the cheapest path over a noise field, so they wind
 *  along its valleys. Two tiles wide, as the tile set draws rivers best. */
void Rivers(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t) {
    int w = g.w, n = w * g.h;
    Noise wander(rng);
    auto coast = g.Distance([&](int i) { return t[i] == kWater; });
    auto mount = g.Distance([&](int i) { return t[i] == kMountains || t[i] == kHills; });
    // sources: well inland, by the high ground, apart from each other
    std::vector<int> sources;
    for (int i = 0; i < n; i++)
        if (t[i] != kWater && t[i] != kMountains && t[i] != kHills && coast[i] >= 6 &&
            mount[i] >= 1 && mount[i] <= 2)
            sources.push_back(i);
    rng.Shuffle(sources);
    int want = s.rivers;
    std::vector<uint8_t> river(n, 0);
    std::vector<int> started;
    // the way down: the cheapest path to the sea, a tile costing more where
    // the noise is high, so the river follows the noise's valleys and winds
    std::vector<double> cost(n);
    for (int i = 0; i < n; i++) {
        double v = 0.5 + 0.5 * wander.Fbm((i % w) / 12.0 + 3.3, (i / w) / 12.0 + 1.1, 3);
        cost[i] = 1 + 14 * v * v;
    }
    for (int src : sources) {
        if (static_cast<int>(started.size()) >= want) break;
        bool apart = true;
        for (int o : started)
            if (Dist(o % w - src % w, o / w - src / w) < 22) apart = false;
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
            if (t[i] == kWater || river[i]) {
                end = i;
                break;
            }
            int x = i % w, y = i / w;
            for (int d = 0; d < 4; d++) {
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!g.In(nx, ny)) continue;
                int j = ny * w + nx;
                if (t[j] == kMountains || t[j] == kHills) continue;
                double nc = c + cost[j];
                if (nc < best[j]) {
                    best[j] = nc;
                    from[j] = i;
                    q.push({nc, j});
                }
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
            bool along_x = std::abs(j - i) == 1;
            int ox = along_x ? 0 : 1, oy = along_x ? 1 : 0;
            if (g.In(x + ox, y + oy) && t[(y + oy) * w + x + ox] != kMountains)
                river[(y + oy) * w + x + ox] = 1;
        }
    }
    for (int i = 0; i < n; i++)
        if (river[i]) t[i] = kWater;
}

// ------------------------------------------------------------------ growth

/** Step 4: forest where it is wet, marsh where it is wet and low. */
void Growth(const Settings& s, Rng& rng, const Grid& g, std::vector<uint8_t>& t,
            const std::vector<double>& height) {
    int w = g.w, n = w * g.h;
    Noise wet(rng);
    auto water = g.Distance([&](int i) { return t[i] == kWater; });
    std::vector<double> m(n, -9), ms;
    int land_tiles = 0;
    for (int i = 0; i < n; i++) {
        if (t[i] == kWater) continue;
        land_tiles++;
        if (t[i] != kPlain) continue;
        int x = i % w, y = i / w;
        m[i] = wet.Fbm(x / 26.0 + 4.4, y / 26.0 + 8.8, 4) +
               (water[i] <= 3 ? 0.08 * (4 - water[i]) : 0);
        ms.push_back(m[i]);
    }
    if (ms.empty()) return;
    double f_cut = Cut(ms, 1 - std::min(0.9, s.forest * land_tiles / ms.size()));
    for (int i = 0; i < n; i++)
        if (t[i] == kPlain && m[i] >= f_cut) t[i] = kForest;
    // marsh: a few small patches on the wettest low ground by the water
    std::vector<double> low;
    for (int i = 0; i < n; i++)
        if (t[i] == kPlain && water[i] <= 2) low.push_back(m[i] - height[i]);
    double m_cut = Cut(low, 0.97);
    for (int i = 0; i < n; i++)
        if (t[i] == kPlain && water[i] <= 2 && m[i] - height[i] >= m_cut) t[i] = kMarsh;
}

// ------------------------------------------------------------------ cities, sites

bool FootprintOk(const Grid& g, const std::vector<uint8_t>& t, int x, int y) {
    if (x < 3 || y < 3 || x + 1 >= g.w - 3 || y + 1 >= g.h - 3) return false;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            int k = t[(y + dy) * g.w + x + dx];
            if (k != kPlain && k != kForest) return false;
        }
    return true;
}

/** Best-candidate sampling (Mitchell): each new point the best of a few
 *  random candidates, the best being the furthest from the points already
 *  placed, plus `bonus`. Gives an even spread with no clumps. */
std::vector<std::pair<int, int>> Spread(Rng& rng, const std::vector<int>& spots, int count, int w,
                                        const std::vector<std::pair<int, int>>& avoid,
                                        double min_dist, const std::function<double(int)>& bonus,
                                        int candidates) {
    std::vector<std::pair<int, int>> out;
    if (spots.empty()) return out;
    auto nearest = [&](int x, int y) {
        double best = 1e9;
        for (auto& [ax, ay] : out) best = std::min(best, Dist(ax - x, ay - y));
        for (auto& [ax, ay] : avoid) best = std::min(best, Dist(ax - x, ay - y));
        return best;
    };
    for (int k = 0; k < count; k++) {
        int pick = -1;
        double pick_score = -1e9;
        // `candidates` that keep their distance; on crowded land it may take
        // many draws to find them
        for (int c = 0, found = 0; found < candidates && c < 600; c++) {
            int s = spots[rng.Below(static_cast<int>(spots.size()))];
            int x = s % w, y = s / w;
            double d = nearest(x, y);
            if (d < min_dist) continue;
            found++;
            double score = std::min(d, 12.0) + bonus(s);
            if (score > pick_score) {
                pick_score = score;
                pick = s;
            }
        }
        if (pick < 0) continue;
        out.push_back({pick % w, pick / w});
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

int NameWidth(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z')
            n += kUpperWidth[c - 'A'];
        else if (c >= 'a' && c <= 'z')
            n += kLowerWidth[c - 'a'];
        else if (c == ' ')
            n += 7;
        else if (c == '\'')
            n += 3;
        else
            n += 16;
    }
    return n;
}
bool Fits(const std::string& s) {
    return static_cast<int>(s.size()) <= kNameChars && NameWidth(s) <= kNameWidth;
}

}  // namespace

Title MakeTitle(Shape shape, long seed) {
    Rng rng(static_cast<double>(seed) +
            1000003.0);  // its own dice: naming never touches the world's
    const auto& nouns = kNouns[shape == Shape::kPangaea ? 1 : shape == Shape::kIslands ? 2 : 0];
    Title t;
    std::string noun;
    for (int tries = 0; tries < 50; tries++) {
        std::string place = PickWord(rng, kFirst);  // two statements: the dice in a fixed order
        place += PickWord(rng, kSecond);
        noun = PickWord(rng, nouns);
        std::string adj = PickWord(rng, kAdjectives);
        int form = rng.Below(3);
        if (form == 0)
            t.name = "The " + adj + " " + noun;
        else if (form == 1)
            t.name = noun + " of " + place;
        else
            t.name = place + " " + noun;
        if (Fits(t.name)) break;
    }
    if (!Fits(t.name)) t.name = "The " + noun;  // fits whatever the noun
    t.text = PickWord(rng, kLines);
    return t;
}

bool ParseShape(const std::string& s, Shape& out) {
    if (s == "continents")
        out = Shape::kContinents;
    else if (s == "pangaea")
        out = Shape::kPangaea;
    else if (s == "islands")
        out = Shape::kIslands;
    else
        return false;
    return true;
}

const char* ShapeName(Shape s) {
    switch (s) {
    case Shape::kPangaea:
        return "pangaea";
    case Shape::kIslands:
        return "islands";
    default:
        return "continents";
    }
}

World MakeWorld(const Settings& s, Rng& rng, int w, int h) {
    Grid g{w, h};
    int n = w * h;
    World world;
    std::vector<double> height;
    auto is_land = Land(s, rng, g, height);
    auto& t = world.terrain;
    t.assign(n, kWater);
    for (int i = 0; i < n; i++)
        if (is_land[i]) t[i] = kPlain;
    Relief(s, rng, g, t, height);
    Rivers(s, rng, g, t);
    Growth(s, rng, g, t, height);

    // cities: on open ground with room round them, rather by water
    auto water = g.Distance([&](int i) { return t[i] == kWater; });
    std::vector<int> spots;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (FootprintOk(g, t, x, y)) spots.push_back(y * w + x);
    int want = std::clamp(s.cities, 8, 100);
    world.cities = Spread(
            rng, spots, want, w, {}, 2.9,
            [&](int i) {
                int d = water[i];
                return d >= 0 && d <= 2 ? 0.6 : 0.0;
            },
            4);
    for (auto& [x, y] : world.cities)
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) t[(y + dy) * w + x + dx] = kCity;

    // sites: in the wilds, on ground an army can walk to from a city
    auto open = g.Parts([&](int i) { return t[i] != kWater && t[i] != kMountains; });
    std::vector<int> part(n, -1);
    std::vector<bool> settled(open.size(), false);
    for (size_t k = 0; k < open.size(); k++)
        for (int i : open[k]) part[i] = static_cast<int>(k);
    for (auto& [x, y] : world.cities)
        if (part[y * w + x] >= 0) settled[part[y * w + x]] = true;
    std::vector<int> wild;
    for (int i = 0; i < n; i++) {
        int x = i % w, y = i / w;
        if (t[i] != kPlain || part[i] < 0 || !settled[part[i]]) continue;
        if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) continue;
        bool clear = true;
        for (int dy = -2; dy <= 2 && clear; dy++)
            for (int dx = -2; dx <= 2 && clear; dx++)
                if (g.In(x + dx, y + dy) && t[(y + dy) * w + x + dx] == kCity) clear = false;
        if (clear) wild.push_back(i);
    }
    std::vector<std::pair<int, int>> avoid;
    for (auto& [x, y] : world.cities) avoid.push_back({x, y});
    world.sites = Spread(rng, wild, s.sites, w, avoid, 3.0, [](int) { return 0.0; }, 20);
    for (auto& [x, y] : world.sites) t[y * w + x] = kSite;
    return world;
}

// ------------------------------------------------------------------ roads

Roads PlanRoads(const std::vector<uint8_t>& terrain, int w, int h,
                const std::vector<std::pair<int, int>>& cities, const std::vector<Span>& spans,
                Rng& rng) {
    Grid g{w, h};
    int n = w * h;
    Roads out;
    out.road.assign(n, 0);
    // the cost of building a road over each kind of ground
    auto build = [&](int i) -> double {
        switch (terrain[i]) {
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
    std::vector<std::vector<std::pair<int, int>>> leap(n);  // tile -> (tile beyond, span)
    for (size_t k = 0; k < spans.size(); k++) {
        auto& sp = spans[k];
        int ax = sp.ew ? sp.x - 1 : sp.x, ay = sp.ew ? sp.y : sp.y - 1;
        int bx = sp.ew ? sp.x + 2 : sp.x, by = sp.ew ? sp.y : sp.y + 2;
        if (!g.In(ax, ay) || !g.In(bx, by)) continue;
        int a = ay * w + ax, b = by * w + bx;
        leap[a].push_back({b, static_cast<int>(k)});
        leap[b].push_back({a, static_cast<int>(k)});
    }
    // the tiles a road may leave a city from: the eight beside its footprint
    auto doors = [&](int c) {
        auto [x, y] = cities[c];
        std::vector<int> d;
        const int ring[8][2] = {{0, -1}, {1, -1}, {2, 0}, {2, 1}, {1, 2}, {0, 2}, {-1, 1}, {-1, 0}};
        for (auto& r : ring) {
            int dx = x + r[0], dy = y + r[1];
            if (g.In(dx, dy) && build(dy * w + dx) > 0 && terrain[dy * w + dx] != kMountains)
                d.push_back(dy * w + dx);
        }
        return d;
    };
    std::vector<std::vector<int>> door(cities.size());
    std::vector<int> door_of(n, -1);
    for (size_t c = 0; c < cities.size(); c++) {
        door[c] = doors(static_cast<int>(c));
        for (int i : door[c]) door_of[i] = static_cast<int>(c);
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
        double lx = bx - ax0, ly = by - ay0, len = std::max(1.0, Dist(lx, ly));
        auto off = [&](int i) {
            return 0.08 * std::abs((i % w - ax0) * ly - (i / w - ay0) * lx) / len;
        };
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
            if (door_of[i] == b) {
                end = i;
                break;
            }
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
                int nx = x + kDX4[d], ny = y + kDY4[d];
                if (!g.In(nx, ny)) continue;
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
    struct Link {
        double cost;
        int a, b;
    };
    std::vector<Link> links;
    int m = static_cast<int>(cities.size());
    for (int a = 0; a < m; a++) {
        std::vector<std::pair<double, int>> near;
        for (int b = 0; b < m; b++)
            if (b != a)
                near.push_back({Dist(cities[a].first - cities[b].first,
                                     cities[a].second - cities[b].second),
                                b});
        std::sort(near.begin(), near.end());
        for (int k = 0; k < 4 && k < static_cast<int>(near.size()); k++) {
            int b = near[k].second;
            if (near[k].first > 26 || b < a) continue;
            double c = route(a, b, nullptr, nullptr);
            if (c > 0) links.push_back({c, a, b});
        }
    }
    auto cheaper = [](const Link& p, const Link& q) {
        if (p.cost != q.cost) return p.cost < q.cost;
        return p.a != q.a ? p.a < q.a : p.b < q.b;
    };
    std::sort(links.begin(), links.end(), cheaper);
    // the minimum spanning tree (Kruskal), then a few links where the tree
    // would send a traveller the long way round
    std::vector<int> root(m);
    std::iota(root.begin(), root.end(), 0);
    std::function<int(int)> find = [&](int i) {
        return root[i] == i ? i : root[i] = find(root[i]);
    };
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
    auto tree_cost = [&](int a, int b) {
        std::vector<double> d(m, -1);
        std::deque<int> q{a};
        d[a] = 0;
        while (!q.empty()) {
            int i = q.front();
            q.pop_front();
            for (auto& [j, c] : tree[i])
                if (d[j] < 0) {
                    d[j] = d[i] + c;
                    q.push_back(j);
                }
        }
        return d[b];
    };
    for (auto& l : links) {
        double t = tree_cost(l.a, l.b);
        if (t > 2.2 * l.cost && l.cost <= 24 && Uniform(rng) < 0.7) {
            chosen.push_back(l);
            tree[l.a].push_back({l.b, l.cost});
            tree[l.b].push_back({l.a, l.cost});
        }
    }
    // lay them, cheapest first, each joining what is there already
    std::sort(chosen.begin(), chosen.end(), cheaper);
    std::vector<uint8_t> bridged(spans.size(), 0);
    for (auto& l : chosen) {
        std::vector<int> tiles, used;
        if (route(l.a, l.b, &tiles, &used) < 0) continue;
        for (int i : tiles) out.road[i] = 1;
        for (int k : used) bridged[k] = 1;
    }
    for (size_t k = 0; k < spans.size(); k++)
        if (bridged[k]) out.bridges.push_back(static_cast<int>(k));
    return out;
}

}  // namespace w2::scengen
