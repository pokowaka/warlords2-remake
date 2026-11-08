#include "warlords/ai/groups.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"

namespace w2::ai::groups {

using namespace core;

// roles a rally city may be picked from (DS:0940, DS:094c)
static const std::set<int> kRallyRoles = {5, 8, 6, 4, 14};

static std::vector<City*> Own(const Game& g, const Side& side) {
    return cities::Own(g, side);
}

static City* CityOrNull(Game& g, int i) {
    return i != kNone ? g.map->City(i) : nullptr;
}
static City* CityOrNull(Game& g, const std::map<int, int>& m, int k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : CityOrNull(g, it->second);
}

bool Free(Game& g, Side& side, const City& c, bool members) {
    AIData& d = Data(g, side);
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && grp->rally == c.index) return false;
    }
    int r = Role(d, c);
    if (r == kStop || r == kBuilding) return true;
    return members && r == kMember;
}

bool IsRally(Game& g, Side& side, const City& c) {
    AIData& d = Data(g, side);
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && grp->rally == c.index) return true;
    }
    return false;
}

void Cancel(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    City* r = CityOrNull(g, grp->rally);
    if (r) SetRole(d, *r, kStop);
    for (int k = 4; k >= 1; k--) {
        City* m = CityOrNull(g, grp->members, k);
        if (m) SetRole(d, *m, kStop);
    }
    d.groups[gi] = EmptyGroup();
}

int SackValue(const Game& g, const City& c) {
    int v = 0;
    for (size_t i = 1; i < c.slots.size(); i++)
        v += std::abs(g.types.ById(c.slots[i].type)->price) / 2;
    return v;
}

void Pillage(Game& g, Side& side, City& c) {
    game::Pillage(g, side, c, {});
    if (hooks.on_spoils) hooks.on_spoils(g, side, c, "pillaged");
}

void Sack(Game& g, Side& side, City& c) {
    if (SackValue(g, c) == 0) return Pillage(g, side, c);
    game::Sack(g, side, c, {});
    if (hooks.on_spoils) hooks.on_spoils(g, side, c, "sacked");
}

City* Raze(Game& g, Side& side, City& c, bool force, Sel* sel) {
    if (!force) {
        auto [nb, nd] = FindNeighbours(g, c);
        int n = 0;
        for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
            if (nb[j]->owner_index == side.index && nd[j] < 45) n++;
        }
        if (n > 2) return nullptr;
    }
    if (SackValue(g, c) >= 400) {
        Sack(g, side, c);
        return nullptr;
    }
    game::Raze(g, side, c, sel ? sel->armies : std::vector<Army*>{});
    if (hooks.on_spoils) hooks.on_spoils(g, side, c, "razed");
    if (!sel || sel->armies.empty()) return nullptr;
    Flood flood = FloodFrom(g, side.index, sel->leader->x, sel->leader->y, 15, sel);
    return BestCity(g, *sel, flood, true);
}

bool EarlyVengeance(Game& g, int me, City& c, int was) {
    Side& side = *g.map->Side(me);
    AIData& d = Data(g, side);
    if (side.computer && was != kNone && was != kNeutral && !IsComputer(g, was) && d.early != 0 &&
        g.turn < 10 && d.quest_city != c.index) {
        if (SackValue(g, c) < 200)
            Raze(g, side, c, false);
        else
            Sack(g, side, c);
        return true;
    }
    return false;
}

// What the group will do to the cities it takes (5f19:06a0).
static void RollSpoils(Game& g, Side& side, Group& grp) {
    AIData& d = Data(g, side);
    int bias = d.own * d.per_city;
    if (!side.computer)
        bias += d.bonus_human;
    else if (side.level == 2)
        bias += d.bonus_warlord;
    else if (side.level == 1)
        bias += d.bonus_lord;
    else if (side.level == 0)
        bias += d.bonus_knight;
    grp.flags = 0;
    if (d.raze != 0 && g.rng.Dice(1, 1000, 0) < d.raze + bias) {
        grp.flags = kGfRaze;
        return;
    }
    if (side.gold < 100) bias += d.poor;
    if (d.sack != 0 && g.rng.Dice(1, 1000, 0) < d.sack + bias) {
        grp.flags = kGfSack;
        return;
    }
    if (d.pillage != 0 && g.rng.Dice(1, 1000, 0) < d.pillage + bias) grp.flags = kGfPillage;
}

