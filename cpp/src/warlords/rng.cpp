/**
 * @file
 * @brief Implementation of the deterministic 32-bit LCG random number generator (`w2::Rng`).
 */
#include "warlords/rng.hpp"

namespace w2 {

Rng::Rng(double seed) {
    // ((seed % M) + M) % M on the integer part of the double held by Lua and JS.
    long long whole_seed = static_cast<long long>(seed);
    long long modulus = 0x100000000LL;
    state_ = static_cast<uint32_t>(((whole_seed % modulus) + modulus) % modulus);
}

int Rng::Below(int n) {
    state_ = state_ * 1664525U + 1013904223U;
    // Take the high 16 bits: low bits of a power-of-two LCG have short periods.
    return static_cast<int>((state_ >> 16) % static_cast<uint32_t>(n));
}

int Rng::Dice(int n, int sides, int bonus) {
    int total = bonus;
    if (sides <= 0) return total;
    for (int i = 0; i < n; i++) total += Below(sides) + 1;
    return total;
}

bool Rng::Chance(int percent) {
    return Dice(1, 100, 0) < percent;
}

int Rng::PickIndex(size_t n) {
    return n == 0 ? -1 : Below(static_cast<int>(n));
}

}  // namespace w2
