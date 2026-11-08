// The scenarios New Scenario offers: SCENARIO.DAT's, and then the ones the
// scenario generator made (not the original's; docs/scengen.md).
//
// A generated scenario lives in its own folder, DATA/NAME/NAME.*, like the
// shipped ones, but is not written into SCENARIO.DAT, so the original's file
// stays as it shipped. Instead DATA/DATA/SCENGEN.LST names the generated
// folders, one a line (w2scengen keeps it), and each folder holds NAME.INF,
// a few lines of "key=value" the list reads:
//
//     name=Continents 11        the name in the list, at most 20 characters
//     text=A new world, seed 11 the line under it, at most 30
//     cities=80
//     ruins=40
//     players=8
//
// and PICS/SCENARIO.PCK, the picture the start menu shows, as the shipped
// scenarios have. A folder named in SCENGEN.LST without its NAME.INF (taken
// away since) is left out. The JavaScript and Lua games read the same files
// the same way (web/src/ui/start.js, love2d/ui/start.lua).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace w2::scenarios {

struct Entry {
    std::string name, dir, text;
    int cities = 0, ruins = 0, players = 0;
    bool generated = false;
};

/** SCENARIO.DAT's records in order, then the generated scenarios SCENGEN.LST
 *  names, in its order. */
std::vector<Entry> List(const std::string& data_dir);

/** Add a folder to SCENGEN.LST, once, keeping the list sorted. */
bool Remember(const std::string& data_dir, const std::string& dir);

/** An entry as NAME.INF holds it. */
std::string Info(const Entry& e);
/** NAME.INF read back; nothing if it lacks a name. */
std::optional<Entry> ParseInfo(const std::string& text, const std::string& dir);

}  // namespace w2::scenarios
