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

Army* Game::Make(const Army& army) {
    pool.push_back(std::make_unique<Army>(army));
    return pool.back().get();
}

Army* Game::Add(const Army& army) {
    std::set<int> used;
    for (Army* existing : armies) used.insert(existing->slot);
    int slot = 0;
    while (used.count(slot)) slot++;
    Army* added = Make(army);
    added->slot = slot;
    size_t pos = armies.size();
    while (pos > 0 && armies[pos - 1]->slot > slot) pos--;
    armies.insert(armies.begin() + pos, added);
    return added;
}

bool Game::Alive(const Army* army) const {
    return army && std::find(armies.begin(), armies.end(), army) != armies.end();
}

void Game::Remove(const Army* army) {
    auto pos = std::find(armies.begin(), armies.end(), army);
    if (pos != armies.end()) armies.erase(pos);
}

namespace game {

static const int kTransitTurns = 2;  ///< A vectored army arrives two turns later.

/** @brief Flat tile index in `game.map` for column `x`, row `y`. */
static int Key(const Game& game, int x, int y) {
    return y * game.map->width + x;
}

std::vector<Army*> ArmiesAt(const Game& game, int x, int y) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (!army->transit && army->x == x && army->y == y) out.push_back(army);
    }
    return out;
}

City* CityAt(const Game& game, int x, int y) {
    if (x < 0 || y < 0 || x >= game.map->width || y >= game.map->height) return nullptr;
    return game.map->city_tile[Key(game, x, y)];
}

void SetCityOwner(Game& game, City& city, int side_index) {
    city.owner_index = side_index;
    move::Invalidate(game);
}

std::vector<City*> SideCities(const Game& game, const Side& side) {
    std::vector<City*> out;
    for (auto& city : game.map->cities)
        if (city.owner_index == side.index) out.push_back(&city);
    return out;
}

std::vector<Army*> SideArmies(const Game& game, const Side& side) {
    std::vector<Army*> out;
    for (Army* army : game.armies)
        if (army->owner == side.index) out.push_back(army);
    return out;
}

int ItemBonus(const Game& game, const Side& side, int item_type) {
    int total = 0;
    for (Army* army : game.armies) {
        if (army->owner == side.index) {
            for (Item* item : army->items)
                if (item->type == item_type) total += std::max(1, item->value);
        }
    }
    return total;
}

std::pair<int, int> FreeTileIn(Game& game, const City& city, bool spill,
                               std::pair<int, int>* last) {
    static const int kOffsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (auto& offset : kOffsets) {
        int x = city.x + offset[0], y = city.y + offset[1];
        if (x < game.map->width && y < game.map->height &&
            static_cast<int>(ArmiesAt(game, x, y).size()) < rules::kMaxStack) {
            return {x, y};
        }
    }
    if (!spill) return {kNone, kNone};
    int last_x = kNone, last_y = kNone;
    for (int i = 0; i < 20; i++) {
        int x = city.x + game.rng.Dice(1, 3, -2);
        int y = city.y + game.rng.Dice(1, 3, -2);
        if (x >= 0 && y >= 0 && x < game.map->width && y < game.map->height) {
            last_x = x;
            last_y = y;
            if (ArmiesAt(game, x, y).empty()) return {x, y};
        }
    }
    if (last) *last = {last_x, last_y};
    return {kNone, kNone};
}

int Income(const Game& game, const Side& side) {
    int total = 0;
    auto cities = SideCities(game, side);
    for (City* city : cities) total += city->income;
    int per_city = ItemBonus(game, side, rules::kItemGoldPerCity);
    return total + static_cast<int>(cities.size()) * per_city;
}

int Upkeep(const Game& game, const Side& side) {
    int total = 0;
    for (Army* army : game.armies) {
        if (army->owner == side.index && !army->transit) {
            int cost = army->upkeep;
            if (army->at_sea) cost = std::max(rules::kSeaMinUpkeep, cost);
            total += cost;
        }
    }
    return total;
}

/**
 * @brief Places one starting army in each standing city, from the last city to the first.
 *
 * See docs/rules.md > Starting garrisons.
 */
static void SetupGarrisons(Game& game) {
    Side* setup = game.map->Side(kSetupPlayer);
    bool enhanced = setup && setup->enhanced;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.razed) continue;
        bool owned = city.owner_index != kNone;
        auto level = rules::GarrisonLevel(owned, game.map->options.neutral_cities, game.rng);
        int slot = kNone;
        if (level && !city.slots.empty()) {
            slot = rules::BestSlot(city.slots, rules::kGarrisonPurpose[*level], game.types,
                                   enhanced);
        }
        if (slot != kNone) {
            Army army = rules::ArmyFromSlot(city.slots[slot], enhanced);
            army.x = city.x;
            army.y = city.y;
            army.owner = city.owner_index;
            army.moves = 0;
            army.home_city = city.index;
            game.Add(army);
        } else {
            // Neutral Cities off: a placeholder Scouts army of strength 1.
            const ArmyType* scouts_type = game.types.ById(armytype::kScouts);
            Army army;
            army.x = city.x;
            army.y = city.y;
            army.owner = kNone;
            army.type = armytype::kScouts;
            army.name = scouts_type->name;
            army.strength = 1;
            army.max_moves = scouts_type->move;
            army.moves = 0;
            army.upkeep = 0;
            army.home_city = city.index;
            game.Add(army);
        }
    }
}

/** @brief Quick Start: deals every neutral city out to the active sides in turn. */
static void DealCities(Game& game) {
    std::map<int, std::pair<int, int>> from;
    int turn = kNone;
    for (int i = 7; i >= 0; i--) {
        Side* side = game.map->Side(i);
        if (side && side->in_use) {
            from[i] = {side->cap_x, side->cap_y};
            turn = i;
        }
    }
    if (turn == kNone) return;
    for (;;) {
        auto [x, y] = from[turn];
        City* pick = nullptr;
        int best = 1000;
        for (auto& city : game.map->cities) {
            if (city.owner_index == kNone) {
                double dx = x - city.x, dy = y - city.y;
                int dist = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
                if (dist < best) {
                    pick = &city;
                    best = dist;
                }
            }
        }
        if (!pick) return;
        Side* side = game.map->Side(turn);
        pick->owner = side;
        pick->owner_index = turn;
        if (game.rng.Dice(1, 10, -1) < 5)
            from[turn] = {side->cap_x, side->cap_y};
        else
            from[turn] = {pick->x, pick->y};
        do {
            turn = (turn + 1) % 8;
        } while (!from.count(turn));
    }
}

/**
 * @brief Scales down the active sides and crops the map around their capitals.
 *
 * Optional scale reduction for curriculum training and fast small-map
 * benchmarks: keeps `opts.active_sides` nearest capitals (preferring
 * land-connected neighbours) and crops the world to a padded island around
 * them without changing kMapW/kMapH.
 */
