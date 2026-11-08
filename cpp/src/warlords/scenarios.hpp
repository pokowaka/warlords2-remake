/**
 * @file
 * @brief The scenarios New Scenario offers: SCENARIO.DAT's, and then the ones
 *        the scenario generator made (not the original's; docs/scengen.md).
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace w2::scenarios {

/** @brief One scenario in New Scenario's list, shipped or generated. */
struct Entry {
    std::string name;        ///< The name the list shows, at most 20 characters.
    std::string dir;         ///< The scenario's folder (and file stem) under the data folder.
    std::string text;        ///< The line of description under the name, at most 30 characters.
    int cities = 0;          ///< How many cities the scenario has.
    int ruins = 0;           ///< How many ruins and other sites it has.
    int players = 0;         ///< How many sides can play it.
    bool generated = false;  ///< Made by the scenario generator, not in SCENARIO.DAT.
};

/**
 * @brief Lists every scenario New Scenario offers.
 * @return SCENARIO.DAT's records in order, then the generated scenarios
 *         SCENGEN.LST names, in its order.
 */
std::vector<Entry> List(const std::filesystem::path& data_dir);

/** @brief Adds a folder to SCENGEN.LST, once, keeping the list sorted. */
bool Remember(const std::filesystem::path& data_dir, const std::string& dir);

/** @brief Writes an entry as its NAME.INF holds it. */
std::string Info(const Entry& entry);

/**
 * @brief Reads a NAME.INF back.
 * @return The entry, or std::nullopt if the file names no scenario.
 */
std::optional<Entry> ParseInfo(std::string_view text, const std::string& dir);

}  // namespace w2::scenarios
