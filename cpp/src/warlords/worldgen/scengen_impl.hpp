/**
 * @file
 * @brief Internal noise, grid connectivity, and A* and MST road-planning classes for `w2::scengen`.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "absl/types/span.h"

#include "warlords/rng.hpp"
#include "warlords/scengen.hpp"

namespace w2::scengen {

/** @brief Cardinal step offsets (N, E, S, W). */
inline constexpr int kDX4[4] = {0, 1, 0, -1};
inline constexpr int kDY4[4] = {-1, 0, 1, 0};

/** @brief Connected component of linear tile indices (`y * width + x`). */
using TileComponent = std::vector<int>;
/** @brief Predicate over a linear tile index (`y * width + x`). */
using TilePredicate = std::function<bool(int)>;
/** @brief Bonus score function over a linear tile index (`y * width + x`). */
using BonusFn = std::function<double(int)>;
/** @brief 2D tile coordinate pair `(x, y)`. */
using TileCoords = std::pair<int, int>;

/** @brief Draws a uniform double in `[0, 1)` using portable 16-bit integer division. */
double Uniform(Rng& rng);

/** @brief Linearly interpolates between `start` and `end` by `frac` using portable IEEE math.
 */
double Mix(double start, double end, double frac);

/** @brief Computes Euclidean length of (`dx`, `dy`) via `std::sqrt` (avoiding non-portable
 * hypot).
 */
double Dist(double dx, double dy);

/** @brief Returns the value at `quantile` (`0..1`) of `values` via `std::nth_element`. */
double Cut(std::vector<double> values, double quantile);

/**
 * @brief Deterministic 2D Perlin gradient noise and fractal Brownian motion sampler.
 *
 * Shuffles a 256-entry permutation table from an `Rng` at construction time using portable
 * IEEE-754 `+`, `-`, `*`, `/` arithmetic so identical seeds produce identical elevation, ridge,
 * river, and moisture fields across the C++, Lua, and JavaScript ports.
 *
 * Invariants: `perm_[i] == perm_[i & 255]` for all `0 <= i < 512`, with `perm_[0..255]` forming
 * a permutation of `0..255`.
 *
 * Thread safety: Immutable after construction; `At()` and `Fbm()` are safe for concurrent
 * reads.
 *
 * Ownership & lifetimes: Self-contained value object; does not retain a reference to `rng`
 * after construction.
 */
class Noise {
  public:
    /**
     * @brief Initializes the gradient permutation table by shuffling `0..255` with `rng`.
     * @param rng Dice sequence consumed once during construction.
     */
    explicit Noise(Rng& rng);

    /**
     * @brief Evaluates single-octave 2D Perlin gradient noise at (`x`, `y`).
     * @return Noise sample in approximately `[-1, 1]`.
     */
    double At(double x, double y) const;

    /**
     * @brief Evaluates multi-octave fractal Brownian motion at (`x`, `y`).
     * @param x Horizontal sample coordinate.
     * @param y Vertical sample coordinate.
     * @param octaves Number of octaves (`>= 1`), lacunarity `2.03`, gain `0.5`.
     * @return Normalized fBm sample in approximately `[-1, 1]`.
     */
    double Fbm(double x, double y, int octaves = 5) const;

  private:
    std::array<int, 512> perm_{};  ///< Doubled 256-entry gradient permutation table.
};

/**
 * @brief Bounds-checked 2D tile grid geometry providing connected-component and BFS distance
 *        queries.
 *
 * Operates over row-major linear tile indices `y * Width() + x` without owning tile payloads,
 * allowing multiple terrain, elevation, and connectivity passes to share the same dimensions.
 *
 * Invariants: `width_ > 0` and `height_ > 0`.
 *
 * Thread safety: Immutable value object; safe for concurrent `const` queries.
 *
 * Ownership & lifetimes: Stateless geometry descriptor holding no external references.
 */
class Grid {
  public:
    /**
     * @brief Constructs a grid descriptor of `width` columns by `height` rows.
     * @param width Grid width in tiles (`> 0`).
     * @param height Grid height in tiles (`> 0`).
     */
    Grid(int width, int height);

    /** @brief Grid width in tiles. */
    int Width() const { return width_; }
    /** @brief Grid height in tiles. */
    int Height() const { return height_; }
    /** @brief Total number of tiles (`Width() * Height()`). */
    int TileCount() const { return width_ * height_; }
    /** @brief Returns true if (`x`, `y`) lies within `[0, Width()) x [0, Height())`. */
    bool In(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }

