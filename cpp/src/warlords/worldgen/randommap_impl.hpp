/**
 * @file
 * @brief Internal `Generator` state machine and terrain/binary helpers for Warlords II world
 *        generation.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"

#include "warlords/randommap.hpp"
#include "warlords/types.hpp"

namespace w2::randommap {

// Terrain type ids, as the grid and the scenario's tile table number them.
constexpr int kRoad = 0, kBridge = 1, kWater = 2, kShore = 3, kForest = 4, kHills = 5;
constexpr int kMountains = 6, kPlain = 7, kMarsh = 8, kTower = 9, kCity = 10, kSite = 11;

// RANDOM.DAT: the parameters (the sliders add to these).
constexpr int kPCities = 0x2a, kPEdgePoints = 0x2c;
constexpr int kPMountains = 0x34, kPHills = 0x36, kPWideRivers = 0x38, kPRivers = 0x3a;
constexpr int kPForest = 0x3c, kPPasses = 0x3e, kPErosion = 0x40;
constexpr int kSliderHills = 0x44, kSliderWater = 0x52, kSliderForest = 0x60, kSliderCities = 0x6e;
constexpr int kCorners = 0x7c, kEdges = 0x8c, kDiagonals = 0xac;
constexpr int kNb = 0xbc;      // The 8 neighbours, S first and anticlockwise.
constexpr int kNbRoad = 0xdc;  // The 8 again, N first and clockwise: road shapes.
// RANDOM.DAT: the generator's working space and its tables.
constexpr int kEdgeCount = 0xfc, kEdgePts = 0x104, kEdgeStart = 0x244, kEdgeEnd = 0x254;
constexpr int kShape = 0x264;      // Neighbour mask -> tile variant, -1 for none.
constexpr int kRoadShape = 0x364;  // Neighbour mask -> road id - 1, -1 for none.
constexpr int kTiles = 0x464;      // Per terrain type, 16 variants x 2 checkerboard tiles.
constexpr int kSideNames = 0x764, kSiteFormats = 0xed2;
constexpr int kSignCity = 0x1888, kSignLeagues = 0x189c, kSigns = 0x18b0;
constexpr int kSideClass = 0x1d62, kRegionClass = 0x1d72, kProduction = 0x1d82;
constexpr int kForestDone = 0x1f52, kForestWanted = 0x1f54;
constexpr int kNodes = 0x5dd6, kNodeCount = 0x611e, kGrid = 0x6120;
constexpr int kDatSize = 0xa560;

// .SCN offsets (docs/formats/scenario.md).
constexpr int kSSideRec = 387, kSRandom = 0x120, kSTerrainSet = 0x161;
constexpr int kSTerrain = 0x710, kSSiteCount = 0x80f, kSSites = 0x811, kSiteStride = 31;
constexpr int kSCityCount = 5499, kSCities = 5501, kCityStride = 65;
constexpr int kNSites = 40;

// Directions the signposts give.
inline constexpr const char* kCompass[8] = {"north", "northeast", "east", "southeast",
                                            "south", "southwest", "west", "northwest"};
// The twelve tiles round a city's 2x2 footprint, where its roads start.
inline constexpr int kRoundCity[12][2] = {{-1, -1}, {0, -1}, {1, -1}, {2, -1}, {2, 0},  {2, 1},
                                          {2, 2},   {1, 2},  {0, 2},  {-1, 2}, {-1, 1}, {-1, 0}};
// The road builder's terrain costs, pseudo-player 14.
inline constexpr int kRoadCost[12] = {1, 1, 3, 3, 4, 5, 7, 2, 5, 2, 1, 7};
inline constexpr int kFootprint[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
constexpr int kSafety = 200000;  // Iteration cap for loops the original might never leave.

/** @brief A tile position or step vector on the map. */
struct Point {
    int x = 0;  ///< Column.
    int y = 0;  ///< Row.
};

