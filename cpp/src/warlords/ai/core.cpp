#include "warlords/ai/core.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <queue>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::core {

int Dist(int x1, int y1, int x2, int y2) {
    double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

bool InPlay(const Game& g, int index) {
    if (index < 0 || index > 7) return false;
    const Side& s = g.map->sides[index];
    return s.in_use && s.alive;
}

bool IsComputer(const Game& g, int index) {
    if (index < 0 || index > 7) return false;
    return g.map->sides[index].computer;
}

int State(const Game& g, int a, int b) {
    if (a == kNeutral || b == kNeutral || a == kNone || b == kNone) return 2;
    if (a == b) return 0;
    return diplomacy::State(g, a, b);
}

int Proposal(const Game& g, int a, int b) {
    if (a == b) return 0;
    return diplomacy::Proposal(g, a, b);
}

void Propose(Game& g, int a, int b, int v) {
    if (a == b) return;
    diplomacy::Propose(g, a, b, v);
}

City* CityAt(const Game& g, int x, int y) {
    return game::CityAt(g, x, y);
}

Site* SiteAt(const Game& g, int x, int y) {
    if (g.map->site_at.empty() || x < 0 || y < 0 || x >= g.map->width || y >= g.map->height)
        return nullptr;
    return g.map->site_at[y * g.map->width + x];
}

int Terrain(const Game& g, int x, int y) {
    return scn::TerrainAt(*g.map, x, y);
}

bool SiteOpen(const Site& s) {
    return s.content == site::kTemple || !s.searched;
}

bool Explored(const Game& g, int sd, int x, int y) {
    if (g.map->options.hidden_map == 0) return true;
    for (int tx = x - 1; tx <= x + 1; tx++) {
        for (int ty = y - 1; ty <= y + 1; ty++) {
            if (tx >= 0 && ty >= 0 && tx < g.map->width && ty < g.map->height &&
                game::Seen(g, sd, tx, ty))
                return true;
        }
    }
    return false;
}

bool Flies(const Game& g, const Army* a) {
    return g.types.ById(a->type)->flies;
}
bool Magical(const Game& g, const Army* a) {
    return g.types.ById(a->type)->bonus[48] != 0;
}
int Ability(const Game& g, const Army* a) {
    return g.types.ById(a->type)->bonus[52];
}
bool Woods(const Game& g, const Army* a) {
    return g.types.ById(a->type)->woods_move;
}
bool Hills(const Game& g, const Army* a) {
    return g.types.ById(a->type)->hills_move;
}

std::vector<Army*> Armies(const Game& g, int side_index) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->owner == side_index) out.push_back(a);
    }
    return out;
}

std::vector<Army*> OnTile(const Game& g, int side_index, int x, int y) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->owner == side_index && !a->transit && a->x == x && a->y == y) out.push_back(a);
    }
    return out;
}

AIData NewData() {
    AIData d;
    d.groups[0] = EmptyGroup();  // never used: groups count from 1
    for (int i = 1; i <= kMaxGroups; i++) d.groups[i] = EmptyGroup();
    return d;
}

AIData& Data(Game& g, Side& sd) {
    if (!sd.ai) sd.ai = std::make_shared<AIData>(NewData());
    return *sd.ai;
}

AIData& Data(Game& g, int side_index) {
    return Data(g, *g.map->Side(side_index));
}

// The shared neighbour table (623c:0398): each city's six nearest cities by
// land, with the distance to each. It is built once, as the game is set up,
// over side 0's cost grid -- the cities as they are then -- and kept with
// the game (g.aiNeighbours, city indices).

std::pair<std::vector<City*>, std::vector<int>> FindNeighbours(Game& g, const City& c) {
    if (!g.ai_neighbours) g.ai_neighbours = BuildNeighbours(g);
    std::pair<std::vector<City*>, std::vector<int>> out;
    if (c.index >= 0 && c.index < static_cast<int>(g.ai_neighbours->by_city.size())) {
        const auto& e = g.ai_neighbours->by_city[c.index];
        for (int i : e.cities) out.first.push_back(&g.map->cities[i]);
        out.second = e.dist;
    }
    return out;
}

