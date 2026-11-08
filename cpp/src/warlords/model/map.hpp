/**
 * @file
 * @brief Map domain model struct: the terrain grid, cities, sites, items, signs, and options.
 */
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "warlords/model/city.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/options.hpp"
#include "warlords/model/side.hpp"
#include "warlords/model/site.hpp"

namespace w2 {

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
    std::string roads;                                    ///< The road layer.
    int width = kMapW;                                    ///< Width in tiles.
    int height = kMapH;                                   ///< Height in tiles.
    int active_x0 = 0;          ///< Left column of the part of the map in play.
    int active_y0 = 0;          ///< Top row of the part in play.
    int active_x1 = kMapW - 1;  ///< Right column of the part in play.
    int active_y1 = kMapH - 1;  ///< Bottom row of the part in play.

    /** @brief The side with this index, or null. */
    Side* Side(int index) { return index >= 0 && index < 8 ? &sides[index] : nullptr; }
    /** @brief The city with this index, or null. */
    City* City(int index) {
        return index >= 0 && index < static_cast<int>(cities.size()) ? &cities[index] : nullptr;
    }
};

}  // namespace w2
