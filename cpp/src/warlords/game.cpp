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

bool Army::hero() const {
    return type == armytype::HERO;
}

Game::Game() = default;
Game::~Game() = default;

Army* Game::make(const Army& a) {
    pool.push_back(std::make_unique<Army>(a));
    return pool.back().get();
}

Army* Game::add(const Army& a) {
    std::set<int> used;
    for (Army* b : armies) used.insert(b->slot);
    int slot = 0;
    while (used.count(slot)) slot++;
    Army* p = make(a);
    p->slot = slot;
    size_t i = armies.size();
    while (i > 0 && armies[i - 1]->slot > slot) i--;
    armies.insert(armies.begin() + i, p);
    return p;
}

bool Game::alive(const Army* a) const {
    return a && std::find(armies.begin(), armies.end(), a) != armies.end();
}

void Game::remove(const Army* a) {
    auto it = std::find(armies.begin(), armies.end(), a);
    if (it != armies.end()) armies.erase(it);
}

namespace game {

static const int kTransitTurns = 2;  // a vectored army arrives two turns later

static int Key(const Game& g, int x, int y) {
    return y * g.map->width + x;
}

std::vector<Army*> armiesAt(const Game& g, int x, int y) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (!a->transit && a->x == x && a->y == y) out.push_back(a);
    }
    return out;
}

City* cityAt(const Game& g, int x, int y) {
    if (x < 0 || y < 0 || x >= g.map->width || y >= g.map->height) return nullptr;
    return g.map->cityTile[Key(g, x, y)];
}

void setCityOwner(Game& g, City& city, int side_index) {
    city.ownerIndex = side_index;
    move::invalidate(g);
}

std::vector<City*> sideCities(const Game& g, const Side& side) {
    std::vector<City*> out;
    for (auto& c : g.map->cities)
        if (c.ownerIndex == side.index) out.push_back(&c);
    return out;
}

std::vector<Army*> sideArmies(const Game& g, const Side& side) {
    std::vector<Army*> out;
    for (Army* a : g.armies)
        if (a->owner == side.index) out.push_back(a);
    return out;
}

int itemBonus(const Game& g, const Side& side, int item_type) {
    int total = 0;
    for (Army* a : g.armies) {
        if (a->owner == side.index) {
            for (Item* it : a->items)
                if (it->type == item_type) total += std::max(1, it->value);
        }
    }
    return total;
}

std::pair<int, int> freeTileIn(Game& g, const City& city, bool spill, std::pair<int, int>* last) {
    static const int kD[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (auto& d : kD) {
        int x = city.x + d[0], y = city.y + d[1];
        if (x < g.map->width && y < g.map->height &&
            static_cast<int>(armiesAt(g, x, y).size()) < rules::MAX_STACK) {
            return {x, y};
        }
    }
    if (!spill) return {NONE, NONE};
    int lx = NONE, ly = NONE;
    for (int i = 0; i < 20; i++) {
        int x = city.x + g.rng.dice(1, 3, -2);
        int y = city.y + g.rng.dice(1, 3, -2);
        if (x >= 0 && y >= 0 && x < g.map->width && y < g.map->height) {
            lx = x;
            ly = y;
            if (armiesAt(g, x, y).empty()) return {x, y};
        }
    }
    if (last) *last = {lx, ly};
    return {NONE, NONE};
}

int income(const Game& g, const Side& side) {
    int total = 0;
    auto cities = sideCities(g, side);
    for (City* c : cities) total += c->income;
    int per_city = itemBonus(g, side, rules::ITEM_GOLD_PER_CITY);
    return total + static_cast<int>(cities.size()) * per_city;
}

int upkeep(const Game& g, const Side& side) {
    int total = 0;
    for (Army* a : g.armies) {
        if (a->owner == side.index && !a->transit) {
            int u = a->upkeep;
            if (a->atSea) u = std::max(rules::SEA_MIN_UPKEEP, u);
            total += u;
        }
    }
    return total;
}

// One starting army per city (setup_garrisons, 6bd8:00cc), the last city
// first (docs/rules.md > Starting garrisons).
static void SetupGarrisons(Game& g) {
    Side* setup = g.map->side(SETUP_PLAYER);
    bool enhanced = setup && setup->enhanced;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.razed) continue;
        bool owned = c.ownerIndex != NONE;
        auto level = rules::garrisonLevel(owned, g.map->options.neutralCities, g.rng);
        int slot = NONE;
        if (level && !c.slots.empty()) {
            slot = rules::bestSlot(c.slots, rules::GARRISON_PURPOSE[*level], g.types, enhanced);
        }
        if (slot != NONE) {
            Army a = rules::armyFromSlot(c.slots[slot], enhanced);
            a.x = c.x;
            a.y = c.y;
            a.owner = c.ownerIndex;
            a.moves = 0;
            a.homeCity = c.index;
            g.add(a);
        } else {
            // Neutral Cities off: a placeholder Scouts army of strength 1.
            const ArmyType* t = g.types.byId(armytype::SCOUTS);
            Army a;
            a.x = c.x;
            a.y = c.y;
            a.owner = NONE;
            a.type = armytype::SCOUTS;
            a.name = t->name;
            a.strength = 1;
            a.maxMoves = t->move;
            a.moves = 0;
            a.upkeep = 0;
            a.homeCity = c.index;
            g.add(a);
        }
    }
}