static void ApplyScaleAndCrop(Map& map, const NewGameOptions& opts) {
    std::vector<int> avail;
    for (int i = 0; i < 8; i++) {
        if (map.sides[i].in_use && map.sides[i].capital) avail.push_back(i);
    }
    if (opts.active_sides > 0 && static_cast<int>(avail.size()) > opts.active_sides) {
        uint32_t hash = static_cast<uint32_t>(opts.seed) * 2654435761U + 0x9e3779b9U;
        int anchor_side = avail[hash % avail.size()];
        int anchor_x = map.sides[anchor_side].cap_x, anchor_y = map.sides[anchor_side].cap_y;

        std::vector<int> land_dist(static_cast<size_t>(kMapW) * kMapH, INT_MAX);
        std::queue<std::pair<int, int>> queue;
        if (anchor_x >= 0 && anchor_y >= 0 && anchor_x < kMapW && anchor_y < kMapH) {
            land_dist[anchor_y * kMapW + anchor_x] = 0;
            queue.push({anchor_x, anchor_y});
        }
        while (!queue.empty()) {
            auto [cur_x, cur_y] = queue.front();
            queue.pop();
            int cur_dist = land_dist[cur_y * kMapW + cur_x];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int next_x = cur_x + dx, next_y = cur_y + dy;
                    if (next_x < 0 || next_y < 0 || next_x >= kMapW || next_y >= kMapH) continue;
                    int next_key = next_y * kMapW + next_x;
                    if (land_dist[next_key] != INT_MAX) continue;
                    if (scn::TerrainAt(map, next_x, next_y) == move::kWater) continue;
                    land_dist[next_key] = cur_dist + 1;
                    queue.push({next_x, next_y});
                }
            }
        }

        auto metric = [&](int side_idx) -> std::tuple<int, int, int, int> {
            if (side_idx == anchor_side) return {0, 0, 0, side_idx};
            int side_x = map.sides[side_idx].cap_x, side_y = map.sides[side_idx].cap_y;
            int dist = (side_x >= 0 && side_y >= 0 && side_x < kMapW && side_y < kMapH)
                               ? land_dist[side_y * kMapW + side_x]
                               : INT_MAX;
            int dx = side_x - anchor_x, dy = side_y - anchor_y;
            int dist_sq = dx * dx + dy * dy;
            return {1, dist == INT_MAX ? 1 : 0, dist < INT_MAX ? dist : dist_sq, side_idx};
        };
        std::sort(avail.begin(), avail.end(),
                  [&](int left, int right) { return metric(left) < metric(right); });
        for (size_t i = static_cast<size_t>(opts.active_sides); i < avail.size(); i++) {
            Side& side = map.sides[avail[i]];
            if (side.capital) side.capital->owner = nullptr;
            side.capital = nullptr;
            side.in_use = false;
        }
    }

    if (opts.crop_margin <= 0) return;

    int min_x = kMapW, max_x = 0, min_y = kMapH, max_y = 0;
    bool any_side = false;
    for (const auto& side : map.sides) {
        if (!side.in_use) continue;
        min_x = std::min(min_x, side.cap_x);
        max_x = std::max(max_x, side.cap_x + 1);
        min_y = std::min(min_y, side.cap_y);
        max_y = std::max(max_y, side.cap_y + 1);
        any_side = true;
    }
    if (!any_side) return;

    int left_x = std::max(2, min_x - opts.crop_margin);
    int right_x = std::min(kMapW - 3, max_x + opts.crop_margin);
    int top_y = std::max(2, min_y - opts.crop_margin);
    int bottom_y = std::min(kMapH - 3, max_y + opts.crop_margin);
    map.active_x0 = left_x;
    map.active_y0 = top_y;
    map.active_x1 = right_x;
    map.active_y1 = bottom_y;

    int plain_tile = 0, water_tile = 0;
    for (int tile = 0; tile < 255; tile++) {
        if (map.terrain_type[tile] == move::kPlain) {
            plain_tile = tile;
            break;
        }
    }
    for (int tile = 0; tile < 255; tile++) {
        if (map.terrain_type[tile] == move::kWater) {
            water_tile = tile;
            break;
        }
    }

    std::vector<City> kept_cities;
    for (auto& city : map.cities) {
        if (city.x >= left_x && city.x + 1 <= right_x && city.y >= top_y &&
            city.y + 1 <= bottom_y) {
            city.index = static_cast<int>(kept_cities.size());
            city.owner = nullptr;
            kept_cities.push_back(std::move(city));
        } else {
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    int tile_x = city.x + dx, tile_y = city.y + dy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < kMapW && tile_y < kMapH) {
                        map.tiles[tile_y * kMapW + tile_x] = plain_tile;
                    }
                }
            }
        }
    }
    map.cities = std::move(kept_cities);
    map.city_at.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    map.city_tile.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    for (auto& city : map.cities) {
        map.city_at[city.y * kMapW + city.x] = &city;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++)
                map.city_tile[(city.y + dy) * kMapW + (city.x + dx)] = &city;
        }
    }
    for (auto& side : map.sides) {
        side.capital = nullptr;
        if (!side.in_use) continue;
        City* cap = (side.cap_x >= 0 && side.cap_y >= 0 && side.cap_x < kMapW && side.cap_y < kMapH)
                            ? map.city_at[side.cap_y * kMapW + side.cap_x]
                            : nullptr;
        if (cap) {
            side.capital = cap;
            cap->owner = &side;
        } else {
            side.in_use = false;
        }
    }

    std::vector<Site> kept_sites;
    for (auto& site : map.sites) {
        if (site.x >= left_x && site.x <= right_x && site.y >= top_y && site.y <= bottom_y) {
            site.index = static_cast<int>(kept_sites.size());
            kept_sites.push_back(std::move(site));
        } else if (site.x >= 0 && site.y >= 0 && site.x < kMapW && site.y < kMapH) {
            map.tiles[site.y * kMapW + site.x] = plain_tile;
        }
    }
    map.sites = std::move(kept_sites);

    std::vector<Sign> kept_signs;
    for (auto& sign : map.signs) {
        if (sign.x >= left_x && sign.x <= right_x && sign.y >= top_y && sign.y <= bottom_y) {
            sign.index = static_cast<int>(kept_signs.size());
            kept_signs.push_back(std::move(sign));
        }
    }
    map.signs = std::move(kept_signs);

    // Leave a 2-tile natural terrain ring around [left_x..right_x]x[top_y..bottom_y]
    // so border cities do not artificially become coastal ports, then convert outer
    // tiles to ocean.
    int bx0 = std::max(0, left_x - 2), bx1 = std::min(kMapW - 1, right_x + 2);
    int by0 = std::max(0, top_y - 2), by1 = std::min(kMapH - 1, bottom_y + 2);
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                int key = y * kMapW + x;
                map.tiles[key] = water_tile;
                map.roads[key] = '\0';
                map.crossing[key] = false;
            }
        }
    }
}

std::unique_ptr<Game> NewGame(const std::string& data_dir, const std::string& scenario,
                              const NewGameOptions& opts) {
    auto game_ptr = std::make_unique<Game>();
    Game& game = *game_ptr;
    game.rng = Rng(opts.seed);
    game.data_dir = data_dir;
    armytype::Load(data_dir + "/TERRAIN0/ARMYTYPE.DAT", game.types);
    game.map = opts.memory_files
                       ? scn::LoadFromFiles(data_dir + "/" + scenario, scenario, *opts.memory_files)
                       : scn::Load(data_dir + "/" + scenario, scenario);
    game.greatest = opts.greatest;
    for (auto& [opt_name, opt_val] : opts.options) {
        if (int* field = game.map->options.Field(opt_name)) *field = opt_val;
    }
    for (auto& side : game.map->sides) {
        auto setup_it = opts.sides.find(side.index);
        if (setup_it != opts.sides.end() && side.in_use) {
            const SideSetup& setup = setup_it->second;
            if (!setup.name.empty()) side.name = setup.name;  // retyped on the setup screen
            if (setup.off) {
                if (side.capital) side.capital->owner = nullptr;
                side.in_use = false;
            } else {
                side.computer = setup.computer;
                side.level = setup.level != kNone ? setup.level : side.level;
                side.card = setup.card;
            }
        }
    }
    if ((opts.active_sides > 0 && opts.active_sides < 8) || opts.crop_margin > 0) {
        ApplyScaleAndCrop(*game.map, opts);
    }

    for (auto& side : game.map->sides) {
        side.alive = side.in_use;
        side.owner_index = side.index;
        if (side.in_use) game.sides.push_back(&side);
    }
    // Every side is observed unless the map is hidden and a human plays.
    int humans = 0;
    for (Side* side : game.sides)
        if (!side->computer) humans++;
    bool observed = !(game.map->options.hidden_map != 0 && humans >= 1);
    for (Side* side : game.sides) side->observe = observed;

    for (auto& city : game.map->cities) city.owner_index = city.owner ? city.owner->index : kNone;
    if (game.map->options.quick_start != 0) DealCities(game);

    for (auto& city : game.map->cities) {
        city.slots = rules::CitySlots(city.produces, game.types, game.rng);
        city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
        city.producing = kNone;  // slot index being built (0-based)
        city.countdown = 0;
        city.vector_to = kNone;
    }
    scn::RefreshCityTiles(*game.map);

    // items 0-7 are the eight sides' standards (docs/rules.md > Heroes)
    for (size_t i = 0; i < game.map->items.size(); i++) {
        Item& item = game.map->items[i];
        if (i < 8 && item.type == rules::kItemStandard) item.standard_of = static_cast<int>(i);
    }

    diplomacy::Init(game);
    SetupGarrisons(game);
    site::Setup(game);
    for (Side* side : game.sides) {
        RevealStart(game, *side);
        if (game.map->options.hidden_map != 0 && opts.crop_margin > 0) {
            int bx0 = std::max(0, game.map->active_x0 - 2),
                bx1 = std::min(kMapW - 1, game.map->active_x1 + 2);
            int by0 = std::max(0, game.map->active_y0 - 2),
                by1 = std::min(kMapH - 1, game.map->active_y1 + 2);
            auto& mask = game.explored[side->index];
            if (static_cast<int>(mask.size()) == kMapW * kMapH) {
                for (int y = 0; y < kMapH; y++) {
                    for (int x = 0; x < kMapW; x++) {
                        if (x < bx0 || x > bx1 || y < by0 || y > by1) mask[y * kMapW + x] = 1;
                    }
                }
            }
        }
    }
    ai::StartGame(game);

    // The turn order (see docs/formats/scenario.md), as the scenario gives it;
    // Random Turns shuffles it at each round's end.
    game.turn_order = {0, 1, 2, 3, 4, 5, 6, 7};
    game.order_at = -1;  // the place in it of the side to play
    game.current = 0;    // index into game.sides
    game.side = game.sides.empty() ? nullptr : game.sides[0];
    game.path_cache = move::NewPathCache();
    return game_ptr;
}

