/**
 * @file
 * @brief Site (temple or ruin), Monster guardian, and Signpost domain model structs.
 */
#pragma once

#include <array>
#include <string>

#include "warlords/model/army_type.hpp"

namespace w2 {

/** @brief A temple or a ruin (docs/rules.md > Ruins, temples and sages). */
struct Site {
    int index = 0;             ///< Its place in Map::sites.
    int x = 0;                 ///< Column.
    int y = 0;                 ///< Row.
    std::string name;          ///< The site's name.
    int type = 0;              ///< 1 = temple, 2 = ruin.
    int content = 0;           ///< What a search finds (temple, empty, sage, gold, allies...).
    int item = kNone;          ///< The item hidden in it, if any.
    int guardian = kNone;      ///< The monster guarding it, if any.
    int ally_type = kNone;     ///< The army type of the allies it holds, if any.
    bool rich = false;         ///< A rich ruin: the reserved items are hidden only in these.
    int revealed = 0;          ///< A bit per side the site is shown to.
    bool searched = false;     ///< Searched already: nothing more to find.
    std::string band;          ///< "rich", "near" or "far": the table its content is drawn from.
    int temple_index = kNone;  ///< For a temple: its number among the temples.
};

/** @brief A ruin's guardian. */
struct Monster {
    int index = 0;     ///< Its place in Map::monsters.
    std::string name;  ///< The monster's name.
    int strength = 0;  ///< Its fighting strength.
};

/** @brief A signpost on the map. */
struct Sign {
    int index = 0;                     ///< Its place in Map::signs.
    int x = 0;                         ///< Column.
    int y = 0;                         ///< Row.
    std::array<std::string, 2> lines;  ///< Its two lines of text.
};

}  // namespace w2
