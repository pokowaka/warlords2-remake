/**
 * @file
 * @brief The random map and scenario generator.
 *
 * RANDOM/RANDOM.DAT holds both the generator's parameters and its working
 * memory: the 0xa560-byte buffer includes the 112x156 grid of terrain types at
 * offset 0x6120. Reads off the edge of the grid land in the buffer where the
 * original's did; writes off the grid are dropped.
 *
 * Every die roll is its own statement so evaluation order is deterministic.
 */
#include "warlords/randommap.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/move.hpp"
#include "warlords/travel.hpp"
#include "warlords/types.hpp"

namespace w2::randommap {

const char* const kFiles[6] = {"SCN", "MAP", "RD", "SGN", "CTY", "SPC"};
const int kSliderShows[4][7] = {{5, 7, 9, 11, 13, 15, 17},
                                {5, 7, 9, 11, 13, 15, 17},
                                {70, 75, 80, 85, 90, 95, 100},
                                {9, 11, 13, 15, 17, 19, 21}};
const char* const kSliderFormats[4] = {"(%d%%)", "(%d%%)", "(%d)", "(%d%%)"};

namespace {

// terrain type ids, as the grid and the scenario's tile table number them
constexpr int kRoad = 0, kBridge = 1, kWater = 2, kShore = 3, kForest = 4, kHills = 5;
constexpr int kMountains = 6, kPlain = 7, kMarsh = 8, kTower = 9, kCity = 10, kSite = 11;

// RANDOM.DAT: the parameters (the sliders add to these)
constexpr int kPCities = 0x2a, kPEdgePoints = 0x2c;
constexpr int kPMountains = 0x34, kPHills = 0x36, kPWideRivers = 0x38, kPRivers = 0x3a;
constexpr int kPForest = 0x3c, kPPasses = 0x3e, kPErosion = 0x40;
constexpr int kSliderHills = 0x44, kSliderWater = 0x52, kSliderForest = 0x60, kSliderCities = 0x6e;
constexpr int kCorners = 0x7c, kEdges = 0x8c, kDiagonals = 0xac;
constexpr int kNb = 0xbc;      // the 8 neighbours, S first and anticlockwise
constexpr int kNbRoad = 0xdc;  // the 8 again, N first and clockwise: road shapes
// RANDOM.DAT: the generator's working space and its tables
constexpr int kEdgeCount = 0xfc, kEdgePts = 0x104, kEdgeStart = 0x244, kEdgeEnd = 0x254;
constexpr int kShape = 0x264;      // neighbour mask -> tile variant, -1 for none
constexpr int kRoadShape = 0x364;  // neighbour mask -> road id - 1, -1 for none
constexpr int kTiles = 0x464;      // per terrain type, 16 variants x 2 checkerboard tiles
constexpr int kSideNames = 0x764, kSiteFormats = 0xed2;
constexpr int kSignCity = 0x1888, kSignLeagues = 0x189c, kSigns = 0x18b0;
constexpr int kSideClass = 0x1d62, kRegionClass = 0x1d72, kProduction = 0x1d82;
constexpr int kForestDone = 0x1f52, kForestWanted = 0x1f54;
constexpr int kNodes = 0x5dd6, kNodeCount = 0x611e, kGrid = 0x6120;
constexpr int kDatSize = 0xa560;

// .SCN offsets (docs/formats/scenario.md)
constexpr int kSSideRec = 387, kSRandom = 0x120, kSTerrainSet = 0x161;
constexpr int kSTerrain = 0x710, kSSiteCount = 0x80f, kSSites = 0x811, kSiteStride = 31;
constexpr int kSCityCount = 5499, kSCities = 5501, kCityStride = 65;
constexpr int kNSites = 40;

// directions the signposts give
const char* const kCompass[8] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};
// the twelve tiles round a city's 2x2 footprint, where its roads start
const int kRoundCity[12][2] = {{-1, -1}, {0, -1}, {1, -1}, {2, -1}, {2, 0},  {2, 1},
                               {2, 2},   {1, 2},  {0, 2},  {-1, 2}, {-1, 1}, {-1, 0}};
// the road builder's terrain costs, pseudo-player 14
const int kRoadCost[12] = {1, 1, 3, 3, 4, 5, 7, 2, 5, 2, 1, 7};
const int kFootprint[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
constexpr int kSafety = 200000;  // a loop the original might never leave

/** @brief A tile position or step vector on the map. */
struct Point {
    int x = 0;  ///< Column.
    int y = 0;  ///< Row.
};

/** @brief Chebyshev distance between two tiles. */
int Reach(int from_x, int from_y, int to_x, int to_y) {
    return std::max(std::abs(from_x - to_x), std::abs(from_y - to_y));
}

/** @brief Euclidean distance between two tiles, rounded down. */
int Distance(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x, dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

/**
 * @brief The step from one tile towards another, one of 8 compass directions
 *        by the slope, cut at tan 22.5 and tan 67.5.
 */
Point Step(int from_x, int from_y, int to_x, int to_y) {
    if (to_x == from_x) return Point{0, to_y == from_y ? 0 : (to_y < from_y ? -1 : 1)};
    if (to_y == from_y) return Point{to_x < from_x ? -1 : 1, 0};
    double slope = static_cast<double>(to_y - from_y) / static_cast<double>(to_x - from_x);
    if (to_x < from_x) {
        if (slope >= 2.414) return Point{0, -1};
        if (slope >= 0.414) return Point{-1, -1};
        if (slope >= -0.414) return Point{-1, 0};
        if (slope >= -2.414) return Point{-1, 1};
        return Point{0, 1};
    }
    if (slope >= 2.414) return Point{0, 1};
    if (slope >= 0.414) return Point{1, 1};
    if (slope >= -0.414) return Point{1, 0};
    if (slope >= -2.414) return Point{1, -1};
    return Point{0, -1};
}

/** @brief The compass direction (0..7) from one tile to another for a signpost. */
int Compass(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x) return from_y < to_y ? 4 : 0;
    if (from_y == to_y) return from_x < to_x ? 2 : 6;
    if (to_x < from_x && to_y < from_y) return 7;
    if (to_x < from_x && from_y < to_y) return 5;
    if (from_x < to_x && to_y < from_y) return 1;
    return 3;
}

/** @brief The checkerboard parity: 1 where x + y is even. */
int Parity(int x, int y) {
    int sum = x + y;
    return sum == 0 || sum / 2 != (sum - 1) / 2 ? 1 : 0;
}

/** @brief Clamps a point to the map bounds. */
void Clamp(Point& point) {
    if (point.x < 1) point.x = 0;
    if (point.y < 1) point.y = 0;
    if (point.x > kMapW - 1) point.x = kMapW - 1;
    if (point.y > kMapH - 1) point.y = kMapH - 1;
}

/** @brief Converts an unsigned byte (0..255) to signed (-128..127). */
int I8(int byte_val) {
    return byte_val > 127 ? byte_val - 256 : byte_val;
}

/** @brief Reads a signed 16-bit little-endian integer from `table` at `offset`. */
int GetI16(const std::vector<uint8_t>& table, int offset) {
    return static_cast<int16_t>(table[offset] | (table[offset + 1] << 8));
}

/** @brief Writes a signed 16-bit little-endian integer into `table` at `offset`. */
void SetI16(std::vector<uint8_t>& table, int offset, int value) {
    table[offset] = static_cast<uint8_t>(value & 0xff);
    table[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

/** @brief Reads a null-terminated string of up to `max_len` bytes from `table` at `offset`. */
std::string CstrOf(const std::vector<uint8_t>& table, int offset, int max_len) {
    std::string out;
    for (int i = offset; i < offset + max_len && i < static_cast<int>(table.size()); i++) {
        if (table[i] == 0) break;
        out += static_cast<char>(table[i]);
    }
    return out;
}

/** @brief Candidate spots for bridges or crossings. */
struct SpotList {
    int x[50];            ///< Column of each candidate spot.
    int y[50];            ///< Row of each candidate spot.
    int east_west[50];    ///< 1 if bridged east-west, 0 if north-south.
    int city_dist[50];    ///< Distance to the nearest city.
    int chosen[50];       ///< 1 if selected, 0 otherwise.
    int chosen_dist[50];  ///< Distance to the nearest already-chosen spot.
};

/** @brief Terrain types adjacent to a city's 2x2 footprint. */
struct Surroundings {
    bool shore;   ///< Touches shore.
    bool forest;  ///< Touches forest.
    bool road;    ///< Touches road.
    bool hills;   ///< Touches hills.
    bool marsh;   ///< Touches marsh.
};

}  // namespace

/** @brief Stateful random map and scenario generator. */
class Generator {
  public:
    std::vector<uint8_t> memory;  ///< RANDOM.DAT buffer and working terrain grid.
    std::vector<uint8_t> scn;     ///< Scenario (.SCN) binary image.
    Types types;                  ///< Loaded army type definitions.
    Rng* rng;                     ///< Dice roller.
    bool allies;                  ///< Whether non-capital cities may produce magical allies.
    bool fair = false;            ///< Options::fair_capitals.
    bool modern = false;          ///< Options::scenario (modern scenario generator).
    scengen::Settings world;      ///< Options::world settings.
    scengen::World plan;          ///< World layout built by the modern scenario generator.
    std::vector<uint16_t> tiles;  ///< Map tile words (.MAP).
    std::vector<uint8_t> roads;   ///< Road overlay bytes (.RD).
    std::string cty;              ///< City description text (.CTY).
    std::string spc;              ///< Site description text (.SPC).
    std::string sgn;              ///< Signpost binary data (.SGN).
    int site_cell = -1;           ///< Current 4x4 grid cell for site placement.

    int stage = 0;                               ///< Current progress-bar stage.
    int site_i = 0;                              ///< Next site index to place.
    int tenth = 0;                               ///< Last reported tenth of site placement.
    std::vector<uint8_t> served;                 ///< Per-city flag: connected by road.
    int shown = 4;                               ///< Road progress steps reported so far.
    std::optional<std::pair<int, int>> pending;  ///< Pending city pair for road laying.

    // --- memory -------------------------------------------------------------

    /** @brief Reads a signed 16-bit integer from `memory` at `offset`. */
    int R16(int offset) const { return GetI16(memory, offset); }
    /** @brief Writes a signed 16-bit integer to `memory` at `offset`. */
    void W16(int offset, int value) { SetI16(memory, offset, value); }
    /** @brief Reads a C string of up to `max_len` bytes from `memory` at `offset`. */
    std::string Str(int offset, int max_len) const { return CstrOf(memory, offset, max_len); }
    /** @brief Reads a signed 16-bit integer from `scn` at `offset`. */
    int S16(int offset) const { return GetI16(scn, offset); }
    /** @brief Writes a signed 16-bit integer to `scn` at `offset`. */
    void Ws16(int offset, int value) { SetI16(scn, offset, value); }
    /** @brief Copies a null-terminated string into `scn` at `offset`. */
    void Wstr(int offset, const std::string& text) {
        for (size_t i = 0; i < text.size(); i++) scn[offset + i] = static_cast<uint8_t>(text[i]);
        scn[offset + text.size()] = 0;
    }

    /**
     * @brief Reads the terrain grid at (x, y). Off the grid a read gets
     *        whatever RANDOM.DAT holds there, as the original did.
     */
    int At(int x, int y) const {
        int offset = kGrid + y * kMapW + x;
        return offset >= 0 && offset < kDatSize ? memory[offset] : 0;
    }
    /** @brief Writes `terrain` into the terrain grid at (x, y) if in bounds. */
    void Put(int x, int y, int terrain) {
        int idx = y * kMapW + x;
        if (idx >= 0 && idx < kMapW * kMapH) memory[kGrid + idx] = static_cast<uint8_t>(terrain);
    }
    /** @brief X step of neighbour `dir` (0..7). */
    int Nx(int dir) const { return R16(kNb + 4 * dir); }
    /** @brief Y step of neighbour `dir` (0..7). */
    int Ny(int dir) const { return R16(kNb + 4 * dir + 2); }
    /** @brief The index of step (dx, dy) among the 8 neighbours, or -1. */
    int NbIndex(int dx, int dy) const {
        int found = -1;
        for (int i = 0; i < 8; i++)
            if (Nx(i) == dx && Ny(i) == dy) found = i;
        return found;
    }

    /** @brief Low tile byte at (x, y), or 0 if out of bounds. */
    int Tile(int x, int y) const {
        return x >= 0 && y >= 0 && x < kMapW && y < kMapH ? tiles[y * kMapW + x] & 0xff : 0;
    }
    /** @brief Sets the low tile byte at (x, y), preserving bit 15 (crossing). */
    void SetTile(int x, int y, int tile_byte) {
        if (x < 0 || y < 0 || x >= kMapW || y >= kMapH) return;
        uint16_t& word = tiles[y * kMapW + x];
        word = static_cast<uint16_t>((word & 0xff00) | (tile_byte & 0xff));
    }
    /** @brief Road piece at (x, y), or 0 if none / out of bounds. */
    int Road(int x, int y) const {
        return x >= 0 && y >= 0 && x < kMapW && y < kMapH ? roads[y * kMapW + x] & 0x1f : 0;
    }
    /** @brief Sets the road piece at (x, y). */
    void SetRoad(int x, int y, int piece) {
        if (x < 0 || y < 0 || x >= kMapW || y >= kMapH) return;
        uint8_t& byte_val = roads[y * kMapW + x];
        byte_val = static_cast<uint8_t>((byte_val & 0xe0) | (piece & 0x1f));
    }
    /** @brief Terrain type id for tile byte `tile_byte`. */
    int TerrainOf(int tile_byte) const { return scn[kSTerrain + tile_byte]; }
    /**
     * @brief Variant `variant` of terrain type `terrain` from RANDOM.DAT's tile
     *        table, the checkerboard picking one of its pair.
     */
    int TileFor(int terrain, int variant, int x, int y) const {
        return R16(kTiles + 64 * terrain + 4 * variant + 2 * Parity(x, y)) & 0xff;
    }

    /** @brief Number of cities currently recorded in `scn`. */
    int CityCount() const { return S16(kSCityCount); }
    /** @brief Sets the number of cities in `scn`. */
    void SetCityCount(int count) { Ws16(kSCityCount, count); }
    /** @brief Byte offset of city `city_idx`'s record in `scn`. */
    int Crec(int city_idx) const { return kSCities + kCityStride * city_idx; }
    /** @brief X coordinate of city `city_idx`. */
    int Cx(int city_idx) const { return S16(Crec(city_idx)); }
    /** @brief Y coordinate of city `city_idx`. */
    int Cy(int city_idx) const { return S16(Crec(city_idx) + 2); }

    /**
     * @brief Rolls `count` dice of `sides` plus `bonus`: 0 sides gives `bonus`,
     *        and negative sides gives `count + bonus`.
     */
    int Dice(int count, int sides, int bonus = 0) {
        if (sides == 0) return bonus;
        if (sides < 0) return count + bonus;
        return rng->Dice(count, sides, bonus);
    }

    /** @brief True if terrain `terrain` is on any of the 8 tiles around (x, y). */
    bool Near(int x, int y, int terrain) const {
        for (int i = 0; i < 8; i++) {
            int adj_x = x + Nx(i), adj_y = y + Ny(i);
            if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                At(adj_x, adj_y) == terrain)
                return true;
        }
        return false;
    }
    /** @brief True if any of the 8 tiles around (x, y) is plain. */
    bool NearPlain(int x, int y) const { return Near(x, y, kPlain); }

    /**
     * @brief Pushes a point by (1d3-2) x 1d(range) each way; with `to_edge`,
     *        30% of the time it then snaps to the nearest map edge.
     */
    void Jitter(Point& point, int range, bool to_edge) {
        int step_x = Dice(1, 3, -2);
        point.x += step_x * Dice(1, range, 0);
        int step_y = Dice(1, 3, -2);
        point.y += step_y * Dice(1, range, 0);
        if (Dice(1, 100, 0) < 30 && to_edge) {
            int left = point.x, top = point.y;
            int right = kMapW - 1 - point.x, bottom = kMapH - 1 - point.y;
            if (left <= top && left <= right && left <= bottom) point.x = 0;
            if (top <= left && top <= right && top <= bottom) point.y = 0;
            if (right <= top && right <= left && right <= bottom) point.x = kMapW - 1;
            if (bottom <= top && bottom <= left && bottom <= right) point.y = kMapH - 1;
        }
        Clamp(point);
    }

    // --- the coastline --------------------------------------------------------

    /** @brief Reads a 2-word point from `memory` at `offset`. */
    Point Pt(int offset) const { return Point{R16(offset), R16(offset + 2)}; }
    /** @brief Writes a 2-word point to `memory` at `offset`. */
    void SetPt(int offset, Point point) {
        W16(offset, point.x);
        W16(offset + 2, point.y);
    }

    /**
     * @brief Determines where edge `edge_idx`'s stretch of coast begins and
     *        ends: the map's corners drawn in 10 along the diagonal, then jittered.
     */
    void CoastEnds(int edge_idx) {
        int next_edge = (edge_idx + 1) % 4;
        if (R16(kPEdgePoints + 2 * edge_idx) < 3) {
            SetPt(kEdgeStart + 4 * edge_idx, Pt(kEdges + 8 * edge_idx));
            SetPt(kEdgeEnd + 4 * edge_idx, Pt(kEdges + 8 * edge_idx + 4));
            return;
        }
        Point start_pt = Pt(kEdges + 8 * edge_idx);
        start_pt.x -= 10 * R16(kDiagonals + 4 * edge_idx);
        start_pt.y -= 10 * R16(kDiagonals + 4 * edge_idx + 2);
        Jitter(start_pt, 16, true);
        Put(start_pt.x, start_pt.y, kPlain);
        SetPt(kEdgeStart + 4 * edge_idx, start_pt);
        Point end_pt = Pt(kEdges + 8 * edge_idx + 4);
        end_pt.x -= 10 * R16(kDiagonals + 4 * next_edge);
        end_pt.y -= 10 * R16(kDiagonals + 4 * next_edge + 2);
        Jitter(end_pt, 16, true);
        Put(end_pt.x, end_pt.y, kPlain);
        SetPt(kEdgeEnd + 4 * edge_idx, end_pt);
    }

    /**
     * @brief Swaps an edge's end with the next one's start where that keeps
     *        the outline turning around the middle.
     */
    void CoastOrder() {
        for (int i = 0; i < 4; i++) {
            int next_edge = (i + 1) % 4;
            Point end_pt = Pt(kEdgeEnd + 4 * i), start_pt = Pt(kEdgeStart + 4 * next_edge);
            if (static_cast<double>(56 - end_pt.x) / static_cast<double>(78 - end_pt.y) <
                static_cast<double>(56 - start_pt.x) / static_cast<double>(78 - start_pt.y)) {
                SetPt(kEdgeEnd + 4 * i, start_pt);
                SetPt(kEdgeStart + 4 * next_edge, end_pt);
            }
        }
    }

    /** @brief True if `point` lies on the outer border of the map. */
    static bool OnEdge(Point point) {
        return point.x == 0 || point.y == 0 || point.x == kMapW - 1 || point.y == kMapH - 1;
    }

    /**
     * @brief Places intermediate points along edge `edge_idx`, each a step on
     *        from the last towards the end and jittered by up to half a step.
     */
    void CoastPoints(int edge_idx) {
        int base = kEdgePts + 0x50 * edge_idx, want = R16(kPEdgePoints + 2 * edge_idx);
        Point start_pt = Pt(kEdgeStart + 4 * edge_idx), end_pt = Pt(kEdgeEnd + 4 * edge_idx);
        SetPt(base, start_pt);
        int count = 1;
        if (want < 3 || (OnEdge(start_pt) && OnEdge(end_pt))) {
            SetPt(base + 4, end_pt);
            W16(kEdgeCount + 2 * edge_idx, 2);
            return;
        }
        int step_len = Reach(end_pt.x, end_pt.y, start_pt.x, start_pt.y) / (want - 1);
        int idx = 0;
        for (;;) {
            Point cur = Pt(base + 4 * idx);
            Point dir = Step(cur.x, cur.y, end_pt.x, end_pt.y);
            Point next_pt{cur.x + dir.x * step_len, cur.y + dir.y * step_len};
            Jitter(next_pt, step_len / 2, false);
            idx++;
            Put(next_pt.x, next_pt.y, kPlain);
            SetPt(base + 4 * idx, next_pt);
            count++;
            if (Reach(end_pt.x, end_pt.y, next_pt.x, next_pt.y) < step_len || count >= want - 1)
                break;
        }
        SetPt(base + 4 * (idx + 1), end_pt);
        W16(kEdgeCount + 2 * edge_idx, count + 1);
    }

    /** @brief Turns a step vector by -1 to +3 eighths of a circle. */
    Point Turn(Point dir) {
        int idx = NbIndex(dir.x, dir.y);
        int next_dir = Dice(1, 5, (idx < 0 ? 8 : idx) - 2) % 8;
        return Point{Nx(next_dir), Ny(next_dir)};
    }

    /** @brief Draws a wandering line of plain from `from` to `to_pt`. */
    void CoastLine(Point from, Point to_pt) {
        Point cur = from;
        Point first = Step(from.x, from.y, to_pt.x, to_pt.y);
        for (int step = 0; step < kSafety; step++) {
            Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
            int roll = Reach(cur.x, cur.y, to_pt.x, to_pt.y) < 3 ? 1 : Dice(1, 3, 0);
            if (roll == 2)
                dir = first;
            else if (roll == 3) {
                dir = Turn(dir);
                first = dir;
            }
            cur.x += dir.x;
            cur.y += dir.y;
            Clamp(cur);
            Put(cur.x, cur.y, kPlain);
            if (cur.x == to_pt.x && cur.y == to_pt.y) return;
        }
    }

    /** @brief Which edge (0..3) a point lies on, or -1 for none. */
    static int EdgeOf(Point point) {
        if (point.x == 0) return 3;
        if (point.y == 0) return 0;
        if (point.x == kMapW - 1) return 1;
        if (point.y == kMapH - 1) return 2;
        return -1;
    }

    /** @brief Draws a straight run of plain from `from` to `to_pt`. */
    void Straight(Point from, Point to_pt) {
        Point dir = Step(from.x, from.y, to_pt.x, to_pt.y);
        Point cur = from;
        for (int step = 0; step < kSafety && !(cur.x == to_pt.x && cur.y == to_pt.y); step++) {
            cur.x += dir.x;
            cur.y += dir.y;
            Clamp(cur);
            Put(cur.x, cur.y, kPlain);
        }
    }

    /**
     * @brief Joins two points on the map's edge along the border, around a
     *        corner if needed; across the map they wander like any coastline.
     */
    void CoastAlongEdge(Point from, Point to_pt) {
        int edge_a = EdgeOf(from), edge_b = EdgeOf(to_pt);
        if (edge_a == edge_b) return Straight(from, to_pt);
        int apart = std::abs(edge_a - edge_b);
        if (apart == 2) return CoastLine(from, to_pt);
        Point corner = Pt(kCorners + 4 * (apart == 3 ? 0 : std::max(edge_a, edge_b)));
        Straight(from, corner);
        Straight(to_pt, corner);
    }

    /** @brief Connects two coastline control points `from` and `to_pt`. */
    void CoastSegment(Point from, Point to_pt) {
        if (OnEdge(from) && OnEdge(to_pt))
            CoastAlongEdge(from, to_pt);
        else
            CoastLine(from, to_pt);
    }

    /** @brief Joins every point of the coastline outline to the next. */
    void CoastJoin() {
        for (int i = 0; i < 4; i++) {
            int base = kEdgePts + 0x50 * i, count = R16(kEdgeCount + 2 * i);
            int idx = 0;
            for (; idx < count - 1; idx++)
                CoastSegment(Pt(base + 4 * idx), Pt(base + 4 * (idx + 1)));
            int next_edge = (i + 1) % 4;
            CoastSegment(Pt(base + 4 * idx), Pt(kEdgePts + 0x50 * next_edge));
        }
    }

    /** @brief True if sea water may flood into (x, y). */
    bool Floods(int x, int y) const {
        int terrain = At(x, y);
        if (terrain == kShore || terrain == kBridge) return false;
        bool inside = x != 0 && y != 0 && x != kMapW - 1 && y != kMapH - 1;
        if (!inside && terrain == 0) return true;
        return At(x + 1, y) == kWater || At(x - 1, y) == kWater || At(x, y + 1) == kWater ||
               At(x, y - 1) == kWater;
    }

    /**
     * @brief Floods sea water in from the edges up to the coastline outline;
     *        everything unreached becomes plain.
     */
    void CoastFill() {
        for (int i = 0; i < kMapW * kMapH; i++)
            memory[kGrid + i] = memory[kGrid + i] == kPlain ? kShore : 0;
        auto flood = [&](int x, int y) {
            if (Floods(x, y)) Put(x, y, kWater);
        };
        for (int pass = 0; pass < 2; pass++) {
            for (int x = 0; x < kMapW; x++)
                for (int y = 0; y < kMapH; y++) flood(x, y);
            for (int y = 0; y < kMapH; y++)
                for (int x = 0; x < kMapW; x++) flood(x, y);
            for (int x = kMapW - 1; x >= 0; x--)
                for (int y = kMapH - 1; y >= 0; y--) flood(x, y);
            for (int y = kMapH - 1; y >= 0; y--)
                for (int x = kMapW - 1; x >= 0; x--) flood(x, y);
        }
        for (int i = 0; i < kMapW * kMapH; i++)
            memory[kGrid + i] = memory[kGrid + i] == kWater ? kWater : kPlain;
    }

    /** @brief Marks water tiles adjacent to land as shore. */
    void Shores() {
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                if (At(x, y) != kWater) continue;
                if (NearPlain(x, y)) Put(x, y, kShore);
                if (Near(x, y, kHills)) Put(x, y, kShore);
                if (Near(x, y, kMountains)) Put(x, y, kShore);
            }
        }
    }