void Prepare(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int active = 0;
    for (int gi = d.max_groups; gi >= 1; gi--)
        if (d.groups[gi]->active != 0) active++;
    d.min_strength = 0;
    d.fly_cities = 0;
    d.strong_cities = 0;
    d.fast_cities = 0;
    std::vector<int> top;
    for (City* c : Own(g, side)) {
        ClearCflag(d, *c, kCfMovE12);
        ClearCflag(d, *c, kCfFlygroup);
        ClearCflag(d, *c, kCfMovE16);
        if (Role(d, *c) != kRally) {
            auto [good, slot_i] = cities::BuildsWell(g, side, *c);
            if (good) {
                const Slot& slot = c->slots[slot_i];
                top.push_back(slot.strength + (g.types.ById(slot.type)->siege ? 2 : 0));
            }
        }
    }
    LuaSort(top, [](int p, int q) { return p > q; });
    int least = 100;
    for (size_t i = 0; i < std::min<size_t>(8, top.size()); i++)
        if (top[i] < least) least = top[i];
    d.min_strength = std::min(4, least);

    auto strong = [&](const Slot& slot) {
        return g.types.ById(slot.type)->siege || d.min_strength <= slot.strength;
    };

    if (active == 4) {
        for (City* c : Own(g, side)) {
            if (Free(g, side, *c, true) && Cflag(d, *c, kCfFlier)) {
                bool ok = false;
                for (auto& slot : c->slots)
                    if (g.types.ById(slot.type)->flies && slot.strength > 4) ok = true;
                if (ok) {
                    d.fly_cities++;
                    SetCflag(d, *c, kCfFlygroup);
                }
            }
        }
    }
    bool fly_enough = d.fly_cities >= 4;
    int left = fly_enough ? d.fly_cities - 4 : d.fly_cities;
    if (active > 2) {
        for (City* c : Own(g, side)) {
            if (Free(g, side, *c, true)) {
                bool skip = false;
                if (Cflag(d, *c, kCfFlygroup)) {
                    if (left == 0)
                        skip = true;
                    else {
                        ClearCflag(d, *c, kCfFlygroup);
                        left--;
                    }
                }
                if (!skip) {
                    bool ok = false;
                    for (auto& slot : c->slots)
                        if (strong(slot) && slot.move > 15) ok = true;
                    if (ok) {
                        d.strong_cities++;
                        SetCflag(d, *c, kCfMovE16);
                    }
                }
            }
        }
    }
    bool many_strong = d.strong_cities > 7;
    int fast_builders = 0;
    bool strong_enough = d.strong_cities >= 4;
    left = strong_enough ? d.strong_cities - 4 : d.strong_cities;
    if (active > 1) {
        for (City* c : Own(g, side)) {
            if (Free(g, side, *c, true) && !Cflag(d, *c, kCfFlygroup)) {
                bool skip = false;
                if (Cflag(d, *c, kCfMovE16)) {
                    if (left == 0)
                        skip = true;
                    else {
                        ClearCflag(d, *c, kCfMovE16);
                        left--;
                    }
                }
                if (!skip) {
                    bool ok = false;
                    for (auto& slot : c->slots) {
                        if (strong(slot) && slot.move > 11) {
                            fast_builders++;
                            if (!many_strong || slot.move > 15) ok = true;
                        }
                    }
                    if (ok) {
                        d.fast_cities++;
                        SetCflag(d, *c, kCfMovE12);
                    }
                }
            }
        }
    }
    bool fast_enough = d.fast_cities >= 4;
    left = fast_enough ? d.fast_cities - 4 : d.fast_cities;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == side.index && !Cflag(d, c, kCfFlygroup) && !Cflag(d, c, kCfMovE16) &&
            Cflag(d, c, kCfMovE12) && left != 0) {
            ClearCflag(d, c, kCfMovE12);
            left--;
        }
    }
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && gi <= 4) {
            grp->size = 8;
            if (gi == 1) {
                grp->size = fast_builders < 8 ? 8 : 12;
            } else if (gi == 2) {
                grp->flags = Clear(grp->flags, kGfMovE12);
                if (fast_enough) {
                    grp->flags = Set(grp->flags, kGfMovE12);
                    grp->size = 12;
                }
            } else if (gi == 3) {
                grp->flags = Clear(grp->flags, kGfMovE16);
                if (strong_enough) {
                    grp->flags = Set(grp->flags, kGfMovE16);
                    grp->size = 16;
                }
            } else if (gi == 4) {
                grp->flags = Clear(grp->flags, kGfFly);
                if (fly_enough) {
                    grp->flags = Set(grp->flags, kGfFly);
                    grp->size = 12;
                }
            }
        }
    }
}