// Quick Start: deal every neutral city out to the sides (setup_capitals,
// 79fa:07ca).
static void DealCities(Game& g) {
    std::map<int, std::pair<int, int>> from;
    int turn = NONE;
    for (int i = 7; i >= 0; i--) {
        Side* s = g.map->side(i);
        if (s && s->inUse) {
            from[i] = {s->capX, s->capY};
            turn = i;
        }
    }
    if (turn == NONE) return;
    for (;;) {
        auto [x, y] = from[turn];
        City* pick = nullptr;
        int best = 1000;
        for (auto& c : g.map->cities) {
            if (c.ownerIndex == NONE) {
                double dx = x - c.x, dy = y - c.y;
                int d = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
                if (d < best) {
                    pick = &c;
                    best = d;
                }
            }
        }
        if (!pick) return;
        Side* side = g.map->side(turn);
        pick->owner = side;
        pick->ownerIndex = turn;
        if (g.rng.dice(1, 10, -1) < 5)
            from[turn] = {side->capX, side->capY};
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
        if (m.sides[i].inUse && m.sides[i].capital) avail.push_back(i);
    }
    if (opts.activeSides > 0 && static_cast<int>(avail.size()) > opts.activeSides) {
        uint32_t h = static_cast<uint32_t>(opts.seed) * 2654435761U + 0x9e3779b9U;
        int s0 = avail[h % avail.size()];
        int ax = m.sides[s0].capX, ay = m.sides[s0].capY;

        std::vector<int> land_dist(static_cast<size_t>(MAP_W) * MAP_H, INT_MAX);
        std::queue<std::pair<int, int>> q;
        if (ax >= 0 && ay >= 0 && ax < MAP_W && ay < MAP_H) {
            land_dist[ay * MAP_W + ax] = 0;
            q.push({ax, ay});
        }
        while (!q.empty()) {
            auto [cx, cy] = q.front();
            q.pop();
            int cd = land_dist[cy * MAP_W + cx];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H) continue;
                    int nk = ny * MAP_W + nx;
                    if (land_dist[nk] != INT_MAX) continue;
                    if (scn::terrainAt(m, nx, ny) == move::WATER) continue;
                    land_dist[nk] = cd + 1;
                    q.push({nx, ny});
                }
            }
        }

        auto metric = [&](int s) -> std::tuple<int, int, int, int> {
            if (s == s0) return {0, 0, 0, s};
            int sx = m.sides[s].capX, sy = m.sides[s].capY;
            int ld = (sx >= 0 && sy >= 0 && sx < MAP_W && sy < MAP_H) ? land_dist[sy * MAP_W + sx]
                                                                      : INT_MAX;
            int dx = sx - ax, dy = sy - ay;
            int d2 = dx * dx + dy * dy;
            return {1, ld == INT_MAX ? 1 : 0, ld < INT_MAX ? ld : d2, s};
        };
        std::sort(avail.begin(), avail.end(), [&](int a, int b) { return metric(a) < metric(b); });
        for (size_t i = static_cast<size_t>(opts.activeSides); i < avail.size(); i++) {
            Side& sd = m.sides[avail[i]];
            if (sd.capital) sd.capital->owner = nullptr;
            sd.capital = nullptr;
            sd.inUse = false;
        }
    }

    if (opts.cropMargin <= 0) return;

    int min_x = MAP_W, max_x = 0, min_y = MAP_H, max_y = 0;
    bool any_side = false;
    for (const auto& sd : m.sides) {
        if (!sd.inUse) continue;
        min_x = std::min(min_x, sd.capX);
        max_x = std::max(max_x, sd.capX + 1);
        min_y = std::min(min_y, sd.capY);
        max_y = std::max(max_y, sd.capY + 1);
        any_side = true;
    }
    if (!any_side) return;

    int x0 = std::max(2, min_x - opts.cropMargin);
    int x1 = std::min(MAP_W - 3, max_x + opts.cropMargin);
    int y0 = std::max(2, min_y - opts.cropMargin);
    int y1 = std::min(MAP_H - 3, max_y + opts.cropMargin);
    m.activeX0 = x0;
    m.activeY0 = y0;
    m.activeX1 = x1;
    m.activeY1 = y1;

    int plain_tile = 0, water_tile = 0;
    for (int t = 0; t < 255; t++) {
        if (m.terrainType[t] == move::PLAIN) {
            plain_tile = t;
            break;
        }
    }
    for (int t = 0; t < 255; t++) {
        if (m.terrainType[t] == move::WATER) {
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
                    if (tx >= 0 && ty >= 0 && tx < MAP_W && ty < MAP_H) {
                        m.tiles[ty * MAP_W + tx] = plain_tile;
                    }
                }
            }
        }
    }
    m.cities = std::move(kept_cities);
    m.cityAt.assign(static_cast<size_t>(MAP_W) * MAP_H, nullptr);
    m.cityTile.assign(static_cast<size_t>(MAP_W) * MAP_H, nullptr);
    for (auto& c : m.cities) {
        m.cityAt[c.y * MAP_W + c.x] = &c;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) m.cityTile[(c.y + dy) * MAP_W + (c.x + dx)] = &c;
        }
    }
    for (auto& sd : m.sides) {
        sd.capital = nullptr;
        if (!sd.inUse) continue;
        City* cap = (sd.capX >= 0 && sd.capY >= 0 && sd.capX < MAP_W && sd.capY < MAP_H)
                            ? m.cityAt[sd.capY * MAP_W + sd.capX]
                            : nullptr;
        if (cap) {
            sd.capital = cap;
            cap->owner = &sd;
        } else {
            sd.inUse = false;
        }
    }

    std::vector<Site> kept_sites;
    for (auto& st : m.sites) {
        if (st.x >= x0 && st.x <= x1 && st.y >= y0 && st.y <= y1) {
            st.index = static_cast<int>(kept_sites.size());
            kept_sites.push_back(std::move(st));
        } else if (st.x >= 0 && st.y >= 0 && st.x < MAP_W && st.y < MAP_H) {
            m.tiles[st.y * MAP_W + st.x] = plain_tile;
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
    int bx0 = std::max(0, x0 - 2), bx1 = std::min(MAP_W - 1, x1 + 2);
    int by0 = std::max(0, y0 - 2), by1 = std::min(MAP_H - 1, y1 + 2);
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                int k = y * MAP_W + x;
                m.tiles[k] = water_tile;
                m.roads[k] = '\0';
                m.crossing[k] = false;
            }
        }
    }
}

