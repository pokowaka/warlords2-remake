/**
 * @file
 * @brief Modern AI military and exploration phase: target collection, coalition scoring, and
 * dispatch.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_internal.hpp"
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
                                    const ml::ContextVec& ctx, const PauseCallback& pause,
                                    ml::RolloutSampler* sampler,
                                    std::vector<MissionSnapshot>* snapshots) {
    int our_side = side.index;
    SideBelief& belief = belief::Ensure(side);
    float caution = 0.0F;

    std::vector<CandidateTarget> targets;
    targets.reserve(80);

    for (City* city : belief.visible_cities) {
        if (city->razed) continue;
        float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->index]
                               : 0.0F;
        bool is_quest_or_own = (side.capital == city) || (side.quest && side.quest->city == city);
        bool is_enemy_cap = false;
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            if (side_idx != our_side && game.map->sides[side_idx].capital == city)
                is_enemy_cap = true;
        }

        if (city->owner_index == our_side) {
            float existing_garrison_str = 0.0F;
            float stationary_garrison_str = 0.0F;
            int existing_count = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    for (Army* army : game::ArmiesAt(game, city->x + dx, city->y + dy)) {
                        if (army->owner == our_side) {
                            existing_garrison_str += static_cast<float>(army->strength);
                            existing_count++;
                            if (army->moves <= 0 || army->ai_moved) {
                                stationary_garrison_str += static_cast<float>(army->strength);
                            }
                        }
                    }
                }
            }
            CandidateTarget hold;
            hold.kind = TargetKind::kHoldGarrison;
            hold.x = city->x;
            hold.y = city->y;
            hold.city = city;
            hold.owner = our_side;
            hold.value =
                    static_cast<float>(city->income) + static_cast<float>(city->defence) * 3.0F;
            hold.is_quest_or_own_cap = is_quest_or_own;
            hold.enemy_threat = threat;
            hold.assigned_strength = stationary_garrison_str;
            hold.label = "Hold " + city->name;
            targets.push_back(std::move(hold));

            if (existing_count < 32) {
                CandidateTarget reinf;
                reinf.kind = TargetKind::kReinforceCity;
                reinf.x = city->x;
                reinf.y = city->y;
                reinf.city = city;
                reinf.owner = our_side;
                reinf.value =
                        static_cast<float>(city->income) + static_cast<float>(city->defence) * 3.0F;
                reinf.is_quest_or_own_cap = is_quest_or_own;
                reinf.enemy_threat = threat;
                reinf.assigned_strength = existing_garrison_str;
                reinf.label = "Reinforce " + city->name;
                targets.push_back(std::move(reinf));
            }
        } else if (city->owner_index == kNone) {
            CandidateTarget neut;
            neut.kind = TargetKind::kNeutralCity;
            neut.x = city->x;
            neut.y = city->y;
            neut.city = city;
            neut.owner = kNone;
            neut.value = static_cast<float>(city->income) +
                         static_cast<float>(city->slots.size()) * 5.0F;
            neut.is_quest_or_own_cap = is_quest_or_own;
            neut.is_enemy_capital = is_enemy_cap;
            neut.enemy_threat = threat;
            neut.label = "Capture Neutral " + city->name;
            targets.push_back(std::move(neut));
        } else if (Attackable(game, our_side, city->owner_index, city)) {
            CandidateTarget enem;
            enem.kind = TargetKind::kEnemyCity;
            enem.x = city->x;
            enem.y = city->y;
            enem.city = city;
            enem.owner = city->owner_index;
            enem.value =
                    static_cast<float>(city->income) + static_cast<float>(city->defence) * 6.0F;
            if (diplomacy::AlliedTargetCity(game, city->owner_index) == city->index)
                enem.value += 10.0F;
            enem.is_quest_or_own_cap = is_quest_or_own;
            enem.is_enemy_capital = is_enemy_cap;
            enem.enemy_threat = threat;
            enem.label = "Siege " + city->name;
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
                (side.quest && side.quest->side && side.quest->side->index == obs.owner);
        stack_target.label = Fmt("Intercept stack (%d,%d)", obs.x, obs.y);
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
            site_target.label = "Bless at " + site->name;
        } else {
            site_target.kind = TargetKind::kRuinOrSage;
            site_target.value = site->rich ? 28.0F : 22.0F;
            site_target.label = "Search " + site->name;
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
        ground_target.is_quest_or_own_cap = (side.quest && side.quest->item == item);
        ground_target.label = "Pick up " + item->name;
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
        fog_target.label = Fmt("Explore %s (%d,%d)", frontier.is_water ? "Sea" : "Fog", frontier.x,
                               frontier.y);
        targets.push_back(std::move(fog_target));
    }

    if (targets.empty()) return;

    for (auto& target : targets) {
        if (target.kind == TargetKind::kNeutralCity || target.kind == TargetKind::kEnemyCity ||
            target.kind == TargetKind::kEnemyStack) {
            float near_str = 0.0F;
            for (Army* army : game.armies) {
                if (army->owner == our_side && army->x != kNone && !army->transit &&
                    army->moves > 0 && move::Distance(army->x, army->y, target.x, target.y) <= 14) {
                    near_str += static_cast<float>(army->strength);
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
    for (Army* army : game.armies) {
        if (army->owner != our_side || army->x == kNone || army->transit || army->moves <= 0 ||
            army->ai_moved)
            continue;
        City* city = game::CityAt(game, army->x, army->y);
        int key = city ? -(city->index + 1) : (army->y * game.map->width + army->x);
        auto& loc_group = groups_by_loc[key];
        loc_group.x = army->x;
        loc_group.y = army->y;
        loc_group.city = city;
        loc_group.units.push_back(army);
    }

    int best_enemy_city_target = kNone;
    float best_enemy_city_score = -1e9F;

    for (auto& [loc_key, loc_group] : groups_by_loc) {
        std::vector<Army*> pool = loc_group.units;
        std::sort(pool.begin(), pool.end(), [&](Army* lhs, Army* rhs) {
            if (lhs->Hero() != rhs->Hero()) return lhs->Hero() > rhs->Hero();
            bool lhs_flies = core::Flies(game, lhs), rhs_flies = core::Flies(game, rhs);
            if (lhs_flies != rhs_flies) return lhs_flies > rhs_flies;
            int lhs_score = lhs->strength * 2 + lhs->max_moves;
            int rhs_score = rhs->strength * 2 + rhs->max_moves;
            if (lhs_score != rhs_score) return lhs_score > rhs_score;
            return lhs->moves > rhs->moves;
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
        if (game.map->options.hidden_map != 0) {
            int reachable_fog_count = 0;
            for (const auto& target : targets) {
                if (target.kind == TargetKind::kFogFrontier &&
                    core::FloodAt(land_flood, target.x, target.y) < core::kUnreached) {
                    reachable_fog_count++;
                }
            }
            if (reachable_fog_count < 4) {
                int width = game.map->width, height = game.map->height;
                for (int radius = 1; radius <= 28 && reachable_fog_count < 6; radius++) {
                    for (int dy = -radius; dy <= radius && reachable_fog_count < 6; dy++) {
                        for (int dx = -radius; dx <= radius && reachable_fog_count < 6; dx++) {
                            if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                            int fog_x = loc_group.x + dx, fog_y = loc_group.y + dy;
                            if (fog_x < 0 || fog_y < 0 || fog_x >= width || fog_y >= height)
                                continue;
                            int fterr =
                                    game.map->terrain_type[game.map->tiles[fog_y * width + fog_x] &
                                                           0xff];
                            if (fterr == move::kMountains) continue;
                            if (core::FloodAt(land_flood, fog_x, fog_y) >= core::kUnreached)
                                continue;
                            int adj_u = 0;
                            for (int oy = -1; oy <= 1; oy++) {
                                for (int ox = -1; ox <= 1; ox++) {
                                    int unseen_x = fog_x + ox, unseen_y = fog_y + oy;
                                    if (unseen_x >= 0 && unseen_y >= 0 && unseen_x < width &&
                                        unseen_y < height &&
                                        !belief::IsTileVisible(game, our_side, unseen_x,
                                                               unseen_y)) {
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
                                    bool road = !is_w &&
                                                (scn::RoadAt(*game.map, fog_x, fog_y) % 0x20 != 0);
                                    fog_target.value =
                                            static_cast<float>(adj_u) * 2.0F + (road ? 6.0F : 0.0F);
                                    fog_target.label = Fmt("Explore %s (%d,%d)",
                                                           is_w ? "Sea" : "Fog", fog_x, fog_y);
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
        std::set<size_t> local_fog_indices;
        if (game.map->options.hidden_map != 0) {
            std::vector<std::pair<int, size_t>> land_fogs, sea_fogs;
            for (size_t ti = 0; ti < targets.size(); ti++) {
                if (targets[ti].kind != TargetKind::kFogFrontier) continue;
                int d_land = core::FloodAt(land_flood, targets[ti].x, targets[ti].y);
                int d_fly = fly_dist(targets[ti].x, targets[ti].y);
                int best_d = std::min(d_land, d_fly);
                if (best_d >= core::kUnreached) continue;
                if (targets[ti].is_water)
                    sea_fogs.emplace_back(best_d, ti);
                else
                    land_fogs.emplace_back(best_d, ti);
            }
            std::sort(land_fogs.begin(), land_fogs.end());
            std::sort(sea_fogs.begin(), sea_fogs.end());
            for (size_t i = 0; i < std::min<size_t>(6, land_fogs.size()); i++) {
                local_fog_indices.insert(land_fogs[i].second);
            }
            for (size_t i = 0; i < std::min<size_t>(4, sea_fogs.size()); i++) {
                local_fog_indices.insert(sea_fogs[i].second);
            }
        }

        std::set<size_t> blocked_targets;

        while (!pool.empty()) {
            Army* seed = pool.front();
            pool.erase(pool.begin());
            if (!game.Alive(seed) || seed->moves <= 0 || seed->ai_moved) continue;

            std::vector<Army*> eligible_escorts;
            for (auto iter = pool.begin(); iter != pool.end();) {
                Army* cand = *iter;
                if (!game.Alive(cand) || cand->moves <= 0 || cand->ai_moved) {
                    iter = pool.erase(iter);
                    continue;
                }
                if (eligible_escorts.size() + 1 < rules::kMaxStack) {
                    eligible_escorts.push_back(cand);
                }
                ++iter;
            }

            float rem_group_str = static_cast<float>(seed->strength);
            for (Army* rem : pool) rem_group_str += static_cast<float>(rem->strength);

            float committed_city_guard_str = 0.0F;
            float origin_threat = 0.0F;
            if (loc_group.city && loc_group.city->owner_index == our_side) {
                if (loc_group.city->index < static_cast<int>(belief.city_threat2_turn.size())) {
                    origin_threat = belief.city_threat2_turn[loc_group.city->index];
                }
                for (int dy = 0; dy <= 1; dy++) {
                    for (int dx = 0; dx <= 1; dx++) {
                        for (Army* army :
                             game::ArmiesAt(game, loc_group.city->x + dx, loc_group.city->y + dy)) {
                            if (army->owner == our_side && (army->moves <= 0 || army->ai_moved)) {
                                committed_city_guard_str += static_cast<float>(army->strength);
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
                ml::StackVec sv{};         ///< Precomputed StackVec features.
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
                    prebuilt.str += static_cast<float>(army->strength);
                float rem_guard_after_leave =
                        (loc_group.city && loc_group.city->owner_index == our_side)
                                ? (committed_city_guard_str +
                                   std::max(0.0F, rem_group_str - prebuilt.str))
                                : 20.0F;
                prebuilt.sv = BuildStackFeatures(game, prebuilt.units, seed->moves,
                                                 rem_guard_after_leave, origin_threat);
                prebuilt.flies = (move::ModeOf(game, prebuilt.units) == move::kFlying);
                pre_stacks.push_back(std::move(prebuilt));
            }

            /** @brief A (target, coalition) pair scored by MissionNet. */
            struct JointOption {
                size_t target_idx = 0;      ///< Index into `targets`.
                size_t stack_opt_idx = 0;   ///< Index into `pre_stacks`.
                ml::MissionInVec in_vec{};  ///< Packed MissionNet input.
                ml::TargetVec tv{};         ///< Target feature vector.
                ml::MissionOutput out{};    ///< MissionNet output (utility and stage logit).
            };
            std::vector<JointOption> options;
            options.reserve(targets.size() * pre_stacks.size());

            // Scoring options only reads the board: answer per-tile army queries
            // (combat estimates) from one index instead of scanning every army.
            // The scope closes before the chosen stack moves.
            {
                belief::ScopedTileIndex scoring_index(game);
                for (size_t ti = 0; ti < targets.size(); ti++) {
                    if (blocked_targets.count(ti)) continue;
                    const CandidateTarget& tgt = targets[ti];

                    if ((tgt.kind == TargetKind::kNeutralCity ||
                         tgt.kind == TargetKind::kEnemyCity) &&
                        tgt.city && (tgt.city->owner_index == our_side || tgt.city->razed)) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kEnemyStack &&
                        belief::IsTileVisible(game, our_side, tgt.x, tgt.y)) {
                        // Only re-check stacks still in view; one that walked into fog stays a
                        // belief
                        auto cur_at = belief::ArmiesAt(game, tgt.x, tgt.y);
                        if (cur_at.empty() || cur_at[0]->owner == our_side) continue;
                    }
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.site &&
                        !core::SiteOpen(*tgt.site))
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item &&
                        (tgt.item->status != 1 || tgt.item->x == kNone)) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kFogFrontier && game.map->options.hidden_map != 0) {
                        if (tgt.assigned_stacks >= 1) continue;
                        if (!local_fog_indices.empty() && !local_fog_indices.count(ti)) continue;
                        bool still_has_unseen = false;
                        for (int oy = -1; oy <= 1 && !still_has_unseen; oy++) {
                            for (int ox = -1; ox <= 1 && !still_has_unseen; ox++) {
                                int unseen_x = tgt.x + ox, unseen_y = tgt.y + oy;
                                if (unseen_x >= 0 && unseen_y >= 0 && unseen_x < game.map->width &&
                                    unseen_y < game.map->height &&
                                    !belief::IsTileVisible(game, our_side, unseen_x, unseen_y)) {
                                    still_has_unseen = true;
                                }
                            }
                        }
                        if (!still_has_unseen) continue;
                    }

                    if (tgt.kind == TargetKind::kHoldGarrison && tgt.city != loc_group.city)
                        continue;
                    if (tgt.kind == TargetKind::kReinforceCity && tgt.city == loc_group.city)
                        continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && !seed->Hero()) continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item && !seed->Hero())
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.site &&
                        !seed->blessings.empty() && !seed->Hero()) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.assigned_stacks >= 1) continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.assigned_stacks >= 1) continue;

                    bool is_combat_tgt = (tgt.kind == TargetKind::kNeutralCity ||
                                          tgt.kind == TargetKind::kEnemyCity ||
                                          tgt.kind == TargetKind::kEnemyStack);
                    for (size_t si = 0; si < pre_stacks.size(); si++) {
                        const PrebuiltStack& prebuilt = pre_stacks[si];
                        const core::Flood& active_flood = prebuilt.flies ? fly_flood : land_flood;
                        int p_cost = TargetPathCost(game, active_flood, tgt);
                        if (p_cost >= core::kUnreached) continue;

                        float near_str = 0.0F;
                        ml::StackVec opt_sv = prebuilt.sv;
                        if (tgt.kind == TargetKind::kHoldGarrison) {
                            near_str = rem_group_str;
                            opt_sv[14] =
                                    std::tanh((committed_city_guard_str + prebuilt.str) / 10.0F);
                        } else if (is_combat_tgt) {
                            near_str = std::max(0.0F, tgt.nearby_friendly_str - rem_group_str);
                        }

                        ml::TargetVec target_vec = BuildTargetFeatures(
                                game, side, prebuilt.units, tgt, near_str, caution, p_cost);
                        ml::MissionInVec m_in = ml::PackMissionInput(ctx, opt_sv, target_vec);
                        auto raw = policy.mission_net.Forward(m_in);
                        options.push_back(JointOption{ti, si, m_in, target_vec,
                                                      ml::MissionOutput{raw[0], raw[1]}});
                    }
                }
            }  // scoringIndex

            if (options.empty()) {
                seed->ai_moved = true;
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
                    snap.city_index.push_back(target.city ? target.city->index : kNone);
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
                snap.start_city = (loc_group.city && loc_group.city->owner_index == our_side)
                                          ? loc_group.city->index
                                          : kNone;
                snapshots->push_back(std::move(snap));
                break;
            }
            float chosen_log_prob = 0.0F;
            int chosen_opt_idx = ml::SelectFromLogits(logits, sampler, &chosen_log_prob, &log_mult);
            const JointOption& chosen_opt = options[chosen_opt_idx];
            CandidateTarget& chosen_tgt = targets[chosen_opt.target_idx];
            std::vector<Army*> current_stack = pre_stacks[chosen_opt.stack_opt_idx].units;
            float stack_str = pre_stacks[chosen_opt.stack_opt_idx].str;

            // The per-stack critic only matters while recording a training rollout;
            // it reuses the privileged features computed as the turn began.
            const bool recording = sampler && sampler->current_turn;
            ml::StackValueInVec pre_group_state =
                    recording ? ml::PackStackValueInput(ctx, pre_stacks.back().sv,
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
            if (is_combat_target) {
                stage_near =
                        ml::SelectBinary(chosen_opt.out.stage_near_logit, sampler, &stage_log_prob);
            }

            float score_before = (sampler && sampler->current_turn)
                                         ? ml::ComputeEmpireScore(game, our_side)
                                         : 0.0F;

            if (chosen_tgt.kind == TargetKind::kHoldGarrison) {
                for (Army* army : current_stack) army->ai_moved = true;
                chosen_tgt.assigned_stacks++;
                chosen_tgt.assigned_strength += stack_str;
            } else {
                chosen_tgt.assigned_stacks++;
                chosen_tgt.assigned_strength += stack_str;

                if (chosen_tgt.kind == TargetKind::kEnemyCity && chosen_tgt.city &&
                    chosen_opt.out.utility > best_enemy_city_score) {
                    best_enemy_city_score = chosen_opt.out.utility;
                    best_enemy_city_target = chosen_tgt.city->index;
                }

                bool moved = ExecuteStackMission(game, side, current_stack, chosen_tgt, stage_near,
                                                 policy, ctx, sampler);
                if (!moved) {
                    blocked_targets.insert(chosen_opt.target_idx);
                    chosen_tgt.assigned_stacks = std::max(0, chosen_tgt.assigned_stacks - 1);
                    chosen_tgt.assigned_strength =
                            std::max(0.0F, chosen_tgt.assigned_strength - stack_str);
                    for (Army* army : current_stack) {
                        if (game.Alive(army) && army->moves > 0) army->ai_moved = true;
                    }
                } else {
                    if (!current_stack.empty() && game.Alive(current_stack[0])) {
                        float inv_disc =
                                chosen_opt.tv[8] > 1e-4F ? (1.0F / chosen_opt.tv[8] - 1.0F) : 0.0F;
                        side.ai_thoughts.push_back(AIThought{
                            "Mission",
                            Fmt("%d-unit stack (%s)", static_cast<int>(current_stack.size()),
                                current_stack[0]->name.c_str()),
                            chosen_tgt.label, current_stack[0]->x, current_stack[0]->y,
                            chosen_tgt.x, chosen_tgt.y, chosen_opt.out.utility, chosen_opt.tv[10],
                            stage_near,
                            Fmt("U=%.2f Win=%.0f%% Turns=%.1f %s", chosen_opt.out.utility,
                                chosen_opt.tv[10] * 100.0F, inv_disc,
                                stage_near ? "[STAGE FOR SYNC]" : "[EXECUTE]")});
                    }

                    // Add newly revealed neutral or enemy cities immediately so remaining stacks
                    // can act on them
                    for (auto& city : game.map->cities) {
                        if (city.razed || city.owner_index == our_side ||
                            !belief::IsCityVisible(game, our_side, city))
                            continue;
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
                                    game.map->sides[side_idx].capital == &city)
                                    is_enemy_cap = true;
                            }
                            if (city.owner_index == kNone) {
                                CandidateTarget neut;
                                neut.kind = TargetKind::kNeutralCity;
                                neut.x = city.x;
                                neut.y = city.y;
                                neut.city = &city;
                                neut.owner = kNone;
                                neut.value = static_cast<float>(city.income) +
                                             static_cast<float>(city.slots.size()) * 5.0F;
                                neut.is_enemy_capital = is_enemy_cap;
                                neut.label = "Capture Neutral " + city.name;
                                targets.push_back(std::move(neut));
                            } else if (Attackable(game, our_side, city.owner_index, &city)) {
                                CandidateTarget enem;
                                enem.kind = TargetKind::kEnemyCity;
                                enem.x = city.x;
                                enem.y = city.y;
                                enem.city = &city;
                                enem.owner = city.owner_index;
                                enem.value = static_cast<float>(city.income) +
                                             static_cast<float>(city.defence) * 6.0F;
                                enem.is_enemy_capital = is_enemy_cap;
                                enem.label = "Siege " + city.name;
                                targets.push_back(std::move(enem));
                            }
                        }
                    }
                }
            }

            // Departure credit (training only): did this move leave its own city empty?
            int vacated_city = kNone;
            if (recording && loc_group.city && loc_group.city->owner_index == our_side &&
                chosen_tgt.kind != TargetKind::kHoldGarrison) {
                bool any_left = false;
                for (int dy = 0; dy <= 1 && !any_left; dy++) {
                    for (int dx = 0; dx <= 1 && !any_left; dx++) {
                        for (Army* army :
                             game::ArmiesAt(game, loc_group.city->x + dx, loc_group.city->y + dy)) {
                            if (army->owner == our_side) {
                                any_left = true;
                                break;
                            }
                        }
                    }
                }
                if (!any_left) vacated_city = loc_group.city->index;
            }

            if (sampler && sampler->current_turn && (options.size() > 1 || is_combat_target)) {
                float score_after = ml::ComputeEmpireScore(game, our_side);
                ml::MissionStepRecord rec;
                rec.candidates.reserve(options.size());
                for (const auto& option : options) rec.candidates.push_back(option.in_vec);
                rec.log_mult = std::move(log_mult);
                rec.chosen_idx = chosen_opt_idx;
                rec.old_log_prob = chosen_log_prob;
                rec.has_stage_choice = is_combat_target;
                rec.stage_chosen = stage_near;
                rec.old_stage_log_prob = stage_log_prob;
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
    if (pause) pause("missions");
}

}  // namespace w2::ai::modern