    /** @brief Generates the landmass, coastline, and optional sea channels. */
    void Coast() {
        for (int i = 0; i < 4; i++) CoastEnds(i);
        CoastOrder();
        for (int i = 0; i < 4; i++) CoastPoints(i);
        CoastJoin();
        CoastFill();
        Shores();
        if (Dice(1, 100, 0) < 50) {
            // 0-2 channels straight across, west to east
            int channels = Dice(1, 3, -1);
            for (int i = 0; i < channels; i++) River(true, false, true);
            WaterTidy();
        }
        Shores();
    }

    // --- mountains and hills ------------------------------------------------

    /** @brief Byte offset of highland node `node_idx` in `memory`. */
    int Node(int node_idx) const { return kNodes + 14 * node_idx; }

    /** @brief Places `count` seed nodes of `terrain` (`kind` 1=hill, 2=mountain) on random plain
     * tiles. */
    void Seeds(int count, int terrain, int kind) {
        for (int idx = 0; idx < count; idx++) {
            int x = 0, y = 0;
            for (int tries = 0; tries < kSafety; tries++) {
                x = Dice(1, kMapW, -1);
                y = Dice(1, kMapH, -1);
                if (At(x, y) == kPlain) break;
            }
            Put(x, y, terrain);
            int cur_count = R16(kNodeCount), offset = Node(cur_count);
            W16(offset, x);
            W16(offset + 2, y);
            W16(offset + 4, kind);
            W16(offset + 6, 0);
            W16(kNodeCount, cur_count + 1);
        }
    }