std::unique_ptr<Game> newGame(const std::string& data_dir, const std::string& scenario,
                              const NewGameOptions& opts) {
    auto gp = std::make_unique<Game>();
    Game& g = *gp;
    g.rng = Rng(opts.seed);
    g.dataDir = data_dir;
    armytype::load(data_dir + "/TERRAIN0/ARMYTYPE.DAT", g.types);
    g.map = opts.memoryFiles
                    ? scn::loadFromFiles(data_dir + "/" + scenario, scenario, *opts.memoryFiles)
                    : scn::load(data_dir + "/" + scenario, scenario);
    g.greatest = opts.greatest;
    for (auto& [k, v] : opts.options) {
        if (int* f = g.map->options.field(k)) *f = v;
    }
    for (auto& s : g.map->sides) {
        auto it = opts.sides.find(s.index);
        if (it != opts.sides.end() && s.inUse) {
            const SideSetup& o = it->second;
            if (!o.name.empty()) s.name = o.name;  // retyped on the setup screen
            if (o.off) {
                if (s.capital) s.capital->owner = nullptr;
                s.inUse = false;
            } else {
                s.computer = o.computer;
                s.level = o.level != NONE ? o.level : s.level;
                s.card = o.card;
            }
        }
    }
    if ((opts.activeSides > 0 && opts.activeSides < 8) || opts.cropMargin > 0) {
        ApplyScaleAndCrop(*g.map, opts);
    }

    for (auto& s : g.map->sides) {
        s.alive = s.inUse;
        s.ownerIndex = s.index;
        if (s.inUse) g.sides.push_back(&s);
    }
    // 79fa:0000: every side is observed unless the map is hidden and a human plays
    int humans = 0;
    for (Side* s : g.sides)
        if (!s->computer) humans++;
    bool observed = !(g.map->options.hiddenMap != 0 && humans >= 1);
    for (Side* s : g.sides) s->observe = observed;

    for (auto& c : g.map->cities) c.ownerIndex = c.owner ? c.owner->index : NONE;
    if (g.map->options.quickStart != 0) DealCities(g);

    for (auto& c : g.map->cities) {
        c.slots = rules::citySlots(c.produces, g.types, g.rng);
        c.defence = rules::cityDefence(static_cast<int>(c.slots.size()));
        c.producing = NONE;  // slot index being built (0-based)
        c.countdown = 0;
        c.vectorTo = NONE;
    }
    scn::refreshCityTiles(*g.map);

    // items 0-7 are the eight sides' standards (docs/rules.md > Heroes)
    for (size_t i = 0; i < g.map->items.size(); i++) {
        Item& it = g.map->items[i];
        if (i < 8 && it.type == rules::ITEM_STANDARD) it.standardOf = static_cast<int>(i);
    }

    diplomacy::init(g);
    SetupGarrisons(g);
    site::setup(g);
    for (Side* s : g.sides) {
        revealStart(g, *s);
        if (g.map->options.hiddenMap != 0 && opts.cropMargin > 0) {
            int bx0 = std::max(0, g.map->activeX0 - 2),
                bx1 = std::min(MAP_W - 1, g.map->activeX1 + 2);
            int by0 = std::max(0, g.map->activeY0 - 2),
                by1 = std::min(MAP_H - 1, g.map->activeY1 + 2);
            auto& mask = g.explored[s->index];
            if (static_cast<int>(mask.size()) == MAP_W * MAP_H) {
                for (int y = 0; y < MAP_H; y++) {
                    for (int x = 0; x < MAP_W; x++) {
                        if (x < bx0 || x > bx1 || y < by0 || y > by1) mask[y * MAP_W + x] = 1;
                    }
                }
            }
        }
    }
    ai::startGame(g);

    // the turn order (.SCN 0x163), as the scenario gives it; Random Turns
    // shuffles it at each round's end (turn_order, 8065:1732)
    g.turnOrder = {0, 1, 2, 3, 4, 5, 6, 7};
    g.orderAt = -1;  // the place in it of the side to play
    g.current = 0;   // index into g.sides
    g.side = g.sides.empty() ? nullptr : g.sides[0];
    g.pathCache = move::newPathCache();
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
    disband(g, side, mine);
    history::deed(g, &side, DeedType::SIDE_VANQUISHED, side.index, 0, "");  // 8065:19f1
    int line = g.rng.dice(1, FALLEN_LINES, -1);
    g.log.push_back(side.name + " has been eliminated.");
    side.alive = false;
    side.gold = 0;
    // every pair with the fallen side, its pair with itself too (8065:18ab)
    for (int o = 0; o < 8; o++) {
        for (int k : {side.index * 8 + o, o * 8 + side.index}) {
            g.diplomacy.state[k] = diplomacy::INTERMEDIATE;
            g.diplomacy.proposal[k] = diplomacy::INTERMEDIATE;
        }
    }
    return Fallen{&side, line, boxed};
}

std::vector<Fallen> eliminateFallen(Game& g) {
    bool human = false;
    for (Side* s : g.sides)
        if (s->alive && !s->computer) human = true;
    std::vector<Fallen> fallen;
    for (Side* s : g.sides) {
        if (s->alive && sideCities(g, *s).empty())
            fallen.push_back(Eliminate(g, *s, human || !s->computer));
    }
    return fallen;
}

static void ApplyIncome(Game& g, Side& side) {
    side.income = income(g, side);
    side.upkeepTotal = upkeep(g, side);
    side.gold = std::max(0, side.gold + side.income - side.upkeepTotal);
}

