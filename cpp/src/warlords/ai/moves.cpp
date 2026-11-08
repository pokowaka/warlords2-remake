#include "warlords/ai/moves.hpp"

#include <algorithm>
#include <cstdlib>

#include "util/util.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::moves {

using namespace core;

static City* CityOrNull(Game& g, int i) {
    return i != kNone ? g.map->City(i) : nullptr;
}

static bool OnCityTile(const Game& g, const Army* a) {
    City* c = CityAt(g, a->x, a->y);
    return c && Standing(*c);
}

// The computer's visit to a sage (5e97:0080).
static void Sage(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int gold = g.rng.Dice(3, 500, 500);
    City* best = nullptr;
    int best_count = -1, best_d = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == kNone && Cflag(d, c, kCfUnseen)) {
            bool next_to_us = false;
            for (City* n : FindNeighbours(g, c).first)
                if (n->owner_index == side.index) next_to_us = true;
            if (next_to_us) {
                int count = 0, d_own = 1000;
                for (auto& o : g.map->cities) {
                    if (o.owner_index == kNone) {
                        if (Cflag(d, o, kCfUnseen) && Dist(c.x, c.y, o.x, o.y) < 20) count++;
                    } else if (o.owner_index == side.index) {
                        int dd = Dist(c.x, c.y, o.x, o.y);
                        if (dd < d_own) d_own = dd;
                    }
                }
                if (count > 3 && (best_count < count || (count == best_count && best_d < d_own))) {
                    best = &c;
                    best_count = count;
                    best_d = d_own;
                }
            }
        }
    }
    if (g.map->options.hidden_map == 0 || !best) {
        side.gold += gold;
        return;
    }
    int x = std::max(0, best->x + g.rng.Dice(1, 11, -6));
    int y = std::max(0, best->y + g.rng.Dice(1, 11, -6));
    x = std::min(x, g.map->width - 1);
    y = std::min(y, g.map->height - 1);
    site::SageMap(g, side, x, y);
    move::Invalidate(g);
}

int SearchSite(Game& g, Sel* sel, Army* h, Site& s) {
    if (!(h && Alive(g, h) && h->x == s.x && h->y == s.y && h->moves != 0)) return 0;
    Side& side = *g.map->Side(h->owner);
    if (SiteOpen(s)) {
        std::vector<Army*> stack = sel ? sel->armies : std::vector<Army*>{h};
        auto r = site::Search(g, stack, s.x, s.y, false);
        if (r && r->kind == "sage") Sage(g, side);
        // at a temple the computer takes a quest, by its own rules (quest_assign(1))
        if (s.content == site::kTemple && g.map->options.quests != 0 && !side.quest && IsHero(h)) {
            quest::Assign(g, side, h, true, move::ModeOf(g, stack) == move::kFlying);
        }
        if (!IsHero(h)) return 0;
        for (Army* a : stack) a->done = false;
        if (g.map->options.hidden_map != 0 && s.content == site::kAllies) {
            for (Army* a : OnTile(g, side.index, s.x, s.y)) {
                if (a->ai_order == 0 && Magical(g, a)) {
                    a->ai_explore = true;
                    Explore(g, side, a, nullptr);
                }
            }
        }
    }
    return 2;
}

