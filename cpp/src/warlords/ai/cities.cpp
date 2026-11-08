#include "warlords/ai/cities.hpp"

#include <algorithm>
#include <set>

#include "util/util.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"

namespace w2::ai::cities {

using namespace core;

// garrison wanted on the first tile, by how many armies the city holds
// (DS:0824), 8 from thirteen up
static const int kKeep[13] = {0, 0, 0, 0, 3, 3, 4, 5, 5, 6, 7, 7, 7};

// the city's four tiles in the order the garrison fills them (DS:0814/081c)
static const int kTileDx[4] = {1, 0, 0, 1};
static const int kTileDy[4] = {0, 0, 1, 1};

// the roles that make a city try a quick attack (DS:0932)
static const std::set<int> kQuickRoles = {5, 8, 6, 4, 14, 7};

std::vector<City*> Own(const Game& g, const Side& side) {
    std::vector<City*> out;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == side.index) out.push_back(&c);
    }
    return out;
}

std::vector<NeutralNeighbour> NeutralNeighbours(Game& g, Side& side, const City& c) {
    AIData& d = Data(g, side);
    auto [nb, nd] = FindNeighbours(g, c);
    std::vector<NeutralNeighbour> out;
    for (size_t j = 0; j < nb.size(); j++) {
        City* n = nb[j];
        if (n->index != d.quest_city && Standing(*n) && n->owner_index == kNone &&
            (out.empty() || j < 2 || nd[j] < 20)) {
            out.push_back(NeutralNeighbour{n, nd[j], static_cast<int>(j) + 1});
        }
    }
    return out;
}

int NearestArmy(const Game& g, const Side& side, int x, int y, int of) {
    int best = 1000;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->owner != kNone && !a->transit) {
            bool ok = of == kNone ? a->owner != side.index : a->owner == of;
            if (ok && Explored(g, side.index, a->x, a->y)) {
                int dd = Dist(x, y, a->x, a->y);
                if (dd < best) best = dd;
            }
        }
    }
    return best;
}

static int SlotStrength(const Game& g, const Slot& slot) {
    int s = slot.strength;
    if (g.types.ById(slot.type)->siege) s += 2;
    return s;
}

std::pair<bool, int> BuildsWell(Game& g, Side& side, const City& c) {
    AIData& d = Data(g, side);
    int slot = rules::BestSlot(c.slots, 3, g.types, side.enhanced);
    if (slot == kNone) return {false, kNone};
    int s = SlotStrength(g, c.slots[slot]);
    if ((d.own < 12 || s > 2) && (d.own < 8 || s > 1)) return {true, slot};
    return {false, slot};
}

// The slot to buy over (623c:1ae9), from 0: the first empty one, else the
// weakest under 10. NONE when all four are full of strong types.
static int SlotToReplace(const Game& g, const City& c) {
    if (static_cast<int>(c.slots.size()) < rules::kProductionSlots)
        return static_cast<int>(c.slots.size());
    int best = kNone, least = 10;
    for (size_t i = 0; i < c.slots.size(); i++) {
        int s = SlotStrength(g, c.slots[i]);
        if (s < least) {
            best = static_cast<int>(i);
            least = s;
        }
    }
    return best;
}

bool Building(const City& c) {
    if (c.producing == kNone) return false;
    if (c.producing >= static_cast<int>(c.slots.size())) return false;
    const Slot& slot = c.slots[c.producing];
    return c.countdown != 0 && c.countdown != slot.time;
}

// set_city_production (623c:0e5a): refused after turn 5 when the side has
// less than the type's cost + 30 in gold.
static bool SetProduction(Game& g, Side& side, City& c, int slot) {
    if (slot < 0 || slot >= static_cast<int>(c.slots.size())) return false;
    if (side.gold < c.slots[slot].cost + 30 && g.turn > 5) return false;
    game::SetProduction(g, c, slot);
    return true;
}

static void ProduceFor(Game& g, Side& side, City& c, int purpose) {
    int slot = rules::BestSlot(c.slots, purpose, g.types, side.enhanced);
    if (slot != kNone) SetProduction(g, side, c, slot);
}

void Vector(Game& g, City& c, const City* dest) {
    if (!Building(c))
        c.vector_to = kNone;
    else
        c.vector_to = dest ? dest->index : kNone;
}

