#include "warlords/ai/belief.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "warlords/ai/classic_ai_player.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::belief {

SideBelief& Ensure(Side& side) {
    if (ModernMlPlayer* modern = ModernMlPlayer::FindMutableOn(side)) {
        return modern->MutableBelief();
    }
    if (ClassicAiPlayer* classic = ClassicAiPlayer::FindMutableOn(side)) {
        return classic->EnsureInspectionBelief();
    }
    return ModernMlPlayer::EnsureOn(side).MutableBelief();
}

bool IsTileVisible(const Game& game, int side_index, int x, int y) {
    return game::Seen(game, side_index, x, y);
}

bool IsCityVisible(const Game& game, int side_index, const City& city) {
    if (city.IsRazed()) return false;
    const Map& map = game.GetMap();
    if (map.GameOptions().hidden_map == 0) return true;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            if (game::Seen(game, side_index, city.X() + dx, city.Y() + dy)) return true;
        }
    }
    const Side* viewer = map.SideAt(side_index);
    if (viewer && viewer->ActiveQuest() && viewer->ActiveQuest()->TargetCityId() == city.Id())
        return true;
    return false;
}

bool IsSiteVisible(const Game& game, int side_index, const Site& site) {
    if (game::Seen(game, side_index, site.x, site.y)) return true;
    const Side* viewer = game.GetMap().SideAt(side_index);
    return viewer && site::ShownTo(site, *viewer);
}

std::vector<Army*> ArmiesAt(const Game& game, int x, int y) {
    return game::ArmiesAt(game, x, y);
}

void RecordBattleObservation(Game& game, int observer_side, int x, int y) {
    if (observer_side < 0 || observer_side >= 8) return;
    Map& map = game.GetMap();
    Side* side = map.SideAt(observer_side);
    if (!side) return;
    SideBelief& belief = Ensure(*side);

    std::vector<std::pair<int, int>> tiles;
    if (City* city = game::CityAt(game, x, y)) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) tiles.emplace_back(city->X() + dx, city->Y() + dy);
    } else {
        tiles.emplace_back(x, y);
    }

    for (auto [tile_x, tile_y] : tiles) {
        int key = tile_y * map.Width() + tile_x;
        auto armies = game::ArmiesAt(game, tile_x, tile_y);
        if (armies.empty() || armies[0]->OwnerIndex() == observer_side) {
            belief.stacks_by_tile.erase(key);
            continue;
        }
        ObservedStack observed;
        observed.x = tile_x;
        observed.y = tile_y;
        observed.owner = armies[0]->OwnerIndex();
        observed.visible_count = static_cast<int>(armies.size());
        observed.top_type = armies[0]->TypeIndex();
        observed.last_inspected_turn = game.Turn();
        float strength_sum = 0.0F;
        for (Army* army : armies) {
            observed.known_types.push_back(army->TypeIndex());
            observed.known_strengths.push_back(army->Strength());
            strength_sum += static_cast<float>(army->Strength());
        }
        if (observed.owner >= 0 && observed.owner < 8 && !armies.empty()) {
            float average = strength_sum / static_cast<float>(armies.size());
            belief.seen_enemy_avg_strength[observed.owner] =
                    0.7F * belief.seen_enemy_avg_strength[observed.owner] + 0.3F * average;
        }
        belief.stacks_by_tile[key] = std::move(observed);
    }
}