int Party(Game& g, Side& side, std::vector<Army*> list) {
    Army* hero = nullptr;
    Army* flier = nullptr;
    for (Army* a : list) {
        if (!hero && IsHero(a)) hero = a;
        if (!flier && Flies(g, a)) flier = a;
    }
    if (!hero) return 0;
    if (!flier) return 1;
    AIData& d = Data(g, side);
    list = {hero, flier};
    ClearOrder(hero);
    ClearOrder(flier);
    int score = -1;
    Site* best_site = nullptr;
    City* best_city = nullptr;
    for (int i = static_cast<int>(g.map->sites.size()) - 1; i >= 0; i--) {
        Site& s = g.map->sites[i];
        bool taken = false;
        for (Army* o : Armies(g, side.index)) {
            if (o != hero && IsHero(o) && o->ai_order == kOrderSite && o->ai_dest == s.index)
                taken = true;
        }
        if (!taken && Explored(g, side.index, s.x, s.y) && s.content != site::kTemple &&
            !s.searched) {
            int dd = Dist(hero->x, hero->y, s.x, s.y);
            int base = kNone;
            if (dd < 15 && score < 215 - dd)
                base = 215;
            else if (!(dd > 39 || 90 - dd <= score))
                base = 90;
            if (base != kNone) {
                score = base - dd;
                best_site = &s;
            }
        }
    }
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == side.index) {
            int dd = Dist(hero->x, hero->y, c.x, c.y);
            if (Role(d, c) == kRally) dd -= 80;
            int base = kNone;
            if (dd < 15 && score < 115 - dd)
                base = 115;
            else if (!(dd > 39 || 40 - dd <= score))
                base = 40;
            if (base != kNone) {
                score = base - dd;
                best_city = &c;
            }
        }
    }
    if (best_site) {
        auto sel = Order(g, list, kOrderSite, best_site->index, 0x100);
        if (sel) MoveTo(g, *sel, sel->leader->target->first, sel->leader->target->second);
        return SearchSite(g, sel ? &*sel : nullptr, hero, *best_site);
    }
    if (best_city && Dist(hero->x, hero->y, best_city->x, best_city->y) > 2) {
        auto sel = Order(g, list, kOrderCity, best_city->index, 0);
        if (sel) MoveTo(g, *sel, sel->leader->target->first, sel->leader->target->second);
    }
    return 0;
}

namespace {
struct ScanResult {
    bool found = false;
    int tx = kNone, ty = kNone;
    int kind = 0;
    int ex = kNone, ey = kNone;
};

// Look over the 10 x 10 tiles round an explorer (57ea:177f) for where to go.
ScanResult Scan(Game& g, Side& side, Army* a, const Sel& sel) {
    int hx, hy;
    if (City* hc = CityOrNull(g, a->home_city)) {
        hx = hc->x;
        hy = hc->y;
    } else if (side.capital) {
        hx = side.capital->x;
        hy = side.capital->y;
    } else {
        hx = a->x;
        hy = a->y;
    }
    bool coast = a->at_sea;
    if (g.rng.Dice(1, 4, -1) == 0 && OnCityTile(g, a) && !IsHero(a)) {
        City* c = CityAt(g, a->x, a->y);
        if (c && move::IsPort(g, *c)) coast = true;
    }
    bool flying = sel.mode == move::kFlying;
    bool hl = move::StackMode(g, sel.armies).hills;
    ScanResult out;
    int best = 0;
    auto explored_round = [&](int x, int y) {
        for (int nx = x - 1; nx <= x + 1; nx++) {
            for (int ny = y - 1; ny <= y + 1; ny++) {
                if (nx >= 0 && ny >= 0 && nx < g.map->width && ny < g.map->height &&
                    game::Seen(g, side.index, nx, ny))
                    return true;
            }
        }
        return false;
    };
    for (int x = a->x - 5; x <= a->x + 4; x++) {
        for (int y = a->y - 5; y <= a->y + 4; y++) {
            if (x >= 0 && y >= 0 && x < g.map->width && y < g.map->height) {
                int t = Terrain(g, x, y);
                City* c = CityAt(g, x, y);
                if (t == move::kCity && (!c || c->owner_index != side.index)) {
                    int dd = flying ? 2 : Dist(a->x, a->y, x, y);
                    if (dd < 4 && (!a->at_sea || (c && move::IsPort(g, *c))) &&
                        explored_round(x, y)) {
                        out.kind = 1;
                        out.ex = x;
                        out.ey = y;
                    }
                }
                if (t == move::kSite && out.kind == 0 && !a->at_sea && explored_round(x, y)) {
                    out.kind = 2;
                    out.ex = x;
                    out.ey = y;
                }
                bool road = scn::RoadAt(*g.map, x, y) % 0x20 != 0;
                if (t != move::kCity && (flying || road || hl || t != move::kHills) &&
                    !game::Seen(g, side.index, x, y) && (flying || move::kCost[t] != 0)) {
                    int unseen = 0;
                    for (int nx = x - 1; nx <= x + 1; nx++) {
                        for (int ny = y - 1; ny <= y + 1; ny++) {
                            if (nx >= 0 && ny >= 0 && nx < g.map->width && ny < g.map->height &&
                                !game::Seen(g, side.index, nx, ny))
                                unseen++;
                        }
                    }
                    if (unseen > 1) {
                        int bonus = 0;
                        bool wet = t == move::kWater || t == move::kShore;
                        if (coast) {
                            if (wet) bonus += 200;
                        } else if (!wet) {
                            if (t != move::kHills) bonus = 100;
                            if (road) bonus += 200;
                        }
                        int from_home = Dist(x, y, hx, hy);
                        int s = g.rng.Dice(1, 10, 0) + Dist(x, y, a->x, a->y) * 2 + from_home +
                                unseen * 10 + bonus;
                        if (coast && bonus != 0) s += std::min(from_home, 50) * 10;
                        if (best < s) {
                            out.tx = x;
                            out.ty = y;
                            out.found = true;
                            best = s;
                        }
                    }
                }
            }
        }
    }
    return out;
}

// A flier's fallback: the nearest unseen city, give or take 1d10 (57ea:164e);
// it aims one up and left of it.
std::pair<int, int> UnseenCity(Game& g, Side& side, Army* a) {
    AIData& d = Data(g, side);
    City* best = nullptr;
    int best_d = kNone;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (Standing(c) && Cflag(d, c, kCfUnseen)) {
            int dd = Dist(c.x, c.y, a->x, a->y) + g.rng.Dice(1, 10, 0);
            if (best_d == kNone || dd < best_d) {
                best = &c;
                best_d = dd;
            }
        }
    }
    if (best) return {best->x - 1, best->y - 1};
    return {kNone, kNone};
}

