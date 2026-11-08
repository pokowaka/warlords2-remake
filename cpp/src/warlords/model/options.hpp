/**
 * @file
 * @brief Game setup Options domain model struct.
 */
#pragma once

#include <string>
#include <vector>

namespace w2 {

/** @brief The game options, as the setup screen and the scenario set them. */
struct Options {
    int neutral_cities = 0;    ///< Neutral cities' garrisons: 0 none .. 3 strong.
    int diplomacy = 0;         ///< Diplomacy on.
    int quests = 0;            ///< Quests on.
    int random_turns = 0;      ///< The turn order is shuffled each round.
    int hidden_map = 0;        ///< Each side sees only what it has explored.
    int intense_combat = 0;    ///< Intense combat.
    int quick_start = 0;       ///< Quick start: the cities are dealt out at the start.
    int view_enemies = 0;      ///< Enemy stacks can be looked into.
    int military_advisor = 1;  ///< The military advisor is available.
    int tutorial = 0;          ///< The tutorial's moments are shown.
    int view_production = 0;   ///< Enemy production can be looked at.
    /** @brief The option by the Lua's name for it; null for an unknown name. */
    int* Field(const std::string& name);
    /** @brief Every option's Lua name. */
    static const std::vector<std::string>& Names();
};

}  // namespace w2
