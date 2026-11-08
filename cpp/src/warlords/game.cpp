#include "warlords/game.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <queue>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2 {

bool Army::Hero() const {
    return type == armytype::kHero;
}

Game::Game() = default;
Game::~Game() = default;

Army* Game::Make(const Army& a) {
    pool.push_back(std::make_unique<Army>(a));
    return pool.back().get();
}

Army* Game::Add(const Army& a) {
    std::set<int> used;
    for (Army* b : armies) used.insert(b->slot);
    int slot = 0;
    while (used.count(slot)) slot++;
    Army* p = Make(a);
    p->slot = slot;
    size_t i = armies.size();
    while (i > 0 && armies[i - 1]->slot > slot) i--;
    armies.insert(armies.begin() + i, p);
    return p;
}

bool Game::Alive(const Army* a) const {
    return a && std::find(armies.begin(), armies.end(), a) != armies.end();
}

void Game::Remove(const Army* a) {
    auto it = std::find(armies.begin(), armies.end(), a);
    if (it != armies.end()) armies.erase(it);
}

namespace game {

static const int kTransitTurns = 2;  // a vectored army arrives two turns later

static int Key(const Game& g, int x, int y) {
    return y * g.map->width + x;
}

std::vector<Army*> ArmiesAt(const Game& g, int x, int y) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (!a->transit && a->x == x && a->y == y) out.push_back(a);
    }
    return out;
}

City* CityAt(const Game& g, int x, int y) {
    if (x < 0 || y < 0 || x >= g.map->width || y >= g.map->height) return nullptr;
    return g.map->city_tile[Key(g, x, y)];
}

void SetCityOwner(Game& g, City& city, int side_index) {
    city.owner_index = side_index;
    move::Invalidate(g);
}

std::vector<City*> SideCities(const Game& g, const Side& side) {
    std::vector<City*> out;
    for (auto& c : g.map->cities)
        if (c.owner_index == side.index) out.push_back(&c);
    return out;
}

std::vector<Army*> SideArmies(const Game& g, const Side& side) {
    std::vector<Army*> out;
    for (Army* a : g.armies)
        if (a->owner == side.index) out.push_back(a);
    return out;
}

int ItemBonus(const Game& g, const Side& side, int item_type) {
    int total = 0;
    for (Army* a : g.armies) {
        if (a->owner == side.index) {
            for (Item* it : a->items)
                if (it->type == item_type) total += std::max(1, it->value);
        }
    }
    return total;
}

std::pair<int, int> FreeTileIn(Game& g, const City& city, bool spill, std::pair<int, int>* last) {
    static const int kD[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (auto& d : kD) {
        int x = city.x + d[0], y = city.y + d[1];
        if (x < g.map->width && y < g.map->height &&
            static_cast<int>(ArmiesAt(g, x, y).size()) < rules::kMaxStack) {
            return {x, y};
        }
    }
    if (!spill) return {kNone, kNone};
    int lx = kNone, ly = kNone;
    for (int i = 0; i < 20; i++) {
        int x = city.x + g.rng.Dice(1, 3, -2);
        int y = city.y + g.rng.Dice(1, 3, -2);
        if (x >= 0 && y >= 0 && x < g.map->width && y < g.map->height) {
            lx = x;
            ly = y;
            if (ArmiesAt(g, x, y).empty()) return {x, y};
        }
    }
    if (last) *last = {lx, ly};
    return {kNone, kNone};
}

int Income(const Game& g, const Side& side) {
    int total = 0;
    auto cities = SideCities(g, side);
    for (City* c : cities) total += c->income;
    int per_city = ItemBonus(g, side, rules::kItemGoldPerCity);
    return total + static_cast<int>(cities.size()) * per_city;
}

int Upkeep(const Game& g, const Side& side) {
    int total = 0;
    for (Army* a : g.armies) {
        if (a->owner == side.index && !a->transit) {
            int u = a->upkeep;
            if (a->at_sea) u = std::max(rules::kSeaMinUpkeep, u);
            total += u;
        }
    }
    return total;
}

// One starting army per city (setup_garrisons, 6bd8:00cc), the last city
// first (docs/rules.md > Starting garrisons).
static void SetupGarrisons(Game& g) {
    Side* setup = g.map->Side(kSetupPlayer);
    bool enhanced = setup && setup->enhanced;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.razed) continue;
        bool owned = c.owner_index != kNone;
        auto level = rules::GarrisonLevel(owned, g.map->options.neutral_cities, g.rng);
        int slot = kNone;
        if (level && !c.slots.empty()) {
            slot = rules::BestSlot(c.slots, rules::kGarrisonPurpose[*level], g.types, enhanced);
        }
        if (slot != kNone) {
            Army a = rules::ArmyFromSlot(c.slots[slot], enhanced);
            a.x = c.x;
            a.y = c.y;
            a.owner = c.owner_index;
            a.moves = 0;
            a.home_city = c.index;
            g.Add(a);
        } else {
            // Neutral Cities off: a placeholder Scouts army of strength 1.
            const ArmyType* t = g.types.ById(armytype::kScouts);
            Army a;
            a.x = c.x;
            a.y = c.y;
            a.owner = kNone;
            a.type = armytype::kScouts;
            a.name = t->name;
            a.strength = 1;
            a.max_moves = t->move;
            a.moves = 0;
            a.upkeep = 0;
            a.home_city = c.index;
            g.Add(a);
        }
    }
}

// Quick Start: deal every neutral city out to the sides (setup_capitals,
// 79fa:07ca).
static void DealCities(Game& g) {
    std::map<int, std::pair<int, int>> from;
    int turn = kNone;
    for (int i = 7; i >= 0; i--) {
        Side* s = g.map->Side(i);
        if (s && s->in_use) {
            from[i] = {s->cap_x, s->cap_y};
            turn = i;
        }
    }
    if (turn == kNone) return;
    for (;;) {
        auto [x, y] = from[turn];
        City* pick = nullptr;
        int best = 1000;
        for (auto& c : g.map->cities) {
            if (c.owner_index == kNone) {
                double dx = x - c.x, dy = y - c.y;
                int d = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
                if (d < best) {
                    pick = &c;
                    best = d;
                }
            }
        }
        if (!pick) return;
        Side* side = g.map->Side(turn);
        pick->owner = side;
        pick->owner_index = turn;
        if (g.rng.Dice(1, 10, -1) < 5)
            from[turn] = {side->cap_x, side->cap_y};
        else
            from[turn] = {pick->x, pick->y};
        do {
            turn = (turn + 1) % 8;
        } while (!from.count(turn));
    }
}

