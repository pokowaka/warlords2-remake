// The scenarios New Scenario offers: SCENARIO.DAT's, and then the ones the
// scenario generator made (not the original's; docs/scengen.md).
//
// A generated scenario lives in its own folder, DATA/NAME/NAME.*, like the
// shipped ones, but is not written into SCENARIO.DAT, so the original's file
// stays as it shipped. Instead the folder holds NAME.INF, a few lines of
// "key=value" the list reads:
//
//     name=Continents 11        the name in the list, at most 20 characters
//     text=A new world, seed 11 the line under it, at most 30
//     cities=80
//     ruins=40
//     players=8
//
// and PICS/SCENARIO.PCK, the picture the start menu shows, as the shipped
// scenarios have.
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

/** SCENARIO.DAT's records in order, then every generated scenario in the
 *  data folder, by folder name. */
std::vector<Entry> list(const std::string& dataDir);

/** An entry as NAME.INF holds it. */
std::string info(const Entry& e);
/** NAME.INF read back; nothing if it lacks a name. */
std::optional<Entry> parseInfo(const std::string& text, const std::string& dir);

}  // namespace w2::scenarios
