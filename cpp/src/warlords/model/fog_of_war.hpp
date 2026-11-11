/**
 * @file
 * @brief Encapsulated per-side fog-of-war exploration state (`w2::FogOfWar`).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"

#include "warlords/model/ids.hpp"

namespace w2 {

/**
 * @brief Per-side tile exploration masks under the Hidden Map scenario option.
 *
 * Stores lazy row-major `uint8_t` (`0` = unseen, `1` = explored) masks indexed
 * in $O(1)$ for standard faction indices `0..7` (with a fallback map for any
 * non-standard `SideId`). Unallocated masks treat every tile as unexplored
 * until revealed.
 *
 * Invariants:
 * - Every stored mask has size `map_width * map_height` for the active map,
 *   with each entry normalized to `0` or `1`.
 *
 * Not thread-safe without external synchronization; owned by `Game`.
 */
class FogOfWar {
  public:
    /** @brief Number of inline faction slots stored without hashing (`0..7`). */
    static constexpr size_t kInlineSides = 8;

    /** @brief Constructs an empty fog-of-war tracker with no allocated side masks. */
    FogOfWar() = default;

    /** @brief Drops all stored side exploration masks. */
    void Clear();

    /** @brief True when `side` has an allocated exploration mask. */
    bool HasMask(SideId side) const;

    /**
     * @brief Allocates or resets `side`'s mask of `map_width * map_height` tiles.
     * @param side Faction whose mask is initialized.
     * @param map_width Map width in tiles.
     * @param map_height Map height in tiles.
     * @param explored Initial visibility flag (`true` reveals all tiles, `false` hides all).
     */
    void ResetSide(SideId side, int map_width, int map_height, bool explored = false);

    /**
     * @brief Ensures `side` has an allocated mask of `map_width * map_height` tiles (zero-filled).
     */
    void EnsureSide(SideId side, int map_width, int map_height);

    /** @brief Read-only view of `side`'s raw mask, or an empty span if none exists. */
    absl::Span<const uint8_t> MaskFor(SideId side) const;

    /** @brief Mutable view of `side`'s raw mask, or an empty span if none exists. */
    absl::Span<uint8_t> MutableMaskFor(SideId side);

    /** @brief Replaces `side`'s mask with `mask` (used when decoding saves). */
    void SetMask(SideId side, std::vector<uint8_t> mask);

    /**
     * @brief Returns whether tile `(x, y)` is marked explored in `side`'s own mask.
     */
    bool IsExplored(SideId side, int map_width, int map_height, int x, int y) const;

    /**
     * @brief Marks tile `(x, y)` explored in `side`'s mask (allocating the mask if needed).
     * @return True when `(x, y)` transitioned from unseen to explored; false if already seen or
     *         out of bounds.
     */
    bool MarkExplored(SideId side, int map_width, int map_height, int x, int y);

    /** @brief Counts how many tiles are marked explored for `side`. */
    int ExploredCount(SideId side) const;

    /** @brief Lists all `SideId`s holding masks in ascending order for deterministic saves. */
    std::vector<SideId> SidesWithMasks() const;

    /** @brief Monotonic mutation counter incremented whenever any exploration mask changes. */
    uint64_t Version() const { return version_; }

    /** @brief Advances the mutation counter after direct `MutableMaskFor` edits (`TidyExplored`).
     */
    void BumpVersion() { ++version_; }

  private:
    using TileMask = std::vector<uint8_t>;
    using SideMaskMap = absl::flat_hash_map<SideId, TileMask>;

    std::array<TileMask, kInlineSides> side_masks_{};  ///< Direct O(1) masks for sides 0..7.
    std::array<bool, kInlineSides> has_mask_{};        ///< Active mask flags for sides 0..7.
    SideMaskMap extra_masks_;                          ///< Fallback storage for non-standard IDs.
    uint64_t version_ = 1;                             ///< Monotonic mask mutation counter.
};

}  // namespace w2
