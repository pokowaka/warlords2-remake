/**
 * @file
 * @brief Warlords II .PCK image decoder and encoder.
 *
 * A .PCK file is a u16 version (1), a u16 width, a u16 height, then four
 * compressed bitplanes. Each plane is LZ77 with a signed big-endian offset:
 *
 *     cmd < 0x80   literal run of (cmd + 1) bytes
 *     cmd >= 0x80  match, 3 bytes: [cmd][mid][len]
 *                  distance = 65536 - ((cmd << 8) | mid)   -- 1..32768
 *                  length   = len + 1                      -- 1..256
 *
 * Copies are byte-at-a-time so they may overlap. Reads before the start of
 * the plane yield 0. The window resets at every plane boundary.
 *
 * Plane p supplies bit p of the 4bpp colour index; within a byte the MSB is
 * the leftmost pixel. See docs/formats/pck.md.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "absl/status/statusor.h"

namespace w2::pck {

/** @brief A decoded sheet: w * h colour indices, 0-15, row by row. */
struct Pixels {
    int w = 0;                ///< Width in pixels.
    int h = 0;                ///< Height in pixels.
    std::vector<uint8_t> px;  ///< The colour indices, w * h of them, top row first.
    /** @brief The colour index at column `x`, row `y`. */
    uint8_t At(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

/**
 * @brief Decodes a .PCK file from disk.
 *
 * Decoded sheets are cached thread-safely by upper-cased path, as game art
 * sheets are immutable assets.
 * @param path Path to the `.PCK` image file.
 * @return Shared decoded `Pixels`, or an actionable `absl::Status` when missing or malformed.
 */
absl::StatusOr<std::shared_ptr<const Pixels>> Decode(const std::filesystem::path& path);

/**
 * @brief Encodes a 4-bit paletted sheet into `.PCK` binary bytes.
 *
 * The sheet's width must be a multiple of 8 and its colours 0-15. Each plane
 * is written as literal runs, and a run of one repeated byte as a match one
 * byte back.
 * @param pixels Source 4bpp pixel sheet.
 * @return Encoded `.PCK` payload, or `absl::InvalidArgumentError` if `pixels.w % 8 != 0`.
 */
absl::StatusOr<std::vector<uint8_t>> Encode(const Pixels& pixels);

}  // namespace w2::pck