// Drop staged stacks that are gone or have left the group (563e:1251).
static void CleanStaged(Game& g, Side& side, int gi) {
    auto grp = Data(g, side).groups[gi];
    for (int s = 1; s <= 4; s++) {
        auto it = grp->staged.find(s);
        if (it == grp->staged.end()) continue;
        Army* a = it->second;
        if (a && !(Alive(g, a) && a->owner == side.index && a->ai_group == gi))
            grp->staged.erase(it);
    }
}

// Re-check the plan (563e:041c). Returns the number of targets.
static int CheckPlan(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    int n = 0;
    for (int k = 1; k <= 6; k++) {
        City* t = CityOrNull(g, grp->cities, k);
        if (t) {
            if (d.quest_city == t->index || Owner(*t) != grp->target)
                grp->cities.erase(k);
            else
                n++;
        }
    }
    City* rally = CityOrNull(g, grp->rally);
    if (!rally) return n;
    auto [nb, nd] = FindNeighbours(g, *rally);
    for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
        City* m = nb[j];
        if (Owner(*m) == grp->target && !Cflag(d, *m, kCfUnseen)) {
            bool have = false;
            for (int k = 1; k <= 6; k++)
                if (HasKey(grp->cities, k) && grp->cities[k] == m->index) have = true;
            if (!have) {
                n++;
                int slot = kNone;
                bool worst_set = false;
                int worst = 0;
                for (int k = 6; k >= 1; k--) {
                    if (!HasKey(grp->cities, k)) {
                        slot = k;
                        worst = -1;
                        worst_set = true;
                        break;
                    }
                    if ((worst_set ? worst : 0) < Get(grp->dist, k)) {
                        slot = k;
                        worst = Get(grp->dist, k);
                        worst_set = true;
                    }
                }
                if (slot != kNone && (worst == -1 || nd[j] < worst)) {
                    grp->cities[slot] = m->index;
                    grp->dist[slot] = nd[j];
                }
            }
        }
    }
    return n;
}

// With one target left, add its neighbours of the target side, and move the
// rally to one of the side's own cities next to it (563e:1579).
static void AdjustRally(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    int only = kNone, count = 0;
    for (int k = 1; k <= 6; k++)
        if (HasKey(grp->cities, k)) {
            only = grp->cities[k];
            count++;
        }
    if (count != 1) return;
    City* oc = CityOrNull(g, only);
    if (!oc) return;
    auto nb = FindNeighbours(g, *oc).first;
    City* next = nullptr;
    bool rally_near = false;
    for (size_t j = 0; j < nb.size(); j++) {
        City* m = nb[j];
        if (!Cflag(d, *m, kCfUnseen)) {
            if (Owner(*m) == grp->target) {
                for (int k = 1; k <= 6; k++) {
                    if (!HasKey(grp->cities, k)) {
                        grp->cities[k] = m->index;
                        break;
                    }
                }
            } else if (m->owner_index == side.index) {
                if (m->index == grp->rally) rally_near = true;
                if (!IsRally(g, side, *m) && !next) next = m;
            }
        }
    }
    if (!rally_near && next) grp->rally = next->index;
}

// Move the rally to the side's nearest neighbouring city that is no other
// group's rally (563e:02ed); cancel the group when there is none.
static bool Relocate(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    City* r = CityOrNull(g, grp->rally);
    City* best = nullptr;
    if (r) {
        auto nb = FindNeighbours(g, *r).first;
        int best_d = 1000;
        for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
            City* m = nb[j];
            if (m->owner_index == side.index && !IsRally(g, side, *m)) {
                int dd = Dist(m->x, m->y, r->x, r->y);
                if (dd < best_d) {
                    best = m;
                    best_d = dd;
                }
            }
        }
    }
    if (!best) {
        Cancel(g, side, gi);
        return false;
    }
    grp->rally = best->index;
    return true;
}