// ai_buy_production_type (5db9:0bd1), if the side has 30 over its price.
static bool BuyType(Game& g, Side& side, City& c, int type_id) {
    const ArmyType* t = g.types.ById(type_id);
    if (!t) return false;
    if (!(side.gold > t->price + 30)) return false;
    int n = SlotToReplace(g, c);
    if (n == kNone) return false;
    game::BuyProduction(g, side, c, n, type_id);
    game::SortProduction(c);
    AIData& d = Data(g, side);
    d.bought++;
    c.producing = kNone;
    c.countdown = 0;
    c.vector_to = kNone;
    SetRole(d, c, kBuilding);
    return true;
}

// Buy the first flying type the side can afford into a city (623c:11d5).
static void BuyFlier(Game& g, Side& side, City& c) {
    int n = SlotToReplace(g, c);
    if (n == kNone) return;
    AIData& d = Data(g, side);
    for (int id = 0; id <= 28; id++) {
        const ArmyType* t = g.types.ById(id);
        if (t && t->flies && t->bonus[48] == 0 && t->price + 30 <= side.gold) {
            game::BuyProduction(g, side, c, n, id);
            game::SortProduction(c);
            SetCflag(d, c, kCfFlier);
            c.producing = kNone;
            c.countdown = 0;
            c.vector_to = kNone;
            return;
        }
    }
}

static void MarkFliers(Game& g, AIData& d) {
    for (auto& c : g.map->cities) {
        if (Standing(c)) {
            ClearCflag(d, c, kCfFlier);
            for (auto& slot : c.slots) {
                if (g.types.ById(slot.type)->flies) SetCflag(d, c, kCfFlier);
            }
        }
    }
}

void ClearUnseen(Game& g, Side& side) {
    if (g.map->options.hidden_map == 0) return;
    AIData& d = Data(g, side);
    for (auto& c : g.map->cities) {
        if (Cflag(d, c, kCfUnseen)) {
            bool seen = false;
            for (int x = c.x - 1; x <= c.x + 2; x++) {
                for (int y = c.y - 1; y <= c.y + 2; y++) {
                    if (x >= 0 && y >= 0 && x < g.map->width && y < g.map->height &&
                        game::Seen(g, side.index, x, y))
                        seen = true;
                }
            }
            if (seen) ClearCflag(d, c, kCfUnseen);
        }
    }
}

void Evaluate(Game& g, Side& side, int who) {
    AIData& d = Data(g, side);
    MarkFliers(g, d);
    ClearUnseen(g, side);
    d.own = 0;
    d.enemy = 0;
    d.neutral = 0;
    d.unseen = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (Standing(c)) {
            if (c.owner_index == who) {
                d.own++;
                d.held[c.index] = Get(d.held, c.index) + 1;
                if (Role(d, c) == 0) SetRole(d, c, kJustTaken);
            } else {
                if (Cflag(d, c, kCfUnseen)) d.unseen++;
                SetRole(d, c, 0);
                d.held[c.index] = 0;
                if (c.owner_index == kNone)
                    d.neutral++;
                else
                    d.enemy++;
            }
        } else {
            SetRole(d, c, 0);
            d.held[c.index] = 0;
        }
    }
    if (d.own != 0) {
        for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
            City& c = g.map->cities[i];
            if (Role(d, c) == kStop && d.unseen != 0 && d.explorers < 5 && Cflag(d, c, kCfFlier)) {
                SetRole(d, c, kExploreR2);
            }
            if (Role(d, c) == kJustTaken) {
                SetRole(d, c, !NeutralNeighbours(g, side, c).empty() ? kNearNeutral : kBuilding);
            }
        }
        d.turns++;
    }
}

int Wanted(Game& g, const Side& side, const City& c) {
    auto nb = FindNeighbours(g, c).first;
    int foreign = 0, war = 0;
    for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
        City* n = nb[j];
        if (n->owner_index != kNone && n->owner_index != side.index && Standing(*n)) {
            foreign++;
            if (State(g, side.index, n->owner_index) == 2) war++;
        }
    }
    if (war != 0) return 8;
    if (foreign >= 2) return 4;
    if (foreign == 1) return 3;
    return 2;
}

