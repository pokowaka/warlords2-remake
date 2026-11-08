/**
 * @file
 * @brief Warlords II .PAL reader.
 *
 * A .PAL is plain ASCII, 16 lines of "RR GG BB", the components
 * PERCENTAGES (0-99), not VGA 0-63. See docs/formats/pck.md.
 */
#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include "absl/status/statusor.h"

namespace w2::pal {

/** @brief One colour as {red, green, blue}, each 0..1. */
using Colour = std::array<double, 3>;
/** @brief The 16 colours of a palette, indexed by colour number from 0. */
using Palette = std::vector<Colour>;

/**
 * @brief Reads a .PAL file from disk.
 * @param path Path to the `.PAL` file.
 * @return The palette as 16 `{r, g, b}` entries in `0..1`, or an actionable error status
 *         when the file is missing or does not hold 16 RGB triplets.
 */
absl::StatusOr<Palette> Load(const std::filesystem::path& path);

}  // namespace w2::pal