namespace {
// 623c:0749: every other city under 100 away goes into a list of 50 -- the
// first free place, or over the farthest one when this one is nearer --
// walking the cities from the last; then six are taken, nearest first and
// 50 further off for a direction that has three already, the list read from
// its end so a tie goes to the later place. Returns how many were found.
int PickNeighbours(Game& g, const City& c, const Flood& flood, std::vector<int>& picked,
                   std::vector<int>& dists) {
    int slot_city[50], slot_dist[50];
    for (int k = 0; k < 50; k++) {
        slot_city[k] = -1;
        slot_dist[k] = 0;
    }
    int found = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        if (i == c.index) continue;
        auto [d, px, py] = CityDistance(g, flood, g.map->cities[i]);
        (void)py;
        if (px == kNone) continue;
        found++;
        int at = -1, worst = d;
        int k = 0;
        for (; k < 50 && slot_city[k] != -1; k++) {
            if (worst < slot_dist[k]) {
                worst = slot_dist[k];
                at = k;
            }
        }
        if (k < 50) at = k;
        if (at != -1 && (slot_city[at] == -1 || d < slot_dist[at])) {
            slot_city[at] = i;
            slot_dist[at] = d & 0xff;
        }
    }
    int left = 0, right = 0, up = 0, down = 0;
    picked.clear();
    dists.clear();
    while (picked.size() < 6) {
        int best = -1, best_score = 10000;
        for (int k = 49; k >= 0; k--) {
            if (slot_city[k] != -1) {
                const City& o = g.map->cities[slot_city[k]];
                int score = slot_dist[k];
                if ((o.x < c.x && left > 2) || (c.x < o.x && right > 2) || (o.y < c.y && up > 2) ||
                    (c.y < o.y && down > 2)) {
                    score += 50;
                }
                if (score < best_score) {
                    best_score = score;
                    best = k;
                }
            }
        }
        if (best == -1) break;
        const City& o = g.map->cities[slot_city[best]];
        picked.push_back(o.index);
        dists.push_back(slot_dist[best]);
        if (o.x < c.x) left++;
        if (c.x < o.x) right++;
        if (o.y < c.y) up++;
        if (c.y < o.y) down++;
        slot_city[best] = -1;
    }
    return found;
}
}  // namespace

std::shared_ptr<Neighbours> BuildNeighbours(Game& g) {
    auto out = std::make_shared<Neighbours>();
    out->by_city.resize(g.map->cities.size());
    for (auto& c : g.map->cities) {
        std::vector<int> picked, dists;
        int found = PickNeighbours(g, c, FloodFrom(g, 0, c.x, c.y, 45, move::kLand, kNone), picked,
                                   dists);
        if (found == 0)
            PickNeighbours(g, c, FloodFrom(g, 0, c.x, c.y, 60, move::kLand, kNone), picked, dists);
        out->by_city[c.index] = Neighbours::Entry{picked, dists};
    }
    return out;
}

Sel& Refresh(Game& g, Sel& sel) {
    int least = 100;
    for (Army* a : sel.armies) least = std::min(least, a->moves);
    sel.min_moves = sel.armies.empty() ? 0 : least;
    sel.mode = sel.armies.empty() ? move::kLand : move::ModeOf(g, sel.armies);
    sel.hero = nullptr;
    for (Army* a : sel.armies)
        if (IsHero(a) && !sel.hero) sel.hero = a;
    return sel;
}

Sel Select(Game& g, const std::vector<Army*>& list) {
    Sel sel;
    for (Army* a : list) {
        if (a && Alive(g, a) && !a->transit) sel.armies.push_back(a);
    }
    sel.leader = sel.armies.empty() ? nullptr : sel.armies[0];
    Refresh(g, sel);
    return sel;
}

static int FightRank(const Game& g, const Army* a) {
    const auto& row = g.map->fight_order[a->owner == kNone ? 8 : a->owner];
    return a->type < static_cast<int>(row.size()) ? row[a->type] : 0;
}

int NewGroup(const Game& g, int side_index) {
    std::set<int> used;
    for (Army* a : g.armies) {
        if (a->owner == side_index && !a->transit && a->group) used.insert(a->group);
    }
    for (int n = 2; n <= 254; n++)
        if (!used.count(n)) return n;
    return 0;
}

std::optional<Sel> Order(Game& g, const std::vector<Army*>& list, int ord, int dest, int flags) {
    Sel sel = Select(g, list);
    if (sel.armies.empty()) return std::nullopt;
    Army* best = nullptr;
    for (Army* a : sel.armies)
        if (!best || FightRank(g, a) > FightRank(g, best)) best = a;
    sel.leader = best;
    int grp = sel.armies.size() > 1 ? NewGroup(g, best->owner) : 0;
    for (Army* a : sel.armies) {
        a->ai_order = ord;
        a->ai_dest = dest;
        a->group = grp;
        SetTarget(g, a, ord, dest);
        a->ai_neutral = false;
        a->ai_explore = false;
        if (flags) {
            if (Has(flags, 0x80)) a->ai_neutral = true;
            if (Has(flags, 0x20)) a->ai_explore = true;
            if (Has(flags, 0x100)) a->ai_party = true;
        }
    }
    Refresh(g, sel);
    return sel;
}