/** @brief Candidate spots for bridges or crossings. */
struct SpotList {
    int x[50]{};            ///< Column of each candidate spot.
    int y[50]{};            ///< Row of each candidate spot.
    int east_west[50]{};    ///< 1 if bridged east-west, 0 if north-south.
    int city_dist[50]{};    ///< Distance to the nearest city.
    int chosen[50]{};       ///< 1 if selected, 0 otherwise.
    int chosen_dist[50]{};  ///< Distance to the nearest already-chosen spot.
};

/** @brief Terrain types adjacent to a city's 2x2 footprint. */
struct Surroundings {
    bool shore = false;   ///< Touches shore.
    bool forest = false;  ///< Touches forest.
    bool road = false;    ///< Touches road.
    bool hills = false;   ///< Touches hills.
    bool marsh = false;   ///< Touches marsh.
};

/** @brief Converts an unsigned byte (0..255) to signed (-128..127). */
constexpr int I8(int byte_val) {
    return byte_val > 127 ? byte_val - 256 : byte_val;
}

/** @brief Chebyshev distance between two tiles. */
int Reach(int from_x, int from_y, int to_x, int to_y);

/** @brief Euclidean distance between two tiles, rounded down. */
int Distance(int from_x, int from_y, int to_x, int to_y);

/**
 * @brief Computes the unit step from (`from_x`, `from_y`) toward (`to_x`, `to_y`), quantized to
 *        one of 8 compass directions by slope thresholds at tan(22.5 deg) and tan(67.5 deg).
 */
Point Step(int from_x, int from_y, int to_x, int to_y);

/** @brief Returns the compass octant (`0..7`) from (`from_x`, `from_y`) to (`to_x`, `to_y`). */
int Compass(int from_x, int from_y, int to_x, int to_y);

/** @brief Returns checkerboard parity: 1 where `x + y` is even, 0 otherwise. */
int Parity(int x, int y);

/** @brief Clamps `point` in-place to the map bounds `[0, kMapW - 1] x [0, kMapH - 1]`. */
void Clamp(Point& point);

/** @brief Reads a signed 16-bit little-endian integer from `table` at byte `offset`. */
int GetI16(absl::Span<const uint8_t> table, int offset);

/** @brief Writes a signed 16-bit little-endian integer into `table` at byte `offset`. */
void SetI16(std::vector<uint8_t>& table, int offset, int value);

/** @brief Reads a null-terminated string of up to `max_len` bytes from `table` at `offset`. */
std::string CstrOf(absl::Span<const uint8_t> table, int offset, int max_len);

/**
 * @brief Phased world-generation state machine producing in-memory `.SCN`/`.MAP`/`.RD`/`.SGN`/
 *        `.CTY`/`.SPC` scenario payloads.
 *
 * Drives either the 18-stage classic `RANDOM.DAT` generator (matching the 1993 algorithm and the
 * Lua/JS ports bit-for-bit) or the 9-stage modern noise-and-MST scenario generator
 * (`Options::scenario`), emitting progress-bar percentages on each `Next()` call and writing the
 * completed file bundle on the final stage.
 *
 * Invariants:
 * - `rng_ != nullptr`
 * - `memory_.size() >= kDatSize` and `scn_.size() > kSTerrainSet`
 * - `tiles_.size() == kMapW * kMapH` and `roads_.size() == kMapW * kMapH`
 *
 * Thread safety: Not thread-safe; confine each `Generator` instance to a single thread.
 *
 * Ownership & lifetimes: Owns its working `RANDOM.DAT` memory image, `.SCN` buffer, tile/road
 * grids, and army-type table. Borrows `rng_` as a non-owning pointer that must remain valid
 * across all `Next()` invocations.
 */
class Generator {
  public:
    /**
     * @brief Constructs a generator initialized from loaded `RANDOM.DAT`, template `.SCN`, and
     *        army-type tables.
     * @param memory Slider-adjusted `RANDOM.DAT` image of at least `kDatSize` bytes.
     * @param scn Template `.SCN` binary buffer (`ERYTHEA.SCN`) larger than `kSTerrainSet`.
     * @param types Loaded army-type catalog (`ARMYTYPE.DAT`).
     * @param opts Caller generation options (`opts.rng` must be non-null).
     */
    explicit Generator(std::vector<uint8_t> memory, std::vector<uint8_t> scn, Types types,
                       const Options& opts);