/**
 * @brief Puts a side with no city left out of the game.
 *
 * Its heroes drop what they carry, its armies are gone, it is vanquished, and
 * its gold is 0. Every side then stands uneasy with it, both ways, state and
 * proposal.
 */
static Fallen Eliminate(Game& game, Side& side, bool boxed) {
    std::vector<Army*> mine;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        if (game.armies[i]->owner == side.index) mine.push_back(game.armies[i]);
    }
    Disband(game, side, mine);
    history::RecordDeed(game, &side, DeedType::kSideVanquished, side.index, 0, "");
    int line = game.rng.Dice(1, kFallenLines, -1);
    game.log.push_back(side.name + " has been eliminated.");
    side.alive = false;
    side.gold = 0;
    // Matches the reference: every pair with the fallen side, its pair with itself too.
    for (int other = 0; other < 8; other++) {
        for (int pair_idx : {side.index * 8 + other, other * 8 + side.index}) {
            game.diplomacy.state[pair_idx] = diplomacy::kIntermediate;
            game.diplomacy.proposal[pair_idx] = diplomacy::kIntermediate;
        }
    }
    return Fallen{&side, line, boxed};
}

std::vector<Fallen> EliminateFallen(Game& game) {
    bool human = false;
    for (Side* side : game.sides)
        if (side->alive && !side->computer) human = true;
    std::vector<Fallen> fallen;
    for (Side* side : game.sides) {
        if (side->alive && SideCities(game, *side).empty())
            fallen.push_back(Eliminate(game, *side, human || !side->computer));
    }
    return fallen;
}

/** @brief Updates `side`'s income, upkeep and treasury for the start of its turn. */
static void ApplyIncome(Game& game, Side& side) {
    side.income = Income(game, side);
    side.upkeep_total = Upkeep(game, side);
    side.gold = std::max(0, side.gold + side.income - side.upkeep_total);
}

/**
 * @brief Places an army arriving from the road, or sends it back.
 *
 * It needs room on one of the city's four tiles -- there is no spilling round a
 * full city -- and the city must still be the side's. One sent back that fails
 * again, at home, is disbanded. It keeps the moves it set off with.
 * @return False if the army was disbanded.
 */
static bool Deliver(Game& game, Army* army, City& city) {
    auto [x, y] = FreeTileIn(game, city, false);
    if (x == kNone || city.owner_index != army->owner) {
        if (army->returning || army->home_city == city.index) return false;
        City* home = game.map->City(army->home_city);
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

/**
 * @brief Advances vectored armies and city production for `side`, logging in `side.produced`.
 *
 * The armies on the road move on first, from the top of the army table down;
 * then each city counts down, and only while its countdown is above 0: one that
 * reaches 0 builds if the side has gold and the army has room on the city's own
 * four tiles (or goes vectored), and otherwise stays at 0 -- idle until its
 * production is set again. A new army has its full move. Whether the city
 * starts the next one is the caller's: a human's do, a computer's wait for its
 * AI.
 */
static std::vector<Produced> RunProduction(Game& game, Side& side) {
    std::vector<Produced> arrived, built;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->transit && army->owner == side.index) {
            army->transit->turns--;
            if (army->transit->turns <= 0) {
                if (army->transit->dest == kStandard) {
                    auto [std_x, std_y] = StandardAt(game, side);
                    army->transit.reset();
                    if (std_x != kNone &&
                        static_cast<int>(ArmiesAt(game, std_x, std_y).size()) < rules::kMaxStack) {
                        army->x = std_x;
                        army->y = std_y;
                        army->returning = false;
                        arrived.push_back(Produced{"arrived", army->type, kNone, true});
                    } else {
                        army->returning = true;
                        army->transit = Transit{kTransitTurns, army->home_city};
                    }
                } else {
                    City* dest = game.map->City(army->transit->dest);
                    army->transit.reset();
                    if (!dest || !Deliver(game, army, *dest))
                        army->disbanded = true;
                    else if (!army->transit)
                        arrived.push_back(Produced{"arrived", army->type, dest->index, false});
                }
            }
        }
    }

    for (City* city : SideCities(game, side)) {
        if (city->producing != kNone && city->countdown > 0) {
            city->countdown--;
            if (city->countdown <= 0) {
                city->countdown = 0;
                if (side.gold > 0) {
                    bool vectored = city->vector_to != kNone && city->vector_to != city->index;
                    auto [x, y] = FreeTileIn(game, *city, false);
                    if (x != kNone || vectored) {
                        const Slot& slot = city->slots[city->producing];
                        Army army = rules::ArmyFromSlot(slot, side.enhanced);
                        army.owner = side.index;
                        army.home_city = city->index;
                        army.moves = army.max_moves;
                        army.x = x;
                        army.y = y;
                        if (vectored) {
                            army.transit = Transit{kTransitTurns, city->vector_to};
                            army.x = kNone;
                            army.y = kNone;
                        }
                        game.Add(army);
                        built.push_back(Produced{vectored ? "sent" : "built", army.type,
                                                 city->index, false});
                    }
                }
            }
        }
    }

    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        if (game.armies[i]->disbanded) game.armies.erase(game.armies.begin() + i);
    }
    side.produced = arrived;
    for (auto& entry : built) side.produced.push_back(entry);
    return built;
}

/**
 * @brief Resets `side`'s armies' movement and turn flags for a new turn.
 *
 * Each army gets its maximum (20 at sea) plus what it kept, 2 at most; a goal
 * it stands on is forgotten; every hero carrying double movement adds each
 * army's maximum again to every army on its tile -- a second such hero there
 * adds it again; then 99 at most.
 */
