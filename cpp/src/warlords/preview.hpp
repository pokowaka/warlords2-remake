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

#include <cstdint>
#include <filesystem>
#include <vector>

#include "absl/status/statusor.h"

#include "warlords/pck.hpp"
#include "warlords/types.hpp"

namespace w2::preview {

/**
 * @brief Renders the map as PNG binary bytes, `scale` pixels a tile (1-40),
 *        drawn from the tile sheets in `data_dir/TERRAIN0`.
 * @return PNG payload bytes, or an actionable error status if the sheets or palette fail to load.
 */
absl::StatusOr<std::vector<uint8_t>> Png(const Map& map, const std::filesystem::path& data_dir,
                                         int scale = 5);

/**
 * @brief Renders the picture the start menu shows for a scenario (its
 *        `PICS/SCENARIO.PCK`, 264 x 225): the same map, shrunk to the frame's
 *        height and centred on the sea, in the game's 16 colours.
 */
absl::StatusOr<pck::Pixels> MenuPicture(const Map& map, const std::filesystem::path& data_dir);

}  // namespace w2::preview