    /**
     * @brief Partitions tiles satisfying `test` into connected components.
     * @param test Predicate returning true for tiles included in the partition.
     * @param eight `true` for 8-neighbour connectivity; `false` for 4-neighbour connectivity.
     * @return Connected components discovered in row-major scan order.
     */
    std::vector<TileComponent> Parts(const TilePredicate& test, bool eight = true) const;

    /**
     * @brief Computes 4-connected Manhattan step distances from the nearest tile satisfying
     * `from`.
     * @param from Seed predicate marking zero-distance source tiles.
     * @return Vector of size `TileCount()` holding step counts, or `-1` if no tile satisfied
     *         `from`.
     */
    std::vector<int> Distance(const TilePredicate& from) const;

  private:
    int width_;   ///< Width in tiles.
    int height_;  ///< Height in tiles.
};

/**
 * @brief Inter-city road and bridge network planner combining A* pathfinding with a Kruskal
 * MST.
 *
 * Evaluates candidate links among each city's four nearest Euclidean neighbours within 26
 * tiles, builds a minimum spanning tree over reachable links (`cost <= 34`), probabilistically
 * adds shortcut links where tree detours exceed `2.2x` direct cost, and lays roads in ascending
 * cost order so later routes merge cheaply (`0.3` per step) onto earlier roads.
 *
 * Invariants:
 * - `terrain_.size() == static_cast<size_t>(grid_.TileCount())`
 * - `road_.size() == static_cast<size_t>(grid_.TileCount())`
 * - `door_.size() == cities_.size()` and `leap_.size() ==
 * static_cast<size_t>(grid_.TileCount())`
 *
 * Thread safety: Not thread-safe; single-use per `PlanRoads()` call.
 *
 * Ownership & lifetimes: Borrows `terrain_`, `cities_`, and `spans_` spans for the duration of
 * the planner's lifetime; callers must keep the backing vectors alive until `Plan()` returns.
 */
class RoadPlanner {
  public:
    /**
     * @brief Precomputes city exit doors and candidate bridge leaps over `terrain`.
     * @param terrain Row-major `width` x `height` terrain grid.
     * @param width Map width in tiles.
     * @param height Map height in tiles.
     * @param cities Top-left coordinates of each 2x2 city footprint.
     * @param spans Candidate two-tile river bridge spans.
     */
    RoadPlanner(absl::Span<const uint8_t> terrain, int width, int height,
                absl::Span<const TileCoords> cities, absl::Span<const Span> spans);

    /**
     * @brief Computes the MST + shortcut road network and returns the road mask and used
     * bridges.
     * @param rng Dice sequence used to roll shortcut link inclusion (`Uniform(rng) < 0.7`).
     * @return Planned `Roads` grid and sorted indices of used bridge spans.
     */
    Roads Plan(Rng& rng);

  private:
    /** @brief Candidate inter-city road connection with travel-cost estimate. */
    struct Link {
        double cost = 0.0;  ///< A* construction cost between city doors.
        int city_a = 0;     ///< Lower city index.
        int city_b = 0;     ///< Higher city index.
    };

    using BridgeLeap = std::pair<int, int>;
    using TreeEdge = std::pair<int, double>;

    double BuildCost(int tile) const;
    std::vector<int> CityDoors(int city_idx) const;
    double Route(int from_city, int to_city, std::vector<int>* tiles, std::vector<int>* used) const;
    static double TreeCost(const std::vector<std::vector<TreeEdge>>& tree, int from_city,
                           int to_city);
    static bool CheaperLink(const Link& lhs, const Link& rhs);

    absl::Span<const uint8_t> terrain_;          ///< Read-only terrain grid.
    absl::Span<const TileCoords> cities_;        ///< City footprint top-left coordinates.
    absl::Span<const Span> spans_;               ///< Candidate river bridge spans.
    Grid grid_;                                  ///< Map bounds helper.
    std::vector<std::vector<BridgeLeap>> leap_;  ///< Per-tile `(landing_tile, span_idx)` leaps.
    std::vector<std::vector<int>> door_;         ///< Per-city passable perimeter tiles.
    std::vector<int> door_of_;                   ///< Per-tile owning city index, or -1.
    std::vector<uint8_t> road_;                  ///< Working road mask (`1` where laid).
};

}  // namespace w2::scengen