// Optional scale reduction for curriculum training & fast small-map benchmarks:
// keeps `activeSides` nearest capitals (preferring land-connected neighbours)
// and crops the world to a padded island around them without changing MAP_W/H.
static void ApplyScaleAndCrop(Map& m, const NewGameOptions& opts) {
    std::vector<int> avail;
    for (int i = 0; i < 8; i++) {
        if (m.sides[i].in_use && m.sides[i].capital) avail.push_back(i);
    }
    if (opts.active_sides > 0 && static_cast<int>(avail.size()) > opts.active_sides) {
        uint32_t h = static_cast<uint32_t>(opts.seed) * 2654435761U + 0x9e3779b9U;
        int s0 = avail[h % avail.size()];
        int ax = m.sides[s0].cap_x, ay = m.sides[s0].cap_y;

        std::vector<int> land_dist(static_cast<size_t>(kMapW) * kMapH, INT_MAX);
        std::queue<std::pair<int, int>> q;
        if (ax >= 0 && ay >= 0 && ax < kMapW && ay < kMapH) {
            land_dist[ay * kMapW + ax] = 0;
            q.push({ax, ay});
        }
        while (!q.empty()) {
            auto [cx, cy] = q.front();
            q.pop();
            int cd = land_dist[cy * kMapW + cx];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= kMapW || ny >= kMapH) continue;
                    int nk = ny * kMapW + nx;
                    if (land_dist[nk] != INT_MAX) continue;
                    if (scn::TerrainAt(m, nx, ny) == move::kWater) continue;
                    land_dist[nk] = cd + 1;
                    q.push({nx, ny});
                }
            }
        }

        auto metric = [&](int s) -> std::tuple<int, int, int, int> {
            if (s == s0) return {0, 0, 0, s};
            int sx = m.sides[s].cap_x, sy = m.sides[s].cap_y;
            int ld = (sx >= 0 && sy >= 0 && sx < kMapW && sy < kMapH) ? land_dist[sy * kMapW + sx]
                                                                      : INT_MAX;
            int dx = sx - ax, dy = sy - ay;
            int d2 = dx * dx + dy * dy;
            return {1, ld == INT_MAX ? 1 : 0, ld < INT_MAX ? ld : d2, s};
        };
        std::sort(avail.begin(), avail.end(), [&](int a, int b) { return metric(a) < metric(b); });
        for (size_t i = static_cast<size_t>(opts.active_sides); i < avail.size(); i++) {
            Side& sd = m.sides[avail[i]];
            if (sd.capital) sd.capital->owner = nullptr;
            sd.capital = nullptr;
            sd.in_use = false;
        }
    }

    if (opts.crop_margin <= 0) return;

    int min_x = kMapW, max_x = 0, min_y = kMapH, max_y = 0;
    bool any_side = false;
    for (const auto& sd : m.sides) {
        if (!sd.in_use) continue;
        min_x = std::min(min_x, sd.cap_x);
        max_x = std::max(max_x, sd.cap_x + 1);
        min_y = std::min(min_y, sd.cap_y);
        max_y = std::max(max_y, sd.cap_y + 1);
        any_side = true;
    }
    if (!any_side) return;

    int x0 = std::max(2, min_x - opts.crop_margin);
    int x1 = std::min(kMapW - 3, max_x + opts.crop_margin);
    int y0 = std::max(2, min_y - opts.crop_margin);
    int y1 = std::min(kMapH - 3, max_y + opts.crop_margin);
    m.active_x0 = x0;
    m.active_y0 = y0;
    m.active_x1 = x1;
    m.active_y1 = y1;

    int plain_tile = 0, water_tile = 0;
    for (int t = 0; t < 255; t++) {
        if (m.terrain_type[t] == move::kPlain) {
            plain_tile = t;
            break;
        }
    }
    for (int t = 0; t < 255; t++) {
        if (m.terrain_type[t] == move::kWater) {
            water_tile = t;
            break;
        }
    }

    std::vector<City> kept_cities;
    for (auto& c : m.cities) {
        if (c.x >= x0 && c.x + 1 <= x1 && c.y >= y0 && c.y + 1 <= y1) {
            c.index = static_cast<int>(kept_cities.size());
            c.owner = nullptr;
            kept_cities.push_back(std::move(c));
        } else {
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    int tx = c.x + dx, ty = c.y + dy;
                    if (tx >= 0 && ty >= 0 && tx < kMapW && ty < kMapH) {
                        m.tiles[ty * kMapW + tx] = plain_tile;
                    }
                }
            }
        }
    }
    m.cities = std::move(kept_cities);
    m.city_at.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    m.city_tile.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    for (auto& c : m.cities) {
        m.city_at[c.y * kMapW + c.x] = &c;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) m.city_tile[(c.y + dy) * kMapW + (c.x + dx)] = &c;
        }
    }
    for (auto& sd : m.sides) {
        sd.capital = nullptr;
        if (!sd.in_use) continue;
        City* cap = (sd.cap_x >= 0 && sd.cap_y >= 0 && sd.cap_x < kMapW && sd.cap_y < kMapH)
                            ? m.city_at[sd.cap_y * kMapW + sd.cap_x]
                            : nullptr;
        if (cap) {
            sd.capital = cap;
            cap->owner = &sd;
        } else {
            sd.in_use = false;
        }
    }

    std::vector<Site> kept_sites;
    for (auto& st : m.sites) {
        if (st.x >= x0 && st.x <= x1 && st.y >= y0 && st.y <= y1) {
            st.index = static_cast<int>(kept_sites.size());
            kept_sites.push_back(std::move(st));
        } else if (st.x >= 0 && st.y >= 0 && st.x < kMapW && st.y < kMapH) {
            m.tiles[st.y * kMapW + st.x] = plain_tile;
        }
    }
    m.sites = std::move(kept_sites);

    std::vector<Sign> kept_signs;
    for (auto& sg : m.signs) {
        if (sg.x >= x0 && sg.x <= x1 && sg.y >= y0 && sg.y <= y1) {
            sg.index = static_cast<int>(kept_signs.size());
            kept_signs.push_back(std::move(sg));
        }
    }
    m.signs = std::move(kept_signs);

    // Leave a 2-tile natural terrain ring around [x0..x1]x[y0..y1] so border
    // cities do not artificially become coastal ports, then convert outer tiles to ocean.
    int bx0 = std::max(0, x0 - 2), bx1 = std::min(kMapW - 1, x1 + 2);
    int by0 = std::max(0, y0 - 2), by1 = std::min(kMapH - 1, y1 + 2);
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                int k = y * kMapW + x;
                m.tiles[k] = water_tile;
                m.roads[k] = '\0';
                m.crossing[k] = false;
            }
        }
    }
}

std::unique_ptr<Game> NewGame(const std::string& data_dir, const std::string& scenario,
                              const NewGameOptions& opts) {
    auto gp = std::make_unique<Game>();
    Game& g = *gp;
    g.rng = Rng(opts.seed);
    g.data_dir = data_dir;
    armytype::Load(data_dir + "/TERRAIN0/ARMYTYPE.DAT", g.types);
    g.map = opts.memory_files
                    ? scn::LoadFromFiles(data_dir + "/" + scenario, scenario, *opts.memory_files)
                    : scn::Load(data_dir + "/" + scenario, scenario);
    g.greatest = opts.greatest;
    for (auto& [k, v] : opts.options) {
        if (int* f = g.map->options.Field(k)) *f = v;
    }
    for (auto& s : g.map->sides) {
        auto it = opts.sides.find(s.index);
        if (it != opts.sides.end() && s.in_use) {
            const SideSetup& o = it->second;
            if (!o.name.empty()) s.name = o.name;  // retyped on the setup screen
            if (o.off) {
                if (s.capital) s.capital->owner = nullptr;
                s.in_use = false;
            } else {
                s.computer = o.computer;
                s.level = o.level != kNone ? o.level : s.level;
                s.card = o.card;
            }
        }
    }
    if ((opts.active_sides > 0 && opts.active_sides < 8) || opts.crop_margin > 0) {
        ApplyScaleAndCrop(*g.map, opts);
    }

    for (auto& s : g.map->sides) {
        s.alive = s.in_use;
        s.owner_index = s.index;
        if (s.in_use) g.sides.push_back(&s);
    }
    // 79fa:0000: every side is observed unless the map is hidden and a human plays
    int humans = 0;
    for (Side* s : g.sides)
        if (!s->computer) humans++;
    bool observed = !(g.map->options.hidden_map != 0 && humans >= 1);
    for (Side* s : g.sides) s->observe = observed;

    for (auto& c : g.map->cities) c.owner_index = c.owner ? c.owner->index : kNone;
    if (g.map->options.quick_start != 0) DealCities(g);

    for (auto& c : g.map->cities) {
        c.slots = rules::CitySlots(c.produces, g.types, g.rng);
        c.defence = rules::CityDefence(static_cast<int>(c.slots.size()));
        c.producing = kNone;  // slot index being built (0-based)
        c.countdown = 0;
        c.vector_to = kNone;
    }
    scn::RefreshCityTiles(*g.map);

    // items 0-7 are the eight sides' standards (docs/rules.md > Heroes)
    for (size_t i = 0; i < g.map->items.size(); i++) {
        Item& it = g.map->items[i];
        if (i < 8 && it.type == rules::kItemStandard) it.standard_of = static_cast<int>(i);
    }

    diplomacy::Init(g);
    SetupGarrisons(g);
    site::Setup(g);
    for (Side* s : g.sides) {
        RevealStart(g, *s);
        if (g.map->options.hidden_map != 0 && opts.crop_margin > 0) {
            int bx0 = std::max(0, g.map->active_x0 - 2),
                bx1 = std::min(kMapW - 1, g.map->active_x1 + 2);
            int by0 = std::max(0, g.map->active_y0 - 2),
                by1 = std::min(kMapH - 1, g.map->active_y1 + 2);
            auto& mask = g.explored[s->index];
            if (static_cast<int>(mask.size()) == kMapW * kMapH) {
                for (int y = 0; y < kMapH; y++) {
                    for (int x = 0; x < kMapW; x++) {
                        if (x < bx0 || x > bx1 || y < by0 || y > by1) mask[y * kMapW + x] = 1;
                    }
                }
            }
        }
    }
    ai::StartGame(g);

    // the turn order (.SCN 0x163), as the scenario gives it; Random Turns
    // shuffles it at each round's end (turn_order, 8065:1732)
    g.turn_order = {0, 1, 2, 3, 4, 5, 6, 7};
    g.order_at = -1;  // the place in it of the side to play
    g.current = 0;    // index into g.sides
    g.side = g.sides.empty() ? nullptr : g.sides[0];
    g.path_cache = move::NewPathCache();
    return gp;
}