// A city the group has taken (563e:134c).
static bool TakeCity(Game& g, Side& side, int gi, City& c) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    if (grp->rally == c.index) return false;
    cities::Garrison(g, side, c, true);
    for (int k = 1; k <= 6; k++) {
        if (HasKey(grp->cities, k) && grp->cities[k] == c.index) {
            grp->cities.erase(k);
            break;
        }
    }
    auto near = [&](const City& x) {
        auto [nb, nd] = FindNeighbours(g, x);
        int n = 0;
        for (size_t j = 0; j < nb.size(); j++) {
            if (Owner(*nb[j]) == grp->target) {
                if (nd[j] < 10) n++;
                if (nd[j] < 20) n++;
                if (nd[j] < 30) n++;
                if (nd[j] < 50) n++;
            }
        }
        return n;
    };
    int here = near(c);
    City* rally = CityOrNull(g, grp->rally);
    int there = rally ? near(*rally) : 0;
    City* pick = there < here ? &c : rally;
    if (pick) {
        bool have = false;
        for (int k = 1; k <= 6; k++)
            if (HasKey(grp->taken, k) && grp->taken[k] == pick->index) have = true;
        if (!have) {
            for (int k = 1; k <= 6; k++) {
                if (!HasKey(grp->taken, k)) {
                    grp->taken[k] = pick->index;
                    break;
                }
            }
        }
    }
    if (here == 0 && there == 0) {
        Cancel(g, side, gi);
        return false;
    }
    grp->rally = pick->index;
    SetRole(d, c, kRally);
    FollowUp(g, side, gi);
    return true;
}

int March(Game& g, Side& side, int gi, std::vector<Army*> list, City* c) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    Army* lead = list.empty() ? nullptr : list[0];
    if (lead && Alive(g, lead) && !Standing(*c)) {
        City* best = nullptr;
        int best_d = 1000;
        for (int k = 1; k <= 6; k++) {
            City* t = CityOrNull(g, grp->cities, k);
            if (t) {
                int dd = Dist(lead->x, lead->y, t->x, t->y);
                if (dd < best_d && Standing(*t)) {
                    best = t;
                    best_d = dd;
                }
            }
        }
        if (best) c = best;
    }
    for (;;) {
        int n = 0;
        for (int k = 1; k <= 6; k++)
            if (HasKey(grp->cities, k)) n++;
        std::vector<Army*> keep;
        for (Army* a : list)
            if (Alive(g, a) && a->owner == side.index) keep.push_back(a);
        list = keep;
        int was = Owner(*c);
        auto sel = Order(g, list, kOrderCity, c->index, 0);
        if (!sel) {
            CleanStaged(g, side, gi);
            return 3;
        }
        int r = MoveTo(g, *sel, sel->leader->target->first, sel->leader->target->second);
        if (r == 1) {
            game::Disband(g, side, sel->armies);
            CleanStaged(g, side, gi);
            return 3;
        }
        CleanStaged(g, side, gi);
        if (c->owner_index != side.index) return 3;
        if (was == side.index) return 3;
        bool capital = side.capital == c;
        int flags = grp->flags;
        if (!capital && n >= 2 && Has(flags, kGfRaze)) {
            City* next_city = Raze(g, side, *c, false, &*sel);
            if (!next_city) return 1;
            c = next_city;
            list = sel->armies;
        } else {
            if (!capital && Has(flags, kGfSack))
                Sack(g, side, *c);
            else if (!capital && Has(flags, kGfPillage))
                Pillage(g, side, *c);
            TakeCity(g, side, gi, *c);
            return 1;
        }
    }
}

// Look for a stack of the target side worth hitting near a staged stack
// (563e:0c32). Returns 2 if it went.
static int HitArmies(Game& g, Side& side, int gi, std::vector<Army*>& list, int target, int range,
                     const Flood& flood, const Sel& sel) {
    Army* lead = list.empty() ? nullptr : list[0];
    if (!lead || lead->at_sea) return 0;
    int bx = kNone, by = kNone, best_odds = 0;
    for (int x = lead->x - range; x <= lead->x + range - 1; x++) {
        for (int y = lead->y - range; y <= lead->y + range - 1; y++) {
            if (x >= 0 && y >= 0 && x < g.map->width && y < g.map->height) {
                auto here = game::ArmiesAt(g, x, y);
                int t = Terrain(g, x, y);
                if (!here.empty() && here[0]->owner == target && t != move::kCity &&
                    t != move::kShore && t != move::kWater &&
                    FloodAt(flood, x, y) <= sel.min_moves - 1 && game::Seen(g, side.index, x, y)) {
                    int count = static_cast<int>(here.size());
                    int strength = 0, heroes = 0;
                    for (Army* a : here) {
                        strength += a->strength;
                        if (IsHero(a)) heroes++;
                    }
                    int o = Odds(g, sel, x, y);
                    if (o > 75 && ((count > 2 && strength > 10) || heroes != 0) && best_odds < o) {
                        bx = x;
                        by = y;
                        best_odds = o;
                    }
                }
            }
        }
    }
    if (bx == kNone) return 0;
    int dest = lead->ai_order == kOrderCity ? lead->ai_dest : kNone;
    auto s = Order(g, list, kOrderRoam, 0, 0x20);
    if (s) {
        s->leader->target = std::make_pair(bx, by);
        MoveTo(g, *s, bx, by);
        for (Army* a : list) {
            if (Alive(g, a) && a->owner == side.index && dest != kNone) {
                a->ai_order = kOrderCity;
                a->ai_dest = dest;
            }
        }
    }
    return 2;
}