// Place an army arriving from the road (6f8c:0123), or send it back; false
// if it was disbanded. It needs room on one of the city's four tiles --
// there is no spilling round a full city -- and the city must still be the
// side's (6f8c:0d4f). One sent back that fails again, at home, is
// disbanded. It keeps the moves it set off with.
static bool Deliver(Game& g, Army* army, City& city) {
    auto [x, y] = freeTileIn(g, city, false);
    if (x == NONE || city.ownerIndex != army->owner) {
        if (army->returning || army->homeCity == city.index) return false;
        City* home = g.map->city(army->homeCity);
        army->returning = true;
        army->transit = Transit{kTransitTurns, home ? home->index : NONE};
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
                if (a->transit->dest == STANDARD) {
                    auto [sx, sy] = standardAt(g, side);
                    a->transit.reset();
                    if (sx != NONE &&
                        static_cast<int>(armiesAt(g, sx, sy).size()) < rules::MAX_STACK) {
                        a->x = sx;
                        a->y = sy;
                        a->returning = false;
                        arrived.push_back(Produced{"arrived", a->type, NONE, true});
                    } else {
                        a->returning = true;
                        a->transit = Transit{kTransitTurns, a->homeCity};
                    }
                } else {
                    City* dest = g.map->city(a->transit->dest);
                    a->transit.reset();
                    if (!dest || !Deliver(g, a, *dest))
                        a->disbanded = true;
                    else if (!a->transit)
                        arrived.push_back(Produced{"arrived", a->type, dest->index, false});
                }
            }
        }
    }

    for (City* c : sideCities(g, side)) {
        if (c->producing != NONE && c->countdown > 0) {
            c->countdown--;
            if (c->countdown <= 0) {
                c->countdown = 0;
                if (side.gold > 0) {
                    bool vectored = c->vectorTo != NONE && c->vectorTo != c->index;
                    auto [x, y] = freeTileIn(g, *c, false);
                    if (x != NONE || vectored) {
                        const Slot& slot = c->slots[c->producing];
                        Army a = rules::armyFromSlot(slot, side.enhanced);
                        a.owner = side.index;
                        a.homeCity = c->index;
                        a.moves = a.maxMoves;
                        a.x = x;
                        a.y = y;
                        if (vectored) {
                            a.transit = Transit{kTransitTurns, c->vectorTo};
                            a.x = NONE;
                            a.y = NONE;
                        }
                        g.add(a);
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
            int carry = std::min(a->moves, rules::MOVE_CARRY);
            a->moves = (a->atSea ? rules::SEA_MOVES : a->maxMoves) + carry;
            if (a->target && a->target->first == a->x && a->target->second == a->y)
                a->target.reset();
        }
    }
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* h = g.armies[i];
        if (h->owner != side.index || h->type != armytype::HERO || h->transit) continue;
        bool double_move = false;
        for (Item* it : h->items)
            if (it->type == rules::ITEM_DOUBLE_MOVE) double_move = true;
        if (!double_move) continue;
        auto here = armiesAt(g, h->x, h->y);
        if (static_cast<int>(here.size()) > rules::MAX_STACK) here.resize(rules::MAX_STACK);
        for (Army* a : here) a->moves += a->maxMoves;
    }
    for (Army* a : g.armies) {
        if (a->owner == side.index && a->moves > rules::MAX_MOVE) a->moves = rules::MAX_MOVE;
    }
}

// A stack given the Defend order encamps as its side's next turn opens
// (8c07:0000): its tile gets the tower flag when it still has its full
// movement and stands on plain, forest, hills, a bridge, marsh, the tower
// terrain or a road.
static void Encamp(Game& g, Side& side) {
    static const std::set<int> kEncampOn = {move::PLAIN,  move::FOREST, move::HILLS,
                                            move::BRIDGE, move::MARSH,  move::TOWER};
    for (Army* a : g.armies) {
        if (a->owner == side.index && a->fortified && !a->transit && a->x != NONE &&
            a->moves >= a->maxMoves) {
            bool road = scn::roadAt(*g.map, a->x, a->y) % 32 != 0;
            if (road || kEncampOn.count(scn::terrainAt(*g.map, a->x, a->y))) {
                g.towers.insert(a->y * g.map->width + a->x);
            }
        }
    }
}

bool towerAt(const Game& g, int x, int y) {
    return g.towers.count(y * g.map->width + x) > 0;
}

void tidyTowers(Game& g) {
    if (g.towers.empty()) return;
    std::set<int> held;
    for (Army* a : g.armies)
        if (a->x != NONE && !a->transit) held.insert(a->y * g.map->width + a->x);
    for (auto it = g.towers.begin(); it != g.towers.end();) {
        if (!held.count(*it))
            it = g.towers.erase(it);
        else
            ++it;
    }
}

bool heroWithDoubleMoveAt(const Game& g, const Side& side, int x, int y) {
    for (Army* a : g.armies) {
        if (a->owner == side.index && !a->transit && a->x == x && a->y == y &&
            a->type == armytype::HERO) {
            for (Item* it : a->items)
                if (it->type == rules::ITEM_DOUBLE_MOVE) return true;
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
        if (a->owner == side.index && a->atSea && !a->transit && a->x != NONE &&
            cityAt(g, a->x, a->y))
            a->atSea = false;
    }
}

// Before every side's turn the side's capital is put where it now is
// (67cc:10d7): while more than one side plays and the side holds cities but
// not its own capital, the city of its own nearest the capital it had; else
// its own capital again. The turn opens centred there.
void followCapital(Game& g, Side& side) {
    if (side.capNowX == NONE) {
        side.capNowX = side.capX;
        side.capNowY = side.capY;
    }
    int playing = 0;
    for (Side* s : g.sides)
        if (s->alive) playing++;
    if (playing != 1) {
        City* c = cityAt(g, side.capX, side.capY);
        if (c && c->razed) c = nullptr;
        if (!sideCities(g, side).empty() && (!c || c->ownerIndex != side.index)) {
            City* n = nearestCity(g, side.capNowX, side.capNowY, &side);
            if (n) {
                side.capNowX = n->x;
                side.capNowY = n->y;
            }
            return;
        }
    }
    side.capNowX = side.capX;
    side.capNowY = side.capY;
}

static void TurnQuest(Game& g, Side& side) {
    auto quest_result = quest::event(g, side, "turn");
    if (quest_result && !quest_result->failed.empty())
        g.log.push_back("Quest abandoned: " + quest_result->failed + ".");
}

bool startTurn(Game& g, Side& side) {
    ComeAshore(g, side);
    followCapital(g, side);
    tidyExplored(g, side.index);
    side.diploNews = diplomacy::apply(g, side);
    for (auto& m : side.diploNews) g.log.push_back(m);
    side.heroOffer.reset();
    side.productionDue = false;
    if (side.computer) {
        if (sideCities(g, side).empty()) return false;
        auto offer = hero::offer(g, side);
        if (offer) hero::recruit(g, side, *offer);
        hero::checkPromotions(g, side);
        ApplyIncome(g, side);
        RunProduction(g, side);
        ResetMovement(g, side);
        tidyTowers(g);
        Encamp(g, side);
        // the army cycle's cursor goes back to the capital (8c07:0113)
        if (side.ai) side.ai->cursor = std::make_pair(side.capNowX, side.capNowY);
        TurnQuest(g, side);
        return true;
    }
    ResetMovement(g, side);
    tidyTowers(g);
    Encamp(g, side);
    ApplyIncome(g, side);
    TurnQuest(g, side);
    side.promoted = hero::checkPromotions(g, side);
    side.heroOffer = hero::offer(g, side);
    side.productionDue = true;
    return true;
}

void humanProduction(Game& g, Side& side) {
    if (!side.productionDue) return;
    side.productionDue = false;
    if (g.turn == 1) return;
    for (auto& e : RunProduction(g, side)) {
        City* c = g.map->city(e.city);
        if (c && c->producing != NONE && c->producing < static_cast<int>(c->slots.size()))
            c->countdown = c->slots[c->producing].time;
    }
}

bool producing(const City& c) {
    return c.producing != NONE && c.countdown > 0;
}

// The turn order for the next round (turn_order, 8065:1732): with Random
// Turns, 20 swaps of two sides picked by dice.
static void TurnOrder(Game& g) {
    g.turnOrder = {0, 1, 2, 3, 4, 5, 6, 7};
    if (g.map->options.randomTurns != 0) {
        for (int i = 0; i < 20; i++) {
            int a = g.rng.dice(1, 8, -1), b = g.rng.dice(1, 8, -1);
            std::swap(g.turnOrder[a], g.turnOrder[b]);
        }
    }
}

static Side* NextSide(Game& g, Side* leaving) {
    for (int n = 0; n < 3 * 8 + 1; n++) {
        if (g.orderAt >= 7) {
            g.orderAt = -1;
            g.turn++;
            // the round's end: the order, the fallen put out of the game, the end
            // looked for, the neutral cities' production, then the history
            TurnOrder(g);
            endRound(g, leaving);
            if (g.ending.over) {
                g.side = nullptr;
                return nullptr;
            }
            history::record(g);  // 8065:17f6 -> 6d51:0d60
        }
        g.orderAt++;
        Side* side = g.map->side(g.turnOrder[g.orderAt]);
        if (side && side->inUse && side->alive) {
            g.side = side;
            g.current = static_cast<int>(std::find(g.sides.begin(), g.sides.end(), side) -
                                         g.sides.begin());
            if (startTurn(g, *side)) return side;
        }
    }
    g.side = nullptr;
    return nullptr;
}

Side* endTurn(Game& g) {
    Side* leaving = g.side;
    if (leaving && leaving->alive && (leaving->computer || !g.won))
        diplomacy::scoreUpdate(g, *leaving);
    // what the last round's end found is for the turn that followed it only
    g.ending = Ending{};
    return NextSide(g, leaving);
}

Side* begin(Game& g) {
    g.orderAt = -1;
    g.ending = Ending{};
    return NextSide(g, nullptr);
}

Side* nextHuman(Game& g) {
    for (int i = std::max(g.orderAt, 0); i <= 7; i++) {
        Side* side = g.map->side(g.turnOrder[i]);
        if (side && side->inUse && side->alive && !side->computer) return side;
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
        bool flies = g.types.byId(a->type)->flies;
        if (a->type == armytype::HERO) {
            for (Item* it : a->items)
                if (it->type == rules::ITEM_FLIGHT) hero_flight = true;
        }
        if (!flies || a->atSea) all_fly = false;
        if (a->type != armytype::HERO && !flies) non_heroes_fly = false;
        if (flies && !a->atSea) free_flier = true;
    }
    return hero_flight || all_fly || (non_heroes_fly && free_flier);
}

int sinkStranded(Game& g, Side& side, int x, int y) {
    auto wet = [&](int tx, int ty) {
        return scn::terrainAt(*g.map, tx, ty) == move::WATER || scn::isCrossing(*g.map, tx, ty);
    };
    std::map<int, std::vector<Army*>> tiles;
    for (Army* a : g.armies) {
        if (a->owner != side.index || a->transit || a->x == NONE || a->x < 0 || a->y < 0) continue;
        if (x != NONE && (a->x != x || a->y != y)) continue;
        if (wet(a->x, a->y)) tiles[a->y * g.map->width + a->x].push_back(a);
    }
    std::vector<Army*> dead;
    for (auto& [k, here] : tiles) {
        if (CanFlyOff(g, here)) continue;
        for (Army* a : here) {
            if (!a->atSea && a->type != armytype::HERO && !g.types.byId(a->type)->flies)
                dead.push_back(a);
        }
    }
    RemoveArmies(g, dead);
    return static_cast<int>(dead.size());
}

Sign* signAt(Game& g, int x, int y) {
    for (auto& s : g.map->signs)
        if (s.x == x && s.y == y) return &s;
    return nullptr;
}

void disband(Game& g, Side& side, const std::vector<Army*>& armies) {
    for (Army* a : armies) {
        if (a->type == armytype::HERO) {
            hero::dropItems(g, a, a->x, a->y, side.index);
            if (side.quest && side.quest->hero == a) side.quest.reset();
        }
    }
    RemoveArmies(g, armies);
}

int loot(const Game& g, const Side* loser) {
    if (!loser) return 0;
    int n = static_cast<int>(sideCities(g, *loser).size());
    int share = n > 1 ? loser->gold / n : loser->gold;
    return share / 2;
}

Battle decideAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    combat::Lines l = combat::lines(g, stack, x, y);
    int me = stack.empty() ? NONE : stack[0]->owner;
    if (l.city && me != NONE) l.city->attacked |= 1 << me;
    Battle result = combat::resolve(g, l.attackers, l.defenders, x, y);
    result.lines.attackers = l.attackers;
    result.lines.defenders = l.defenders;
    result.lines.city = l.city;
    Side* loser = l.defOwner != NONE ? g.map->side(l.defOwner) : nullptr;
    if (result.won && l.city && loser) result.loot = loot(g, loser);
    // the stack's hero (stack_movement_mode, 1c8c:0529): the leader if a
    // hero, else its first hero
    for (Army* a : stack) {
        if (a->type == armytype::HERO) {
            result.hero = a;
            break;
        }
    }
    result.fought = Battle::Fought{stack, x, y, l.defOwner};
    return result;
}

void chargeAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    City* city = cityAt(g, x, y);
    int t = city && !city->razed ? move::CITY : scn::terrainAt(*g.map, x, y);
    int cost = std::max(2, t >= 0 && t < 12 ? move::COST[t] : 0);
    move::Mode m = move::stackMode(g, stack);
    if (m.mode != move::FLYING && !m.atSea && (t == move::WATER || t == move::SHORE)) {
        for (Army* a : stack) {
            bool flight = false;
            if (a->type == armytype::HERO) {
                for (Item* it : a->items)
                    if (it->type == rules::ITEM_FLIGHT) flight = true;
            }
            if (!g.types.byId(a->type)->flies && !flight) {
                a->atSea = true;
                a->moves = 0;
            }
        }
    }
    for (Army* a : stack) {
        a->moves = std::max(0, a->moves - cost);
        a->target.reset();
    }
}

