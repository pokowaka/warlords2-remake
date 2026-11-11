/**
 * @file
 * @brief Modern AI feature extraction and DiploNet diplomacy/missive phase.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/classic_ai_player.hpp"
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
 * @brief Returns whether `our_side` may choose an attack on `owner` (at `city`, or a field stack
 * when null).
 *
 * Same rule the actuator enforces: no declaration needed, or DiploNet is no
 * longer proposing peace to that side (so the attack declares war).
 */
bool Attackable(const Game& game, int our_side, int owner, const City* city) {
    if (owner == kNone) return true;
    return !diplomacy::MustDeclare(game, our_side, owner, city) ||
           diplomacy::Proposal(game, our_side, owner) != diplomacy::kPeace;
}

/**
 * @brief Builds the 16-dimensional feature vector describing `stack` for MissionNet and
 * StackValueNet.
 */
ml::StackVec BuildStackFeatures(const Game& game, const std::vector<Army*>& stack,
                                int prev_min_move, float origin_rem_guard_str,
                                float origin_threat) {
    ml::StackVec features{};
    if (stack.empty()) return features;
    int total_str = 0, min_move = 99, total_upkeep = 0, blessed = 0;
    bool has_hero = false, has_flier = false, all_fly = true, has_siege = false,
         has_special = false;
    for (Army* army : stack) {
        total_str += army->Strength();
        min_move = std::min(min_move, army->Moves());
        total_upkeep += army->Upkeep();
        if (!army->Blessings().empty()) blessed++;
        if (army->Hero()) has_hero = true;
        const ArmyType* army_type = game.ArmyTypes().ById(army->Type());
        if (army_type) {
            if (army_type->flies)
                has_flier = true;
            else
                all_fly = false;
            if (army_type->siege) has_siege = true;
            if (army_type->IsMagical() || army_type->Ability() != 0) has_special = true;
        } else {
            all_fly = false;
        }
    }
    if (min_move == 99) min_move = 0;
    int hero_cmd = combat::HeroBonus(game, stack);
    float speed_drop = (prev_min_move > 0 && min_move < prev_min_move)
                               ? static_cast<float>(prev_min_move - min_move) / 20.0F
                               : 0.0F;

    features[0] = static_cast<float>(stack.size()) / 8.0F;
    features[1] = std::tanh(static_cast<float>(total_str) / 40.0F);
    features[2] = static_cast<float>(total_str) / static_cast<float>(stack.size() * 9);
    features[3] = std::tanh(static_cast<float>(min_move) / 16.0F);
    features[4] = has_hero ? 1.0F : 0.0F;
    features[5] = has_flier ? 1.0F : 0.0F;
    features[6] = all_fly ? 1.0F : 0.0F;
    features[7] = has_siege ? 1.0F : 0.0F;
    features[8] = has_special ? 1.0F : 0.0F;
    features[9] = static_cast<float>(blessed) / static_cast<float>(stack.size());
    features[10] = std::tanh(static_cast<float>(total_upkeep) / 20.0F);
    features[11] = std::tanh(static_cast<float>(hero_cmd) / 3.0F);
    features[12] = (stack.size() == 1) ? 1.0F : 0.0F;
    features[13] = speed_drop;
    features[14] = std::tanh(origin_rem_guard_str / 10.0F);
    features[15] = std::tanh(origin_threat / 20.0F);
    return features;
}

/**
 * @brief Returns the movement-point flood cost to reach `tgt` (or its best corner for a 2x2 city).
 */
int TargetPathCost(const Game& game, const core::Flood& flood, const CandidateTarget& tgt) {
    if (tgt.kind == TargetKind::kHoldGarrison) return 0;
    if (tgt.city) {
        const Map& map = game.GetMap();
        int best = core::kUnreached;
        for (int dy = -1; dy <= 2; dy++) {
            for (int dx = -1; dx <= 2; dx++) {
                int tile_x = tgt.city->X() + dx, tile_y = tgt.city->Y() + dy;
                if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() && tile_y < map.Height()) {
                    int cost = core::FloodAt(flood, tile_x, tile_y);
                    if (cost < best) best = cost;
                }
            }
        }
        return best;
    }
    return core::FloodAt(flood, tgt.x, tgt.y);
}

