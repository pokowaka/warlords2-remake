/**
 * @file
 * @brief Encapsulated `w2::Map` scenario entity and `w2::ActiveRect` playable-area value struct.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"

#include "warlords/model/city.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/options.hpp"
#include "warlords/model/side.hpp"
#include "warlords/model/site.hpp"

namespace w2 {

/** @brief In-memory bundle of scenario payloads keyed by upper-case file extension. */
using ScenarioFiles = absl::flat_hash_map<std::string, std::vector<uint8_t>>;

/// Canonical scenario map dimensions in tiles.
constexpr int kMapW = 112, kMapH = 156;

/** @brief Inclusive tile rectangle (`x0..x1`, `y0..y1`) bounding the active playable map area. */
struct ActiveRect {
    int x0 = 0;          ///< Left column in play (inclusive).
    int y0 = 0;          ///< Top row in play (inclusive).
    int x1 = kMapW - 1;  ///< Right column in play (inclusive).
    int y1 = kMapH - 1;  ///< Bottom row in play (inclusive).

    /** @brief Exact coordinate equality across all four bounds. */
    bool operator==(const ActiveRect& other) const = default;
};

/**
 * @brief Complete scenario world state: terrain and road grids, the eight factions, fortified
 * cities, temples and ruins, magic items, guardians, signposts, lore texts, and rule options.
 *
 * Maintains three flat $O(1)$ per-tile spatial lookup tables (`city_at_`, `city_tile_`, and
 * `site_at_`) synchronized automatically whenever cities, sites, or map dimensions change, and
 * rebound automatically across copy and move operations so external callers never manage raw
 * spatial index vectors.
 *
 * Invariants:
 * - `width_ > 0`, `height_ > 0`, `0 <= active_x0_ <= active_x1_ < width_`, and
 *   `0 <= active_y0_ <= active_y1_ < height_`.
 * - When `cities_` is non-empty, `city_at_` and `city_tile_` each have size `width_ * height_`,
 *   every `City` in `cities_` has its in-bounds top-left tile indexed in `city_at_` and its
 *   in-bounds 2x2 footprint indexed in `city_tile_`, and every non-null entry points into
 *   `this->cities_`.
 * - When `sites_` is non-empty, `site_at_` has size `width_ * height_`, every in-bounds `Site`
 *   in `sites_` is indexed at `site_at_[y * width_ + x]`, and every non-null entry points into
 *   `this->sites_`.
 *
 * Thread safety: Not thread-safe without external synchronization.
 *
 * Ownership & lifetimes: Exclusively owns all `Side`, `City`, `Site`, `Item`, `Monster`, and
 * `Sign` records stored within it. Non-owning pointers returned by `SideAt`, `CityAt`, `SiteAt`,
 * `ItemById`, and `SignAt` remain valid until the corresponding collection is replaced or resized.
 */
