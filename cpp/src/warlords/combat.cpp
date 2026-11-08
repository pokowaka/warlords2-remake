#include "warlords/combat.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2::combat {

const char* const kClassNames[4] = {"city", "open", "woods", "hills"};
const char* const kAdvice[10] = {
    "complete and utter suicide!",
    "sheerest folly! Thou shouldst not attack!",
    "a foolish decision!",
    "a brave choice! I leave it to thee!",
    "difficult but not impossible to win!",
    "very evenly matched!",
    "a hard-fought victory! But we shall win!",
    "a comfortable victory!",
    "an easy victory! We cannot lose!",
    "as simple as butchering sleeping cattle!",
};

// Offsets into an army type's bonus fields (see docs/formats/armytype.md).
// The two terrain bonuses are per class: add 2 * class.
static const int kBonusSelf = 32;   ///< The army's own +n strength on a terrain class.
static const int kBonusStack = 40;  ///< The +n the army gives its whole line on a class.
static const int kSubtract = 50;    ///< The -n the army takes off every enemy army.
static const int kAbility = 52;     ///< The ability enum: kSiege, kNegateHero, kNegateOther.

int TerrainClass(const Game& game, int x, int y) {
    if (game::TowerAt(game, x, y)) return kCity;
    int terrain = scn::TerrainAt(*game.map, x, y);
    if (terrain == move::kCity || terrain == move::kSite) return kCity;
    if (terrain == move::kForest) return kWoods;
    if (terrain == move::kHills || terrain == move::kMountains) return kHills;
    return kOpen;
}

namespace {
/**
 * @brief Picks the flier a line holding a hero sends to the back.
 *
 * On water, shore and mountains a line holding a hero sends one flier to the
 * back, keeping the hero's carrier alive longest: the first flier in the
 * order the line was gathered that is not at sea gets 80 on its rank (before
 * the line is sorted).
 * @return The carrier, or null.
 */
const Army* HeroCarrier(const Game& game, const std::vector<Army*>& line, int x, int y) {
    int terrain = scn::TerrainAt(*game.map, x, y);
    if (terrain != move::kWater && terrain != move::kShore && terrain != move::kMountains)
        return nullptr;
    if (std::none_of(line.begin(), line.end(),
                     [](Army* army) { return army->type == armytype::kHero; }))
        return nullptr;
    for (Army* army : line)
        if (!army->at_sea && game.types.ById(army->type)->flies) return army;
    return nullptr;
}

/**
 * @brief Sorts a line by its owner's fight order, lowest first.
 *
 * Ties keep the order the line was gathered in, as the original's insertion
 * sort does. The hero's carrier ranks 80 higher, so it fights last.
 */
void SortLine(const Game& game, std::vector<Army*>& line, int owner_index, const Army* carrier) {
    const auto& row = game.map->fight_order[owner_index == kNone ? 8 : owner_index];
    std::map<const Army*, std::pair<int, int>> sort_keys;
    for (size_t i = 0; i < line.size(); i++) {
        sort_keys[line[i]] = {row[line[i]->type] + (line[i] == carrier ? 80 : 0),
                              static_cast<int>(i)};
    }
    LuaSort(line, [&](Army* left, Army* right) {
        const auto& left_key = sort_keys[left];
        const auto& right_key = sort_keys[right];
        if (left_key.first != right_key.first) return left_key.first < right_key.first;
        return left_key.second < right_key.second;
    });
}

/** @brief The summed value of the army's items of one type. */
int ItemTotal(const Army& army, int item_type) {
    int total = 0;
    for (Item* item : army.items)
        if (item->type == item_type) total += item->value;
    return total;
}

/** @brief Whether any army in the line has the given ability (kSiege, ...). */
bool LineHas(const Game& game, const std::vector<Army*>& line, int ability) {
    for (Army* army : line)
        if (game.types.ById(army->type)->bonus[kAbility] == ability) return true;
    return false;
}

/** @brief The best stack bonus any army in the line gives on the terrain class. */
int StackBonus(const Game& game, const std::vector<Army*>& line, int cls) {
    int best = 0;
    for (Army* army : line)
        best = std::max(best, game.types.ById(army->type)->bonus[kBonusStack + 2 * cls]);
    return best;
}

/** @brief The largest penalty (most negative subtract) the line puts on its enemies. */
int Subtract(const Game& game, const std::vector<Army*>& line) {
    int least = 0;
    for (Army* army : line) least = std::min(least, game.types.ById(army->type)->bonus[kSubtract]);
    return least;
}
}  // namespace