void SetTarget(Game& g, Army* a, int ord, int dest) {
    if (ord == kOrderRoam) return;
    int x = kNone, y = kNone;
    if (ord == kOrderCity && dest != kNone) {
        City* c = g.map->City(dest);
        if (c) {
            x = c->x;
            y = c->y;
            if (c->owner_index == a->owner) {
                x++;
                y++;
            } else {
                if (x < a->x) x++;
                if (y < a->y) y++;
            }
        }
    } else if (ord == kOrderItem && dest != kNone) {
        if (dest >= 0 && dest < static_cast<int>(g.map->items.size())) {
            const Item& it = g.map->items[dest];
            if (it.x != kNone) {
                x = it.x;
                y = it.y;
            }
        }
    } else if (ord == kOrderSite && dest != kNone) {
        if (dest >= 0 && dest < static_cast<int>(g.map->sites.size())) {
            const Site& s = g.map->sites[dest];
            x = s.x;
            y = s.y;
        }
    }
    a->target = std::make_pair(x != kNone ? x : a->x, y != kNone ? y : a->y);
}

Sel SelectStackOf(Game& g, Army* a) {
    std::vector<Army*> list;
    for (Army* b : OnTile(g, a->owner, a->x, a->y)) {
        if (b == a || (a->group != 0 && b->group == a->group)) {
            list.push_back(b);
            b->ai_moved = true;
        }
    }
    Sel sel = Select(g, list);
    Army* best = nullptr;
    for (Army* b : sel.armies)
        if (!best || FightRank(g, b) > FightRank(g, best)) best = b;
    sel.leader = best ? best : a;
    return sel;
}

std::vector<Army*> Collect(const Game& g, int side_index, int x, int y, int min_moves) {
    std::vector<Army*> out;
    for (Army* a : OnTile(g, side_index, x, y)) {
        if (static_cast<int>(out.size()) >= rules::kMaxStack) break;
        if (!min_moves || a->moves >= min_moves) out.push_back(a);
    }
    return out;
}

std::vector<Army*> CollectOrdered(const Game& g, int side_index, int x, int y, int grp, int ord,
                                  int min_max) {
    std::vector<Army*> out;
    for (Army* a : OnTile(g, side_index, x, y)) {
        if (static_cast<int>(out.size()) >= rules::kMaxStack) break;
        if (a->ai_group == grp && a->ai_order == ord && (!min_max || a->max_moves >= min_max))
            out.push_back(a);
    }
    return out;
}

Flood FloodFrom(Game& g, int side_index, int x, int y, int range, const Sel* sel, int fog_side) {
    int mode = sel ? sel->mode : move::kLand;
    bool wd = false, hl = false;
    if (sel) {
        move::Mode m = move::StackMode(g, sel->armies);
        wd = m.woods;
        hl = m.hills;
    }
    Flood f;
    f.w = g.map->width;
    f.h = g.map->height;
    f.dist = move::Flood(g, side_index, x, y, range, mode, wd, hl,
                         fog_side == kSameSide ? side_index : fog_side);
    return f;
}

Flood FloodFrom(Game& g, int side_index, int x, int y, int range, int mode, int fog_side) {
    Flood f;
    f.w = g.map->width;
    f.h = g.map->height;
    f.dist = move::Flood(g, side_index, x, y, range, mode, false, false,
                         fog_side == kSameSide ? side_index : fog_side);
    return f;
}

int FloodAt(const Flood& f, int x, int y) {
    if (x < 0 || y < 0 || x >= f.w || y >= f.h) return kUnreached;
    int v = f.dist[y * f.w + x];
    if (v == 30000 || v == move::kShut) return kUnreached;
    return std::abs(v);
}

// the ring round a city's 2x2 footprint, as 59bf:0a85 walks it
static const int kRing[12] = {0, 2, 2, 4, 4, 4, 6, 6, 6, 0, 0, 0};
static const int kRdx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int kRdy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