Battle resolveAttack(Game& g, const std::vector<Army*>& stack, int x, int y) {
    Battle b = decideAttack(g, stack, x, y);
    applyAttack(g, b);
    return b;
}

static Army* Medal(Game& g, const Battle& result, int side);

Battle& applyAttack(Game& g, Battle& result) {
    if (!result.fought) return result;
    Battle::Fought f = *result.fought;
    result.fought.reset();
    const auto& stack = f.stack;
    int x = f.x, y = f.y, def_owner = f.defOwner;
    const auto& attackers = result.lines.attackers;
    const auto& defenders = result.lines.defenders;
    City* city = result.lines.city;
    // the city on the tile, ruins too (828e:044a)
    City* tile_city = g.map->cityTile[y * g.map->width + x];
    const auto& boats = result.boats;

    auto fallen = [&](Army* h) {
        history::deed(g, g.map->side(h->owner == NONE ? 8 : h->owner), DeedType::HERO_KILLED,
                      tile_city ? tile_city->index : history::KILLED_IN_BATTLE, 0, h->name);
    };
    int mine = stack.empty() ? NONE : stack[0]->owner;
    {
        int heroes_lost = 0;
        for (Army* dd : result.deadDefenders)
            if (dd->type == armytype::HERO) heroes_lost++;
        ai::recordBattle(g, def_owner, mine, x, y, heroes_lost,
                         static_cast<int>(result.deadDefenders.size()),
                         result.won && !defenders.empty(), city != nullptr);
    }
    for (Army* a : result.deadAttackers) history::tally(g, a, def_owner, boats.count(a) > 0);
    for (Army* d : result.deadDefenders) history::tally(g, d, mine, boats.count(d) > 0);
    // a big battle won at a city goes down in history (67cc:0af5): eight
    // armies or more, four of them defending -- by the hero's name when a hero
    // led the stack
    if (tile_city && result.won && mine != NONE && attackers.size() + defenders.size() >= 8 &&
        defenders.size() >= 4) {
        Side* side = g.map->side(mine);
        if (result.hero)
            history::deed(g, side, DeedType::CITY_WON, history::WON_BY_NAME, tile_city->index,
                          result.hero->name);
        else
            history::deed(g, side, DeedType::CITY_WON, mine, tile_city->index, "");
    }
    // the fallen heroes' items drop on the tile fought for, attacker's too
    // (67cc:16cd with after_battle's x, y)
    for (Army* a : result.deadAttackers) {
        if (a->type == armytype::HERO) {
            fallen(a);
            hero::dropItems(g, a, x, y, mine);
        }
    }
    for (Army* d : result.deadDefenders) {
        if (d->type == armytype::HERO) {
            fallen(d);
            hero::dropItems(g, d, x, y, mine);
        }
    }
    hero::battleExperience(g, attackers, defenders, result, city != nullptr);
    Side* my_side =
            g.map->side(stack.empty() ? 0 : (stack[0]->owner == NONE ? 0 : stack[0]->owner));
    {
        quest::Event ev;
        ev.stack = attackers;
        ev.killed = result.deadDefenders;
        result.quest = quest::event(g, *my_side, "battle", ev);
    }

    RemoveArmies(g, result.deadAttackers);
    RemoveArmies(g, result.deadDefenders);
    tidyTowers(g);

    if (result.won && city) {
        Side* winner = g.map->side(stack[0]->owner);
        Side* loser = def_owner != NONE ? g.map->side(def_owner) : nullptr;
        if (loser) {
            int l = result.loot;
            winner->gold += l;
            loser->gold = std::max(0, loser->gold - 2 * l);
        }
        // whom the city was taken from (+0x2f): its owner, or no one when that
        // side takes its own back
        if (city->ownerIndex != NONE) {
            city->claim = city->claim == winner->index ? rules::NEUTRAL : city->ownerIndex;
        }
        city->attacked = 0;
        city->countdown = 0;
        city->vectorTo = NONE;
        // every city that was sending its armies here stops: no vector, and
        // nothing in production either (67cc:0a6b)
        for (auto& c : g.map->cities) {
            if (c.vectorTo == city->index) {
                c.vectorTo = NONE;
                c.producing = NONE;
                c.countdown = 0;
            }
        }
        city->ownerIndex = winner->index;
        scn::setCityTiles(*g.map, *city);
        move::invalidate(g);
        result.captured = city;
        // a computer's quest hero taking its quest city (5e97:038d) razes it
        bool handled = winner->computer && ai::questCapture(g, *winner, *city, result.attackers);
        if (!handled && winner->computer) {
            quest::Event ev;
            ev.city = city;
            ev.stack = result.attackers;
            auto q = quest::event(g, *winner, "occupy", ev);
            if (q) result.quest = q;
        }
    }

    // The survivors walk into the tile they just cleared -- as many as fit.
    if (result.won) {
        g.towers.erase(y * g.map->width + x);
        int room = rules::MAX_STACK - static_cast<int>(armiesAt(g, x, y).size());
        for (Army* a : result.attackers) {
            if (room <= 0) break;
            a->x = x;
            a->y = y;
            room--;
        }
        // and the winner sees round what it took (67cc:0a6b calls 8611:1298):
        // the tile fought for, then each of a city's four, 2 out from a city
        if (g.map->options.hiddenMap != 0 && mine != NONE) {
            bool flying = move::modeOf(g, result.attackers) == move::FLYING;
            int found = reveal(g, mine, x, y, flying);
            if (city) {
                for (int dx = 0; dx <= 1; dx++)
                    for (int dy = 0; dy <= 1; dy++)
                        found += reveal(g, mine, city->x + dx, city->y + dy, false);
            }
            if (found > 0) move::invalidate(g);
        }
    }
    result.medal = Medal(g, result, mine);
    return result;
}

