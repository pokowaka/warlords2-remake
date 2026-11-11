/**
 * @file
 * @brief Modern AI military and exploration phase: target collection, coalition scoring, and
 * dispatch.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_internal.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"

namespace w2::ai::modern {

/**
 * @brief Forms army coalitions at each location, scores mission options with MissionNet, and
 * executes the chosen moves.
 */
void RunMilitaryAndExplorationPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx, IAiTurnEvents* events,
                                    ml::RolloutSampler* sampler, MovedArmySet& moved_armies,
                                    std::vector<MissionSnapshot>* snapshots) {
    int our_side = side.Index();
    Map& map = game.GetMap();
    SideBelief& belief = belief::Ensure(side);
    const bool want_thoughts = ml::RecordThoughts();
    const bool use_doctrine = policy.PersonaActive() && !(sampler && sampler->current_turn);
    const personality::Profile& prof = personality::ForSide(side);
    game::SeenMasks seen = game::BuildSeenMasks(game, our_side);
    float caution = use_doctrine ? (1.4F * prof.traits[personality::kCaution]) : 0.0F;
    std::array<float, 8> vendetta_by_side{};
    if (use_doctrine) {
        if (const ModernMlPlayer* modern_player = ModernMlPlayer::FindOn(side)) {
            for (int rival_idx = 0; rival_idx < 8; ++rival_idx) {
                if (rival_idx != our_side) {
                    vendetta_by_side[rival_idx] = ai::ComputeVendettaScore(
                            modern_player->Grudge(), rival_idx, game.Turn());
                }
            }
        }
    }
    auto is_muster_city = [&](int city_idx) {
        return policy.Version() >= 8 && policy.UseMuster() && city_idx != kNone &&
               (belief.muster_cities[0] == city_idx || belief.muster_cities[1] == city_idx);
    };

    std::vector<CandidateTarget> targets;
    targets.reserve(80);

    for (City* city : belief.visible_cities) {
        if (city->IsRazed()) continue;
        float threat = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->Index()]
                               : 0.0F;
        bool is_quest_or_own =
                (side.CapitalCity(map) == city) ||
                (side.ActiveQuest() && side.ActiveQuest()->TargetCityId() == city->Id());
        bool is_enemy_cap = false;
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            if (side_idx != our_side && map.Sides()[side_idx].CapitalCity(map) == city)
                is_enemy_cap = true;
        }

        if (city->OwnerIndex() == our_side) {
            float existing_garrison_str = 0.0F;
            float stationary_garrison_str = 0.0F;
            int existing_count = 0;
            bool city_has_hero = false;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    for (Army* army :
                         game::ArmiesOnTileSpan(game, city->X() + dx, city->Y() + dy)) {
                        if (army->OwnerIndex() == our_side) {
                            existing_garrison_str += static_cast<float>(army->Strength());
                            existing_count++;
                            if (army->Hero()) city_has_hero = true;
                            if (army->Moves() <= 0 || moved_armies.contains(army->Id())) {
                                stationary_garrison_str += static_cast<float>(army->Strength());
                            }
                        }
                    }
                }
            }
            const bool hub = is_muster_city(city->Index());
            int hub_flow = 0;
            if (hub) {
                hub_flow = static_cast<int>(game::VectoredTo(game, city).size());
                for (Army* army : game.Armies()) {
                    if (army->OwnerIndex() != our_side || army->IsDisbanded()) continue;
                    if (army->InTransit() &&
                        std::holds_alternative<CityId>(army->TransitState()->dest) &&
                        std::get<CityId>(army->TransitState()->dest) == city->Id()) {
                        hub_flow++;
                    } else if (army->X() != kNone &&
                               game::CityAt(game, army->X(), army->Y()) != city &&
                               army->AiOrder() >= 100 && army->AiDest() == city->Index()) {
                        hub_flow++;
                    }
                }
            }
            CandidateTarget hold;
            hold.kind = TargetKind::kHoldGarrison;
            hold.x = city->X();
            hold.y = city->Y();
            hold.city = city;
            hold.owner = our_side;
            hold.value =
                    static_cast<float>(city->Income()) + static_cast<float>(city->Defence()) * 3.0F;
            hold.is_quest_or_own_cap = is_quest_or_own;
            hold.is_muster_hub = hub;
            hold.hub_armies = existing_count;
            hold.hub_incoming_flow = hub_flow;
            hold.hub_has_hero = city_has_hero;
            hold.enemy_threat = threat;
            hold.assigned_strength = stationary_garrison_str;
            if (want_thoughts) hold.label = "Hold " + city->Name();
            targets.push_back(std::move(hold));

            if (hub && existing_count < 16) {
                CandidateTarget mst;
                mst.kind = TargetKind::kReinforceCity;
                mst.is_muster_hub = true;
                mst.hub_armies = existing_count;
                mst.hub_incoming_flow = hub_flow;
                mst.hub_has_hero = city_has_hero;
                mst.x = city->X();
                mst.y = city->Y();
                mst.city = city;
                mst.owner = our_side;
                mst.value = static_cast<float>(city->Income()) +
                            static_cast<float>(city->Defence()) * 3.0F;
                mst.is_quest_or_own_cap = is_quest_or_own;
                mst.enemy_threat = threat;
                mst.assigned_strength = existing_garrison_str;
                if (want_thoughts) mst.label = "Muster " + city->Name();
                targets.push_back(std::move(mst));
            } else if (!hub && existing_count < 32) {
                CandidateTarget reinf;
                reinf.kind = TargetKind::kReinforceCity;
                reinf.x = city->X();
                reinf.y = city->Y();
                reinf.city = city;
                reinf.owner = our_side;
                reinf.value = static_cast<float>(city->Income()) +
                              static_cast<float>(city->Defence()) * 3.0F;
                reinf.is_quest_or_own_cap = is_quest_or_own;
                reinf.enemy_threat = threat;
                reinf.assigned_strength = existing_garrison_str;
                if (want_thoughts) reinf.label = "Reinforce " + city->Name();
                targets.push_back(std::move(reinf));
            }
        } else if (city->IsNeutral()) {
            CandidateTarget neut;
            neut.kind = TargetKind::kNeutralCity;
            neut.x = city->X();
            neut.y = city->Y();
            neut.city = city;
            neut.owner = kNone;
            neut.value = static_cast<float>(city->Income()) +
                         static_cast<float>(city->Slots().size()) * 5.0F;
            neut.is_quest_or_own_cap = is_quest_or_own;
            neut.is_enemy_capital = is_enemy_cap;
            neut.enemy_threat = threat;
            if (want_thoughts) neut.label = "Capture Neutral " + city->Name();
            targets.push_back(std::move(neut));
        } else if (Attackable(game, our_side, city->OwnerIndex(), city)) {
            CandidateTarget enem;
            enem.kind = TargetKind::kEnemyCity;
            enem.x = city->X();
            enem.y = city->Y();
            enem.city = city;
            enem.owner = city->OwnerIndex();
            enem.value =
                    static_cast<float>(city->Income()) + static_cast<float>(city->Defence()) * 6.0F;
            if (diplomacy::AlliedTargetCity(game, city->OwnerIndex()) == city->Index())
                enem.value += 10.0F;
            enem.is_quest_or_own_cap = is_quest_or_own;
            enem.is_enemy_capital = is_enemy_cap;
            enem.enemy_threat = threat;
            if (want_thoughts) enem.label = "Siege " + city->Name();
            targets.push_back(std::move(enem));
        }
    }

    for (const auto& [tile_key, obs] : belief.stacks_by_tile) {
        if (obs.owner == our_side || obs.owner == kNone) continue;
        if (game::CityAt(game, obs.x, obs.y)) continue;
        if (!Attackable(game, our_side, obs.owner, nullptr)) continue;
        CandidateTarget stack_target;
        stack_target.kind = TargetKind::kEnemyStack;
        stack_target.x = obs.x;
        stack_target.y = obs.y;
        stack_target.owner = obs.owner;
        stack_target.value = static_cast<float>(obs.visible_count) * 4.0F;
        stack_target.is_quest_or_own_cap =
                (side.ActiveQuest() && side.ActiveQuest()->TargetSideId() == SideId(obs.owner));
        if (want_thoughts)
            stack_target.label = absl::StrFormat("Intercept stack (%d,%d)", obs.x, obs.y);
        targets.push_back(std::move(stack_target));
    }

    for (Site* site : belief.visible_sites) {
        if (!core::SiteOpen(*site)) continue;
        CandidateTarget site_target;
        site_target.x = site->x;
        site_target.y = site->y;
        site_target.site = site;
        if (site->content == site::kTemple) {
            site_target.kind = TargetKind::kTempleOrItem;
            site_target.value = 18.0F;
            if (want_thoughts) site_target.label = "Bless at " + site->name;
        } else {
            site_target.kind = TargetKind::kRuinOrSage;
            site_target.value = site->rich ? 28.0F : 22.0F;
            if (want_thoughts) site_target.label = "Search " + site->name;
        }
        targets.push_back(std::move(site_target));
    }

    for (Item* item : belief.visible_ground_items) {
        CandidateTarget ground_target;
        ground_target.kind = TargetKind::kTempleOrItem;
        ground_target.x = item->x;
        ground_target.y = item->y;
        ground_target.item = item;
        ground_target.value = 20.0F;
        ground_target.is_quest_or_own_cap =
                (side.ActiveQuest() && side.ActiveQuest()->TargetItemId() == ItemId(item->index));
        if (want_thoughts) ground_target.label = "Pick up " + item->name;
        targets.push_back(std::move(ground_target));
    }

    for (const auto& frontier : belief.fog_frontiers) {
        CandidateTarget fog_target;
        fog_target.kind = TargetKind::kFogFrontier;
        fog_target.x = frontier.x;
        fog_target.y = frontier.y;
        fog_target.is_water = frontier.is_water;
        fog_target.value = static_cast<float>(frontier.region_unseen_count) * 2.0F +
                           (frontier.on_road ? 6.0F : 0.0F);
        if (want_thoughts) {
            fog_target.label = absl::StrFormat(
                    "Explore %s (%d,%d)", frontier.is_water ? "Sea" : "Fog", frontier.x,
                    frontier.y);
        }
        targets.push_back(std::move(fog_target));
    }

    // v7: Rally targets onto friendly field hero stacks or staged siege stacks (< 8 units)
    // so reinforcements can merge into a concentrated 4-8 unit assault force.
    // Disabled when an MoE includes pre-v7 experts so legacy experts see their native target menu.
    bool allow_field_rally = (policy.Version() >= 7);
    for (const auto& exp : policy.Experts()) {
        if (exp.version < 7) allow_field_rally = false;
    }
    if (allow_field_rally) {
        auto [std_x, std_y] = game::StandardAt(game, side);
        if (std_x != kNone && std_y != kNone) {
            absl::Span<Army* const> std_here = game::ArmiesOnTileSpan(game, std_x, std_y);
            if ((std_here.empty() || std_here[0]->OwnerIndex() == our_side) &&
                static_cast<int>(std_here.size()) < rules::kMaxStack) {
                bool std_hero = false;
                float std_str = 0.0F;
                for (Army* army : std_here) {
                    std_str += static_cast<float>(army->Strength());
                    if (army->Hero()) std_hero = true;
                }
                int std_flow = static_cast<int>(game::VectoredTo(game, nullptr, &side).size());
                for (Army* army : game.Armies()) {
                    if (army->OwnerIndex() == our_side && army->InTransit() &&
                        std::holds_alternative<PlantedStandard>(army->TransitState()->dest)) {
                        std_flow++;
                    }
                }
                CandidateTarget std_tgt;
                std_tgt.kind = TargetKind::kReinforceCity;
                std_tgt.x = std_x;
                std_tgt.y = std_y;
                std_tgt.owner = our_side;
                std_tgt.is_muster_hub =
                        (policy.Version() >= 8 && (policy.UseMuster() || policy.UseStandard()));
                std_tgt.hub_armies = static_cast<int>(std_here.size());
                std_tgt.hub_incoming_flow = std_flow;
                std_tgt.hub_has_hero = std_hero;
                std_tgt.value = 24.0F;
                std_tgt.is_quest_or_own_cap = std_hero;
                std_tgt.assigned_strength = std_str;
                if (want_thoughts)
                    std_tgt.label = absl::StrFormat("Rally standard (%d,%d)", std_x, std_y);
                targets.push_back(std::move(std_tgt));
            }
        }
        std::map<int, std::vector<Army*>> field_tiles;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() != our_side || army->IsDisbanded() || army->InTransit() ||
                army->X() == kNone)
                continue;
            if (game::CityAt(game, army->X(), army->Y())) continue;
            if (army->X() == std_x && army->Y() == std_y) continue;
            field_tiles[army->Y() * map.Width() + army->X()].push_back(army);
        }
        int rally_added = 0;
        for (const auto& [t_key, units] : field_tiles) {
            if (static_cast<int>(units.size()) >= rules::kMaxStack || rally_added >= 8) continue;
            bool has_hero = false, is_staged = false;
            float str = 0.0F;
            for (Army* army : units) {
                str += static_cast<float>(army->Strength());
                if (army->Hero()) has_hero = true;
                if (army->AiOrder() >= 120) is_staged = true;
            }
            if (!has_hero && !is_staged) continue;
            CandidateTarget ral;
            ral.kind = TargetKind::kReinforceCity;
            ral.x = units[0]->X();
            ral.y = units[0]->Y();
            ral.owner = our_side;
            ral.value = has_hero ? 26.0F : 18.0F;
            ral.is_quest_or_own_cap = has_hero;
            ral.assigned_strength = str;
            if (want_thoughts) ral.label = absl::StrFormat("Rally stack (%d,%d)", ral.x, ral.y);
            targets.push_back(std::move(ral));
            rally_added++;
        }
    }

    if (targets.empty()) return;

    std::map<int, float> order_strength_by_dest;
    if (policy.Version() >= 7) {
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() == our_side && !army->IsDisbanded() && army->X() != kNone &&
                !army->InTransit() && army->AiOrder() >= 100 && army->AiDest() != kNone) {
                order_strength_by_dest[army->AiDest()] += static_cast<float>(army->Strength());
            }
        }
    }

    for (auto& target : targets) {
        if (policy.Version() >= 7) {
            auto found = order_strength_by_dest.find(TargetOrderKey(game, target));
            if (found != order_strength_by_dest.end()) target.en_route_strength = found->second;
        }
        if (target.kind == TargetKind::kNeutralCity || target.kind == TargetKind::kEnemyCity ||
            target.kind == TargetKind::kEnemyStack) {
            float near_str = 0.0F;
            for (Army* army : game.Armies()) {
                if (army->OwnerIndex() == our_side && army->X() != kNone && !army->InTransit() &&
                    army->Moves() > 0 &&
                    move::Distance(army->X(), army->Y(), target.x, target.y) <= 14) {
                    near_str += static_cast<float>(army->Strength());
                }
            }
            target.nearby_friendly_str = near_str;
        }
    }

    /** @brief Armies sharing a starting city or field tile. */
    struct LocGroup {
        int x = 0, y = 0;          ///< Representative tile coordinates.
        City* city = nullptr;      ///< City at the location, or nullptr in the field.
        std::vector<Army*> units;  ///< Unmoved armies standing at this location.
    };
    std::map<int, LocGroup> groups_by_loc;
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() != our_side || army->X() == kNone || army->InTransit() ||
            army->Moves() <= 0 || moved_armies.contains(army->Id()))
            continue;
        City* city = game::CityAt(game, army->X(), army->Y());
        int key = city ? -(city->Index() + 1) : (army->Y() * map.Width() + army->X());
        auto& loc_group = groups_by_loc[key];
        loc_group.x = army->X();
        loc_group.y = army->Y();
        loc_group.city = city;
        loc_group.units.push_back(army);
    }

    int best_enemy_city_target = kNone;
    float best_enemy_city_score = -1e9F;

    std::vector<LocGroup*> ordered_groups;
    ordered_groups.reserve(groups_by_loc.size());
    for (auto& [loc_key, loc_group] : groups_by_loc) ordered_groups.push_back(&loc_group);
    if (policy.Version() >= 8) {
        // Dispatch non-hub feeder cities and field stacks BEFORE Muster Hubs so newly
        // arriving reinforcements can join the Muster Hub's strike force this same turn!
        std::stable_sort(ordered_groups.begin(), ordered_groups.end(),
                         [&](const LocGroup* lhs, const LocGroup* rhs) {
                             bool lhs_hub = lhs->city && is_muster_city(lhs->city->Index());
                             bool rhs_hub = rhs->city && is_muster_city(rhs->city->Index());
                             return static_cast<int>(lhs_hub) < static_cast<int>(rhs_hub);
                         });
    }

    for (LocGroup* group_ptr : ordered_groups) {
        LocGroup& loc_group = *group_ptr;
        if (IAiTurnEvents* active_events = game.ActiveAiEvents();
            active_events && active_events->IsAborted()) {
            return;
        }
        if (policy.Version() >= 8 && loc_group.city && is_muster_city(loc_group.city->Index())) {
            loc_group.units.clear();
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    for (Army* army : game::ArmiesOnTileSpan(game, loc_group.city->X() + dx,
                                                             loc_group.city->Y() + dy)) {
                        if (army->OwnerIndex() == our_side && army->Moves() > 0 &&
                            !moved_armies.contains(army->Id())) {
                            loc_group.units.push_back(army);
                        }
                    }
                }
            }
            if (loc_group.units.empty()) continue;
        }
        std::vector<Army*> pool = loc_group.units;
        std::sort(pool.begin(), pool.end(), [&](Army* lhs, Army* rhs) {
            if (lhs->Hero() != rhs->Hero()) return lhs->Hero() > rhs->Hero();
            bool lhs_flies = core::Flies(game, lhs), rhs_flies = core::Flies(game, rhs);
            if (lhs_flies != rhs_flies) return lhs_flies > rhs_flies;
            int lhs_score = lhs->Strength() * 2 + lhs->MaxMoves();
            int rhs_score = rhs->Strength() * 2 + rhs->MaxMoves();
            if (lhs_score != rhs_score) return lhs_score > rhs_score;
            return lhs->Moves() > rhs->Moves();
        });

        core::Flood land_flood = core::FloodFrom(game, our_side, loc_group.x, loc_group.y, 72,
                                                 move::kLand, our_side);
        // The flying flood only matters to a coalition that can fly; skipping it
        // for flightless groups also keeps land stacks off fog only fliers can reach
        bool any_flier = false;
        for (Army* army : pool) any_flier = any_flier || core::Flies(game, army);
        core::Flood fly_flood = any_flier
                                        ? core::FloodFrom(game, our_side, loc_group.x, loc_group.y,
                                                          72, move::kFlying, our_side)
                                        : core::Flood{};
        auto fly_dist = [&](int x, int y) {
            return any_flier ? core::FloodAt(fly_flood, x, y) : core::kUnreached;
        };

        // Ensure local reachable fog frontiers (land or coastal sea) are included near this stack
        if (map.GameOptions().hidden_map != 0) {
            int reachable_fog_count = 0;
            for (const auto& target : targets) {
                if (target.kind == TargetKind::kFogFrontier &&
                    core::FloodAt(land_flood, target.x, target.y) < core::kUnreached) {
                    reachable_fog_count++;
                }
            }
            if (reachable_fog_count < 4) {
                int width = map.Width(), height = map.Height();
                for (int radius = 1; radius <= 28 && reachable_fog_count < 6; radius++) {
                    for (int dy = -radius; dy <= radius && reachable_fog_count < 6; dy++) {
                        for (int dx = -radius; dx <= radius && reachable_fog_count < 6; dx++) {
                            if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                            int fog_x = loc_group.x + dx, fog_y = loc_group.y + dy;
                            if (fog_x < 0 || fog_y < 0 || fog_x >= width || fog_y >= height)
                                continue;
                            int fterr = map.TerrainTypeOf(map.TileWord(fog_x, fog_y));
                            if (fterr == move::kMountains) continue;
                            if (core::FloodAt(land_flood, fog_x, fog_y) >= core::kUnreached)
                                continue;
                            int adj_u = 0;
                            for (int off_y = -1; off_y <= 1; off_y++) {
                                for (int off_x = -1; off_x <= 1; off_x++) {
                                    int unseen_x = fog_x + off_x, unseen_y = fog_y + off_y;
                                    if (unseen_x >= 0 && unseen_y >= 0 && unseen_x < width &&
                                        unseen_y < height &&
                                        !game::IsSeenAt(seen, unseen_x, unseen_y)) {
                                        adj_u++;
                                    }
                                }
                            }
                            if (adj_u >= 1) {
                                bool is_w = (fterr == move::kWater || fterr == move::kShore);
                                bool dup = false;
                                for (const auto& target : targets) {
                                    if (target.kind == TargetKind::kFogFrontier &&
                                        target.is_water == is_w &&
                                        move::Distance(fog_x, fog_y, target.x, target.y) < 3) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (!dup) {
                                    CandidateTarget fog_target;
                                    fog_target.kind = TargetKind::kFogFrontier;
                                    fog_target.x = fog_x;
                                    fog_target.y = fog_y;
                                    fog_target.is_water = is_w;
                                    bool road =
                                            !is_w && (scn::RoadAt(map, fog_x, fog_y) % 0x20 != 0);
                                    fog_target.value =
                                            static_cast<float>(adj_u) * 2.0F + (road ? 6.0F : 0.0F);
                                    if (want_thoughts) {
                                        fog_target.label =
                                                absl::StrFormat("Explore %s (%d,%d)",
                                                                is_w ? "Sea" : "Fog", fog_x, fog_y);
                                    }
                                    targets.push_back(std::move(fog_target));
                                    reachable_fog_count++;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Select up to 6 closest reachable land fog frontiers + 4 closest sea frontiers for this
        // stack
        std::vector<uint8_t> local_fog_mask;
        bool has_local_fogs = false;
        if (map.GameOptions().hidden_map != 0) {
            std::vector<std::pair<int, size_t>> land_fogs, sea_fogs;
            for (size_t tgt_idx = 0; tgt_idx < targets.size(); tgt_idx++) {
                if (targets[tgt_idx].kind != TargetKind::kFogFrontier) continue;
                int d_land = core::FloodAt(land_flood, targets[tgt_idx].x, targets[tgt_idx].y);
                int d_fly = fly_dist(targets[tgt_idx].x, targets[tgt_idx].y);
                int best_d = std::min(d_land, d_fly);
                if (best_d >= core::kUnreached) continue;
                if (targets[tgt_idx].is_water)
                    sea_fogs.emplace_back(best_d, tgt_idx);
                else
                    land_fogs.emplace_back(best_d, tgt_idx);
            }
            std::sort(land_fogs.begin(), land_fogs.end());
            std::sort(sea_fogs.begin(), sea_fogs.end());
            if (!land_fogs.empty() || !sea_fogs.empty()) {
                has_local_fogs = true;
                local_fog_mask.assign(targets.size(), uint8_t{0});
                for (size_t i = 0; i < std::min<size_t>(6, land_fogs.size()); i++) {
                    local_fog_mask[land_fogs[i].second] = uint8_t{1};
                }
                for (size_t i = 0; i < std::min<size_t>(4, sea_fogs.size()); i++) {
                    local_fog_mask[sea_fogs[i].second] = uint8_t{1};
                }
            }
        }

        std::vector<uint8_t> blocked_targets(targets.size(), uint8_t{0});

        while (!pool.empty()) {
            if (IAiTurnEvents* active_events = game.ActiveAiEvents();
                active_events && active_events->IsAborted()) {
                return;
            }
            Army* seed = pool.front();
            pool.erase(pool.begin());
            if (!game.Alive(seed) || seed->Moves() <= 0 || moved_armies.contains(seed->Id()))
                continue;

            std::vector<Army*> eligible_escorts;
            for (auto iter = pool.begin(); iter != pool.end();) {
                Army* cand = *iter;
                if (!game.Alive(cand) || cand->Moves() <= 0 || moved_armies.contains(cand->Id())) {
                    iter = pool.erase(iter);
                    continue;
                }
                if (eligible_escorts.size() + 1 < rules::kMaxStack) {
                    eligible_escorts.push_back(cand);
                }
                ++iter;
            }

            float rem_group_str = static_cast<float>(seed->Strength());
            for (Army* rem : pool) rem_group_str += static_cast<float>(rem->Strength());

            float committed_city_guard_str = 0.0F;
            float origin_threat = 0.0F;
            if (loc_group.city && loc_group.city->OwnerIndex() == our_side) {
                if (loc_group.city->Index() < static_cast<int>(belief.city_threat2_turn.size())) {
                    origin_threat = belief.city_threat2_turn[loc_group.city->Index()];
                }
                for (int dy = 0; dy <= 1; dy++) {
                    for (int dx = 0; dx <= 1; dx++) {
                        for (Army* army : game::ArmiesOnTileSpan(game, loc_group.city->X() + dx,
                                                                 loc_group.city->Y() + dy)) {
                            if (army->OwnerIndex() == our_side &&
                                (army->Moves() <= 0 || moved_armies.contains(army->Id()))) {
                                committed_city_guard_str += static_cast<float>(army->Strength());
                            }
                        }
                    }
                }
            }

            std::vector<size_t> sizes = BuildCoalitionSizes(game, seed, eligible_escorts);

            /** @brief A candidate coalition prefix and its precomputed stack features. */
            struct PrebuiltStack {
                size_t size = 1;           ///< Number of units in the coalition.
                std::vector<Army*> units;  ///< The armies in the coalition.
                ml::StackVec stack_vec{};  ///< Precomputed StackVec features.
                bool flies = false;        ///< Whether the entire coalition flies.
                float str = 0.0F;          ///< Sum of army strengths in the coalition.
            };
            std::vector<PrebuiltStack> pre_stacks;
            pre_stacks.reserve(sizes.size());
            for (size_t coalition_size : sizes) {
                PrebuiltStack prebuilt;
                prebuilt.size = coalition_size;
                prebuilt.units.push_back(seed);
                for (size_t i = 0; i + 1 < coalition_size && i < eligible_escorts.size(); i++) {
                    prebuilt.units.push_back(eligible_escorts[i]);
                }
                for (Army* army : prebuilt.units)
                    prebuilt.str += static_cast<float>(army->Strength());
                float rem_guard_after_leave =
                        (loc_group.city && loc_group.city->OwnerIndex() == our_side)
                                ? (committed_city_guard_str +
                                   std::max(0.0F, rem_group_str - prebuilt.str))
                                : 20.0F;
                prebuilt.stack_vec = BuildStackFeatures(game, prebuilt.units, seed->Moves(),
                                                        rem_guard_after_leave, origin_threat);
                prebuilt.flies = (move::ModeOf(game, prebuilt.units) == move::kFlying);
                pre_stacks.push_back(std::move(prebuilt));
            }

            /** @brief A (target, coalition) pair scored by MissionNet. */
            struct JointOption {
                size_t target_idx = 0;      ///< Index into `targets`.
                size_t stack_opt_idx = 0;   ///< Index into `pre_stacks`.
                ml::MissionInVec in_vec{};  ///< Packed MissionNet input.
                ml::TargetVec tgt_vec{};    ///< Target feature vector.
                ml::MissionOutput out{};    ///< MissionNet output (utility and stage logit).
            };
            std::vector<JointOption> options;
            options.reserve(targets.size() * pre_stacks.size());

            // Scoring options only reads the board; per-tile army queries resolve
            // in O(1) via Game::ArmiesOnTile before the chosen stack moves.
            {
                if (blocked_targets.size() < targets.size()) {
                    blocked_targets.resize(targets.size(), uint8_t{0});
                }
                for (size_t tgt_idx = 0; tgt_idx < targets.size(); tgt_idx++) {
                    if (blocked_targets[tgt_idx] != 0) continue;
                    const CandidateTarget& tgt = targets[tgt_idx];

                    if ((tgt.kind == TargetKind::kNeutralCity ||
                         tgt.kind == TargetKind::kEnemyCity) &&
                        tgt.city && (tgt.city->OwnerIndex() == our_side || tgt.city->IsRazed())) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kEnemyStack &&
                        game::IsSeenAt(seen, tgt.x, tgt.y)) {
                        // Only re-check stacks still in view; one that walked into fog stays a
                        // belief
                        absl::Span<Army* const> cur_at = game::ArmiesOnTileSpan(game, tgt.x, tgt.y);
                        if (cur_at.empty() || cur_at[0]->OwnerIndex() == our_side) continue;
                    }
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.site &&
                        !core::SiteOpen(*tgt.site))
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item &&
                        (tgt.item->status != 1 || tgt.item->x == kNone)) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kFogFrontier && map.GameOptions().hidden_map != 0) {
                        if (tgt.assigned_stacks >= 1) continue;
                        if (has_local_fogs &&
                            (tgt_idx >= local_fog_mask.size() || local_fog_mask[tgt_idx] == 0))
                            continue;
                        bool still_has_unseen = false;
                        for (int off_y = -1; off_y <= 1 && !still_has_unseen; off_y++) {
                            for (int off_x = -1; off_x <= 1 && !still_has_unseen; off_x++) {
                                int unseen_x = tgt.x + off_x, unseen_y = tgt.y + off_y;
                                if (unseen_x >= 0 && unseen_y >= 0 && unseen_x < map.Width() &&
                                    unseen_y < map.Height() &&
                                    !game::IsSeenAt(seen, unseen_x, unseen_y)) {
                                    still_has_unseen = true;
                                }
                            }
                        }
                        if (!still_has_unseen) continue;
                    }

                    if (tgt.kind == TargetKind::kHoldGarrison && tgt.city != loc_group.city)
                        continue;
                    if (tgt.kind == TargetKind::kReinforceCity && tgt.city &&
                        tgt.city == loc_group.city)
                        continue;
                    if (tgt.kind == TargetKind::kReinforceCity && !tgt.city &&
                        tgt.x == loc_group.x && tgt.y == loc_group.y)
                        continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && !seed->Hero()) continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item && !seed->Hero())
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.site &&
                        !seed->Blessings().empty() && !seed->Hero()) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.assigned_stacks >= 1) continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.assigned_stacks >= 1) continue;

                    bool is_combat_tgt = (tgt.kind == TargetKind::kNeutralCity ||
                                          tgt.kind == TargetKind::kEnemyCity ||
                                          tgt.kind == TargetKind::kEnemyStack);
                    for (size_t stk_idx = 0; stk_idx < pre_stacks.size(); stk_idx++) {
                        const PrebuiltStack& prebuilt = pre_stacks[stk_idx];
                        const core::Flood& active_flood = prebuilt.flies ? fly_flood : land_flood;
                        int p_cost = TargetPathCost(game, active_flood, tgt);
                        if (p_cost >= core::kUnreached) continue;

                        float near_str = 0.0F;
                        ml::StackVec opt_sv = prebuilt.stack_vec;
                        if (tgt.kind == TargetKind::kHoldGarrison) {
                            near_str = rem_group_str;
                            opt_sv[14] =
                                    std::tanh((committed_city_guard_str + prebuilt.str) / 10.0F);
                        } else if (is_combat_tgt) {
                            near_str = std::max(0.0F, tgt.nearby_friendly_str - rem_group_str);
                        }

                        if (tgt.kind == TargetKind::kReinforceCity && !tgt.city) {
                            if (static_cast<int>(game::ArmiesOnTileSpan(game, tgt.x, tgt.y).size() +
                                                 prebuilt.units.size()) +
                                        tgt.assigned_stacks * 2 >
                                rules::kMaxStack) {
                                continue;
                            }
                        }
                        if (tgt.is_muster_hub && tgt.kind == TargetKind::kReinforceCity &&
                            tgt.hub_armies + static_cast<int>(prebuilt.units.size()) +
                                            tgt.assigned_stacks * 2 >
                                    12) {
                            continue;
                        }

                        ml::TargetVec target_vec =
                                BuildTargetFeatures(game, side, prebuilt.units, tgt, near_str,
                                                    caution, p_cost, policy.Version());
                        if (tgt.is_muster_hub && tgt.kind == TargetKind::kReinforceCity &&
                            (target_vec[19] > 0.5F || target_vec[8] < 0.33F)) {
                            // Only rally nearby same-landmass (or flying) stacks within ~2 turns;
                            // never embark slow boats across water just to reach a Meeting Point.
                            continue;
                        }
                        ml::MissionInVec m_in = ml::PackMissionInput(ctx, opt_sv, target_vec);
                        auto raw = policy.ForwardMission(m_in);
                        float utility = raw[0];
                        if (use_doctrine) {
                            personality::MissionDoctrineContext doc_ctx{};
                            doc_ctx.is_enemy_target = (tgt.kind == TargetKind::kEnemyCity ||
                                                       tgt.kind == TargetKind::kEnemyStack);
                            if (doc_ctx.is_enemy_target && tgt.owner >= 0 && tgt.owner < 8) {
                                doc_ctx.vendetta_score = vendetta_by_side[tgt.owner];
                            }
                            doc_ctx.is_enemy_capital = tgt.is_enemy_capital;
                            doc_ctx.is_hold_garrison = (tgt.kind == TargetKind::kHoldGarrison);
                            doc_ctx.leaves_city_sparse =
                                    (loc_group.city && loc_group.city->OwnerIndex() == our_side &&
                                     committed_city_guard_str < 5.0F);
                            doc_ctx.win_converged = target_vec[11];
                            utility += personality::ScoreMissionCharacterPrior(prof, doc_ctx);
                        }
                        options.push_back(JointOption{tgt_idx, stk_idx, m_in, target_vec,
                                                      ml::MissionOutput{utility, raw[1]}});
                    }
                }
            }  // scoringIndex

            if (options.empty()) {
                moved_armies.insert(seed->Id());
                continue;
            }

            std::array<int, 8> kind_counts{};
            for (const auto& option : options) {
                kind_counts[static_cast<int>(targets[option.target_idx].kind)]++;
            }
            std::vector<float> logits(options.size());
            std::vector<float> log_mult(options.size());
            for (size_t i = 0; i < options.size(); i++) {
                logits[i] = options[i].out.utility;
                int kind_count = std::max(
                        1, kind_counts[static_cast<int>(targets[options[i].target_idx].kind)]);
                log_mult[i] = std::log(static_cast<float>(kind_count));
            }
            if (snapshots) {
                // Dry run: record this group's first decision and move on, touching nothing
                MissionSnapshot snap;
                snap.candidates.reserve(options.size());
                for (const auto& option : options) {
                    const CandidateTarget& target = targets[option.target_idx];
                    snap.candidates.push_back(option.in_vec);
                    snap.kind.push_back(static_cast<int>(target.kind));
                    snap.city_index.push_back(target.city ? target.city->Index() : kNone);
                    snap.tx.push_back(target.x);
                    snap.ty.push_back(target.y);
                    snap.stack_size.push_back(
                            static_cast<int>(pre_stacks[option.stack_opt_idx].units.size()));
                }
                snap.log_mult = log_mult;
                snap.seed = reinterpret_cast<std::uintptr_t>(seed);
                for (Army* army : loc_group.units)
                    snap.group.push_back(reinterpret_cast<std::uintptr_t>(army));
                snap.start_x = loc_group.x;
                snap.start_y = loc_group.y;
                snap.start_city = (loc_group.city && loc_group.city->OwnerIndex() == our_side)
                                          ? loc_group.city->Index()
                                          : kNone;
                snapshots->push_back(std::move(snap));
                break;
            }
            float chosen_log_prob = 0.0F;
            int chosen_opt_idx = ml::SelectFromLogits(logits, sampler, &chosen_log_prob, &log_mult);
            const float chosen_sample_temp = sampler ? sampler->last_step_temp : 0.0F;
            const JointOption& chosen_opt = options[chosen_opt_idx];
            CandidateTarget& chosen_tgt = targets[chosen_opt.target_idx];
            std::vector<Army*> current_stack = pre_stacks[chosen_opt.stack_opt_idx].units;
            float stack_str = pre_stacks[chosen_opt.stack_opt_idx].str;

            // The per-stack critic only matters while recording a training rollout;
            // it reuses the privileged features computed as the turn began.
            const bool recording = sampler && sampler->current_turn;
            ml::StackValueInVec pre_group_state =
                    recording ? ml::PackStackValueInput(ctx, pre_stacks.back().stack_vec,
                                                        sampler->current_turn->start_critic)
                              : ml::StackValueInVec{};
            float pre_group_value = recording ? policy.EvaluateStackValue(pre_group_state) : 0.0F;

            std::vector<std::uintptr_t> chosen_unit_ids;
            std::vector<std::uintptr_t> pool_unit_ids;
            if (sampler && sampler->current_turn) {
                chosen_unit_ids.reserve(current_stack.size());
                for (Army* army : current_stack)
                    chosen_unit_ids.push_back(reinterpret_cast<std::uintptr_t>(army));
                const auto& full_pool = pre_stacks.back().units;
                pool_unit_ids.reserve(full_pool.size());
                for (Army* army : full_pool)
                    pool_unit_ids.push_back(reinterpret_cast<std::uintptr_t>(army));
            }

            for (size_t i = 1; i < current_stack.size(); i++) {
                auto found = std::find(pool.begin(), pool.end(), current_stack[i]);
                if (found != pool.end()) pool.erase(found);
            }

            bool is_combat_target = (chosen_tgt.kind == TargetKind::kNeutralCity ||
                                     chosen_tgt.kind == TargetKind::kEnemyCity ||
                                     chosen_tgt.kind == TargetKind::kEnemyStack);
            bool stage_near = false;
            float stage_log_prob = 0.0F;
            float stage_sample_temp = 0.0F;
            if (is_combat_target) {
                stage_near =
                        ml::SelectBinary(chosen_opt.out.stage_near_logit, sampler, &stage_log_prob);
                stage_sample_temp = sampler ? sampler->last_step_temp : 0.0F;
            }

            float score_before = (sampler && sampler->current_turn)
                                         ? ml::ComputeEmpireScore(game, our_side)
                                         : 0.0F;
            std::vector<ml::DetourStepRecord> detour_steps;

            if (chosen_tgt.kind == TargetKind::kHoldGarrison) {
                for (Army* army : current_stack) {
                    moved_armies.insert(army->Id());
                    if (policy.Version() >= 7) {
                        army->SetAiOrder(100);
                        army->SetAiDest(TargetOrderKey(game, chosen_tgt));
                        army->SetAiGroup(1);
                    }
                }
                chosen_tgt.assigned_stacks++;
                chosen_tgt.assigned_strength += stack_str;
            } else {
                chosen_tgt.assigned_stacks++;
                chosen_tgt.assigned_strength += stack_str;

                if (chosen_tgt.kind == TargetKind::kEnemyCity && chosen_tgt.city &&
                    chosen_opt.out.utility > best_enemy_city_score) {
                    best_enemy_city_score = chosen_opt.out.utility;
                    best_enemy_city_target = chosen_tgt.city->Index();
                }

                MissionFight plan;
                plan.side_idx = our_side;
                plan.target_kind = static_cast<int>(chosen_tgt.kind);
                plan.decided_win = chosen_opt.tgt_vec[10];
                plan.decided_turns = chosen_opt.tgt_vec[8] > 1e-4F
                                             ? (1.0F / chosen_opt.tgt_vec[8] - 1.0F)
                                             : 99.0F;
                plan.decided_size = static_cast<int>(current_stack.size());
                bool blocked_on_way = false;
                bool moved = ExecuteStackMission(game, side, current_stack, chosen_tgt, stage_near,
                                                 policy, ctx, sampler, moved_armies, plan,
                                                 &blocked_on_way,
                                                 recording ? &detour_steps : nullptr);
                if (!moved) {
                    if (chosen_opt.target_idx >= blocked_targets.size()) {
                        blocked_targets.resize(chosen_opt.target_idx + 1, uint8_t{0});
                    }
                    blocked_targets[chosen_opt.target_idx] = uint8_t{1};
                    chosen_tgt.assigned_stacks = std::max(0, chosen_tgt.assigned_stacks - 1);
                    chosen_tgt.assigned_strength =
                            std::max(0.0F, chosen_tgt.assigned_strength - stack_str);
                    if (blocked_on_way) {
                        std::vector<Army*> again;
                        for (Army* army : current_stack) {
                            if (game.Alive(army) && army->Moves() > 0) again.push_back(army);
                        }
                        pool.insert(pool.begin(), again.begin(), again.end());
                    } else {
                        for (Army* army : current_stack) {
                            if (game.Alive(army) && army->Moves() > 0)
                                moved_armies.insert(army->Id());
                        }
                    }
                } else {
                    seen = game::BuildSeenMasks(game, our_side);
                    if (policy.Version() >= 7) {
                        bool done_obj =
                                (chosen_tgt.city && chosen_tgt.city->OwnerIndex() == our_side &&
                                 chosen_tgt.kind != TargetKind::kReinforceCity);
                        int ord_key = TargetOrderKey(game, chosen_tgt);
                        int ord_code =
                                100 + static_cast<int>(chosen_tgt.kind) + (stage_near ? 20 : 0);
                        for (Army* army : current_stack) {
                            if (!game.Alive(army)) continue;
                            if (done_obj) {
                                army->ClearAiOrder();
                            } else {
                                army->SetAiGroup(
                                        (army->AiOrder() >= 100 && army->AiDest() == ord_key)
                                                ? std::min(250, army->AiGroup() + 1)
                                                : 1);
                                army->SetAiOrder(ord_code);
                                army->SetAiDest(ord_key);
                            }
                        }
                    }
                    if (want_thoughts && !current_stack.empty() && game.Alive(current_stack[0])) {
                        float inv_disc = chosen_opt.tgt_vec[8] > 1e-4F
                                                 ? (1.0F / chosen_opt.tgt_vec[8] - 1.0F)
                                                 : 0.0F;
                        if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                            modern_player->PushThought(AIThought{
                                "Mission",
                                absl::StrFormat("%d-unit stack (%s)",
                                                static_cast<int>(current_stack.size()),
                                                current_stack[0]->Name()),
                                chosen_tgt.label, current_stack[0]->X(), current_stack[0]->Y(),
                                chosen_tgt.x, chosen_tgt.y, chosen_opt.out.utility,
                                chosen_opt.tgt_vec[10], stage_near,
                                absl::StrFormat("U=%.2f Win=%.0f%% Turns=%.1f %s",
                                                chosen_opt.out.utility,
                                                chosen_opt.tgt_vec[10] * 100.0F, inv_disc,
                                                stage_near ? "[STAGE FOR SYNC]" : "[EXECUTE]")});
                        }
                    }

                    // Add newly revealed neutral or enemy cities immediately so remaining stacks
                    // can act on them
                    const CityId quest_city_id =
                            side.ActiveQuest() ? side.ActiveQuest()->TargetCityId()
                                               : CityId::None();
                    for (auto& city : map.Cities()) {
                        if (city.IsRazed() || city.OwnerIndex() == our_side) continue;
                        bool city_vis = seen.all_visible || (city.Id() == quest_city_id);
                        for (int dy = 0; dy <= 1 && !city_vis; dy++) {
                            for (int dx = 0; dx <= 1 && !city_vis; dx++) {
                                if (game::IsSeenAt(seen, city.X() + dx, city.Y() + dy))
                                    city_vis = true;
                            }
                        }
                        if (!city_vis) continue;
                        bool exists = false;
                        for (const auto& target : targets) {
                            if (target.city == &city) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            bool is_enemy_cap = false;
                            for (int side_idx = 0; side_idx < 8; side_idx++) {
                                if (side_idx != our_side &&
                                    map.Sides()[side_idx].CapitalCity(map) == &city)
                                    is_enemy_cap = true;
                            }
                            if (city.IsNeutral()) {
                                CandidateTarget neut;
                                neut.kind = TargetKind::kNeutralCity;
                                neut.x = city.X();
                                neut.y = city.Y();
                                neut.city = &city;
                                neut.owner = kNone;
                                neut.value = static_cast<float>(city.Income()) +
                                             static_cast<float>(city.Slots().size()) * 5.0F;
                                neut.is_enemy_capital = is_enemy_cap;
                                if (want_thoughts) neut.label = "Capture Neutral " + city.Name();
                                targets.push_back(std::move(neut));
                            } else if (Attackable(game, our_side, city.OwnerIndex(), &city)) {
                                CandidateTarget enem;
                                enem.kind = TargetKind::kEnemyCity;
                                enem.x = city.X();
                                enem.y = city.Y();
                                enem.city = &city;
                                enem.owner = city.OwnerIndex();
                                enem.value = static_cast<float>(city.Income()) +
                                             static_cast<float>(city.Defence()) * 6.0F;
                                enem.is_enemy_capital = is_enemy_cap;
                                if (want_thoughts) enem.label = "Siege " + city.Name();
                                targets.push_back(std::move(enem));
                            }
                        }
                    }
                }
            }

            // Departure credit (training only): did this move leave its own city empty?
            int vacated_city = kNone;
            if (recording && loc_group.city && loc_group.city->OwnerIndex() == our_side &&
                chosen_tgt.kind != TargetKind::kHoldGarrison) {
                bool any_left = false;
                for (int dy = 0; dy <= 1 && !any_left; dy++) {
                    for (int dx = 0; dx <= 1 && !any_left; dx++) {
                        for (Army* army : game::ArmiesOnTileSpan(game, loc_group.city->X() + dx,
                                                                 loc_group.city->Y() + dy)) {
                            if (army->OwnerIndex() == our_side) {
                                any_left = true;
                                break;
                            }
                        }
                    }
                }
                if (!any_left) vacated_city = loc_group.city->Index();
            }

            if (sampler && sampler->current_turn &&
                (options.size() > 1 || is_combat_target || !detour_steps.empty())) {
                float score_after = ml::ComputeEmpireScore(game, our_side);
                ml::MissionStepRecord rec;
                rec.candidates.reserve(options.size());
                for (const auto& option : options) rec.candidates.push_back(option.in_vec);
                rec.log_mult = std::move(log_mult);
                rec.chosen_idx = chosen_opt_idx;
                rec.old_log_prob = chosen_log_prob;
                rec.sample_temp = chosen_sample_temp;
                rec.has_stage_choice = is_combat_target;
                rec.stage_chosen = stage_near;
                rec.old_stage_log_prob = stage_log_prob;
                rec.stage_sample_temp = stage_sample_temp;
                rec.detour_steps = std::move(detour_steps);
                rec.immediate_delta_score = score_after - score_before;
                rec.stack_value_in = pre_group_state;
                rec.stack_value_pred = pre_group_value;
                rec.unit_ids = std::move(chosen_unit_ids);
                rec.pool_unit_ids = std::move(pool_unit_ids);
                rec.vacated_city = vacated_city;
                sampler->current_turn->mission_steps.push_back(std::move(rec));
            }
        }
    }

    if (snapshots) return;
    diplomacy::SetAlliedTargetCity(game, our_side, best_enemy_city_target);
    if (events && !events->IsAborted()) (void)events->OnPhase("missions");
}

}  // namespace w2::ai::modern