    /**
     * @brief Advances map generation by one progress-bar stage.
     * @param files Destination file map populated when the final 100% stage runs.
     * @return Progress percentage (`0..100`), or `std::nullopt` once generation has finished.
     */
    std::optional<int> Next(Files& files);

  private:
    using CityPair = std::pair<int, int>;
    using TerrainPredicate = std::function<bool(int)>;

    // --- memory & grid primitives (randommap_grid.cpp) -----------------------

    int R16(int offset) const { return GetI16(memory_, offset); }
    void W16(int offset, int value) { SetI16(memory_, offset, value); }
    std::string Str(int offset, int max_len) const;
    int S16(int offset) const { return GetI16(scn_, offset); }
    void Ws16(int offset, int value) { SetI16(scn_, offset, value); }
    void Wstr(int offset, std::string_view text);

    int At(int x, int y) const;
    void Put(int x, int y, int terrain);
    int Nx(int dir) const { return R16(kNb + 4 * dir); }
    int Ny(int dir) const { return R16(kNb + 4 * dir + 2); }
    int NbIndex(int dx, int dy) const;

    int Tile(int x, int y) const;
    void SetTile(int x, int y, int tile_byte);
    int Road(int x, int y) const;
    void SetRoad(int x, int y, int piece);
    int TerrainOf(int tile_byte) const { return scn_[kSTerrain + tile_byte]; }
    int TileFor(int terrain, int variant, int x, int y) const;

    int CityCount() const { return S16(kSCityCount); }
    void SetCityCount(int count) { Ws16(kSCityCount, count); }
    static constexpr int Crec(int city_idx) { return kSCities + kCityStride * city_idx; }
    int Cx(int city_idx) const { return S16(Crec(city_idx)); }
    int Cy(int city_idx) const { return S16(Crec(city_idx) + 2); }

    int Dice(int count, int sides, int bonus = 0);
    bool Near(int x, int y, int terrain) const;
    bool NearPlain(int x, int y) const { return Near(x, y, kPlain); }
    void Jitter(Point& point, int range, bool to_edge);

    Point Pt(int offset) const { return Point{R16(offset), R16(offset + 2)}; }
    void SetPt(int offset, Point point);