std::tuple<int, int, int> CityDistance(const Game& g, const Flood& flood, const City& c, bool far) {
    int best = far ? 1000 : 100;
    int bx = kNone, by = kNone;
    int x = c.x, y = c.y;
    for (int dir : kRing) {
        int nx = x + kRdx[dir], ny = y + kRdy[dir];
        if (nx < 0 || ny < 0 || nx >= 112 || ny >= 156) return {best, bx, by};
        x = nx;
        y = ny;
        int d = FloodAt(flood, x, y);
        if (d < best) {
            best = d;
            bx = x;
            by = y;
        }
    }
    return {best, bx, by};
}

int Odds(Game& g, const Sel& sel, int x, int y) {
    if (sel.armies.empty()) return 0;
    int own = sel.armies[0]->owner;
    int n = own != kNone ? Data(g, own).sims : 10;
    if (n < 1) n = 1;
    combat::Lines l = combat::BattleLines(g, sel.armies, x, y);
    if (l.defenders.empty()) return 100;
    int wins = 0;
    for (int i = 0; i < n; i++)
        if (combat::Resolve(g, l.attackers, l.defenders, x, y).won) wins++;
    return wins * 100 / n;
}

void PickUp(Game& g, const Sel& sel) {
    if (!sel.hero || !Alive(g, sel.hero)) return;
    Army* h = sel.hero;
    City* c = CityAt(g, h->x, h->y);
    for (auto& it : g.map->items) {
        if (it.status == 1 && it.x != kNone && !it.planted) {
            bool here;
            if (c)
                here = it.x >= c->x && it.x <= c->x + 1 && it.y >= c->y && it.y <= c->y + 1;
            else
                here = it.x == h->x && it.y == h->y;
            if (here) {
                it.status = 3;
                it.x = kNone;
                it.y = kNone;
                h->items.push_back(&it);
            }
        }
    }
}

namespace {
struct StepResult {
    int code = 0;
    int x = kNone, y = kNone;
};

// Walk the selection towards (tx, ty) (1a8b:0c4f and 1a8b:07f9): 1 no path,
// 2 stopped short, 3 an enemy army is next, 4 the path is walked, 5 an enemy
// city is next; with the tile ahead.
StepResult Step(Game& g, Sel& sel, int tx, int ty) {
    Army* lead = sel.leader;
    lead->target = std::make_pair(tx, ty);
    auto path = move::FindPath(g, sel.armies, lead->x, lead->y, tx, ty);
    if (!path) return {1};
    if (path->empty()) return {4};
    move::WalkResult r = move::Walk(g, sel.armies, *path);
    if (r.steps > 0) Walked(g, sel.armies, r);
    Refresh(g, sel);
    PickUp(g, sel);
    if (r.stopped == "attack") {
        return {r.attack->city ? 5 : 3, r.attack->x, r.attack->y};
    } else if (r.stopped == "at peace") {
        if (r.steps < static_cast<int>(path->size())) {
            const move::Step& ahead = (*path)[r.steps];
            if (CityAt(g, ahead.x, ahead.y)) return {5, ahead.x, ahead.y};
        }
        return {2};
    } else if (r.stopped == "arrived") {
        return {4};
    }
    return {2};
}
}  // namespace

Battle Fight(Game& g, Sel& sel, int x, int y) {
    // the computer's attack (67cc:124a): the lines are drawn up first; a city
    // of a side it is not at war with is treachery -- war, and the score's
    // penalty (67cc:20a8); then the attack is paid for and fought, shown, and
    // only then taken effect (after_battle, 67cc:0a6b)
    Battle result = game::DecideAttack(g, sel.armies, x, y);
    int me = sel.armies.empty() ? kNone : sel.armies[0]->owner;
    City* city = result.lines.city;
    if (city && city->owner_index != kNone && me != kNone && g.map->options.diplomacy != 0 &&
        diplomacy::State(g, me, city->owner_index) != diplomacy::kWar) {
        diplomacy::AttackAnyway(g, me, city->owner_index);
    }
    game::ChargeAttack(g, sel.armies, x, y);
    if (hooks.on_fight) hooks.on_fight(g, sel.armies, x, y, result);
    game::ApplyAttack(g, result);
    std::vector<Army*> keep;
    for (Army* a : sel.armies)
        if (Alive(g, a)) keep.push_back(a);
    sel.armies = keep;
    if (!Alive(g, sel.leader)) sel.leader = keep.empty() ? nullptr : keep[0];
    Refresh(g, sel);
    return result;
}

void Done(const Sel& sel) {
    for (Army* a : sel.armies) a->done = true;
}