Lines BattleLines(const Game& game, const std::vector<Army*>& stack, int x, int y) {
    int attacking_side = stack.empty() ? kNone : stack[0]->owner;
    // only a standing city is fought for as one (its terrain is 10, which the
    // original's battle setup and aftermath test); ruins are open ground
    City* city = game.map->city_tile[y * game.map->width + x];
    if (city && city->razed) city = nullptr;
    int min_x = city ? city->x : x, min_y = city ? city->y : y;
    int max_x = city ? city->x + 1 : x, max_y = city ? city->y + 1 : y;
    Lines lines;
    bool found_defender = false;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (!army->transit && army->owner != attacking_side && army->x >= min_x &&
            army->x <= max_x && army->y >= min_y && army->y <= max_y) {
            lines.defenders.push_back(army);
            if (!found_defender) {
                lines.def_owner = army->owner;
                found_defender = true;
            }
        }
    }
    lines.attackers = stack;
    SortLine(game, lines.attackers, attacking_side, HeroCarrier(game, lines.attackers, x, y));
    SortLine(game, lines.defenders, lines.def_owner, HeroCarrier(game, lines.defenders, x, y));
    lines.city = city;
    return lines;
}

int BattleItems(const Army& army) {
    return ItemTotal(army, rules::kItemBattle);
}

int CommandItems(const Army& army) {
    int count = ItemTotal(army, rules::kItemCommand);
    for (Item* item : army.items)
        if (item->type == rules::kItemStandard) count++;
    return count;
}

int HeroBonus(const Game& game, const std::vector<Army*>& line) {
    int best = 0, command = 0;
    for (Army* army : line) {
        if (army->type == armytype::kHero) {
            int hero_strength = army->strength + BattleItems(*army);
            best = std::max(best, std::min(9, hero_strength));
            command += CommandItems(*army);
        }
    }
    return kHeroTable[std::max(0, best)] + command;
}

int Fortify(const Game& game, const std::vector<Army*>& attackers, int x, int y, int cls) {
    if (cls != kCity) return 0;
    if (LineHas(game, attackers, kSiege)) return 0;
    int value;
    City* city = game.map->city_tile[y * game.map->width + x];
    if (game::TowerAt(game, x, y))
        value = 1;
    else if (scn::TerrainAt(*game.map, x, y) == move::kSite)
        value = 2;
    else
        value = city ? city->defence : 0;
    if (city && city->owner_index == kNone) value = value / 2;  // neutral
    return value;
}

std::pair<int, int> Modifiers(const Game& game, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls) {
    int cap = game.map->combat_cap;
    int atk_hero = LineHas(game, defenders, kNegateHero) ? 0 : HeroBonus(game, attackers);
    int atk_stack = LineHas(game, defenders, kNegateOther) ? 0 : StackBonus(game, attackers, cls);
    int def_hero = LineHas(game, attackers, kNegateHero) ? 0 : HeroBonus(game, defenders);
    int def_stack = LineHas(game, attackers, kNegateOther) ? 0 : StackBonus(game, defenders, cls);
    int attack_mod = std::min(cap, atk_hero + atk_stack) + Subtract(game, defenders);
    int defend_mod = std::min(cap, def_hero + def_stack + Fortify(game, attackers, x, y, cls)) +
                     Subtract(game, attackers);
    return {attack_mod, defend_mod};
}

int Strength(const Game& game, const Army& army, int mod, int cls, int terrain) {
    if (army.at_sea && (terrain == move::kWater || terrain == move::kShore)) return 4;
    int strength = army.strength + mod + BattleItems(army) +
                   game.types.ById(army.type)->bonus[kBonusSelf + 2 * cls];
    return std::min(kMaxStrength, strength);
}

std::map<const Army*, int> StackStrengths(const Game& game, const std::vector<Army*>& armies, int x,
                                          int y) {
    int cls = TerrainClass(game, x, y);
    int total = 0, best_stack_bonus = 0;
    for (Army* army : armies) {
        if (!army->at_sea) {
            if (army->type == armytype::kHero) {
                int hero_strength = std::min(9, army->strength + BattleItems(*army));
                total += kHeroTable[std::max(0, hero_strength)] + CommandItems(*army);
            } else {
                int stack_bonus = game.types.ById(army->type)->bonus[kBonusStack + 2 * cls];
                if (stack_bonus > best_stack_bonus) {
                    total = total + stack_bonus - best_stack_bonus;
                    best_stack_bonus = stack_bonus;
                }
            }
        }
    }
    total = std::min(total, game.map->combat_cap);
    std::map<const Army*, int> strengths;
    for (Army* army : armies) {
        int strength;
        if (army->at_sea)
            strength = 4;
        else if (army->type == armytype::kHero)
            strength = army->strength + BattleItems(*army);
        else
            strength = army->strength + game.types.ById(army->type)->bonus[kBonusSelf + 2 * cls];
        strengths[army] = std::min(9, strength) + total;
    }
    return strengths;
}

