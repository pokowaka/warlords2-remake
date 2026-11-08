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

// ARMYTYPE offsets
static const int kBonusSelf = 32, kBonusStack = 40;  // + 2*class
static const int kSubtract = 50, kAbility = 52;

int TerrainClass(const Game& g, int x, int y) {
    if (game::TowerAt(g, x, y)) return kCity;
    int t = scn::TerrainAt(*g.map, x, y);
    if (t == move::kCity || t == move::kSite) return kCity;
    if (t == move::kForest) return kWoods;
    if (t == move::kHills || t == move::kMountains) return kHills;
    return kOpen;
}

namespace {
// On water, shore and mountains a side holding a hero sends one flier to the
// back, keeping the hero's carrier alive longest: the first flier in the
// order the line was gathered that is not at sea gets 80 on its rank
// (6a89:008b, before the sort). Returns the army, or null.
const Army* HeroCarrier(const Game& g, const std::vector<Army*>& line, int x, int y) {
    int t = scn::TerrainAt(*g.map, x, y);
    if (t != move::kWater && t != move::kShore && t != move::kMountains) return nullptr;
    if (std::none_of(line.begin(), line.end(), [](Army* a) { return a->type == armytype::kHero; }))
        return nullptr;
    for (Army* a : line)
        if (!a->at_sea && g.types.ById(a->type)->flies) return a;
    return nullptr;
}

// Sort a line by its owner's fight order, lowest first, ties keeping the
// order it was gathered in (an insertion sort, 6a89:05f1).
void SortLine(const Game& g, std::vector<Army*>& line, int owner_index, const Army* carrier) {
    const auto& row = g.map->fight_order[owner_index == kNone ? 8 : owner_index];
    std::map<const Army*, std::pair<int, int>> key;
    for (size_t i = 0; i < line.size(); i++) {
        key[line[i]] = {row[line[i]->type] + (line[i] == carrier ? 80 : 0), static_cast<int>(i)};
    }
    LuaSort(line, [&](Army* p, Army* q) {
        const auto& kp = key[p];
        const auto& kq = key[q];
        if (kp.first != kq.first) return kp.first < kq.first;
        return kp.second < kq.second;
    });
}

int ItemTotal(const Army& army, int item_type) {
    int total = 0;
    for (Item* it : army.items)
        if (it->type == item_type) total += it->value;
    return total;
}

bool LineHas(const Game& g, const std::vector<Army*>& line, int ability) {
    for (Army* a : line)
        if (g.types.ById(a->type)->bonus[kAbility] == ability) return true;
    return false;
}

int StackBonus(const Game& g, const std::vector<Army*>& line, int cls) {
    int best = 0;
    for (Army* a : line) best = std::max(best, g.types.ById(a->type)->bonus[kBonusStack + 2 * cls]);
    return best;
}

int Subtract(const Game& g, const std::vector<Army*>& line) {
    int least = 0;
    for (Army* a : line) least = std::min(least, g.types.ById(a->type)->bonus[kSubtract]);
    return least;
}
}  // namespace

Lines BattleLines(const Game& g, const std::vector<Army*>& stack, int x, int y) {
    int attacker = stack.empty() ? kNone : stack[0]->owner;
    // only a standing city is fought for as one (its terrain is 10, which
    // combat_setup and after_battle test); ruins are open ground
    City* city = g.map->city_tile[y * g.map->width + x];
    if (city && city->razed) city = nullptr;
    int x0 = city ? city->x : x, y0 = city ? city->y : y;
    int x1 = city ? city->x + 1 : x, y1 = city ? city->y + 1 : y;
    Lines out;
    bool any = false;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (!a->transit && a->owner != attacker && a->x >= x0 && a->x <= x1 && a->y >= y0 &&
            a->y <= y1) {
            out.defenders.push_back(a);
            if (!any) {
                out.def_owner = a->owner;
                any = true;
            }
        }
    }
    out.attackers = stack;
    SortLine(g, out.attackers, attacker, HeroCarrier(g, out.attackers, x, y));
    SortLine(g, out.defenders, out.def_owner, HeroCarrier(g, out.defenders, x, y));
    out.city = city;
    return out;
}

int BattleItems(const Army& army) {
    return ItemTotal(army, rules::kItemBattle);
}

int CommandItems(const Army& army) {
    int n = ItemTotal(army, rules::kItemCommand);
    for (Item* it : army.items)
        if (it->type == rules::kItemStandard) n++;
    return n;
}

int HeroBonus(const Game& g, const std::vector<Army*>& line) {
    int best = 0, command = 0;
    for (Army* a : line) {
        if (a->type == armytype::kHero) {
            int str = a->strength + BattleItems(*a);
            best = std::max(best, std::min(9, str));
            command += CommandItems(*a);
        }
    }
    return kHeroTable[std::max(0, best)] + command;
}

int Fortify(const Game& g, const std::vector<Army*>& attackers, int x, int y, int cls) {
    if (cls != kCity) return 0;
    if (LineHas(g, attackers, kSiege)) return 0;
    int value;
    City* c = g.map->city_tile[y * g.map->width + x];
    if (game::TowerAt(g, x, y))
        value = 1;
    else if (scn::TerrainAt(*g.map, x, y) == move::kSite)
        value = 2;
    else
        value = c ? c->defence : 0;
    if (c && c->owner_index == kNone) value = value / 2;  // neutral
    return value;
}