// Put the city's armies in the order they stand in (5ca7:0b5c): the ones to
// keep on the first tile first. {list, keep}.
static std::pair<std::vector<Army*>, int> Arrange(Game& g, Side& side, const City& c,
                                                  const std::vector<Army*>& list) {
    AIData& d = Data(g, side);
    int n = static_cast<int>(list.size());
    int keep_wanted = n < 13 ? kKeep[n] : 8;
    int mask = 0, min_max = 0, keep = 0;
    bool attack = false;
    std::vector<Army*> pool = list;
    if (Role(d, c) == kRally) {
        int fliers = 0, heroes = 0;
        for (int i = n - 1; i >= 0; i--) {
            Army* a = pool[i];
            if (IsHero(a))
                heroes++;
            else if (Flies(g, a) && (a->strength > 3 || fliers == 0))
                fliers++;
        }
        if (fliers != 0 && heroes != 0) fliers += heroes;
        if ((fliers > 3 && n < 17) || (fliers > 2 && n < 9)) attack = true;
        for (int gi = kMaxGroups; gi >= 1; gi--) {
            const Group& grp = *d.groups[gi];
            if (grp.active != 0 && grp.rally == c.index) {
                if (Has(grp.flags, kGfMovE12))
                    min_max = 12;
                else if (Has(grp.flags, kGfMovE16))
                    min_max = 16;
                else
                    min_max = grp.size;
                if (Has(grp.flags, kGfFly)) attack = true;
            }
        }
    }
    int k = 3 - Get(d.held, c.index) / 3;
    if (k > 0 && k < n && n - keep_wanted < k) keep_wanted = n - k;

    std::vector<Army*> out;
    for (;;) {
        int best_keep = -1, best_keep_i = kNone, best_other = -1, best_other_i = kNone;
        for (int i = n - 1; i >= 0; i--) {
            Army* a = pool[i];
            if (a) {
                int score = a->strength;
                if (static_cast<int>(out.size()) < keep_wanted) {
                    int ab = Ability(g, a);
                    if (!Has(mask, 1) && IsHero(a)) {
                        score += 1000;
                    } else if (min_max <= a->max_moves) {
                        if (!Has(mask, 0x10) && Flies(g, a))
                            score += 900;
                        else if (!attack) {
                            if (!Has(mask, 2) && ab == 2)
                                score += 800;
                            else if (!Has(mask, 4) && ab == 3)
                                score += 700;
                            else if (!Has(mask, 0x80) && Magical(g, a))
                                score += 600;
                            else if (!Has(mask, 8) && ab == 1)
                                score += 500;
                            else if (!Has(mask, 0x20) && Woods(g, a))
                                score += 400;
                            else if (!Has(mask, 0x40) && Hills(g, a))
                                score += 300;
                        }
                    }
                } else {
                    mask = 0xfff;
                }
                if (Has(mask, 1) && IsHero(a)) score = 1;
                if (Has(mask, 0x80) && Magical(g, a)) score = 2;
                if (a->max_moves < min_max || (attack && !Flies(g, a) && !IsHero(a))) {
                    if (best_other < score) {
                        best_other = score;
                        best_other_i = i;
                    }
                } else if (best_keep < score) {
                    best_keep = score;
                    best_keep_i = i;
                }
            }
        }
        if (best_keep_i == kNone && best_other_i == kNone) break;
        if (mask != 0xfff) {
            Army* a = pool[best_keep_i != kNone ? best_keep_i : best_other_i];
            int ab = Ability(g, a);
            if (IsHero(a))
                mask = Set(mask, 1);
            else {
                if (ab == 2) mask = Set(mask, 2);
                if (ab == 3) mask = Set(mask, 4);
                if (Flies(g, a)) mask = Set(mask, 0x10);
                if (Magical(g, a)) mask = Set(mask, 0x80);
                if (ab == 1) mask = Set(mask, 8);
                if (Woods(g, a)) mask = Set(mask, 0x20);
                if (Hills(g, a)) mask = Set(mask, 0x40);
            }
        }
        if (best_keep_i == kNone) {
            out.push_back(pool[best_other_i]);
            pool[best_other_i] = nullptr;
        } else {
            out.push_back(pool[best_keep_i]);
            pool[best_keep_i] = nullptr;
            keep++;
        }
    }
    if (keep_wanted < keep) keep = keep_wanted;
    return {out, keep};
}

static void Disband(Game& g, Side& side, Army* a) {
    game::Disband(g, side, {a});
}