// An explorer next to a city goes for it (57ea:10a8) if it may and the odds
// are good. True if it went.
bool TryCity(Game& g, Side& side, Army* a, int ex, int ey) {
    AIData& d = Data(g, side);
    City* pick = nullptr;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (Standing(c) && c.owner_index != side.index && d.quest_city != c.index &&
            Dist(c.x, c.y, ex, ey) <= 2 && diplo::CanAttack(g, side, c)) {
            if (Cflag(d, c, kCfUnseen)) cities::ClearUnseen(g, side);
            if (!Cflag(d, c, kCfUnseen)) {
                pick = &c;
                break;
            }
        }
    }
    if (!pick) return false;
    auto sel = Order(g, {a}, kOrderRoam, 0, 0x20);
    if (!sel) return false;
    int o = Odds(g, *sel, pick->x, pick->y);
    int need = 75;
    if (!IsHero(a) && a->strength < 4) {
        if (d.neutral < static_cast<int>(g.map->cities.size()) / 10)
            need = 50;
        else if (d.own <= std::min(g.turn / 2, 10))
            need = 40;
    }
    if (o < need) return false;
    a->ai_explore = false;
    sel = Order(g, {a}, kOrderCity, pick->index, 0x80);
    if (sel) MoveTo(g, *sel, sel->leader->target->first, sel->leader->target->second);
    return true;
}

// An explorer next to a site visits it (57ea:12eb). True if it went.
bool TrySite(Game& g, Side& side, Army* a, int sx, int sy) {
    Site* s = SiteAt(g, sx, sy);
    if (!s || !SiteOpen(*s) || a->at_sea) return false;
    bool temple = s->content == site::kTemple;
    if (temple && s->temple_index != kNone && a->blessings.count(s->temple_index)) return false;
    if (!(IsHero(a) || temple)) return false;
    auto there = game::ArmiesAt(g, sx, sy);
    if (!there.empty() && there[0]->owner != side.index) return false;
    auto sel = Order(g, {a}, kOrderRoam, 0, 0);
    if (!sel) return false;
    sel->leader->target = std::make_pair(sx, sy);
    int r = MoveTo(g, *sel, sx, sy);
    if (r == 1) return false;
    if (Alive(g, a)) {
        if (a->x == sx && a->y == sy) {
            if (IsHero(a))
                SearchSite(g, &*sel, a, *s);
            else
                site::Search(g, {a}, sx, sy, false);
            if (s->content == site::kAllies) {
                for (Army* b : OnTile(g, side.index, sx, sy)) {
                    if (b->ai_order == 0 && Magical(g, b)) b->ai_explore = true;
                }
            }
        } else {
            if (temple && a->at_sea && s->temple_index != kNone)
                a->blessings.insert(s->temple_index);
            a->done = true;
        }
        a->ai_explore = true;
        ClearOrder(a);
        a->target.reset();
    }
    return true;
}