Battle Resolve(Game& game, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y) {
    int cls = TerrainClass(game, x, y);
    int terrain = scn::TerrainAt(*game.map, x, y);
    auto [attack_mod, defend_mod] = Modifiers(game, attackers, defenders, x, y, cls);
    int die = game.map->options.intense_combat != 0 ? kDieIntense : kDie;

    /** @brief One army in a battle line, as the fight wears it down. */
    struct Fighter {
        Army* army;         ///< The army fighting.
        int strength;       ///< Its battle strength, at least 1.
        int hits;           ///< Hits it can still take; it dies below 0.
        bool dead = false;  ///< Whether it has died.
    };
    auto prepare = [&](const std::vector<Army*>& line, int mod) {
        std::vector<Fighter> fighters;
        for (Army* army : line)
            fighters.push_back(Fighter{army, std::max(1, Strength(game, *army, mod, cls, terrain)),
                                       kHitPoints - 1});
        return fighters;
    };
    auto attack_line = prepare(attackers, attack_mod), defend_line = prepare(defenders, defend_mod);
    Battle battle;
    // an army at sea on water or shore fights as a boat, strength 4;
    // Triumphs counts it a navy
    for (const auto* line : {&attackers, &defenders}) {
        for (Army* army : *line) {
            if (army->at_sea && (terrain == move::kWater || terrain == move::kShore))
                battle.boats.insert(army);
        }
    }
    size_t attacker_index = 0, defender_index = 0;
    int throws = 0;
    while (attacker_index < attack_line.size() && defender_index < defend_line.size()) {
        Fighter& attacker = attack_line[attacker_index];
        Fighter& defender = defend_line[defender_index];
        int attacker_roll = game.rng.Dice(1, die, 0), defender_roll = game.rng.Dice(1, die, 0);
        bool attacker_hits =
                attacker_roll <= attacker.strength && defender_roll > defender.strength;
        bool defender_hits =
                defender_roll <= defender.strength && attacker_roll > attacker.strength;
        // past 10,000 throws in one duel the attacker takes the hit; the count
        // starts again with each army that falls
        if (throws > kStalemate) {
            attacker_hits = false;
            defender_hits = true;
        }
        throws++;

        // Tutorial: a human player's hero cannot die attacking a neutral defender.
        if (defender_hits && game.map->options.tutorial != 0 &&
            attacker.army->type == armytype::kHero) {
            Side* side = game.map->Side(attacker.army->owner == kNone ? 0 : attacker.army->owner);
            if (!(side && side->computer) && defender.army->owner == kNone) defender_hits = false;
        }
        if (attacker_hits) {
            defender.hits--;
            if (defender.hits < 0) {
                defender.dead = true;
                battle.log.push_back(0);
                defender_index++;
                throws = 0;
            }
        } else if (defender_hits) {
            attacker.hits--;
            if (attacker.hits < 0) {
                attacker.dead = true;
                battle.log.push_back(1);
                attacker_index++;
                throws = 0;
            }
        }
    }
    for (auto& fighter : attack_line)
        (fighter.dead ? battle.dead_attackers : battle.attackers).push_back(fighter.army);
    for (auto& fighter : defend_line)
        (fighter.dead ? battle.dead_defenders : battle.defenders).push_back(fighter.army);
    for (Army* army : battle.dead_attackers) battle.dead_by_army.insert(army);
    for (Army* army : battle.dead_defenders) battle.dead_by_army.insert(army);
    battle.won = battle.defenders.empty();
    battle.attack_mod = attack_mod;
    battle.defend_mod = defend_mod;
    battle.cls = cls;
    battle.def_strength0 =
            defenders.empty() ? 0 : Strength(game, *defenders[0], defend_mod, cls, terrain);
    return battle;
}

std::pair<std::string, int> Advise(Game& game, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y) {
    int wins = 0;
    for (int i = 0; i < kAdviceBattles; i++) {
        if (Resolve(game, attackers, defenders, x, y).won) wins++;
    }
    return {kAdvice[wins / 2], wins};
}

}  // namespace w2::combat