/**
 * @brief Encodes `tgt` into a compact integer key for cross-turn order persistence (`ai_dest_`):
 * city index when targeting a city, or `10000 + y * width + x` for tile targets.
 */
int TargetOrderKey(const Game& game, const CandidateTarget& tgt) {
    if (tgt.city) return tgt.city->Index();
    return 10000 + tgt.y * game.GetMap().Width() + tgt.x;
}

/**
 * @brief Builds the 29-dimensional feature vector describing mission target `tgt` for `stack`.
 */
ml::TargetVec BuildTargetFeatures(Game& game, const Side& side, const std::vector<Army*>& stack,
                                  const CandidateTarget& tgt, float nearby_friendly_str,
                                  float caution_bias, int path_cost, int policy_version,
                                  int blocker_x, int blocker_y) {
    ml::TargetVec features{};
    features[static_cast<int>(tgt.kind)] = 1.0F;

    int start_x = stack[0]->X(), start_y = stack[0]->Y();
    int min_move = std::max(2, move::StackMoves(stack));
    int eff_cost =
            path_cost >= 0 ? path_cost : (move::Distance(start_x, start_y, tgt.x, tgt.y) * 2);
    float turns_to_reach = static_cast<float>(eff_cost) / static_cast<float>(std::max(4, min_move));

    float win_now = 0.0F, win_converged = 0.0F, surv_ratio = 0.0F;
    float seen_fraction = 1.0F;
    bool is_combat = (tgt.kind == TargetKind::kNeutralCity || tgt.kind == TargetKind::kEnemyCity ||
                      tgt.kind == TargetKind::kEnemyStack);
    float extra_en_route = policy_version >= 7 ? tgt.en_route_strength : 0.0F;
    if (is_combat) {
        auto est = belief::EstimateCombat(game, side, stack, tgt.x, tgt.y, caution_bias);
        win_now = est.win_prob;
        surv_ratio = est.expected_survivors / static_cast<float>(std::max<size_t>(1, stack.size()));
        seen_fraction = est.seen_tile_fraction;

        float my_str = 0.0F;
        for (Army* army : stack) my_str += static_cast<float>(army->Strength());
        int free_slots = std::max(0, rules::kMaxStack - static_cast<int>(stack.size()));
        float add_str =
                std::min(nearby_friendly_str + tgt.assigned_strength + extra_en_route * 0.5F,
                         static_cast<float>(free_slots) * 5.0F);
        if (add_str > 0.5F && est.estimated_defender_strength > 0.0F) {
            float combined_ratio =
                    (my_str + add_str) / (my_str + add_str + est.estimated_defender_strength);
            win_converged = std::clamp(std::max(win_now, combined_ratio), 0.0F, 1.0F);
        } else {
            win_converged = win_now;
        }
        if (blocker_x != kNone && !(blocker_x == tgt.x && blocker_y == tgt.y)) {
            auto b_est =
                    belief::EstimateCombat(game, side, stack, blocker_x, blocker_y, caution_bias);
            win_now *= b_est.win_prob;
            float b_surv = b_est.expected_survivors /
                           static_cast<float>(std::max<size_t>(1, stack.size()));
            surv_ratio = std::min(surv_ratio, b_surv);
            win_converged = std::min(win_converged, b_est.win_prob);
        }
    }

    bool flies = (move::ModeOf(game, stack) == move::kFlying);
    bool at_sea = false;
    for (Army* army : stack) {
        if (army->IsAtSea()) at_sea = true;
    }
    int tile_dist = std::max(1, move::Distance(start_x, start_y, tgt.x, tgt.y));
    bool requires_boat_embark =
            !flies && !at_sea && (tgt.is_water || (eff_cost >= tile_dist * 2 + 24));

    features[8] = 1.0F / (1.0F + turns_to_reach);
    features[9] = (turns_to_reach <= 1.0F) ? 1.0F : 0.0F;
    features[10] = win_now;
    features[11] = win_converged;
    features[12] = surv_ratio;
    features[13] = std::tanh(tgt.value / 30.0F);
    features[14] = tgt.is_quest_or_own_cap ? 1.0F : 0.0F;
    features[15] = tgt.is_enemy_capital ? 1.0F : 0.0F;
    features[16] = std::tanh(tgt.enemy_threat / 25.0F);
    features[17] = std::tanh((tgt.assigned_strength + nearby_friendly_str) / 25.0F);
    features[18] = std::tanh(static_cast<float>(tgt.assigned_stacks) / 2.0F);
    features[19] = requires_boat_embark ? 1.0F : 0.0F;
    features[20] = seen_fraction;
    if (policy_version >= 7) {
        int ord_key = TargetOrderKey(game, tgt);
        int matched = 0, max_committed = 0;
        bool staged_here = false;
        for (Army* army : stack) {
            if (army->AiOrder() >= 100 && army->AiDest() == ord_key) {
                matched++;
                max_committed = std::max(max_committed, army->AiGroup());
                if (army->AiOrder() >= 120) staged_here = true;
            }
        }
        features[21] =
                static_cast<float>(matched) / static_cast<float>(std::max<size_t>(1, stack.size()));
        features[22] = staged_here ? 1.0F : 0.0F;
        features[23] = std::tanh(static_cast<float>(max_committed) / 4.0F);
        features[24] = std::tanh(tgt.en_route_strength / 25.0F);
    }
    if (policy_version >= 8) {
        bool stack_has_hero = false;
        for (Army* army : stack) {
            if (army->Hero()) stack_has_hero = true;
        }
        if (tgt.is_muster_hub && tgt.kind == TargetKind::kReinforceCity) {
            features[25] = 1.0F;
            features[26] = std::clamp(
                    (8.0F - static_cast<float>(tgt.hub_armies + tgt.assigned_stacks * 2)) / 8.0F,
                    0.0F, 1.0F);
            features[27] = std::tanh(
                    static_cast<float>(tgt.hub_incoming_flow + tgt.assigned_stacks) / 4.0F);
            features[28] = (tgt.hub_has_hero || stack_has_hero) ? 1.0F : 0.0F;
        } else if (tgt.is_muster_hub && tgt.kind == TargetKind::kHoldGarrison) {
            features[25] = 0.0F;
            features[26] =
                    std::clamp((7.0F - static_cast<float>(tgt.hub_armies)) / 6.0F, 0.0F, 1.0F);
            features[27] = std::tanh(static_cast<float>(tgt.hub_incoming_flow) / 4.0F);
            features[28] = tgt.hub_has_hero ? 1.0F : 0.0F;
        }
    }
    return features;
}