// A staged stack's step (563e:0b49).
static int StagedStep(Game& g, Side& side, int gi, std::vector<Army*>& list, int target,
                      const Flood& flood, const Sel& sel) {
    City* nearest = nullptr;
    int nd = 1000;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == target) {
            int dd = std::get<0>(CityDistance(g, flood, c));
            if (dd < 50 && dd < sel.min_moves) {
                int o = Odds(g, sel, c.x, c.y);
                if (o > 75 && dd < nd) {
                    nearest = &c;
                    nd = dd;
                }
            }
        }
    }
    int r = HitArmies(g, side, gi, list, target, 15, flood, sel);
    if (r != 0 && nearest) r = March(g, side, gi, list, nearest);
    return r;
}

// Move the group's staged stacks on (563e:0996).
static void Gather(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    for (int s = 4; s >= 1; s--) {
        bool again = true;
        while (again) {
            again = false;
            CleanStaged(g, side, gi);
            auto it = grp->staged.find(s);
            Army* a = it == grp->staged.end() ? nullptr : it->second;
            if (!a) break;
            auto list = CollectOrdered(g, side.index, a->x, a->y, a->ai_group, a->ai_order, 0);
            if (list.empty()) break;
            Sel sel = Select(g, list);
            Flood flood = FloodFrom(g, side.index, a->x, a->y, 15, &sel);
            std::vector<std::array<int, 3>> was;
            for (Army* b : list) was.push_back({b->x, b->y, b->moves});
            size_t armies = g.armies.size();
            int r = StagedStep(g, side, gi, list, grp->target, flood, sel);
            if (r == 0) {
                Army* last = list.empty() ? nullptr : list.back();
                if (last && Alive(g, last) && last->ai_order == kOrderCity &&
                    CityOrNull(g, last->ai_dest)) {
                    March(g, side, gi, list, CityOrNull(g, last->ai_dest));
                }
            } else if (r == 2) {
                // The original goes round again for as long as a stack is found to
                // strike. One that cannot take a step would be found every time --
                // until the odds' dice fall short, which for a strong stack is
                // never -- so a pass that changed nothing ends it here.
                again = g.armies.size() != armies;
                for (size_t i = 0; i < list.size() && i < was.size(); i++) {
                    Army* b = list[i];
                    if (b->x != was[i][0] || b->y != was[i][1] || b->moves != was[i][2])
                        again = true;
                }
            }
        }
    }
}

// Strike from the rally city (563e:06e9).
static bool Strike(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    City* r = CityOrNull(g, grp->rally);
    if (!r) return false;
    auto list = Collect(g, side.index, r->x + 1, r->y, 0);
    int n = static_cast<int>(list.size());
    if (n == 0) return false;
    Sel sel = Select(g, list);
    City* best = nullptr;
    int best_score = -1;
    for (int k = 1; k <= 6; k++) {
        City* t = CityOrNull(g, grp->cities, k);
        if (t) {
            int dk = HasKey(grp->dist, k) ? grp->dist[k] : -1;
            int turns =
                    dk / std::max(1, sel.min_moves - 2) + 1;  // truncating, as the original divides
            int o = Odds(g, sel, t->x, t->y);
            int score =
                    (turns < 11 ? 10 - turns : 0) + o + Get(grp->bonus, k) + g.rng.Dice(1, 4, 0);
            if (turns == 1) score += 100;
            if ((o > 75 || n > 7) && best_score < score) {
                best = t;
                best_score = score;
            }
        }
    }
    if (!best) return false;
    // the stack's lead -- a hero, else the strongest -- is staged (563e:08b2)
    int slot = kNone;
    for (int s = 4; s >= 1; s--) {
        auto it = grp->staged.find(s);
        if (it == grp->staged.end() || !it->second) {
            slot = s;
            break;
        }
    }
    if (slot != kNone) {
        Army* pick = nullptr;
        int strongest = -1;
        for (int i = static_cast<int>(list.size()) - 1; i >= 0; i--) {
            Army* a = list[i];
            if (IsHero(a)) {
                pick = a;
                break;
            }
            if (strongest < a->strength) {
                pick = a;
                strongest = a->strength;
            }
        }
        grp->staged[slot] = pick;
    }
    for (Army* a : list) a->ai_group = gi;
    March(g, side, gi, list, best);
    return true;
}