// Put a side with no city left out of the game (8065:18ab): its heroes drop
// what they carry, its armies are gone, it is vanquished, and its gold is 0.
// Every side then stands uneasy with it, both ways, state and proposal.
static Fallen Eliminate(Game& g, Side& side, bool boxed) {
    std::vector<Army*> mine;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        if (g.armies[i]->owner == side.index) mine.push_back(g.armies[i]);
    }
    Disband(g, side, mine);
    history::RecordDeed(g, &side, DeedType::kSideVanquished, side.index, 0, "");  // 8065:19f1
    int line = g.rng.Dice(1, kFallenLines, -1);
    g.log.push_back(side.name + " has been eliminated.");
    side.alive = false;
    side.gold = 0;
    // every pair with the fallen side, its pair with itself too (8065:18ab)
    for (int o = 0; o < 8; o++) {
        for (int k : {side.index * 8 + o, o * 8 + side.index}) {
            g.diplomacy.state[k] = diplomacy::kIntermediate;
            g.diplomacy.proposal[k] = diplomacy::kIntermediate;
        }
    }
    return Fallen{&side, line, boxed};
}

std::vector<Fallen> EliminateFallen(Game& g) {
    bool human = false;
    for (Side* s : g.sides)
        if (s->alive && !s->computer) human = true;
    std::vector<Fallen> fallen;
    for (Side* s : g.sides) {
        if (s->alive && SideCities(g, *s).empty())
            fallen.push_back(Eliminate(g, *s, human || !s->computer));
    }
    return fallen;
}

static void ApplyIncome(Game& g, Side& side) {
    side.income = Income(g, side);
    side.upkeep_total = Upkeep(g, side);
    side.gold = std::max(0, side.gold + side.income - side.upkeep_total);
}

// Place an army arriving from the road (6f8c:0123), or send it back; false
// if it was disbanded. It needs room on one of the city's four tiles --
// there is no spilling round a full city -- and the city must still be the
// side's (6f8c:0d4f). One sent back that fails again, at home, is
// disbanded. It keeps the moves it set off with.
static bool Deliver(Game& g, Army* army, City& city) {
    auto [x, y] = FreeTileIn(g, city, false);
    if (x == kNone || city.owner_index != army->owner) {
        if (army->returning || army->home_city == city.index) return false;
        City* home = g.map->City(army->home_city);
        army->returning = true;
        army->transit = Transit{kTransitTurns, home ? home->index : kNone};
        return true;
    }
    army->x = x;
    army->y = y;
    army->transit.reset();
    army->returning = false;
    return true;
}

// Step 5: city_production_turn (6f8c:0000), logged in side.produced and
// returned. The armies on the road move on first, from the top of the army
// table down; then each city counts down, and only while its countdown is
// above 0 (6f8c:04f3): one that reaches 0 builds if the side has gold and
// the army has room on the city's own four tiles (or goes vectored), and
// otherwise stays at 0 -- idle until its production is set again. A new
// army has its full move (6f8c:0dd7). Whether the city starts the next one
// is the caller's: a human's do (6f8c:06bd), a computer's wait for its AI.
static std::vector<Produced> RunProduction(Game& g, Side& side) {
    std::vector<Produced> arrived, built;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->transit && a->owner == side.index) {
            a->transit->turns--;
            if (a->transit->turns <= 0) {
                if (a->transit->dest == kStandard) {
                    auto [sx, sy] = StandardAt(g, side);
                    a->transit.reset();
                    if (sx != kNone &&
                        static_cast<int>(ArmiesAt(g, sx, sy).size()) < rules::kMaxStack) {
                        a->x = sx;
                        a->y = sy;
                        a->returning = false;
                        arrived.push_back(Produced{"arrived", a->type, kNone, true});
                    } else {
                        a->returning = true;
                        a->transit = Transit{kTransitTurns, a->home_city};
                    }
                } else {
                    City* dest = g.map->City(a->transit->dest);
                    a->transit.reset();
                    if (!dest || !Deliver(g, a, *dest))
                        a->disbanded = true;
                    else if (!a->transit)
                        arrived.push_back(Produced{"arrived", a->type, dest->index, false});
                }
            }
        }
    }

    for (City* c : SideCities(g, side)) {
        if (c->producing != kNone && c->countdown > 0) {
            c->countdown--;
            if (c->countdown <= 0) {
                c->countdown = 0;
                if (side.gold > 0) {
                    bool vectored = c->vector_to != kNone && c->vector_to != c->index;
                    auto [x, y] = FreeTileIn(g, *c, false);
                    if (x != kNone || vectored) {
                        const Slot& slot = c->slots[c->producing];
                        Army a = rules::ArmyFromSlot(slot, side.enhanced);
                        a.owner = side.index;
                        a.home_city = c->index;
                        a.moves = a.max_moves;
                        a.x = x;
                        a.y = y;
                        if (vectored) {
                            a.transit = Transit{kTransitTurns, c->vector_to};
                            a.x = kNone;
                            a.y = kNone;
                        }
                        g.Add(a);
                        built.push_back(
                                Produced{vectored ? "sent" : "built", a.type, c->index, false});
                    }
                }
            }
        }
    }

    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        if (g.armies[i]->disbanded) g.armies.erase(g.armies.begin() + i);
    }
    side.produced = arrived;
    for (auto& e : built) side.produced.push_back(e);
    return built;
}

// Step 6: movement reset (reset_movement, 8cc6:05fb). Each army gets its
// maximum (20 at sea) plus what it kept, 2 at most; a goal it stands on is
// forgotten; every hero carrying double movement adds each army's maximum
// again to every army on its tile -- a second such hero there adds it
// again; then 99 at most. 8c07:0113 wipes the turn's marks.
static void ResetMovement(Game& g, Side& side) {
    for (Army* a : g.armies) {
        if (a->owner == side.index) {
            a->done = false;
            a->offered = false;
        }
    }
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->owner == side.index && !a->transit) {
            int carry = std::min(a->moves, rules::kMoveCarry);
            a->moves = (a->at_sea ? rules::kSeaMoves : a->max_moves) + carry;
            if (a->target && a->target->first == a->x && a->target->second == a->y)
                a->target.reset();
        }
    }
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* h = g.armies[i];
        if (h->owner != side.index || h->type != armytype::kHero || h->transit) continue;
        bool double_move = false;
        for (Item* it : h->items)
            if (it->type == rules::kItemDoubleMove) double_move = true;
        if (!double_move) continue;
        auto here = ArmiesAt(g, h->x, h->y);
        if (static_cast<int>(here.size()) > rules::kMaxStack) here.resize(rules::kMaxStack);
        for (Army* a : here) a->moves += a->max_moves;
    }
    for (Army* a : g.armies) {
        if (a->owner == side.index && a->moves > rules::kMaxMove) a->moves = rules::kMaxMove;
    }
}

