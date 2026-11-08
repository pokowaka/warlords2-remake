/**
 * @file
 * @brief TERRAIN0/ARMYTYPE.DAT, the 29 army types.
 *
 * See docs/formats/armytype.md. Records are keyed by their type id (the
 * record's first word), not by position in the file: the file is in display
 * order.
 */
#pragma once

#include <filesystem>

#include "absl/status/status.h"

#include "warlords/types.hpp"

namespace w2::armytype {

// Offsets in an army type's bonus record worth naming (docs/formats/armytype.md,
// docs/rules.md).
constexpr int kSiege = 52;      ///< Value 1 is the siege ability (+2 strength against cities).
constexpr int kFlies = 54;      ///< Non-zero: the army flies.
constexpr int kWoodsMove = 56;  ///< Non-zero: moves through woods as on open ground.
constexpr int kHillsMove = 58;  ///< Non-zero: moves through hills as on open ground.
constexpr int kBoat = 60;       ///< Non-zero: the army is a boat.

constexpr int kNavy = 5;     ///< Removed from every city's production at game start.
constexpr int kHero = 28;    ///< The hero's type id.
constexpr int kScouts = 11;  ///< Placeholder garrison when Neutral Cities is off.

/**
 * @brief Reads `ARMYTYPE.DAT` into `out`, replacing what it held.
 *
 * Fills `out.all` in file order and `out.ids` by type id.
 * @param path Path to `ARMYTYPE.DAT`.
 * @param out Destination `Types` table.
 * @return `absl::OkStatus()` on success, or an actionable error status if missing or malformed.
 */
absl::Status Load(const std::filesystem::path& path, Types& out);

}  // namespace w2::armytype
