/**
 * @file
 * @brief Implementation of `w2::game::NewGame` scenario initialization, garrison placement,
 * Quick Start city dealing, and map scaling/cropping.
 */
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::game {

namespace {

void SetupGarrisons(Game& game) {
    Side* setup = game.map->Side(kSetupPlayer);
    bool enhanced = setup && setup->IsEnhanced();
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.IsRazed()) continue;
        bool owned = !city.IsNeutral();
        auto level = rules::GarrisonLevel(owned, game.map->options.neutral_cities, game.rng);
        int slot = kNone;
        if (level && !city.Slots().empty()) {
            slot = rules::BestSlot(city.Slots(), rules::kGarrisonPurpose[*level], game.types,
                                   enhanced);
        }
        if (slot != kNone) {
            Army army = rules::ArmyFromSlot(city.SlotAt(static_cast<size_t>(slot)), enhanced);
            army.PlaceAt(city.Position());
            army.SetOwner(city.Owner());
            army.SetMoves(0);
            army.SetHomeCity(city.Id());
            game.Add(army);
        } else {
            const ArmyType* scouts_type = game.types.ById(armytype::kScouts);
            Army army;
            army.PlaceAt(city.Position());
            army.SetOwner(SideId::None());
            army.SetType(ArmyTypeId(armytype::kScouts));
            army.SetName(scouts_type->name);
            army.SetStrength(1);
            army.SetMaxMoves(scouts_type->move);
            army.SetMoves(0);
            army.SetUpkeep(0);
            army.SetHomeCity(city.Id());
            game.Add(army);
        }
    }
}

void DealCities(Game& game) {
    std::map<int, std::pair<int, int>> from;
    int turn = kNone;
    for (int i = 7; i >= 0; i--) {
        Side* side = game.map->Side(i);
        if (side && side->InUse()) {
            from[i] = side->OriginCapitalPos().ToPair();
            turn = i;
        }
    }
    if (turn == kNone) return;
    for (;;) {
        auto [x, y] = from[turn];
        City* pick = nullptr;
        int best = 1000;
        for (auto& city : game.map->cities) {
            if (city.IsNeutral()) {
                double dx = x - city.X();
                double dy = y - city.Y();
                int dist = static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
                if (dist < best) {
                    pick = &city;
                    best = dist;
                }
            }
        }
        if (!pick) return;
        Side* side = game.map->Side(turn);
        pick->SetOwner(SideId(turn));
        if (game.rng.Dice(1, 10, -1) < 5) {
            from[turn] = side->OriginCapitalPos().ToPair();
        } else {
            from[turn] = pick->Position().ToPair();
        }
        do {
            turn = (turn + 1) % 8;
        } while (!from.count(turn));
    }
}

void ApplyScaleAndCrop(Map& map, const NewGameOptions& opts) {
    std::vector<int> avail;
    for (int i = 0; i < 8; i++) {
        if (map.sides[i].InUse() && map.sides[i].HasCapitalCity()) avail.push_back(i);
    }
    if (opts.active_sides > 0 && static_cast<int>(avail.size()) > opts.active_sides) {
        uint32_t hash = static_cast<uint32_t>(opts.seed) * 2654435761U + 0x9e3779b9U;
        int anchor_side = avail[hash % avail.size()];
        TilePos anchor_pos = map.sides[anchor_side].OriginCapitalPos();
        int anchor_x = anchor_pos.x;
        int anchor_y = anchor_pos.y;

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
                    int next_x = cur_x + dx;
                    int next_y = cur_y + dy;
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
            TilePos side_pos = map.sides[side_idx].OriginCapitalPos();
            int side_x = side_pos.x;
            int side_y = side_pos.y;
            int dist = (side_x >= 0 && side_y >= 0 && side_x < kMapW && side_y < kMapH)
                               ? land_dist[side_y * kMapW + side_x]
                               : INT_MAX;
            int dx = side_x - anchor_x;
            int dy = side_y - anchor_y;
            int dist_sq = dx * dx + dy * dy;
            return {1, dist == INT_MAX ? 1 : 0, dist < INT_MAX ? dist : dist_sq, side_idx};
        };
        std::sort(avail.begin(), avail.end(),
                  [&](int left, int right) { return metric(left) < metric(right); });
        for (size_t i = static_cast<size_t>(opts.active_sides); i < avail.size(); i++) {
            Side& side = map.sides[avail[i]];
            if (City* cap = side.CapitalCity(map)) cap->SetOwner(SideId::None());
            side.SetCapitalCityId(CityId::None());
            side.SetInUse(false);
        }
    }

    if (opts.crop_margin <= 0) return;

    int min_x = kMapW;
    int max_x = 0;
    int min_y = kMapH;
    int max_y = 0;
    bool any_side = false;
    for (const auto& side : map.sides) {
        if (!side.InUse()) continue;
        TilePos cap_pos = side.OriginCapitalPos();
        min_x = std::min(min_x, cap_pos.x);
        max_x = std::max(max_x, cap_pos.x + 1);
        min_y = std::min(min_y, cap_pos.y);
        max_y = std::max(max_y, cap_pos.y + 1);
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

    int plain_tile = 0;
    int water_tile = 0;
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
        if (city.X() >= left_x && city.X() + 1 <= right_x && city.Y() >= top_y &&
            city.Y() + 1 <= bottom_y) {
            city.SetId(CityId(static_cast<int>(kept_cities.size())));
            city.SetOwner(SideId::None());
            kept_cities.push_back(std::move(city));
        } else {
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    int tile_x = city.X() + dx;
                    int tile_y = city.Y() + dy;
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
        map.city_at[city.Y() * kMapW + city.X()] = &city;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                map.city_tile[(city.Y() + dy) * kMapW + (city.X() + dx)] = &city;
            }
        }
    }
    for (auto& side : map.sides) {
        side.SetCapitalCityId(CityId::None());
        if (!side.InUse()) continue;
        TilePos cap_pos = side.OriginCapitalPos();
        City* cap = (cap_pos.x >= 0 && cap_pos.y >= 0 && cap_pos.x < kMapW && cap_pos.y < kMapH)
                            ? map.city_at[cap_pos.y * kMapW + cap_pos.x]
                            : nullptr;
        if (cap) {
            side.SetCapitalCityId(cap->Id());
            cap->SetOwner(side.Id());
        } else {
            side.SetInUse(false);
        }
    }

    std::vector<Site> kept_sites;
    for (auto& map_site : map.sites) {
        if (map_site.x >= left_x && map_site.x <= right_x && map_site.y >= top_y &&
            map_site.y <= bottom_y) {
            map_site.index = static_cast<int>(kept_sites.size());
            kept_sites.push_back(std::move(map_site));
        } else if (map_site.x >= 0 && map_site.y >= 0 && map_site.x < kMapW && map_site.y < kMapH) {
            map.tiles[map_site.y * kMapW + map_site.x] = plain_tile;
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

    int bx0 = std::max(0, left_x - 2);
    int bx1 = std::min(kMapW - 1, right_x + 2);
    int by0 = std::max(0, top_y - 2);
    int by1 = std::min(kMapH - 1, bottom_y + 2);
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                int key = y * kMapW + x;
                map.tiles[key] = water_tile;
                map.roads[key] = 0;
                map.crossing[key] = false;
            }
        }
    }
}

}  // namespace