    /**
     * @brief Links each highland node to its nearest neighbours: a hill to
     *        0..1, a mountain to 0..3.
     */
    void Link() {
        int count = R16(kNodeCount);
        if (count < 2) return;
        for (int i = 0; i < count; i++) {
            int offset = Node(i);
            int links = R16(offset + 4) == 1 ? Dice(1, 2, -1) : Dice(1, 4, -1);
            if (links >= count - 1) links = count - 1;
            W16(offset + 6, links);
            for (int link_idx = 0; link_idx < links; link_idx++) {
                int best = 10000, best_j = -1;
                for (int j = 0; j < count; j++) {
                    if (j == i) continue;
                    int prev = 0;
                    while (prev < link_idx && R16(offset + 8 + 2 * prev) != j) prev++;
                    if (prev != link_idx) continue;
                    int other = Node(j);
                    int dist = Reach(R16(other), R16(other + 2), R16(offset), R16(offset + 2));
                    if (dist < best) {
                        best = dist;
                        best_j = j;
                    }
                }
                W16(offset + 8 + 2 * link_idx, best_j);
            }
        }
    }

    /**
     * @brief Draws a ridge from `from` to `to_pt`: `width` 2 adds a tile to
     *        either side, 3 adds one two tiles out. Only plain and hills give way.
     */
    void Ridge(Point from, Point to_pt, int width, int terrain) {
        Point cur = from;
        Point first = Step(from.x, from.y, to_pt.x, to_pt.y);
        int dir_idx = 0;
        auto lay = [&](int x, int y) {
            int cur_terr = At(x, y);
            if (cur_terr == kPlain || cur_terr == kHills) Put(x, y, terrain);
        };
        for (int step = 0; step < kSafety; step++) {
            Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
            int roll = Reach(cur.x, cur.y, to_pt.x, to_pt.y) < 3 ? 1 : Dice(1, 3, 0);
            if (roll == 2)
                dir = first;
            else if (roll == 3) {
                dir = Turn(dir);
                first = dir;
            }
            cur.x += dir.x;
            cur.y += dir.y;
            int found_dir = NbIndex(dir.x, dir.y);
            if (found_dir >= 0) dir_idx = found_dir;
            int left = (dir_idx + 2) % 8, right = (dir_idx + 6) % 8;
            Point side1{cur.x + Nx(left), cur.y + Ny(left)};
            Point side2{cur.x + Nx(right), cur.y + Ny(right)};
            Point outer1{cur.x + 2 * Nx(left), cur.y + 2 * Ny(left)};
            Point outer2{cur.x + 2 * Nx(right), cur.y + 2 * Ny(right)};
            Clamp(cur);
            lay(cur.x, cur.y);
            bool done = cur.x == to_pt.x && cur.y == to_pt.y;
            if (width == 2) {
                Clamp(side1);
                lay(side1.x, side1.y);
                lay(side2.x, side2.y);  // the original clamps only the one
            }
            if (width == 3) {
                Clamp(outer1);
                lay(outer1.x, outer1.y);
                lay(outer2.x, outer2.y);
            }
            if (done) return;
        }
    }

    /** @brief Gives a node with no links a short ridge of its own, up and to the left. */
    void Blob(Point start_pt, int terrain) {
        Point end_pt;
        end_pt.x = start_pt.x + Dice(1, 5, -10);
        end_pt.y = start_pt.y + Dice(1, 5, -10);
        Clamp(end_pt);
        int width = Dice(1, 2, 1);
        Ridge(start_pt, end_pt, width, terrain);
    }

    /** @brief Draws ridges along all highland node links. */
    void Ridges() {
        int count = R16(kNodeCount);
        if (count < 2) return;
        for (int i = 0; i < count; i++) {
            int offset = Node(i);
            Point from{R16(offset), R16(offset + 2)};
            bool hill = R16(offset + 4) == 1;
            int links = R16(offset + 6);
            if (links == 0) {
                Blob(from, hill ? kHills : kMountains);
                continue;
            }
            for (int link_idx = 0; link_idx < links; link_idx++) {
                int other = Node(R16(offset + 8 + 2 * link_idx));
                Point to_pt{R16(other), R16(other + 2)};
                if (hill && R16(other + 4) == 1)
                    Ridge(from, to_pt, 3, kHills);
                else {
                    int width = Dice(1, 2, 1);
                    Ridge(from, to_pt, width, kMountains);
                }
            }
        }
    }