void Update(Game& game, Side& side) {
    SideBelief& belief = Ensure(side);
    Map& map = game.GetMap();
    belief.updated_turn = game.Turn();
    int side_index = side.Index();
    int width = map.Width(), height = map.Height();

    belief.visible_cities.clear();
    belief.visible_sites.clear();
    belief.visible_ground_items.clear();
    belief.fog_frontiers.clear();
    belief.visible_cities_by_side.fill(0);
    belief.my_border_strength.fill(0.0F);
    belief.their_border_strength.fill(0.0F);

    // Clean stale tile observations and scan visible enemy stacks
    std::map<int, ObservedStack> refreshed;
    int unseen_total = 0;
    std::vector<FrontierCluster> raw_frontiers;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (!IsTileVisible(game, side_index, x, y)) {
                unseen_total++;
                continue;
            }
            int key = y * width + x;
            auto armies = ArmiesAt(game, x, y);
            if (!armies.empty() && armies[0]->OwnerIndex() != side_index &&
                armies[0]->OwnerIndex() != kNone) {
                int owner = armies[0]->OwnerIndex();
                bool tower = game::TowerAt(game, x, y);
                bool full_view = map.GameOptions().view_enemies != 0;
                ObservedStack observed;
                observed.x = x;
                observed.y = y;
                observed.owner = owner;
                observed.top_type = armies[0]->TypeIndex();
                observed.visible_count =
                        (tower && !full_view) ? 4 : static_cast<int>(armies.size());

                const ArmyType* top_army_type = game.ArmyTypes().ById(observed.top_type);
                if (top_army_type && owner >= 0 && owner < 8) {
                    belief.seen_enemy_avg_strength[owner] =
                            0.9F * belief.seen_enemy_avg_strength[owner] +
                            0.1F * static_cast<float>(std::max(2, top_army_type->strength));
                }

                auto previous = belief.stacks_by_tile.find(key);
                if (full_view) {
                    observed.last_inspected_turn = game.Turn();
                    for (Army* army : armies) {
                        observed.known_types.push_back(army->TypeIndex());
                        observed.known_strengths.push_back(army->Strength());
                    }
                } else if (previous != belief.stacks_by_tile.end() &&
                           previous->second.owner == owner &&
                           previous->second.visible_count == static_cast<int>(armies.size()) &&
                           game.Turn() - previous->second.last_inspected_turn <= 2) {
                    observed.last_inspected_turn = previous->second.last_inspected_turn;
                    observed.known_types = previous->second.known_types;
                    observed.known_strengths = previous->second.known_strengths;
                }
                refreshed[key] = std::move(observed);
            }

            // Record visible land and coastal/sea tiles bordering unseen fog
            if (map.GameOptions().hidden_map != 0) {
                int terrain = map.TerrainTypeOf(map.TileWord(x, y));
                if (terrain != move::kMountains) {
                    bool is_water = (terrain == move::kWater || terrain == move::kShore);
                    bool is_road = !is_water && (scn::RoadAt(map, x, y) % 0x20 != 0);
                    int unseen_neighbours = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int neighbour_x = x + dx, neighbour_y = y + dy;
                            if (neighbour_x >= 0 && neighbour_y >= 0 && neighbour_x < width &&
                                neighbour_y < height &&
                                !IsTileVisible(game, side_index, neighbour_x, neighbour_y)) {
                                unseen_neighbours++;
                            }
                        }
                    }
                    if (unseen_neighbours >= 1) {
                        raw_frontiers.push_back(FrontierCluster{
                            x, y, unseen_neighbours, unseen_neighbours, is_water, is_road});
                    }
                }
            }
        }
    }
    belief.stacks_by_tile = std::move(refreshed);
    int active_area = std::max(
            1, (map.ActiveX1() - map.ActiveX0() + 1) * (map.ActiveY1() - map.ActiveY0() + 1));
    belief.frontier_ratio =
            std::min(1.0F, static_cast<float>(unseen_total) / static_cast<float>(active_area));

    // Sort the raw frontiers by proximity to our own cities and armies (preferring road
    // tiles that connect cities) so local expansion frontiers are among the top clusters.
    std::vector<std::pair<int, int>> my_anchors;
    for (const auto& city : map.Cities()) {
        if (!city.IsRazed() && city.OwnerIndex() == side_index)
            my_anchors.emplace_back(city.X(), city.Y());
    }
    for (const Army* army : game.Armies()) {
        if (army->OwnerIndex() == side_index && army->X() != kNone && !army->InTransit())
            my_anchors.emplace_back(army->X(), army->Y());
    }
    std::sort(raw_frontiers.begin(), raw_frontiers.end(),
              [&](const FrontierCluster& lhs, const FrontierCluster& rhs) {
                  int lhs_distance = 999, rhs_distance = 999;
                  for (const auto& [anchor_x, anchor_y] : my_anchors) {
                      lhs_distance = std::min(lhs_distance,
                                              move::Distance(lhs.x, lhs.y, anchor_x, anchor_y));
                      rhs_distance = std::min(rhs_distance,
                                              move::Distance(rhs.x, rhs.y, anchor_x, anchor_y));
                  }
                  int lhs_score = lhs_distance - (lhs.on_road ? 4 : 0);
                  int rhs_score = rhs_distance - (rhs.on_road ? 4 : 0);
                  if (lhs_score != rhs_score) return lhs_score < rhs_score;
                  return lhs.on_road > rhs.on_road;
              });

    // Cluster land frontiers separately from water frontiers so coastal land does not suppress
    // amphibious sea lanes (and vice versa); snap cluster anchor onto road tiles when merged.
    int land_count = 0, water_count = 0;
    for (const auto& frontier : raw_frontiers) {
        bool merged = false;
        for (auto& existing : belief.fog_frontiers) {
            if (existing.is_water == frontier.is_water &&
                move::Distance(frontier.x, frontier.y, existing.x, existing.y) < 3) {
                existing.region_unseen_count += frontier.unseen_neighbours;
                if (frontier.on_road && !existing.on_road) {
                    existing.x = frontier.x;
                    existing.y = frontier.y;
                    existing.on_road = true;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (!frontier.is_water && land_count < 48) {
                belief.fog_frontiers.push_back(frontier);
                land_count++;
            } else if (frontier.is_water && water_count < 24) {
                belief.fog_frontiers.push_back(frontier);
                water_count++;
            }
        }
    }

    for (auto& city : map.Cities()) {
        if (IsCityVisible(game, side_index, city)) {
            belief.visible_cities.push_back(&city);
            if (city.OwnerIndex() >= 0 && city.OwnerIndex() < 8) {
                belief.visible_cities_by_side[city.OwnerIndex()]++;
            }
        }
    }

    for (auto& site : map.Sites()) {
        if (IsSiteVisible(game, side_index, site)) belief.visible_sites.push_back(&site);
    }

    for (auto& item : map.Items()) {
        if (item.status == 1 && item.x != kNone && !item.planted &&
            IsTileVisible(game, side_index, item.x, item.y)) {
            belief.visible_ground_items.push_back(&item);
        }
    }

    // Compute per-city threat and nearest visible rival distance using fair-information state only
    size_t city_count = map.Cities().size();
    belief.city_threat1_turn.assign(city_count, 0.0F);
    belief.city_threat2_turn.assign(city_count, 0.0F);
    belief.city_nearest_enemy_dist.assign(city_count, 200.0F);

    for (City* city : belief.visible_cities) {
        float min_dist = 200.0F;
        for (City* other : belief.visible_cities) {
            if (other->OwnerIndex() >= 0 && other->OwnerIndex() != side_index &&
                !other->IsRazed() && !diplomacy::IsAllied(game, side_index, other->OwnerIndex())) {
                float distance = static_cast<float>(
                        move::Distance(city->X(), city->Y(), other->X(), other->Y()));
                if (distance < min_dist) min_dist = distance;
            }
        }
        for (const auto& [tile_key, observed] : belief.stacks_by_tile) {
            if (observed.owner >= 0 && observed.owner != side_index &&
                !diplomacy::IsAllied(game, side_index, observed.owner)) {
                float distance = static_cast<float>(
                        move::Distance(city->X(), city->Y(), observed.x, observed.y));
                if (distance < min_dist) min_dist = distance;
            }
        }
        belief.city_nearest_enemy_dist[city->Index()] = min_dist;

        float threat1 = 0.0F, threat2 = 0.0F;
        for (const auto& [tile_key, observed] : belief.stacks_by_tile) {
            if (observed.owner == side_index || observed.owner == kNone ||
                diplomacy::IsAllied(game, side_index, observed.owner))
                continue;
            int distance = move::Distance(city->X(), city->Y(), observed.x, observed.y);
            const ArmyType* top_army_type = game.ArmyTypes().ById(observed.top_type);
            int estimated_move = top_army_type ? std::max(10, top_army_type->move) : 12;
            int reach1 = std::max(8, (estimated_move * 3) / 4);
            int reach2 = reach1 * 2;
            float estimated_strength = 0.0F;
            if (!observed.known_strengths.empty()) {
                for (int strength : observed.known_strengths)
                    estimated_strength += static_cast<float>(strength);
            } else {
                float top_strength =
                        top_army_type ? static_cast<float>(top_army_type->strength) : 4.0F;
                float rest_strength = (observed.owner >= 0 && observed.owner < 8)
                                              ? belief.seen_enemy_avg_strength[observed.owner]
                                              : 4.0F;
                estimated_strength =
                        top_strength +
                        static_cast<float>(std::max(0, observed.visible_count - 1)) * rest_strength;
            }
            if (distance <= reach1) threat1 += estimated_strength;
            if (distance <= reach2) threat2 += estimated_strength;

            if (city->OwnerIndex() == side_index && distance <= 18 && observed.owner >= 0 &&
                observed.owner < 8) {
                belief.their_border_strength[observed.owner] += estimated_strength;
            }
        }
        belief.city_threat1_turn[city->Index()] = threat1;
        belief.city_threat2_turn[city->Index()] = threat2;
    }

    // Compute our own border strength near each rival's visible cities
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() != side_index || army->X() == kNone || army->InTransit()) continue;
        for (City* city : belief.visible_cities) {
            if (city->OwnerIndex() >= 0 && city->OwnerIndex() < 8 &&
                city->OwnerIndex() != side_index) {
                if (move::Distance(army->X(), army->Y(), city->X(), city->Y()) <= 18) {
                    belief.my_border_strength[city->OwnerIndex()] +=
                            static_cast<float>(army->Strength());
                }
            }
        }
    }
}