absl::StatusOr<std::unique_ptr<Game>> NewGame(const std::filesystem::path& data_dir,
                                              const std::string& scenario,
                                              const NewGameOptions& opts) {
    auto game_ptr = std::make_unique<Game>();
    Game& game = *game_ptr;
    game.rng = Rng(opts.seed);
    game.data_dir = data_dir;
    if (absl::Status status = armytype::Load(data_dir / "TERRAIN0/ARMYTYPE.DAT", game.types);
        !status.ok()) {
        return status;
    }
    auto loaded_map = opts.memory_files ? scn::LoadFromFiles(data_dir / scenario, scenario,
                                                             *opts.memory_files)
                                        : scn::Load(data_dir / scenario, scenario);
    if (!loaded_map.ok()) return loaded_map.status();
    game.map = std::move(*loaded_map);
    game.greatest = opts.greatest;
    for (const auto& [opt_name, opt_val] : opts.options) {
        if (int* field = game.map->options.Field(opt_name)) *field = opt_val;
    }
    for (auto& side : game.map->sides) {
        auto setup_it = opts.sides.find(side.Index());
        if (setup_it != opts.sides.end() && side.InUse()) {
            const SideSetup& setup = setup_it->second;
            if (!setup.name.empty()) side.SetName(setup.name);
            if (setup.off) {
                if (City* cap = side.CapitalCity(*game.map)) cap->SetOwner(SideId::None());
                side.SetCapitalCityId(CityId::None());
                side.SetInUse(false);
            } else {
                side.SetComputerControlled(setup.computer);
                if (setup.level != kNone) side.SetLevel(setup.level);
                side.SetCard(setup.card);
            }
        }
    }
    if ((opts.active_sides > 0 && opts.active_sides < 8) || opts.crop_margin > 0) {
        ApplyScaleAndCrop(*game.map, opts);
    }

    for (auto& side : game.map->sides) {
        side.SetAlive(side.InUse());
        if (side.InUse()) game.sides.push_back(&side);
    }
    int humans = 0;
    for (Side* side : game.sides) {
        if (!side->IsComputerControlled()) humans++;
    }
    bool observed = !(game.map->options.hidden_map != 0 && humans >= 1);
    for (Side* side : game.sides) side->SetObserved(observed);

    if (game.map->options.quick_start != 0) DealCities(game);

    for (auto& city : game.map->cities) {
        city.ReplaceSlots(rules::CitySlots(city.Produces(), game.types, game.rng));
        city.SetDefence(rules::CityDefence(city.SlotCount()));
        city.StopProduction();
    }
    scn::RefreshCityTiles(*game.map);

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
            int bx0 = std::max(0, game.map->active_x0 - 2);
            int bx1 = std::min(kMapW - 1, game.map->active_x1 + 2);
            int by0 = std::max(0, game.map->active_y0 - 2);
            int by1 = std::min(kMapH - 1, game.map->active_y1 + 2);
            SideId side_id = side->Id();
            if (game.explored.HasMask(side_id)) {
                for (int y = 0; y < kMapH; y++) {
                    for (int x = 0; x < kMapW; x++) {
                        if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                            game.explored.MarkExplored(side_id, kMapW, kMapH, x, y);
                        }
                    }
                }
            }
        }
    }
    if (opts.computer_setup) {
        opts.computer_setup(game);
    } else {
        RunDefaultGameComputerSetup(game);
    }

    game.turn_schedule.Reset();
    game.current = 0;
    game.side = game.sides.empty() ? nullptr : game.sides[0];
    game.GetPathfinder().ClearRouteCache();
    return game_ptr;
}

}  // namespace w2::game
