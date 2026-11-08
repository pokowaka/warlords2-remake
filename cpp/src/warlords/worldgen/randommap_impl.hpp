/**
 * @file
 * @brief Internal Generator class and helpers for the Warlords II random map and scenario
 * generator.
 */
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"
#include "warlords/travel.hpp"
#include "warlords/types.hpp"

namespace w2::randommap {

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
inline constexpr const char* kCompass[8] = {"north", "northeast", "east", "southeast",
                                            "south", "southwest", "west", "northwest"};
// the twelve tiles round a city's 2x2 footprint, where its roads start
inline constexpr int kRoundCity[12][2] = {{-1, -1}, {0, -1}, {1, -1}, {2, -1}, {2, 0},  {2, 1},
                                          {2, 2},   {1, 2},  {0, 2},  {-1, 2}, {-1, 1}, {-1, 0}};
// the road builder's terrain costs, pseudo-player 14
inline constexpr int kRoadCost[12] = {1, 1, 3, 3, 4, 5, 7, 2, 5, 2, 1, 7};
inline constexpr int kFootprint[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
constexpr int kSafety = 200000;  // a loop the original might never leave

/** @brief A tile position or step vector on the map. */
struct Point {
    int x = 0;  ///< Column.
    int y = 0;  ///< Row.
};

/** @brief Chebyshev distance between two tiles. */
inline int Reach(int from_x, int from_y, int to_x, int to_y) {
    return std::max(std::abs(from_x - to_x), std::abs(from_y - to_y));
}

/** @brief Euclidean distance between two tiles, rounded down. */
inline int Distance(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x, dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

/**
 * @brief The step from one tile towards another, one of 8 compass directions
 *        by the slope, cut at tan 22.5 and tan 67.5.
 */
inline Point Step(int from_x, int from_y, int to_x, int to_y) {
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
inline int Compass(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x) return from_y < to_y ? 4 : 0;
    if (from_y == to_y) return from_x < to_x ? 2 : 6;
    if (to_x < from_x && to_y < from_y) return 7;
    if (to_x < from_x && from_y < to_y) return 5;
    if (from_x < to_x && to_y < from_y) return 1;
    return 3;
}

/** @brief The checkerboard parity: 1 where x + y is even. */
inline int Parity(int x, int y) {
    int sum = x + y;
    return sum == 0 || sum / 2 != (sum - 1) / 2 ? 1 : 0;
}

/** @brief Clamps a point to the map bounds. */
inline void Clamp(Point& point) {
    if (point.x < 1) point.x = 0;
    if (point.y < 1) point.y = 0;
    if (point.x > kMapW - 1) point.x = kMapW - 1;
    if (point.y > kMapH - 1) point.y = kMapH - 1;
}

/** @brief Converts an unsigned byte (0..255) to signed (-128..127). */
inline int I8(int byte_val) {
    return byte_val > 127 ? byte_val - 256 : byte_val;
}

/** @brief Reads a signed 16-bit little-endian integer from `table` at `offset`. */
inline int GetI16(const std::vector<uint8_t>& table, int offset) {
    return static_cast<int16_t>(table[offset] | (table[offset + 1] << 8));
}

/** @brief Writes a signed 16-bit little-endian integer into `table` at `offset`. */
inline void SetI16(std::vector<uint8_t>& table, int offset, int value) {
    table[offset] = static_cast<uint8_t>(value & 0xff);
    table[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

/** @brief Reads a null-terminated string of up to `max_len` bytes from `table` at `offset`. */
inline std::string CstrOf(const std::vector<uint8_t>& table, int offset, int max_len) {
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

    /** @brief Calls `func(x, y)` for every tile whose terrain equals `terrain`. */
    template <class Func>
    void Each(int terrain, Func func) {
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++)
                if (At(x, y) == terrain) func(x, y);
    }

    /**
     * @brief Determines where edge `edge_idx`'s stretch of coast begins and
     *        ends: the map's corners drawn in 10 along the diagonal, then jittered.
     */
    void CoastEnds(int edge_idx);
    /**
     * @brief Swaps an edge's end with the next one's start where that keeps
     *        the outline turning around the middle.
     */
    void CoastOrder();
    /** @brief True if `point` lies on the outer border of the map. */
    static bool OnEdge(Point point);
    /**
     * @brief Places intermediate points along edge `edge_idx`, each a step on
     *        from the last towards the end and jittered by up to half a step.
     */
    void CoastPoints(int edge_idx);
    /** @brief Turns a step vector by -1 to +3 eighths of a circle. */
    Point Turn(Point dir);
    /** @brief Draws a wandering line of plain from `from` to `to_pt`. */
    void CoastLine(Point from, Point to_pt);
    /** @brief Which edge (0..3) a point lies on, or -1 for none. */
    static int EdgeOf(Point point);
    /** @brief Draws a straight run of plain from `from` to `to_pt`. */
    void Straight(Point from, Point to_pt);
    /**
     * @brief Joins two points on the map's edge along the border, around a
     *        corner if needed; across the map they wander like any coastline.
     */
    void CoastAlongEdge(Point from, Point to_pt);
    /** @brief Connects two coastline control points `from` and `to_pt`. */
    void CoastSegment(Point from, Point to_pt);
    /** @brief Joins every point of the coastline outline to the next. */
    void CoastJoin();
    /** @brief True if sea water may flood into (x, y). */
    bool Floods(int x, int y) const;
    /**
     * @brief Floods sea water in from the edges up to the coastline outline;
     *        everything unreached becomes plain.
     */
    void CoastFill();
    /** @brief Marks water tiles adjacent to land as shore. */
    void Shores();
    /** @brief Generates the landmass, coastline, and optional sea channels. */
    void Coast();
    /** @brief Byte offset of highland node `node_idx` in `memory`. */
    int Node(int node_idx) const { return kNodes + 14 * node_idx; }
    /** @brief Places `count` seed nodes of `terrain` (`kind` 1=hill, 2=mountain) on random plain
     * tiles. */
    void Seeds(int count, int terrain, int kind);
    /**
     * @brief Links each highland node to its nearest neighbours: a hill to
     *        0..1, a mountain to 0..3.
     */
    void Link();
    /**
     * @brief Draws a ridge from `from` to `to_pt`: `width` 2 adds a tile to
     *        either side, 3 adds one two tiles out. Only plain and hills give way.
     */
    void Ridge(Point from, Point to_pt, int width, int terrain);
    /** @brief Gives a node with no links a short ridge of its own, up and to the left. */
    void Blob(Point start_pt, int terrain);
    /** @brief Draws ridges along all highland node links. */
    void Ridges();
    /**
     * @brief Tidies mountains: plain hemmed in on all four sides joins them,
     *        mountains on the shore come down, and diagonal runs are thickened.
     */
    void MountainTidy();
    /** @brief Rings mountains with foothills. */
    void Foothills();
    /** @brief Generates mountains, hills, ridges, and foothills. */
    void Highlands();
    /**
     * @brief Walks between two random tiles: erosion wears mountains to hills,
     *        while a pass cuts mountains and hills to plain, five tiles across.
     */
    void Wear(bool pass);
    /**
     * @brief Converts tiles of `terrain` with at least three orthogonal
     *        neighbours of `surrounding` into `surrounding`.
     */
    void Surrounded(int terrain, int surrounding);
    /** @brief Applies erosion and mountain passes. */
    void Erosion();
    /**
     * @brief Carves a river or east-west sea channel across the map.
     */
    void River(bool wide, bool stops, bool channel);
    /** @brief True if `terrain` is shore or open water. */
    static bool Wet(int terrain) { return terrain == kShore || terrain == kWater; }
    /**
     * @brief Tidies water and shores: land surrounded on three sides by water
     *        submerges, shore far from land becomes open water, and diagonal
     *        water runs receive an adjacent shore tile.
     */
    void WaterTidy();
    /** @brief Generates narrow and wide rivers and tidies water and shores. */
    void Rivers();
    /** @brief Increments the count of forest tiles placed. */
    void AddForest() { W16(kForestDone, R16(kForestDone) + 1); }
    /**
     * @brief Grows a wood around a random plain tile: eight arms with two side
     *        shoots off each step that shorten along the arm.
     */
    void Wood();
    /**
     * @brief Tidies forests: plain with forest on three sides becomes forest,
     *        and diagonal runs of forest get an adjacent forest tile.
     */
    void ForestTidy();
    /** @brief Grows woods until they cover land / 100 x the Forest parameter. */
    void Forests();
    /** @brief Scatters 1d5+3 tiles of marsh around (x, y). */
    void MarshAround(int x, int y);
    /** @brief Places a marsh cluster, near the shore if one of four tries finds it. */
    void Marsh();
    /** @brief Places 1d3 marsh clusters. */
    void Marshes();
    /**
     * @brief Places a city's 2x2 footprint clear of water, hills, and other
     *        cities by a tile; on the coast if one of four tries finds it.
     */
    void PlaceCity();
    /** @brief Places all cities on the terrain grid. */
    void Cities();
    /**
     * @brief Finds a spot for a site, cycling through the map's 16 cells in turn.
     */
    Point SiteSpot();
    /** @brief Initialises the 40 site records as unplaced. */
    void SitesBegin();
    /** @brief Places site `site_idx` at a chosen spot and generates its name and text. */
    void Site(int site_idx) { SiteAt(site_idx, SiteSpot()); }
    /** @brief Writes site `site_idx` at `spot` with its generated name and description. */
    void SiteAt(int site_idx, Point spot);
    /**
     * @brief Assigns initial tile indices for every terrain cell, castle tiles
     *        for cities, and temple tiles for the first eight sites.
     */
    void ToTiles();
    /**
     * @brief Builds an 8-bit neighbour mask (bit i for neighbour i), counting
     *        the map's outer edge as matching.
     */
    int Mask(int x, int y, const std::function<bool(int)>& test) const;
    /** @brief Looks up the tile variant for neighbour mask `mask` (-1 if none). */
    int Shape(int mask) const { return I8(memory[kShape + mask]); }
    /**
     * @brief Selects transition tile variants that join each terrain type to
     *        its neighbours, replacing unrepresentable shapes with fallback terrain.
     */
    void Shapes();
    /** @brief True if (x, y) is firm ground a bridge may land on. */
    bool Firm(int x, int y) const;
    /** @brief Computes the distance from each spot in `list` to its nearest city. */
    void NearestCities(SpotList& list, int count);
    /**
     * @brief Picks the highest-scoring unchosen spot in `list`, favouring
     *        spots near cities and (if `apart`) keeping spots at least 10 apart.
     */
    int PickSpot(SpotList& list, int count, bool apart);
    /** @brief Places bridges across two-tile rivers. */
    void Bridges(SpotList& list);
    /**
     * @brief Marks up to ten sea crossings on shore tiles that face three
     *        tiles of open water and lie at least 15 tiles from other candidates.
     */
    void Crossings(SpotList& list);
    /** @brief True if city `city_idx` has already been connected by road. */
    bool IsServed(int city_idx) const;
    /** @brief Marks city `city_idx` as connected by road. */
    void Serve(int city_idx);
    /**
     * @brief Picks a pair of unserved cities 30..70 tiles apart to connect by road.
     */
    std::optional<std::pair<int, int>> RoadPair();
    /** @brief Picks a firm tile adjacent to city `city_idx`'s footprint to start a road. */
    std::optional<Point> RoadEnd(int city_idx);
    /**
     * @brief Lays a road along the pathfinder route from `from` to `to_pt`,
     *        marking cities within 10 tiles every six steps as served.
     */
    void LayRoad(Point from, Point to_pt);
    /** @brief Converts terrain to tiles, applies transition shapes twice, and clears road/crossing
     * bits. */
    void RoadTiles();
    /** @brief Places bridges and sea crossings and resets road service tracking. */
    void RoadCrossings();
    /**
     * @brief Connects cities by road a pair at a time, returning a progress
     *        percentage when the bar advances or `std::nullopt` when finished.
     */
    std::optional<int> RoadsUntilShown();
    /**
     * @brief Assigns road piece shapes from neighbouring roads and bridges,
     *        and randomly swaps a few straight pieces to their alternate look.
     */
    void RoadShapes();
    /** @brief Reads the cities back off the tile map in row order. */
    void CityList();
    /**
     * @brief Picks a random city in one of the map's eight regions (4x2);
     *        after 202 tries, returns whatever the last draw was.
     */
    int CityIn(int region);
    /** @brief Byte offset of side `side_idx`'s record in `scn`. */
    static int SideRec(int side_idx) { return kSSideRec + 20 * side_idx; }
    /**
     * @brief Assigns each side a capital in its own region, at least 12 tiles
     *        from the map edge and 20 from other capitals along both axes.
     */
    void Capitals();
    /** @brief Marks each capital's owner byte and paints its castle in the side's colours. */
    void Crown();
    /**
     * @brief Chooses the eight capitals by local search over travel-cost
     *        fairness (Options::fair_capitals).
     */
    void FairCapitals();
    /** @brief Assigns each side one of five names and 3d50+20 starting gold. */
    void Sides();
    /** @brief Inspects which terrain features touch city `city_idx`'s 2x2 footprint. */
    Surroundings SurroundingsOf(int city_idx) const;
    /**
     * @brief Computes a city's value (0..9): 9 for a capital, otherwise
     *        1d4-1 +4 by shore, +2 by road, -1 by forest, -2 by marsh.
     */
    int Value(int city_idx, const Surroundings& surr);
    /** @brief Generates a two- or three-syllable name for city `city_idx`. */
    std::string CityName(int city_idx, const Surroundings& surr);
    /** @brief True if any city before `city_idx` already has `name`. */
    bool NameTaken(int city_idx, const std::string& name) const;
    /**
     * @brief Generates city `city_idx`'s name, income, and three-line `.CTY`
     *        description.
     */
    void CityText(int city_idx, int val, const Surroundings& surr);
    /** @brief True if army type `type_id` has the flying flag (ARMYTYPE +54). */
    bool Flies(int type_id) const;
    /** @brief True if army type `type_id` is magical / an ally (ARMYTYPE +48). */
    bool Magical(int type_id) const;
    /** @brief Populates production slot `slot_idx` of city `city_idx` with army type `type_id`. */
    void SetSlot(int city_idx, int slot_idx, int type_id);
    /** @brief Chooses the up to four army production slots for city `city_idx`. */
    void Production(int city_idx, int val, const Surroundings& surr);
    /** @brief Picks a random magical (ally) army type id. */
    int MagicalType();
    /** @brief Adds a magical army type to 2d3 cities when allies are enabled. */
    void AllyProduction();
    /**
     * @brief Adds a strong non-magical, non-navy army type (strength >= 5) to
     *        each capital's last empty production slot.
     */
    void CapitalArmies();
    /** @brief Populates names, descriptions, and army production for all cities. */
    void CityDetails();
    /**
     * @brief Places 41..70 signposts on open plain tiles off the roads, most
     *        pointing to the nearest city and a few bearing fixed messages.
     */
    std::string Signs();
    /** @brief Packs the generated scenario, tile map, roads, signposts, and texts into `files`. */
    void WriteFiles(Files& files);
    /** @brief Returns a copy of the 112x156 terrain grid. */
    std::vector<uint8_t> Grid() const;
    /** @brief Finds all two-tile river stretches that can be bridged. */
    std::vector<scengen::Span> Spans() const;
    /** @brief Lays the modern scenario generator's planned roads, bridges, and crossings. */
    void LayPlannedRoads();
    /**
     * @brief Ensures every landmass containing a city or site has at least one
     *        port city or sea crossing.
     */
    void Landings();
    /**
     * @brief Assigns road pieces for the modern scenario generator from the 4
     *        cardinal neighbours and extends road ends into adjacent cities.
     */
    void ModernRoadShapes();
    /** @brief Places signposts along roads for the modern scenario generator. */
    std::string ModernSigns();
    /** @brief Runs one stage of the modern scenario generator, returning progress %. */
    std::optional<int> NextModern(Files& files);
    /** @brief Runs one stage of map generation, returning progress % or `std::nullopt` when done.
     */
    std::optional<int> Next(Files& files);
};

}  // namespace w2::randommap