int MoveTo(Game& g, Sel& sel, int tx, int ty, bool keep) {
    if (sel.armies.empty() || tx < 0 || ty < 0 || tx >= g.map->width || ty >= g.map->height)
        return 0;
    int code = 0;
    bool again = true;
    while (again) {
        again = false;
        if (sel.armies.empty()) break;
        StepResult s = Step(g, sel, tx, ty);
        code = s.code;
        int ax = s.x, ay = s.y;
        Army* lead = sel.leader;
        if (lead && lead->target && lead->x == lead->target->first &&
            lead->y == lead->target->second) {
            code = 4;
            lead->target.reset();
        }
        if (code == 2) {
            Done(sel);
        } else if (code == 5) {
            City* c = CityAt(g, tx, ty);
            if (c && Standing(*c)) {
                City* dest =
                        (lead && lead->ai_dest != kNone) ? g.map->City(lead->ai_dest) : nullptr;
                if (!dest) dest = c;
                auto [a2, nx, ny] = AttackCity(g, sel, dest, ax, ay);
                again = a2;
                if (again) {
                    tx = nx;
                    ty = ny;
                }
            }
        } else if (code == 4) {
            if (lead && lead->ai_order == kOrderSite && sel.hero) {
                Site* st = SiteAt(g, sel.hero->x, sel.hero->y);
                if (st) moves::SearchSite(g, &sel, sel.hero, *st);
            }
            for (Army* a : sel.armies) {
                ClearOrder(a);
                a->ai_group = 0;
            }
        } else if (code == 3) {
            Fight(g, sel, ax, ay);
            if (!sel.armies.empty()) again = true;
        }
    }
    if (!keep) Done(sel);
    return code;
}

std::tuple<bool, int, int> AttackCity(Game& g, Sel& sel, City* dest, int ax, int ay) {
    int me = sel.leader->owner;
    AIData& d = Data(g, me);
    int own = Owner(*dest);
    City* city = CityAt(g, ax, ay);
    if (own == kNeutral) {
        Fight(g, sel, ax, ay);
        if (city && city->owner_index == me) SetRole(d, *city, kJustTaken);
    } else if (own != me) {
        City* pick = dest;
        if (d.quest_city != dest->index || !sel.hero) pick = Retarget(g, sel, dest, ax, ay);
        if (pick != dest) {
            for (Army* a : sel.armies) {
                a->ai_order = kOrderCity;
                a->ai_dest = pick->index;
            }
            return {true, pick->x, pick->y};
        }
        int was = city ? Owner(*city) : kNone;
        Fight(g, sel, ax, ay);
        if (city && city->owner_index == me) groups::EarlyVengeance(g, me, *city, was);
    }
    for (Army* a : sel.armies) ClearOrder(a);
    return {false, kNone, kNone};
}

City* Retarget(Game& g, Sel& sel, City* c, int x, int y) {
    int me = sel.leader->owner;
    int od = Odds(g, sel, x, y);
    int own = Owner(*c);
    bool fair = own == kNeutral || State(g, own, me) == 2;
    if (own != kNeutral && !IsComputer(g, own) && g.rng.Dice(1, 4, -1) == 0) fair = true;
    if (!fair || od < 51) {
        Flood f = FloodFrom(g, me, sel.leader->x, sel.leader->y, 15, &sel);
        City* best = BestCity(g, sel, f, fair);
        return best ? best : c;
    }
    return c;
}

City* BestCity(Game& g, const Sel& sel, const Flood& flood, bool skip_own) {
    int me = sel.leader->owner;
    AIData& d = Data(g, me);
    City* best = nullptr;
    int best_score = -1;
    auto& cities = g.map->cities;
    for (int i = static_cast<int>(cities.size()) - 1; i >= 0; i--) {
        City& c = cities[i];
        int own = Owner(c);
        if (!(skip_own && own == me) && Standing(c) && d.quest_city != c.index &&
            !Cflag(d, c, kCfUnseen) && (own == kNeutral || own == me || State(g, own, me) == 2)) {
            auto [dst, px, py] = CityDistance(g, flood, c);
            (void)py;
            if (px != kNone) {
                int od;
                if (own == me) {
                    od = 15;
                    dst += 20;
                } else
                    od = Odds(g, sel, c.x, c.y);
                int score;
                if (od >= 51 && dst < sel.min_moves)
                    score = 400 - dst + od * 10;
                else if (od > 10)
                    score = 100 - dst + od * 5;
                else
                    score = -2;
                if (score > best_score) {
                    best = &c;
                    best_score = score;
                }
            }
        }
    }
    return best;
}

}  // namespace w2::ai::core
