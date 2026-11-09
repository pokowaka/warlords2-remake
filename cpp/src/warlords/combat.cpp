/**
 * @file
 * @brief Implementation of `CombatResolver` and `w2::combat` tactical battle rules.
 */
#include "warlords/combat.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

const Army* HeroCarrier(const Game& game, const std::vector<Army*>& line, int x, int y) {
    int terrain = scn::TerrainAt(*game.map, x, y);
    if (terrain != move::kWater && terrain != move::kShore && terrain != move::kMountains)
        return nullptr;
    if (std::none_of(line.begin(), line.end(),
                     [](Army* army) { return army->type == armytype::kHero; }))
        return nullptr;
    for (Army* army : line) {
        if (!army->at_sea && game.types.ById(army->type)->flies) return army;
    }
    return nullptr;
}

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

int ItemTotal(const Army& army, int item_type) {
    int total = 0;
    for (Item* item : army.items) {
        if (item->type == item_type) total += item->value;
    }
    return total;
}

bool LineHas(const Game& game, const std::vector<Army*>& line, int ability) {
    for (Army* army : line) {
        if (game.types.ById(army->type)->Ability() == ability) return true;
    }
    return false;
}

int StackBonus(const Game& game, const std::vector<Army*>& line, int cls) {
    int best = 0;
    for (Army* army : line) best = std::max(best, game.types.ById(army->type)->StackBonus(cls));
    return best;
}

int Subtract(const Game& game, const std::vector<Army*>& line) {
    int least = 0;
    for (Army* army : line) least = std::min(least, game.types.ById(army->type)->Subtract());
    return least;
}

bool Ordinary(const Game& game, const Army* army) {
    return army->type != armytype::kHero && !game.types.ById(army->type)->IsMagical();
}

}  // namespace

namespace combat {

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

int BattleItems(const Army& army) {
    return ItemTotal(army, rules::kItemBattle);
}

int CommandItems(const Army& army) {
    int count = ItemTotal(army, rules::kItemCommand);
    for (Item* item : army.items) {
        if (item->type == rules::kItemStandard) count++;
    }
    return count;
}

int TerrainClass(const Game& game, int x, int y) {
    return CombatResolver(const_cast<Game&>(game)).TerrainClass(x, y);
}

Lines BattleLines(const Game& game, const std::vector<Army*>& stack, int x, int y) {
    return CombatResolver(const_cast<Game&>(game)).BattleLines(stack, x, y);
}

int HeroBonus(const Game& game, const std::vector<Army*>& line) {
    return CombatResolver(const_cast<Game&>(game)).HeroBonus(line);
}

int Fortify(const Game& game, const std::vector<Army*>& attackers, int x, int y, int cls) {
    return CombatResolver(const_cast<Game&>(game)).Fortify(attackers, x, y, cls);
}

std::pair<int, int> Modifiers(const Game& game, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls) {
    return CombatResolver(const_cast<Game&>(game)).Modifiers(attackers, defenders, x, y, cls);
}

int Strength(const Game& game, const Army& army, int mod, int cls, int terrain) {
    return CombatResolver(const_cast<Game&>(game)).Strength(army, mod, cls, terrain);
}

std::map<const Army*, int> StackStrengths(const Game& game, const std::vector<Army*>& armies, int x,
                                          int y) {
    return CombatResolver(const_cast<Game&>(game)).StackStrengths(armies, x, y);
}

Battle Resolve(Game& game, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y) {
    return CombatResolver(game).Resolve(attackers, defenders, x, y);
}

std::pair<std::string, int> Advise(Game& game, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y) {
    return CombatResolver(game).Advise(attackers, defenders, x, y);
}

}  // namespace combat

int CombatResolver::TerrainClass(int x, int y) const {
    if (game::TowerAt(game_, x, y)) return combat::kCity;
    int terrain = scn::TerrainAt(*game_.map, x, y);
    if (terrain == move::kCity || terrain == move::kSite) return combat::kCity;
    if (terrain == move::kForest) return combat::kWoods;
    if (terrain == move::kHills || terrain == move::kMountains) return combat::kHills;
    return combat::kOpen;
}

