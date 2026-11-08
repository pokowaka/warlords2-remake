/**
 * @file
 * @brief A preview picture of a scenario map.
 *
 * The map in the game's own tiles, shrunk, with the roads, cities, capitals
 * and sites drawn bold over it so they read at a few pixels a tile. For
 * looking at a world before playing it (w2scengen writes one beside every
 * scenario it makes); the same picture tools/mapvote.py draws.
 */
#pragma once

#include <string>

#include "warlords/pck.hpp"
#include "warlords/types.hpp"

namespace w2::preview {

/**
 * @brief The map as PNG bytes, `scale` pixels a tile (1-40), drawn from the
 *        tile sheets in `data_dir`/TERRAIN0.
 * @throws std::runtime_error if the sheets cannot be read.
 */
std::string Png(const Map& map, const std::string& data_dir, int scale = 5);

/**
 * @brief The picture the start menu shows for a scenario (its
 *        PICS/SCENARIO.PCK, 264 x 225): the same map, shrunk to the frame's
 *        height and centred on the sea, in the game's 16 colours.
 */
pck::Pixels MenuPicture(const Map& map, const std::string& data_dir);

}  // namespace w2::preview