// A stack given the Defend order encamps as its side's next turn opens
// (8c07:0000): its tile gets the tower flag when it still has its full
// movement and stands on plain, forest, hills, a bridge, marsh, the tower
// terrain or a road.
static void Encamp(Game& g, Side& side) {
    static const std::set<int> kEncampOn = {move::kPlain,  move::kForest, move::kHills,
                                            move::kBridge, move::kMarsh,  move::kTower};
    for (Army* a : g.armies) {
        if (a->owner == side.index && a->fortified && !a->transit && a->x != kNone &&
            a->moves >= a->max_moves) {
            bool road = scn::RoadAt(*g.map, a->x, a->y) % 32 != 0;
            if (road || kEncampOn.count(scn::TerrainAt(*g.map, a->x, a->y))) {
                g.towers.insert(a->y * g.map->width + a->x);
            }
        }
    }
}

bool TowerAt(const Game& g, int x, int y) {
    return g.towers.count(y * g.map->width + x) > 0;
}

void TidyTowers(Game& g) {
    if (g.towers.empty()) return;
    std::set<int> held;
    for (Army* a : g.armies)
        if (a->x != kNone && !a->transit) held.insert(a->y * g.map->width + a->x);
    for (auto it = g.towers.begin(); it != g.towers.end();) {
        if (!held.count(*it))
            it = g.towers.erase(it);
        else
            ++it;
    }
}

bool HeroWithDoubleMoveAt(const Game& g, const Side& side, int x, int y) {
    for (Army* a : g.armies) {
        if (a->owner == side.index && !a->transit && a->x == x && a->y == y &&
            a->type == armytype::kHero) {
            for (Item* it : a->items)
                if (it->type == rules::kItemDoubleMove) return true;
        }
    }
    return false;
}

// At the start of every side's turn, before anything else, its armies
// standing in a city -- any city, ruins too -- are no longer at sea
// (8cc6:089d, from 8065:2123). It costs no movement: the reset that follows
// sees them ashore. A stack that took a city from the sea stays at sea until
// then.
static void ComeAshore(Game& g, Side& side) {
    for (Army* a : g.armies) {
        if (a->owner == side.index && a->at_sea && !a->transit && a->x != kNone &&
            CityAt(g, a->x, a->y))
            a->at_sea = false;
    }
}

// Before every side's turn the side's capital is put where it now is
// (67cc:10d7): while more than one side plays and the side holds cities but
// not its own capital, the city of its own nearest the capital it had; else
// its own capital again. The turn opens centred there.
void FollowCapital(Game& g, Side& side) {
    if (side.cap_now_x == kNone) {
        side.cap_now_x = side.cap_x;
        side.cap_now_y = side.cap_y;
    }
    int playing = 0;
    for (Side* s : g.sides)
        if (s->alive) playing++;
    if (playing != 1) {
        City* c = CityAt(g, side.cap_x, side.cap_y);
        if (c && c->razed) c = nullptr;
        if (!SideCities(g, side).empty() && (!c || c->owner_index != side.index)) {
            City* n = NearestCity(g, side.cap_now_x, side.cap_now_y, &side);
            if (n) {
                side.cap_now_x = n->x;
                side.cap_now_y = n->y;
            }
            return;
        }
    }
    side.cap_now_x = side.cap_x;
    side.cap_now_y = side.cap_y;
}

static void TurnQuest(Game& g, Side& side) {
    auto quest_result = quest::Notify(g, side, "turn");
    if (quest_result && !quest_result->failed.empty())
        g.log.push_back("Quest abandoned: " + quest_result->failed + ".");
}

bool StartTurn(Game& g, Side& side) {
    ComeAshore(g, side);
    FollowCapital(g, side);
    TidyExplored(g, side.index);
    side.diplo_news = diplomacy::Apply(g, side);
    for (auto& m : side.diplo_news) g.log.push_back(m);
    side.hero_offer.reset();
    side.production_due = false;
    if (side.computer) {
        if (SideCities(g, side).empty()) return false;
        auto offer = hero::Offer(g, side);
        if (offer) hero::Recruit(g, side, *offer);
        hero::CheckPromotions(g, side);
        ApplyIncome(g, side);
        RunProduction(g, side);
        ResetMovement(g, side);
        TidyTowers(g);
        Encamp(g, side);
        // the army cycle's cursor goes back to the capital (8c07:0113)
        if (side.ai) side.ai->cursor = std::make_pair(side.cap_now_x, side.cap_now_y);
        TurnQuest(g, side);
        return true;
    }
    ResetMovement(g, side);
    TidyTowers(g);
    Encamp(g, side);
    ApplyIncome(g, side);
    TurnQuest(g, side);
    side.promoted = hero::CheckPromotions(g, side);
    side.hero_offer = hero::Offer(g, side);
    side.production_due = true;
    return true;
}

void HumanProduction(Game& g, Side& side) {
    if (!side.production_due) return;
    side.production_due = false;
    if (g.turn == 1) return;
    for (auto& e : RunProduction(g, side)) {
        City* c = g.map->City(e.city);
        if (c && c->producing != kNone && c->producing < static_cast<int>(c->slots.size()))
            c->countdown = c->slots[c->producing].time;
    }
}

bool Producing(const City& c) {
    return c.producing != kNone && c.countdown > 0;
}

// The turn order for the next round (turn_order, 8065:1732): with Random
// Turns, 20 swaps of two sides picked by dice.
static void TurnOrder(Game& g) {
    g.turn_order = {0, 1, 2, 3, 4, 5, 6, 7};
    if (g.map->options.random_turns != 0) {
        for (int i = 0; i < 20; i++) {
            int a = g.rng.Dice(1, 8, -1), b = g.rng.Dice(1, 8, -1);
            std::swap(g.turn_order[a], g.turn_order[b]);
        }
    }
}

static Side* NextSide(Game& g, Side* leaving) {
    for (int n = 0; n < 3 * 8 + 1; n++) {
        if (g.order_at >= 7) {
            g.order_at = -1;
            g.turn++;
            // the round's end: the order, the fallen put out of the game, the end
            // looked for, the neutral cities' production, then the history
            TurnOrder(g);
            EndRound(g, leaving);
            if (g.ending.over) {
                g.side = nullptr;
                return nullptr;
            }
            history::Record(g);  // 8065:17f6 -> 6d51:0d60
        }
        g.order_at++;
        Side* side = g.map->Side(g.turn_order[g.order_at]);
        if (side && side->in_use && side->alive) {
            g.side = side;
            g.current = static_cast<int>(std::find(g.sides.begin(), g.sides.end(), side) -
                                         g.sides.begin());
            if (StartTurn(g, *side)) return side;
        }
    }
    g.side = nullptr;
    return nullptr;
}

Side* EndTurn(Game& g) {
    Side* leaving = g.side;
    if (leaving && leaving->alive && (leaving->computer || !g.won))
        diplomacy::ScoreUpdate(g, *leaving);
    // what the last round's end found is for the turn that followed it only
    g.ending = Ending{};
    return NextSide(g, leaving);
}

Side* Begin(Game& g) {
    g.order_at = -1;
    g.ending = Ending{};
    return NextSide(g, nullptr);
}

Side* NextHuman(Game& g) {
    for (int i = std::max(g.order_at, 0); i <= 7; i++) {
        Side* side = g.map->Side(g.turn_order[i]);
        if (side && side->in_use && side->alive && !side->computer) return side;
    }
    for (Side* s : g.sides)
        if (s->alive && !s->computer) return s;
    return nullptr;
}

static void RemoveArmies(Game& g, const std::vector<Army*>& dead) {
    std::set<const Army*> gone(dead.begin(), dead.end());
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        if (gone.count(g.armies[i])) g.armies.erase(g.armies.begin() + i);
    }
}