/**
 * @brief Evaluates DiploNet against each rival and proposes stances, tribute, joint war, and
 * missives.
 */
void RunDiplomacyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       std::array<bool, 8>& decided) {
    Map& map = game.GetMap();
    if (map.GameOptions().diplomacy == 0) return;
    int our_side = side.Index();
    const SideBelief& belief = belief::Ensure(side);
    const auto& prof = personality::ForSide(side);

    int total_vis_cities = 0, max_vis_cities = 0, leader_side = kNone;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        total_vis_cities += belief.visible_cities_by_side[side_idx];
        if (belief.visible_cities_by_side[side_idx] > max_vis_cities) {
            max_vis_cities = belief.visible_cities_by_side[side_idx];
            leader_side = side_idx;
        }
    }

    int active_wars = 0;
    int primary_enemy = kNone;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        if (side_idx != our_side && map.Sides()[side_idx].InUse() &&
            map.Sides()[side_idx].IsAlive() &&
            (diplomacy::State(game, our_side, side_idx) == diplomacy::kWar ||
             (decided[side_idx] &&
              diplomacy::Proposal(game, our_side, side_idx) != diplomacy::kPeace))) {
            active_wars++;
            if (primary_enemy == kNone &&
                (diplomacy::State(game, our_side, side_idx) == diplomacy::kWar ||
                 diplomacy::Proposal(game, our_side, side_idx) == diplomacy::kWar)) {
                primary_enemy = side_idx;
            }
        }
    }

    float total_my_mobile_str = 0.0F;
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() == our_side && army->X() != kNone && !army->InTransit())
            total_my_mobile_str += static_cast<float>(army->Strength());
    }

    auto my_cities_list = game::SideCities(game, side);
    std::array<float, 8> rival_min_dist;
    rival_min_dist.fill(120.0F);
    int closest_rival = kNone;
    float closest_rival_dist = 999.0F;
    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = map.Sides()[other_idx];
        if (other_idx == our_side || !other.InUse() || !other.IsAlive()) continue;
        if (belief.visible_cities_by_side[other_idx] == 0 &&
            belief.their_border_strength[other_idx] == 0.0F)
            continue;
        float min_dist = 120.0F;
        for (City* my_city : my_cities_list) {
            for (City* vis_city : belief.visible_cities) {
                if (vis_city->OwnerIndex() == other_idx) {
                    min_dist = std::min(min_dist, static_cast<float>(move::Distance(
                                                          my_city->X(), my_city->Y(), vis_city->X(),
                                                          vis_city->Y())));
                }
            }
        }
        rival_min_dist[other_idx] = min_dist;
        if (min_dist < closest_rival_dist) {
            closest_rival_dist = min_dist;
            closest_rival = other_idx;
        }
    }

    int wars_committed = active_wars;
    const City* own_capital = side.CapitalCity(map);

    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = map.Sides()[other_idx];
        if (other_idx == our_side || !other.InUse() || !other.IsAlive() || decided[other_idx])
            continue;
        int cur_st = diplomacy::State(game, our_side, other_idx);
        if (map.GameOptions().hidden_map != 0 && belief.visible_cities_by_side[other_idx] == 0 &&
            belief.their_border_strength[other_idx] == 0.0F && cur_st != diplomacy::kWar) {
            continue;
        }
        decided[other_idx] = true;

        auto att = diplomacy::ComputeAttitude(game, our_side, other_idx);
        bool cur_allied = diplomacy::IsAllied(game, our_side, other_idx);
        float min_dist = rival_min_dist[other_idx];

        int shared_wars = 0;
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            if (side_idx != our_side && side_idx != other_idx && map.Sides()[side_idx].IsAlive() &&
                diplomacy::State(game, our_side, side_idx) == diplomacy::kWar &&
                diplomacy::State(game, other_idx, side_idx) == diplomacy::kWar) {
                shared_wars++;
            }
        }

        int grudge = 0;
        if (const ModernMlPlayer* modern_player = ModernMlPlayer::FindOn(side)) {
            const ai::BattleGrudge& battle_grudge = modern_player->Grudge();
            grudge = battle_grudge.cities_lost[other_idx] * 8 +
                     battle_grudge.heroes_killed[other_idx] * 6 + battle_grudge.lost[other_idx] * 2;
        } else if (const ClassicAiPlayer* classic_player = ClassicAiPlayer::FindOn(side)) {
            const AIData& data = classic_player->Data();
            grudge = data.cities_lost[other_idx] * 8 + data.heroes_killed[other_idx] * 6 +
                     data.lost[other_idx] * 2;
        }

        int other_wars = wars_committed - (cur_st == diplomacy::kWar ? 1 : 0);
        float eff_my_str =
                std::max(belief.my_border_strength[other_idx],
                         total_my_mobile_str * std::clamp(1.1F - min_dist / 45.0F, 0.0F, 0.8F));
        bool is_leader_or_closest =
                (leader_side == other_idx &&
                 belief.visible_cities_by_side[other_idx] * 3 >= total_vis_cities) ||
                (other_idx == closest_rival);

        ml::RivalVec rival_vec{};
        rival_vec[0] = cur_allied ? 1.0F : 0.0F;
        rival_vec[1] = (!cur_allied && cur_st == diplomacy::kPeace) ? 1.0F : 0.0F;
        rival_vec[2] = (cur_st == diplomacy::kIntermediate) ? 1.0F : 0.0F;
        rival_vec[3] = (cur_st == diplomacy::kWar) ? 1.0F : 0.0F;
        rival_vec[4] = static_cast<float>(diplomacy::TruceTurns(game, our_side, other_idx)) / 15.0F;
        rival_vec[5] = static_cast<float>(att.trust_score) / 100.0F;
        rival_vec[6] = static_cast<float>(att.total_score) / 100.0F;
        rival_vec[7] = std::tanh(eff_my_str / 35.0F);
        rival_vec[8] = std::tanh(belief.their_border_strength[other_idx] / 35.0F);
        rival_vec[9] = std::tanh((eff_my_str - belief.their_border_strength[other_idx]) / 20.0F);
        rival_vec[10] = static_cast<float>(belief.visible_cities_by_side[other_idx]) /
                        static_cast<float>(std::max(1, total_vis_cities));
        rival_vec[11] = is_leader_or_closest ? 1.0F : 0.0F;
        rival_vec[12] = std::min(1.0F, static_cast<float>(shared_wars) / 2.0F);
        rival_vec[13] = std::tanh(static_cast<float>(grudge) / 30.0F);
        rival_vec[14] = (own_capital && own_capital->OwnerIndex() == other_idx) ? 1.0F : 0.0F;
        rival_vec[15] = std::tanh(
                static_cast<float>(diplomacy::GoldOffer(game, other_idx, our_side)) / 200.0F);
        rival_vec[16] = std::tanh(min_dist / 45.0F);
        rival_vec[17] = std::tanh(static_cast<float>(other_wars) / 3.0F);

        ml::DiploInVec d_in = ml::PackDiploInput(ctx, rival_vec);
        auto raw = policy.ForwardDiplo(d_in);
        std::vector<float> stance_vec = {raw[0], raw[1], raw[2], raw[3]};
        ml::DiploStepRecord rec;
        rec.input = d_in;
        int best_choice = ml::SelectFromLogits(stance_vec, sampler, &rec.old_stance_log_prob);
        rec.chosen_stance = best_choice;
        float best_logit = stance_vec[best_choice];

        if (best_choice == 0) {
            diplomacy::Propose(game, our_side, other_idx, diplomacy::kPeace);
            diplomacy::ProposeAlliance(game, our_side, other_idx, true);
        } else if (best_choice == 1) {
            diplomacy::Propose(game, our_side, other_idx, diplomacy::kPeace);
            diplomacy::ProposeAlliance(game, our_side, other_idx, false);
        } else if (best_choice == 2) {
            diplomacy::Propose(game, our_side, other_idx, diplomacy::kIntermediate);
            if (cur_st != diplomacy::kWar) wars_committed++;
        } else {
            diplomacy::Propose(game, our_side, other_idx, diplomacy::kWar);
            if (cur_st != diplomacy::kWar) wars_committed++;
            if (primary_enemy == kNone) primary_enemy = other_idx;
        }

        // Binary sub-decision (output 4): sweeten a peace offer to a side we are at war with
        bool friendly = (best_choice == 0 || best_choice == 1);
        diplomacy::SetGoldOffer(game, our_side, other_idx, 0);
        if (friendly && cur_st == diplomacy::kWar && side.Gold() >= 100) {
            rec.has_tribute = true;
            rec.tribute_chosen = ml::SelectBinary(raw[4], sampler, &rec.old_tribute_log_prob);
            if (rec.tribute_chosen) {
                int offer =
                        std::clamp(side.Gold() / 4 / 25 * 25, 50, std::min(300, side.Gold() / 2));
                diplomacy::SetGoldOffer(game, our_side, other_idx, offer);
            }
        }

        // Binary sub-decision (output 5): ask a friendly side to join our war
        diplomacy::SetWarTarget(game, our_side, other_idx, kNone);
        if (friendly && primary_enemy != kNone && primary_enemy != other_idx) {
            rec.has_joint_war = true;
            rec.joint_war_chosen = ml::SelectBinary(raw[5], sampler, &rec.old_joint_war_log_prob);
            if (rec.joint_war_chosen)
                diplomacy::SetWarTarget(game, our_side, other_idx, primary_enemy);
        }

        if (sampler && sampler->current_turn) {
            sampler->current_turn->diplo_steps.push_back(rec);
        }

        const char* stance_names[4] = {"ALLIED", "PEACE", "UNEASY", "WAR"};
        if (ml::RecordThoughts()) {
            if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                TilePos side_cap = side.OriginCapitalPos();
                TilePos other_cap = other.OriginCapitalPos();
                modern_player->PushThought(
                        AIThought{"Diplomacy", side.Name(), other.Name(), side_cap.x, side_cap.y,
                                  other_cap.x, other_cap.y, best_logit, 0.0F, false,
                                  absl::StrFormat("Stance -> %s (Attitude %+d, Trust %+d): %s",
                                                  stance_names[best_choice], att.total_score,
                                                  att.trust_score, att.stance_summary)});
            }
        }

        if (!other.IsComputerControlled() && other.Missives().empty() && game.Turn() > 1 &&
            game.Turn() - other.LastMissiveTurn(SideId(our_side)) >= 4) {
            bool send_missive = false;
            DiplomaticMissive missive;
            missive.from_side = our_side;
            missive.to_side = other_idx;
            missive.reason_summary = att.what_it_takes;

            if (best_choice == 0 && !cur_allied) {
                send_missive = true;
                missive.proposed_state = diplomacy::kPeace;
                missive.propose_alliance = true;
                missive.joint_war_target = diplomacy::WarTarget(game, our_side, other_idx);
                missive.headline = absl::StrFormat("Envoy from %s (%s)", side.Name(), prof.name);
                missive.body = absl::StrFormat(
                        "\"%s\" We propose a full Military Alliance with shared vision!",
                        prof.motto);
            } else if (best_choice == 2 && cur_st == diplomacy::kPeace &&
                       belief.their_border_strength[other_idx] > 6.0F) {
                send_missive = true;
                missive.proposed_state = diplomacy::kIntermediate;
                missive.headline =
                        absl::StrFormat("Border Warning from %s (%s)", side.Name(), prof.name);
                missive.body =
                        "Your armies mass along our frontier. Withdraw them or our peace shall "
                        "end!";
            } else if (best_choice == 2 && cur_st == diplomacy::kPeace && rival_vec[9] > 0.35F) {
                // Human-only flavour (never reached in all-AI training): demand tribute
                // when our border forces clearly outmatch theirs.
                send_missive = true;
                missive.proposed_state = diplomacy::kPeace;
                missive.gold_demand =
                        std::max(75, att.peace_gold_needed > 0 ? att.peace_gold_needed : 100);
                missive.headline =
                        absl::StrFormat("Tribute Demand from %s (%s)", side.Name(), prof.name);
                missive.body = absl::StrFormat(
                        "\"%s\" Pay %d gold in tribute to preserve the peace between us!",
                        prof.motto, missive.gold_demand);
            } else if (best_choice == 1 && cur_st == diplomacy::kWar) {
                send_missive = true;
                missive.proposed_state = diplomacy::kPeace;
                int offered = diplomacy::GoldOffer(game, our_side, other_idx);
                missive.gold_demand = -offered;
                missive.headline =
                        absl::StrFormat("Peace Envoy from %s (%s)", side.Name(), prof.name);
                missive.body = offered > 0 ? absl::StrFormat(
                                                     "We offer %d gold and a 10-turn truce to end "
                                                     "this war.",
                                                     offered)
                                           : "Let us lay down arms and sign a 10-turn truce.";
            }

            if (send_missive) {
                other.AddMissive(std::move(missive));
                other.SetLastMissiveTurn(SideId(our_side), game.Turn());
            }
        }
    }
}

}  // namespace w2::ai::modern