// Walk an explorer's stack to (x, y) (57ea:1581).
void WalkTo(Game& g, Army* a, int x, int y) {
    a->target = std::make_pair(x, y);
    Sel sel = SelectStackOf(g, a);
    int r = MoveTo(g, sel, x, y);
    if (Alive(g, a)) {
        if (r == 1) {
            a->ai_explore = false;
            ClearOrder(a);
        }
        a->done = true;
        a->target.reset();
        a->group = 0;
    }
}

// One step of an explorer (57ea:0f46).
void ExploreStep(Game& g, Side& side, Army* a, City* home) {
    Sel sel = Select(g, {a});
    ScanResult sc = Scan(g, side, a, sel);
    int tx = sc.tx, ty = sc.ty;
    if (!sc.found) {
        if (Flies(g, a)) std::tie(tx, ty) = UnseenCity(g, side, a);
        if (tx == kNone) {
            std::map<int, int> none;
            City* n = home ? cities::OpenNeutral(g, side, *home, none) : nullptr;
            if (!n) {
                ClearOrder(a);
                a->ai_explore = false;
                a->target.reset();
                a->group = 0;
                return;
            }
            tx = n->x;
            ty = n->y;
        }
    } else if (sc.kind == 1) {
        if (TryCity(g, side, a, sc.ex, sc.ey)) return;
    } else if (sc.kind == 2) {
        if (TrySite(g, side, a, sc.ex, sc.ey)) return;
    }
    WalkTo(g, a, tx, ty);
}
}  // namespace

void Explore(Game& g, Side& side, Army* a, City* home) {
    if (!Alive(g, a)) return;
    Order(g, {a}, kOrderRoam, 0, 0x20);
    int steps = a->moves / 5 + 1;
    for (;;) {
        if (!Alive(g, a) || a->moves < 2) return;
        int px = a->x, py = a->y;
        ExploreStep(g, side, a, home);
        if (!Alive(g, a) || (a->x == px && a->y == py)) return;
        if (a->moves < 4) return;
        if (steps == 0) return;
        if (!a->ai_explore) return;
        steps--;
    }
}

void Search(Game& g, Side& side) {
    AIData& d = Data(g, side);
    bool rally = false;
    for (auto& c : g.map->cities) {
        if (c.owner_index == side.index && Role(d, c) == kRally) rally = true;
    }
    d.searchers = 0;
    d.explorers = 0;
    for (int pass = 0; pass <= 1; pass++) {
        for (Army* a : Armies(g, side.index)) {
            if (Alive(g, a) && !a->transit && a->ai_explore && ((pass == 0) == IsHero(a))) {
                if ((IsHero(a) && rally) || (Flies(g, a) && d.explorers > 7 && a->strength < 4)) {
                    ClearOrder(a);
                    a->ai_explore = false;
                } else {
                    Explore(g, side, a, nullptr);
                    if (Alive(g, a) && a->owner == side.index) {
                        if (Flies(g, a))
                            d.explorers++;
                        else
                            d.searchers++;
                    }
                }
            }
        }
    }
}

void HeroParties(Game& g, Side& side) {
    for (Army* a : Armies(g, side.index)) {
        if (Alive(g, a) && !a->transit && a->ai_party) {
            a->ai_party = false;
            if (IsHero(a)) {
                bool going = true;
                int guard = 0;
                while (going && guard < 20) {
                    guard++;
                    if (!Alive(g, a)) break;
                    auto list =
                            CollectOrdered(g, side.index, a->x, a->y, a->ai_group, a->ai_order, 0);
                    if (list.empty() || a->moves < 3) break;
                    int px = a->x, py = a->y;
                    int r = Party(g, side, list);
                    if (r == 0 || r == 1) going = false;
                    if (Alive(g, a) && a->x == px && a->y == py && a->max_moves > a->moves) break;
                }
            }
        }
    }
}