// An ordinary army: not a hero, not a magical creature.
static bool Ordinary(const Game& g, const Army* a) {
    return a->type != armytype::HERO && g.types.byId(a->type)->bonus[48] == 0;
}

// A medal for a battle won (auto_ui_medal_effect, 67cc:2274): a stack with a
// hero and no army at sea that won a fight worth the name -- two armies a
// side, or before turn 20 a first defender above strength 3 -- gives one of
// its ordinary armies a medal, 40% of the time less 5 for each of the side's
// ordinary armies with one already, 12% at least. A medal is +1 strength
// (9 at most), or +1 move past strength 14; four at most. Returns the army.
static Army* Medal(Game& g, const Battle& result, int side) {
    if (side == NONE || !result.won) return nullptr;
    const auto& survivors = result.attackers;
    for (Army* a : survivors)
        if (a->atSea) return nullptr;
    size_t n_atk = result.lines.attackers.size(), n_def = result.lines.defenders.size();
    bool counted =
            (n_atk >= 2 && n_def >= 2) || (g.turn < 20 && n_def >= 1 && result.defStrength0 > 3);
    if (!counted) return nullptr;
    std::vector<Army*> plain;
    for (Army* a : survivors)
        if (Ordinary(g, a)) plain.push_back(a);
    if (plain.empty()) return nullptr;
    int decorated = 0;
    for (Army* a : g.armies)
        if (a->owner == side && Ordinary(g, a) && a->medals > 0) decorated++;
    int chance = std::max(12, 40 - 5 * decorated);
    if (!(g.rng.dice(1, 100, 0) < chance) || !result.hero) return nullptr;
    Army* a = plain[g.rng.dice(1, static_cast<int>(plain.size()), -1)];
    if (a->medals >= 4) return nullptr;
    a->medals++;
    if (a->strength > 14)
        a->maxMoves++;
    else
        a->strength = std::min(9, a->strength + 1);
    return a;
}

// A production type's "value" is half its purchase price (ARMYTYPE +30).
static int SlotValue(const Game& g, const Slot& slot) {
    return std::abs(g.types.byId(slot.type)->price) / 2;
}

static void Recompute(Game& g, City& city) {
    city.defence = rules::cityDefence(static_cast<int>(city.slots.size()));
    if (city.producing != NONE && city.producing >= static_cast<int>(city.slots.size())) {
        city.producing = NONE;
        city.countdown = 0;
    }
}

void addDiploScore(Game& g, Side& side, int n) {
    side.diploScore += n;
}

std::shared_ptr<QuestResult> occupy(Game& g, Side& side, City& city,
                                    const std::vector<Army*>& stack) {
    quest::Event ev;
    ev.city = &city;
    ev.stack = stack;
    return quest::event(g, side, "occupy", ev);
}

