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
    Map& map = game.GetMap();
    Side* setup = map.SideAt(kSetupPlayer);
    bool enhanced = setup && setup->IsEnhanced();
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (city.IsRazed()) continue;
        bool owned = !city.IsNeutral();
        auto level = rules::GarrisonLevel(owned, map.GameOptions().neutral_cities, game.GetRng());
        int slot = kNone;
        if (level && !city.Slots().empty()) {
            slot = rules::BestSlot(city.Slots(), rules::kGarrisonPurpose[*level], game.ArmyTypes(),
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
            const ArmyType* scouts_type = game.ArmyTypes().ById(armytype::kScouts);
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
    Map& map = game.GetMap();
    std::map<int, std::pair<int, int>> from;
    int turn = kNone;
    for (int i = 7; i >= 0; i--) {
        Side* side = map.SideAt(i);
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
        for (auto& city : map.Cities()) {
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
        Side* side = map.SideAt(turn);
        pick->SetOwner(SideId(turn));
        if (game.GetRng().Dice(1, 10, -1) < 5) {
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
        if (map.Sides()[i].InUse() && map.Sides()[i].HasCapitalCity()) avail.push_back(i);
    }
    if (opts.active_sides > 0 && static_cast<int>(avail.size()) > opts.active_sides) {
        uint32_t hash = static_cast<uint32_t>(opts.seed) * 2654435761U + 0x9e3779b9U;
        int anchor_side = avail[hash % avail.size()];
        TilePos anchor_pos = map.Sides()[anchor_side].OriginCapitalPos();
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
            TilePos side_pos = map.Sides()[side_idx].OriginCapitalPos();
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
            Side& side = map.Sides()[avail[i]];
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
    for (const auto& side : map.Sides()) {
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
    map.CropActiveArea(left_x, top_y, right_x, bottom_y);

    int plain_tile = 0;
    int water_tile = 0;
    for (int tile = 0; tile < 255; tile++) {
        if (map.TerrainTypeOf(tile) == move::kPlain) {
            plain_tile = tile;
            break;
        }
    }
    for (int tile = 0; tile < 255; tile++) {
        if (map.TerrainTypeOf(tile) == move::kWater) {
            water_tile = tile;
            break;
        }
    }

    Map::CityList kept_cities;
    for (auto& city : map.Cities()) {
        if (city.X() >= left_x && city.X() + 1 <= right_x && city.Y() >= top_y &&
            city.Y() + 1 <= bottom_y) {
            city.SetId(CityId(static_cast<int>(kept_cities.size())));
            city.SetOwner(SideId::None());
            kept_cities.push_back(std::move(city));
        } else {
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    map.SetTileWord(city.X() + dx, city.Y() + dy, plain_tile);
                }
            }
        }
    }
    map.SetCities(std::move(kept_cities));

    for (auto& side : map.Sides()) {
        side.SetCapitalCityId(CityId::None());
        if (!side.InUse()) continue;
        TilePos cap_pos = side.OriginCapitalPos();
        City* cap = map.CityAtTopLeft(cap_pos.x, cap_pos.y);
        if (cap) {
            side.SetCapitalCityId(cap->Id());
            cap->SetOwner(side.Id());
        } else {
            side.SetInUse(false);
        }
    }

    Map::SiteList kept_sites;
    for (auto& map_site : map.Sites()) {
        if (map_site.x >= left_x && map_site.x <= right_x && map_site.y >= top_y &&
            map_site.y <= bottom_y) {
            map_site.index = static_cast<int>(kept_sites.size());
            kept_sites.push_back(std::move(map_site));
        } else {
            map.SetTileWord(map_site.x, map_site.y, plain_tile);
        }
    }
    map.SetSites(std::move(kept_sites));

    Map::SignList kept_signs;
    for (auto& sign : map.Signs()) {
        if (sign.x >= left_x && sign.x <= right_x && sign.y >= top_y && sign.y <= bottom_y) {
            sign.index = static_cast<int>(kept_signs.size());
            kept_signs.push_back(std::move(sign));
        }
    }
    map.SetSigns(std::move(kept_signs));

    int bx0 = std::max(0, left_x - 2);
    int bx1 = std::min(kMapW - 1, right_x + 2);
    int by0 = std::max(0, top_y - 2);
    int by1 = std::min(kMapH - 1, bottom_y + 2);
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                map.SetTileWord(x, y, water_tile);
                map.SetRoadByte(x, y, 0);
                map.SetCrossing(x, y, false);
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
    game.SetRng(Rng(opts.seed));
    game.SetDataDir(data_dir);
    if (absl::Status status =
                armytype::Load(data_dir / "TERRAIN0/ARMYTYPE.DAT", game.MutableArmyTypes());
        !status.ok()) {
        return status;
    }
    auto loaded_map = opts.memory_files ? scn::LoadFromFiles(data_dir / scenario, scenario,
                                                             *opts.memory_files)
                                        : scn::Load(data_dir / scenario, scenario);
    if (!loaded_map.ok()) return loaded_map.status();
    game.SetMap(std::move(*loaded_map));
    game.SetGreatestCampaign(opts.greatest);
    Map& map = game.GetMap();
    for (const auto& [opt_name, opt_val] : opts.options) {
        if (int* field = map.MutableOptions().Field(opt_name)) *field = opt_val;
    }
    for (auto& side : map.Sides()) {
        auto setup_it = opts.sides.find(side.Index());
        if (setup_it != opts.sides.end() && side.InUse()) {
            const SideSetup& setup = setup_it->second;
            if (!setup.name.empty()) side.SetName(setup.name);
            if (setup.off) {
                if (City* cap = side.CapitalCity(map)) cap->SetOwner(SideId::None());
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
        ApplyScaleAndCrop(map, opts);
    }

    for (auto& side : map.Sides()) {
        side.SetAlive(side.InUse());
        if (side.InUse()) game.AddActiveSide(&side);
    }
    int humans = 0;
    for (Side* side : game.Sides()) {
        if (!side->IsComputerControlled()) humans++;
    }
    bool observed = !(map.GameOptions().hidden_map != 0 && humans >= 1);
    for (Side* side : game.Sides()) side->SetObserved(observed);

    if (map.GameOptions().quick_start != 0) DealCities(game);

    for (auto& city : map.Cities()) {
        city.ReplaceSlots(rules::CitySlots(city.Produces(), game.ArmyTypes(), game.GetRng()));
        city.SetDefence(rules::CityDefence(city.SlotCount()));
        city.StopProduction();
    }
    scn::RefreshCityTiles(map);

    for (size_t i = 0; i < map.Items().size(); i++) {
        Item& item = map.Items()[i];
        if (i < 8 && item.type == rules::kItemStandard) item.standard_of = static_cast<int>(i);
    }

    diplomacy::Init(game);
    SetupGarrisons(game);
    site::Setup(game);
    for (Side* side : game.Sides()) {
        RevealStart(game, *side);
        if (map.GameOptions().hidden_map != 0 && opts.crop_margin > 0) {
            int bx0 = std::max(0, map.ActiveX0() - 2);
            int bx1 = std::min(kMapW - 1, map.ActiveX1() + 2);
            int by0 = std::max(0, map.ActiveY0() - 2);
            int by1 = std::min(kMapH - 1, map.ActiveY1() + 2);
            SideId side_id = side->Id();
            if (game.Explored().HasMask(side_id)) {
                for (int y = 0; y < kMapH; y++) {
                    for (int x = 0; x < kMapW; x++) {
                        if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                            game.Explored().MarkExplored(side_id, kMapW, kMapH, x, y);
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

    game.Schedule().Reset();
    game.SetCurrentSideIndex(0);
    game.GetPathfinder().ClearRouteCache();
    return game_ptr;
}

}  // namespace w2::game
