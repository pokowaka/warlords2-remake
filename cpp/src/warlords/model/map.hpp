/**
 * @file
 * @brief Map domain model struct: the terrain grid, cities, sites, items, signs, and options.
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"

#include "warlords/model/city.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/options.hpp"
#include "warlords/model/side.hpp"
#include "warlords/model/site.hpp"

namespace w2 {

/** @brief In-memory bundle of scenario payloads keyed by upper-case file extension. */
using ScenarioFiles = absl::flat_hash_map<std::string, std::vector<uint8_t>>;

/// The map's full size in tiles.
constexpr int kMapW = 112, kMapH = 156;

/** @brief Everything the scenario holds: the sides, the cities, the sites, the tiles. */
struct Map {
    std::string name;                                 ///< The scenario's name.
    std::array<Side, 8> sides;                        ///< The eight sides.
    std::vector<City> cities;                         ///< Every city.
    std::vector<City*> city_at;                       ///< By tile: a city's top-left tile.
    std::vector<City*> city_tile;                     ///< By tile: every tile of its 2x2 footprint.
    std::vector<Site> sites;                          ///< Every temple and ruin.
    std::vector<Site*> site_at;                       ///< By tile.
    std::vector<Item> items;                          ///< Every item.
    std::array<std::optional<Monster>, 10> monsters;  ///< The ruins' guardians.
    std::vector<bool> crossing;                       ///< By tile: land and sea may meet here.
    std::optional<std::vector<PoolItem>> item_pool;   ///< The scenario's item pool, if any.
    std::map<int, std::array<std::string, 3>> city_text;  ///< The cities' descriptions.
    std::map<int, std::array<std::string, 3>> site_text;  ///< The sites' descriptions.
    std::vector<Sign> signs;                              ///< The signposts.
    Options options;                                      ///< The game options.
    std::array<int, 256> terrain_type{};                  ///< Terrain type by tile graphic.
    std::vector<std::vector<int>> fight_order;            ///< 9 rows of 29, row 8 neutral.
    int combat_cap = 0;                                   ///< The most a stack's bonus may add.
    std::vector<int> tiles;                               ///< The map words, row by row.
    std::vector<uint8_t> roads;                           ///< The road layer.
    ScenarioFiles source_files;  ///< In-memory scenario bundle for generated maps.
    int width = kMapW;           ///< Width in tiles.
    int height = kMapH;          ///< Height in tiles.
    int active_x0 = 0;           ///< Left column of the part of the map in play.
    int active_y0 = 0;           ///< Top row of the part in play.
    int active_x1 = kMapW - 1;   ///< Right column of the part in play.
    int active_y1 = kMapH - 1;   ///< Bottom row of the part in play.

    /** @brief The side with this index, or null. */
    w2::Side* Side(int index) { return index >= 0 && index < 8 ? &sides[index] : nullptr; }
    /** @brief The side with this index (const), or null. */
    const w2::Side* Side(int index) const {
        return index >= 0 && index < 8 ? &sides[index] : nullptr;
    }
    /** @brief The side identified by `side_id`, or null. */
    w2::Side* Side(SideId side_id) { return Side(side_id.Value()); }
    /** @brief The side identified by `side_id` (const), or null. */
    const w2::Side* Side(SideId side_id) const { return Side(side_id.Value()); }
    /** @brief The city with this index, or null. */
    w2::City* City(int index) {
        return index >= 0 && index < static_cast<int>(cities.size()) ? &cities[index] : nullptr;
    }
    /** @brief The city with this index (const), or null. */
    const w2::City* City(int index) const {
        return index >= 0 && index < static_cast<int>(cities.size()) ? &cities[index] : nullptr;
    }
    /** @brief The city identified by `city_id`, or null. */
    w2::City* City(CityId city_id) { return City(city_id.Value()); }
    /** @brief The city identified by `city_id` (const), or null. */
    const w2::City* City(CityId city_id) const { return City(city_id.Value()); }
    /** @brief The site identified by `site_id`, or null. */
    struct Site* SiteById(SiteId site_id) {
        int index = site_id.Value();
        return index >= 0 && index < static_cast<int>(sites.size()) ? &sites[index] : nullptr;
    }
    /** @brief The site identified by `site_id` (const), or null. */
    const struct Site* SiteById(SiteId site_id) const {
        int index = site_id.Value();
        return index >= 0 && index < static_cast<int>(sites.size()) ? &sites[index] : nullptr;
    }
    /** @brief The item identified by `item_id`, or null. */
    struct Item* ItemById(ItemId item_id) {
        int index = item_id.Value();
        return index >= 0 && index < static_cast<int>(items.size()) ? &items[index] : nullptr;
    }
    /** @brief The item identified by `item_id` (const), or null. */
    const struct Item* ItemById(ItemId item_id) const {
        int index = item_id.Value();
        return index >= 0 && index < static_cast<int>(items.size()) ? &items[index] : nullptr;
    }
    /** @brief True if `(x, y)` lies inside the full map dimensions `(0..width-1, 0..height-1)`. */
    bool InBounds(int x, int y) const { return x >= 0 && x < width && y >= 0 && y < height; }
    /** @brief True if `(x, y)` lies inside the cropped active playable area. */
    bool InActiveArea(int x, int y) const {
        return x >= active_x0 && x <= active_x1 && y >= active_y0 && y <= active_y1;
    }
    /** @brief Returns the city whose top-left tile is `(x, y)`, or `nullptr`. */
    w2::City* CityAtTopLeft(int x, int y) const {
        return InBounds(x, y) && !city_at.empty() ? city_at[y * width + x] : nullptr;
    }
    /** @brief Returns the city covering tile `(x, y)` in its 2x2 footprint, or `nullptr`. */
    w2::City* CityAtFootprint(int x, int y) const {
        return InBounds(x, y) && !city_tile.empty() ? city_tile[y * width + x] : nullptr;
    }
    /** @brief Returns the temple or ruin on tile `(x, y)`, or `nullptr`. */
    struct Site* SiteAt(int x, int y) const {
        return InBounds(x, y) && !site_at.empty() ? site_at[y * width + x] : nullptr;
    }
};

}  // namespace w2
