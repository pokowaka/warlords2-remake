/**
 * @file
 * @brief Army and Transit domain model structs.
 */
#pragma once

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "warlords/model/army_type.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

struct Item;

/** @brief An army on its way to a city or a standard, off the map. */
struct Transit {
    int turns = 0;     ///< Turns until it arrives.
    int dest = kNone;  ///< A city index, or kStandard.
};

/** @brief One army: a unit, a hero, or a ruin's allies. */
struct Army {
    ArmyId id = ArmyId::None();  ///< Stable monotonic identifier assigned by `ArmyRegistry`.
    int slot = 0;                ///< Its place in the army table.
    int x = kNone;               ///< Column; kNone while in transit.
    int y = kNone;               ///< Row; kNone while in transit.
    int owner = kNone;           ///< The side owning it; kNone for neutral.
    int type = 0;                ///< The army type id.
    std::string name;            ///< Its name.
    bool female = false;         ///< A heroine.
    int strength = 0;            ///< Fighting strength.
    int moves = 0;               ///< Movement points left this turn.
    int max_moves = 0;           ///< Movement points per turn.
    int upkeep = 0;              ///< Gold it costs each turn.
    int home_city = kNone;       ///< The city that built it.
    int medals = 0;              ///< Medals won in battle.
    bool at_sea = false;         ///< Aboard a ship.
    int group = 0;               ///< The bottom bar's group it belongs to; 0 ungrouped.
    std::optional<std::pair<int, int>> target;  ///< Where it is walking to.
    bool fortified = false;                     ///< Fortified: left out of the cycle.
    // heroes
    int level = 0;                   ///< A hero's level.
    int experience = 0;              ///< A hero's experience.
    std::string title;               ///< A hero's title.
    std::vector<Item*> items;        ///< Items it carries.
    std::set<int> blessings;         ///< Temples that have blessed it.
    std::optional<Transit> transit;  ///< On its way somewhere, off the map.
    bool returning = false;          ///< Going back to its home city.
    // the turn's marks
    bool done = false;       ///< Done for the turn.
    bool offered = false;    ///< Offered by the army cycle already this pass.
    bool disbanded = false;  ///< Disbanded this turn.
    // the computer player's
    int ai_order = 0;         ///< The computer's order for it.
    int ai_dest = kNone;      ///< The order's destination.
    int ai_group = 0;         ///< The computer's group it moves with.
    bool ai_explore = false;  ///< Sent exploring.
    bool ai_neutral = false;  ///< Sent after a neutral city.
    bool ai_party = false;    ///< Part of a party.
    bool ai_moved = false;    ///< Moved this turn.

    /** @brief Is it a hero? */
    bool Hero() const;
    /** @brief True if the army is standing on a valid map tile (not in transit). */
    bool OnMap() const { return !transit.has_value() && x != kNone && y != kNone; }
    /** @brief True if the army belongs to `side_index`. */
    bool OwnedBy(int side_index) const { return owner == side_index; }
    /** @brief True if the army is on the map, has moves remaining, and is not done/fortified. */
    bool CanMove() const { return moves > 0 && !done && !fortified && OnMap(); }
    /** @brief Returns the army's current tile position `(x, y)`. */
    TilePos Position() const { return {x, y}; }
};

}  // namespace w2