static void ResetMovement(Game& game, Side& side) {
    for (Army* army : game.armies) {
        if (army->owner == side.index) {
            army->done = false;
            army->offered = false;
        }
    }
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->owner == side.index && !army->transit) {
            int carry = std::min(army->moves, rules::kMoveCarry);
            army->moves = (army->at_sea ? rules::kSeaMoves : army->max_moves) + carry;
            if (army->target && army->target->first == army->x && army->target->second == army->y)
                army->target.reset();
        }
    }
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* hero = game.armies[i];
        if (hero->owner != side.index || hero->type != armytype::kHero || hero->transit) continue;
        bool double_move = false;
        for (Item* item : hero->items)
            if (item->type == rules::kItemDoubleMove) double_move = true;
        if (!double_move) continue;
        auto here = ArmiesAt(game, hero->x, hero->y);
        if (static_cast<int>(here.size()) > rules::kMaxStack) here.resize(rules::kMaxStack);
        for (Army* army : here) army->moves += army->max_moves;
    }
    for (Army* army : game.armies) {
        if (army->owner == side.index && army->moves > rules::kMaxMove)
            army->moves = rules::kMaxMove;
    }
}

/**
 * @brief Turns fortified stacks into encampments as `side`'s next turn opens.
 *
 * A stack's tile gets the tower flag when it still has its full movement and
 * stands on plain, forest, hills, a bridge, marsh, the tower terrain or a road.
 */
static void Encamp(Game& game, Side& side) {
    static const std::set<int> kEncampOn = {move::kPlain,  move::kForest, move::kHills,
                                            move::kBridge, move::kMarsh,  move::kTower};
    for (Army* army : game.armies) {
        if (army->owner == side.index && army->fortified && !army->transit && army->x != kNone &&
            army->moves >= army->max_moves) {
            bool road = scn::RoadAt(*game.map, army->x, army->y) % 32 != 0;
            if (road || kEncampOn.count(scn::TerrainAt(*game.map, army->x, army->y))) {
                game.towers.insert(army->y * game.map->width + army->x);
            }
        }
    }
}

bool TowerAt(const Game& game, int x, int y) {
    return game.towers.count(y * game.map->width + x) > 0;
}

void TidyTowers(Game& game) {
    if (game.towers.empty()) return;
    std::set<int> held;
    for (Army* army : game.armies)
        if (army->x != kNone && !army->transit) held.insert(army->y * game.map->width + army->x);
    for (auto it = game.towers.begin(); it != game.towers.end();) {
        if (!held.count(*it))
            it = game.towers.erase(it);
        else
            ++it;
    }
}

bool HeroWithDoubleMoveAt(const Game& game, const Side& side, int x, int y) {
    for (Army* army : game.armies) {
        if (army->owner == side.index && !army->transit && army->x == x && army->y == y &&
            army->type == armytype::kHero) {
            for (Item* item : army->items)
                if (item->type == rules::kItemDoubleMove) return true;
        }
    }
    return false;
}

/**
 * @brief Disembarks any of `side`'s armies standing in a city as its turn opens.
 *
 * At the start of every side's turn, before anything else, its armies standing
 * in a city -- any city, ruins too -- are no longer at sea. It costs no
 * movement: the reset that follows sees them ashore. A stack that took a city
 * from the sea stays at sea until then.
 */
static void ComeAshore(Game& game, Side& side) {
    for (Army* army : game.armies) {
        if (army->owner == side.index && army->at_sea && !army->transit && army->x != kNone &&
            CityAt(game, army->x, army->y))
            army->at_sea = false;
    }
}

void FollowCapital(Game& game, Side& side) {
    if (side.cap_now_x == kNone) {
        side.cap_now_x = side.cap_x;
        side.cap_now_y = side.cap_y;
    }
    int playing = 0;
    for (Side* other : game.sides)
        if (other->alive) playing++;
    if (playing != 1) {
        City* cap_city = CityAt(game, side.cap_x, side.cap_y);
        if (cap_city && cap_city->razed) cap_city = nullptr;
        if (!SideCities(game, side).empty() && (!cap_city || cap_city->owner_index != side.index)) {
            City* nearest = NearestCity(game, side.cap_now_x, side.cap_now_y, &side);
            if (nearest) {
                side.cap_now_x = nearest->x;
                side.cap_now_y = nearest->y;
            }
            return;
        }
    }
    side.cap_now_x = side.cap_x;
    side.cap_now_y = side.cap_y;
}

/** @brief Notifies `side`'s active quest that a new turn has begun. */
static void TurnQuest(Game& game, Side& side) {
    auto quest_result = quest::Notify(game, side, "turn");
    if (quest_result && !quest_result->failed.empty())
        game.log.push_back("Quest abandoned: " + quest_result->failed + ".");
}

bool StartTurn(Game& game, Side& side) {
    ComeAshore(game, side);
    FollowCapital(game, side);
    TidyExplored(game, side.index);
    side.diplo_news = diplomacy::Apply(game, side);
    for (auto& msg : side.diplo_news) game.log.push_back(msg);
    side.hero_offer.reset();
    side.production_due = false;
    if (side.computer) {
        if (SideCities(game, side).empty()) return false;
        auto offer = hero::Offer(game, side);
        if (offer) hero::Recruit(game, side, *offer);
        hero::CheckPromotions(game, side);
        ApplyIncome(game, side);
        RunProduction(game, side);
        ResetMovement(game, side);
        TidyTowers(game);
        Encamp(game, side);
        // The army cycle's cursor goes back to the capital.
        if (side.ai) side.ai->cursor = std::make_pair(side.cap_now_x, side.cap_now_y);
        TurnQuest(game, side);
        return true;
    }
    ResetMovement(game, side);
    TidyTowers(game);
    Encamp(game, side);
    ApplyIncome(game, side);
    TurnQuest(game, side);
    side.promoted = hero::CheckPromotions(game, side);
    side.hero_offer = hero::Offer(game, side);
    side.production_due = true;
    return true;
}

void HumanProduction(Game& game, Side& side) {
    if (!side.production_due) return;
    side.production_due = false;
    if (game.turn == 1) return;
    for (auto& entry : RunProduction(game, side)) {
        City* city = game.map->City(entry.city);
        if (city && city->producing != kNone &&
            city->producing < static_cast<int>(city->slots.size()))
            city->countdown = city->slots[city->producing].time;
    }
}

bool Producing(const City& city) {
    return city.producing != kNone && city.countdown > 0;
}

/**
 * @brief Sets the turn order for the next round.
 *
 * With Random Turns enabled, performs 20 swaps of two sides picked by dice.
 */
static void TurnOrder(Game& game) {
    game.turn_order = {0, 1, 2, 3, 4, 5, 6, 7};
    if (game.map->options.random_turns != 0) {
        for (int i = 0; i < 20; i++) {
            int first = game.rng.Dice(1, 8, -1), second = game.rng.Dice(1, 8, -1);
            std::swap(game.turn_order[first], game.turn_order[second]);
        }
    }
}

/** @brief Advances to and starts the turn of the next living side in the turn order. */
static Side* NextSide(Game& game, Side* leaving) {
    for (int step = 0; step < 3 * 8 + 1; step++) {
        if (game.order_at >= 7) {
            game.order_at = -1;
            game.turn++;
            // the round's end: the order, the fallen put out of the game, the end
            // looked for, the neutral cities' production, then the history
            TurnOrder(game);
            EndRound(game, leaving);
            if (game.ending.over) {
                game.side = nullptr;
                return nullptr;
            }
            history::Record(game);
        }
        game.order_at++;
        Side* side = game.map->Side(game.turn_order[game.order_at]);
        if (side && side->in_use && side->alive) {
            game.side = side;
            game.current = static_cast<int>(std::find(game.sides.begin(), game.sides.end(), side) -
                                            game.sides.begin());
            if (StartTurn(game, *side)) return side;
        }
    }
    game.side = nullptr;
    return nullptr;
}

Side* EndTurn(Game& game) {
    Side* leaving = game.side;
    if (leaving && leaving->alive && (leaving->computer || !game.won))
        diplomacy::ScoreUpdate(game, *leaving);
    // what the last round's end found is for the turn that followed it only
    game.ending = Ending{};
    return NextSide(game, leaving);
}

Side* Begin(Game& game) {
    game.order_at = -1;
    game.ending = Ending{};
    return NextSide(game, nullptr);
}

