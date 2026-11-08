/**
 * @file
 * @brief Diplomacy relational state between every pair of sides.
 */
#pragma once

#include <array>

#include "warlords/model/army_type.hpp"

namespace w2 {

/**
 * @brief The diplomatic relations between every pair of sides.
 *
 * The 64-entry arrays are indexed by a * 8 + b, for side a's view of side b.
 */
struct Diplomacy {
    std::array<int, 64> state{};            ///< The diplomatic state.
    std::array<int, 64> proposal{};         ///< The state proposed; kNone where none was made.
    std::array<bool, 64> allied{};          ///< Allied.
    std::array<bool, 64> propose_allied{};  ///< An alliance proposed.
    std::array<int, 64> truce_turns{};      ///< Turns of truce left.
    std::array<int, 64> gold_offer{};       ///< > 0 offered by a to b; < 0 demanded by a from b.
    std::array<int, 64> war_target{};       ///< kNone or target side index 0..7 for joint war pact.
    std::array<int, 64> trust{};            ///< Relational trust from a toward b (-100..+100).
    /// Each side's allied target city.
    std::array<int, 8> allied_target_city{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
};

}  // namespace w2