// Hand items round between a city's heroes (6087:17db, 1a3d).
static void ShareItems(Game& g, AIData& d, const std::vector<Army*>& heroes) {
    auto count = [](Army* h, const std::set<int>& types) {
        int n = 0;
        for (Item* it : h->items)
            if (types.count(it->type)) n++;
        return n;
    };
    auto give = [&](Army* from, Army* to, const std::set<int>& types) {
        auto& items = from->items;
        for (size_t i = 0; i < items.size(); i++) {
            Item* it = items[i];
            if (types.count(it->type)) {
                items.erase(items.begin() + i);
                to->items.push_back(it);
                d.items_passed++;
                return true;
            }
        }
        return false;
    };
    for (const std::set<int>& t : {std::set<int>{6}, std::set<int>{5}, std::set<int>{7}}) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (Army* h : heroes) {
                if (count(h, t) > 1) {
                    for (Army* o : heroes) {
                        if (o != h && count(o, t) == 0 && count(h, t) > 1) {
                            if (give(h, o, t)) moved = true;
                        }
                    }
                }
            }
        }
    }
    // battle, command and standard items: each hero passes on only what it
    // started with, so nothing goes back and forth
    std::set<int> fight_t = {1, 2, 8};
    std::vector<std::vector<Item*>> start(heroes.size());
    std::vector<int> own_n(heroes.size());
    for (size_t i = 0; i < heroes.size(); i++) {
        for (Item* it : heroes[i]->items)
            if (fight_t.count(it->type)) start[i].push_back(it);
        own_n[i] = static_cast<int>(start[i].size());
    }
    bool moved = true;
    while (moved) {
        moved = false;
        for (size_t i = 0; i < heroes.size(); i++) {
            Army* h = heroes[i];
            if (!start[i].empty()) {
                int least = own_n[i], to = kNone;
                for (size_t j = 0; j < heroes.size(); j++) {
                    if (j != i && own_n[j] < least) {
                        least = own_n[j];
                        to = static_cast<int>(j);
                    }
                }
                if (to != kNone) {
                    Item* it = start[i].front();
                    start[i].erase(start[i].begin());
                    auto k = std::find(h->items.begin(), h->items.end(), it);
                    if (k != h->items.end()) h->items.erase(k);
                    heroes[to]->items.push_back(it);
                    d.items_passed++;
                    own_n[i]--;
                    own_n[to]++;
                    moved = true;
                }
            }
        }
    }
}

bool Garrison(Game& g, Side& side, City& c, bool force) {
    AIData& d = Data(g, side);
    int cx = c.x, cy = c.y;
    int last = Get(d.garrison, c.index);
    SetCflag(d, c, kCfCleaned);
    d.garrison[c.index] = 0;
    std::vector<Army*> list, heroes;
    int ordered = 0, fliers = 0, magic = 0;
    Army* last_hero = nullptr;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        if (i >= static_cast<int>(g.armies.size())) continue;
        Army* a = g.armies[i];
        if (a && !a->transit && a->x != kNone && a->x >= cx && a->x <= cx + 1 && a->y >= cy &&
            a->y <= cy + 1) {
            if (a->owner != side.index) {
                Side* s = g.map->Side(a->owner == kNone ? 8 : a->owner);
                Disband(g, s ? *s : side, a);
            } else {
                d.garrison[c.index]++;
                if (a->ai_order == kOrderCity && a->ai_dest == c.index) ClearOrder(a);
                if (a->ai_order != 0) ordered++;
                if (IsHero(a) && heroes.size() < 8) {
                    heroes.push_back(a);
                    last_hero = a;
                }
                if (Flies(g, a)) fliers++;
                if (Magical(g, a)) magic++;
                if (list.size() < 32)
                    list.push_back(a);
                else
                    Disband(g, side, a);
            }
        }
    }
    if (last_hero) {
        Sel s;
        s.hero = last_hero;
        s.armies = {last_hero};
        PickUp(g, s);
    }
    if (heroes.size() > 1) ShareItems(g, d, heroes);
    ClearCflag(d, c, kCfHero);
    ClearCflag(d, c, kCfMagic);
    if (!heroes.empty()) SetCflag(d, c, kCfHero);
    if (magic != 0) SetCflag(d, c, kCfMagic);
    if (list.size() > 16) {
        last = 0;
        ordered = 0;
    }

    int rl = Role(d, c);
    if (!((ordered < 3 || rl == kRally) &&
          (force || !heroes.empty() || Get(d.garrison, c.index) != last ||
           g.rng.Dice(1, 6, -1) == 0 || !game::ArmiesAt(g, cx + 1, cy + 1).empty()))) {
        return true;
    }
    d.keep[c.index] = 0;
    if (rl == kWeak || rl == kBuilding || rl == kStop) {
        int want = Wanted(g, side, c);
        if (list.size() < 2)
            SetRole(d, c, kWeak);
        else if (static_cast<int>(list.size()) < want)
            SetRole(d, c, kBuilding);
        else
            SetRole(d, c, kStop);
    }
    auto [sorted, keep_n] = Arrange(g, side, c, list);
    int n = static_cast<int>(sorted.size());
    if (!force && side.gold < 300 && side.income < side.upkeep_total * 2) {
        if (Role(d, c) != kRally && Get(d.held, c.index) > 10 &&
            static_cast<int>(heroes.size()) + magic + 4 < n && NearestArmy(g, side, cx, cy) > 10) {
            for (int i = static_cast<int>(heroes.size()) + magic + 4; i < n; i++) {
                if (sorted[i]) {
                    Disband(g, side, sorted[i]);
                    sorted[i] = nullptr;
                }
            }
        }
    }
    if (n > 24) {
        for (int i = 24; i < n; i++) {
            if (sorted[i]) {
                Disband(g, side, sorted[i]);
                sorted[i] = nullptr;
            }
        }
    }
    if ((g.map->options.quick_start == 0 || g.turn > 2) && n < 2) SetRole(d, c, kWeak);
    if (g.map->options.hidden_map != 0 && Role(d, c) == kExploreR2 && d.explorers < 5) {
        int go = fliers;
        int short_by = 2 - (n - fliers);
        if (short_by > 0) go = fliers - short_by;
        if (g.map->options.quick_start != 0 && g.turn < 3) go = 1;
        for (int i = 0; i < n; i++) {
            Army* a = sorted[i];
            if (go > 0 && a && Alive(g, a) && Flies(g, a)) {
                d.explorers++;
                a->ai_explore = true;
                moves::Explore(g, side, a, nullptr);
                sorted[i] = nullptr;
                go--;
            }
        }
    }
    int tile, per_tile;
    int r = Role(d, c);
    if (r == kNearNeutral || r == kTakingNeutral) {
        tile = 1;
        per_tile = 8;
    } else {
        tile = keep_n == 0 ? 1 : 0;
        per_tile = keep_n == 0 ? 8 : keep_n;
    }
    for (int i = 0; i < n; i++) {
        Army* a = sorted[i];
        if (a && Alive(g, a) && tile <= 3) {
            if (tile == 0) d.keep[c.index] = Get(d.keep, c.index) + 1;
            a->x = cx + kTileDx[tile];
            a->y = cy + kTileDy[tile];
            a->at_sea = false;
            a->target.reset();
            a->group = 0;
            a->ai_group = 0;
            a->done = false;
            per_tile--;
            if (per_tile == 0) {
                tile++;
                per_tile = 8;
            }
        }
    }
    return true;
}

