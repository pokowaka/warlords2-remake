/**
 * @file
 * @brief TilePos value type representing a 2D map coordinate and distance calculations.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <utility>

#include "warlords/model/army_type.hpp"

namespace w2 {

/**
 * @brief A 2D map tile coordinate `(x, y)` with value semantics and distance helpers.
 *
 * `x == kNone || y == kNone` represents an unset or off-map position (such as an
 * army in transit).
 */
struct TilePos {
    int x = kNone;  ///< Column index (`0..width-1`), or `kNone`.
    int y = kNone;  ///< Row index (`0..height-1`), or `kNone`.

    /** @brief Default constructor: initializes to `(kNone, kNone)`. */
    constexpr TilePos() = default;

    /** @brief Constructs a tile position at column `col_x`, row `row_y`. */
    constexpr TilePos(int col_x, int row_y) : x(col_x), y(row_y) {}

    /** @brief Implicitly constructs from a `std::pair<int, int>` for interoperability. */
    constexpr TilePos(std::pair<int, int> pair_pos)  // NOLINT(google-explicit-constructor)
            : x(pair_pos.first), y(pair_pos.second) {}

    /** @brief Returns whether both coordinates are valid (non-negative and not `kNone`). */
    constexpr bool IsValid() const { return x >= 0 && y >= 0; }

    /** @brief Converts to `std::pair<int, int>`. */
    constexpr std::pair<int, int> ToPair() const { return {x, y}; }

    /** @brief Chebyshev (king-move / 8-way) distance to `other`. */
    int ChebyshevDistance(TilePos other) const {
        return std::max(std::abs(x - other.x), std::abs(y - other.y));
    }

    /** @brief Manhattan (4-way) distance to `other`. */
    int ManhattanDistance(TilePos other) const {
        return std::abs(x - other.x) + std::abs(y - other.y);
    }

    /** @brief Three-way comparison in row-major order `(y, x)`. */
    constexpr auto operator<=>(const TilePos& other) const = default;

    /** @brief Abseil hash integration for spatial `TilePos` tables. */
    template <typename HashState>
    friend HashState AbslHashValue(HashState hash_state, TilePos tile_pos) {
        return HashState::combine(std::move(hash_state), tile_pos.x, tile_pos.y);
    }
};

}  // namespace w2
