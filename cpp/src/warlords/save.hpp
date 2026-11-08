/**
 * @file
 * @brief Saving and loading a game in progress.
 *
 * Uses the C++ port's JSON save format (`nlohmann::ordered_json`). Only mutable
 * game state is written; static scenario data (map geometry, army type tables,
 * terrain) is reloaded from the data files (or embedded `"randomWorld"` files)
 * on decode. Armies are assigned sequential save IDs so cross-references (such
 * as quest heroes and staged AI group armies) are preserved across save and
 * load.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

#include "warlords/types.hpp"

namespace w2::save {

/// Current JSON save schema version.
constexpr int kVersion = 1;

/** @brief Serializes `game` to a JSON string. */
std::string Encode(const Game& game);

/** @brief Reconstructs a `Game` from serialized JSON `text`. */
absl::StatusOr<std::unique_ptr<Game>> Decode(std::string_view text,
                                             const std::filesystem::path& data_dir);

}  // namespace w2::save