// Could these armies fly off together (1c8c:07fa)? A hero's flight item
// carries them all; else every army flies and none is at sea, or every army
// but the heroes flies and one of the fliers is not at sea.
static bool CanFlyOff(const Game& g, const std::vector<Army*>& armies) {
    bool hero_flight = false, all_fly = true, non_heroes_fly = true, free_flier = false;
    for (Army* a : armies) {
        bool flies = g.types.ById(a->type)->flies;
        if (a->type == armytype::kHero) {
            for (Item* it : a->items)
                if (it->type == rules::kItemFlight) hero_flight = true;
        }
        if (!flies || a->at_sea) all_fly = false;
        if (a->type != armytype::kHero && !flies) non_heroes_fly = false;
        if (flies && !a->at_sea) free_flier = true;
    }
    return hero_flight || all_fly || (non_heroes_fly && free_flier);
}

int SinkStranded(Game& g, Side& side, int x, int y) {
    auto wet = [&](int tx, int ty) {
        return scn::TerrainAt(*g.map, tx, ty) == move::kWater || scn::IsCrossing(*g.map, tx, ty);
    };
    std::map<int, std::vector<Army*>> tiles;
    for (Army* a : g.armies) {
        if (a->owner != side.index || a->transit || a->x == kNone || a->x < 0 || a->y < 0) continue;
        if (x != kNone && (a->x != x || a->y != y)) continue;
        if (wet(a->x, a->y)) tiles[a->y * g.map->width + a->x].push_back(a);
    }
    std::vector<Army*> dead;
    for (auto& [k, here] : tiles) {
        if (CanFlyOff(g, here)) continue;
        for (Army* a : here) {
            if (!a->at_sea && a->type != armytype::kHero && !g.types.ById(a->type)->flies)
                dead.push_back(a);
        }
    }
    RemoveArmies(g, dead);
    return static_cast<int>(dead.size());
}

Sign* SignAt(Game& g, int x, int y) {
    for (auto& s : g.map->signs)
        if (s.x == x && s.y == y) return &s;
    return nullptr;
}

void Disband(Game& g, Side& side, const std::vector<Army*>& armies) {
    for (Army* a : armies) {
        if (a->type == armytype::kHero) {
            hero::DropItems(g, a, a->x, a->y, side.index);
            if (side.quest && side.quest->hero == a) side.quest.reset();
        }
    }
    RemoveArmies(g, armies);
}

int Loot(const Game& g, const Side* loser) {
    if (!loser) return 0;
    int n = static_cast<int>(SideCities(g, *loser).size());
    int share = n > 1 ? loser->gold / n : loser->gold;
    return share / 2;
}

Battle DecideAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    combat::Lines l = combat::BattleLines(g, stack, x, y);
    int me = stack.empty() ? kNone : stack[0]->owner;
    if (l.city && me != kNone) l.city->attacked |= 1 << me;
    Battle result = combat::Resolve(g, l.attackers, l.defenders, x, y);
    result.lines.attackers = l.attackers;
    result.lines.defenders = l.defenders;
    result.lines.city = l.city;
    Side* loser = l.def_owner != kNone ? g.map->Side(l.def_owner) : nullptr;
    if (result.won && l.city && loser) result.loot = Loot(g, loser);
    // the stack's hero (stack_movement_mode, 1c8c:0529): the leader if a
    // hero, else its first hero
    for (Army* a : stack) {
        if (a->type == armytype::kHero) {
            result.hero = a;
            break;
        }
    }
    result.fought = Battle::Fought{stack, x, y, l.def_owner};
    return result;
}

void ChargeAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    City* city = CityAt(g, x, y);
    int t = city && !city->razed ? move::kCity : scn::TerrainAt(*g.map, x, y);
    int cost = std::max(2, t >= 0 && t < 12 ? move::kCost[t] : 0);
    move::Mode m = move::StackMode(g, stack);
    if (m.mode != move::kFlying && !m.at_sea && (t == move::kWater || t == move::kShore)) {
        for (Army* a : stack) {
            bool flight = false;
            if (a->type == armytype::kHero) {
                for (Item* it : a->items)
                    if (it->type == rules::kItemFlight) flight = true;
            }
            if (!g.types.ById(a->type)->flies && !flight) {
                a->at_sea = true;
                a->moves = 0;
            }
        }
    }
    for (Army* a : stack) {
        a->moves = std::max(0, a->moves - cost);
        a->target.reset();
    }
}

Battle ResolveAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    Battle b = DecideAttack(g, stack, x, y);
    ApplyAttack(g, b);
    return b;
}

static Army* Medal(Game& g, const Battle& result, int side);

Battle& ApplyAttack(Game& g, Battle& result) {
    if (!result.fought) return result;
    Battle::Fought f = *result.fought;
    result.fought.reset();
    const auto& stack = f.stack;
    int x = f.x, y = f.y, def_owner = f.def_owner;
    const auto& attackers = result.lines.attackers;
    const auto& defenders = result.lines.defenders;
    City* city = result.lines.city;
    // the city on the tile, ruins too (828e:044a)
    City* tile_city = g.map->city_tile[y * g.map->width + x];
    const auto& boats = result.boats;

    auto fallen = [&](Army* h) {
        history::RecordDeed(g, g.map->Side(h->owner == kNone ? 8 : h->owner), DeedType::kHeroKilled,
                            tile_city ? tile_city->index : history::kKilledInBattle, 0, h->name);
    };
    int mine = stack.empty() ? kNone : stack[0]->owner;
    {
        int heroes_lost = 0;
        for (Army* dd : result.dead_defenders)
            if (dd->type == armytype::kHero) heroes_lost++;
        ai::RecordBattle(g, def_owner, mine, x, y, heroes_lost,
                         static_cast<int>(result.dead_defenders.size()),
                         result.won && !defenders.empty(), city != nullptr);
    }
    for (Army* a : result.dead_attackers) history::Tally(g, a, def_owner, boats.count(a) > 0);
    for (Army* d : result.dead_defenders) history::Tally(g, d, mine, boats.count(d) > 0);
    // a big battle won at a city goes down in history (67cc:0af5): eight
    // armies or more, four of them defending -- by the hero's name when a hero
    // led the stack
    if (tile_city && result.won && mine != kNone && attackers.size() + defenders.size() >= 8 &&
        defenders.size() >= 4) {
        Side* side = g.map->Side(mine);
        if (result.hero)
            history::RecordDeed(g, side, DeedType::kCityWon, history::kWonByName, tile_city->index,
                                result.hero->name);
        else
            history::RecordDeed(g, side, DeedType::kCityWon, mine, tile_city->index, "");
    }
    // the fallen heroes' items drop on the tile fought for, attacker's too
    // (67cc:16cd with after_battle's x, y)
    for (Army* a : result.dead_attackers) {
        if (a->type == armytype::kHero) {
            fallen(a);
            hero::DropItems(g, a, x, y, mine);
        }
    }
    for (Army* d : result.dead_defenders) {
        if (d->type == armytype::kHero) {
            fallen(d);
            hero::DropItems(g, d, x, y, mine);
        }
    }
    hero::BattleExperience(g, attackers, defenders, result, city != nullptr);
    Side* my_side =
            g.map->Side(stack.empty() ? 0 : (stack[0]->owner == kNone ? 0 : stack[0]->owner));
    {
        quest::Event ev;
        ev.stack = attackers;
        ev.killed = result.dead_defenders;
        result.quest = quest::Notify(g, *my_side, "battle", ev);
    }

    RemoveArmies(g, result.dead_attackers);
    RemoveArmies(g, result.dead_defenders);
    TidyTowers(g);

    if (result.won && city) {
        Side* winner = g.map->Side(stack[0]->owner);
        Side* loser = def_owner != kNone ? g.map->Side(def_owner) : nullptr;
        if (loser) {
            int l = result.loot;
            winner->gold += l;
            loser->gold = std::max(0, loser->gold - 2 * l);
        }
        // whom the city was taken from (+0x2f): its owner, or no one when that
        // side takes its own back
        if (city->owner_index != kNone) {
            city->claim = city->claim == winner->index ? rules::kNeutral : city->owner_index;
        }
        city->attacked = 0;
        city->countdown = 0;
        city->vector_to = kNone;
        // every city that was sending its armies here stops: no vector, and
        // nothing in production either (67cc:0a6b)
        for (auto& c : g.map->cities) {
            if (c.vector_to == city->index) {
                c.vector_to = kNone;
                c.producing = kNone;
                c.countdown = 0;
            }
        }
        city->owner_index = winner->index;
        scn::SetCityTiles(*g.map, *city);
        move::Invalidate(g);
        result.captured = city;
        // a computer's quest hero taking its quest city (5e97:038d) razes it
        bool handled = winner->computer && ai::QuestCapture(g, *winner, *city, result.attackers);
        if (!handled && winner->computer) {
            quest::Event ev;
            ev.city = city;
            ev.stack = result.attackers;
            auto q = quest::Notify(g, *winner, "occupy", ev);
            if (q) result.quest = q;
        }
    }

    // The survivors walk into the tile they just cleared -- as many as fit.
    if (result.won) {
        g.towers.erase(y * g.map->width + x);
        int room = rules::kMaxStack - static_cast<int>(ArmiesAt(g, x, y).size());
        for (Army* a : result.attackers) {
            if (room <= 0) break;
            a->x = x;
            a->y = y;
            room--;
        }
        // and the winner sees round what it took (67cc:0a6b calls 8611:1298):
        // the tile fought for, then each of a city's four, 2 out from a city
        if (g.map->options.hidden_map != 0 && mine != kNone) {
            bool flying = move::ModeOf(g, result.attackers) == move::kFlying;
            int found = Reveal(g, mine, x, y, flying);
            if (city) {
                for (int dx = 0; dx <= 1; dx++)
                    for (int dy = 0; dy <= 1; dy++)
                        found += Reveal(g, mine, city->x + dx, city->y + dy, false);
            }
            if (found > 0) move::Invalidate(g);
        }
    }
    result.medal = Medal(g, result, mine);
    return result;
}

