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

#include <cstdint>
#include <vector>

namespace w2 {

/**
 * @brief The game's random number generator: a 32-bit linear congruential one.
 *
 * Uses Numerical Recipes' constants, as the Lua and JavaScript ports do, so a
 * seed plays the same in every port.
 */
class Rng {
  public:
    /** @brief Starts the sequence from a seed, wrapped into 32 bits. */
    explicit Rng(double seed = 0) {
        // ((seed % M) + M) % M, on the double the Lua and JS hold
        long long whole_seed = static_cast<long long>(seed);
        long long modulus = 0x100000000LL;
        state = static_cast<uint32_t>(((whole_seed % modulus) + modulus) % modulus);
    }

    /** @brief A uniform integer in [0, n). */
    int Below(int n) {
        state = state * 1664525U + 1013904223U;
        // take the high bits: the low ones of an LCG cycle far too visibly
        return static_cast<int>((state >> 16) % static_cast<uint32_t>(n));
    }

    /** @brief The sum of `n` rolls of a `sides`-sided die, plus `bonus`. */
    int Dice(int n, int sides, int bonus = 0) {
        int total = bonus;
        if (sides <= 0) return total;
        for (int i = 0; i < n; i++) total += Below(sides) + 1;
        return total;
    }

    /**
     * @brief The game's `dice(1,100,0) < percent`.
     *
     * True (percent - 1) times in 100, as the original writes its
     * percentages.
     */
    bool Chance(int percent) { return Dice(1, 100, 0) < percent; }

    /** @brief A random index into a list of `n`, or -1 for an empty one (no draw). */
    int PickIndex(size_t n) { return n == 0 ? -1 : Below(static_cast<int>(n)); }

    /** @brief A random element of a list, or `none` if it is empty. */
    template <class T>
    T Pick(const std::vector<T>& list, T none = T()) {
        int index = PickIndex(list.size());
        return index < 0 ? none : list[index];
    }

    /** @brief Shuffles a list in place (Fisher-Yates), in the Lua's order of draws exactly. */
    template <class T>
    void Shuffle(std::vector<T>& list) {
        for (int i = static_cast<int>(list.size()); i >= 2; i--) {
            int swap_with = Below(i) + 1;
            std::swap(list[i - 1], list[swap_with - 1]);
        }
    }

    uint32_t state = 0;  ///< The generator's whole state.
};

}  // namespace w2