void Clean(Game& g, Side& side) {
    AIData& d = Data(g, side);
    for (City* c : Own(g, side)) {
        if (!Cflag(d, *c, kCfCleaned)) Garrison(g, side, *c, false);
    }
}

// Send stacks from a city at the neutral cities round it (57ea:06c7).
static void TakeNeutrals(Game& g, Side& side, City& c, int count, std::map<int, int>& heading,
                         std::vector<Army*> stack) {
    AIData& d = Data(g, side);
    bool neutral_on = g.map->options.neutral_cities != 0;
    int odds_v = 100;
    City* from = &c;
    City* last_from = nullptr;
    City* last_pick = nullptr;
    for (;;) {
        std::vector<Army*> keep;
        for (Army* a : stack)
            if (Alive(g, a) && a->owner == side.index) keep.push_back(a);
        stack = keep;
        if (stack.empty()) return;
        Sel sel = Select(g, stack);
        auto nb = NeutralNeighbours(g, side, *from);
        if (nb.empty()) return;
        City* best = nullptr;
        int best_score = 1000;
        for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
            const NeutralNeighbour& e = nb[j];
            City* n = e.city;
            if (!Cflag(d, *n, kCfUnseen) && Get(heading, n->index) < 3) {
                if (neutral_on) odds_v = Odds(g, sel, n->x, n->y);
                if (odds_v > 74) {
                    int score = g.rng.Dice(1, 10, 0) + e.dist + Get(heading, n->index) * 10 +
                                (e.dist >= 41 ? 10 : 0) + (e.dist >= 51 ? 30 : 0) + (100 - odds_v);
                    if (score < best_score) {
                        best = n;
                        best_score = score;
                    }
                }
            }
        }
        if (!best) break;
        heading[best->index] = Get(heading, best->index) + count;
        last_from = &c;
        last_pick = best;
        auto s = Order(g, stack, kOrderCity, best->index, 0x80);
        if (s) MoveTo(g, *s, s->leader->target->first, s->leader->target->second);
        if (best->owner_index != side.index) return;
        if (d.cautious != 0) return;
        from = best;
        if (NearestArmy(g, side, best->x, best->y) < 10) return;
    }
    if (g.map->options.hidden_map != 0 && last_from == &c && last_pick) {
        Order(g, stack, kOrderCity, last_pick->index, 0x80);
    }
}