// An ordinary army: not a hero, not a magical creature.
static bool Ordinary(const Game& g, const Army* a) {
    return a->type != armytype::kHero && g.types.ById(a->type)->bonus[48] == 0;
}

// A medal for a battle won (auto_ui_medal_effect, 67cc:2274): a stack with a
// hero and no army at sea that won a fight worth the name -- two armies a
// side, or before turn 20 a first defender above strength 3 -- gives one of
// its ordinary armies a medal, 40% of the time less 5 for each of the side's
// ordinary armies with one already, 12% at least. A medal is +1 strength
// (9 at most), or +1 move past strength 14; four at most. Returns the army.
static Army* Medal(Game& g, const Battle& result, int side) {
    if (side == kNone || !result.won) return nullptr;
    const auto& survivors = result.attackers;
    for (Army* a : survivors)
        if (a->at_sea) return nullptr;
    size_t n_atk = result.lines.attackers.size(), n_def = result.lines.defenders.size();
    bool counted =
            (n_atk >= 2 && n_def >= 2) || (g.turn < 20 && n_def >= 1 && result.def_strength0 > 3);
    if (!counted) return nullptr;
    std::vector<Army*> plain;
    for (Army* a : survivors)
        if (Ordinary(g, a)) plain.push_back(a);
    if (plain.empty()) return nullptr;
    int decorated = 0;
    for (Army* a : g.armies)
        if (a->owner == side && Ordinary(g, a) && a->medals > 0) decorated++;
    int chance = std::max(12, 40 - 5 * decorated);
    if (!(g.rng.Dice(1, 100, 0) < chance) || !result.hero) return nullptr;
    Army* a = plain[g.rng.Dice(1, static_cast<int>(plain.size()), -1)];
    if (a->medals >= 4) return nullptr;
    a->medals++;
    if (a->strength > 14)
        a->max_moves++;
    else
        a->strength = std::min(9, a->strength + 1);
    return a;
}

// A production type's "value" is half its purchase price (ARMYTYPE +30).
static int SlotValue(const Game& g, const Slot& slot) {
    return std::abs(g.types.ById(slot.type)->price) / 2;
}

static void Recompute(Game& g, City& city) {
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    if (city.producing != kNone && city.producing >= static_cast<int>(city.slots.size())) {
        city.producing = kNone;
        city.countdown = 0;
    }
}

void AddDiploScore(Game& g, Side& side, int n) {
    side.diplo_score += n;
}

std::shared_ptr<QuestResult> Occupy(Game& g, Side& side, City& city,
                                    const std::vector<Army*>& stack) {
    quest::Event ev;
    ev.city = &city;
    ev.stack = stack;
    return quest::Notify(g, side, "occupy", ev);
}

Spoils Pillage(Game& g, Side& side, City& city, const std::vector<Army*>& stack) {
    Spoils out;
    if (city.slots.empty()) return out;
    Slot slot = city.slots.back();  // slots are sorted cheapest first
    city.slots.pop_back();
    int gold = SlotValue(g, slot);
    out.gold = gold;
    out.lost.emplace_back(slot.type, gold);
    side.gold += gold;
    AddDiploScore(g, side, g.rng.Dice(1, 5, 0));
    Recompute(g, city);
    quest::Event ev;
    ev.city = &city;
    ev.gold = gold;
    ev.stack = stack;
    quest::Notify(g, side, "pillage", ev);
    return out;
}

Spoils Sack(Game& g, Side& side, City& city, const std::vector<Army*>& stack) {
    Spoils out;
    if (city.slots.size() < 2) return out;
    int gold = 0;
    for (size_t i = 1; i < city.slots.size(); i++) {
        int v = SlotValue(g, city.slots[i]);
        out.lost.emplace_back(city.slots[i].type, v);
        gold += v;
    }
    city.slots.resize(1);
    out.gold = gold;
    side.gold += gold;
    AddDiploScore(g, side, g.rng.Dice(1, 10, 5));
    Recompute(g, city);
    quest::Event ev;
    ev.city = &city;
    ev.gold = gold;
    ev.stack = stack;
    quest::Notify(g, side, "pillage", ev);
    return out;
}

// The city becomes ruins (649c:016b).
static void MakeRuins(Game& g, City& city) {
    city.razed = true;
    city.razed_by = city.owner_index == kNone ? 0 : city.owner_index;
    city.owner_index = kNone;
    city.claim = rules::kNeutral;
    city.slots.clear();
    city.producing = kNone;
    city.countdown = 0;
    city.vector_to = kNone;
    city.defence = 0;
    city.income = 0;
    // every city sending its armies here stops: no vector, nothing in
    // production (649c:016b)
    for (auto& c : g.map->cities) {
        if (c.vector_to == city.index) {
            c.vector_to = kNone;
            c.producing = kNone;
            c.countdown = 0;
        }
    }
    scn::SetCityTiles(*g.map, city);
    move::Invalidate(g);
}

void Raze(Game& g, Side& side, City& city, const std::vector<Army*>& stack, bool own_city) {
    MakeRuins(g, city);
    if (own_city) {
        AddDiploScore(g, side, g.rng.Dice(1, 25, 25));
        return;
    }
    AddDiploScore(g, side, g.rng.Dice(1, 15, 10));
    quest::Event ev;
    ev.city = &city;
    ev.stack = stack;
    quest::Notify(g, side, "raze", ev);
}

void Resign(Game& g, Side& side) {
    for (auto& c : g.map->cities) {
        if (!c.razed && c.owner_index == side.index) MakeRuins(g, c);
    }
    Disband(g, side, SideArmies(g, side));
    if (g.map->options.hidden_map != 0) {
        g.explored[side.index].assign(static_cast<size_t>(g.map->width) * g.map->height, 1);
    }
}

// Tiles a side has seen: a mask per side.