std::pair<int, int> Modifiers(const Game& g, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls) {
    int cap = g.map->combat_cap;
    int atk_hero = LineHas(g, defenders, kNegateHero) ? 0 : HeroBonus(g, attackers);
    int atk_stack = LineHas(g, defenders, kNegateOther) ? 0 : StackBonus(g, attackers, cls);
    int def_hero = LineHas(g, attackers, kNegateHero) ? 0 : HeroBonus(g, defenders);
    int def_stack = LineHas(g, attackers, kNegateOther) ? 0 : StackBonus(g, defenders, cls);
    int attack_mod = std::min(cap, atk_hero + atk_stack) + Subtract(g, defenders);
    int defend_mod = std::min(cap, def_hero + def_stack + Fortify(g, attackers, x, y, cls)) +
                     Subtract(g, attackers);
    return {attack_mod, defend_mod};
}

int Strength(const Game& g, const Army& army, int mod, int cls, int terrain) {
    if (army.at_sea && (terrain == move::kWater || terrain == move::kShore)) return 4;
    int s = army.strength + mod + BattleItems(army) +
            g.types.ById(army.type)->bonus[kBonusSelf + 2 * cls];
    return std::min(kMaxStrength, s);
}

std::map<const Army*, int> StackStrengths(const Game& g, const std::vector<Army*>& armies, int x,
                                          int y) {
    int cls = TerrainClass(g, x, y);
    int total = 0, best = 0;
    for (Army* a : armies) {
        if (!a->at_sea) {
            if (a->type == armytype::kHero) {
                int s = std::min(9, a->strength + BattleItems(*a));
                total += kHeroTable[std::max(0, s)] + CommandItems(*a);
            } else {
                int v = g.types.ById(a->type)->bonus[kBonusStack + 2 * cls];
                if (v > best) {
                    total = total + v - best;
                    best = v;
                }
            }
        }
    }
    total = std::min(total, g.map->combat_cap);
    std::map<const Army*, int> out;
    for (Army* a : armies) {
        int s;
        if (a->at_sea)
            s = 4;
        else if (a->type == armytype::kHero)
            s = a->strength + BattleItems(*a);
        else
            s = a->strength + g.types.ById(a->type)->bonus[kBonusSelf + 2 * cls];
        out[a] = std::min(9, s) + total;
    }
    return out;
}

Battle Resolve(Game& g, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y) {
    int cls = TerrainClass(g, x, y);
    int terrain = scn::TerrainAt(*g.map, x, y);
    auto [attack_mod, defend_mod] = Modifiers(g, attackers, defenders, x, y, cls);
    int die = g.map->options.intense_combat != 0 ? kDieIntense : kDie;

    struct E {
        Army* army;
        int strength;
        int hits;
        bool dead = false;
    };
    auto prepare = [&](const std::vector<Army*>& line, int mod) {
        std::vector<E> out;
        for (Army* a : line)
            out.push_back(E{a, std::max(1, Strength(g, *a, mod, cls, terrain)), kHitPoints - 1});
        return out;
    };
    auto atk = prepare(attackers, attack_mod), def = prepare(defenders, defend_mod);
    Battle b;
    // an army at sea on water or shore fights as a boat, strength 4
    // (DS:029c / DS:02a4); Triumphs counts it a navy
    for (const auto* line : {&attackers, &defenders}) {
        for (Army* a : *line) {
            if (a->at_sea && (terrain == move::kWater || terrain == move::kShore))
                b.boats.insert(a);
        }
    }
    size_t ai = 0, di = 0;
    int throws = 0;
    while (ai < atk.size() && di < def.size()) {
        E& a = atk[ai];
        E& d = def[di];
        int ra = g.rng.Dice(1, die, 0), rd = g.rng.Dice(1, die, 0);
        bool a_hits = ra <= a.strength && rd > d.strength;
        bool d_hits = rd <= d.strength && ra > a.strength;
        // past 10,000 throws in one duel the attacker takes the hit; the count
        // starts again with each army that falls (67cc:08a6)
        if (throws > kStalemate) {
            a_hits = false;
            d_hits = true;
        }
        throws++;

        // Tutorial: a human player's hero cannot die attacking a neutral defender.
        if (d_hits && g.map->options.tutorial != 0 && a.army->type == armytype::kHero) {
            Side* s = g.map->Side(a.army->owner == kNone ? 0 : a.army->owner);
            if (!(s && s->computer) && d.army->owner == kNone) d_hits = false;
        }
        if (a_hits) {
            d.hits--;
            if (d.hits < 0) {
                d.dead = true;
                b.log.push_back(0);
                di++;
                throws = 0;
            }
        } else if (d_hits) {
            a.hits--;
            if (a.hits < 0) {
                a.dead = true;
                b.log.push_back(1);
                ai++;
                throws = 0;
            }
        }
    }
    for (auto& e : atk) (e.dead ? b.dead_attackers : b.attackers).push_back(e.army);
    for (auto& e : def) (e.dead ? b.dead_defenders : b.defenders).push_back(e.army);
    for (Army* a : b.dead_attackers) b.dead_by_army.insert(a);
    for (Army* a : b.dead_defenders) b.dead_by_army.insert(a);
    b.won = b.defenders.empty();
    b.attack_mod = attack_mod;
    b.defend_mod = defend_mod;
    b.cls = cls;
    b.def_strength0 = defenders.empty() ? 0 : Strength(g, *defenders[0], defend_mod, cls, terrain);
    return b;
}

std::pair<std::string, int> Advise(Game& g, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y) {
    int wins = 0;
    for (int i = 0; i < kAdviceBattles; i++) {
        if (Resolve(g, attackers, defenders, x, y).won) wins++;
    }
    return {kAdvice[wins / 2], wins};
}

}  // namespace w2::combat