Spoils pillage(Game& g, Side& side, City& city, const std::vector<Army*>& stack) {
    Spoils out;
    if (city.slots.empty()) return out;
    Slot slot = city.slots.back();  // slots are sorted cheapest first
    city.slots.pop_back();
    int gold = SlotValue(g, slot);
    out.gold = gold;
    out.lost.emplace_back(slot.type, gold);
    side.gold += gold;
    addDiploScore(g, side, g.rng.dice(1, 5, 0));
    Recompute(g, city);
    quest::Event ev;
    ev.city = &city;
    ev.gold = gold;
    ev.stack = stack;
    quest::event(g, side, "pillage", ev);
    return out;
}

Spoils sack(Game& g, Side& side, City& city, const std::vector<Army*>& stack) {
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
    addDiploScore(g, side, g.rng.dice(1, 10, 5));
    Recompute(g, city);
    quest::Event ev;
    ev.city = &city;
    ev.gold = gold;
    ev.stack = stack;
    quest::event(g, side, "pillage", ev);
    return out;
}

// The city becomes ruins (649c:016b).
static void MakeRuins(Game& g, City& city) {
    city.razed = true;
    city.razedBy = city.ownerIndex == NONE ? 0 : city.ownerIndex;
    city.ownerIndex = NONE;
    city.claim = rules::NEUTRAL;
    city.slots.clear();
    city.producing = NONE;
    city.countdown = 0;
    city.vectorTo = NONE;
    city.defence = 0;
    city.income = 0;
    // every city sending its armies here stops: no vector, nothing in
    // production (649c:016b)
    for (auto& c : g.map->cities) {
        if (c.vectorTo == city.index) {
            c.vectorTo = NONE;
            c.producing = NONE;
            c.countdown = 0;
        }
    }
    scn::setCityTiles(*g.map, city);
    move::invalidate(g);
}

void raze(Game& g, Side& side, City& city, const std::vector<Army*>& stack, bool own_city) {
    MakeRuins(g, city);
    if (own_city) {
        addDiploScore(g, side, g.rng.dice(1, 25, 25));
        return;
    }
    addDiploScore(g, side, g.rng.dice(1, 15, 10));
    quest::Event ev;
    ev.city = &city;
    ev.stack = stack;
    quest::event(g, side, "raze", ev);
}

void resign(Game& g, Side& side) {
    for (auto& c : g.map->cities) {
        if (!c.razed && c.ownerIndex == side.index) MakeRuins(g, c);
    }
    disband(g, side, sideArmies(g, side));
    if (g.map->options.hiddenMap != 0) {
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

bool hasActiveHuman(const Game& g) {
    for (const Side* s : g.sides) {
        if (s && s->alive && !s->computer && !sideCities(g, *s).empty()) return true;
    }
    return false;
}

int viewerSide(const Game& g, const Side* player) {
    if (!hasActiveHuman(g)) return NONE;
    return player ? player->index : NONE;
}

bool seen(const Game& g, int side, int x, int y) {
    if (g.map->options.hiddenMap == 0) return true;
    if (x < 0 || y < 0 || x >= g.map->width || y >= g.map->height) return false;
    if (side == NONE) return true;
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

int reveal(Game& g, int side, int x, int y, bool flying) {
    if (g.map->options.hiddenMap == 0) return 0;
    auto& mask = *MaskFor(g, side, true);
    bool on_city = x >= 0 && y >= 0 && x < g.map->width && y < g.map->height &&
                   g.map->cityTile[y * g.map->width + x];
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

const int FOG_DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
const int FOG_DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
const int FOG_CELL[256] = {
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

int fogCell(const Game& g, int side, int x, int y) {
    if (seen(g, side, x, y)) return NONE;
    int bits = 0;
    for (int i = 0; i < 8; i++) {
        int nx = x + FOG_DX[i], ny = y + FOG_DY[i];
        if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height || !seen(g, side, nx, ny))
            bits += 1 << i;
    }
    int cell = FOG_CELL[bits];
    return cell == FOG_NONE ? NONE : cell;
}

void tidyExplored(Game& g, int side) {
    if (g.map->options.hiddenMap == 0) return;
    auto& mask = *MaskFor(g, side, true);
    int map_width = g.map->width, map_height = g.map->height;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < map_width; x++) {
            for (int y = 0; y < map_height; y++) {
                int k = y * map_width + x;
                if (!mask[k]) {
                    int bits = 0;
                    for (int i = 0; i < 8; i++) {
                        int nx = x + FOG_DX[i], ny = y + FOG_DY[i];
                        if (nx < 0 || ny < 0 || nx >= map_width || ny >= map_height ||
                            !mask[ny * map_width + nx])
                            bits += 1 << i;
                    }
                    if (FOG_CELL[bits] == FOG_NONE) mask[k] = 1;
                }
            }
        }
    }
}

void revealStart(Game& g, const Side& side) {
    if (g.map->options.hiddenMap == 0) return;
    for (City* c : sideCities(g, side)) reveal(g, side.index, c->x, c->y, false);
    for (Army* a : sideArmies(g, side))
        if (!a->transit) reveal(g, side.index, a->x, a->y, false);
    tidyExplored(g, side.index);
}

void acceptSurrender(Game& g, Side& side) {
    for (Side* s : g.sides)
        if (s->computer) s->alive = false;
    g.won = true;
    history::deed(g, &side, DeedType::SIDE_VICTORIOUS, side.index, 0, "");
}

bool surrenderPending(const Game& g) {
    return g.surrenderOffered && !g.surrenderShown && !g.won;
}

Ending endRound(Game& g, Side* last) {
    auto fallen = eliminateFallen(g);
    Ending ending = checkEnd(g);
    ending.fallen = fallen;
    if (!ending.message.empty()) g.log.push_back(ending.message);
    g.ending = ending;
    neutralProduction(g, last ? last : g.map->side(SETUP_PLAYER));
    return ending;
}

bool setCityProduction(Game& g, const Side* who, City& c, int slot) {
    if (slot < 0 || slot >= static_cast<int>(c.slots.size())) return false;
    if ((who ? who->gold : 0) < c.slots[slot].cost + 30 && g.turn > 5) return false;
    setProduction(g, c, slot);
    return true;
}

// A neutral city's garrison is built (produce_garrison, 6bd8:0381): on its
// top-left tile, room or not, with no moves, and the city builds nothing
// more until it is set to again.
static void ProduceGarrison(Game& g, City& c, const Side* who) {
    if (c.producing != NONE && c.producing < static_cast<int>(c.slots.size())) {
        Army a = rules::armyFromSlot(c.slots[c.producing], who && who->enhanced);
        a.x = c.x;
        a.y = c.y;
        a.owner = c.ownerIndex;
        a.moves = 0;
        a.homeCity = c.index;
        g.add(a);
    }
    c.producing = NONE;
    c.countdown = 0;
}

void neutralProduction(Game& g, const Side* who) {
    if (!(g.map->options.neutralCities > 1)) return;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.ownerIndex == NONE && !c.razed && c.attacked) {
            if (c.countdown != 0) {
                c.countdown--;
                if (c.countdown == 0) ProduceGarrison(g, c, who);
            }
            const Slot* slot = c.producing != NONE && c.producing < static_cast<int>(c.slots.size())
                                       ? &c.slots[c.producing]
                                       : nullptr;
            bool building = slot && c.countdown != 0 && c.countdown != slot->time;
            if (!building &&
                std::min(static_cast<int>(armiesAt(g, c.x, c.y).size()), rules::MAX_STACK) < 4) {
                int best = rules::bestSlot(c.slots, 2, g.types, who && who->enhanced);
                if (best != NONE) setCityProduction(g, who, c, best);
            }
        }
    }
}

