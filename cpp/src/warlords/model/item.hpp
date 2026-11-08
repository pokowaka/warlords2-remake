/**
 * @file
 * @brief Magic Item and PoolItem domain model structs.
 */
#pragma once

#include <string>

#include "warlords/model/army_type.hpp"

namespace w2 {

/** @brief A magic item, a standard included. */
struct Item {
    int index = 0;            ///< Its place in Map::items.
    std::string name;         ///< The item's name.
    int type = 0;             ///< Its effect (rules::kItemBattle and so on).
    int value = 0;            ///< How strong the effect is.
    int status = 0;           ///< 0 out of play, 1 on the ground, 2 hidden, 3 carried.
    int x = kNone;            ///< Column where it lies, if on the ground or hidden.
    int y = kNone;            ///< Row where it lies, if on the ground or hidden.
    bool planted = false;     ///< A standard planted in the ground.
    int standard_of = kNone;  ///< Items 0-7: the eight sides' standards.
};

/** @brief An entry of the scenario's item pool (.ITM, docs/formats/itm.md). */
struct PoolItem {
    std::string name;  ///< The item's name.
    int type = 0;      ///< Its effect.
    int value = 0;     ///< How strong the effect is.
};

}  // namespace w2