class Map {
  public:
    /** @brief Fixed table of all eight scenario faction slots (`0..7`). */
    using SideTable = std::array<Side, 8>;
    /** @brief Contiguous owned storage of scenario cities. */
    using CityList = std::vector<City>;
    /** @brief Mutable view over `cities_` preventing external reallocation. */
    using CitySpan = absl::Span<City>;
    /** @brief Read-only view over `cities_`. */
    using ConstCitySpan = absl::Span<const City>;
    /** @brief Contiguous owned storage of scenario temples and ruins. */
    using SiteList = std::vector<Site>;
    /** @brief Mutable view over `sites_` preventing external reallocation. */
    using SiteSpan = absl::Span<Site>;
    /** @brief Read-only view over `sites_`. */
    using ConstSiteSpan = absl::Span<const Site>;
    /** @brief Contiguous owned storage of magic items and standards. */
    using ItemList = std::vector<Item>;
    /** @brief Mutable view over `items_`. */
    using ItemSpan = absl::Span<Item>;
    /** @brief Read-only view over `items_`. */
    using ConstItemSpan = absl::Span<const Item>;
    /** @brief Optional ruin guardian monster slot. */
    using OptionalMonster = std::optional<Monster>;
    /** @brief Fixed 10-slot guardian monster table indexed by `Site::guardian`. */
    using MonsterTable = std::array<OptionalMonster, 10>;
    /** @brief Scenario `.ITM` item pool entries. */
    using ItemPoolList = std::vector<PoolItem>;
    /** @brief Optional `.ITM` item pool when the scenario supplies one. */
    using OptionalItemPool = std::optional<ItemPoolList>;
    /** @brief Three-line lore text block from `.CTY` or `.SPC`. */
    using DescriptionLines = std::array<std::string, 3>;
    /** @brief Lore descriptions keyed by city or site index. */
    using DescriptionMap = std::map<int, DescriptionLines>;
    /** @brief Contiguous owned storage of map signposts. */
    using SignList = std::vector<Sign>;
    /** @brief Mutable view over `signs_`. */
    using SignSpan = absl::Span<Sign>;
    /** @brief Read-only view over `signs_`. */
    using ConstSignSpan = absl::Span<const Sign>;
    /** @brief Tile graphic byte (`0..255`) to terrain type (`0..11`) lookup table. */
    using TerrainTypeTable = std::array<int, 256>;
    /** @brief Per-army-type combat rank row (`29` entries). */
    using FightOrderRow = std::vector<int>;
    /** @brief Nine-row fight order table (`0..7` per side, `8` neutral). */
    using FightOrderTable = std::vector<FightOrderRow>;
    /** @brief Row-major terrain tile graphic words (`width_ * height_`). */
    using TileGrid = std::vector<int>;
    /** @brief Row-major road overlay bytes (`width_ * height_`). */
    using RoadGrid = std::vector<uint8_t>;
    /** @brief Row-major land/sea crossing flags (`width_ * height_`). */
    using CrossingGrid = std::vector<bool>;

    /** @brief Constructs an empty `kMapW x kMapH` scenario map. */
    Map();
    /** @brief Copies `other` and rebinds spatial lookup pointers to this instance's storage. */
    Map(const Map& other);
    /** @brief Copy-assigns `other` and rebinds spatial lookup pointers to this instance. */
    Map& operator=(const Map& other);
    /** @brief Moves `other` and maintains valid spatial lookup pointers. */
    Map(Map&& other) noexcept;
    /** @brief Move-assigns `other` and maintains valid spatial lookup pointers. */
    Map& operator=(Map&& other) noexcept;
    ~Map() = default;

    /** @brief Returns the scenario stem/name. */
    const std::string& Name() const { return name_; }
    /** @brief Sets the scenario stem/name. */
    void SetName(std::string name);

    /** @brief Returns the map width in tiles. */
    int Width() const { return width_; }
    /** @brief Returns the map height in tiles. */
    int Height() const { return height_; }
    /**
     * @brief Updates the map dimensions (`> 0`), clamps the active area, and rebuilds spatial
     * indices.
     */
    void SetDimensions(int width, int height);

    /** @brief Returns the inclusive playable area rectangle. */
    ActiveRect ActiveArea() const {
        return ActiveRect{active_x0_, active_y0_, active_x1_, active_y1_};
    }
    /** @brief Returns the left inclusive column of the active playable area. */
    int ActiveX0() const { return active_x0_; }
    /** @brief Returns the top inclusive row of the active playable area. */
    int ActiveY0() const { return active_y0_; }
    /** @brief Returns the right inclusive column of the active playable area. */
    int ActiveX1() const { return active_x1_; }
    /** @brief Returns the bottom inclusive row of the active playable area. */
    int ActiveY1() const { return active_y1_; }
    /**
     * @brief Restricts the active playable area to inclusive tile bounds `(col0..col1,
     * row0..row1)`, clamped to `[0, width_-1] x [0, height_-1]`.
     */
    void CropActiveArea(int col0, int row0, int col1, int row1);

