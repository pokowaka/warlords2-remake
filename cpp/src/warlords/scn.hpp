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

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "warlords/scenario_source.hpp"
#include "warlords/types.hpp"

namespace w2::scn {

constexpr int kTile = 40;          ///< Terrain tiles are 40x40 pixels.
constexpr int kTileMask = 0x7FFF;  ///< Bit 15 of a map entry is a flag, not part of the tile.

/** @brief Reads an .ITM file: the items the ruins hand out; `std::nullopt` if absent. */
std::optional<std::vector<PoolItem>> LoadItemPool(const std::filesystem::path& path);
/** @brief Reads a .CTY or .SPC file: each city's or site's three lines, by number. */
std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::filesystem::path& path);
/** @brief Reads an .SGN file: the signposts and what they say. */
std::vector<Sign> LoadSigns(const std::filesystem::path& path);

/**
 * @brief Decodes a scenario `Map` from an explicit `IScenarioSource`.
 * @param source Provider of `.SCN`, `.MAP`, `.RD`, and optional `.ITM`/`.CTY`/`.SPC`/`.SGN` files.
 * @param name Scenario name stored on `Map::name`.
 * @return Loaded `Map`, or an actionable `absl::Status` when mandatory assets are missing/corrupt.
 */
absl::StatusOr<std::unique_ptr<Map>> LoadFromSource(const IScenarioSource& source,
                                                    std::string_view name);

/** @brief Loads a scenario from `dir` (the scenario's folder) and its stem `name`. */
absl::StatusOr<std::unique_ptr<Map>> Load(const std::filesystem::path& dir, std::string_view name);

/**
 * @brief Loads a scenario primarily from in-memory `files`, falling back to
 *        `dir/name.EXT` on disk for any extension `files` lacks (such as `.ITM`).
 * @param dir Fallback directory on disk.
 * @param name Scenario stem.
 * @param files Scenario payloads keyed by extension (`"SCN"`, `"MAP"`, ...).
 * @return Loaded `Map` retaining `files` in `Map::source_files`, or an error status.
 */
absl::StatusOr<std::unique_ptr<Map>> LoadFromFiles(const std::filesystem::path& dir,
                                                   std::string_view name,
                                                   const ScenarioFiles& files);

/** @brief The tile drawn at (x, y), with the crossing flag. */
inline int TileAt(const Map& map, int x, int y) {
    return map.tiles[y * kMapW + x];
}
/** @brief The road piece at (x, y), 0 for none. */
inline int RoadAt(const Map& map, int x, int y) {
    return map.roads[y * kMapW + x];
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
