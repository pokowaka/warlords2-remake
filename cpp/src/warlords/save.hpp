/**
 * @file
 * @brief Saving and loading a game in progress.
 *
 * Uses the C++ port's JSON save format (`nlohmann::ordered_json`). Only mutable
 * game state is written; static scenario data (map geometry, army type tables,
 * terrain) is reloaded from the data files (or embedded `"randomWorld"` files)
 * on decode. Armies preserve their table `slot` numbers and sequential save
 * indices so cross-references (such as quest heroes and computer-player state)
 * round-trip cleanly.
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

/**
 * @brief Reconstructs a `Game` from serialized JSON `text`.
 * @param text Serialized JSON save payload.
 * @param data_dir Root scenario/resource data directory.
 * @param player_factory Optional `IComputerPlayer` factory override; defaults to the registered
 *        process-wide factory when null.
 */
absl::StatusOr<std::unique_ptr<Game>> Decode(std::string_view text,
                                             const std::filesystem::path& data_dir,
                                             const ComputerPlayerFactory& player_factory = nullptr);

}  // namespace w2::save