    /**
     * @brief Tidies mountains: plain hemmed in on all four sides joins them,
     *        mountains on the shore come down, and diagonal runs are thickened.
     */
    void MountainTidy() {
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                if (x != 0 && y != 0 && x != kMapW - 1 && y != kMapH - 1 && At(x, y) == kPlain) {
                    for (int terrain : {kMountains, kHills}) {
                        if (At(x, y + 1) == terrain && At(x + 1, y) == terrain &&
                            At(x - 1, y) == terrain && At(x, y - 1) == terrain)
                            Put(x, y, terrain);
                    }
                }
                if (At(x, y) == kMountains && Near(x, y, kShore)) Put(x, y, kPlain);
            }
        }
        for (int x = 1; x < kMapW - 1; x++) {
            for (int y = 1; y < kMapH - 1; y++) {
                if (At(x, y) != kMountains) continue;
                auto is_mtn = [&](int dx, int dy) { return At(x + dx, y + dy) == kMountains; };
                if (is_mtn(1, 1) && is_mtn(-1, -1) && !is_mtn(1, -1) && !is_mtn(-1, 1)) {
                    if (Dice(1, 10, 0) > 5)
                        Put(x - 1, y + 1, kMountains);
                    else
                        Put(x + 1, y - 1, kMountains);
                } else if (is_mtn(-1, 1) && is_mtn(1, -1) && !is_mtn(-1, -1) && !is_mtn(1, 1)) {
                    if (Dice(1, 10, 0) > 5)
                        Put(x + 1, y + 1, kMountains);
                    else
                        Put(x - 1, y - 1, kMountains);
                }
            }
        }
    }

    /** @brief Rings mountains with foothills. */
    void Foothills() {
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                int terrain = At(x, y);
                if (terrain != kMountains && terrain != kHills && Near(x, y, kMountains))
                    Put(x, y, kHills);
            }
        }
    }

    /** @brief Generates mountains, hills, ridges, and foothills. */
    void Highlands() {
        W16(kNodeCount, 0);
        Seeds(R16(kPMountains), kMountains, 2);
        Seeds(R16(kPHills), kHills, 1);
        Link();
        Ridges();
        MountainTidy();
        Foothills();
    }

    // --- erosion and passes --------------------------------------------------

    /**
     * @brief Walks between two random tiles: erosion wears mountains to hills,
     *        while a pass cuts mountains and hills to plain, five tiles across.
     */
    void Wear(bool pass) {
        Point from, to_pt;
        from.x = Dice(1, kMapW, -1);
        from.y = Dice(1, kMapH, -1);
        to_pt.x = Dice(1, kMapW, -1);
        to_pt.y = Dice(1, kMapH, -1);
        Point cur = from;
        while (!(cur.x == to_pt.x && cur.y == to_pt.y)) {
            Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
            int dir_idx = NbIndex(dir.x, dir.y);
            int left = (dir_idx + 1) % 8, right = (dir_idx + 7) % 8;
            int terrain = At(cur.x, cur.y);
            bool hit = pass ? terrain == kMountains || terrain == kHills : terrain == kMountains;
            if (hit) {
                int target_terr = pass ? kPlain : kHills;
                const int offs[5][2] = {{0, 0},
                                        {Nx(left), Ny(left)},
                                        {Nx(right), Ny(right)},
                                        {2 * Nx(left), 2 * Ny(left)},
                                        {2 * Nx(right), 2 * Ny(right)}};
                for (auto& off : offs) {
                    Point pos{cur.x + off[0], cur.y + off[1]};
                    Clamp(pos);
                    Put(pos.x, pos.y, target_terr);
                }
            }
            cur.x += dir.x;
            cur.y += dir.y;
        }
    }

    /**
     * @brief Converts tiles of `terrain` with at least three orthogonal
     *        neighbours of `surrounding` into `surrounding`.
     */
    void Surrounded(int terrain, int surrounding) {
        for (int x = 1; x < kMapW - 1; x++) {
            for (int y = 1; y < kMapH - 1; y++) {
                if (At(x, y) != terrain) continue;
                int count = (At(x, y + 1) == surrounding) + (At(x, y - 1) == surrounding) +
                            (At(x + 1, y) == surrounding) + (At(x - 1, y) == surrounding);
                if (count > 2) Put(x, y, surrounding);
            }
        }
    }

    /** @brief Applies erosion and mountain passes. */
    void Erosion() {
        for (int i = 0; i < R16(kPErosion); i++) Wear(false);
        for (int i = 0; i < R16(kPPasses); i++) Wear(true);
        Surrounded(kHills, kMountains);
        Surrounded(kPlain, kHills);
        Foothills();
    }

    // --- rivers --------------------------------------------------------------

    /**
     * @brief Carves a river or east-west sea channel across the map.
     */
    void River(bool wide, bool stops, bool channel) {
        Point from, to_pt;
        if (channel) {
            from.x = 0;
            from.y = Dice(1, kMapH, -1);
            to_pt.x = kMapW - 1;
            to_pt.y = Dice(1, kMapH, -1);
        } else {
            for (int i = 0; i < 200; i++) {
                from.x = Dice(1, kMapW, -1);
                from.y = Dice(1, kMapH, -1);
                int terrain = At(from.x, from.y);
                if (terrain == kHills || terrain == kMountains) break;
            }
            for (int i = 0; i < 200; i++) {
                to_pt.x = Dice(1, kMapW, -1);
                to_pt.y = Dice(1, kMapH, -1);
                int terrain = At(to_pt.x, to_pt.y);
                if (terrain == kWater || terrain == kShore) break;
            }
        }
        Point cur = from;
        int met = 0;
        bool done = cur.x == to_pt.x && cur.y == to_pt.y;
        for (int step = 0; !done && step < kSafety; step++) {
            Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
            int dir_idx = NbIndex(dir.x, dir.y);
            int left = (dir_idx + 1) % 8, right = (dir_idx + 7) % 8;
            int aside1 = (dir_idx + 2) % 8, aside2 = (dir_idx + 6) % 8;
            if (stops) {
                // nb(-1) reads the word before the table, as the original's did
                int ahead = At(cur.x + Nx(dir_idx), cur.y + Ny(dir_idx));
                if (ahead == kWater && met == 1) done = true;
                if (ahead == kWater && met == 0) met++;
            }
            auto lay = [&](int off_x, int off_y) {
                Point pos{cur.x + off_x, cur.y + off_y};
                Clamp(pos);
                Put(pos.x, pos.y, kWater);
            };
            lay(0, 0);
            lay(Nx(left), Ny(left));
            lay(Nx(right), Ny(right));
            if (wide) {
                lay(2 * Nx(left), 2 * Ny(left));
                lay(2 * Nx(right), 2 * Ny(right));
                if (channel) {
                    lay(3 * Nx(left), 3 * Ny(left));
                    lay(3 * Nx(right), 3 * Ny(right));
                }
            }
            cur.x += dir.x;
            cur.y += dir.y;
            if (cur.x == to_pt.x && cur.y == to_pt.y) done = true;
            int roll = Dice(1, 4, 0);
            if (roll == 2) {
                cur.x += Nx(aside1);
                cur.y += Ny(aside1);
            } else if (roll == 3) {
                cur.x += Nx(aside2);
                cur.y += Ny(aside2);
            }
        }
    }

    /** @brief True if `terrain` is shore or open water. */
    static bool Wet(int terrain) { return terrain == kShore || terrain == kWater; }

    /**
     * @brief Tidies water and shores: land surrounded on three sides by water
     *        submerges, shore far from land becomes open water, and diagonal
     *        water runs receive an adjacent shore tile.
     */
    void WaterTidy() {
        auto land = [](int terrain) {
            return terrain == kPlain || terrain == kHills || terrain == kMountains;
        };
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                if (!land(At(x, y))) continue;
                int wet_adj = Wet(At(x, y + 1)) + Wet(At(x, y - 1)) + Wet(At(x + 1, y)) +
                              Wet(At(x - 1, y));
                if (wet_adj > 2) Put(x, y, kWater);
            }
        }
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                if (At(x, y) == kShore && !Near(x, y, kPlain) && !Near(x, y, kHills) &&
                    !Near(x, y, kMountains))
                    Put(x, y, kWater);
            }
        }
        auto dry = [](int terrain) {
            return terrain == kHills || terrain == kForest || terrain == kPlain;
        };
        for (int x = 1; x < kMapW - 1; x++) {
            for (int y = 1; y < kMapH - 1; y++) {
                if (!Wet(At(x, y))) continue;
                auto adj = [&](int dx, int dy) { return At(x + dx, y + dy); };
                if (Wet(adj(1, 1)) && Wet(adj(-1, -1)) && dry(adj(1, -1)) && dry(adj(-1, 1))) {
                    if (Dice(1, 10, 0) > 4)
                        Put(x - 1, y + 1, kShore);
                    else
                        Put(x + 1, y - 1, kShore);
                } else if (Wet(adj(-1, 1)) && Wet(adj(1, -1)) && dry(adj(-1, -1)) &&
                           dry(adj(1, 1))) {
                    if (Dice(1, 10, 0) > 4)
                        Put(x + 1, y + 1, kShore);
                    else
                        Put(x - 1, y - 1, kShore);
                }
            }
        }
    }

    /** @brief Generates narrow and wide rivers and tidies water and shores. */
    void Rivers() {
        for (int i = 0; i < R16(kPRivers); i++) River(false, true, false);
        for (int i = 0; i < R16(kPWideRivers); i++) River(true, true, false);
        WaterTidy();
        Shores();
        MountainTidy();
        Foothills();
        WaterTidy();
        Shores();
    }

    // --- forests -------------------------------------------------------------

    /** @brief Increments the count of forest tiles placed. */
    void AddForest() { W16(kForestDone, R16(kForestDone) + 1); }

    /**
     * @brief Grows a wood around a random plain tile: eight arms with two side
     *        shoots off each step that shorten along the arm.
     */
    void Wood() {
        int x = 0, y = 0;
        for (int tries = 0; tries < kSafety; tries++) {
            x = Dice(1, kMapW, -1);
            y = Dice(1, kMapH, -1);
            if (At(x, y) == kPlain) break;
        }
        int arms[8];
        int span, base;
        if (Dice(1, 100, 0) < 65) {
            for (int i = 0; i < 8; i++) arms[i] = Dice(1, 8, 2);
            span = 4;
            base = 2;
        } else {
            for (int i = 0; i < 8; i++) arms[i] = Dice(1, 10, 5);
            span = 6;
            base = 4;
        }
        auto grow = [&](Point& pos) {
            Clamp(pos);
            if (At(pos.x, pos.y) == kPlain) {
                Put(pos.x, pos.y, kForest);
                AddForest();
            }
        };
        Put(x, y, kForest);
        AddForest();
        for (int i = 0; i < 8; i++) {
            Point cur{x, y};
            int dist = 0;
            for (int step = 0; step < arms[i]; step++) {
                cur.x += Nx(i);
                cur.y += Ny(i);
                grow(cur);
                int left = (i + 2) % 8, right = (i + 6) % 8;
                dist++;
                int num_left = Dice(1, span - dist / 2, base - dist / 4);
                int num_right = Dice(1, span - dist / 2, base - dist / 3);
                Point left_pt = cur;
                for (int j = 0; j < num_left; j++) {
                    left_pt.x += Nx(left);
                    left_pt.y += Ny(left);
                    grow(left_pt);
                }
                Point right_pt = cur;
                for (int j = 0; j < num_right; j++) {
                    right_pt.x += Nx(right);
                    right_pt.y += Ny(right);
                    grow(right_pt);
                }
            }
        }
    }

    /**
     * @brief Tidies forests: plain with forest on three sides becomes forest,
     *        and diagonal runs of forest get an adjacent forest tile.
     */
    void ForestTidy() {
        for (int x = 0; x < kMapW; x++) {
            for (int y = 0; y < kMapH; y++) {
                if (At(x, y) != kPlain) continue;
                int adj_forest = (At(x, y + 1) == kForest) + (At(x, y - 1) == kForest) +
                                 (At(x + 1, y) == kForest) + (At(x - 1, y) == kForest);
                if (adj_forest > 2) {
                    Put(x, y, kForest);
                    AddForest();
                }
            }
        }
        auto open = [](int terrain) {
            return terrain == kHills || terrain == kShore || terrain == kPlain;
        };
        for (int x = 1; x < kMapW - 1; x++) {
            for (int y = 1; y < kMapH - 1; y++) {
                if (At(x, y) != kForest) continue;
                auto adj = [&](int dx, int dy) { return At(x + dx, y + dy); };
                if (adj(1, 1) == kForest && adj(-1, -1) == kForest && open(adj(1, -1)) &&
                    open(adj(-1, 1))) {
                    if (Dice(1, 10, 0) < 5)
                        Put(x + 1, y - 1, kForest);
                    else
                        Put(x - 1, y + 1, kForest);
                } else if (adj(-1, 1) == kForest && adj(1, -1) == kForest && open(adj(-1, -1)) &&
                           open(adj(1, 1))) {
                    if (Dice(1, 10, 0) < 5)
                        Put(x - 1, y - 1, kForest);
                    else
                        Put(x + 1, y + 1, kForest);
                }
            }
        }
    }

    /** @brief Grows woods until they cover land / 100 x the Forest parameter. */
    void Forests() {
        int land = 0;
        for (int i = 0; i < kMapW * kMapH; i++) {
            int terrain = memory[kGrid + i];
            if (terrain == kPlain || terrain == kHills || terrain == kMountains) land++;
        }
        W16(kForestWanted, land / 100 * R16(kPForest));
        W16(kForestDone, 0);
        for (int step = 0; step < 10000 && R16(kForestDone) < R16(kForestWanted); step++) {
            Wood();
            ForestTidy();
        }
    }

    // --- marshes -------------------------------------------------------------

    /** @brief Scatters 1d5+3 tiles of marsh around (x, y). */
    void MarshAround(int x, int y) {
        int count = Dice(1, 5, 3);
        for (int i = 0; i < count; i++) {
            Point pos;
            int dist1 = Dice(1, 3, 0);
            int dir1 = Dice(1, 8, -1);
            pos.x = x + dist1 * Nx(dir1);
            int dist2 = Dice(1, 3, 0);
            int dir2 = Dice(1, 8, -1);
            pos.y = y + dist2 * Ny(dir2);
            Clamp(pos);
            if (At(pos.x, pos.y) == kPlain) Put(pos.x, pos.y, kMarsh);
        }
    }

    /** @brief Places a marsh cluster, near the shore if one of four tries finds it. */
    void Marsh() {
        int x = 0, y = 0, tries = 0;
        for (int step = 0; step < kSafety; step++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            if (At(x, y) != kPlain) continue;
            if (tries >= 4) break;
            tries++;
            if (Near(x, y, kShore) || Near(x + 1, y, kShore) || Near(x, y + 1, kShore) ||
                Near(x + 1, y + 1, kShore))
                break;
        }
        Put(x, y, kMarsh);
        MarshAround(x, y);
        MarshAround(x + 1, y + 1);
        MarshAround(x - 1, y - 1);
        MarshAround(x - 1, y + 1);
        MarshAround(x + 1, y - 1);
    }

    /** @brief Places 1d3 marsh clusters. */
    void Marshes() {
        int count = Dice(1, 3, 0);
        for (int i = 0; i < count; i++) Marsh();
    }

    // --- cities and sites ----------------------------------------------------

    /**
     * @brief Places a city's 2x2 footprint clear of water, hills, and other
     *        cities by a tile; on the coast if one of four tries finds it.
     */
    void PlaceCity() {
        int x = 0, y = 0, tries = 0;
        for (int step = 0; step < kSafety; step++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            bool valid = true;
            for (auto& cell : kFootprint) {
                int dx = cell[0], dy = cell[1];
                int terrain = At(x + dx, y + dy);
                if (terrain == kShore || terrain == kWater || terrain == kCity ||
                    terrain == kMountains || terrain == kHills)
                    valid = false;
                if (Near(x + dx, y + dy, kCity) || Near(x + dx + 1, y + dy + 1, kCity) ||
                    Near(x + dx - 1, y + dy - 1, kCity))
                    valid = false;
            }
            if (!valid) continue;
            if (tries >= 4) break;
            tries++;
            if (Near(x, y, kShore) || Near(x + 1, y, kShore) || Near(x, y + 1, kShore) ||
                Near(x + 1, y + 1, kShore))
                break;
        }
        int city_idx = CityCount();
        Ws16(Crec(city_idx), x);
        Ws16(Crec(city_idx) + 2, y);
        SetCityCount(city_idx + 1);
        for (auto& cell : kFootprint) Put(x + cell[0], y + cell[1], kCity);
    }

    /** @brief Places all cities on the terrain grid. */
    void Cities() {
        SetCityCount(0);
        for (int i = R16(0xa84); i < R16(kPCities); i++) PlaceCity();
    }

    /**
     * @brief Finds a spot for a site, cycling through the map's 16 cells in turn.
     */
    Point SiteSpot() {
        if (site_cell == -1) site_cell = Dice(1, 16, -1);
        int cell_x = site_cell % 4, cell_y = site_cell / 4;
        int x = 0, y = 0;
        bool found = false;
        for (int i = 0; i < 50 && !found; i++) {
            x = Dice(1, 20, cell_x * kMapW / 4 + 3);
            y = Dice(1, 31, cell_y * kMapH / 4 + 3);
            found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite) &&
                    !Near(x - 3, y, kSite) && !Near(x + 3, y, kSite) && !Near(x, y - 3, kSite) &&
                    !Near(x, y + 3, kSite);
        }
        for (int i = 0; i < 50 && !found; i++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite);
        }
        if (!found) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            for (int i = 0; i < 50 && !found; i++) {
                x = Dice(1, 102, 5);
                y = Dice(1, 146, 5);
                found = At(x, y) != kCity && At(x, y) != kSite;
            }
        }
        site_cell = (site_cell + 1) % 16;
        return Point{x, y};
    }

    /** @brief Initialises the 40 site records as unplaced. */
    void SitesBegin() {
        Ws16(kSSiteCount, kNSites);
        for (int i = 0; i < kNSites; i++) {
            Ws16(kSSites + kSiteStride * i + 2, -1);
            Ws16(kSSites + kSiteStride * i, -1);
        }
    }

    /** @brief Places site `site_idx` at a chosen spot and generates its name and text. */
    void Site(int site_idx) { SiteAt(site_idx, SiteSpot()); }

    /** @brief Writes site `site_idx` at `spot` with its generated name and description. */
    void SiteAt(int site_idx, Point spot) {
        int offset = kSSites + kSiteStride * site_idx;
        Ws16(offset, spot.x);
        Ws16(offset + 2, spot.y);
        Put(spot.x, spot.y, kSite);
        std::string name, text;
        if (scn[offset + 24] == 1) {
            int word_idx = Dice(1, 10, -1);
            name = Str(0x1000 + 10 * word_idx, 10) + " Temple";
            text = "#%03d|The %s can bless your|armies or give you|quests|\n";
        } else {
            int fmt_idx = Dice(1, R16(kSiteFormats), -1);
            int idx1 = Dice(1, 10, -1);
            std::string suffix = Str(0xf9c + 10 * idx1, 10);
            int idx2 = Dice(1, 10, -1);
            std::string word = Str(0xf38 + 10 * idx2, 10) + suffix;
            name = Format(Str(kSiteFormats + 2 + 20 * fmt_idx, 20), word);
            text = "#%03d|%s is|inhabited by monsters and|full of treasure!|\n";
        }
        Wstr(offset + 4, name);
        spc += Format(text, site_idx, name);
    }

    // --- terrain to tiles ----------------------------------------------------

    /**
     * @brief Assigns initial tile indices for every terrain cell, castle tiles
     *        for cities, and temple tiles for the first eight sites.
     */
    void ToTiles() {
        for (int y = 0; y < kMapH; y++) {
            for (int x = 0; x < kMapW; x++) {
                int terrain = At(x, y);
                if (terrain == kCity && At(x, y - 1) != kCity && At(x - 1, y) != kCity && x > 0 &&
                    y > 0) {
                    int castle = R16(kTiles + 64 * kCity) & 0xff;
                    SetTile(x, y, castle);
                    SetTile(x + 1, y, castle + 1);
                    SetTile(x, y + 1, castle + 16);
                    SetTile(x + 1, y + 1, castle + 17);
                } else if (terrain == kSite) {
                    SetTile(x, y, TileFor(kSite, 0, x, y));
                } else if (terrain != kCity) {
                    SetTile(x, y, TileFor(terrain, 0, x, y));
                }
            }
        }
        for (int i = 0; i < 8; i++) {
            int offset = kSSites + kSiteStride * i;
            SetTile(S16(offset), S16(offset + 2), R16(kTiles + 64 * kSite + 4) & 0xff);
        }
    }

    /**
     * @brief Builds an 8-bit neighbour mask (bit i for neighbour i), counting
     *        the map's outer edge as matching.
     */
    int Mask(int x, int y, const std::function<bool(int)>& test) const {
        int mask = 0;
        for (int i = 0; i < 8; i++) {
            int adj_x = x + Nx(i), adj_y = y + Ny(i);
            bool off = adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH;
            if (off || test(At(adj_x, adj_y))) mask |= 1 << i;
        }
        return mask;
    }

    /** @brief Looks up the tile variant for neighbour mask `mask` (-1 if none). */
    int Shape(int mask) const { return I8(memory[kShape + mask]); }

    /** @brief Calls `func(x, y)` for every tile whose terrain equals `terrain`. */
    template <class Func>
    void Each(int terrain, Func func) {
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++)
                if (At(x, y) == terrain) func(x, y);
    }

    /**
     * @brief Selects transition tile variants that join each terrain type to
     *        its neighbours, replacing unrepresentable shapes with fallback terrain.
     */
    void Shapes() {
        for (int y = 0; y < kMapH; y++) {
            for (int x = 0; x < kMapW; x++) {
                int terrain = At(x, y);
                if (terrain != kWater && terrain != kShore) continue;
                int variant = Shape(Mask(x, y, [](int adj) {
                    return adj == kWater || adj == kShore || adj == kBridge;
                }));
                if (variant < 0) {
                    Put(x, y, kMarsh);
                    SetTile(x, y, TileFor(kMarsh, 0, x, y));
                    MarshAround(x, y);
                    tiles[y * kMapW + x] &= 0x7fff;
                } else {
                    Put(x, y, kWater);
                    SetTile(x, y, TileFor(kWater, variant, x, y));
                }
            }
        }
        Each(kForest, [&](int x, int y) {
            int variant = Shape(Mask(x, y, [](int adj) { return adj == kForest; }));
            if (variant >= 0) {
                SetTile(x, y, TileFor(kForest, variant, x, y));
                Put(x, y, kForest);
            } else if (Dice(1, 10, 0) < 6) {
                SetTile(x, y, TileFor(kPlain, 0, x, y));
                Put(x, y, kPlain);
            } else {
                SetTile(x, y, TileFor(kForest, 13, x, y));
            }
        });
        Each(kMountains, [&](int x, int y) {
            int variant = Shape(Mask(x, y, [](int adj) { return adj == kMountains; }));
            if (variant >= 0) {
                SetTile(x, y, TileFor(kMountains, variant, x, y));
                Put(x, y, kMountains);
            } else {
                SetTile(x, y, TileFor(kHills, 0, x, y));
                Put(x, y, kHills);
            }
        });
        Each(kHills, [&](int x, int y) {
            int variant =
                    Shape(Mask(x, y, [](int adj) { return adj == kMountains || adj == kHills; }));
            if (variant >= 0) {
                SetTile(x, y, TileFor(kHills, variant, x, y));
                Put(x, y, kHills);
            } else if (Dice(1, 10, 0) < 6) {
                SetTile(x, y, TileFor(kPlain, 0, x, y));
                Put(x, y, kPlain);
            } else {
                Put(x, y, kHills);
                SetTile(x, y, TileFor(kHills, 13, x, y));
            }
        });
        Each(kMarsh, [&](int x, int y) {
            // mostly the plain marsh; three in ten one of four others
            int offset = Dice(1, 10, 0) < 7 ? 2 * Parity(x, y) : 4 * Dice(1, 4, 0);
            SetTile(x, y, R16(kTiles + 64 * kMarsh + offset) & 0xff);
            Put(x, y, kMarsh);
        });
    }

    // --- bridges, crossings and roads ----------------------------------------

    /** @brief True if (x, y) is firm ground a bridge may land on. */
    bool Firm(int x, int y) const {
        int terrain = At(x, y);
        return terrain == kPlain || terrain == kForest || terrain == kHills;
    }

    /** @brief Computes the distance from each spot in `list` to its nearest city. */
    void NearestCities(SpotList& list, int count) {
        if (list.city_dist[0] != -1) return;
        for (int i = count - 1; i >= 0; i--) {
            if (list.x[i] == -1) continue;
            int best = 10000;
            for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--)
                best = std::min(best, Distance(list.x[i], list.y[i], Cx(city_idx), Cy(city_idx)));
            list.city_dist[i] = best;
        }
    }

    /**
     * @brief Picks the highest-scoring unchosen spot in `list`, favouring
     *        spots near cities and (if `apart`) keeping spots at least 10 apart.
     */
    int PickSpot(SpotList& list, int count, bool apart) {
        NearestCities(list, count);
        for (int i = count - 1; i >= 0; i--) {
            if (list.chosen[i] != 0 || list.x[i] == -1) continue;
            int best = 10000;
            for (int j = count - 1; j >= 0; j--) {
                if (list.chosen[j] != 0)
                    best = std::min(best, Distance(list.x[i], list.y[i], list.x[j], list.y[j]));
            }
            list.chosen_dist[i] = best;
        }
        int best_score = -1, pick = -1;
        for (int i = count - 1; i >= 0; i--) {
            if (list.chosen[i] != 0 || list.x[i] == -1) continue;
            if (apart && !(list.chosen_dist[i] > 9)) continue;
            int roll = Dice(1, 15, 1);
            int near_by = list.city_dist[i] < 31 ? 30 - list.city_dist[i] : 0;
            if (best_score < near_by + roll) {
                best_score = near_by + roll;
                pick = i;
            }
        }
        return pick;
    }

    /** @brief Places bridges across two-tile rivers. */
    void Bridges(SpotList& list) {
        int count = 0;
        for (int y = 1; y < kMapH - 2; y++) {
            for (int x = 1; x < kMapW - 2; x++) {
                int tile = Tile(x, y);
                bool across = (tile == 0x26 || tile == 0x16) &&
                              (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
                bool along = (tile == 0x21 || tile == 0x11) &&
                             (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
                if (!across && !along) continue;
                int east_west = tile == 0x26 ? 1 : 0;
                bool valid = east_west && ((Firm(x - 1, y) && Firm(x + 2, y)) ||
                                           (Firm(x, y - 1) && Firm(x, y + 2)));
                if (valid && count < 50) {
                    list.x[count] = x;
                    list.y[count] = y;
                    list.east_west[count] = east_west;
                    count++;
                }
            }
        }
        if (count == 0) return;
        for (int i; (i = PickSpot(list, count, true)) != -1;) {
            list.chosen[i] = 1;
            int x = list.x[i], y = list.y[i];
            if (list.east_west[i] == 0) {
                SetTile(x, y, 0x84);
                SetTile(x, y + 1, 0x94);
                Put(x, y + 1, kBridge);
                Put(x, y, kBridge);
                SetRoad(x, y + 2, 1);
                SetRoad(x, y - 1, 1);
            } else {
                SetTile(x, y, 0x85);
                SetTile(x + 1, y, 0x86);
                Put(x + 1, y, kBridge);
                Put(x, y, kBridge);
                SetRoad(x + 2, y, 1);
                SetRoad(x - 1, y, 1);
            }
        }
    }

    /**
     * @brief Marks up to ten sea crossings on shore tiles that face three
     *        tiles of open water and lie at least 15 tiles from other candidates.
     */
    void Crossings(SpotList& list) {
        for (int y = 1; y < kMapH - 2; y++) {
            for (int x = 1; x < kMapW - 2; x++) {
                if (Dice(1, 2, -1) != 0) continue;
                int dx = 0, dy = 0;
                switch (Tile(x, y)) {
                case 0x20:
                case 0x23:
                    dx = -1;
                    break;
                case 0x21:
                    dy = -1;
                    break;
                case 0x22:
                case 0x25:
                    dx = 1;
                    break;
                case 0x24:
                    dy = 1;
                    break;
                default:
                    continue;
                }
                bool open = true;
                for (int dist = 1; dist <= 3; dist++)
                    if (At(x + dist * dx, y + dist * dy) != kWater) open = false;
                if (!open) continue;
                int near_by = 10000;
                for (int i = 49; i >= 0; i--) {
                    if (list.x[i] != -1)
                        near_by = std::min(near_by, Distance(x, y, list.x[i], list.y[i]));
                }
                if (near_by <= 14) continue;
                for (int i = 0; i < 50; i++) {
                    if (list.x[i] == -1) {
                        list.x[i] = x;
                        list.y[i] = y;
                        break;
                    }
                }
            }
        }
        list.city_dist[0] = -1;
        for (int count = 0, i; count < 10 && (i = PickSpot(list, 50, false)) != -1; count++) {
            list.chosen[i] = 1;
            tiles[list.y[i] * kMapW + list.x[i]] |= 0x8000;
        }
    }

    /** @brief True if city `city_idx` has already been connected by road. */
    bool IsServed(int city_idx) const {
        return city_idx >= 0 && city_idx < static_cast<int>(served.size()) ? served[city_idx] != 0
                                                                           : false;
    }
    /** @brief Marks city `city_idx` as connected by road. */
    void Serve(int city_idx) {
        if (city_idx >= 0 && city_idx < static_cast<int>(served.size())) served[city_idx] = 1;
    }

    /**
     * @brief Picks a pair of unserved cities 30..70 tiles apart to connect by road.
     */
    std::optional<std::pair<int, int>> RoadPair() {
        for (;;) {
            int first = -1;
            for (int tries = 80; tries > 0; tries--) {
                first = Dice(1, CityCount(), -1);
                if (!IsServed(first)) break;
                first = -1;
            }
            if (first == -1) return std::nullopt;
            Serve(first);
            int second = -1, best = -1;
            for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--) {
                if (IsServed(city_idx)) continue;
                int dist = Distance(Cx(first), Cy(first), Cx(city_idx), Cy(city_idx));
                if (dist > 29 && dist < 71) {
                    int roll = Dice(1, 1000, 0);
                    if (best < roll) {
                        best = roll;
                        second = city_idx;
                    }
                }
            }
            if (second != -1) {
                Serve(second);
                return std::make_pair(first, second);
            }
        }
    }

    /** @brief Picks a firm tile adjacent to city `city_idx`'s footprint to start a road. */
    std::optional<Point> RoadEnd(int city_idx) {
        for (int tries = 8; tries > 0; tries--) {
            const int* off = kRoundCity[Dice(1, 12, -1)];
            int x = Cx(city_idx) + off[0], y = Cy(city_idx) + off[1];
            int terrain = At(x, y);
            if (terrain == kPlain || terrain == kForest || terrain == kHills) return Point{x, y};
        }
        return std::nullopt;
    }

    /**
     * @brief Lays a road along the pathfinder route from `from` to `to_pt`,
     *        marking cities within 10 tiles every six steps as served.
     */
    void LayRoad(Point from, Point to_pt) {
        move::RoadGround ground;
        ground.terrain = [this](int x, int y) { return TerrainOf(Tile(x, y)); };
        ground.road = [this](int x, int y) { return Road(x, y) != 0; };
        ground.crossing = [this](int x, int y) {
            return x >= 0 && y >= 0 && x < kMapW && y < kMapH &&
                   (tiles[y * kMapW + x] & 0x8000) != 0;
        };
        auto route =
                move::RoadRoute(ground, kRoadCost, kMapW, kMapH, from.x, from.y, to_pt.x, to_pt.y);
        if (!route) return;
        int x = from.x, y = from.y, since = 0;
        for (size_t i = 0; i < route->size() && i < 200; i++) {
            since++;
            if (x == to_pt.x && y == to_pt.y) break;
            x = (*route)[i].first;
            y = (*route)[i].second;
            int terrain = At(x, y);
            if (terrain != kShore && terrain != kWater && terrain != kBridge) SetRoad(x, y, 1);
            if (since > 5) {
                since = 0;
                for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--) {
                    if (Distance(x, y, Cx(city_idx), Cy(city_idx)) < 10) Serve(city_idx);
                }
            }
        }
    }

    /** @brief Converts terrain to tiles, applies transition shapes twice, and clears road/crossing
     * bits. */
    void RoadTiles() {
        ToTiles();
        Shapes();
        Shapes();
        for (int i = 0; i < kMapW * kMapH; i++) {
            tiles[i] &= 0x7fff;
            roads[i] &= 0xe0;
        }
    }

    /** @brief Places bridges and sea crossings and resets road service tracking. */
    void RoadCrossings() {
        SpotList list;
        for (int i = 0; i < 50; i++) {
            list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] = list.chosen_dist[i] =
                    -1;
            list.chosen[i] = 0;
        }
        Bridges(list);
        for (int i = 49; i >= 0; i--) {
            if (list.chosen[i] == 0)
                list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] =
                        list.chosen_dist[i] = -1;
        }
        Crossings(list);
        served.assign(100, 0);
        shown = 4;
    }

    /**
     * @brief Connects cities by road a pair at a time, returning a progress
     *        percentage when the bar advances or `std::nullopt` when finished.
     */
    std::optional<int> RoadsUntilShown() {
        for (;;) {
            std::pair<int, int> pair;
            if (pending) {
                pair = *pending;
                pending.reset();
            } else {
                auto next_pair = RoadPair();
                if (!next_pair) break;
                pair = *next_pair;
                if (shown < 10 && Dice(1, 3, -1) == 0) {
                    pending = pair;
                    return 80 + shown++;
                }
            }
            auto start_pt = RoadEnd(pair.first);
            std::optional<Point> end_pt;
            if (start_pt) end_pt = RoadEnd(pair.second);
            if (start_pt && end_pt && !(start_pt->x == end_pt->x && start_pt->y == end_pt->y))
                LayRoad(*start_pt, *end_pt);
        }
        for (int i = 0; i < kMapW * kMapH; i++)
            if (memory[kGrid + i] == kSite) roads[i] &= 0xe0;
        return std::nullopt;
    }

    /**
     * @brief Assigns road piece shapes from neighbouring roads and bridges,
     *        and randomly swaps a few straight pieces to their alternate look.
     */
    void RoadShapes() {
        for (int y = 0; y < kMapH; y++) {
            for (int x = 0; x < kMapW; x++) {
                if (Road(x, y) == 0) continue;
                int mask = 0;
                for (int i = 0; i < 8; i++) {
                    int adj_x = x + R16(kNbRoad + 4 * i), adj_y = y + R16(kNbRoad + 4 * i + 2);
                    if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                        (Road(adj_x, adj_y) != 0 || TerrainOf(Tile(adj_x, adj_y)) == kBridge))
                        mask |= 1 << i;
                }
                int variant = I8(memory[kRoadShape + mask]);
                if (variant >= 0)
                    SetRoad(x, y, variant + 1);
                else if (Dice(1, 10, 0) < 6) {
                    SetTile(x, y, TileFor(kPlain, 0, x, y));
                    Put(x, y, kPlain);
                } else {
                    SetTile(x, y, TileFor(kHills, 13, x, y));
                }
            }
        }
        const int swaps[2][2] = {{2, 17}, {1, 16}};
        for (auto& swap_pair : swaps) {
            int count = Dice(1, 10, 10);
            for (int idx = 0; idx < count; idx++) {
                for (int tries = 0; tries < 10000; tries++) {
                    int x = Dice(1, kMapW, -1);
                    int y = Dice(1, kMapH, -1);
                    if (Road(x, y) == swap_pair[0]) {
                        SetRoad(x, y, swap_pair[1]);
                        break;
                    }
                }
            }
        }
    }

    // --- the sides and the cities' details -----------------------------------

    /** @brief Reads the cities back off the tile map in row order. */
    void CityList() {
        int count = 0;
        for (int y = 0; y < kMapH; y++) {
            for (int x = 0; x < kMapW; x++) {
                if (Tile(x, y) != 0x60) continue;
                int offset = Crec(count);
                Ws16(offset, x);
                Ws16(offset + 2, y);
                scn[offset + 21] = 15;
                scn[offset + 20] = static_cast<uint8_t>(Dice(1, 3, 2));
                count++;
            }
        }
        SetCityCount(count);
    }

    /**
     * @brief Picks a random city in one of the map's eight regions (4x2);
     *        after 202 tries, returns whatever the last draw was.
     */
    int CityIn(int region) {
        int min_x = (region % 4) * 28, min_y = region / 4 * 78;
        int city_idx = 0;
        for (int tries = 0; tries < 202; tries++) {
            city_idx = Dice(1, CityCount(), -1);
            int x = Cx(city_idx), y = Cy(city_idx);
            if (x >= min_x && x < min_x + 28 && y >= min_y && y < min_y + 78) break;
        }
        return city_idx;
    }

    /** @brief Byte offset of side `side_idx`'s record in `scn`. */
    static int SideRec(int side_idx) { return kSSideRec + 20 * side_idx; }

    /**
     * @brief Assigns each side a capital in its own region, at least 12 tiles
     *        from the map edge and 20 from other capitals along both axes.
     */
    void Capitals() {
        for (int round = 1;; round++) {
            bool used[8] = {false};
            for (int side_idx = 0; side_idx < 8; side_idx++) {
                Ws16(SideRec(side_idx) + 6, -100);
                Ws16(SideRec(side_idx) + 8, -100);
            }
            bool again = false;
            for (int side_idx = 0; side_idx < 8 && !again; side_idx++) {
                int region;
                do region = Dice(1, 8, -1);
                while (used[region]);
                used[region] = true;
                int city_idx = 0, tries = 0;
                bool valid = false;
                for (; !valid && tries < 100; tries++) {
                    city_idx = CityIn(region);
                    int x = Cx(city_idx), y = Cy(city_idx);
                    if (x < 12 || x > 100 || y < 12 || y > 144) continue;
                    valid = true;
                    for (int other = 0; other < 8; other++) {
                        if (std::abs(x - S16(SideRec(other) + 6)) < 20 ||
                            std::abs(y - S16(SideRec(other) + 8)) < 20) {
                            valid = false;
                            break;
                        }
                    }
                }
                // a find on the hundredth try counts as none, as the original has it
                if (tries > 99 && round < 10) {
                    again = true;
                    break;
                }
                W16(kRegionClass + 2 * region, R16(kSideClass + 2 * side_idx));
                Ws16(SideRec(side_idx) + 6, Cx(city_idx));
                Ws16(SideRec(side_idx) + 8, Cy(city_idx));
            }
            if (!again) break;
        }
        Crown();
    }

    /** @brief Marks each capital's owner byte and paints its castle in the side's colours. */
    void Crown() {
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            int x = S16(SideRec(side_idx) + 6), y = S16(SideRec(side_idx) + 8);
            for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
                if (Cx(city_idx) == x && Cy(city_idx) == y) {
                    scn[Crec(city_idx) + 21] = static_cast<uint8_t>(side_idx);
                    break;
                }
            }
            int tile_base = side_idx < 6 ? 0x62 + 2 * side_idx : 0x80 + 2 * (side_idx - 6);
            SetTile(x, y, tile_base);
            SetTile(x + 1, y, tile_base + 1);
            SetTile(x, y + 1, tile_base + 16);
            SetTile(x + 1, y + 1, tile_base + 17);
        }
    }

    /**
     * @brief Chooses the eight capitals by local search over travel-cost
     *        fairness (Options::fair_capitals).
     */
    void FairCapitals() {
        constexpr int kSides = 8;
        constexpr double kFairGap = 3.0;  // turns
        constexpr double kRemote = 5.0;   // turns from the nearest capital
        constexpr int kStarts = 12, kPasses = 40;
        const int num_cities = CityCount();
        if (num_cities < kSides) {
            Capitals();
            return;
        }
        travel::Ground ground;
        ground.w = kMapW;
        ground.h = kMapH;
        ground.terrain.resize(static_cast<size_t>(kMapW) * kMapH);
        ground.road.resize(static_cast<size_t>(kMapW) * kMapH);
        ground.crossing.resize(static_cast<size_t>(kMapW) * kMapH);
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++) {
                int cell = y * kMapW + x, terrain = TerrainOf(Tile(x, y));
                ground.terrain[cell] = terrain;
                ground.road[cell] = Road(x, y) != 0;
                ground.crossing[cell] = (tiles[cell] & 0x8000) != 0 || terrain == kBridge;
            }
        auto footprint = [&](int city_idx) {
            std::vector<int> cells;
            for (auto& off : kFootprint)
                if (Cx(city_idx) + off[0] < kMapW && Cy(city_idx) + off[1] < kMapH)
                    cells.push_back((Cy(city_idx) + off[1]) * kMapW + Cx(city_idx) + off[0]);
            return cells;
        };
        // a port city is a crossing: its footprint touches the sea
        for (int city_idx = 0; city_idx < num_cities; city_idx++) {
            bool port = false;
            for (auto& ring : kRoundCity) {
                int terrain = TerrainOf(Tile(Cx(city_idx) + ring[0], Cy(city_idx) + ring[1]));
                if (terrain == kBridge || terrain == kWater || terrain == kShore) port = true;
            }
            if (port)
                for (int cell : footprint(city_idx)) ground.crossing[cell] = 1;
        }
        std::vector<std::vector<int>> cost_matrix(num_cities,
                                                  std::vector<int>(num_cities, travel::kUnreached));
        for (int city_idx = 0; city_idx < num_cities; city_idx++) {
            auto field = travel::Costs(ground, footprint(city_idx));
            for (int other = 0; other < num_cities; other++)
                for (int cell : footprint(other))
                    cost_matrix[city_idx][other] =
                            std::min(cost_matrix[city_idx][other], field[cell]);
        }
        // how unfair a choice is; lower is better
        std::vector<int> owner(num_cities);
        auto unfairness = [&](const std::array<int, kSides>& caps) {
            double gap = 1e9;
            for (int i = 0; i < kSides; i++)
                for (int j = i + 1; j < kSides; j++)
                    gap = std::min(gap, static_cast<double>(cost_matrix[caps[i]][caps[j]]));
            if (gap >= travel::kUnreached) return 1e12;  // a capital cut off from another
            gap /= travel::kTurn;
            std::array<int, kSides> share{};
            int remote = 0;
            double reach = 0;
            for (int city_idx = 0; city_idx < num_cities; city_idx++) {
                if (std::find(caps.begin(), caps.end(), city_idx) != caps.end()) continue;
                int best = travel::kUnreached, who = 0;
                for (int side_idx = 0; side_idx < kSides; side_idx++)
                    if (cost_matrix[caps[side_idx]][city_idx] < best) {
                        best = cost_matrix[caps[side_idx]][city_idx];
                        who = side_idx;
                    }
                if (best == travel::kUnreached) {
                    remote++;
                    continue;
                }
                share[who]++;
                reach += static_cast<double>(best) / travel::kTurn;
                if (best > kRemote * travel::kTurn) remote++;
            }
            auto [min_it, max_it] = std::minmax_element(share.begin(), share.end());
            return 3.0 * remote + 1.0 * (*max_it - *min_it) + reach / num_cities +
                   (gap < kFairGap ? 10.0 * (kFairGap - gap) : 0.0);
        };
        std::array<int, kSides> best{};
        double best_score = 1e18;
        for (int start = 0; start < kStarts; start++) {
            std::array<int, kSides> caps{};
            for (int side_idx = 0; side_idx < kSides; side_idx++) {
                int city_idx;
                do city_idx = Dice(1, num_cities, -1);
                while (std::find(caps.begin(), caps.begin() + side_idx, city_idx) !=
                       caps.begin() + side_idx);
                caps[side_idx] = city_idx;
            }
            double score = unfairness(caps);
            for (int pass = 0; pass < kPasses; pass++) {
                bool better = false;
                for (int side_idx = 0; side_idx < kSides; side_idx++)
                    for (int city_idx = 0; city_idx < num_cities; city_idx++) {
                        if (std::find(caps.begin(), caps.end(), city_idx) != caps.end()) continue;
                        int was = caps[side_idx];
                        caps[side_idx] = city_idx;
                        double now = unfairness(caps);
                        if (now < score - 1e-9) {
                            score = now;
                            better = true;
                        } else
                            caps[side_idx] = was;
                    }
                if (!better) break;
            }
            if (score < best_score) {
                best_score = score;
                best = caps;
            }
        }
        // the sides take the capitals in a random order; a side's armies favour
        // the region its capital stands in
        for (int side_idx = kSides - 1; side_idx > 0; side_idx--)
            std::swap(best[side_idx], best[Dice(1, side_idx + 1, -1)]);
        for (int side_idx = 0; side_idx < kSides; side_idx++) {
            int x = Cx(best[side_idx]), y = Cy(best[side_idx]);
            int region = std::min(3, x / 28) + 4 * std::min(1, y / 78);
            W16(kRegionClass + 2 * region, R16(kSideClass + 2 * side_idx));
            Ws16(SideRec(side_idx) + 6, x);
            Ws16(SideRec(side_idx) + 8, y);
        }
        Crown();
    }

    /** @brief Assigns each side one of five names and 3d50+20 starting gold. */
    void Sides() {
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            int name_idx = Dice(1, 5, -1);
            std::string name = Str(kSideNames + 100 * side_idx + 20 * name_idx, 20);
            for (int i = 0; i < 20; i++) scn[20 * side_idx + i] = 0;
            Wstr(20 * side_idx, name);
        }
        for (int side_idx = 0; side_idx < 8; side_idx++)
            Ws16(SideRec(side_idx) + 2, Dice(3, 50, 20));
    }

    /** @brief Inspects which terrain features touch city `city_idx`'s 2x2 footprint. */
    Surroundings SurroundingsOf(int city_idx) const {
        int x = Cx(city_idx), y = Cy(city_idx);
        auto touches = [&](int terrain) {
            return Near(x, y, terrain) || Near(x + 1, y, terrain) || Near(x, y + 1, terrain) ||
                   Near(x + 1, y + 1, terrain);
        };
        return Surroundings{touches(kShore), touches(kForest), touches(kRoad), touches(kHills),
                            touches(kMarsh)};
    }

    /**
     * @brief Computes a city's value (0..9): 9 for a capital, otherwise
     *        1d4-1 +4 by shore, +2 by road, -1 by forest, -2 by marsh.
     */
    int Value(int city_idx, const Surroundings& surr) {
        if (scn[Crec(city_idx) + 21] != 15) return 9;
        int val = Dice(1, 4, -1);
        if (surr.shore) val += 4;
        if (surr.road) val += 2;
        if (surr.forest) val -= 1;
        if (surr.marsh) val -= 2;
        if (val > 8) val = 9;
        if (val < 1) val = 0;
        return val;
    }

    /** @brief Generates a two- or three-syllable name for city `city_idx`. */
    std::string CityName(int city_idx, const Surroundings& surr) {
        bool three = Dice(1, 10, 0) < 7;
        int idx1 = Dice(1, 20, -1);
        std::string name = Str(0xa86 + 10 * idx1, 10);
        if (three) {
            int idx2 = Dice(1, 20, -1);
            name += Str(0xb4e + 10 * idx2, 10);
        }
        int suffix_table = -1;
        if (Dice(1, 10, 0) < 7) {
            if (surr.marsh)
                suffix_table = 0xda6;
            else if (surr.forest)
                suffix_table = 0xcde;
            else if (surr.shore)
                suffix_table = 0xd42;
            else if (surr.hills)
                suffix_table = 0xe0a;
            else if (Dice(1, 10, 0) < 5)
                suffix_table = 0xe6e;
        }
        if (suffix_table != -1) {
            int idx3 = Dice(1, 10, -1);
            name += Str(suffix_table + 10 * idx3, 10);
        } else {
            int idx3 = Dice(1, 20, -1);
            name += Str(0xc16 + 10 * idx3, 10);
        }
        name = name.substr(0, 15);
        int offset = Crec(city_idx) + 4;
        for (int i = 0; i < 16; i++) scn[offset + i] = 0;
        Wstr(offset, name);
        return name;
    }

    /** @brief True if any city before `city_idx` already has `name`. */
    bool NameTaken(int city_idx, const std::string& name) const {
        for (int j = 0; j < city_idx; j++)
            if (CstrOf(scn, Crec(j) + 4, 16) == name) return true;
        return false;
    }

    /**
     * @brief Generates city `city_idx`'s name, income, and three-line `.CTY`
     *        description.
     */
    void CityText(int city_idx, int val, const Surroundings& surr) {
        std::string name = CityName(city_idx, surr);
        // the scenario generator's names never repeat (the original's may)
        for (int tries = 0; modern && tries < 200 && NameTaken(city_idx, name); tries++)
            name = CityName(city_idx, surr);
        Ws16(Crec(city_idx) + 42, val * 2 + Dice(1, 8, 0) + 14);
        auto grade = [&]() { return std::min(9, std::max(0, Dice(1, 3, val - 2))); };
        int grade1 = grade();
        int grade2 = grade();
        auto word_at = [&](int offset, int idx) { return Str(offset + 16 * idx, 16); };
        std::string part_a, part_b, part_c;
        bool plain = !surr.forest && !surr.marsh && !surr.hills;
        if (plain || Dice(1, 100, 0) > 79) {
            if (Dice(1, 100, 0) < 50) {
                part_a = word_at(0x16a8, Dice(1, 10, -1));
                part_b = word_at(0x1748, Dice(1, 10, -1));
                part_c = word_at(0x17e8, Dice(1, 10, -1));
            } else {
                part_a = word_at(0x14c8, Dice(1, 10, -1));
                part_b = word_at(0x1568, Dice(1, 10, -1));
                part_c = word_at(0x1608, Dice(1, 10, -1));
            }
        } else {
            int sub_idx;
            if (surr.marsh)
                sub_idx = Dice(1, 2, 7);
            else if (surr.hills)
                sub_idx = Dice(1, 4, 3);
            else
                sub_idx = Dice(1, 4, -1);
            part_a = word_at(0x12e8, Dice(1, 10, -1));
            part_b = word_at(0x1388, Dice(1, 10, -1));
            part_c = word_at(0x1428, sub_idx);
        }
        std::string kind = word_at(0x1248, grade2);
        int adj_idx = Dice(1, 3, -1);
        std::string adj = Str(0x1068 + 0x30 * grade1 + 16 * adj_idx, 16);
        cty += Format("#%03d|%s is a %s|%s, %s|%s %s|\n", city_idx, name, adj, kind, part_a, part_b,
                      part_c);
    }

    /** @brief True if army type `type_id` has the flying flag (ARMYTYPE +54). */
    bool Flies(int type_id) const {
        const ArmyType* army_type = types.ById(type_id);
        return army_type && army_type->bonus[54] != 0;
    }
    /** @brief True if army type `type_id` is magical / an ally (ARMYTYPE +48). */
    bool Magical(int type_id) const {
        const ArmyType* army_type = types.ById(type_id);
        return army_type && army_type->bonus[48] != 0;
    }

    /** @brief Populates production slot `slot_idx` of city `city_idx` with army type `type_id`. */
    void SetSlot(int city_idx, int slot_idx, int type_id) {
        int offset = Crec(city_idx);
        const ArmyType* army_type = types.ById(type_id);
        scn[offset + 22 + slot_idx] = static_cast<uint8_t>(type_id);
        scn[offset + 26 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->time : 0);
        scn[offset + 30 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->strength : 0);
        scn[offset + 34 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->move : 0);
        scn[offset + 38 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->cost : 0);
    }

    /** @brief Chooses the up to four army production slots for city `city_idx`. */
    void Production(int city_idx, int val, const Surroundings& surr) {
        int x = Cx(city_idx), y = Cy(city_idx);
        int region = x * 4 / kMapW + y * 2 / kMapH * 4;
        int slots = val / 2;
        slots += Dice(1, 4, -1);
        if (surr.forest) slots++;
        if (surr.hills) slots++;
        if (slots > 3) slots = 4;
        if (slots < 1) slots = 0;
        auto rec = [](int entry) { return kProduction + 16 * entry; };
        auto suits = [&](int entry) {
            int terr = R16(rec(entry) + 6);
            return terr == 7 || (terr == 4 && surr.forest) || (terr == 5 && surr.hills) ||
                   R16(rec(entry) + 4) == R16(kRegionClass + 2 * region);
        };
        int slot_idx = 0;
        for (int entry = 0; entry < 29 && slot_idx < slots; entry++) {
            int type_id = R16(rec(entry));
            if (!allies && Magical(type_id)) continue;
            if (Dice(1, 10, -1) >= R16(rec(entry) + 2)) continue;
            bool valid = suits(entry) || (R16(rec(entry) + 6) == 3 && surr.shore);
            if (type_id == 5 && !surr.shore) valid = false;
            if (valid) SetSlot(city_idx, slot_idx++, type_id);
        }
        if (slot_idx == 0) SetSlot(city_idx, slot_idx++, R16(rec(0)));
        if (val > 6 && Dice(1, 10, -1) < 5 && slot_idx < 4) {
            bool flier = false;
            for (int j = 0; j < slot_idx; j++)
                if (Flies(scn[Crec(city_idx) + 22 + j])) flier = true;
            if (!flier && slot_idx < slots) {
                for (int entry = 0; entry < 29; entry++) {
                    int type_id = R16(rec(entry));
                    if (Flies(type_id) && !Magical(type_id) && suits(entry)) {
                        SetSlot(city_idx, slot_idx++, type_id);
                        break;
                    }
                }
            }
        }
        for (; slot_idx < 4; slot_idx++) scn[Crec(city_idx) + 22 + slot_idx] = 255;
    }

    /** @brief Picks a random magical (ally) army type id. */
    int MagicalType() {
        int count = 0;
        for (int type_id = 0; type_id < 28; type_id++)
            if (Magical(type_id)) count++;
        int pick = Dice(1, count, -1);
        int match_idx = 0, type_id = 0;
        for (; type_id < 28; type_id++) {
            if (!Magical(type_id)) continue;
            if (match_idx == pick) break;
            match_idx++;
        }
        return type_id > 27 || !Magical(type_id) ? 25 : type_id;
    }

    /** @brief Adds a magical army type to 2d3 cities when allies are enabled. */
    void AllyProduction() {
        int count = Dice(2, 3, 0);
        for (int tries = 0; count > 0 && tries < 100; tries++) {
            int city_idx = Dice(1, CityCount(), -1), offset = Crec(city_idx);
            int slot_idx = 0;
            for (int j = 0; j < 4; j++)
                if (scn[offset + 22 + j] < 128) slot_idx++;
            if (slot_idx < 4) {
                SetSlot(city_idx, slot_idx, MagicalType());
                count--;
            }
        }
    }

    /**
     * @brief Adds a strong non-magical, non-navy army type (strength >= 5) to
     *        each capital's last empty production slot.
     */
    void CapitalArmies() {
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            int x = S16(SideRec(side_idx) + 6), y = S16(SideRec(side_idx) + 8);
            int city_idx = -1;
            for (int i = 0; i < CityCount() && city_idx < 0; i++)
                if (Cx(i) == x && Cy(i) == y) city_idx = i;
            if (city_idx < 0) continue;
            int offset = Crec(city_idx);
            // the highest empty slot, else the last
            int slot_idx = 3;
            while (slot_idx >= 0 && scn[offset + 22 + slot_idx] < 128) slot_idx--;
            if (slot_idx < 0) slot_idx = 3;
            int draws = 0, pick = -1;
            while (draws <= 39) {
                int type_id = Dice(1, 28, -1);
                if (type_id == 5 || Magical(type_id)) continue;
                draws++;
                bool has = false;
                for (int j = 0; j < 4; j++)
                    if (scn[offset + 22 + j] == type_id) has = true;
                const ArmyType* army_type = types.ById(type_id);
                if (!has && army_type && army_type->strength >= 5) {
                    pick = type_id;
                    break;
                }
            }
            // one found on the fortieth draw is dropped, as the original has it
            if (pick >= 0 && draws < 40) scn[offset + 22 + slot_idx] = static_cast<uint8_t>(pick);
        }
    }

    /** @brief Populates names, descriptions, and army production for all cities. */
    void CityDetails() {
        for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
            Surroundings surr = SurroundingsOf(city_idx);
            int val = Value(city_idx, surr);
            CityText(city_idx, val, surr);
            Production(city_idx, val, surr);
        }
        if (allies) AllyProduction();
        CapitalArmies();
    }

    /**
     * @brief Places 41..70 signposts on open plain tiles off the roads, most
     *        pointing to the nearest city and a few bearing fixed messages.
     */
    std::string Signs() {
        int count = Dice(1, 30, 40);
        std::vector<uint8_t> out(2 + 104 * count, 0);
        SetI16(out, 0, count);
        auto put_str = [&](int offset, const std::string& text) {
            for (size_t i = 0; i < text.size() && i < 49; i++)
                out[offset + i] = static_cast<uint8_t>(text[i]);
        };
        int fixed = 0;
        for (int i = 0; i < count; i++) {
            int x = 0, y = 0;
            for (int tries = 0; tries < kSafety; tries++) {
                x = Dice(1, kMapW, -1);
                y = Dice(1, kMapH, -1);
                if (TerrainOf(Tile(x, y)) == kPlain && !Near(x, y, kCity) && !Near(x, y, kTower) &&
                    Road(x, y) == 0)
                    break;
            }
            SetTile(x, y, 0);
            int offset = 2 + 104 * i;
            SetI16(out, offset, x);
            SetI16(out, offset + 2, y);
            if (Dice(1, 10, 0) < 4 && fixed < R16(kSigns)) {
                put_str(offset + 4, Str(kSigns + 2 + 60 * fixed, 30));
                put_str(offset + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
                fixed++;
            } else {
                int city_idx = -1, best = 1000;
                for (int j = 0; j < CityCount(); j++) {
                    int dist = Distance(x, y, Cx(j), Cy(j));
                    if (dist < best) {
                        best = dist;
                        city_idx = j;
                    }
                }
                if (city_idx < 0) continue;
                int city_x = Cx(city_idx), city_y = Cy(city_idx);
                put_str(offset + 4,
                        Format(Str(kSignCity, 20), CstrOf(scn, Crec(city_idx) + 4, 16)));
                put_str(offset + 54,
                        Format(Str(kSignLeagues, 20), Distance(x, y, city_x, city_y) * 2,
                               std::string(kCompass[Compass(x, y, city_x, city_y)])));
            }
        }
        return std::string(out.begin(), out.end());
    }

    // --- the whole -------------------------------------------------------------

    /** @brief Packs the generated scenario, tile map, roads, signposts, and texts into `files`. */
    void WriteFiles(Files& files) {
        std::string map(2UZ * kMapW * kMapH, '\0');
        for (int i = 0; i < kMapW * kMapH; i++) {
            map[2UZ * i] = static_cast<char>(tiles[i] & 0xff);
            map[2 * i + 1] = static_cast<char>(tiles[i] >> 8);
        }
        std::string road(static_cast<size_t>(kMapW) * kMapH, '\0');
        for (int i = 0; i < kMapW * kMapH; i++) road[i] = static_cast<char>(roads[i] & 0x1f);
        files["SCN"] = std::string(scn.begin(), scn.end());
        files["MAP"] = map;
        files["RD"] = road;
        files["SGN"] = sgn;
        files["CTY"] = cty;
        files["SPC"] = spc;
    }

    // --- the modern scenario generator (warlords/scengen) --------------------

    /** @brief Returns a copy of the 112x156 terrain grid. */
    std::vector<uint8_t> Grid() const {
        std::vector<uint8_t> grid(static_cast<size_t>(kMapW) * kMapH);
        for (int i = 0; i < kMapW * kMapH; i++) grid[i] = memory[kGrid + i];
        return grid;
    }

    /** @brief Finds all two-tile river stretches that can be bridged. */
    std::vector<scengen::Span> Spans() const {
        std::vector<scengen::Span> out;
        for (int y = 1; y < kMapH - 2; y++)
            for (int x = 1; x < kMapW - 2; x++) {
                int tile = Tile(x, y);
                bool across = (tile == 0x26 || tile == 0x16) &&
                              (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
                bool along = (tile == 0x21 || tile == 0x11) &&
                             (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
                if (across && Firm(x - 1, y) && Firm(x + 2, y)) out.push_back({x, y, true});
                if (along && Firm(x, y - 1) && Firm(x, y + 2)) out.push_back({x, y, false});
            }
        return out;
    }

    /** @brief Lays the modern scenario generator's planned roads, bridges, and crossings. */
    void LayPlannedRoads() {
        std::vector<std::pair<int, int>> where;
        for (int city_idx = 0; city_idx < CityCount(); city_idx++)
            where.push_back({Cx(city_idx), Cy(city_idx)});
        auto spans = Spans();
        auto road_plan = scengen::PlanRoads(Grid(), kMapW, kMapH, where, spans, *rng);
        SpotList list;
        for (int i = 0; i < 50; i++) {
            list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] = list.chosen_dist[i] =
                    -1;
            list.chosen[i] = 0;
        }
        int count = 0;
        for (int span_idx : road_plan.bridges) {
            auto& bridge = spans[span_idx];
            if (bridge.ew) {
                SetTile(bridge.x, bridge.y, 0x85);
                SetTile(bridge.x + 1, bridge.y, 0x86);
                Put(bridge.x, bridge.y, kBridge);
                Put(bridge.x + 1, bridge.y, kBridge);
            } else {
                SetTile(bridge.x, bridge.y, 0x84);
                SetTile(bridge.x, bridge.y + 1, 0x94);
                Put(bridge.x, bridge.y, kBridge);
                Put(bridge.x, bridge.y + 1, kBridge);
            }
            if (count < 50) {
                list.x[count] = bridge.x;
                list.y[count] = bridge.y;
                list.chosen[count] = 1;
                count++;
            }
        }
        for (int i = 0; i < kMapW * kMapH; i++)
            if (road_plan.road[i]) SetRoad(i % kMapW, i / kMapW, 1);
        // the crossings keep their distance from the bridges
        Crossings(list);
        Landings();
    }

    /**
     * @brief Ensures every landmass containing a city or site has at least one
     *        port city or sea crossing.
     */
    void Landings() {
        auto sea = [&](int x, int y) {
            int terrain = At(x, y);
            return terrain == kWater || terrain == kShore;
        };
        std::vector<int> part(static_cast<size_t>(kMapW) * kMapH, -1);
        int parts = 0;
        for (int start = 0; start < kMapW * kMapH; start++) {
            if (part[start] >= 0 || sea(start % kMapW, start / kMapW)) continue;
            std::vector<int> todo{start};
            part[start] = parts;
            for (size_t idx = 0; idx < todo.size(); idx++) {
                int x = todo[idx] % kMapW, y = todo[idx] / kMapW;
                for (int dir = 0; dir < 8; dir++) {
                    int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
                    if (adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH ||
                        sea(adj_x, adj_y))
                        continue;
                    int adj_idx = adj_y * kMapW + adj_x;
                    if (part[adj_idx] < 0) {
                        part[adj_idx] = parts;
                        todo.push_back(adj_idx);
                    }
                }
            }
            parts++;
        }
        std::vector<uint8_t> needs(parts, 0), has_landing(parts, 0);
        for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
            int part_id = part[Cy(city_idx) * kMapW + Cx(city_idx)];
            if (part_id < 0) continue;
            needs[part_id] = 1;
            for (auto& ring : kRoundCity)
                if (sea(Cx(city_idx) + ring[0], Cy(city_idx) + ring[1])) has_landing[part_id] = 1;
        }
        for (int i = 0; i < kNSites; i++) {
            int x = S16(kSSites + kSiteStride * i), y = S16(kSSites + kSiteStride * i + 2);
            if (x >= 0 && y >= 0 && x < kMapW && y < kMapH && part[y * kMapW + x] >= 0)
                needs[part[y * kMapW + x]] = 1;
        }
        for (int i = 0; i < kMapW * kMapH; i++)
            if (tiles[i] & 0x8000) {
                int x = i % kMapW, y = i / kMapW;
                for (int dir = 0; dir < 8; dir++) {
                    int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
                    if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                        part[adj_y * kMapW + adj_x] >= 0)
                        has_landing[part[adj_y * kMapW + adj_x]] = 1;
                }
            }
        for (int i = 0; i < kMapW * kMapH; i++) {
            int x = i % kMapW, y = i / kMapW;
            if (!sea(x, y) || (tiles[i] & 0x8000)) continue;
            int touched = -1;
            for (int dir = 0; dir < 8; dir += 2) {  // the four sides
                int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
                if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                    part[adj_y * kMapW + adj_x] >= 0)
                    touched = part[adj_y * kMapW + adj_x];
            }
            if (touched >= 0 && needs[touched] && !has_landing[touched]) {
                tiles[i] |= 0x8000;
                has_landing[touched] = 1;
            }
        }
    }

    /**
     * @brief Assigns road pieces for the modern scenario generator from the 4
     *        cardinal neighbours and extends road ends into adjacent cities.
     */
    void ModernRoadShapes() {
        // the piece for each N 1, E 2, S 4, W 8 combination
        static const int kPiece[16] = {0, 15, 14, 10, 13, 2, 11, 7, 12, 9, 1, 6, 8, 5, 4, 3};
        const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        std::vector<int> shape(static_cast<size_t>(kMapW) * kMapH, 0);
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++) {
                if (Road(x, y) == 0) continue;
                int mask = 0, city = -1;
                for (int dir = 0; dir < 4; dir++) {
                    int adj_x = x + dx[dir], adj_y = y + dy[dir];
                    if (adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH) continue;
                    if (Road(adj_x, adj_y) != 0 || At(adj_x, adj_y) == kBridge)
                        mask |= 1 << dir;
                    else if (At(adj_x, adj_y) == kCity && city < 0)
                        city = dir;
                }
                // a road's end runs into the city beside it
                if (city >= 0 && std::popcount(static_cast<unsigned>(mask)) <= 1) mask |= 1 << city;
                if (mask == 0) {
                    SetRoad(x, y, 0);
                    continue;
                }
                shape[y * kMapW + x] = kPiece[mask];
            }
        for (int i = 0; i < kMapW * kMapH; i++)
            if (shape[i]) SetRoad(i % kMapW, i / kMapW, shape[i]);
        // a few straight pieces take their standing-stone look
        const int swaps[2][2] = {{2, 17}, {1, 16}};
        for (auto& swap_pair : swaps) {
            int count = Dice(1, 6, 3);
            for (int idx = 0; idx < count; idx++)
                for (int tries = 0; tries < 2000; tries++) {
                    int x = Dice(1, kMapW, -1), y = Dice(1, kMapH, -1);
                    if (Road(x, y) == swap_pair[0]) {
                        SetRoad(x, y, swap_pair[1]);
                        break;
                    }
                }
        }
    }

    /** @brief Places signposts along roads for the modern scenario generator. */
    std::string ModernSigns() {
        std::vector<Point> spots;
        for (int tries = 0; tries < 20000 && static_cast<int>(spots.size()) < 60; tries++) {
            int x = Dice(1, kMapW - 6, 2), y = Dice(1, kMapH - 6, 2);
            if (At(x, y) != kPlain || Road(x, y) != 0 || Tile(x, y) == 0) continue;
            bool by_road = false, clear = true;
            for (int dyy = -1; dyy <= 1; dyy++)
                for (int dxx = -1; dxx <= 1; dxx++)
                    if (Road(x + dxx, y + dyy) != 0) by_road = true;
            for (int dyy = -3; dyy <= 3 && clear; dyy++)
                for (int dxx = -3; dxx <= 3 && clear; dxx++) {
                    int terrain = At(x + dxx, y + dyy);
                    if (terrain == kCity || terrain == kSite) clear = false;
                }
            for (auto& spot : spots)
                if (Reach(spot.x, spot.y, x, y) < 7) clear = false;
            if (by_road && clear) spots.push_back(Point{x, y});
        }
        int count = static_cast<int>(spots.size());
        std::vector<uint8_t> out(2 + 104 * count, 0);
        SetI16(out, 0, count);
        auto put_str = [&](int offset, const std::string& text) {
            for (size_t i = 0; i < text.size() && i < 49; i++)
                out[offset + i] = static_cast<uint8_t>(text[i]);
        };
        int fixed = 0;
        for (int i = 0; i < count; i++) {
            int x = spots[i].x, y = spots[i].y, offset = 2 + 104 * i;
            SetTile(x, y, 0);
            SetI16(out, offset, x);
            SetI16(out, offset + 2, y);
            if (Dice(1, 10, 0) < 3 && fixed < R16(kSigns)) {
                put_str(offset + 4, Str(kSigns + 2 + 60 * fixed, 30));
                put_str(offset + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
                fixed++;
                continue;
            }
            int city_idx = -1, best = 1 << 30;
            for (int j = 0; j < CityCount(); j++) {
                int dist = Distance(x, y, Cx(j), Cy(j));
                if (dist < best) {
                    best = dist;
                    city_idx = j;
                }
            }
            if (city_idx < 0) continue;
            put_str(offset + 4, Format(Str(kSignCity, 20), CstrOf(scn, Crec(city_idx) + 4, 16)));
            put_str(offset + 54,
                    Format(Str(kSignLeagues, 20), Distance(x, y, Cx(city_idx), Cy(city_idx)) * 2,
                           std::string(kCompass[Compass(x, y, Cx(city_idx), Cy(city_idx))])));
        }
        return std::string(out.begin(), out.end());
    }

    /** @brief Runs one stage of the modern scenario generator, returning progress %. */
    std::optional<int> NextModern(Files& files) {
        switch (stage++) {
        case 0:
            return 0;
        case 1: {
            plan = scengen::MakeWorld(world, *rng, kMapW, kMapH);
            for (int i = 0; i < kMapW * kMapH; i++) {
                memory[kGrid + i] = plan.terrain[i];
                roads[i] &= 0xe0;
            }
            Shores();
            return 40;
        }
        case 2: {
            SetCityCount(0);
            for (auto& [x, y] : plan.cities) {
                int city_idx = CityCount();
                Ws16(Crec(city_idx), x);
                Ws16(Crec(city_idx) + 2, y);
                SetCityCount(city_idx + 1);
            }
            SitesBegin();
            for (int i = 0; i < kNSites && i < static_cast<int>(plan.sites.size()); i++)
                SiteAt(i, Point{plan.sites[i].first, plan.sites[i].second});
            return 60;
        }
        case 3:
            RoadTiles();
            return 70;
        case 4:
            LayPlannedRoads();
            return 85;
        case 5:
            ModernRoadShapes();
            return 90;
        case 6:
            CityList();
            FairCapitals();
            Sides();
            return 94;
        case 7:
            CityDetails();
            return 96;
        case 8:
            sgn = ModernSigns();
            return 98;
        case 9:
            scn[kSRandom] = 0;
            scn[kSRandom + 1] = 0;  // an ordinary scenario
            WriteFiles(files);
            return 100;
        default:
            return std::nullopt;
        }
    }

    /** @brief Runs one stage of map generation, returning progress % or `std::nullopt` when done.
     */
    std::optional<int> Next(Files& files) {
        if (modern) return NextModern(files);
        switch (stage++) {
        case 0:
            return 0;
        case 1:
            // the map starts as sea
            for (int i = 0; i < kMapW * kMapH; i++) {
                memory[kGrid + i] = kWater;
                roads[i] &= 0xe0;
            }
            Coast();
            return 10;
        case 2:
            Highlands();
            return 20;
        case 3:
            Erosion();
            return 30;
        case 4:
            Rivers();
            return 40;
        case 5:
            Forests();
            return 50;
        case 6:
            Marshes();
            return 60;
        case 7:
            Cities();
            return 70;
        case 8:
            SitesBegin();
            [[fallthrough]];
        case 9:
            // the sites, the bar moving on a tenth of the way through them each
            while (site_i < kNSites) {
                int now = site_i * 10 / kNSites;
                if (now != tenth) {
                    tenth = now;
                    stage = 9;
                    return 70 + now;
                }
                Site(site_i++);
            }
            return 80;
        case 10:
            return 81;
        case 11:
            RoadTiles();
            return 82;
        case 12:
            RoadCrossings();
            return 83;
        case 13:
            if (auto pct = RoadsUntilShown()) {
                stage = 13;
                return *pct;
            }
            return 90;
        case 14:
            RoadShapes();
            return 92;
        case 15:
            CityList();
            if (fair)
                FairCapitals();
            else
                Capitals();
            Sides();
            return 94;
        case 16:
            CityDetails();
            return 96;
        case 17:
            sgn = Signs();
            return 98;
        case 18:
            scn[kSRandom] = 1;
            scn[kSRandom + 1] = 0;
            WriteFiles(files);
            return 100;
        default:
            return std::nullopt;
        }
    }
};