// How many armies a city holds back (57ea:03aa): {extra, lost, near}.
static std::tuple<int, int, int> HoldBack(Game& g, Side& side, const City& c) {
    AIData& d = Data(g, side);
    int near = NearestArmy(g, side, c.x, c.y);
    int extra = (near < 5 ? 2 : 0) + (near < 15 ? 1 : 0) + (d.cautious != 0 ? 1 : 0);
    int lost = 0;
    for (int i = 0; i < 8; i++) lost += d.cities_lost[i];
    return {extra, std::min(2, lost), near};
}

// The idle armies on a city's corner tile, beyond the ones held back, the
// fastest first.
template <class Test>
static std::vector<Army*> Idle(Game& g, Side& side, const City& c, int reserve, Test test) {
    std::vector<Army*> found;
    for (Army* a : OnTile(g, side.index, c.x, c.y)) {
        if (a->ai_group == 0 && !a->done && a->ai_order == 0 && test(a)) {
            if (reserve > 0)
                reserve--;
            else if (found.size() < 8)
                found.push_back(a);
        }
    }
    LuaSort(found, [](Army* p, Army* q) { return p->max_moves > q->max_moves; });
    return found;
}

// Send a city's idle armies at its neutral neighbours (57ea:03aa). True with
// the map hidden (the city then sends explorers).
static bool AttackNeutrals(Game& g, Side& side, City& c, std::map<int, int>& heading) {
    auto [extra, lost, near] = HoldBack(g, side, c);
    (void)near;
    auto list = Idle(g, side, c, extra + lost, [](Army*) { return true; });
    if (list.empty()) return false;
    size_t per = g.map->options.neutral_cities != 0 ? 8 : 1;
    size_t i = 0;
    for (;;) {
        std::vector<Army*> stack;
        while (i < list.size() && stack.size() < per) {
            stack.push_back(list[i]);
            i++;
        }
        if (stack.empty()) break;
        if (extra + lost == 0 && g.map->options.hidden_map != 0 &&
            NearestArmy(g, side, c.x, c.y) < 15)
            break;
        TakeNeutrals(g, side, c, static_cast<int>(stack.size()), heading, stack);
    }
    return g.map->options.hidden_map != 0;
}

// Send a city's idle armies to explore, one by one (57ea:0b19).
static void SendExplorers(Game& g, Side& side, City& c, bool fliers_only) {
    AIData& d = Data(g, side);
    bool grouping = false;
    for (int i = 1; i <= kMaxGroups; i++)
        if (d.groups[i]->active != 0) grouping = true;
    auto [extra, lost, near] = HoldBack(g, side, c);
    (void)near;
    auto list = Idle(g, side, c, extra + lost, [&](Army* a) {
        if (IsHero(a) && grouping) return false;
        if (fliers_only && !Flies(g, a)) return false;
        return true;
    });
    for (Army* a : list) {
        if (extra + lost == 0 && g.map->options.hidden_map != 0 &&
            NearestArmy(g, side, c.x, c.y) < 15)
            return;
        if (Alive(g, a)) moves::Explore(g, side, a, &c);
    }
}

City* OpenNeutral(Game& g, Side& side, const City& c, std::map<int, int>& heading) {
    auto nb = NeutralNeighbours(g, side, c);
    for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
        City* n = nb[j].city;
        if (Get(heading, n->index) <= 2) return n;
    }
    return nullptr;
}

// One pass of the neutral phase (57ea:00b5).
static int NeutralPass(Game& g, Side& side, int pass, std::map<int, int>& heading) {
    AIData& d = Data(g, side);
    int acted = 0;
    for (City* c : Own(g, side)) {
        if (c->owner_index == side.index && (pass == 0 || Role(d, *c) == kJustTaken)) {
            if (Role(d, *c) == kJustTaken) {
                Garrison(g, side, *c, true);
                SetRole(d, *c, kNearNeutral);
            }
            bool act = !NeutralNeighbours(g, side, *c).empty();
            if (!act) {
                int r = Role(d, *c);
                if (r == kNearNeutral || r == kTakingNeutral) SetRole(d, *c, kWeak);
                if (g.map->options.quick_start == 0 && Role(d, *c) == kBuilding && g.turn < 6)
                    act = true;
            }
            if (act) {
                acted++;
                if (!AttackNeutrals(g, side, *c, heading)) {
                    SetRole(d, *c, OpenNeutral(g, side, *c, heading) ? kNearNeutral : kWeak);
                } else {
                    SendExplorers(g, side, *c, g.turn > 10);
                    SetRole(d, *c, kTakingNeutral);
                }
            }
        }
    }
    return acted;
}

