#include "warlords/combat.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2::combat {

const char* const CLASS_NAMES[4] = {"city", "open", "woods", "hills"};
const char* const ADVICE[10] = {
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

int terrainClass(const Game& g, int x, int y) {
    if (game::towerAt(g, x, y)) return CITY;
    int t = scn::terrainAt(*g.map, x, y);
    if (t == move::CITY || t == move::SITE) return CITY;
    if (t == move::FOREST) return WOODS;
    if (t == move::HILLS || t == move::MOUNTAINS) return HILLS;
    return OPEN;
}

namespace {
// On water, shore and mountains a side holding a hero sends one flier to the
// back, keeping the hero's carrier alive longest: the first flier in the
// order the line was gathered that is not at sea gets 80 on its rank
// (6a89:008b, before the sort). Returns the army, or null.
const Army* HeroCarrier(const Game& g, const std::vector<Army*>& line, int x, int y) {
    int t = scn::terrainAt(*g.map, x, y);
    if (t != move::WATER && t != move::SHORE && t != move::MOUNTAINS) return nullptr;
    if (std::none_of(line.begin(), line.end(), [](Army* a) { return a->type == armytype::HERO; }))
        return nullptr;
    for (Army* a : line)
        if (!a->atSea && g.types.byId(a->type)->flies) return a;
    return nullptr;
}

// Sort a line by its owner's fight order, lowest first, ties keeping the
// order it was gathered in (an insertion sort, 6a89:05f1).
void SortLine(const Game& g, std::vector<Army*>& line, int owner_index, const Army* carrier) {
    const auto& row = g.map->fightOrder[owner_index == NONE ? 8 : owner_index];
    std::map<const Army*, std::pair<int, int>> key;
    for (size_t i = 0; i < line.size(); i++) {
        key[line[i]] = {row[line[i]->type] + (line[i] == carrier ? 80 : 0), static_cast<int>(i)};
    }
    luaSort(line, [&](Army* p, Army* q) {
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
        if (g.types.byId(a->type)->bonus[kAbility] == ability) return true;
    return false;
}

int StackBonus(const Game& g, const std::vector<Army*>& line, int cls) {
    int best = 0;
    for (Army* a : line) best = std::max(best, g.types.byId(a->type)->bonus[kBonusStack + 2 * cls]);
    return best;
}

int Subtract(const Game& g, const std::vector<Army*>& line) {
    int least = 0;
    for (Army* a : line) least = std::min(least, g.types.byId(a->type)->bonus[kSubtract]);
    return least;
}
}  // namespace

Lines lines(const Game& g, const std::vector<Army*>& stack, int x, int y) {
    int attacker = stack.empty() ? NONE : stack[0]->owner;
    // only a standing city is fought for as one (its terrain is 10, which
    // combat_setup and after_battle test); ruins are open ground
    City* city = g.map->cityTile[y * g.map->width + x];
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
                out.defOwner = a->owner;
                any = true;
            }
        }
    }
    out.attackers = stack;
    SortLine(g, out.attackers, attacker, HeroCarrier(g, out.attackers, x, y));
    SortLine(g, out.defenders, out.defOwner, HeroCarrier(g, out.defenders, x, y));
    out.city = city;
    return out;
}

int battleItems(const Army& army) {
    return ItemTotal(army, rules::ITEM_BATTLE);
}

int commandItems(const Army& army) {
    int n = ItemTotal(army, rules::ITEM_COMMAND);
    for (Item* it : army.items)
        if (it->type == rules::ITEM_STANDARD) n++;
    return n;
}

int heroBonus(const Game& g, const std::vector<Army*>& line) {
    int best = 0, command = 0;
    for (Army* a : line) {
        if (a->type == armytype::HERO) {
            int str = a->strength + battleItems(*a);
            best = std::max(best, std::min(9, str));
            command += commandItems(*a);
        }
    }
    return HERO_TABLE[std::max(0, best)] + command;
}

int fortify(const Game& g, const std::vector<Army*>& attackers, int x, int y, int cls) {
    if (cls != CITY) return 0;
    if (LineHas(g, attackers, SIEGE)) return 0;
    int value;
    City* c = g.map->cityTile[y * g.map->width + x];
    if (game::towerAt(g, x, y))
        value = 1;
    else if (scn::terrainAt(*g.map, x, y) == move::SITE)
        value = 2;
    else
        value = c ? c->defence : 0;
    if (c && c->ownerIndex == NONE) value = value / 2;  // neutral
    return value;
}

std::pair<int, int> modifiers(const Game& g, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls) {
    int cap = g.map->combatCap;
    int atk_hero = LineHas(g, defenders, NEGATE_HERO) ? 0 : heroBonus(g, attackers);
    int atk_stack = LineHas(g, defenders, NEGATE_OTHER) ? 0 : StackBonus(g, attackers, cls);
    int def_hero = LineHas(g, attackers, NEGATE_HERO) ? 0 : heroBonus(g, defenders);
    int def_stack = LineHas(g, attackers, NEGATE_OTHER) ? 0 : StackBonus(g, defenders, cls);
    int attack_mod = std::min(cap, atk_hero + atk_stack) + Subtract(g, defenders);
    int defend_mod = std::min(cap, def_hero + def_stack + fortify(g, attackers, x, y, cls)) +
                     Subtract(g, attackers);
    return {attack_mod, defend_mod};
}