    template <class Func>
    void Each(int terrain, Func func) {
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++)
                if (At(x, y) == terrain) func(x, y);
    }

    // --- coastline, highlands, and erosion (randommap_terrain.cpp) -----------

    void CoastEnds(int edge_idx);
    void CoastOrder();
    static bool OnEdge(Point point);
    void CoastPoints(int edge_idx);
    Point Turn(Point dir);
    void CoastLine(Point from, Point to_pt);
    static int EdgeOf(Point point);
    void Straight(Point from, Point to_pt);
    void CoastAlongEdge(Point from, Point to_pt);
    void CoastSegment(Point from, Point to_pt);
    void CoastJoin();
    bool Floods(int x, int y) const;
    void CoastFill();
    void Shores();
    void Coast();

    static constexpr int Node(int node_idx) { return kNodes + 14 * node_idx; }
    void Seeds(int count, int terrain, int kind);
    void Link();
    void Ridge(Point from, Point to_pt, int width, int terrain);
    void Blob(Point start_pt, int terrain);
    void Ridges();
    void MountainTidy();
    void Foothills();
    void Highlands();

    void Wear(bool pass);
    void Surrounded(int terrain, int surrounding);
    void Erosion();

    // --- rivers, forests, and marshes (randommap_biomes.cpp) -----------------

    void River(bool wide, bool stops, bool channel);
    static constexpr bool Wet(int terrain) { return terrain == kShore || terrain == kWater; }
    void WaterTidy();
    void Rivers();

    void AddForest() { W16(kForestDone, R16(kForestDone) + 1); }
    void Wood();
    void ForestTidy();
    void Forests();

    void MarshAround(int x, int y);
    void Marsh();
    void Marshes();

    // --- cities, sites, tiles, and roads (randommap_features.cpp) ------------

    void PlaceCity();
    void Cities();
    Point SiteSpot();
    void SitesBegin();
    void Site(int site_idx) { SiteAt(site_idx, SiteSpot()); }
    void SiteAt(int site_idx, Point spot);

    void ToTiles();
    int Mask(int x, int y, const TerrainPredicate& test) const;
    int Shape(int mask) const { return I8(memory_[kShape + mask]); }
    void Shapes();

    bool Firm(int x, int y) const;
    void NearestCities(SpotList& list, int count);
    int PickSpot(SpotList& list, int count, bool apart);
    void Bridges(SpotList& list);
    void Crossings(SpotList& list);
    bool IsServed(int city_idx) const;
    void Serve(int city_idx);
    std::optional<CityPair> RoadPair();
    std::optional<Point> RoadEnd(int city_idx);
    void LayRoad(Point from, Point to_pt);
    void RoadTiles();
    void RoadCrossings();
    std::optional<int> RoadsUntilShown();
    void RoadShapes();

    // --- capitals, city details, signs, and output (randommap_populate.cpp) --

    void CityList();
    int CityIn(int region);
    static constexpr int SideRec(int side_idx) { return kSSideRec + 20 * side_idx; }
    void Capitals();
    void Crown();
    void FairCapitals();
    void Sides();

    Surroundings SurroundingsOf(int city_idx) const;
    int Value(int city_idx, const Surroundings& surr);
    std::string CityName(int city_idx, const Surroundings& surr);
    bool NameTaken(int city_idx, std::string_view name) const;
    void CityText(int city_idx, int val, const Surroundings& surr);
    bool Flies(int type_id) const;
    bool Magical(int type_id) const;
    void SetSlot(int city_idx, int slot_idx, int type_id);
    void Production(int city_idx, int val, const Surroundings& surr);
    int MagicalType();
    void AllyProduction();
    void CapitalArmies();
    void CityDetails();

    std::vector<uint8_t> Signs();
    void WriteFiles(Files& files);

    // --- modern scenario generator bridge (randommap.cpp) --------------------

    std::vector<uint8_t> Grid() const;
    std::vector<scengen::Span> Spans() const;
    void LayPlannedRoads();
    void Landings();
    void ModernRoadShapes();
    std::vector<uint8_t> ModernSigns();
    std::optional<int> NextModern(Files& files);

    // --- state invariants ----------------------------------------------------

    std::vector<uint8_t> memory_;      ///< `RANDOM.DAT` buffer and working terrain grid.
    std::vector<uint8_t> scn_;         ///< Scenario (`.SCN`) binary image.
    std::vector<uint16_t> tiles_;      ///< Map tile words (`.MAP`).
    std::vector<uint8_t> roads_;       ///< Road overlay bytes (`.RD`).
    std::vector<uint8_t> sgn_;         ///< Signpost binary data (`.SGN`).
    std::vector<uint8_t> served_;      ///< Per-city flag: connected by road.
    std::string cty_;                  ///< City description text (`.CTY`).
    std::string spc_;                  ///< Site description text (`.SPC`).
    Types types_;                      ///< Loaded army type definitions.
    scengen::Settings world_;          ///< Scenario generator settings (`Options::world`).
    scengen::World plan_;              ///< World layout built by the modern scenario generator.
    Rng* rng_ = nullptr;               ///< Non-owning pointer to the caller's dice roller.
    std::optional<CityPair> pending_;  ///< Pending city pair for road laying.
    int site_cell_ = -1;               ///< Current 4x4 grid cell for site placement.
    int stage_ = 0;                    ///< Current progress-bar stage.
    int site_i_ = 0;                   ///< Next site index to place.
    int tenth_ = 0;                    ///< Last reported tenth of site placement.
    int shown_ = 4;                    ///< Road progress steps reported so far.
    bool allies_ = false;              ///< Whether non-capital cities may produce magical allies.
    bool fair_ = false;                ///< Whether to choose fair starting capitals.
    bool modern_ = false;              ///< Whether to run the modern scenario generator.
};

}  // namespace w2::randommap
