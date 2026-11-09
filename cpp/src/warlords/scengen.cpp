/**
 * @file
 * @brief Modern scenario terrain, relief, river, flora, city/site placement, and title generation.
 */
#include "warlords/scengen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <queue>
#include <string_view>

#include "warlords/move.hpp"
#include "warlords/worldgen/scengen_impl.hpp"

namespace w2::scengen {

namespace {

using namespace w2::move;  // Terrain type ids.

/** @brief Returns the default target land share for `shape`. */
double DefaultLand(Shape shape) {
    switch (shape) {
    case Shape::kContinents:
        return 0.55;
    case Shape::kPangaea:
        return 0.60;
    case Shape::kIslands:
        return 0.42;
    }
    return 0.55;
}

// ------------------------------------------------------------------ land

/** @brief Step 1: where the land is (true) and its height (0..1 inland). */
std::vector<bool> Land(const Settings& settings, Rng& rng, const Grid& grid,
                       std::vector<double>& height) {
    int width = grid.Width(), height_tiles = grid.Height(), tile_count = grid.TileCount();
    Noise base(rng), warp1(rng), warp2(rng), channel(rng);
    double freq = settings.shape == Shape::kIslands ? 1.0 / 26 : 1.0 / 42;
    std::vector<double> elev(tile_count);
    for (int y = 0; y < height_tiles; y++)
        for (int x = 0; x < width; x++) {
            double sample_x = x * freq, sample_y = y * freq;
            // Domain warping: sample base noise at coordinates offset by auxiliary fBm fields.
            double warp_x = warp1.Fbm(sample_x + 5.2, sample_y + 1.3, 4),
                   warp_y = warp2.Fbm(sample_x + 1.7, sample_y + 9.2, 4);
            double val = base.Fbm(sample_x + 1.6 * warp_x, sample_y + 1.6 * warp_y, 5);
            // Sea closes in at the map's edges.
            double edge = std::min({x, width - 1 - x, y, height_tiles - 1 - y}) / 14.0;
            edge = std::min(1.0, edge);
            val -= (1 - edge) * (1 - edge) * 0.9;
            if (settings.shape == Shape::kContinents) {
                // Straits along the zero lines of a slow noise split the land.
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
    // Smooth the coast so the tile set can draw it cleanly.
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
    // Keep the outermost two border rows as open sea.
    for (int y = 0; y < height_tiles; y++)
        for (int x = 0; x < width; x++)
            if (x < 2 || y < 2 || x >= width - 2 || y >= height_tiles - 2)
                is_land[y * width + x] = false;
    // Submerge tiny islets and fill small inland lakes.
    int min_island = settings.shape == Shape::kIslands ? 40 : 70;
    for (const auto& part : grid.Parts([&](int tile) { return is_land[tile]; }))
        if (static_cast<int>(part.size()) < min_island)
            for (int tile : part) is_land[tile] = false;
    for (const auto& part : grid.Parts([&](int tile) { return !is_land[tile]; }, false))
        if (static_cast<int>(part.size()) < 30)
            for (int tile : part) is_land[tile] = true;
    // Inland height combines coast distance with normalized noise elevation.
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
    int width = grid.Width(), tile_count = grid.TileCount();
    Noise ridges(rng), lumps(rng);
    auto coast = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    std::vector<double> score(tile_count, -1), eligible;
    for (int i = 0; i < tile_count; i++) {
        if (terrain[i] == kWater) continue;
        int x = i % width, y = i / width;
        double ridge = 1 - std::abs(ridges.Fbm(x / 30.0, y / 30.0, 4));
        score[i] = ridge * ridge + 0.35 * height[i];
        if (coast[i] >= 2) eligible.push_back(score[i]);
    }
    int land_tiles = 0;
    for (int i = 0; i < tile_count; i++) land_tiles += terrain[i] != kWater;
    // Seed crests at one third of target share, then widen onto adjacent high ground.
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
    // Hills form foothills around mountains and independent clusters.
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
    // Carve a minimum-mountain pass from every isolated open pocket to its landmass's main pocket.
    for (const auto& mass : grid.Parts([&](int tile) { return terrain[tile] != kWater; })) {
        std::vector<uint8_t> on_mass(tile_count, 0);
        for (int tile : mass) on_mass[tile] = 1;
        auto pockets =
                grid.Parts([&](int tile) { return on_mass[tile] && terrain[tile] != kMountains; });
        if (pockets.size() < 2) continue;
        std::sort(pockets.begin(), pockets.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.size() != rhs.size() ? lhs.size() > rhs.size() : lhs[0] < rhs[0];
        });
        // 0-1 BFS from the largest pocket: mountain tiles cost 1, open ground costs 0.
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
 *        another river), along the cheapest path over a noise field, two tiles wide.
 */
void Rivers(const Settings& settings, Rng& rng, const Grid& grid, std::vector<uint8_t>& terrain) {
    int width = grid.Width(), tile_count = grid.TileCount();
    Noise wander(rng);
    auto coast = grid.Distance([&](int tile) { return terrain[tile] == kWater; });
    auto mount = grid.Distance(
            [&](int tile) { return terrain[tile] == kMountains || terrain[tile] == kHills; });
    std::vector<int> sources;
    for (int i = 0; i < tile_count; i++)
        if (terrain[i] != kWater && terrain[i] != kMountains && terrain[i] != kHills &&
            coast[i] >= 6 && mount[i] >= 1 && mount[i] <= 2)
            sources.push_back(i);
    rng.Shuffle(sources);
    int want = settings.rivers;
    std::vector<uint8_t> river(tile_count, 0);
    std::vector<int> started;
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
        using QueueItem = std::pair<double, int>;
        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
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
            // Widen to two tiles perpendicular to the flow direction.
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
    int width = grid.Width(), tile_count = grid.TileCount();
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
bool FootprintOk(const Grid& grid, absl::Span<const uint8_t> terrain, int x, int y) {
    int width = grid.Width(), height = grid.Height();
    if (x < 3 || y < 3 || x + 1 >= width - 3 || y + 1 >= height - 3) return false;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
            int terr = terrain[(y + dy) * width + x + dx];
            if (terr != kPlain && terr != kForest) return false;
        }
    return true;
}

/**
 * @brief Mitchell best-candidate blue-noise sampling over `spots`, maximizing distance from
 *        already placed points and `avoid` plus `bonus`.
 */
std::vector<TileCoords> Spread(Rng& rng, absl::Span<const int> spots, int count, int width,
                               absl::Span<const TileCoords> avoid, double min_dist,
                               const BonusFn& bonus, int candidates) {
    std::vector<TileCoords> out;
    if (spots.empty()) return out;
    auto nearest = [&](int x, int y) {
        double best = 1e9;
        for (const auto& [avoid_x, avoid_y] : out)
            best = std::min(best, Dist(avoid_x - x, avoid_y - y));
        for (const auto& [avoid_x, avoid_y] : avoid)
            best = std::min(best, Dist(avoid_x - x, avoid_y - y));
        return best;
    };
    for (int idx = 0; idx < count; idx++) {
        int pick = -1;
        double pick_score = -1e9;
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

constexpr const char* kAdjectives[] = {
    "Shattered", "Sundered", "Burning", "Frozen",  "Whispering", "Forgotten", "Gilded",  "Crimson",
    "Iron",      "Silver",   "Hollow",  "Endless", "Drowned",    "Ashen",     "Emerald", "Howling",
    "Sunken",    "Twilight", "Broken",  "Haunted", "Golden",     "Bitter",    "Wild",    "Lost"};
constexpr const char* kFirst[] = {"Thorn",  "Dusk",   "Ember", "Raven", "Storm", "Frost",
                                  "Gold",   "Wolf",   "Mist",  "Drake", "Shade", "Iron",
                                  "Bright", "Grim",   "Moon",  "Star",  "Oak",   "Black",
                                  "Red",    "Winter", "Wyrm",  "Ash",   "Salt",  "Hawk"};
constexpr const char* kSecond[] = {"mere",  "hold", "vale",  "fell",  "wick", "haven",
                                   "march", "moor", "crest", "reach", "gard", "shire",
                                   "fall",  "wood", "spire", "ford"};
constexpr const char* kNouns[3][8] = {
    {"Kingdoms", "Marches", "Dominions", "Realms", "Baronies", "Lands", "Reaches", "Shires"},
    {"Realm", "Expanse", "Heartland", "Empire", "Dominion", "Wilds", "Steppe", "Highlands"},
    {"Isles", "Archipelago", "Atolls", "Shoals", "Skerries", "Islands", "Straits", "Sea Lords"}};
constexpr const char* kLines[] = {"Eight warlords, one throne",    "Where empires rise and fall",
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

// Glyph advance widths in CHANCE17.FIN; "The Frozen Steppe" spans 140 pixels.
constexpr int kUpperWidth[26] = {15, 16, 13, 17, 15, 18, 15, 16, 12, 14, 15, 14, 18,
                                 16, 15, 15, 15, 16, 14, 13, 16, 17, 24, 16, 13, 14};
constexpr int kLowerWidth[26] = {7, 6, 7, 9, 6, 8, 6, 7, 4, 7,  7, 4, 12,
                                 8, 7, 8, 6, 6, 6, 5, 9, 9, 14, 9, 9, 8};
constexpr int kNameWidth = 140, kNameChars = 20;

/** @brief Pixel width of `text` in the CHANCE17 scenario-list font. */
int NameWidth(std::string_view text) {
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
bool Fits(std::string_view text) {
    return static_cast<int>(text.size()) <= kNameChars && NameWidth(text) <= kNameWidth;
}

}  // namespace

Title MakeTitle(Shape shape, long seed) {
    Rng rng(static_cast<double>(seed) + 1000003.0);  // Isolated stream so naming never alters map.
    const auto& nouns = kNouns[shape == Shape::kPangaea ? 1 : shape == Shape::kIslands ? 2 : 0];
    Title title;
    std::string noun;
    for (int tries = 0; tries < 50; tries++) {
        std::string place = PickWord(rng, kFirst);  // Separate statements keep draw order fixed.
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
    if (!Fits(title.name)) title.name = "The " + noun;
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
    case Shape::kContinents:
        return "continents";
    case Shape::kPangaea:
        return "pangaea";
    case Shape::kIslands:
        return "islands";
    }
    return "continents";
}

World MakeWorld(const Settings& settings, Rng& rng, int width, int height) {
    Grid grid(width, height);
    int tile_count = grid.TileCount();
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

    // Cities: on open ground with room round them, preferring water proximity.
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
    for (const auto& [x, y] : world.cities)
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) terrain[(y + dy) * width + x + dx] = kCity;

    // Sites: in the wilds, on ground an army can walk to from a settled landmass.
    auto open = grid.Parts(
            [&](int tile) { return terrain[tile] != kWater && terrain[tile] != kMountains; });
    std::vector<int> part(tile_count, -1);
    std::vector<bool> settled(open.size(), false);
    for (size_t idx = 0; idx < open.size(); idx++)
        for (int tile : open[idx]) part[tile] = static_cast<int>(idx);
    for (const auto& [x, y] : world.cities)
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
    world.sites = Spread(
            rng, wild, settings.sites, width, world.cities, 3.0, [](int) { return 0.0; }, 20);
    for (const auto& [x, y] : world.sites) terrain[y * width + x] = kSite;
    return world;
}

}  // namespace w2::scengen
