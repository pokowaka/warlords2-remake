// Saving and loading a game in progress.
//
// Uses the C++ port's JSON save format. Only mutable game state is written;
// static scenario data (map geometry, army type tables, terrain) is reloaded
// from the data files on decode. Armies are assigned sequential save IDs so
// cross-references (such as quest heroes and staged AI group armies) are
// preserved across save and load.
#pragma once

#include <memory>
#include <string>

#include "warlords/types.hpp"

namespace w2::save {

/// Current JSON save schema version.
constexpr int kVersion = 1;

/** @brief Serializes `game` to a JSON string. */
std::string Encode(const Game& game);
/** @brief Reconstructs a `Game` from serialized JSON `text`, or throws `std::runtime_error`. */
std::unique_ptr<Game> Decode(const std::string& text, const std::string& data_dir);

}  // namespace w2::save
