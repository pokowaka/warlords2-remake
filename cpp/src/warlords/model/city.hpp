/**
 * @file
 * @brief City and production Slot domain model structs.
 */
#pragma once

#include <string>
#include <vector>

#include "warlords/model/army_type.hpp"

namespace w2 {

struct Side;

/** @brief A production slot: an army type with the city's own numbers for it. */
struct Slot {
    int type = 0;      ///< The army type id.
    std::string name;  ///< The type's name.
    int strength = 0;  ///< Strength of the armies the city builds.
    int time = 0;      ///< Turns the city takes to build one.
    int cost = 0;      ///< Upkeep basis of the armies built.
    int move = 0;      ///< Movement points of the armies built.
    int price = 0;     ///< What the slot cost to buy.
    /** @brief Equal when every number matches. */
    bool operator==(const Slot& other) const {
        return type == other.type && name == other.name && strength == other.strength &&
               time == other.time && cost == other.cost && move == other.move &&
               price == other.price;
    }
};

/** @brief A city: a 2x2 block of tiles that earns gold and builds armies. */
struct City {
    int index = 0;              ///< Its place in Map::cities.
    int x = 0;                  ///< Column of its top-left tile.
    int y = 0;                  ///< Row of its top-left tile.
    std::string name;           ///< The city's name.
    int income = 0;             ///< Gold it earns its owner each turn.
    std::vector<int> produces;  ///< Army type ids, as the scenario gives them.
    int defence = 0;            ///< Its defence level, which strengthens its defenders.
    Side* owner = nullptr;      ///< Only while the scenario loads.
    int owner_index = kNone;    ///< The side holding it; kNone for neutral.
    std::vector<Slot> slots;    ///< What it can build.
    int producing = kNone;      ///< The slot index being built.
    int countdown = 0;          ///< Turns left until the army in production is done.
    int vector_to = kNone;      ///< Where it sends what it builds: a city index, or kStandard.
    bool razed = false;         ///< Burnt to the ground: out of play.
    int razed_by = kNone;       ///< The side that razed it.
    /// The computer whose ground it is; after a capture, the side it was
    /// taken from (docs/rules.md > Capture).
    int claim = kNone;
    int attacked = 0;  ///< A bit per side that has attacked it.

    /** @brief True if the city is currently neutral (unowned). */
    bool IsNeutral() const { return owner_index == kNone; }
    /** @brief True if the city is owned by `side_index`. */
    bool OwnedBy(int side_index) const { return owner_index == side_index; }
    /** @brief True if `(tile_x, tile_y)` lies within this city's 2x2 footprint. */
    bool ContainsTile(int tile_x, int tile_y) const {
        return tile_x >= x && tile_x <= x + 1 && tile_y >= y && tile_y <= y + 1;
    }
};

}  // namespace w2
