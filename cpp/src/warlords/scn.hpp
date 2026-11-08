// Warlords II scenario data: .SCN, .MAP, .RD, .ITM
// See docs/formats/scenario.md, docs/formats/map.md and docs/formats/itm.md.
//
// The .SCN file is loaded verbatim into one segment by the original, so every
// offset here is also a live memory address in WARLORD2.EXE.
#pragma once

#include <memory>
#include <string>

#include "warlords/types.hpp"

namespace w2::scn {

constexpr int kTile = 40;          // terrain tiles are 40x40
constexpr int kTileMask = 0x7FFF;  // bit 15 of a map entry is a flag

std::optional<std::vector<PoolItem>> LoadItemPool(const std::string& path);
std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::string& path);
std::vector<Sign> LoadSigns(const std::string& path);

/** Load a scenario from `dir` (the scenario's folder) and its name. */
std::unique_ptr<Map> Load(const std::string& dir, const std::string& name);
/** Load a scenario primarily from in-memory extension->bytes `files`, falling back to
 * `dir/name.EXT`. */
std::unique_ptr<Map> LoadFromFiles(const std::string& dir, const std::string& name,
                                   const std::map<std::string, std::string>& files);

inline int TileAt(const Map& m, int x, int y) {
    return m.tiles[y * kMapW + x];
}
inline int RoadAt(const Map& m, int x, int y) {
    return static_cast<uint8_t>(m.roads[y * kMapW + x]);
}
inline bool IsCrossing(const Map& m, int x, int y) {
    return m.crossing[y * kMapW + x];
}
/** The terrain type id (0-11) of a map tile, via the scenario's own table. */
inline int TerrainAt(const Map& m, int x, int y) {
    return m.terrain_type[TileAt(m, x, y) % 256];
}

/** A city's castle is drawn from the map itself: the game rewrites its four
 *  tiles whenever the city changes hands (set_city_tiles, 6bd8:0000); ruins
 *  keep the colours of whoever held the city (city_make_ruins, 649c:016b). */
int CityTileBase(const City& city);
void SetCityTiles(Map& map, const City& city);
void RefreshCityTiles(Map& map);

}  // namespace w2::scn
