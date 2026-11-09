/**
 * @file
 * @brief Decoded .PCK sheets as images in a palette.
 *
 * Colours are keyed out or remapped as the original blits them. See
 * docs/formats/pck.md.
 */
#pragma once

#include <filesystem>
#include <map>
#include <set>

#include "absl/status/statusor.h"

#include "platform/gfx.hpp"
#include "warlords/pal.hpp"
#include "warlords/pck.hpp"

namespace pckimage {

/// Palette index remap table (`source_index -> target_index`).
using ColourRemap = std::map<int, int>;
/// Set of palette indices rendered transparent.
using TransparentSet = std::set<int>;

/**
 * @brief An image from decoded palette indices on `renderer`.
 *
 * Each index is passed through `map` (index -> index) if it is there;
 * `keys` is the sheet's own indices to leave clear. This is how a font is
 * drawn in colours other than its own.
 */
gfx::ImageP FromPixels(gfx::Renderer& renderer, const w2::pck::Pixels& pixels,
                       const w2::pal::Palette& palette, const ColourRemap& map = {},
                       const TransparentSet& keys = {});

constexpr int kCorner = -2;  ///< Key out whatever colour the sheet's top left is.
constexpr int kNokey = -1;   ///< Key out nothing.

/** @brief Decodes a .PCK into an image; `key` is the index to make transparent, kNokey or kCorner.
 */
absl::StatusOr<gfx::ImageP> Load(gfx::Renderer& renderer, const std::filesystem::path& path,
                                 const w2::pal::Palette& palette, int key = kNokey);
/** @brief Decodes a .PCK into an image with every index in `keys` transparent. */
absl::StatusOr<gfx::ImageP> Load(gfx::Renderer& renderer, const std::filesystem::path& path,
                                 const w2::pal::Palette& palette, const TransparentSet& keys);

}  // namespace pckimage