combat::Lines CombatResolver::BattleLines(const ArmyStack& stack, int x, int y) const {
    int attacking_side = stack.empty() ? kNone : stack[0]->owner;
    City* city = game_.map->city_tile[y * game_.map->width + x];
    if (city && city->razed) city = nullptr;
    int min_x = city ? city->x : x, min_y = city ? city->y : y;
    int max_x = city ? city->x + 1 : x, max_y = city ? city->y + 1 : y;
    combat::Lines lines;
    bool found_defender = false;
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        Army* army = game_.armies[i];
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
    SortLine(game_, lines.attackers, attacking_side, HeroCarrier(game_, lines.attackers, x, y));
    SortLine(game_, lines.defenders, lines.def_owner, HeroCarrier(game_, lines.defenders, x, y));
    lines.city = city;
    return lines;
}

int CombatResolver::HeroBonus(const ArmyStack& line) const {
    int best = 0, command = 0;
    for (Army* army : line) {
        if (army->type == armytype::kHero) {
            int hero_strength = army->strength + combat::BattleItems(*army);
            best = std::max(best, std::min(9, hero_strength));
            command += combat::CommandItems(*army);
        }
    }
    return combat::kHeroTable[std::max(0, best)] + command;
}

int CombatResolver::Fortify(const ArmyStack& attackers, int x, int y, int cls) const {
    if (cls != combat::kCity) return 0;
    if (LineHas(game_, attackers, combat::kSiege)) return 0;
    int value;
    City* city = game_.map->city_tile[y * game_.map->width + x];
    if (game::TowerAt(game_, x, y))
        value = 1;
    else if (scn::TerrainAt(*game_.map, x, y) == move::kSite)
        value = 2;
    else
        value = city ? city->defence : 0;
    if (city && city->owner_index == kNone) value = value / 2;
    return value;
}

CombatResolver::ModifierPair CombatResolver::Modifiers(const ArmyStack& attackers,
                                                       const ArmyStack& defenders, int x, int y,
                                                       int cls) const {
    int cap = game_.map->combat_cap;
    int atk_hero = LineHas(game_, defenders, combat::kNegateHero) ? 0 : HeroBonus(attackers);
    int atk_stack =
            LineHas(game_, defenders, combat::kNegateOther) ? 0 : StackBonus(game_, attackers, cls);
    int def_hero = LineHas(game_, attackers, combat::kNegateHero) ? 0 : HeroBonus(defenders);
    int def_stack =
            LineHas(game_, attackers, combat::kNegateOther) ? 0 : StackBonus(game_, defenders, cls);
    int attack_mod = std::min(cap, atk_hero + atk_stack) + Subtract(game_, defenders);
    int defend_mod = std::min(cap, def_hero + def_stack + Fortify(attackers, x, y, cls)) +
                     Subtract(game_, attackers);
    return {attack_mod, defend_mod};
}

int CombatResolver::Strength(const Army& army, int mod, int cls, int terrain) const {
    if (army.at_sea && (terrain == move::kWater || terrain == move::kShore)) return 4;
    int strength = army.strength + mod + combat::BattleItems(army) +
                   game_.types.ById(army.type)->SelfBonus(cls);
    return std::min(combat::kMaxStrength, strength);
}

CombatResolver::StrengthMap CombatResolver::StackStrengths(const ArmyStack& armies, int x,
                                                           int y) const {
    int cls = TerrainClass(x, y);
    int total = 0, best_stack_bonus = 0;
    for (Army* army : armies) {
        if (!army->at_sea) {
            if (army->type == armytype::kHero) {
                int hero_strength = std::min(9, army->strength + combat::BattleItems(*army));
                total += combat::kHeroTable[std::max(0, hero_strength)] +
                         combat::CommandItems(*army);
            } else {
                int stack_bonus = game_.types.ById(army->type)->StackBonus(cls);
                if (stack_bonus > best_stack_bonus) {
                    total = total + stack_bonus - best_stack_bonus;
                    best_stack_bonus = stack_bonus;
                }
            }
        }
    }
    total = std::min(total, game_.map->combat_cap);
    StrengthMap strengths;
    for (Army* army : armies) {
        int strength;
        if (army->at_sea)
            strength = 4;
        else if (army->type == armytype::kHero)
            strength = army->strength + combat::BattleItems(*army);
        else
            strength = army->strength + game_.types.ById(army->type)->SelfBonus(cls);
        strengths[army] = std::min(9, strength) + total;
    }
    return strengths;
}