// Is the stack's neutral target still the thing to do (5ad0:05ff)?
static bool NeutralStillOn(Game& g, Side& side, Sel& sel) {
    AIData& d = Data(g, side);
    Army* lead = sel.leader;
    City* dest = CityOrNull(g, lead->ai_dest);
    if (!dest) return true;
    if (dest->owner_index == side.index) {
        City* here = CityAt(g, lead->x, lead->y);
        auto nb = here ? cities::NeutralNeighbours(g, side, *here)
                       : std::vector<cities::NeutralNeighbour>{};
        if (nb.empty()) nb = cities::NeutralNeighbours(g, side, *dest);
        int odds_v = 100;
        City* best = nullptr;
        int bd = 1000;
        for (int j = static_cast<int>(nb.size()) - 1; j >= 0; j--) {
            City* n = nb[j].city;
            if (!Cflag(d, *n, kCfUnseen)) {
                if (g.map->options.neutral_cities != 0) odds_v = Odds(g, sel, n->x, n->y);
                int dd = Dist(lead->x, lead->y, n->x, n->y);
                if (odds_v > 74 && dd < bd) {
                    best = n;
                    bd = dd;
                }
            }
        }
        if (!best) {
            if (g.map->options.hidden_map != 0 && g.turn < 10) {
                std::vector<Army*> list = sel.armies;
                for (Army* a : list) Explore(g, side, a, nullptr);
                return false;
            }
            return true;
        }
        for (Army* a : sel.armies) {
            a->ai_order = kOrderCity;
            a->ai_dest = best->index;
            SetTarget(g, a, kOrderCity, best->index);
        }
        return true;
    }
    if (!diplo::CanAttack(g, side, *dest)) {
        for (Army* a : sel.armies) {
            ClearOrder(a);
            a->target.reset();
        }
        return false;
    }
    return true;
}

void MoveAll(Game& g, Side& side) {
    AIData& d = Data(g, side);
    for (Army* a : Armies(g, side.index)) {
        if (!a->transit) a->ai_moved = a->moves < 2;
    }
    if (!d.cursor) d.cursor = std::make_pair(side.cap_x, side.cap_y);
    for (;;) {
        Army* pick = nullptr;
        int pd = kNone;
        for (Army* a : Armies(g, side.index)) {
            if (!a->transit && a->x != kNone && !a->done && !a->ai_moved) {
                int dd = std::abs(a->x - d.cursor->first) + std::abs(a->y - d.cursor->second);
                if (dd == 0) dd = 9000;
                if (pd == kNone || dd < pd) {
                    pick = a;
                    pd = dd;
                }
            }
        }
        if (!pick) break;
        d.cursor = std::make_pair(pick->x, pick->y);
        pick->ai_moved = true;
        if (pick->target && pick->target->first != kNone && pick->target->first >= 0 &&
            pick->moves > 1) {
            Sel sel = SelectStackOf(g, pick);
            Army* lead = sel.leader;
            int ord = lead->ai_order;
            City* dest = CityOrNull(g, lead->ai_dest);
            if (ord == kOrderNone ||
                (ord == kOrderCity &&
                 (!dest || lead->ai_dest == d.quest_city || !Standing(*dest)))) {
                for (Army* a : sel.armies) {
                    ClearOrder(a);
                    a->target.reset();
                    a->done = true;
                }
            } else if (!lead->ai_neutral || NeutralStillOn(g, side, sel)) {
                dest = CityOrNull(g, lead->ai_dest);
                if (lead->ai_order == kOrderCity && dest && dest->owner_index == side.index) {
                    lead->target = std::make_pair(dest->x + 1, dest->y + 1);
                }
                if (lead->target && Alive(g, lead)) {
                    MoveTo(g, sel, lead->target->first, lead->target->second);
                }
            }
        }
    }
}

