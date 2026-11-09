/**
 * @file
 * @brief ArmyType and Types domain model structs (TERRAIN0/ARMYTYPE.DAT).
 */
#pragma once

#include <array>
#include <string>
#include <vector>

#include "warlords/model/ids.hpp"

namespace w2 {

/** @brief An army type as ARMYTYPE.DAT gives it (docs/formats/armytype.md). */
struct ArmyType {
    int id = 0;        ///< The type id; also the sprite index.
    std::string name;  ///< The name shown in the game.
    int strength = 0;  ///< Fighting strength, 1..9.
    int time = 0;      ///< Turns to build one.
    int cost = 0;      ///< Per-turn upkeep basis.
    int move = 0;      ///< Movement points per turn.
    int price = 0;     ///< What it costs to buy for a city; negative: can never be bought.
    /// The record's ability and bonus words, keyed by their offset in the
    /// record (32..60); see docs/formats/armytype.md.
    std::array<int, 64> bonus{};
    bool flies = false;       ///< It flies: crosses any terrain, water included.
    bool siege = false;       ///< A siege type.
    bool boat = false;        ///< It is a boat.
    bool woods_move = false;  ///< It moves through forest at little cost.
    bool hills_move = false;  ///< It moves through hills at little cost.

    /** @brief The army's own +n strength bonus on terrain class `cls` (0..3). */
    int SelfBonus(int cls) const { return bonus[32 + 2 * cls]; }
    /** @brief The +n strength bonus the army gives its whole battle line on terrain class `cls`. */
    int StackBonus(int cls) const { return bonus[40 + 2 * cls]; }
    /** @brief True if this is a magical / special ally type (offset 48). */
    bool IsMagical() const { return bonus[48] != 0; }
    /** @brief The -n penalty this army applies to every enemy army in battle (offset 50). */
    int Subtract() const { return bonus[50]; }
    /** @brief Special combat ability code: 1 = siege, 2 = negate hero, 3 = negate stack bonus. */
    int Ability() const { return bonus[52]; }
};

/** @brief Every army type of the game, in file order and by id. */
struct Types {
    std::vector<ArmyType> all;        ///< In file (display) order.
    std::array<ArmyType*, 64> ids{};  ///< By type id; null where there is none.
    /** @brief The type with this id, or null for an unknown one. */
    const ArmyType* ById(int type_id) const {
        return type_id >= 0 && type_id < 64 ? ids[type_id] : nullptr;
    }
    /** @brief The type with strong identifier `type_id`, or null for an unknown one. */
    const ArmyType* ById(ArmyTypeId type_id) const { return ById(type_id.Value()); }
    /** @brief Every type, in file order. */
    std::vector<const ArmyType*> List() const {
        std::vector<const ArmyType*> out;
        for (const auto& army_type : all) out.push_back(&army_type);
        return out;
    }
};

}  // namespace w2
