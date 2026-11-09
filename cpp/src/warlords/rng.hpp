/**
 * @file
 * @brief Dice, the way the original game rolls them.
 *
 * Every random rule in the original goes through one function,
 * `dice(n, sides, bonus)` (see docs/re/dice_callers.md), so this is the
 * single source of randomness for the engine too. The *sequence* is not
 * bit-compatible with the original, but it is with the Lua remake: the same
 * seed plays the same game in both.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace w2 {

/**
 * @brief Deterministic 32-bit linear congruential random number generator.
 *
 * Uses Numerical Recipes' constants (`1664525`, `1013904223`), matching the
 * Lua and JavaScript ports bit-for-bit so any given seed produces the exact
 * same game and generated worlds across every port.
 *
 * Not thread-safe without external synchronization; each `Game` or generator
 * instance owns its own `Rng` value.
 */
class Rng {
  public:
    /**
     * @brief Initializes the sequence from `seed`, wrapped modulo $2^{32}$.
     * @param seed Floating-point seed matching Lua/JS numeric representation.
     */
    explicit Rng(double seed = 0.0);

    /**
     * @brief Draws a uniform integer in `[0, n)`.
     * @param n Exclusive positive upper bound.
     * @return Sampled integer in `0..n-1`.
     */
    int Below(int n);

    /**
     * @brief Rolls `n` dice of `sides` faces each and adds `bonus`.
     * @param n Number of dice to roll.
     * @param sides Faces per die (`<= 0` performs no rolls and returns `bonus`).
     * @param bonus Flat modifier added to the sum.
     * @return Sum of rolls plus `bonus`.
     */
    int Dice(int n, int sides, int bonus = 0);

    /**
     * @brief Evaluates the original game's `dice(1, 100, 0) < percent` check.
     *
     * True `(percent - 1)` times in 100, matching how the 1993 engine encodes
     * percentage thresholds.
     * @param percent Strict upper bound compared against `1d100`.
     * @return True when the `1..100` roll is strictly below `percent`.
     */
    bool Chance(int percent);

    /**
     * @brief Picks a uniform index into a list of size `n`, or `-1` when `n == 0` (no draw).
     * @param n Number of elements in the candidate collection.
     * @return Chosen `0`-based index, or `-1` if empty.
     */
    int PickIndex(size_t n);

    /**
     * @brief Picks a uniform element from `list`, or returns `none` without drawing when empty.
     * @param list Candidate elements.
     * @param none Fallback returned when `list` is empty.
     * @return Sampled element or `none`.
     */
    template <class T>
    T Pick(const std::vector<T>& list, T none = T()) {
        int index = PickIndex(list.size());
        return index < 0 ? none : list[index];
    }

    /**
     * @brief Shuffles `list` in place (Fisher-Yates) in the exact draw order of the Lua port.
     * @param list Mutable vector to permute in place.
     */
    template <class T>
    void Shuffle(std::vector<T>& list) {
        for (int i = static_cast<int>(list.size()); i >= 2; i--) {
            int swap_with = Below(i) + 1;
            std::swap(list[i - 1], list[swap_with - 1]);
        }
    }

    /** @brief Returns the current 32-bit internal LCG state word. */
    uint32_t State() const { return state_; }

    /**
     * @brief Restores the 32-bit internal LCG state word directly (e.g. when loading a save).
     * @param state Exact 32-bit LCG state word to install.
     */
    void SetState(uint32_t state) { state_ = state; }

  private:
    uint32_t state_ = 0;  ///< Current 32-bit LCG state word.
};

}  // namespace w2