// The best city for an idle stack by the flood (5ad0:0f3c).
static City* IdleTarget(Game& g, Side& side, const Sel& sel, const Flood& flood, int n) {
    AIData& d = Data(g, side);
    City* best = nullptr;
    int best_d = 1000;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        int ow = Owner(c);
        if (Standing(c) && !Cflag(d, c, kCfUnseen) && d.quest_city != c.index &&
            (ow == kNeutral || ow == side.index || State(g, side.index, ow) == 2)) {
            auto [dd, px, py] = CityDistance(g, flood, c);
            (void)py;
            if (px != kNone) {
                if (ow == side.index) {
                    dd += Role(d, c) == kRally ? 10 : 30;
                    if (n > 3) dd += 100;
                    if (dd < best_d) {
                        best = &c;
                        best_d = dd;
                    }
                } else if (Odds(g, sel, c.x, c.y) > 74 && dd < best_d) {
                    best = &c;
                    best_d = dd;
                }
            }
        }
    }
    return best;
}

void SendIdle(Game& g, Side& side, Army* a) {
    int home = 1000;
    for (auto& c : g.map->cities) {
        if (c.owner_index == side.index && Standing(c)) {
            int dd = Dist(a->x, a->y, c.x, c.y);
            if (dd < home) home = dd;
        }
    }
    bool weak = a->strength < 3 || a->max_moves < 8;
    if (!Flies(g, a) && weak) {
        game::Disband(g, side, {a});
        return;
    }
    auto list = Collect(g, side.index, a->x, a->y, 8);
    if (list.empty()) return;
    int fl = 0, he = 0, ot = 0;
    for (Army* b : list) {
        if (Flies(g, b))
            fl++;
        else if (IsHero(b))
            he++;
        else
            ot++;
    }
    if (fl != 0 && he != 0 && ot != 0) {
        std::vector<Army*> keep;
        for (Army* b : list)
            if (Flies(g, b) || IsHero(b)) keep.push_back(b);
        list = keep;
    }
    if (list.size() < 2 && weak) {
        game::Disband(g, side, {a});
        return;
    }
    Sel sel = Select(g, list);
    int range = sel.hero ? 50 : (a->strength / 2) * 10;
    range = std::min(range, home + 10);
    Flood flood = FloodFrom(g, side.index, a->x, a->y, range, &sel);
    City* c = IdleTarget(g, side, sel, flood, static_cast<int>(list.size()));
    if (!c) {
        game::Disband(g, side, list);
        return;
    }
    auto s = Order(g, list, kOrderCity, c->index, 0);
    if (s) MoveTo(g, *s, s->leader->target->first, s->leader->target->second);
}

void Rescue(Game& g, Side& side) {
    for (Army* a : Armies(g, side.index)) {
        if (Alive(g, a) && !a->transit && a->x != kNone && a->max_moves <= a->moves) {
            if (a->moves == a->max_moves + 2 && !OnCityTile(g, a) && a->ai_order == kOrderCity) {
                City* dest = CityOrNull(g, a->ai_dest);
                if (!dest || !Standing(*dest)) {
                    int gone = a->ai_dest;
                    for (Army* b : OnTile(g, side.index, a->x, a->y)) {
                        if (b->ai_order == kOrderCity && b->ai_dest == gone) {
                            ClearOrder(b);
                            b->ai_group = 0;
                        }
                    }
                }
            }
            if (Alive(g, a) && !a->ai_explore && a->ai_order == 0 && !OnCityTile(g, a))
                SendIdle(g, side, a);
        }
    }
}

void LastRescue(Game& g, Side& side) {
    for (Army* a : Armies(g, side.index)) {
        if (Alive(g, a) && !a->transit && a->x != kNone && a->moves == a->max_moves + 2 &&
            !OnCityTile(g, a)) {
            a->ai_explore = false;
            a->ai_neutral = false;
            ClearOrder(a);
            a->target.reset();
            SendIdle(g, side, a);
        }
    }
}

void Specials(Game& g, Side& side) {
    AIData& d = Data(g, side);
    for (City* c : cities::Own(g, side)) {
        if (Cflag(d, *c, kCfHero) && Role(d, *c) != kRally) {
            auto list = CollectOrdered(g, side.index, c->x + 1, c->y, 0, 0, 0);
            if (!list.empty() && Party(g, side, list) == 1 && !cities::Building(*c))
                SetRole(d, *c, kExplorer);
        }
    }
}

}  // namespace w2::ai::moves