static std::vector<uint8_t>* MaskFor(Game& g, int side, bool make) {
    auto it = g.explored.find(side);
    if (it != g.explored.end()) return &it->second;
    if (!make) return nullptr;
    auto& m = g.explored[side];
    m.assign(static_cast<size_t>(g.map->width) * g.map->height, 0);
    return &m;
}

bool HasActiveHuman(const Game& g) {
    for (const Side* s : g.sides) {
        if (s && s->alive && !s->computer && !SideCities(g, *s).empty()) return true;
    }
    return false;
}

int ViewerSide(const Game& g, const Side* player) {
    if (!HasActiveHuman(g)) return kNone;
    return player ? player->index : kNone;
}

bool Seen(const Game& g, int side, int x, int y) {
    if (g.map->options.hidden_map == 0) return true;
    if (x < 0 || y < 0 || x >= g.map->width || y >= g.map->height) return false;
    if (side == kNone) return true;
    int idx = y * g.map->width + x;
    auto it = g.explored.find(side);
    if (it != g.explored.end() && it->second[idx] == 1) return true;
    if (side >= 0 && side < 8) {
        for (int b = 0; b < 8; b++) {
            if (b != side && g.diplomacy.allied[side * 8 + b]) {
                auto bit = g.explored.find(b);
                if (bit != g.explored.end() && bit->second[idx] == 1) return true;
            }
        }
    }
    return false;
}

int Reveal(Game& g, int side, int x, int y, bool flying) {
    if (g.map->options.hidden_map == 0) return 0;
    auto& mask = *MaskFor(g, side, true);
    bool on_city = x >= 0 && y >= 0 && x < g.map->width && y < g.map->height &&
                   g.map->city_tile[y * g.map->width + x];
    int r = (flying || on_city) ? 2 : 1;
    int found = 0;
    for (int ty = y - r; ty <= y + r; ty++) {
        for (int tx = x - r; tx <= x + r; tx++) {
            if (tx >= 0 && ty >= 0 && tx < g.map->width && ty < g.map->height) {
                int k = ty * g.map->width + tx;
                if (!mask[k]) {
                    mask[k] = 1;
                    found++;
                }
            }
        }
    }
    return found;
}

const int kFogDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
const int kFogDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
const int kFogCell[256] = {
    255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255,
    255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255,
    255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255,
    255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255,
    255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,   3,
    12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  4,   4,
    4,   4,   4,   4,   4,   13,  4,   4,   4,   4,   8,   8,   8,   9,   255, 255, 255, 255, 255,
    255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,
    255, 255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255,
    255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,
    3,   12,  255, 11,  255, 11,  255, 11,  255, 1,   255, 11,  255, 11,  255, 11,  255, 1,   255,
    11,  255, 11,  255, 11,  255, 1,   255, 11,  255, 11,  3,   6,   3,   2,   255, 11,  255, 11,
    255, 11,  255, 1,   255, 11,  255, 11,  255, 11,  255, 1,   4,   5,   4,   5,   4,   5,   4,
    0,   4,   5,   4,   5,   8,   7,   8,   14,
};

int FogCell(const Game& g, int side, int x, int y) {
    if (Seen(g, side, x, y)) return kNone;
    int bits = 0;
    for (int i = 0; i < 8; i++) {
        int nx = x + kFogDx[i], ny = y + kFogDy[i];
        if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height || !Seen(g, side, nx, ny))
            bits += 1 << i;
    }
    int cell = kFogCell[bits];
    return cell == kFogNone ? kNone : cell;
}

void TidyExplored(Game& g, int side) {
    if (g.map->options.hidden_map == 0) return;
    auto& mask = *MaskFor(g, side, true);
    int map_width = g.map->width, map_height = g.map->height;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < map_width; x++) {
            for (int y = 0; y < map_height; y++) {
                int k = y * map_width + x;
                if (!mask[k]) {
                    int bits = 0;
                    for (int i = 0; i < 8; i++) {
                        int nx = x + kFogDx[i], ny = y + kFogDy[i];
                        if (nx < 0 || ny < 0 || nx >= map_width || ny >= map_height ||
                            !mask[ny * map_width + nx])
                            bits += 1 << i;
                    }
                    if (kFogCell[bits] == kFogNone) mask[k] = 1;
                }
            }
        }
    }
}

void RevealStart(Game& g, const Side& side) {
    if (g.map->options.hidden_map == 0) return;
    for (City* c : SideCities(g, side)) Reveal(g, side.index, c->x, c->y, false);
    for (Army* a : SideArmies(g, side))
        if (!a->transit) Reveal(g, side.index, a->x, a->y, false);
    TidyExplored(g, side.index);
}

void AcceptSurrender(Game& g, Side& side) {
    for (Side* s : g.sides)
        if (s->computer) s->alive = false;
    g.won = true;
    history::RecordDeed(g, &side, DeedType::kSideVictorious, side.index, 0, "");
}

bool SurrenderPending(const Game& g) {
    return g.surrender_offered && !g.surrender_shown && !g.won;
}

Ending EndRound(Game& g, Side* last) {
    auto fallen = EliminateFallen(g);
    Ending ending = CheckEnd(g);
    ending.fallen = fallen;
    if (!ending.message.empty()) g.log.push_back(ending.message);
    g.ending = ending;
    NeutralProduction(g, last ? last : g.map->Side(kSetupPlayer));
    return ending;
}

bool SetCityProduction(Game& g, const Side* who, City& c, int slot) {
    if (slot < 0 || slot >= static_cast<int>(c.slots.size())) return false;
    if ((who ? who->gold : 0) < c.slots[slot].cost + 30 && g.turn > 5) return false;
    SetProduction(g, c, slot);
    return true;
}

// A neutral city's garrison is built (produce_garrison, 6bd8:0381): on its
// top-left tile, room or not, with no moves, and the city builds nothing
// more until it is set to again.
static void ProduceGarrison(Game& g, City& c, const Side* who) {
    if (c.producing != kNone && c.producing < static_cast<int>(c.slots.size())) {
        Army a = rules::ArmyFromSlot(c.slots[c.producing], who && who->enhanced);
        a.x = c.x;
        a.y = c.y;
        a.owner = c.owner_index;
        a.moves = 0;
        a.home_city = c.index;
        g.Add(a);
    }
    c.producing = kNone;
    c.countdown = 0;
}

void NeutralProduction(Game& g, const Side* who) {
    if (!(g.map->options.neutral_cities > 1)) return;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == kNone && !c.razed && c.attacked) {
            if (c.countdown != 0) {
                c.countdown--;
                if (c.countdown == 0) ProduceGarrison(g, c, who);
            }
            const Slot* slot =
                    c.producing != kNone && c.producing < static_cast<int>(c.slots.size())
                            ? &c.slots[c.producing]
                            : nullptr;
            bool building = slot && c.countdown != 0 && c.countdown != slot->time;
            if (!building &&
                std::min(static_cast<int>(ArmiesAt(g, c.x, c.y).size()), rules::kMaxStack) < 4) {
                int best = rules::BestSlot(c.slots, 2, g.types, who && who->enhanced);
                if (best != kNone) SetCityProduction(g, who, c, best);
            }
        }
    }
}

