// A preview picture of a scenario map, as a PNG: the map in the game's own
// tiles, shrunk, with the roads, cities, capitals and sites drawn bold over
// it so they read at a few pixels a tile. For looking at a world before
// playing it (w2scengen writes one beside every scenario it makes); the same
// picture tools/mapvote.py draws.
#pragma once

#include <string>

#include "warlords/types.hpp"

namespace w2::preview {

/** The map as PNG bytes, `scale` pixels a tile (1-40), drawn from the tile
 *  sheets in `dataDir`/TERRAIN0. Throws if the sheets cannot be read. */
std::string png(const Map& map, const std::string& dataDir, int scale = 5);

}  // namespace w2::preview