int strength(const Game& g, const Army& army, int mod, int cls, int terrain) {
    if (army.atSea && (terrain == move::WATER || terrain == move::SHORE)) return 4;
    int s = army.strength + mod + battleItems(army) +
            g.types.byId(army.type)->bonus[kBonusSelf + 2 * cls];
    return std::min(MAX_STRENGTH, s);
}

std::map<const Army*, int> stackStrengths(const Game& g, const std::vector<Army*>& armies, int x,
                                          int y) {
    int cls = terrainClass(g, x, y);
    int total = 0, best = 0;
    for (Army* a : armies) {
        if (!a->atSea) {
            if (a->type == armytype::HERO) {
                int s = std::min(9, a->strength + battleItems(*a));
                total += HERO_TABLE[std::max(0, s)] + commandItems(*a);
            } else {
                int v = g.types.byId(a->type)->bonus[kBonusStack + 2 * cls];
                if (v > best) {
                    total = total + v - best;
                    best = v;
                }
            }
        }
    }
    total = std::min(total, g.map->combatCap);
    std::map<const Army*, int> out;
    for (Army* a : armies) {
        int s;
        if (a->atSea)
            s = 4;
        else if (a->type == armytype::HERO)
            s = a->strength + battleItems(*a);
        else
            s = a->strength + g.types.byId(a->type)->bonus[kBonusSelf + 2 * cls];
        out[a] = std::min(9, s) + total;
    }
    return out;
}

Battle resolve(Game& g, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y) {
    int cls = terrainClass(g, x, y);
    int terrain = scn::terrainAt(*g.map, x, y);
    auto [attack_mod, defend_mod] = modifiers(g, attackers, defenders, x, y, cls);
    int die = g.map->options.intenseCombat != 0 ? DIE_INTENSE : DIE;

    struct E {
        Army* army;
        int strength;
        int hits;
        bool dead = false;
    };
    auto prepare = [&](const std::vector<Army*>& line, int mod) {
        std::vector<E> out;
        for (Army* a : line)
            out.push_back(E{a, std::max(1, strength(g, *a, mod, cls, terrain)), HIT_POINTS - 1});
        return out;
    };
    auto atk = prepare(attackers, attack_mod), def = prepare(defenders, defend_mod);
    Battle b;
    // an army at sea on water or shore fights as a boat, strength 4
    // (DS:029c / DS:02a4); Triumphs counts it a navy
    for (const auto* line : {&attackers, &defenders}) {
        for (Army* a : *line) {
            if (a->atSea && (terrain == move::WATER || terrain == move::SHORE)) b.boats.insert(a);
        }
    }
    size_t ai = 0, di = 0;
    int throws = 0;
    while (ai < atk.size() && di < def.size()) {
        E& a = atk[ai];
        E& d = def[di];
        int ra = g.rng.dice(1, die, 0), rd = g.rng.dice(1, die, 0);
        bool a_hits = ra <= a.strength && rd > d.strength;
        bool d_hits = rd <= d.strength && ra > a.strength;
        // past 10,000 throws in one duel the attacker takes the hit; the count
        // starts again with each army that falls (67cc:08a6)
        if (throws > STALEMATE) {
            a_hits = false;
            d_hits = true;
        }
        throws++;

        // Tutorial: a human player's hero cannot die attacking a neutral defender.
        if (d_hits && g.map->options.tutorial != 0 && a.army->type == armytype::HERO) {
            Side* s = g.map->side(a.army->owner == NONE ? 0 : a.army->owner);
            if (!(s && s->computer) && d.army->owner == NONE) d_hits = false;
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
    for (auto& e : atk) (e.dead ? b.deadAttackers : b.attackers).push_back(e.army);
    for (auto& e : def) (e.dead ? b.deadDefenders : b.defenders).push_back(e.army);
    for (Army* a : b.deadAttackers) b.deadByArmy.insert(a);
    for (Army* a : b.deadDefenders) b.deadByArmy.insert(a);
    b.won = b.defenders.empty();
    b.attackMod = attack_mod;
    b.defendMod = defend_mod;
    b.cls = cls;
    b.defStrength0 = defenders.empty() ? 0 : strength(g, *defenders[0], defend_mod, cls, terrain);
    return b;
}

std::pair<std::string, int> advise(Game& g, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y) {
    int wins = 0;
    for (int i = 0; i < ADVICE_BATTLES; i++) {
        if (resolve(g, attackers, defenders, x, y).won) wins++;
    }
    return {ADVICE[wins / 2], wins};
}

}  // namespace w2::combat