Ending checkEnd(Game& g) {
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
    if (!g.won && humans.empty() && fallen_human && !g.noHumansSaid) {
        g.noHumansSaid = true;
        e.noHumans = true;
    }
    // a lone human with more than half the cities has won -- once; the game
    // goes on, for the kingdom to be looked over
    if (!g.won && humans.size() == 1 && computers.empty()) {
        int mine = static_cast<int>(sideCities(g, *humans[0]).size());
        if (mine * 2 > standing) {
            g.won = true;
            history::deed(g, humans[0], DeedType::SIDE_VICTORIOUS, humans[0]->index, 0, "");
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
        history::deed(g, winner, DeedType::SIDE_VICTORIOUS, winner->index, 0, "");
        e.triumph = true;
        e.winner = winner;
        e.message = winner->name + " has triumphed!";
    }
    // offered once, while computers still play
    if (!g.won && humans.size() == 1 && !computers.empty() && !g.surrenderOffered) {
        int mine = static_cast<int>(sideCities(g, *humans[0]).size());
        int biggest = 0;
        for (Side* s : computers)
            biggest = std::max(biggest, static_cast<int>(sideCities(g, *s).size()));
        // every city counts here, ruins too (8065:1aed reads city_count)
        int all = static_cast<int>(g.map->cities.size());
        if (mine > all / 2 && mine > biggest + all / 8) {
            g.surrenderOffered = true;
            e.surrender = true;
            e.message = "Your enemies offer their surrender!";
        }
    }
    return e;
}

std::shared_ptr<SearchResult> searchHere(Game& g, const std::vector<Army*>& stack, bool human) {
    if (stack.empty()) return nullptr;
    return site::search(g, stack, stack[0]->x, stack[0]->y, human);
}

std::string describeSearch(const SearchResult* r) {
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

void setProduction(Game& g, City& city, int slot_index) {
    city.producing = slot_index;
    city.countdown = slot_index != NONE ? city.slots[slot_index].time : 0;
    if (slot_index == NONE) city.vectorTo = NONE;  // vectoring only sticks while building
}

std::vector<City*> vectoredTo(Game& g, const City* dest, const Side* side) {
    std::vector<City*> out;
    for (auto& c : g.map->cities) {
        if (!dest) {
            if (c.vectorTo == STANDARD && side && c.ownerIndex == side->index) out.push_back(&c);
        } else if (c.vectorTo == dest->index && &c != dest) {
            out.push_back(&c);
        }
    }
    return out;
}

std::pair<int, int> standardAt(const Game& g, const Side& side) {
    if (side.index < static_cast<int>(g.map->items.size())) {
        const Item& it = g.map->items[side.index];
        if (it.status == 1 && it.planted && it.x != NONE) return {it.x, it.y};
    }
    return {NONE, NONE};
}

bool plantFlag(Game& g, Side& side, Army* h) {
    int t = scn::terrainAt(*g.map, h->x, h->y);
    if (t == move::WATER || t == move::SHORE || t == move::CITY || t == move::SITE) return false;
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

bool vectorToStandard(Game& g, City& city, const Side& side) {
    if (standardAt(g, side).first == NONE) return false;
    if (city.vectorTo != STANDARD &&
        static_cast<int>(vectoredTo(g, nullptr, &side).size()) >= MAX_VECTORED_TO)
        return false;
    city.vectorTo = STANDARD;
    return true;
}

bool vector(Game& g, City& city, City* dest_city) {
    if (dest_city == &city) dest_city = nullptr;
    if (dest_city && city.vectorTo != dest_city->index &&
        static_cast<int>(vectoredTo(g, dest_city).size()) >= MAX_VECTORED_TO) {
        return false;
    }
    city.vectorTo = dest_city ? dest_city->index : NONE;
    return true;
}

City* nearestCity(Game& g, int x, int y, const Side* side, const Side* seer) {
    City* best = nullptr;
    int best_d = NONE;
    for (auto& c : g.map->cities) {
        bool ok = !side || c.ownerIndex == side->index;
        if (ok && seer) {
            int s = seer->index;
            ok = seen(g, s, c.x, c.y) || seen(g, s, c.x + 1, c.y) || seen(g, s, c.x, c.y + 1) ||
                 seen(g, s, c.x + 1, c.y + 1);
        }
        if (ok) {
            int d = move::distance(x, y, c.x, c.y);
            if (best_d == NONE || d < best_d) {
                best = &c;
                best_d = d;
            }
        }
    }
    return best;
}

std::vector<const ArmyType*> buyableTypes(const Game& g) {
    std::vector<const ArmyType*> out;
    for (auto* a : g.types.list())
        if (a->price >= 0) out.push_back(a);
    return out;
}

std::string cannotBuy(const Game& g, const Side& side, const City& city, const ArmyType& a) {
    for (auto& slot : city.slots)
        if (slot.type == a.id) return "has it";
    if (side.gold < a.price) return "too dear";
    return "";
}

int buySlot(const City& city) {
    if (static_cast<int>(city.slots.size()) < rules::PRODUCTION_SLOTS)
        return static_cast<int>(city.slots.size());
    return 0;
}

void buyProduction(Game& g, Side& side, City& city, int n, int type_id) {
    const ArmyType* a = g.types.byId(type_id);
    bool building = city.producing != NONE;
    int was = city.producing;
    Slot s{a->id, a->name, a->strength, a->time, a->cost, a->move, std::abs(a->price)};
    if (n >= static_cast<int>(city.slots.size())) city.slots.resize(n + 1);
    city.slots[n] = s;
    city.defence = rules::cityDefence(static_cast<int>(city.slots.size()));
    side.gold -= a->price;
    // the slot being built was bought over
    if (building && was == n) {
        city.producing = NONE;
        city.countdown = 0;
        city.vectorTo = NONE;
    }
}

void sortProduction(City& city) {
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
    if (city.producing != NONE) city.producing = building;
}

void renameCity(Game& g, City& city, const std::string& name) {
    city.name = name;
}

}  // namespace game
}  // namespace w2