std::array<int, 4> Settle(std::array<int, 4> sliders, Rng& rng) {
    for (int& val : sliders) {
        if (val == kRandomSlider) val = rng.Dice(1, 7, -1);
    }
    return sliders;
}

namespace {
/** @brief Loads `RANDOM.DAT` and adds the chosen slider offsets to the parameters. */
std::vector<uint8_t> Params(const std::string& data_dir, const std::array<int, 4>& sliders) {
    auto bytes = ReadFile(data_dir + "/RANDOM/RANDOM.DAT");
    if (!bytes) throw std::runtime_error("Random data not found");
    std::vector<uint8_t> memory(bytes->begin(), bytes->end());
    if (memory.size() < static_cast<size_t>(kDatSize)) memory.resize(kDatSize, 0);
    auto add = [&](int offset, int table, int idx) {
        SetI16(memory, offset, GetI16(memory, offset) + GetI16(memory, table + 2 * idx));
    };
    int water = sliders[0], hills = sliders[1], cities = sliders[2], forest = sliders[3];
    add(kPCities, kSliderCities, cities);
    add(kPForest, kSliderForest, forest);
    add(kPHills, kSliderHills, hills);
    add(kPMountains, kSliderHills, hills);
    add(kPWideRivers, kSliderWater, water);
    add(kPRivers, kSliderWater, water);
    return memory;
}
}  // namespace