    /** @brief True when `(x, y)` lies inside `[0, width_) x [0, height_)`. */
    bool InBounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }
    /** @brief True when `(x, y)` lies inside the cropped active playable area. */
    bool InActiveArea(int x, int y) const {
        return x >= active_x0_ && x <= active_x1_ && y >= active_y0_ && y <= active_y1_;
    }

    /** @brief Returns mutable access to all eight faction slots. */
    SideTable& Sides() { return sides_; }
    /** @brief Returns read-only access to all eight faction slots. */
    const SideTable& Sides() const { return sides_; }
    /** @brief Returns the faction at `index` (`0..7`), or `nullptr` if out of range. */
    Side* SideAt(int index);
    /** @brief Returns the faction at `index` (`0..7`), or `nullptr` if out of range (const). */
    const Side* SideAt(int index) const;
    /** @brief Returns the faction identified by `side_id`, or `nullptr` if invalid. */
    Side* SideById(SideId side_id) { return SideAt(side_id.Value()); }
    /** @brief Returns the faction identified by `side_id` (const), or `nullptr` if invalid. */
    const Side* SideById(SideId side_id) const { return SideAt(side_id.Value()); }

    /** @brief Returns a mutable element span over `cities_` (preserving container capacity). */
    CitySpan Cities() { return absl::MakeSpan(cities_); }
    /** @brief Returns a read-only span over `cities_`. */
    ConstCitySpan Cities() const { return cities_; }
    /** @brief Returns the total number of cities on the map. */
    int CityCount() const { return static_cast<int>(cities_.size()); }
    /** @brief Returns the city at `index` (`0..CityCount()-1`), or `nullptr` if out of range. */
    City* CityAt(int index);
    /** @brief Returns the city at `index` (const), or `nullptr` if out of range. */
    const City* CityAt(int index) const;
    /** @brief Returns the city identified by `city_id`, or `nullptr` if invalid. */
    City* CityById(CityId city_id) { return CityAt(city_id.Value()); }
    /** @brief Returns the city identified by `city_id` (const), or `nullptr` if invalid. */
    const City* CityById(CityId city_id) const { return CityAt(city_id.Value()); }
    /** @brief Returns the city whose top-left tile is `(x, y)`, or `nullptr`. */
    City* CityAtTopLeft(int x, int y);
    /** @brief Returns the city whose top-left tile is `(x, y)` (const), or `nullptr`. */
    const City* CityAtTopLeft(int x, int y) const;
    /** @brief Returns the city covering `(x, y)` in its 2x2 footprint, or `nullptr`. */
    City* CityAtFootprint(int x, int y);
    /** @brief Returns the city covering `(x, y)` in its 2x2 footprint (const), or `nullptr`. */
    const City* CityAtFootprint(int x, int y) const;
    /** @brief Replaces `cities_` and rebuilds `city_at_` and `city_tile_`. */
    void SetCities(CityList cities);
    /** @brief Appends `city` to `cities_`, rebuilds city spatial indices, and returns it. */
    City& AddCity(City city);
    /** @brief Shrinks `cities_` to at most `count` cities and rebuilds city spatial indices. */
    void TruncateCities(size_t count);

    /** @brief Returns a mutable element span over `sites_` (preserving container capacity). */
    SiteSpan Sites() { return absl::MakeSpan(sites_); }
    /** @brief Returns a read-only span over `sites_`. */
    ConstSiteSpan Sites() const { return sites_; }
    /** @brief Returns the total number of temples and ruins on the map. */
    int SiteCount() const { return static_cast<int>(sites_.size()); }
    /** @brief Returns the site identified by `site_id`, or `nullptr` if out of range. */
    Site* SiteById(SiteId site_id);
    /** @brief Returns the site identified by `site_id` (const), or `nullptr` if out of range. */
    const Site* SiteById(SiteId site_id) const;
    /** @brief Returns the temple or ruin standing on tile `(x, y)`, or `nullptr`. */
    Site* SiteAt(int x, int y);
    /** @brief Returns the temple or ruin standing on tile `(x, y)` (const), or `nullptr`. */
    const Site* SiteAt(int x, int y) const;
    /** @brief Replaces `sites_` and rebuilds `site_at_`. */
    void SetSites(SiteList sites);
    /** @brief Appends `site` to `sites_`, rebuilds `site_at_`, and returns it. */
    Site& AddSite(Site site);

    /** @brief Recomputes `city_at_`, `city_tile_`, and `site_at_` from `cities_` and `sites_`. */
    void RebuildSpatialIndices();

    /** @brief Returns a mutable element span over `items_`. */
    ItemSpan Items() { return absl::MakeSpan(items_); }
    /** @brief Returns a read-only span over `items_`. */
    ConstItemSpan Items() const { return items_; }
    /** @brief Returns the number of items loaded in `items_`. */
    int ItemCount() const { return static_cast<int>(items_.size()); }
    /** @brief Returns the item whose `Item::index` matches `item_id`, or `nullptr`. */
    Item* ItemById(ItemId item_id);
    /** @brief Returns the item whose `Item::index` matches `item_id` (const), or `nullptr`. */
    const Item* ItemById(ItemId item_id) const;
    /** @brief Replaces the scenario item list. */
    void SetItems(ItemList items);
    /** @brief Appends `item` to `items_` and returns a reference to the stored entry. */
    Item& AddItem(Item item);

    /** @brief Returns the optional `.ITM` item pool. */
    const OptionalItemPool& ItemPool() const { return item_pool_; }
    /** @brief Sets or clears the optional `.ITM` item pool. */
    void SetItemPool(OptionalItemPool pool);

    /** @brief Returns the 10-slot guardian monster table. */
    const MonsterTable& Monsters() const { return monsters_; }
    /** @brief Returns a pointer to the guardian monster in slot `index` (`0..9`), or `nullptr`. */
    const Monster* MonsterAt(int index) const;
    /** @brief Sets or clears the guardian monster in slot `index` (`0..9`). */
    void SetMonster(int index, OptionalMonster monster);

    /** @brief Returns read-only scenario game options. */
    const Options& GameOptions() const { return options_; }
    /** @brief Returns mutable scenario game options. */
    Options& MutableOptions() { return options_; }
    /** @brief Replaces scenario game options. */
    void SetOptions(const Options& options) { options_ = options; }

    /** @brief Returns the 256-entry tile-graphic-to-terrain-type lookup table. */
    const TerrainTypeTable& TerrainTypes() const { return terrain_type_; }
    /** @brief Maps `tile_byte` (`0..255`, masked with `0xff`) to its terrain type id (`0..11`). */
    int TerrainTypeOf(int tile_byte) const {
        return terrain_type_[static_cast<size_t>(tile_byte & 0xff)];
    }
    /** @brief Sets the terrain type id for `tile_byte` (`0..255`). */
    void SetTerrainType(int tile_byte, int terrain_id);
    /** @brief Replaces the entire 256-entry terrain lookup table. */
    void SetTerrainTypes(const TerrainTypeTable& table) { terrain_type_ = table; }

    /** @brief Returns read-only access to the 9-row fight order table. */
    const FightOrderTable& FightOrder() const { return fight_order_; }
    /** @brief Returns mutable access to the 9-row fight order table. */
    FightOrderTable& MutableFightOrder() { return fight_order_; }
    /** @brief Replaces the fight order table. */
    void SetFightOrder(FightOrderTable fight_order);

    /** @brief Returns the scenario's maximum stack combat modifier cap. */
    int CombatCap() const { return combat_cap_; }
    /** @brief Sets the scenario's maximum stack combat modifier cap. */
    void SetCombatCap(int cap) { combat_cap_ = cap; }

    /** @brief Returns read-only access to the flat tile word grid. */
    const TileGrid& Tiles() const { return tiles_; }
    /** @brief Returns the terrain tile word at `(x, y)`, or `0` if out of bounds/unset. */
    int TileWord(int x, int y) const;
    /** @brief Writes `word` at `(x, y)` (lazily sizing `tiles_` to `width_ * height_`). */
    void SetTileWord(int x, int y, int word);
    /** @brief Replaces the flat tile word grid. */
    void SetTiles(TileGrid tiles);

    /** @brief Returns read-only access to the flat road byte grid. */
    const RoadGrid& Roads() const { return roads_; }
    /** @brief Returns the road overlay byte at `(x, y)`, or `0` if out of bounds/unset. */
    uint8_t RoadByte(int x, int y) const;
    /** @brief Writes `road` at `(x, y)` (lazily sizing `roads_` to `width_ * height_`). */
    void SetRoadByte(int x, int y, uint8_t road);
    /** @brief Replaces the flat road byte grid. */
    void SetRoads(RoadGrid roads);

    /** @brief Returns read-only access to the flat crossing flag grid. */
    const CrossingGrid& Crossings() const { return crossing_; }
    /** @brief True when tile `(x, y)` is marked as a land/sea crossing. */
    bool IsCrossing(int x, int y) const;
    /** @brief Sets the land/sea crossing flag at `(x, y)` (lazily sizing `crossing_`). */
    void SetCrossing(int x, int y, bool is_crossing);
    /** @brief Replaces the flat crossing flag grid. */
    void SetCrossings(CrossingGrid crossings);

    /** @brief Returns the in-memory generated scenario payload bundle (empty for disk scenarios).
     */
    const ScenarioFiles& SourceFiles() const { return source_files_; }
    /** @brief Stores the in-memory scenario payload bundle. */
    void SetSourceFiles(ScenarioFiles files);

    /** @brief Returns all `.CTY` city lore descriptions keyed by city index. */
    const DescriptionMap& CityDescriptions() const { return city_text_; }
    /** @brief Returns the 3-line `.CTY` description for `city_index`, or `nullptr` if absent. */
    const DescriptionLines* CityDescription(int city_index) const;
    /** @brief Replaces `.CTY` city lore descriptions. */
    void SetCityDescriptions(DescriptionMap descriptions);

    /** @brief Returns all `.SPC` site lore descriptions keyed by site index. */
    const DescriptionMap& SiteDescriptions() const { return site_text_; }
    /** @brief Returns the 3-line `.SPC` description for `site_index`, or `nullptr` if absent. */
    const DescriptionLines* SiteDescription(int site_index) const;
    /** @brief Replaces `.SPC` site lore descriptions. */
    void SetSiteDescriptions(DescriptionMap descriptions);

    /** @brief Returns a mutable element span over `signs_`. */
    SignSpan Signs() { return absl::MakeSpan(signs_); }
    /** @brief Returns a read-only span over `signs_`. */
    ConstSignSpan Signs() const { return signs_; }
    /** @brief Returns the signpost at `(x, y)`, or `nullptr` if none stands there. */
    Sign* SignAt(int x, int y);
    /** @brief Returns the signpost at `(x, y)` (const), or `nullptr` if none stands there. */
    const Sign* SignAt(int x, int y) const;
    /** @brief Replaces the signpost list. */
    void SetSigns(SignList signs);
    /** @brief Appends `sign` to `signs_` and returns a reference to the stored entry. */
    Sign& AddSign(Sign sign);

  private:
    using CityIndexGrid = std::vector<City*>;
    using SiteIndexGrid = std::vector<Site*>;

    void RebuildCityIndices();
    void RebuildSiteIndex();

    std::string name_;                 ///< Scenario stem/name.
    SideTable sides_{};                ///< The eight scenario factions (`0..7`).
    CityList cities_;                  ///< All cities in scenario index order.
    CityIndexGrid city_at_;            ///< Per-tile pointer to city whose top-left is `(x, y)`.
    CityIndexGrid city_tile_;          ///< Per-tile pointer to city covering `(x, y)` in its 2x2.
    SiteList sites_;                   ///< All temples and ruins in scenario index order.
    SiteIndexGrid site_at_;            ///< Per-tile pointer to temple or ruin on `(x, y)`.
    ItemList items_;                   ///< Magic items and standards.
    MonsterTable monsters_{};          ///< Ruin guardian definitions (`0..9`).
    CrossingGrid crossing_;            ///< Per-tile land/sea crossing flag.
    OptionalItemPool item_pool_;       ///< Optional `.ITM` item pool.
    DescriptionMap city_text_;         ///< `.CTY` city descriptions keyed by city index.
    DescriptionMap site_text_;         ///< `.SPC` site descriptions keyed by site index.
    SignList signs_;                   ///< Map signposts.
    Options options_;                  ///< Scenario rule options.
    TerrainTypeTable terrain_type_{};  ///< Tile graphic (`0..255`) -> terrain type id (`0..11`).
    FightOrderTable fight_order_;      ///< 9 rows of 29 combat ordering ranks (`8` = neutral).
    int combat_cap_ = 0;               ///< Maximum stack combat bonus cap.
    TileGrid tiles_;                   ///< Row-major map tile words (`width_ * height_`).
    RoadGrid roads_;                   ///< Row-major road overlay bytes (`width_ * height_`).
    ScenarioFiles source_files_;       ///< In-memory scenario files for generated worlds.
    int width_ = kMapW;                ///< Map width in tiles.
    int height_ = kMapH;               ///< Map height in tiles.
    int active_x0_ = 0;                ///< Left inclusive column in play.
    int active_y0_ = 0;                ///< Top inclusive row in play.
    int active_x1_ = kMapW - 1;        ///< Right inclusive column in play.
    int active_y1_ = kMapH - 1;        ///< Bottom inclusive row in play.
};

}  // namespace w2