CombatEstimate EstimateCombat(Game& game, const Side& side, const std::vector<Army*>& attackers,
                              int target_x, int target_y, float caution_bias) {
    CombatEstimate estimate;
    if (attackers.empty()) {
        estimate.win_prob = 0.0F;
        return estimate;
    }
    int side_index = side.Index();
    City* city = game::CityAt(game, target_x, target_y);
    bool target_visible = city ? IsCityVisible(game, side_index, *city)
                               : IsTileVisible(game, side_index, target_x, target_y);
    if (!target_visible) {
        estimate.win_prob = 0.85F;
        estimate.expected_survivors = static_cast<float>(attackers.size()) * 0.85F;
        return estimate;
    }

    const SideBelief* existing = side.ComputerPlayer() ? side.ComputerPlayer()->Belief() : nullptr;
    const SideBelief& belief = existing ? *existing : Ensure(const_cast<Side&>(side));

    // Gather visible defender tiles (4 tiles if city, 1 tile otherwise)
    std::vector<std::pair<int, int>> defender_tiles;
    if (city) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                defender_tiles.emplace_back(city->X() + dx, city->Y() + dy);
    } else {
        defender_tiles.emplace_back(target_x, target_y);
    }

    const Map& map = game.GetMap();
    bool full_view = map.GameOptions().view_enemies != 0;
    std::vector<Army> synthetic_pool;
    synthetic_pool.reserve(32);
    std::vector<Army*> defenders;

    int seen_tiles = 0;
    for (auto [tile_x, tile_y] : defender_tiles) {
        if (!IsTileVisible(game, side_index, tile_x, tile_y)) continue;
        seen_tiles++;
        int key = tile_y * map.Width() + tile_x;
        auto actual = ArmiesAt(game, tile_x, tile_y);
        if (actual.empty() || actual[0]->OwnerIndex() == side_index) continue;

        if (full_view) {
            for (Army* army : actual) {
                defenders.push_back(army);
                estimate.estimated_defender_strength += static_cast<float>(army->Strength());
            }
            estimate.estimated_defender_count += static_cast<int>(actual.size());
            continue;
        }

        auto observed = belief.stacks_by_tile.find(key);
        int owner = actual[0]->OwnerIndex();
        int top_type = actual[0]->TypeIndex();
        bool tower = game::TowerAt(game, tile_x, tile_y);
        int count = tower ? 4 : static_cast<int>(actual.size());
        if (observed != belief.stacks_by_tile.end() && !observed->second.known_types.empty()) {
            for (size_t i = 0; i < observed->second.known_types.size(); i++) {
                Army synthetic;
                synthetic.SetOwner(SideId(owner));
                synthetic.PlaceAt(TilePos(tile_x, tile_y));
                synthetic.SetType(ArmyTypeId(observed->second.known_types[i]));
                synthetic.SetStrength(observed->second.known_strengths[i]);
                synthetic_pool.push_back(synthetic);
                defenders.push_back(&synthetic_pool.back());
                estimate.estimated_defender_strength += static_cast<float>(synthetic.Strength());
            }
            estimate.estimated_defender_count +=
                    static_cast<int>(observed->second.known_types.size());
        } else {
            float average_strength =
                    (owner >= 0 && owner < 8) ? belief.seen_enemy_avg_strength[owner] : 3.5F;
            int under_strength =
                    std::clamp(static_cast<int>(std::round(average_strength + caution_bias)), 2, 8);
            for (int i = 0; i < count; i++) {
                Army synthetic;
                synthetic.SetOwner(SideId(owner));
                synthetic.PlaceAt(TilePos(tile_x, tile_y));
                if (i == 0) {
                    synthetic.SetType(ArmyTypeId(top_type));
                    const ArmyType* army_type = game.ArmyTypes().ById(top_type);
                    synthetic.SetStrength(army_type ? army_type->strength : under_strength);
                } else {
                    synthetic.SetType(ArmyTypeId(2));
                    synthetic.SetStrength(under_strength);
                }
                synthetic_pool.push_back(synthetic);
                defenders.push_back(&synthetic_pool.back());
                estimate.estimated_defender_strength += static_cast<float>(synthetic.Strength());
            }
            estimate.estimated_defender_count += count;
        }
    }

    estimate.seen_tile_fraction =
            defender_tiles.empty()
                    ? 1.0F
                    : static_cast<float>(seen_tiles) / static_cast<float>(defender_tiles.size());

    // Calibrated partial-visibility belief when some tiles of a 2x2 city remain in fog
    // and no defenders were observed on the visible tiles:
    int unseen_enemy_tiles = 0;
    std::vector<Army*> unseen_fallback_defenders;
    if (defenders.empty() && city && city->OwnerIndex() != side_index && !city->IsRazed() &&
        seen_tiles < 4) {
        if (city->IsNeutral()) {
            // Neutral garrisons stand only on the city's top-left tile, so a neutral
            // garrison can only be hidden when that tile itself is unseen.
            if (!IsTileVisible(game, side_index, city->X(), city->Y())) {
                Army synthetic;
                synthetic.SetOwner(SideId::None());
                synthetic.PlaceAt(city->Position());
                synthetic.SetType(ArmyTypeId((map.GameOptions().neutral_cities <= 0) ? 0 : 2));
                synthetic.SetStrength((map.GameOptions().neutral_cities <= 0) ? 1 : 4);
                synthetic_pool.push_back(synthetic);
                defenders.push_back(&synthetic_pool.back());
                estimate.estimated_defender_count = 1;
                estimate.estimated_defender_strength = static_cast<float>(synthetic.Strength());
            }
        } else {
            // Enemy city with unseen corner(s): sample unseen garrison occupancy in
            // proportion to (4 - seen tiles) / 4 across the simulated battles rather
            // than assuming a guaranteed 3-stack
            unseen_enemy_tiles = 4 - seen_tiles;
            int owner = city->OwnerIndex();
            float average_strength =
                    (owner >= 0 && owner < 8) ? belief.seen_enemy_avg_strength[owner] : 4.0F;
            int estimated_strength =
                    std::clamp(static_cast<int>(std::round(average_strength + caution_bias)), 2, 7);
            for (int i = 0; i < 2; i++) {
                Army synthetic;
                synthetic.SetOwner(SideId(owner));
                synthetic.PlaceAt(city->Position());
                synthetic.SetType(ArmyTypeId(2));
                synthetic.SetStrength(estimated_strength);
                synthetic_pool.push_back(synthetic);
                unseen_fallback_defenders.push_back(&synthetic_pool.back());
            }
            float unseen_fraction = static_cast<float>(unseen_enemy_tiles) / 4.0F;
            estimate.estimated_defender_count =
                    static_cast<int>(std::round(2.0F * unseen_fraction));
            estimate.estimated_defender_strength =
                    2.0F * static_cast<float>(estimated_strength) * unseen_fraction;
        }
    }

    if (defenders.empty() && unseen_fallback_defenders.empty()) {
        estimate.win_prob = 1.0F;
        estimate.expected_survivors = static_cast<float>(attackers.size());
        return estimate;
    }

    // Save RNG state so combat estimation is 100% side-effect free
    uint32_t saved_rng = game.GetRng().State();
    uint32_t sim_seed =
            saved_rng ^ static_cast<uint32_t>(target_x * 73856093 ^ target_y * 19349663 ^
                                              static_cast<int>(attackers.size()) * 83492791);
    if (sim_seed == 0) sim_seed = 1;
    game.GetRng().SetState(sim_seed);

    constexpr int kSimulations = 8;
    int wins = 0;
    int total_survivors = 0;
    int total_enemy_dead = 0;
    for (int i = 0; i < kSimulations; i++) {
        const std::vector<Army*>& simulated_defenders =
                !defenders.empty()
                        ? defenders
                        : ((i * 4 < unseen_enemy_tiles * kSimulations) ? unseen_fallback_defenders
                                                                       : defenders);
        if (simulated_defenders.empty()) {
            wins++;
            total_survivors += static_cast<int>(attackers.size());
            continue;
        }
        Battle battle = combat::Resolve(game, attackers, simulated_defenders, target_x, target_y);
        if (battle.won) wins++;
        total_survivors += static_cast<int>(battle.attackers.size());
        total_enemy_dead += static_cast<int>(battle.dead_defenders.size());
    }

    game.GetRng().SetState(saved_rng);
    estimate.win_prob = static_cast<float>(wins) / static_cast<float>(kSimulations);
    estimate.expected_survivors =
            static_cast<float>(total_survivors) / static_cast<float>(kSimulations);
    estimate.expected_enemy_losses =
            static_cast<float>(total_enemy_dead) / static_cast<float>(kSimulations);
    return estimate;
}

}  // namespace w2::ai::belief
