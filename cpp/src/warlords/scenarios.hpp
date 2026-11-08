/**
 * @file
 * @brief The scenarios New Scenario offers: SCENARIO.DAT's, and then the ones
 *        the scenario generator made (not the original's; docs/scengen.md).
 *
 * A generated scenario lives in its own folder, DATA/NAME/NAME.*, like the
 * shipped ones, but is not written into SCENARIO.DAT, so the original's file
 * stays as it shipped. Instead DATA/DATA/SCENGEN.LST names the generated
 * folders, one a line (w2scengen keeps it), and each folder holds NAME.INF,
 * a few lines of "key=value" the list reads:
 *
 *     name=Continents 11        the name in the list, at most 20 characters
 *     text=A new world, seed 11 the line under it, at most 30
 *     cities=80
 *     ruins=40
 *     players=8
 *
 * and PICS/SCENARIO.PCK, the picture the start menu shows, as the shipped
 * scenarios have. A folder named in SCENGEN.LST without its NAME.INF (taken
 * away since) is left out. The JavaScript and Lua games read the same files
 * the same way (web/src/ui/start.js, love2d/ui/start.lua).
 */
#pragma once

#include <optional>
#include <string>
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
std::vector<Entry> List(const std::string& data_dir);

/** @brief Adds a folder to SCENGEN.LST, once, keeping the list sorted. */
bool Remember(const std::string& data_dir, const std::string& dir);

/** @brief Writes an entry as its NAME.INF holds it. */
std::string Info(const Entry& entry);

/**
 * @brief Reads a NAME.INF back.
 * @return The entry, or std::nullopt if the file names no scenario.
 */
std::optional<Entry> ParseInfo(const std::string& text, const std::string& dir);

}  // namespace w2::scenarios