void Neutral(Game& g, Side& side) {
    std::map<int, int> heading;
    for (Army* a : Armies(g, side.index)) {
        if (a->ai_order == kOrderCity && a->ai_dest != kNone &&
            a->ai_dest < static_cast<int>(g.map->cities.size())) {
            heading[a->ai_dest] = Get(heading, a->ai_dest) + 1;
        }
    }
    for (int pass = 0; pass <= 9; pass++) {
        if (NeutralPass(g, side, pass, heading) == 0) break;
    }
}

// A strong garrison strikes at a weak neighbour (5e97:053f).
static void QuickStrike(Game& g, Side& side, City& c) {
    AIData& d = Data(g, side);
    int rl = Role(d, c);
    int x = c.x;
    int y = c.y;
    if (rl != kRally) x++;
    if (!(rl != kRally || Get(d.garrison, c.index) > 11)) return;
    int need = (side.gold < 40 && side.income < side.upkeep_total) ? 65 : 85;
    auto list = Collect(g, side.index, x, y, 8);
    int n = static_cast<int>(list.size());
    if (n == 0) return;
    if (n < 4 && (rl == kMember || rl == kRally)) return;
    Sel sel = Select(g, list);
    auto [nb, nd] = FindNeighbours(g, c);
    bool danger = false;
    City* best = nullptr;
    int best_odds = 0, best_turns = 100;
    for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
        City* t = nb[j];
        if (!Cflag(d, *t, kCfUnseen) && t->owner_index != side.index && Standing(*t) &&
            t->index != d.quest_city &&
            (t->owner_index == kNone || State(g, side.index, t->owner_index) == 2)) {
            int o = Odds(g, sel, t->x, t->y);
            if (o == 0) {
                if ((n < 4 && nd[j] < 25) || (n < 6 && nd[j] < 15)) danger = true;
            } else if (o >= need) {
                int turns = nd[j] / std::max(1, sel.min_moves - 2) + 1;
                if (turns < best_turns || (turns == best_turns && best_odds < o)) {
                    best = t;
                    best_odds = o;
                    best_turns = turns;
                }
            }
        }
    }
    if (!danger && best) {
        auto s = Order(g, list, kOrderCity, best->index, 0);
        if (s) MoveTo(g, *s, s->leader->target->first, s->leader->target->second);
    }
}

void QuickAttack(Game& g, Side& side) {
    AIData& d = Data(g, side);
    if (d.cautious != 0) return;
    for (City* c : Own(g, side)) {
        if (c->owner_index == side.index && kQuickRoles.count(Role(d, *c)) &&
            Get(d.garrison, c->index) > 3) {
            QuickStrike(g, side, *c);
        }
    }
}

void UpdateHide(Game& g, Side& side) {
    AIData& d = Data(g, side);
    ClearUnseen(g, side);
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (Role(d, c) == kTakingNeutral) {
            for (auto& e : NeutralNeighbours(g, side, c)) {
                if (!Cflag(d, *e.city, kCfUnseen)) {
                    SetRole(d, c, kNearNeutral);
                    break;
                }
            }
        }
    }
}

// The city that should buy a new type (ai_city_for_type, 5db9:0c83).
static City* CityForType(Game& g, Side& side) {
    AIData& d = Data(g, side);
    City* best = nullptr;
    int best_score = -1;
    for (City* c : Own(g, side)) {
        int r = Role(d, *c);
        bool candidate = false;
        int younger = 0;
        if (r == kTakingNeutral || r == kNearNeutral) {
            if (Get(d.held, c->index) > 3) {
                younger = 4;
                candidate = !BuildsWell(g, side, *c).first;
            }
        } else if (r == kWeak || r == kBuilding) {
            candidate = !BuildsWell(g, side, *c).first;
        } else if (r == kStop) {
            if (!BuildsWell(g, side, *c).first) {
                SetRole(d, *c, kNowhere);
                candidate = true;
            }
        } else if (r == kNowhere) {
            if (!BuildsWell(g, side, *c).first)
                candidate = true;
            else
                SetRole(d, *c, kStop);
        }
        if (candidate) {
            int score = Get(d.held, c->index) - younger;
            if (best_score < score) {
                best = c;
                best_score = score;
            }
        }
    }
    return best;
}

void Rebuild(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int limit =
            std::max(5, static_cast<int>(FloorDiv(static_cast<long>(d.rebuild_limit) *
                                                          static_cast<long>(g.map->cities.size()),
                                                  80)));
    if ((d.heroes > 4 || d.own <= limit * d.heroes) && side.gold > 499) {
        int t = side.gold < 2001 ? d.rebuild_type : d.rebuild_type_rich;
        City* c = CityForType(g, side);
        if (c && side.gold > 499) BuyType(g, side, *c, t);
    }
}