Battle CombatResolver::Resolve(const ArmyStack& attackers, const ArmyStack& defenders, int x,
                               int y) {
    int cls = TerrainClass(x, y);
    int terrain = scn::TerrainAt(*game_.map, x, y);
    auto [attack_mod, defend_mod] = Modifiers(attackers, defenders, x, y, cls);
    int die = game_.map->options.intense_combat != 0 ? combat::kDieIntense : combat::kDie;

    struct Fighter {
        Army* army;
        int strength;
        int hits;
        bool dead = false;
    };
    auto prepare = [&](const ArmyStack& line, int mod) {
        std::vector<Fighter> fighters;
        for (Army* army : line) {
            fighters.push_back(Fighter{army, std::max(1, Strength(*army, mod, cls, terrain)),
                                       combat::kHitPoints - 1});
        }
        return fighters;
    };
    auto attack_line = prepare(attackers, attack_mod);
    auto defend_line = prepare(defenders, defend_mod);
    Battle battle;
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
        int attacker_roll = game_.rng.Dice(1, die, 0), defender_roll = game_.rng.Dice(1, die, 0);
        bool attacker_hits =
                attacker_roll <= attacker.strength && defender_roll > defender.strength;
        bool defender_hits =
                defender_roll <= defender.strength && attacker_roll > attacker.strength;
        if (throws > combat::kStalemate) {
            attacker_hits = false;
            defender_hits = true;
        }
        throws++;

        if (defender_hits && game_.map->options.tutorial != 0 &&
            attacker.army->type == armytype::kHero) {
            Side* side = game_.map->Side(attacker.army->owner == kNone ? 0 : attacker.army->owner);
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
            defenders.empty() ? 0 : Strength(*defenders[0], defend_mod, cls, terrain);
    return battle;
}

CombatResolver::AdviceVerdict CombatResolver::Advise(const ArmyStack& attackers,
                                                     const ArmyStack& defenders, int x, int y) {
    int wins = 0;
    for (int i = 0; i < combat::kAdviceBattles; i++) {
        if (Resolve(attackers, defenders, x, y).won) wins++;
    }
    return {combat::kAdvice[wins / 2], wins};
}

int CombatResolver::Loot(const Side* loser) const {
    if (!loser) return 0;
    int city_count = static_cast<int>(game::SideCities(game_, *loser).size());
    int share = city_count > 1 ? loser->gold / city_count : loser->gold;
    return share / 2;
}

Battle CombatResolver::DecideAttack(const ArmyStack& stack, int x, int y) {
    combat::Lines lines = BattleLines(stack, x, y);
    int attacker_side = stack.empty() ? kNone : stack[0]->owner;
    if (lines.city && attacker_side != kNone) lines.city->attacked |= 1 << attacker_side;
    Battle result = Resolve(lines.attackers, lines.defenders, x, y);
    result.lines.attackers = lines.attackers;
    result.lines.defenders = lines.defenders;
    result.lines.city = lines.city;
    Side* loser = lines.def_owner != kNone ? game_.map->Side(lines.def_owner) : nullptr;
    if (result.won && lines.city && loser) result.loot = Loot(loser);
    for (Army* army : stack) {
        if (army->type == armytype::kHero) {
            result.hero = army;
            break;
        }
    }
    result.fought = Battle::Fought{stack, x, y, lines.def_owner};
    return result;
}

void CombatResolver::ChargeAttack(const ArmyStack& stack, int x, int y) {
    City* city = game::CityAt(game_, x, y);
    int terrain = city && !city->razed ? move::kCity : scn::TerrainAt(*game_.map, x, y);
    int cost = std::max(2, terrain >= 0 && terrain < 12 ? move::kCost[terrain] : 0);
    move::Mode mode = move::StackMode(game_, stack);
    if (mode.mode != move::kFlying && !mode.at_sea &&
        (terrain == move::kWater || terrain == move::kShore)) {
        for (Army* army : stack) {
            bool flight = false;
            if (army->type == armytype::kHero) {
                for (Item* item : army->items) {
                    if (item->type == rules::kItemFlight) flight = true;
                }
            }
            if (!game_.types.ById(army->type)->flies && !flight) {
                army->at_sea = true;
                army->moves = 0;
            }
        }
    }
    for (Army* army : stack) {
        army->moves = std::max(0, army->moves - cost);
        army->target.reset();
    }
}

Army* CombatResolver::AwardMedal(const Battle& result, int side_index) {
    if (side_index == kNone || !result.won) return nullptr;
    const auto& survivors = result.attackers;
    for (Army* army : survivors) {
        if (army->at_sea) return nullptr;
    }
    size_t n_atk = result.lines.attackers.size(), n_def = result.lines.defenders.size();
    bool counted = (n_atk >= 2 && n_def >= 2) ||
                   (game_.turn < 20 && n_def >= 1 && result.def_strength0 > 3);
    if (!counted) return nullptr;
    ArmyStack plain;
    for (Army* army : survivors) {
        if (Ordinary(game_, army)) plain.push_back(army);
    }
    if (plain.empty()) return nullptr;
    int decorated = 0;
    for (Army* army : game_.armies) {
        if (army->owner == side_index && Ordinary(game_, army) && army->medals > 0) decorated++;
    }
    int chance = std::max(12, 40 - 5 * decorated);
    if (!(game_.rng.Dice(1, 100, 0) < chance) || !result.hero) return nullptr;
    Army* chosen = plain[game_.rng.Dice(1, static_cast<int>(plain.size()), -1)];
    if (chosen->medals >= 4) return nullptr;
    chosen->medals++;
    if (chosen->strength > 14)
        chosen->max_moves++;
    else
        chosen->strength = std::min(9, chosen->strength + 1);
    return chosen;
}

Battle& CombatResolver::ApplyAttack(Battle& result) {
    if (!result.fought) return result;
    Battle::Fought fought = *result.fought;
    result.fought.reset();
    const auto& stack = fought.stack;
    int x = fought.x, y = fought.y, def_owner = fought.def_owner;
    const auto& attackers = result.lines.attackers;
    const auto& defenders = result.lines.defenders;
    City* city = result.lines.city;
    City* tile_city = game_.map->city_tile[y * game_.map->width + x];
    const auto& boats = result.boats;

    auto fallen = [&](Army* hero) {
        history::RecordDeed(game_, game_.map->Side(hero->owner == kNone ? 8 : hero->owner),
                            DeedType::kHeroKilled,
                            tile_city ? tile_city->index : history::kKilledInBattle, 0, hero->name);
    };
    int mine = stack.empty() ? kNone : stack[0]->owner;
    if (def_owner >= 0 && def_owner <= 7 && mine >= 0 && mine <= 7) {
        int heroes_lost = 0;
        for (Army* dead_def : result.dead_defenders) {
            if (dead_def->type == armytype::kHero) heroes_lost++;
        }
        Side* def_side = game_.map->Side(def_owner);
        Side* atk_side = game_.map->Side(mine);
        TilePos pos(x, y);
        if (def_side && def_side->computer_player) {
            def_side->computer_player->RecordBattle(
                    game_, SideId(def_owner), SideId(mine), pos, heroes_lost,
                    static_cast<int>(result.dead_defenders.size()),
                    result.won && !defenders.empty(), city != nullptr);
            def_side->computer_player->RecordBattleObservation(game_, SideId(def_owner), pos);
        }
        if (atk_side && atk_side->computer_player) {
            atk_side->computer_player->RecordBattleObservation(game_, SideId(mine), pos);
        }
    }
    for (Army* attacker : result.dead_attackers)
        history::Tally(game_, attacker, def_owner, boats.count(attacker) > 0);
    for (Army* defender : result.dead_defenders)
        history::Tally(game_, defender, mine, boats.count(defender) > 0);

    if (tile_city && result.won && mine != kNone && attackers.size() + defenders.size() >= 8 &&
        defenders.size() >= 4) {
        Side* side = game_.map->Side(mine);
        if (result.hero)
            history::RecordDeed(game_, side, DeedType::kCityWon, history::kWonByName,
                                tile_city->index, result.hero->name);
        else
            history::RecordDeed(game_, side, DeedType::kCityWon, mine, tile_city->index, "");
    }
    for (Army* attacker : result.dead_attackers) {
        if (attacker->type == armytype::kHero) {
            fallen(attacker);
            hero::DropItems(game_, attacker, x, y, mine);
        }
    }
    for (Army* defender : result.dead_defenders) {
        if (defender->type == armytype::kHero) {
            fallen(defender);
            hero::DropItems(game_, defender, x, y, mine);
        }
    }
    hero::BattleExperience(game_, attackers, defenders, result, city != nullptr);
    Side* my_side =
            game_.map->Side(stack.empty() ? 0 : (stack[0]->owner == kNone ? 0 : stack[0]->owner));
    {
        quest::Event event;
        event.stack = attackers;
        event.killed = result.dead_defenders;
        result.quest = quest::Notify(game_, *my_side, "battle", event);
    }

    game_.armies.RemoveAll(result.dead_attackers);
    game_.armies.RemoveAll(result.dead_defenders);
    game::TidyTowers(game_);

    if (result.won && city) {
        Side* winner = game_.map->Side(stack[0]->owner);
        Side* loser = def_owner != kNone ? game_.map->Side(def_owner) : nullptr;
        if (loser) {
            int loot = result.loot;
            winner->gold += loot;
            loser->gold = std::max(0, loser->gold - 2 * loot);
        }
        if (city->owner_index != kNone) {
            city->claim = city->claim == winner->index ? rules::kNeutral : city->owner_index;
        }
        city->attacked = 0;
        city->countdown = 0;
        city->vector_to = kNone;
        for (auto& other_city : game_.map->cities) {
            if (other_city.vector_to == city->index) {
                other_city.vector_to = kNone;
                other_city.producing = kNone;
                other_city.countdown = 0;
            }
        }
        city->owner_index = winner->index;
        scn::SetCityTiles(*game_.map, *city);
        game_.GetPathfinder().Invalidate();
        result.captured = city;
        bool handled =
                winner->computer && winner->computer_player &&
                winner->computer_player->OnQuestCapture(game_, *winner, *city, result.attackers);
        if (!handled && winner->computer) {
            quest::Event event;
            event.city = city;
            event.stack = result.attackers;
            auto quest_res = quest::Notify(game_, *winner, "occupy", event);
            if (quest_res) result.quest = quest_res;
        }
    }

    if (result.won) {
        game_.towers.erase(y * game_.map->width + x);
        int room = rules::kMaxStack - static_cast<int>(game::ArmiesAt(game_, x, y).size());
        for (Army* attacker : result.attackers) {
            if (room <= 0) break;
            game_.armies.Relocate(attacker, TilePos(x, y));
            room--;
        }
        if (game_.map->options.hidden_map != 0 && mine != kNone) {
            bool flying = move::ModeOf(game_, result.attackers) == move::kFlying;
            int found = game::Reveal(game_, mine, x, y, flying);
            if (city) {
                for (int dx = 0; dx <= 1; dx++) {
                    for (int dy = 0; dy <= 1; dy++)
                        found += game::Reveal(game_, mine, city->x + dx, city->y + dy, false);
                }
            }
            if (found > 0) game_.GetPathfinder().Invalidate();
        }
    }
    result.medal = AwardMedal(result, mine);
    return result;
}

Battle CombatResolver::ResolveAttack(const ArmyStack& stack, int x, int y) {
    Battle battle = DecideAttack(stack, x, y);
    ApplyAttack(battle);
    return battle;
}

}  // namespace w2