void FollowUp(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    for (int k = 1; k <= 6; k++) {
        City* t = CityOrNull(g, grp->taken, k);
        if (t && t->index != grp->rally && t->owner_index == side.index) {
            auto [nb, nd] = FindNeighbours(g, *t);
            int near = 0;
            for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
                int o = nb[j]->owner_index;
                if (o != kNone && o != side.index && nd[j] < 25) near++;
            }
            if (near == 0) {
                cities::Garrison(g, side, *t, true);
                auto list = CollectOrdered(g, side.index, t->x + 1, t->y, 0, grp->size, 0);
                if (list.size() > 3) {
                    auto sel = Order(g, list, kOrderCity, grp->rally, 0);
                    if (sel)
                        MoveTo(g, *sel, sel->leader->target->first, sel->leader->target->second);
                }
            }
        }
    }
}

// Cancel the longest-running group (563e:0607).
static void CancelOldest(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int oldest = kNone, age = -1;
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && age < grp->active) {
            oldest = gi;
            age = grp->active;
        }
    }
    if (oldest != kNone) Cancel(g, side, oldest);
}

// A group's turn (563e:00ca).
static bool RunGroup(Game& g, Side& side, int gi) {
    AIData& d = Data(g, side);
    auto grp = d.groups[gi];
    City* cap = side.capital;
    if (cap && cap->owner_index != kNone && cap->owner_index != side.index) {
        bool going = false;
        for (int k = d.max_groups; k >= 1; k--) {
            auto o = d.groups[k];
            if (o->active != 0 && o->target == cap->owner_index) going = true;
        }
        if (!going) {
            CancelOldest(g, side);
            return false;
        }
    }
    if (CheckPlan(g, side, gi) == 0) {
        Cancel(g, side, gi);
        return false;
    }
    AdjustRally(g, side, gi);
    bool shared = false;
    for (int k = d.max_groups; k >= 1; k--) {
        auto o = d.groups[k];
        if (o->active != 0 && k != gi && o->rally == grp->rally) shared = true;
    }
    City* r = CityOrNull(g, grp->rally);
    if ((!shared && r && r->owner_index == side.index) || Relocate(g, side, gi)) {
        Gather(g, side, gi);
        if (d.groups[gi]->active != 0) {
            City* rc = CityOrNull(g, d.groups[gi]->rally);
            if (rc) cities::Garrison(g, side, *rc, true);
            if (Strike(g, side, gi)) {
                rc = CityOrNull(g, d.groups[gi]->rally);
                if (rc) cities::Garrison(g, side, *rc, true);
            }
            if (d.groups[gi]->active != 0) {
                FollowUp(g, side, gi);
                return true;
            }
        }
    }
    return false;
}

void Assault(Game& g, Side& side) {
    AIData& d = Data(g, side);
    if (d.turns == 0) return;
    Prepare(g, side);
    for (City* c : Own(g, side)) {
        if (Role(d, *c) == kRally) SetRole(d, *c, kStop);
    }
    for (int gi = 1; gi <= d.max_groups; gi++) {
        auto grp = d.groups[gi];
        if (grp->active != 0) {
            City* r = CityOrNull(g, grp->rally);
            if (r) SetRole(d, *r, kRally);
            if (RunGroup(g, side, gi)) d.groups[gi]->active++;
        }
    }
}

// The enemy city with most of its own side's cities round it (5f19:04d2).
static City* Hub(Game& g, int enemy) {
    City* best = nullptr;
    int most = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == enemy) {
            int n = 1;
            auto nb = FindNeighbours(g, c).first;
            for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--)
                if (nb[j]->owner_index == enemy) n++;
            if (n > most || (n == most && g.rng.Dice(1, 2, -1) != 0)) {
                best = &c;
                most = n;
            }
        }
    }
    return best;
}