Run::Run(const Options& opts) {
    if (!opts.rng) throw std::runtime_error("random map: no dice");
    auto sliders = Settle(opts.sliders, *opts.rng);
    gen_ = std::make_unique<Generator>();
    Generator& gen = *gen_;
    gen.memory = Params(opts.data_dir, sliders);
    // the scenario of the chosen terrain set (Erythea) is the template
    auto tmpl = ReadFile(opts.data_dir + "/ERYTHEA/ERYTHEA.SCN");
    if (!tmpl) throw std::runtime_error("cannot open: ERYTHEA.SCN");
    gen.scn.assign(tmpl->begin(), tmpl->end());
    armytype::Load(opts.data_dir + "/TERRAIN0/ARMYTYPE.DAT", gen.types);
    gen.rng = opts.rng;
    gen.allies = opts.allies;
    gen.fair = opts.fair_capitals;
    gen.modern = opts.scenario;
    gen.world = opts.world;
    gen.tiles.assign(static_cast<size_t>(kMapW) * kMapH, 0);
    gen.roads.assign(static_cast<size_t>(kMapW) * kMapH, 0);
    gen.scn[kSTerrainSet] = static_cast<uint8_t>(opts.terrain_set);
}

Run::~Run() = default;

bool Run::Step() {
    auto pct = gen_->Next(files_);
    if (!pct) return false;
    pct_ = *pct;
    return true;
}

Files GenerateNow(const Options& opts) {
    Run run(opts);
    while (run.Step()) {
    }
    return run.GetFiles();
}

void Install(const std::string& data_dir, const Files& files) {
    for (auto& [ext, bytes] : files)
        InstallFile(data_dir + "/" + kDir + "/" + kDir + "." + ext, bytes);
}

Files Installed(const std::string& data_dir) {
    Files out;
    for (const char* ext : kFiles) {
        if (auto bytes = ReadFile(data_dir + "/" + kDir + "/" + kDir + "." + ext))
            out[ext] = *bytes;
    }
    return out;
}

std::string TerrainSetName(const std::string& data_dir, int set) {
    auto bytes = ReadFile(data_dir + "/DATA/TERRAIN.DAT");
    if (!bytes || set >= U16(*bytes, 0)) return "";
    return Cstr(*bytes, 2 + 52 * set + 2, 50);
}

int TerrainSets(const std::string& data_dir) {
    auto bytes = ReadFile(data_dir + "/DATA/TERRAIN.DAT");
    return bytes ? std::max(1, U16(*bytes, 0)) : 1;
}

}  // namespace w2::randommap