Ending CheckEnd(Game& g) {
    // a human side out of the game is the computer's from now on, so that its
    // fall is told only once
    std::vector<Side*> humans, computers;
    bool fallen_human = false;
    for (Side* s : g.sides) {
        if (s->alive) {
            (s->computer ? computers : humans).push_back(s);
        } else if (!s->computer) {
            s->computer = true;
            fallen_human = true;
        }
    }
    int standing = 0;
    for (auto& c : g.map->cities)
        if (!c.razed) standing++;

    Ending e;
    if (humans.empty() && computers.empty()) {
        g.over = true;
        e.over = true;
        e.message = "Alas! No more players are left!";
        return e;
    }
    // the last human has fallen and the computers fight on (8065:1c6f); a game
    // that never had a human said so as it began (group 14) instead
    if (!g.won && humans.empty() && fallen_human && !g.no_humans_said) {
        g.no_humans_said = true;
        e.no_humans = true;
    }
    // a lone human with more than half the cities has won -- once; the game
    // goes on, for the kingdom to be looked over
    if (!g.won && humans.size() == 1 && computers.empty()) {
        int mine = static_cast<int>(SideCities(g, *humans[0]).size());
        if (mine * 2 > standing) {
            g.won = true;
            history::RecordDeed(g, humans[0], DeedType::kSideVictorious, humans[0]->index, 0, "");
            e.won = true;
            e.winner = humans[0];
            e.message = humans[0]->name + " rules the world!";
        }
    }
    // the last computer side has triumphed: it is handed to the player, its
    // turn opens as a human's, and the world can be looked over
    if (!g.won && humans.empty() && computers.size() == 1) {
        Side* winner = computers[0];
        g.won = true;
        winner->computer = false;
        history::RecordDeed(g, winner, DeedType::kSideVictorious, winner->index, 0, "");
        e.triumph = true;
        e.winner = winner;
        e.message = winner->name + " has triumphed!";
    }
    // offered once, while computers still play
    if (!g.won && humans.size() == 1 && !computers.empty() && !g.surrender_offered) {
        int mine = static_cast<int>(SideCities(g, *humans[0]).size());
        int biggest = 0;
        for (Side* s : computers)
            biggest = std::max(biggest, static_cast<int>(SideCities(g, *s).size()));
        // every city counts here, ruins too (8065:1aed reads city_count)
        int all = static_cast<int>(g.map->cities.size());
        if (mine > all / 2 && mine > biggest + all / 8) {
            g.surrender_offered = true;
            e.surrender = true;
            e.message = "Your enemies offer their surrender!";
        }
    }
    return e;
}

std::shared_ptr<SearchResult> SearchHere(Game& g, const std::vector<Army*>& stack, bool human) {
    if (stack.empty()) return nullptr;
    return site::Search(g, stack, stack[0]->x, stack[0]->y, human);
}

std::string DescribeSearch(const SearchResult* r) {
    if (!r) return "";
    const std::string& name = r->site->name;
    if (r->kind == "temple") {
        return r->blessed > 0 ? name + " blesses " + std::to_string(r->blessed) + " of your armies."
                              : name + " has blessed them already.";
    }
    if (r->kind == "no hero") return name + " can only be searched by a hero.";
    if (r->kind == "killed")
        return "Your hero is slain in " + name + " by a " +
               (r->monster ? r->monster->name : "guardian") + "!";
    if (r->kind == "gold") return name + " yields " + std::to_string(r->gold) + " gold!";
    if (r->kind == "item")
        return "Your hero finds the " + (r->item ? r->item->name : std::string("treasure")) +
               " in " + name + "!";
    if (r->kind == "allies")
        return std::to_string(r->armies.size()) + " " + r->type->name + " join you at " + name +
               "!";
    if (r->kind == "sage") return "A sage dwells in " + name + ".";
    return name + " holds nothing.";
}

void SetProduction(Game& g, City& city, int slot_index) {
    city.producing = slot_index;
    city.countdown = slot_index != kNone ? city.slots[slot_index].time : 0;
    if (slot_index == kNone) city.vector_to = kNone;  // vectoring only sticks while building
}

std::vector<City*> VectoredTo(Game& g, const City* dest, const Side* side) {
    std::vector<City*> out;
    for (auto& c : g.map->cities) {
        if (!dest) {
            if (c.vector_to == kStandard && side && c.owner_index == side->index) out.push_back(&c);
        } else if (c.vector_to == dest->index && &c != dest) {
            out.push_back(&c);
        }
    }
    return out;
}

std::pair<int, int> StandardAt(const Game& g, const Side& side) {
    if (side.index < static_cast<int>(g.map->items.size())) {
        const Item& it = g.map->items[side.index];
        if (it.status == 1 && it.planted && it.x != kNone) return {it.x, it.y};
    }
    return {kNone, kNone};
}

bool PlantFlag(Game& g, Side& side, Army* h) {
    int t = scn::TerrainAt(*g.map, h->x, h->y);
    if (t == move::kWater || t == move::kShore || t == move::kCity || t == move::kSite)
        return false;
    for (auto& it : g.map->items)
        if (it.planted && it.x == h->x && it.y == h->y) return false;
    if (side.index >= static_cast<int>(g.map->items.size())) return false;
    Item* std = &g.map->items[side.index];
    auto i = std::find(h->items.begin(), h->items.end(), std);
    if (i != h->items.end()) {
        h->items.erase(i);
        std->status = 1;
        std->x = h->x;
        std->y = h->y;
        std->planted = true;
        return true;
    }
    return false;
}

bool VectorToStandard(Game& g, City& city, const Side& side) {
    if (StandardAt(g, side).first == kNone) return false;
    if (city.vector_to != kStandard &&
        static_cast<int>(VectoredTo(g, nullptr, &side).size()) >= kMaxVectoredTo)
        return false;
    city.vector_to = kStandard;
    return true;
}

bool Vector(Game& g, City& city, City* dest_city) {
    if (dest_city == &city) dest_city = nullptr;
    if (dest_city && city.vector_to != dest_city->index &&
        static_cast<int>(VectoredTo(g, dest_city).size()) >= kMaxVectoredTo) {
        return false;
    }
    city.vector_to = dest_city ? dest_city->index : kNone;
    return true;
}

City* NearestCity(Game& g, int x, int y, const Side* side, const Side* seer) {
    City* best = nullptr;
    int best_d = kNone;
    for (auto& c : g.map->cities) {
        bool ok = !side || c.owner_index == side->index;
        if (ok && seer) {
            int s = seer->index;
            ok = Seen(g, s, c.x, c.y) || Seen(g, s, c.x + 1, c.y) || Seen(g, s, c.x, c.y + 1) ||
                 Seen(g, s, c.x + 1, c.y + 1);
        }
        if (ok) {
            int d = move::Distance(x, y, c.x, c.y);
            if (best_d == kNone || d < best_d) {
                best = &c;
                best_d = d;
            }
        }
    }
    return best;
}

std::vector<const ArmyType*> BuyableTypes(const Game& g) {
    std::vector<const ArmyType*> out;
    for (auto* a : g.types.List())
        if (a->price >= 0) out.push_back(a);
    return out;
}

std::string CannotBuy(const Game& g, const Side& side, const City& city, const ArmyType& a) {
    for (auto& slot : city.slots)
        if (slot.type == a.id) return "has it";
    if (side.gold < a.price) return "too dear";
    return "";
}

int BuySlot(const City& city) {
    if (static_cast<int>(city.slots.size()) < rules::kProductionSlots)
        return static_cast<int>(city.slots.size());
    return 0;
}

void BuyProduction(Game& g, Side& side, City& city, int n, int type_id) {
    const ArmyType* a = g.types.ById(type_id);
    bool building = city.producing != kNone;
    int was = city.producing;
    Slot s{a->id, a->name, a->strength, a->time, a->cost, a->move, std::abs(a->price)};
    if (n >= static_cast<int>(city.slots.size())) city.slots.resize(n + 1);
    city.slots[n] = s;
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    side.gold -= a->price;
    // the slot being built was bought over
    if (building && was == n) {
        city.producing = kNone;
        city.countdown = 0;
        city.vector_to = kNone;
    }
}

void SortProduction(City& city) {
    int building = city.producing;
    auto& s = city.slots;
    for (int i = 1; i < static_cast<int>(s.size()); i++) {
        int j = i;
        while (j > 0 && s[j].price < s[j - 1].price) {
            std::swap(s[j], s[j - 1]);
            if (building == j)
                building = j - 1;
            else if (building == j - 1)
                building = j;
            j--;
        }
    }
    if (city.producing != kNone) city.producing = building;
}

void RenameCity(Game& g, City& city, const std::string& name) {
    city.name = name;
}

}  // namespace game
}  // namespace w2