// Distances over the neighbour graph from a city (5f19:058a).
static void Spread(Game& g, const City& from, std::vector<char>& reached, std::vector<int>& dist) {
    size_t n = g.map->cities.size();
    reached.assign(n, 0);
    dist.assign(n, 0);
    std::vector<char> done(n, 0);
    reached[from.index] = 1;
    dist[from.index] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = static_cast<int>(n) - 1; i >= 0; i--) {
            City& c = g.map->cities[i];
            if (reached[c.index] && !done[c.index]) {
                changed = true;
                done[c.index] = 1;
                auto [nb, nd] = FindNeighbours(g, c);
                for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
                    City* m = nb[j];
                    int nd2 = dist[c.index] + nd[j];
                    if (!reached[m->index]) {
                        reached[m->index] = 1;
                        dist[m->index] = nd2;
                    } else if (nd2 < dist[m->index]) {
                        dist[m->index] = nd2;
                    }
                }
            }
        }
    }
}

// Plan a group against a side (5f19:01bb); the rally city, or null.
static City* Plan(Game& g, Side& side, Group& grp, int enemy) {
    AIData& d = Data(g, side);
    grp.cities.clear();
    grp.dist.clear();
    City* h = Hub(g, enemy);
    if (!h) return nullptr;
    std::vector<char> reached;
    std::vector<int> dist;
    Spread(g, *h, reached, dist);
    City* rally = nullptr;
    long best = 1000000000;
    for (City* c : Own(g, side)) {
        if (kRallyRoles.count(Role(d, *c)) && reached[c->index] && dist[c->index] < best) {
            rally = c;
            best = dist[c->index];
        }
    }
    if (!rally) {
        if (!Cflag(d, *h, kCfUnseen)) {
            long best_d = 1000000000;
            for (City* c : Own(g, side)) {
                if (kRallyRoles.count(Role(d, *c))) {
                    int dd = core::Dist(c->x, c->y, h->x, h->y);
                    if (dd < best_d) {
                        rally = c;
                        best_d = dd;
                    }
                }
            }
            if (rally) {
                grp.cities[1] = h->index;
                grp.dist[1] = 100;
            }
        }
        return rally;
    }
    Spread(g, *rally, reached, dist);
    int n = 0;
    for (int k = 1; k <= 6; k++) {
        City* pick = nullptr;
        long pd = 1000000000;
        for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
            City& c = g.map->cities[i];
            if (c.owner_index == enemy && !Cflag(d, c, kCfUnseen) && reached[c.index] &&
                dist[c.index] < pd) {
                pick = &c;
                pd = dist[c.index];
            }
        }
        if (!pick) break;
        grp.cities[k] = pick->index;
        grp.dist[k] = static_cast<int>(pd);
        reached[pick->index] = 0;
        n++;
    }
    if (n == 0 && !Cflag(d, *h, kCfUnseen)) {
        grp.cities[1] = h->index;
        grp.dist[1] = 100;
        n = 1;
    }
    if (n == 0) return nullptr;
    return rally;
}