// The member cities of an assault group (5f19:0b72).
static void PickMembers(Game& g, Side& side, Group& grp) {
    AIData& d = Data(g, side);
    int min_max = Has(grp.flags, kGfMovE12) ? 12 : Has(grp.flags, kGfMovE16) ? 16 : grp.size;
    // 1-based, as the group's member slots are
    std::map<int, City*> members;
    std::map<int, int> strength;
    int count = 0;
    for (City* c : Own(g, side)) {
        if (groups::Free(g, side, *c, false)) {
            int f = Get(d.flags, c->index);
            bool fits = (!Has(grp.flags, kGfMovE12) || Has(f, kCfMovE12)) &&
                        (!Has(grp.flags, kGfMovE16) || Has(f, kCfMovE16)) &&
                        (!Has(grp.flags, kGfFly) || Has(f, kCfFlygroup));
            bool plain = Has(grp.flags, kGfMovE16) || Has(grp.flags, kGfMovE12) ||
                         Has(grp.flags, kGfFly) ||
                         !(Has(f, kCfMovE12) || Has(f, kCfFlygroup) || Has(f, kCfMovE16));
            if (fits && plain) {
                auto [good, slot_i] = BuildsWell(g, side, *c);
                if (good && min_max <= c->slots[slot_i].move) {
                    const Slot& slot = c->slots[slot_i];
                    int at = kNone;
                    if (count < 4) {
                        at = count + 1;
                    } else {
                        int least = 100;
                        for (int k = 1; k <= 4; k++) {
                            if (strength.count(k) && strength[k] < least) {
                                least = strength[k];
                                at = k;
                            }
                        }
                        if (at != kNone && !(least <= slot.strength)) at = kNone;
                    }
                    if (at != kNone) {
                        if (!members.count(at)) count++;
                        members[at] = c;
                        strength[at] = slot.strength;
                    }
                }
            }
        }
    }
    grp.members.clear();
    for (int k = 1; k <= count; k++) {
        City* c = members[k];
        grp.members[k] = c->index;
        SetRole(d, *c, kMember);
    }
}

void Production(Game& g, Side& side) {
    AIData& d = Data(g, side);
    for (City* c : Own(g, side)) {
        if (Role(d, *c) == kMember) {
            SetRole(d, *c, kStop);
            c->vector_to = kNone;
        }
    }
    for (int gi = kMaxGroups; gi >= 1; gi--) {
        if (d.groups[gi]->active != 0) PickMembers(g, side, *d.groups[gi]);
    }
    if (g.map->options.hidden_map != 0) {
        // a city whose neutral neighbours are all unseen goes looking (57ea:02ff)
        for (City* c : Own(g, side)) {
            if (Role(d, *c) == kNearNeutral) {
                auto nb = NeutralNeighbours(g, side, *c);
                if (!nb.empty()) {
                    bool seen = false;
                    for (auto& e : nb)
                        if (!Cflag(d, *e.city, kCfUnseen)) seen = true;
                    if (!seen) SetRole(d, *c, kTakingNeutral);
                }
            }
        }
    }
    for (City* c : Own(g, side)) {
        c->vector_to = kNone;
        if (!Building(*c)) {
            if (side.gold < 40 && side.income < side.upkeep_total) return;
            int r = Role(d, *c);
            int purpose = 3;
            if (r == kTakingNeutral || r == kExplorer) {
                if (!Cflag(d, *c, kCfFlier)) BuyFlier(g, side, *c);
                purpose = 4;
            } else if (r == kNearNeutral) {
                purpose = g.map->options.neutral_cities != 0 ? 2 : 1;
            } else if (r == kWeak) {
                purpose = 2;
            } else if (r == kExploreR2) {
                purpose = 4;
            } else if (r == kStop) {
                purpose = kNone;
                c->producing = kNone;
                c->countdown = 0;
            }
            if (purpose != kNone) ProduceFor(g, side, *c, purpose);
        }
    }
}

void Vectoring(Game& g, Side& side) {
    AIData& d = Data(g, side);
    for (int gi = d.max_groups; gi >= 1; gi--) {
        Group& grp = *d.groups[gi];
        City* rally = grp.rally != kNone ? g.map->City(grp.rally) : nullptr;
        if (grp.active != 0 && rally && rally->owner_index == side.index) {
            SetRole(d, *rally, kRally);
            for (int k = 4; k >= 1; k--) {
                City* m = grp.members.count(k) ? g.map->City(grp.members[k]) : nullptr;
                if (m && Role(d, *m) == kMember) Vector(g, *m, rally);
            }
        }
    }
}

}  // namespace w2::ai::cities