Side* NextHuman(Game& game) {
    for (int i = std::max(game.order_at, 0); i <= 7; i++) {
        Side* side = game.map->Side(game.turn_order[i]);
        if (side && side->in_use && side->alive && !side->computer) return side;
    }
    for (Side* side : game.sides)
        if (side->alive && !side->computer) return side;
    return nullptr;
}

/** @brief Erases every army in `dead` from `game.armies`. */
static void RemoveArmies(Game& game, const std::vector<Army*>& dead) {
    std::set<const Army*> gone(dead.begin(), dead.end());
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        if (gone.count(game.armies[i])) game.armies.erase(game.armies.begin() + i);
    }
}

/**
 * @brief Returns whether `armies` could fly off a water tile together.
 *
 * A hero's flight item carries them all; else every army flies and none is at
 * sea, or every army but the heroes flies and one of the fliers is not at sea.
 */
static bool CanFlyOff(const Game& game, const std::vector<Army*>& armies) {
    bool hero_flight = false, all_fly = true, non_heroes_fly = true, free_flier = false;
    for (Army* army : armies) {
        bool flies = game.types.ById(army->type)->flies;
        if (army->type == armytype::kHero) {
            for (Item* item : army->items)
                if (item->type == rules::kItemFlight) hero_flight = true;
        }
        if (!flies || army->at_sea) all_fly = false;
        if (army->type != armytype::kHero && !flies) non_heroes_fly = false;
        if (flies && !army->at_sea) free_flier = true;
    }
    return hero_flight || all_fly || (non_heroes_fly && free_flier);
}

int SinkStranded(Game& game, Side& side, int x, int y) {
    auto wet = [&](int tile_x, int tile_y) {
        return scn::TerrainAt(*game.map, tile_x, tile_y) == move::kWater ||
               scn::IsCrossing(*game.map, tile_x, tile_y);
    };
    std::map<int, std::vector<Army*>> tiles;
    for (Army* army : game.armies) {
        if (army->owner != side.index || army->transit || army->x == kNone || army->x < 0 ||
            army->y < 0)
            continue;
        if (x != kNone && (army->x != x || army->y != y)) continue;
        if (wet(army->x, army->y)) tiles[army->y * game.map->width + army->x].push_back(army);
    }
    std::vector<Army*> dead;
    for (auto& [tile_key, here] : tiles) {
        if (CanFlyOff(game, here)) continue;
        for (Army* army : here) {
            if (!army->at_sea && army->type != armytype::kHero &&
                !game.types.ById(army->type)->flies)
                dead.push_back(army);
        }
    }
    RemoveArmies(game, dead);
    return static_cast<int>(dead.size());
}

Sign* SignAt(Game& game, int x, int y) {
    for (auto& sign : game.map->signs)
        if (sign.x == x && sign.y == y) return &sign;
    return nullptr;
}

void Disband(Game& game, Side& side, const std::vector<Army*>& armies) {
    for (Army* army : armies) {
        if (army->type == armytype::kHero) {
            hero::DropItems(game, army, army->x, army->y, side.index);
            if (side.quest && side.quest->hero == army) side.quest.reset();
        }
    }
    RemoveArmies(game, armies);
}

int Loot(const Game& game, const Side* loser) {
    if (!loser) return 0;
    int city_count = static_cast<int>(SideCities(game, *loser).size());
    int share = city_count > 1 ? loser->gold / city_count : loser->gold;
    return share / 2;
}

Battle DecideAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    combat::Lines lines = combat::BattleLines(game, stack, x, y);
    int attacker_side = stack.empty() ? kNone : stack[0]->owner;
    if (lines.city && attacker_side != kNone) lines.city->attacked |= 1 << attacker_side;
    Battle result = combat::Resolve(game, lines.attackers, lines.defenders, x, y);
    result.lines.attackers = lines.attackers;
    result.lines.defenders = lines.defenders;
    result.lines.city = lines.city;
    Side* loser = lines.def_owner != kNone ? game.map->Side(lines.def_owner) : nullptr;
    if (result.won && lines.city && loser) result.loot = Loot(game, loser);
    // The stack's hero: the leader if a hero, else its first hero.
    for (Army* army : stack) {
        if (army->type == armytype::kHero) {
            result.hero = army;
            break;
        }
    }
    result.fought = Battle::Fought{stack, x, y, lines.def_owner};
    return result;
}

void ChargeAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    City* city = CityAt(game, x, y);
    int terrain = city && !city->razed ? move::kCity : scn::TerrainAt(*game.map, x, y);
    int cost = std::max(2, terrain >= 0 && terrain < 12 ? move::kCost[terrain] : 0);
    move::Mode mode = move::StackMode(game, stack);
    if (mode.mode != move::kFlying && !mode.at_sea &&
        (terrain == move::kWater || terrain == move::kShore)) {
        for (Army* army : stack) {
            bool flight = false;
            if (army->type == armytype::kHero) {
                for (Item* item : army->items)
                    if (item->type == rules::kItemFlight) flight = true;
            }
            if (!game.types.ById(army->type)->flies && !flight) {
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

Battle ResolveAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    Battle battle = DecideAttack(game, stack, x, y);
    ApplyAttack(game, battle);
    return battle;
}

static Army* Medal(Game& game, const Battle& result, int side);

Battle& ApplyAttack(Game& game, Battle& result) {
    if (!result.fought) return result;
    Battle::Fought fought = *result.fought;
    result.fought.reset();
    const auto& stack = fought.stack;
    int x = fought.x, y = fought.y, def_owner = fought.def_owner;
    const auto& attackers = result.lines.attackers;
    const auto& defenders = result.lines.defenders;
    City* city = result.lines.city;
    // The city on the tile, ruins too.
    City* tile_city = game.map->city_tile[y * game.map->width + x];
    const auto& boats = result.boats;

    auto fallen = [&](Army* hero) {
        history::RecordDeed(game, game.map->Side(hero->owner == kNone ? 8 : hero->owner),
                            DeedType::kHeroKilled,
                            tile_city ? tile_city->index : history::kKilledInBattle, 0, hero->name);
    };
    int mine = stack.empty() ? kNone : stack[0]->owner;
    {
        int heroes_lost = 0;
        for (Army* dead_def : result.dead_defenders)
            if (dead_def->type == armytype::kHero) heroes_lost++;
        ai::RecordBattle(game, def_owner, mine, x, y, heroes_lost,
                         static_cast<int>(result.dead_defenders.size()),
                         result.won && !defenders.empty(), city != nullptr);
    }
    for (Army* attacker : result.dead_attackers)
        history::Tally(game, attacker, def_owner, boats.count(attacker) > 0);
    for (Army* defender : result.dead_defenders)
        history::Tally(game, defender, mine, boats.count(defender) > 0);
    // A big battle won at a city goes down in history: eight armies or more,
    // four of them defending -- by the hero's name when a hero led the stack.
    if (tile_city && result.won && mine != kNone && attackers.size() + defenders.size() >= 8 &&
        defenders.size() >= 4) {
        Side* side = game.map->Side(mine);
        if (result.hero)
            history::RecordDeed(game, side, DeedType::kCityWon, history::kWonByName,
                                tile_city->index, result.hero->name);
        else
            history::RecordDeed(game, side, DeedType::kCityWon, mine, tile_city->index, "");
    }
    // The fallen heroes' items drop on the tile fought for, attacker's too.
    for (Army* attacker : result.dead_attackers) {
        if (attacker->type == armytype::kHero) {
            fallen(attacker);
            hero::DropItems(game, attacker, x, y, mine);
        }
    }
    for (Army* defender : result.dead_defenders) {
        if (defender->type == armytype::kHero) {
            fallen(defender);
            hero::DropItems(game, defender, x, y, mine);
        }
    }
    hero::BattleExperience(game, attackers, defenders, result, city != nullptr);
    Side* my_side =
            game.map->Side(stack.empty() ? 0 : (stack[0]->owner == kNone ? 0 : stack[0]->owner));
    {
        quest::Event event;
        event.stack = attackers;
        event.killed = result.dead_defenders;
        result.quest = quest::Notify(game, *my_side, "battle", event);
    }

    RemoveArmies(game, result.dead_attackers);
    RemoveArmies(game, result.dead_defenders);
    TidyTowers(game);

    if (result.won && city) {
        Side* winner = game.map->Side(stack[0]->owner);
        Side* loser = def_owner != kNone ? game.map->Side(def_owner) : nullptr;
        if (loser) {
            int loot = result.loot;
            winner->gold += loot;
            loser->gold = std::max(0, loser->gold - 2 * loot);
        }
        // Whom the city was taken from: its owner, or no one when that side
        // takes its own back.
        if (city->owner_index != kNone) {
            city->claim = city->claim == winner->index ? rules::kNeutral : city->owner_index;
        }
        city->attacked = 0;
        city->countdown = 0;
        city->vector_to = kNone;
        // Every city that was sending its armies here stops: no vector, and
        // nothing in production either.
        for (auto& other_city : game.map->cities) {
            if (other_city.vector_to == city->index) {
                other_city.vector_to = kNone;
                other_city.producing = kNone;
                other_city.countdown = 0;
            }
        }
        city->owner_index = winner->index;
        scn::SetCityTiles(*game.map, *city);
        move::Invalidate(game);
        result.captured = city;
        // A computer's quest hero taking its quest city razes it.
        bool handled = winner->computer && ai::QuestCapture(game, *winner, *city, result.attackers);
        if (!handled && winner->computer) {
            quest::Event event;
            event.city = city;
            event.stack = result.attackers;
            auto quest_res = quest::Notify(game, *winner, "occupy", event);
            if (quest_res) result.quest = quest_res;
        }
    }

    // The survivors walk into the tile they just cleared -- as many as fit.
    if (result.won) {
        game.towers.erase(y * game.map->width + x);
        int room = rules::kMaxStack - static_cast<int>(ArmiesAt(game, x, y).size());
        for (Army* attacker : result.attackers) {
            if (room <= 0) break;
            attacker->x = x;
            attacker->y = y;
            room--;
        }
        // And the winner sees round what it took: the tile fought for, then each
        // of a city's four, 2 out from a city.
        if (game.map->options.hidden_map != 0 && mine != kNone) {
            bool flying = move::ModeOf(game, result.attackers) == move::kFlying;
            int found = Reveal(game, mine, x, y, flying);
            if (city) {
                for (int dx = 0; dx <= 1; dx++)
                    for (int dy = 0; dy <= 1; dy++)
                        found += Reveal(game, mine, city->x + dx, city->y + dy, false);
            }
            if (found > 0) move::Invalidate(game);
        }
    }
    result.medal = Medal(game, result, mine);
    return result;
}

/** @brief Returns whether `army` is an ordinary army (not a hero or magical creature). */
static bool Ordinary(const Game& game, const Army* army) {
    return army->type != armytype::kHero && game.types.ById(army->type)->bonus[48] == 0;
}

/**
 * @brief Awards a medal to one surviving ordinary army after a significant victory.
 *
 * A stack with a hero and no army at sea that won a fight worth the name -- two
 * armies a side, or before turn 20 a first defender above strength 3 -- gives
 * one of its ordinary armies a medal, 40% of the time less 5 for each of the
 * side's ordinary armies with one already, 12% at least. A medal is +1 strength
 * (9 at most), or +1 move past strength 14; four at most.
 * @return The decorated army, or null.
 */
static Army* Medal(Game& game, const Battle& result, int side) {
    if (side == kNone || !result.won) return nullptr;
    const auto& survivors = result.attackers;
    for (Army* army : survivors)
        if (army->at_sea) return nullptr;
    size_t n_atk = result.lines.attackers.size(), n_def = result.lines.defenders.size();
    bool counted = (n_atk >= 2 && n_def >= 2) ||
                   (game.turn < 20 && n_def >= 1 && result.def_strength0 > 3);
    if (!counted) return nullptr;
    std::vector<Army*> plain;
    for (Army* army : survivors)
        if (Ordinary(game, army)) plain.push_back(army);
    if (plain.empty()) return nullptr;
    int decorated = 0;
    for (Army* army : game.armies)
        if (army->owner == side && Ordinary(game, army) && army->medals > 0) decorated++;
    int chance = std::max(12, 40 - 5 * decorated);
    if (!(game.rng.Dice(1, 100, 0) < chance) || !result.hero) return nullptr;
    Army* chosen = plain[game.rng.Dice(1, static_cast<int>(plain.size()), -1)];
    if (chosen->medals >= 4) return nullptr;
    chosen->medals++;
    if (chosen->strength > 14)
        chosen->max_moves++;
    else
        chosen->strength = std::min(9, chosen->strength + 1);
    return chosen;
}

/** @brief Returns a production slot's pillage value (half its purchase price). */
static int SlotValue(const Game& game, const Slot& slot) {
    return std::abs(game.types.ById(slot.type)->price) / 2;
}

/** @brief Recomputes `city`'s defence and stops invalid production after losing slots. */
static void Recompute(Game& game, City& city) {
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    if (city.producing != kNone && city.producing >= static_cast<int>(city.slots.size())) {
        city.producing = kNone;
        city.countdown = 0;
    }
}

void AddDiploScore(Game& game, Side& side, int points) {
    side.diplo_score += points;
}

std::shared_ptr<QuestResult> Occupy(Game& game, Side& side, City& city,
                                    const std::vector<Army*>& stack) {
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    return quest::Notify(game, side, "occupy", event);
}

Spoils Pillage(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    Spoils out;
    if (city.slots.empty()) return out;
    Slot slot = city.slots.back();  // slots are sorted cheapest first
    city.slots.pop_back();
    int gold = SlotValue(game, slot);
    out.gold = gold;
    out.lost.emplace_back(slot.type, gold);
    side.gold += gold;
    AddDiploScore(game, side, game.rng.Dice(1, 5, 0));
    Recompute(game, city);
    quest::Event event;
    event.city = &city;
    event.gold = gold;
    event.stack = stack;
    quest::Notify(game, side, "pillage", event);
    return out;
}

Spoils Sack(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    Spoils out;
    if (city.slots.size() < 2) return out;
    int gold = 0;
    for (size_t i = 1; i < city.slots.size(); i++) {
        int val = SlotValue(game, city.slots[i]);
        out.lost.emplace_back(city.slots[i].type, val);
        gold += val;
    }
    city.slots.resize(1);
    out.gold = gold;
    side.gold += gold;
    AddDiploScore(game, side, game.rng.Dice(1, 10, 5));
    Recompute(game, city);
    quest::Event event;
    event.city = &city;
    event.gold = gold;
    event.stack = stack;
    quest::Notify(game, side, "pillage", event);
    return out;
}

/** @brief Turns `city` into ruins and stops any cities vectoring into it. */
static void MakeRuins(Game& game, City& city) {
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
    // Every city sending its armies here stops: no vector, nothing in production.
    for (auto& other_city : game.map->cities) {
        if (other_city.vector_to == city.index) {
            other_city.vector_to = kNone;
            other_city.producing = kNone;
            other_city.countdown = 0;
        }
    }
    scn::SetCityTiles(*game.map, city);
    move::Invalidate(game);
}

void Raze(Game& game, Side& side, City& city, const std::vector<Army*>& stack, bool own_city) {
    MakeRuins(game, city);
    if (own_city) {
        AddDiploScore(game, side, game.rng.Dice(1, 25, 25));
        return;
    }
    AddDiploScore(game, side, game.rng.Dice(1, 15, 10));
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    quest::Notify(game, side, "raze", event);
}

void Resign(Game& game, Side& side) {
    for (auto& city : game.map->cities) {
        if (!city.razed && city.owner_index == side.index) MakeRuins(game, city);
    }
    Disband(game, side, SideArmies(game, side));
    if (game.map->options.hidden_map != 0) {
        game.explored[side.index].assign(static_cast<size_t>(game.map->width) * game.map->height,
                                         1);
    }
}

// Tiles a side has seen: a mask per side.

/** @brief Returns the explored-tile mask for `side`, creating it when `make` is true. */
static std::vector<uint8_t>* MaskFor(Game& game, int side, bool make) {
    auto mask_it = game.explored.find(side);
    if (mask_it != game.explored.end()) return &mask_it->second;
    if (!make) return nullptr;
    auto& mask = game.explored[side];
    mask.assign(static_cast<size_t>(game.map->width) * game.map->height, 0);
    return &mask;
}

bool HasActiveHuman(const Game& game) {
    for (const Side* side : game.sides) {
        if (side && side->alive && !side->computer && !SideCities(game, *side).empty()) return true;
    }
    return false;
}

int ViewerSide(const Game& game, const Side* player) {
    if (!HasActiveHuman(game)) return kNone;
    return player ? player->index : kNone;
}

bool Seen(const Game& game, int side, int x, int y) {
    if (game.map->options.hidden_map == 0) return true;
    if (x < 0 || y < 0 || x >= game.map->width || y >= game.map->height) return false;
    if (side == kNone) return true;
    int idx = y * game.map->width + x;
    auto side_it = game.explored.find(side);
    if (side_it != game.explored.end() && side_it->second[idx] == 1) return true;
    if (side >= 0 && side < 8) {
        for (int ally = 0; ally < 8; ally++) {
            if (ally != side && game.diplomacy.allied[side * 8 + ally]) {
                auto ally_it = game.explored.find(ally);
                if (ally_it != game.explored.end() && ally_it->second[idx] == 1) return true;
            }
        }
    }
    return false;
}

int Reveal(Game& game, int side, int x, int y, bool flying) {
    if (game.map->options.hidden_map == 0) return 0;
    auto& mask = *MaskFor(game, side, true);
    bool on_city = x >= 0 && y >= 0 && x < game.map->width && y < game.map->height &&
                   game.map->city_tile[y * game.map->width + x];
    int radius = (flying || on_city) ? 2 : 1;
    int found = 0;
    for (int tile_y = y - radius; tile_y <= y + radius; tile_y++) {
        for (int tile_x = x - radius; tile_x <= x + radius; tile_x++) {
            if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width &&
                tile_y < game.map->height) {
                int key = tile_y * game.map->width + tile_x;
                if (!mask[key]) {
                    mask[key] = 1;
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

int FogCell(const Game& game, int side, int x, int y) {
    if (Seen(game, side, x, y)) return kNone;
    int bits = 0;
    for (int i = 0; i < 8; i++) {
        int nbr_x = x + kFogDx[i], nbr_y = y + kFogDy[i];
        if (nbr_x < 0 || nbr_y < 0 || nbr_x >= game.map->width || nbr_y >= game.map->height ||
            !Seen(game, side, nbr_x, nbr_y))
            bits += 1 << i;
    }
    int cell = kFogCell[bits];
    return cell == kFogNone ? kNone : cell;
}

void TidyExplored(Game& game, int side) {
    if (game.map->options.hidden_map == 0) return;
    auto& mask = *MaskFor(game, side, true);
    int map_width = game.map->width, map_height = game.map->height;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < map_width; x++) {
            for (int y = 0; y < map_height; y++) {
                int key = y * map_width + x;
                if (!mask[key]) {
                    int bits = 0;
                    for (int i = 0; i < 8; i++) {
                        int nbr_x = x + kFogDx[i], nbr_y = y + kFogDy[i];
                        if (nbr_x < 0 || nbr_y < 0 || nbr_x >= map_width || nbr_y >= map_height ||
                            !mask[nbr_y * map_width + nbr_x])
                            bits += 1 << i;
                    }
                    if (kFogCell[bits] == kFogNone) mask[key] = 1;
                }
            }
        }
    }
}

void RevealStart(Game& game, const Side& side) {
    if (game.map->options.hidden_map == 0) return;
    for (City* city : SideCities(game, side)) Reveal(game, side.index, city->x, city->y, false);
    for (Army* army : SideArmies(game, side))
        if (!army->transit) Reveal(game, side.index, army->x, army->y, false);
    TidyExplored(game, side.index);
}

void AcceptSurrender(Game& game, Side& side) {
    for (Side* other : game.sides)
        if (other->computer) other->alive = false;
    game.won = true;
    history::RecordDeed(game, &side, DeedType::kSideVictorious, side.index, 0, "");
}

bool SurrenderPending(const Game& game) {
    return game.surrender_offered && !game.surrender_shown && !game.won;
}

Ending EndRound(Game& game, Side* last) {
    auto fallen = EliminateFallen(game);
    Ending ending = CheckEnd(game);
    ending.fallen = fallen;
    if (!ending.message.empty()) game.log.push_back(ending.message);
    game.ending = ending;
    NeutralProduction(game, last ? last : game.map->Side(kSetupPlayer));
    return ending;
}

bool SetCityProduction(Game& game, const Side* who, City& city, int slot) {
    if (slot < 0 || slot >= static_cast<int>(city.slots.size())) return false;
    if ((who ? who->gold : 0) < city.slots[slot].cost + 30 && game.turn > 5) return false;
    SetProduction(game, city, slot);
    return true;
}

/**
 * @brief Builds a neutral city's garrison on its top-left tile with 0 moves.
 *
 * Placed on its top-left tile, room or not, with no moves, and the city builds
 * nothing more until it is set to again.
 */
static void ProduceGarrison(Game& game, City& city, const Side* who) {
    if (city.producing != kNone && city.producing < static_cast<int>(city.slots.size())) {
        Army army = rules::ArmyFromSlot(city.slots[city.producing], who && who->enhanced);
        army.x = city.x;
        army.y = city.y;
        army.owner = city.owner_index;
        army.moves = 0;
        army.home_city = city.index;
        game.Add(army);
    }
    city.producing = kNone;
    city.countdown = 0;
}

void NeutralProduction(Game& game, const Side* who) {
    if (!(game.map->options.neutral_cities > 1)) return;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == kNone && !city.razed && city.attacked) {
            if (city.countdown != 0) {
                city.countdown--;
                if (city.countdown == 0) ProduceGarrison(game, city, who);
            }
            const Slot* slot =
                    city.producing != kNone && city.producing < static_cast<int>(city.slots.size())
                            ? &city.slots[city.producing]
                            : nullptr;
            bool building = slot && city.countdown != 0 && city.countdown != slot->time;
            if (!building && std::min(static_cast<int>(ArmiesAt(game, city.x, city.y).size()),
                                      rules::kMaxStack) < 4) {
                int best = rules::BestSlot(city.slots, 2, game.types, who && who->enhanced);
                if (best != kNone) SetCityProduction(game, who, city, best);
            }
        }
    }
}

Ending CheckEnd(Game& game) {
    // a human side out of the game is the computer's from now on, so that its
    // fall is told only once
    std::vector<Side*> humans, computers;
    bool fallen_human = false;
    for (Side* side : game.sides) {
        if (side->alive) {
            (side->computer ? computers : humans).push_back(side);
        } else if (!side->computer) {
            side->computer = true;
            fallen_human = true;
        }
    }
    int standing = 0;
    for (auto& city : game.map->cities)
        if (!city.razed) standing++;

    Ending ending;
    if (humans.empty() && computers.empty()) {
        game.over = true;
        ending.over = true;
        ending.message = "Alas! No more players are left!";
        return ending;
    }
    // The last human has fallen and the computers fight on; a game that never
    // had a human said so as it began (group 14) instead.
    if (!game.won && humans.empty() && fallen_human && !game.no_humans_said) {
        game.no_humans_said = true;
        ending.no_humans = true;
    }
    // a lone human with more than half the cities has won -- once; the game
    // goes on, for the kingdom to be looked over
    if (!game.won && humans.size() == 1 && computers.empty()) {
        int mine = static_cast<int>(SideCities(game, *humans[0]).size());
        if (mine * 2 > standing) {
            game.won = true;
            history::RecordDeed(game, humans[0], DeedType::kSideVictorious, humans[0]->index, 0,
                                "");
            ending.won = true;
            ending.winner = humans[0];
            ending.message = humans[0]->name + " rules the world!";
        }
    }
    // the last computer side has triumphed: it is handed to the player, its
    // turn opens as a human's, and the world can be looked over
    if (!game.won && humans.empty() && computers.size() == 1) {
        Side* winner = computers[0];
        game.won = true;
        winner->computer = false;
        history::RecordDeed(game, winner, DeedType::kSideVictorious, winner->index, 0, "");
        ending.triumph = true;
        ending.winner = winner;
        ending.message = winner->name + " has triumphed!";
    }
    // offered once, while computers still play
    if (!game.won && humans.size() == 1 && !computers.empty() && !game.surrender_offered) {
        int mine = static_cast<int>(SideCities(game, *humans[0]).size());
        int biggest = 0;
        for (Side* side : computers)
            biggest = std::max(biggest, static_cast<int>(SideCities(game, *side).size()));
        // Matches the reference: every city counts here, ruins too.
        int all = static_cast<int>(game.map->cities.size());
        if (mine > all / 2 && mine > biggest + all / 8) {
            game.surrender_offered = true;
            ending.surrender = true;
            ending.message = "Your enemies offer their surrender!";
        }
    }
    return ending;
}

std::shared_ptr<SearchResult> SearchHere(Game& game, const std::vector<Army*>& stack, bool human) {
    if (stack.empty()) return nullptr;
    return site::Search(game, stack, stack[0]->x, stack[0]->y, human);
}

std::string DescribeSearch(const SearchResult* result) {
    if (!result) return "";
    const std::string& name = result->site->name;
    if (result->kind == "temple") {
        return result->blessed > 0
                       ? name + " blesses " + std::to_string(result->blessed) + " of your armies."
                       : name + " has blessed them already.";
    }
    if (result->kind == "no hero") return name + " can only be searched by a hero.";
    if (result->kind == "killed")
        return "Your hero is slain in " + name + " by a " +
               (result->monster ? result->monster->name : "guardian") + "!";
    if (result->kind == "gold") return name + " yields " + std::to_string(result->gold) + " gold!";
    if (result->kind == "item")
        return "Your hero finds the " +
               (result->item ? result->item->name : std::string("treasure")) + " in " + name + "!";
    if (result->kind == "allies")
        return std::to_string(result->armies.size()) + " " + result->type->name + " join you at " +
               name + "!";
    if (result->kind == "sage") return "A sage dwells in " + name + ".";
    return name + " holds nothing.";
}

void SetProduction(Game& game, City& city, int slot_index) {
    city.producing = slot_index;
    city.countdown = slot_index != kNone ? city.slots[slot_index].time : 0;
    if (slot_index == kNone) city.vector_to = kNone;  // vectoring only sticks while building
}

std::vector<City*> VectoredTo(Game& game, const City* dest, const Side* side) {
    std::vector<City*> out;
    for (auto& city : game.map->cities) {
        if (!dest) {
            if (city.vector_to == kStandard && side && city.owner_index == side->index)
                out.push_back(&city);
        } else if (city.vector_to == dest->index && &city != dest) {
            out.push_back(&city);
        }
    }
    return out;
}

std::pair<int, int> StandardAt(const Game& game, const Side& side) {
    if (side.index < static_cast<int>(game.map->items.size())) {
        const Item& item = game.map->items[side.index];
        if (item.status == 1 && item.planted && item.x != kNone) return {item.x, item.y};
    }
    return {kNone, kNone};
}

bool PlantFlag(Game& game, Side& side, Army* hero) {
    int terrain = scn::TerrainAt(*game.map, hero->x, hero->y);
    if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
        terrain == move::kSite)
        return false;
    for (auto& item : game.map->items)
        if (item.planted && item.x == hero->x && item.y == hero->y) return false;
    if (side.index >= static_cast<int>(game.map->items.size())) return false;
    Item* standard = &game.map->items[side.index];
    auto item_it = std::find(hero->items.begin(), hero->items.end(), standard);
    if (item_it != hero->items.end()) {
        hero->items.erase(item_it);
        standard->status = 1;
        standard->x = hero->x;
        standard->y = hero->y;
        standard->planted = true;
        return true;
    }
    return false;
}

bool VectorToStandard(Game& game, City& city, const Side& side) {
    if (StandardAt(game, side).first == kNone) return false;
    if (city.vector_to != kStandard &&
        static_cast<int>(VectoredTo(game, nullptr, &side).size()) >= kMaxVectoredTo)
        return false;
    city.vector_to = kStandard;
    return true;
}

bool Vector(Game& game, City& city, City* dest_city) {
    if (dest_city == &city) dest_city = nullptr;
    if (dest_city && city.vector_to != dest_city->index &&
        static_cast<int>(VectoredTo(game, dest_city).size()) >= kMaxVectoredTo) {
        return false;
    }
    city.vector_to = dest_city ? dest_city->index : kNone;
    return true;
}

City* NearestCity(Game& game, int x, int y, const Side* side, const Side* seer) {
    City* best = nullptr;
    int best_d = kNone;
    for (auto& city : game.map->cities) {
        bool allowed = !side || city.owner_index == side->index;
        if (allowed && seer) {
            int seer_idx = seer->index;
            allowed = Seen(game, seer_idx, city.x, city.y) ||
                      Seen(game, seer_idx, city.x + 1, city.y) ||
                      Seen(game, seer_idx, city.x, city.y + 1) ||
                      Seen(game, seer_idx, city.x + 1, city.y + 1);
        }
        if (allowed) {
            int dist = move::Distance(x, y, city.x, city.y);
            if (best_d == kNone || dist < best_d) {
                best = &city;
                best_d = dist;
            }
        }
    }
    return best;
}

std::vector<const ArmyType*> BuyableTypes(const Game& game) {
    std::vector<const ArmyType*> out;
    for (auto* army_type : game.types.List())
        if (army_type->price >= 0) out.push_back(army_type);
    return out;
}

std::string CannotBuy(const Game& game, const Side& side, const City& city,
                      const ArmyType& army_type) {
    for (auto& slot : city.slots)
        if (slot.type == army_type.id) return "has it";
    if (side.gold < army_type.price) return "too dear";
    return "";
}

int BuySlot(const City& city) {
    if (static_cast<int>(city.slots.size()) < rules::kProductionSlots)
        return static_cast<int>(city.slots.size());
    return 0;
}

void BuyProduction(Game& game, Side& side, City& city, int slot_index, int type_id) {
    const ArmyType* army_type = game.types.ById(type_id);
    bool building = city.producing != kNone;
    int was = city.producing;
    Slot slot{army_type->id,   army_type->name, army_type->strength,       army_type->time,
              army_type->cost, army_type->move, std::abs(army_type->price)};
    if (slot_index >= static_cast<int>(city.slots.size())) city.slots.resize(slot_index + 1);
    city.slots[slot_index] = slot;
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    side.gold -= army_type->price;
    // the slot being built was bought over
    if (building && was == slot_index) {
        city.producing = kNone;
        city.countdown = 0;
        city.vector_to = kNone;
    }
}

void SortProduction(City& city) {
    int building = city.producing;
    auto& slots = city.slots;
    for (int i = 1; i < static_cast<int>(slots.size()); i++) {
        int pos = i;
        while (pos > 0 && slots[pos].price < slots[pos - 1].price) {
            std::swap(slots[pos], slots[pos - 1]);
            if (building == pos)
                building = pos - 1;
            else if (building == pos - 1)
                building = pos;
            pos--;
        }
    }
    if (city.producing != kNone) city.producing = building;
}

void RenameCity(Game& game, City& city, const std::string& name) {
    city.name = name;
}

}  // namespace game
}  // namespace w2