int PickEnemy(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int me = side.index;
    City* cap = side.capital;
    int cap_owner = cap ? Owner(*cap) : kNeutral;
    int held[8], score[8], count[8], cap_held[8], from_them[8], targeted[8], rnd[8];
    for (int s = 0; s < 8; s++) {
        held[s] = 0;
        score[s] = 0;
        count[s] = 0;
        cap_held[s] = 0;
        from_them[s] = 0;
        targeted[s] = 0;
        rnd[s] = g.rng.Dice(1, 10, 0);
    }
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        int o = c.owner_index;
        if (o != kNone) {
            count[o]++;
            if (!Cflag(d, c, kCfUnseen)) held[o]++;
            if (o == me && c.claim != kNone && c.claim != kNeutral && c.claim >= 0 && c.claim < 8)
                from_them[c.claim]++;
        }
    }
    int n_groups = 0;
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && grp->target != kNone) {
            if (grp->target >= 0 && grp->target < 8) targeted[grp->target]++;
            n_groups++;
        }
    }
    if (cap_owner != kNeutral && cap_owner != me) score[cap_owner] += 20;
    int others = 0;
    for (int s = 7; s >= 0; s--) {
        if (count[s] != 0) {
            if (s != me) others++;
            City* theirs = g.map->sides[s].capital;
            if (theirs && theirs->owner_index == me) cap_held[s] = 1;
        }
    }
    int constant;
    if (!side.computer)
        constant = d.die_human;
    else if (side.level == 2)
        constant = d.die_warlord;
    else if (side.level == 1)
        constant = d.die_lord;
    else
        constant = d.die_knight;
    for (int s = 7; s >= 0; s--) {
        const Side& o = g.map->sides[s];
        score[s] += rnd[s] + cap_held[s] * 15 + from_them[s] * 4 + d.heroes_killed[s] * 4 +
                    d.armies_killed[s] + d.battles[s] + d.lost[s] * 2 + d.city_battles[s] * 2 +
                    d.cities_lost[s] * 2 + constant;
        score[s] += std::abs(side.diplo_score - o.diplo_score) / 8;
        score[s] += std::abs(count[me] - count[s]) / 4;
    }
    bool proposing_war = false;
    for (int s = 7; s >= 0; s--) {
        if (InPlay(g, s) && s != me && Proposal(g, me, s) == 2) proposing_war = true;
    }
    if (g.greatest && side.computer) {
        auto fights = FightingHumans(g);
        for (int s = 7; s >= 0; s--) {
            if (InPlay(g, s) && IsComputer(g, s) && (fights[me] || fights[s])) score[s] = 0;
        }
    }
    int early = g.map->options.quick_start != 0 ? 4 : 8;
    for (int s = 7; s >= 0; s--) {
        int fought = d.battles[s] + d.city_battles[s];
        if (diplo::Spared(g, side, s)) score[s] = 0;
        if (proposing_war && Proposal(g, me, s) == 0) score[s] = 0;
        if (n_groups == 0 && held[s] == 0) score[s] = 0;
        if (g.turn < early && fought == 0) score[s] = 0;
        if (others > 1 && count[s] / 4 < targeted[s]) score[s] = 0;
    }
    score[me] = 0;
    int pick = kNone, top = 0;
    for (int s = 7; s >= 0; s--) {
        if (count[s] != 0 && top < score[s]) {
            pick = s;
            top = score[s];
        }
    }
    if (cap_owner != kNeutral && cap_owner != me) {
        // 5f19:1393 compares the capital's holder with the groups-per-side
        // counts, not with the groups' targets: kept as it is
        bool found = false;
        for (int gi = d.max_groups - 1; gi >= 0; gi--)
            if (gi < 8 && cap_owner == targeted[gi]) found = true;
        if (!found) pick = cap_owner;
    }
    if (pick != kNone && held[pick] == 0) pick = kNone;
    return pick;
}

std::array<bool, 8> FightingHumans(const Game& g) {
    std::array<bool, 8> out{};
    for (int s = 0; s < 8; s++) {
        if (InPlay(g, s) && IsComputer(g, s)) {
            for (int h = 0; h < 8; h++) {
                if (InPlay(g, h) && !IsComputer(g, h) && State(g, s, h) == 2) out[s] = true;
            }
        }
    }
    return out;
}

void AssaultXx(Game& g, Side& side) {
    AIData& d = Data(g, side);
    if (d.turns == 0) return;
    int quiet = 0;
    for (City* c : Own(g, side)) {
        int r = Role(d, *c);
        if (r == kBuilding || r == kStop) quiet++;
    }
    int active = 0;
    for (int gi = 1; gi <= kMaxGroups; gi++)
        if (d.groups[gi]->active != 0) active++;
    if (!(active < d.max_groups && quiet != 0 && (active == 0 || quiet > 2))) return;
    int slot = kNone;
    for (int gi = d.max_groups; gi >= 1; gi--)
        if (d.groups[gi]->active == 0) slot = gi;
    if (slot == kNone) return;
    d.groups[slot] = EmptyGroup();
    int enemy = PickEnemy(g, side);
    if (enemy == kNone) return;
    auto grp = d.groups[slot];
    City* rally = Plan(g, side, *grp, enemy);
    if (!rally) {
        d.groups[slot] = EmptyGroup();
        return;
    }
    grp->active = 1;
    grp->target = enemy;
    grp->rally = rally->index;
    SetRole(d, *rally, kRally);
    RollSpoils(g, side, *grp);
    Propose(g, side.index, enemy, 2);
    // the rally city is no group's member any more (5f19:083e)
    for (int gi = d.max_groups; gi >= 1; gi--) {
        auto o = d.groups[gi];
        if (o->active != 0) {
            for (int k = 1; k <= 4; k++) {
                if (HasKey(o->members, k) && o->members[k] == rally->index) {
                    o->members.erase(k);
                    rally->vector_to = kNone;
                }
            }
        }
    }
    // each target's bonus: its own side's cities round it (5f19:09ca)
    for (int k = 1; k <= 6; k++) {
        City* t = CityOrNull(g, grp->cities, k);
        if (t) {
            int n = 0;
            for (City* m : FindNeighbours(g, *t).first)
                if (m->owner_index == enemy) n++;
            grp->bonus[k] = n;
        }
    }
    Prepare(g, side);
}

}  // namespace w2::ai::groups
