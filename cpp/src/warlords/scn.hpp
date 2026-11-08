/**
 * @file
 * @brief Warlords II scenario data: .SCN, .MAP, .RD, .ITM and the text files.
 *
 * See docs/formats/scenario.md, docs/formats/map.md and docs/formats/itm.md.
 * The original loads the .SCN file verbatim into memory and works in it, so
 * its byte offsets are also the names the reverse engineering notes use for
 * the scenario's fields.
 */
#pragma once

#include <memory>
#include <string>

#include "warlords/types.hpp"

namespace w2::scn {

constexpr int kTile = 40;          ///< Terrain tiles are 40x40 pixels.
constexpr int kTileMask = 0x7FFF;  ///< Bit 15 of a map entry is a flag, not part of the tile.

/** @brief Reads an .ITM file: the items the ruins hand out; nothing if it is missing. */
std::optional<std::vector<PoolItem>> LoadItemPool(const std::string& path);
/** @brief Reads a .CTY or .SPC file: each city's or site's three lines, by number. */
std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::string& path);
/** @brief Reads an .SGN file: the signposts and what they say. */
std::vector<Sign> LoadSigns(const std::string& path);

/** @brief Loads a scenario from `dir` (the scenario's folder) and its name. */
std::unique_ptr<Map> Load(const std::string& dir, const std::string& name);
/**
 * @brief Loads a scenario primarily from in-memory files, falling back to
 *        `dir/name.EXT` for any extension `files` lacks.
 * @param files File contents keyed by extension ("SCN", "MAP", ...).
 * @throws std::runtime_error if the .SCN, .MAP or .RD cannot be had.
 */
std::unique_ptr<Map> LoadFromFiles(const std::string& dir, const std::string& name,
                                   const std::map<std::string, std::string>& files);

/** @brief The tile drawn at (x, y), with the crossing flag. */
inline int TileAt(const Map& map, int x, int y) {
    return map.tiles[y * kMapW + x];
}
/** @brief The road piece at (x, y), 0 for none. */
inline int RoadAt(const Map& map, int x, int y) {
    return static_cast<uint8_t>(map.roads[y * kMapW + x]);
}
/** @brief Is (x, y) marked as a crossing between land and sea? */
inline bool IsCrossing(const Map& map, int x, int y) {
    return map.crossing[y * kMapW + x];
}
/** @brief The terrain type id (0-11) of a map tile, via the scenario's own table. */
inline int TerrainAt(const Map& map, int x, int y) {
    return map.terrain_type[TileAt(map, x, y) % 256];
}

/**
 * @brief The first of the four castle tiles a city shows.
 *
 * A city's castle is drawn from the map itself: the game rewrites its four
 * tiles whenever the city changes hands; ruins keep the colours of whoever
 * held the city.
 */
int CityTileBase(const City& city);
/** @brief Rewrites a city's four castle tiles for its current owner. */
void SetCityTiles(Map& map, const City& city);
/** @brief Rewrites every city's castle tiles. */
void RefreshCityTiles(Map& map);

}  // namespace w2::scn
