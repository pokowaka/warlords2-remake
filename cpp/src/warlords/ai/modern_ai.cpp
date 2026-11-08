#include "warlords/ai/modern_ai.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
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

namespace {

/** @brief Category of mission objective scored by MissionNet. */
enum class TargetKind : int {
    kHoldGarrison = 0,   ///< Stay in our current city as garrison.
    kNeutralCity = 1,    ///< Capture a neutral city.
    kEnemyCity = 2,      ///< Siege an enemy city.
    kEnemyStack = 3,     ///< Intercept a visible enemy stack in the field.
    kRuinOrSage = 4,     ///< Search an open ruin, library, or sage site.
    kTempleOrItem = 5,   ///< Visit a temple for blessing or pick up a ground item.
    kFogFrontier = 6,    ///< Explore unseen tiles along the fog frontier.
    kReinforceCity = 7,  ///< March to reinforce another owned city.
};

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

/** @brief A candidate mission target considered during the military and exploration phase. */
struct CandidateTarget {
    TargetKind kind = TargetKind::kHoldGarrison;  ///< Objective category.
    int x = 0, y = 0;                             ///< Target tile coordinates.
    City* city = nullptr;                         ///< Target city, if any.
    Site* site = nullptr;                         ///< Target site, if any.
    Item* item = nullptr;                         ///< Target ground item, if any.
    int owner = kNone;                            ///< Owning side index, or kNone.
    float value = 0.0F;                           ///< Base objective value.
    bool is_quest_or_own_cap = false;             ///< Whether this is our quest target or capital.
    bool is_enemy_capital = false;                ///< Whether this is an enemy side's capital.
    bool is_water = false;                        ///< Whether the target tile is on water.
    float enemy_threat = 0.0F;         ///< Nearby hostile strength threatening the target.
    float nearby_friendly_str = 0.0F;  ///< Friendly mobile strength within 14 tiles.
    int assigned_stacks = 0;           ///< Stacks already assigned to this target.
    float assigned_strength = 0.0F;    ///< Combined strength already assigned here.
    std::string label;                 ///< Human-readable description for AIThoughts.
};

/**
 * @brief Builds the 16-dimensional feature vector describing `stack` for MissionNet and
 * StackValueNet.
 */
ml::StackVec BuildStackFeatures(const Game& game, const std::vector<Army*>& stack,
                                int prev_min_move, float origin_rem_guard_str = 10.0F,
                                float origin_threat = 0.0F) {
    ml::StackVec features{};
    if (stack.empty()) return features;
    int total_str = 0, min_move = 99, total_upkeep = 0, blessed = 0;
    bool has_hero = false, has_flier = false, all_fly = true, has_siege = false,
         has_special = false;
    for (Army* army : stack) {
        total_str += army->strength;
        min_move = std::min(min_move, army->moves);
        total_upkeep += army->upkeep;
        if (!army->blessings.empty()) blessed++;
        if (army->Hero()) has_hero = true;
        const ArmyType* army_type = game.types.ById(army->type);
        if (army_type) {
            if (army_type->flies)
                has_flier = true;
            else
                all_fly = false;
            if (army_type->siege) has_siege = true;
            if (army_type->bonus[48] != 0 || army_type->bonus[52] != 0) has_special = true;
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
        int best = core::kUnreached;
        for (int dy = -1; dy <= 2; dy++) {
            for (int dx = -1; dx <= 2; dx++) {
                int tile_x = tgt.city->x + dx, tile_y = tgt.city->y + dy;
                if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width &&
                    tile_y < game.map->height) {
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
 * @brief Builds the 21-dimensional feature vector describing mission target `tgt` for `stack`.
 */
ml::TargetVec BuildTargetFeatures(Game& game, const Side& side, const std::vector<Army*>& stack,
                                  const CandidateTarget& tgt, float nearby_friendly_str,
                                  float caution_bias, int path_cost) {
    ml::TargetVec features{};
    features[static_cast<int>(tgt.kind)] = 1.0F;

    int start_x = stack[0]->x, start_y = stack[0]->y;
    int min_move = std::max(2, move::StackMoves(stack));
    int eff_cost =
            path_cost >= 0 ? path_cost : (move::Distance(start_x, start_y, tgt.x, tgt.y) * 2);
    float turns_to_reach = static_cast<float>(eff_cost) / static_cast<float>(std::max(4, min_move));

    float win_now = 0.0F, win_converged = 0.0F, surv_ratio = 0.0F;
    float seen_fraction = 1.0F;
    bool is_combat = (tgt.kind == TargetKind::kNeutralCity || tgt.kind == TargetKind::kEnemyCity ||
                      tgt.kind == TargetKind::kEnemyStack);
    if (is_combat) {
        auto est = belief::EstimateCombat(game, side, stack, tgt.x, tgt.y, caution_bias);
        win_now = est.win_prob;
        surv_ratio = est.expected_survivors / static_cast<float>(std::max<size_t>(1, stack.size()));
        seen_fraction = est.seen_tile_fraction;

        float my_str = 0.0F;
        for (Army* army : stack) my_str += static_cast<float>(army->strength);
        int free_slots = std::max(0, rules::kMaxStack - static_cast<int>(stack.size()));
        float add_str = std::min(nearby_friendly_str + tgt.assigned_strength,
                                 static_cast<float>(free_slots) * 5.0F);
        if (add_str > 0.5F && est.estimated_defender_strength > 0.0F) {
            float combined_ratio =
                    (my_str + add_str) / (my_str + add_str + est.estimated_defender_strength);
            win_converged = std::clamp(std::max(win_now, combined_ratio), 0.0F, 1.0F);
        } else {
            win_converged = win_now;
        }
    }

    bool flies = (move::ModeOf(game, stack) == move::kFlying);
    bool at_sea = false;
    for (Army* army : stack) {
        if (army->at_sea) at_sea = true;
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
    return features;
}

/**
 * @brief Evaluates DiploNet against each rival and proposes stances, tribute, joint war, and
 * missives.
 */
void RunDiplomacyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       std::array<bool, 8>& decided) {
    if (game.map->options.diplomacy == 0) return;
    int our_side = side.index;
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
        if (side_idx != our_side && game.map->sides[side_idx].in_use &&
            game.map->sides[side_idx].alive &&
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
    for (Army* army : game.armies) {
        if (army->owner == our_side && army->x != kNone && !army->transit)
            total_my_mobile_str += static_cast<float>(army->strength);
    }

    auto my_cities_list = game::SideCities(game, side);
    std::array<float, 8> rival_min_dist;
    rival_min_dist.fill(120.0F);
    int closest_rival = kNone;
    float closest_rival_dist = 999.0F;
    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = game.map->sides[other_idx];
        if (other_idx == our_side || !other.in_use || !other.alive) continue;
        if (belief.visible_cities_by_side[other_idx] == 0 &&
            belief.their_border_strength[other_idx] == 0.0F)
            continue;
        float min_dist = 120.0F;
        for (City* my_city : my_cities_list) {
            for (City* vis_city : belief.visible_cities) {
                if (vis_city->owner_index == other_idx) {
                    min_dist = std::min(
                            min_dist, static_cast<float>(move::Distance(my_city->x, my_city->y,
                                                                        vis_city->x, vis_city->y)));
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

    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = game.map->sides[other_idx];
        if (other_idx == our_side || !other.in_use || !other.alive || decided[other_idx]) continue;
        int cur_st = diplomacy::State(game, our_side, other_idx);
        if (game.map->options.hidden_map != 0 && belief.visible_cities_by_side[other_idx] == 0 &&
            belief.their_border_strength[other_idx] == 0.0F && cur_st != diplomacy::kWar) {
            continue;
        }
        decided[other_idx] = true;

        auto att = diplomacy::ComputeAttitude(game, our_side, other_idx);
        bool cur_allied = diplomacy::IsAllied(game, our_side, other_idx);
        float min_dist = rival_min_dist[other_idx];

        int shared_wars = 0;
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            if (side_idx != our_side && side_idx != other_idx && game.map->sides[side_idx].alive &&
                diplomacy::State(game, our_side, side_idx) == diplomacy::kWar &&
                diplomacy::State(game, other_idx, side_idx) == diplomacy::kWar) {
                shared_wars++;
            }
        }

        int grudge = 0;
        if (side.ai) {
            grudge = side.ai->cities_lost[other_idx] * 8 + side.ai->heroes_killed[other_idx] * 6 +
                     side.ai->lost[other_idx] * 2;
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
        rival_vec[14] = (side.capital && side.capital->owner_index == other_idx) ? 1.0F : 0.0F;
        rival_vec[15] = std::tanh(
                static_cast<float>(diplomacy::GoldOffer(game, other_idx, our_side)) / 200.0F);
        rival_vec[16] = std::tanh(min_dist / 45.0F);
        rival_vec[17] = std::tanh(static_cast<float>(other_wars) / 3.0F);

        ml::DiploInVec d_in = ml::PackDiploInput(ctx, rival_vec);
        auto raw = policy.diplo_net.Forward(d_in);
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
        if (friendly && cur_st == diplomacy::kWar && side.gold >= 100) {
            rec.has_tribute = true;
            rec.tribute_chosen = ml::SelectBinary(raw[4], sampler, &rec.old_tribute_log_prob);
            if (rec.tribute_chosen) {
                int offer = std::clamp(side.gold / 4 / 25 * 25, 50, std::min(300, side.gold / 2));
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
        side.ai_thoughts.push_back(AIThought{
            "Diplomacy", side.name, other.name, side.cap_x, side.cap_y, other.cap_x, other.cap_y,
            best_logit, 0.0F, false,
            Fmt("Stance -> %s (Attitude %+d, Trust %+d): %s", stance_names[best_choice],
                att.total_score, att.trust_score, att.stance_summary.c_str())});

        if (!other.computer && other.missives.empty() && game.turn > 1 &&
            game.turn - other.last_missive_turn[our_side] >= 4) {
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
                missive.headline = Fmt("Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
                missive.body = Fmt("\"%s\" We propose a full Military Alliance with shared vision!",
                                   prof.motto.c_str());
            } else if (best_choice == 2 && cur_st == diplomacy::kPeace &&
                       belief.their_border_strength[other_idx] > 6.0F) {
                send_missive = true;
                missive.proposed_state = diplomacy::kIntermediate;
                missive.headline =
                        Fmt("Border Warning from %s (%s)", side.name.c_str(), prof.name.c_str());
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
                        Fmt("Tribute Demand from %s (%s)", side.name.c_str(), prof.name.c_str());
                missive.body =
                        Fmt("\"%s\" Pay %d gold in tribute to preserve the peace between us!",
                            prof.motto.c_str(), missive.gold_demand);
            } else if (best_choice == 1 && cur_st == diplomacy::kWar) {
                send_missive = true;
                missive.proposed_state = diplomacy::kPeace;
                int offered = diplomacy::GoldOffer(game, our_side, other_idx);
                missive.gold_demand = -offered;
                missive.headline =
                        Fmt("Peace Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
                missive.body = offered > 0 ? Fmt("We offer %d gold and a 10-turn truce to end this "
                                                 "war.",
                                                 offered)
                                           : "Let us lay down arms and sign a 10-turn truce.";
            }

            if (send_missive) {
                other.missives.push_back(std::move(missive));
                other.last_missive_turn[our_side] = game.turn;
            }
        }
    }
}

/**
 * @brief Uses EconNet to choose whether to occupy, pillage, sack, or raze a newly captured `city`.
 */
void DecideCaptureSpoils(Game& game, Side& side, City& city, const std::vector<Army*>& attackers,
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx,
                         ml::RolloutSampler* sampler) {
    if (city.razed || city.owner_index != side.index) return;
    if (ai::QuestCapture(game, side, city, attackers)) return;

    const SideBelief& belief = belief::Ensure(side);
    float local_threat = city.index < static_cast<int>(belief.city_threat1_turn.size())
                                 ? belief.city_threat1_turn[city.index]
                                 : 0.0F;
    float dist_enemy = city.index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city.index]
                               : 60.0F;
    int sack_gold = ai::groups::SackValue(game, city);
    int pillage_gold = city.slots.size() >= 2
                               ? std::abs(game.types.ById(city.slots.back().type)->price) / 2
                               : 0;

    auto make_spoil_in = [&](float ongoing_income, float instant_gold, float diplo_pen) {
        ml::EconVec econ_vec{};
        econ_vec[6] = 1.0F;
        econ_vec[7] = ongoing_income / 35.0F;
        econ_vec[14] = std::tanh(local_threat / 25.0F);
        econ_vec[15] = std::tanh(dist_enemy / 50.0F);
        econ_vec[18] = std::tanh(instant_gold / 250.0F);
        econ_vec[19] = diplo_pen / 40.0F;
        return ml::PackEconInput(ctx, econ_vec);
    };

    bool is_any_capital = false;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        if (game.map->sides[side_idx].capital == &city) is_any_capital = true;
    }

    std::vector<int> modes = {0};
    std::vector<ml::EconInVec> cand_inputs = {
        make_spoil_in(static_cast<float>(city.income), 0.0F, 0.0F)};
    if (pillage_gold > 0) {
        modes.push_back(1);
        cand_inputs.push_back(make_spoil_in(static_cast<float>(city.income) * 0.8F,
                                            static_cast<float>(pillage_gold), 4.0F));
    }
    if (sack_gold > 0) {
        modes.push_back(2);
        cand_inputs.push_back(make_spoil_in(static_cast<float>(city.income) * 0.5F,
                                            static_cast<float>(sack_gold), 15.0F));
    }
    if (!is_any_capital) {
        modes.push_back(3);
        cand_inputs.push_back(make_spoil_in(0.0F, 0.0F, 38.0F));
    }

    std::vector<float> scores(cand_inputs.size());
    for (size_t i = 0; i < cand_inputs.size(); i++) {
        scores[i] = policy.econ_net.Forward(cand_inputs[i])[0];
    }
    float log_prob = 0.0F;
    int pick = ml::SelectFromLogits(scores, sampler, &log_prob);
    if (sampler && sampler->current_turn && cand_inputs.size() > 1) {
        sampler->current_turn->econ_steps.push_back(
                ml::EconStepRecord{std::move(cand_inputs), {}, pick, log_prob});
    }

    int best_mode = modes[pick];
    if (best_mode == 1) {
        game::Pillage(game, side, city, attackers);
        if (ai::hooks.on_spoils) ai::hooks.on_spoils(game, side, city, "pillaged");
    } else if (best_mode == 2) {
        game::Sack(game, side, city, attackers);
        if (ai::hooks.on_spoils) ai::hooks.on_spoils(game, side, city, "sacked");
    } else if (best_mode == 3) {
        game::Raze(game, side, city, attackers);
        if (ai::hooks.on_spoils) ai::hooks.on_spoils(game, side, city, "razed");
    } else {
        game::Occupy(game, side, city, attackers);
    }
}

/**
 * @brief Moves `stack` along its path toward `tgt`, fighting blockers or the target and searching
 * sites.
 */
bool ExecuteStackMission(Game& game, Side& side, std::vector<Army*>& stack, CandidateTarget& tgt,
                         bool stage_near, const ml::PolicyNetwork& policy,
                         const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [&](Army* army) { return !game.Alive(army) || army->moves <= 0; }),
                stack.end());
    if (stack.empty()) return false;

    if (tgt.kind == TargetKind::kHoldGarrison) {
        for (Army* army : stack) army->ai_moved = true;
        return true;
    }

    int our_side = side.index;
    std::map<Army*, int> start_moves;
    for (Army* army : stack) start_moves[army] = army->moves;

    // A coalition drawn from one owned 2x2 city may stand on different corners.
    // Members walk (by the normal rules, paying moves) onto the leader's tile;
    // any that cannot join without breaking the 8-army stack limit stay behind
    // and remain free for later waves.
    {
        int leader_x = stack[0]->x, leader_y = stack[0]->y;
        int on_leader_tile = static_cast<int>(game::ArmiesAt(game, leader_x, leader_y).size());
        std::vector<Army*> joined;
        for (Army* army : stack) {
            if (army->x == leader_x && army->y == leader_y) {
                joined.push_back(army);
                continue;
            }
            if (on_leader_tile + 1 > rules::kMaxStack) continue;
            auto path = move::FindPath(game, {army}, army->x, army->y, leader_x, leader_y);
            if (!path || path->empty()) continue;
            move::WalkResult join_result = move::Walk(game, {army}, *path);
            if (game.Alive(army) && army->x == leader_x && army->y == leader_y &&
                join_result.steps > 0) {
                ai::Walked(game, {army}, join_result);
                joined.push_back(army);
                on_leader_tile++;
            }
        }
        stack = std::move(joined);
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [&](Army* army) { return army->moves <= 0; }),
                    stack.end());
        if (stack.empty()) return false;
    }

    int dest_x = tgt.x, dest_y = tgt.y;
    if (tgt.city && !stage_near) {
        if (tgt.city->owner_index == our_side) {
            for (int oy = 0; oy <= 1; oy++) {
                for (int ox = 0; ox <= 1; ox++) {
                    int tile_x = tgt.city->x + ox, tile_y = tgt.city->y + oy;
                    if (static_cast<int>(game::ArmiesAt(game, tile_x, tile_y).size() +
                                         stack.size()) <= rules::kMaxStack) {
                        dest_x = tile_x;
                        dest_y = tile_y;
                        break;
                    }
                }
            }
        } else {
            dest_x = tgt.city->x + (tgt.city->x < stack[0]->x ? 1 : 0);
            dest_y = tgt.city->y + (tgt.city->y < stack[0]->y ? 1 : 0);
        }
    }

    if (stage_near && (tgt.kind == TargetKind::kEnemyCity || tgt.kind == TargetKind::kNeutralCity ||
                       tgt.kind == TargetKind::kEnemyStack)) {
        int best_sx = dest_x, best_sy = dest_y;
        int best_dist = 999;
        int sx0 = stack[0]->x, sy0 = stack[0]->y;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int rad = std::max(std::abs(dx), std::abs(dy));
                if (rad < 1 || rad > 2) continue;
                int next_x = tgt.x + dx, next_y = tgt.y + dy;
                if (next_x < 0 || next_y < 0 || next_x >= game.map->width ||
                    next_y >= game.map->height)
                    continue;
                if (tgt.city && game::CityAt(game, next_x, next_y) == tgt.city) continue;
                if (!belief::IsTileVisible(game, our_side, next_x, next_y)) continue;
                int terr =
                        game.map->terrain_type[game.map->tiles[next_y * game.map->width + next_x] &
                                               0xff];
                if (terr == move::kWater || terr == move::kShore || terr == move::kMountains)
                    continue;
                auto occ = game::ArmiesAt(game, next_x, next_y);
                if (!occ.empty() &&
                    (occ[0]->owner != our_side ||
                     static_cast<int>(occ.size() + stack.size()) > rules::kMaxStack)) {
                    continue;
                }
                int dist = move::Distance(sx0, sy0, next_x, next_y) - (!occ.empty() ? 15 : 0);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_sx = next_x;
                    best_sy = next_y;
                }
            }
        }
        dest_x = best_sx;
        dest_y = best_sy;
    }

    core::Sel sel = core::Select(game, stack);
    if (sel.armies.empty() || !sel.leader) return false;

    bool is_fog_mission = (tgt.kind == TargetKind::kFogFrontier);
    bool made_progress = false;
    bool completed_objective = false;

    for (int pass = 0; pass < 24 && !sel.armies.empty() && sel.min_moves > 0; pass++) {
        int leader_x = sel.leader->x, leader_y = sel.leader->y;
        if (leader_x == dest_x && leader_y == dest_y) {
            if (is_fog_mission) completed_objective = true;
            break;
        }

        auto path = move::FindPath(game, sel.armies, leader_x, leader_y, dest_x, dest_y);
        if (!path || path->empty()) break;

        std::vector<move::Step> walk_steps = *path;
        if (is_fog_mission && walk_steps.size() > 6) {
            walk_steps.resize(6);
        }

        int seen_box_before = 0;
        if (is_fog_mission && game.map->options.hidden_map != 0) {
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int tile_x = leader_x + ox, tile_y = leader_y + oy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width &&
                        tile_y < game.map->height &&
                        belief::IsTileVisible(game, our_side, tile_x, tile_y)) {
                        seen_box_before++;
                    }
                }
            }
        }

        move::WalkResult walk_result = move::Walk(game, sel.armies, walk_steps);
        if (walk_result.steps > 0) {
            made_progress = true;
            ai::Walked(game, sel.armies, walk_result);
        }
        core::Refresh(game, sel);
        core::PickUp(game, sel);

        int attack_x = kNone, attack_y = kNone;
        City* att_city = nullptr;
        if (walk_result.stopped == "attack" && walk_result.attack && !stage_near) {
            attack_x = walk_result.attack->x;
            attack_y = walk_result.attack->y;
            att_city = walk_result.attack->city;
        } else if (walk_result.stopped == "at peace" && !stage_near &&
                   walk_result.steps < static_cast<int>(walk_steps.size())) {
            const move::Step& ahead = walk_steps[walk_result.steps];
            attack_x = ahead.x;
            attack_y = ahead.y;
            att_city = game::CityAt(game, attack_x, attack_y);
        }

        if (attack_x != kNone && attack_y != kNone) {
            auto def_armies = game::ArmiesAt(game, attack_x, attack_y);
            int def_owner = def_armies.empty() ? (att_city ? att_city->owner_index : kNone)
                                               : def_armies[0]->owner;

            // Breaking a treaty is DiploNet's call, not the mission's: an attack that
            // needs a declaration (the game's own mustDeclare rule) only goes ahead
            // when this side is no longer proposing peace to the defender.
            if (diplomacy::MustDeclare(game, our_side, def_owner, att_city)) {
                if (diplomacy::Proposal(game, our_side, def_owner) == diplomacy::kPeace) break;
                diplomacy::AttackAnyway(game, our_side, def_owner);
            }

            // Gather friendly units on the launching tile so converged allies fight as one combined
            // force
            std::vector<Army*> combined_here =
                    core::Collect(game, our_side, sel.leader->x, sel.leader->y, 1);
            if (combined_here.empty()) combined_here = sel.armies;

            core::Sel comb_sel = core::Select(game, combined_here);
            int prev_owner = att_city ? att_city->owner_index : kNone;
            Battle b_res = core::Fight(game, comb_sel, attack_x, attack_y);
            made_progress = true;
            belief::RecordBattleObservation(game, our_side, attack_x, attack_y);
            if (att_city && att_city->owner_index == our_side && prev_owner != our_side &&
                !comb_sel.armies.empty()) {
                DecideCaptureSpoils(game, side, *att_city, comb_sel.armies, policy, ctx, sampler);
            }
            sel = comb_sel;

            if (!b_res.won || sel.armies.empty()) {
                break;
            }
            if (tgt.city && tgt.city->owner_index == our_side) {
                completed_objective = true;
                break;
            }
            if (tgt.kind == TargetKind::kEnemyStack && attack_x == tgt.x && attack_y == tgt.y) {
                completed_objective = true;
                break;
            }
            // Won an intermediate road/blocker battle and still have moves: keep advancing toward
            // dest!
            continue;
        }

        if (is_fog_mission && walk_result.steps > 0 && game.map->options.hidden_map != 0) {
            int seen_box_after = 0;
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int tile_x = leader_x + ox, tile_y = leader_y + oy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width &&
                        tile_y < game.map->height &&
                        belief::IsTileVisible(game, our_side, tile_x, tile_y)) {
                        seen_box_after++;
                    }
                }
            }
            if (seen_box_after > seen_box_before ||
                (sel.leader->x == dest_x && sel.leader->y == dest_y)) {
                // Yield back to the neural wave loop as soon as fog movement reveals new map tiles,
                // allowing MissionNet itself to evaluate newly uncovered castles or sites!
                completed_objective = true;
                break;
            }
        }

        if (walk_result.steps == 0 || walk_result.stopped == "out of moves" ||
            walk_result.stopped == "blocked" || walk_result.stopped == "at peace") {
            if (stage_near && walk_result.steps > 0) {
                // Arrived at or advanced toward staging position
                made_progress = true;
            }
            break;
        }
    }

    // Interact with open temple or ruin/sage if standing on one with moves left
    if (!sel.armies.empty() && sel.leader && sel.min_moves > 0) {
        int final_x = sel.leader->x, final_y = sel.leader->y;
        Site* site = core::SiteAt(game, final_x, final_y);
        if (site && core::SiteOpen(*site)) {
            if (site->content == site::kTemple) {
                site::Search(game, sel.armies, final_x, final_y, false);
                made_progress = true;
                completed_objective = true;
                if (game.map->options.quests != 0 && sel.hero &&
                    (!side.quest || !side.quest->HasTarget())) {
                    quest::Assign(game, side, sel.hero, true, core::Flies(game, sel.hero));
                }
            } else if (sel.hero && game.Alive(sel.hero)) {
                site::Search(game, sel.armies, final_x, final_y, false);
                made_progress = true;
                completed_objective = true;
            }
        }
        core::PickUp(game, sel);
    }

    for (Army* army : sel.armies) {
        if (!game.Alive(army)) continue;
        auto found = start_moves.find(army);
        bool spent_moves = (found != start_moves.end() && army->moves < found->second);
        if (completed_objective && spent_moves && army->moves >= 2 && !stage_near &&
            tgt.kind != TargetKind::kReinforceCity) {
            // Allow remaining movement points to be dispatched in the next wave of this turn
            army->ai_moved = false;
        } else {
            army->ai_moved = made_progress;
        }
    }
    return made_progress;
}

/**
 * @brief Returns the distinct candidate stack sizes to consider for a group led by `seed` with
 * `escorts`.
 */
std::vector<size_t> BuildCoalitionSizes(const Game& game, Army* seed,
                                        const std::vector<Army*>& escorts) {
    std::vector<size_t> sizes = {1};
    auto add_unique = [&](size_t count) {
        if (count >= 1 && count <= escorts.size() + 1 &&
            std::find(sizes.begin(), sizes.end(), count) == sizes.end()) {
            sizes.push_back(count);
        }
    };
    if (!escorts.empty()) {
        add_unique(2);
        add_unique(4);
        add_unique(escorts.size());
        add_unique(escorts.size() + 1);

        // Also include fast/flying-matching prefix size if distinct
        bool seed_flies = core::Flies(game, seed);
        size_t fast_count = 1;
        for (Army* escort : escorts) {
            if (seed_flies ? core::Flies(game, escort) : (escort->moves >= 12)) {
                fast_count++;
            } else {
                break;
            }
        }
        add_unique(fast_count);
    }
    return sizes;
}

/**
 * @brief Forms army coalitions at each location, scores mission options with MissionNet, and
 * executes the chosen moves.
 */
void RunMilitaryAndExplorationPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx, const PauseCallback& pause,
                                    ml::RolloutSampler* sampler,
                                    std::vector<MissionSnapshot>* snapshots = nullptr) {
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

/**
 * @brief Uses EconNet to decide unit retention/disbanding, empire-wide production slot purchases,
 * city production, and vectoring.
 */
void RunEconomyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                     const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    int our_side = side.index;
    const SideBelief& belief = belief::Ensure(side);

    auto make_hold_gold_in = [&](const ml::ContextVec& cur_ctx) {
        ml::EconVec econ_vec{};
        econ_vec[0] = 1.0F;
        econ_vec[10] = std::tanh(
                static_cast<float>(game::Upkeep(game, side) - game::Income(game, side)) / 40.0F);
        return ml::PackEconInput(cur_ctx, econ_vec);
    };

    // Army retention vs disbanding: only offered when upkeep exceeds income, the
    // one situation where the rules make disbanding worth anything. Keeping an
    // irreversible action off the table otherwise stops an untrained policy from
    // dissolving its own army during early training.
    std::vector<Army*> my_armies = game::SideArmies(game, side);
    bool over_budget = game::Upkeep(game, side) > game::Income(game, side);
    if (over_budget && my_armies.size() > 3) {
        for (Army* army : my_armies) {
            if (!game.Alive(army) || army->Hero() || army->transit || army->x == kNone) continue;
            City* city = game::CityAt(game, army->x, army->y);
            if (!city || city->owner_index != our_side) continue;
            int city_armies = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    city_armies += static_cast<int>(
                            game::ArmiesAt(game, city->x + dx, city->y + dy).size());
                }
            }
            if (city_armies <= 2) continue;

            const ArmyType* army_type = game.types.ById(army->type);
            float threat = (city->index < static_cast<int>(belief.city_threat2_turn.size()))
                                   ? belief.city_threat2_turn[city->index]
                                   : 0.0F;
            float dist_e = (city->index < static_cast<int>(belief.city_nearest_enemy_dist.size()))
                                   ? belief.city_nearest_enemy_dist[city->index]
                                   : 100.0F;

            ml::ContextVec cur_ctx = ml::BuildContext(game, side);

            ml::EconVec econ_vec{};
            econ_vec[5] = 1.0F;
            econ_vec[7] = static_cast<float>(army->strength) / 9.0F;
            econ_vec[8] = static_cast<float>(army->max_moves) / 24.0F;
            econ_vec[10] = static_cast<float>(army->upkeep) / 15.0F;
            econ_vec[11] = (army_type && army_type->flies) ? 1.0F : 0.0F;
            econ_vec[12] =
                    (army_type && (army_type->siege || army_type->bonus[48] != 0)) ? 1.0F : 0.0F;
            econ_vec[13] =
                    (army->max_moves >= 14 && (!army_type || !army_type->flies)) ? 1.0F : 0.0F;
            econ_vec[14] = std::tanh(threat / 25.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[ml::kEconCurrent] = 1.0F;  // keeping the army is the status quo
            ml::EconInVec keep_in = ml::PackEconInput(cur_ctx, econ_vec);

            ml::EconVec disband_vec = econ_vec;
            disband_vec[10] = -econ_vec[10];
            disband_vec[ml::kEconCurrent] = 0.0F;
            ml::EconInVec disband_in = ml::PackEconInput(cur_ctx, disband_vec);

            std::vector<ml::EconInVec> cands = {keep_in, disband_in};
            std::vector<float> logits = {policy.econ_net.Forward(keep_in)[0],
                                         policy.econ_net.Forward(disband_in)[0]};
            float log_prob = 0.0F;
            int pick = ml::SelectFromLogits(logits, sampler, &log_prob);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->econ_steps.push_back(
                        ml::EconStepRecord{std::move(cands), {}, pick, log_prob});
            }
            if (pick == 1) {
                game::Disband(game, side, {army});
            }
        }
    }

    std::vector<City*> own_cities = game::SideCities(game, side);

    // Empire-wide slot upgrade rounds (up to 3 purchases/turn): each round asks EconNet
    // to compare HoldGold against every legal (city, unit) upgrade, stopping immediately
    // as soon as EconNet chooses HoldGold so multi-city empires neither hoard 10k gold
    // nor suffer 1 - p^N treasury-draining coin-flip cascades across N cities.
    auto menu = game::BuyableTypes(game);
    for (int buy_round = 0; buy_round < 3 && !own_cities.empty() && side.gold >= 250; buy_round++) {
        /** @brief A candidate production-slot upgrade in one city, or HoldGold when city is null.
         */
        struct BuyOption {
            City* city = nullptr;    ///< City to buy the slot in, or nullptr for HoldGold.
            int type_id = kNone;     ///< ArmyType ID to purchase, or kNone.
            ml::EconInVec in_vec{};  ///< Packed EconNet input.
        };
        ml::ContextVec buy_ctx = ml::BuildContext(game, side);
        std::vector<BuyOption> buy_opts;
        buy_opts.push_back(BuyOption{nullptr, kNone, make_hold_gold_in(buy_ctx)});

        for (City* city : own_cities) {
            bool full_slots = static_cast<int>(city->slots.size()) >= rules::kProductionSlots;
            int replace_price = (!city->slots.empty()) ? city->slots[0].price : 0;
            int best_existing_str = 0;
            for (const auto& slot : city->slots)
                best_existing_str = std::max(best_existing_str, slot.strength);
            float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[city->index]
                                   : 0.0F;
            float dist_e = city->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[city->index]
                                   : 100.0F;
            for (const ArmyType* army_type : menu) {
                int price = std::abs(army_type->price);
                if (price <= 0 || price + 100 > side.gold) continue;
                if (full_slots && price <= replace_price) continue;
                if (!game::CannotBuy(game, side, *city, *army_type).empty()) continue;

                ml::EconVec econ_vec{};
                econ_vec[3] = 1.0F;
                econ_vec[7] = static_cast<float>(army_type->strength) / 9.0F;
                econ_vec[8] = static_cast<float>(army_type->move) / 24.0F;
                econ_vec[9] = static_cast<float>(army_type->time) / 6.0F;
                econ_vec[10] = static_cast<float>(price) /
                               std::max(100.0F, static_cast<float>(side.gold - 80));
                econ_vec[11] = army_type->flies ? 1.0F : 0.0F;
                econ_vec[12] = (army_type->siege || army_type->bonus[48] != 0) ? 1.0F : 0.0F;
                econ_vec[13] = (army_type->move >= 14 && !army_type->flies) ? 1.0F : 0.0F;
                econ_vec[14] = std::tanh(threat / 25.0F);
                econ_vec[15] = std::tanh(dist_e / 60.0F);
                econ_vec[16] = static_cast<float>(city->slots.size()) /
                               static_cast<float>(rules::kProductionSlots);
                econ_vec[17] = static_cast<float>(army_type->strength - best_existing_str) / 9.0F;
                econ_vec[18] = static_cast<float>(army_type->strength * army_type->strength) /
                               static_cast<float>(std::max(1, army_type->time * 16));
                buy_opts.push_back(
                        BuyOption{city, army_type->id, ml::PackEconInput(buy_ctx, econ_vec)});
            }
        }

        if (buy_opts.size() <= 1) break;
        std::vector<float> logits(buy_opts.size());
        std::vector<float> log_mult(buy_opts.size(), 0.0F);
        for (size_t i = 0; i < buy_opts.size(); i++) {
            logits[i] = policy.econ_net.Forward(buy_opts[i].in_vec)[0];
            if (i > 0 && buy_opts.size() > 2)
                log_mult[i] = std::log(static_cast<float>(buy_opts.size() - 1));
        }
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
        if (sampler && sampler->current_turn) {
            std::vector<ml::EconInVec> cands;
            cands.reserve(buy_opts.size());
            for (const auto& buy_opt : buy_opts) cands.push_back(buy_opt.in_vec);
            sampler->current_turn->econ_steps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
        }
        if (pick == 0 || !buy_opts[pick].city) break;

        City* best_buy_city = buy_opts[pick].city;
        int bought_type = buy_opts[pick].type_id;
        int slot_idx = game::BuySlot(*best_buy_city);
        game::BuyProduction(game, side, *best_buy_city, slot_idx, bought_type);
        game::SortProduction(*best_buy_city);
        for (int si = 0; si < static_cast<int>(best_buy_city->slots.size()); si++) {
            if (best_buy_city->slots[si].type == bought_type) {
                game::SetProduction(game, *best_buy_city, si);
                break;
            }
        }
    }

    // City production macro-action: queried whenever a city has no active build
    // or has just completed a build (`city->countdown == slot.time`, matching
    // game.cpp:1331), so multi-turn unit builds complete without mid-countdown
    // cancellation while cities remain free to switch units after each build.
    ml::ContextVec prod_ctx = ml::BuildContext(game, side);
    for (City* city : own_cities) {
        bool mid_countdown =
                (city->producing >= 0 && city->producing < static_cast<int>(city->slots.size()) &&
                 city->countdown > 0 && city->countdown < city->slots[city->producing].time);
        if (city->slots.empty() || mid_countdown) continue;
        float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->index]
                               : 0.0F;
        float dist_e = city->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city->index]
                               : 100.0F;
        int gar_count = 0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                gar_count +=
                        static_cast<int>(game::ArmiesAt(game, city->x + dx, city->y + dy).size());
            }
        }

        ml::EconVec stop_vec{};
        stop_vec[2] = 1.0F;
        stop_vec[10] = std::tanh(
                static_cast<float>(game::Upkeep(game, side) - game::Income(game, side)) / 30.0F);
        stop_vec[14] = std::tanh(threat / 25.0F);
        stop_vec[15] = std::tanh(dist_e / 60.0F);
        stop_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
        stop_vec[ml::kEconCurrent] = (city->producing == kNone) ? 1.0F : 0.0F;

        std::vector<int> slot_indices = {kNone};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, stop_vec)};
        for (size_t si = 0; si < city->slots.size(); si++) {
            const Slot& slot = city->slots[si];
            const ArmyType* army_type = game.types.ById(slot.type);
            bool is_flier = army_type && army_type->flies;
            bool is_siege_or_spec = army_type && (army_type->siege || army_type->bonus[48] != 0 ||
                                                  army_type->bonus[52] != 0);
            bool is_cav = slot.move >= 14 && !is_flier;

            ml::EconVec econ_vec{};
            econ_vec[1] = 1.0F;
            econ_vec[7] = static_cast<float>(slot.strength) / 9.0F;
            econ_vec[8] = static_cast<float>(slot.move) / 24.0F;
            econ_vec[9] = static_cast<float>(slot.time) / 6.0F;
            econ_vec[10] = static_cast<float>(slot.cost) / 20.0F;
            econ_vec[11] = is_flier ? 1.0F : 0.0F;
            econ_vec[12] = is_siege_or_spec ? 1.0F : 0.0F;
            econ_vec[13] = is_cav ? 1.0F : 0.0F;
            econ_vec[14] = std::tanh(threat / 25.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
            econ_vec[18] = static_cast<float>(slot.strength * slot.strength) /
                           static_cast<float>(std::max(1, slot.time * 16));
            econ_vec[ml::kEconCurrent] = (city->producing == static_cast<int>(si)) ? 1.0F : 0.0F;
            slot_indices.push_back(static_cast<int>(si));
            cands.push_back(ml::PackEconInput(prod_ctx, econ_vec));
        }

        if (cands.size() == 1) {
            game::SetProduction(game, *city, kNone);
            continue;
        }

        std::vector<float> logits(cands.size());
        std::vector<float> log_mult(cands.size(), 0.0F);
        for (size_t i = 0; i < cands.size(); i++) {
            logits[i] = policy.econ_net.Forward(cands[i])[0];
            if (i > 0 && cands.size() > 2)
                log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
        }
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
        if (sampler && sampler->current_turn) {
            sampler->current_turn->econ_steps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
        }

        if (pick == 0) {
            game::SetProduction(game, *city, kNone);
        } else if (city->producing != slot_indices[pick] || !game::Producing(*city)) {
            game::SetProduction(game, *city, slot_indices[pick]);
        }
    }

    // City vectoring: re-evaluated each turn, but the vector already set is a
    // flagged candidate (ECON_CURRENT) rather than wiped, so keeping it is a choice
    for (City* src : own_cities) {
        if (!game::Producing(*src) || src->producing < 0 ||
            src->producing >= static_cast<int>(src->slots.size())) {
            src->vector_to = kNone;
            continue;
        }
        const Slot& slot = src->slots[src->producing];
        float src_threat = src->index < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[src->index]
                                   : 0.0F;
        float src_dist_e = src->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[src->index]
                                   : 100.0F;

        ml::EconVec keep_here{};
        keep_here[4] = 1.0F;
        keep_here[14] = std::tanh(src_threat / 25.0F);
        keep_here[15] = std::tanh(src_dist_e / 60.0F);
        keep_here[17] = 0.0F;
        keep_here[ml::kEconCurrent] = src->vector_to == kNone ? 1.0F : 0.0F;

        std::vector<City*> dest_cities = {nullptr};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, keep_here)};

        for (City* dst : own_cities) {
            if (dst == src) continue;
            bool is_current = (src->vector_to == dst->index);
            if (!is_current &&
                static_cast<int>(game::VectoredTo(game, dst).size()) >= game::kMaxVectoredTo)
                continue;
            int v_turns = std::max(1, move::Distance(src->x, src->y, dst->x, dst->y) / 14 + 1);
            if (v_turns > 4) continue;
            float dst_threat = dst->index < static_cast<int>(belief.city_threat2_turn.size())
                                       ? belief.city_threat2_turn[dst->index]
                                       : 0.0F;
            float dst_dist_e = dst->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                       ? belief.city_nearest_enemy_dist[dst->index]
                                       : 100.0F;
            float threat_gain =
                    (dst_threat - src_threat) / 25.0F + (src_dist_e - dst_dist_e) / 45.0F;

            ml::EconVec vec_econ{};
            vec_econ[4] = 1.0F;
            vec_econ[7] = static_cast<float>(slot.strength) / 9.0F;
            vec_econ[8] = static_cast<float>(slot.move) / 24.0F;
            vec_econ[9] = static_cast<float>(v_turns) / 4.0F;
            vec_econ[14] = std::tanh(dst_threat / 25.0F);
            vec_econ[15] = std::tanh(dst_dist_e / 60.0F);
            vec_econ[17] = std::tanh(threat_gain);
            vec_econ[ml::kEconCurrent] = is_current ? 1.0F : 0.0F;
            dest_cities.push_back(dst);
            cands.push_back(ml::PackEconInput(prod_ctx, vec_econ));
        }

        if (cands.size() > 1) {
            std::vector<float> logits(cands.size());
            std::vector<float> log_mult(cands.size(), 0.0F);
            for (size_t i = 0; i < cands.size(); i++) {
                logits[i] = policy.econ_net.Forward(cands[i])[0];
                if (i > 0 && cands.size() > 2)
                    log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
            }
            float log_prob = 0.0F;
            int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->econ_steps.push_back(
                        ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
            }
            City* best_dest = dest_cities[pick];
            game::Vector(game, *src, best_dest);
            if (best_dest) {
                side.ai_thoughts.push_back(
                        AIThought{"Economy", src->name, best_dest->name, src->x, src->y,
                                  best_dest->x, best_dest->y, logits[pick], 1.0F, false,
                                  Fmt("Vectoring %s from %s -> %s", slot.name.c_str(),
                                      src->name.c_str(), best_dest->name.c_str())});
            }
        }
    }
}

}  // namespace

void PlayTurn(Game& game, Side& side, const PauseCallback& pause,
              const ml::PolicyNetwork* custom_policy, ml::RolloutSampler* sampler) {
    const ml::PolicyNetwork& policy = custom_policy ? *custom_policy : ml::ActivePolicy();
    // Outside training the policy is still sampled, as in training, but at the
    // deployment temperature and from a generator seeded by the game state (never
    // advancing the game's own dice), so play stays reproducible. Taking the single
    // best-scoring option instead would follow a different policy from the one
    // trained: for a spread-out distribution its mode is not its typical behaviour.
    ml::RolloutSampler deploy_sampler;
    if (!sampler) {
        deploy_sampler.temperature = ml::DeployTemperature();
        deploy_sampler.rng_state = (game.rng.state * 2654435761U) ^
                                   static_cast<uint32_t>(game.turn * 40503U) ^
                                   static_cast<uint32_t>((side.index + 1) * 2246822519U);
        sampler = &deploy_sampler;
    }
    for (Army* army : game.armies) {
        if (army->owner == side.index) army->ai_moved = false;
    }
    side.ai_thoughts.clear();

    std::array<bool, 8> diplo_decided{};
    for (int wave = 0; wave < 6; wave++) {
        belief::Update(game, side);
        ml::ContextVec wave_ctx = ml::BuildContext(game, side);
        if (wave == 0) {
            ml::CriticVec critic = ml::BuildCriticFeatures(game, side);
            float board_value = policy.EvaluateValue(wave_ctx, critic);
            side.last_board_value = board_value;
            if (sampler && sampler->current_turn) {
                sampler->current_turn->start_ctx = wave_ctx;
                sampler->current_turn->start_critic = critic;
                sampler->current_turn->value_pred = board_value;
                sampler->current_turn->start_score = ml::ComputeEmpireScore(game, side.index);
            }
        }
        RunDiplomacyPhase(game, side, policy, wave_ctx, sampler, diplo_decided);
        if (wave == 0 && pause) pause("diplomacy");
        RunMilitaryAndExplorationPhase(game, side, policy, wave_ctx, wave == 0 ? pause : nullptr,
                                       sampler);
        bool any_left = false;
        for (Army* army : game.armies) {
            if (army->owner == side.index && army->x != kNone && !army->transit &&
                !army->ai_moved && army->moves >= 2) {
                any_left = true;
                break;
            }
        }
        if (!any_left) break;
    }

    // A city emptied by one move but re-garrisoned by a later one was not left open
    if (sampler && sampler->current_turn) {
        for (auto& mission_step : sampler->current_turn->mission_steps) {
            if (mission_step.vacated_city == kNone ||
                mission_step.vacated_city >= static_cast<int>(game.map->cities.size()))
                continue;
            const City& vacated_city = game.map->cities[mission_step.vacated_city];
            for (int dy = 0; dy <= 1 && mission_step.vacated_city != kNone; dy++) {
                for (int dx = 0; dx <= 1 && mission_step.vacated_city != kNone; dx++) {
                    for (Army* army :
                         game::ArmiesAt(game, vacated_city.x + dx, vacated_city.y + dy)) {
                        if (army->owner == side.index) {
                            mission_step.vacated_city = kNone;
                            break;
                        }
                    }
                }
            }
        }
    }

    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side);
    side.last_board_value = policy.EvaluateValue(ctx, ml::BuildCriticFeatures(game, side));

    RunEconomyPhase(game, side, policy, ctx, sampler);
    if (pause) pause("economy");
}

std::vector<MissionSnapshot> SnapshotMissions(Game& game, Side& side,
                                              const ml::PolicyNetwork& policy) {
    std::vector<MissionSnapshot> out;
    std::vector<bool> moved_before;
    for (Army* army : game.armies) moved_before.push_back(army->ai_moved);
    for (Army* army : game.armies) {
        if (army->owner == side.index) army->ai_moved = false;
    }
    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side);
    RunMilitaryAndExplorationPhase(game, side, policy, ctx, nullptr, nullptr, &out);
    for (size_t i = 0; i < game.armies.size() && i < moved_before.size(); i++)
        game.armies[i]->ai_moved = moved_before[i];
    return out;
}

City* SelectHeroCity(Game& game, Side& side, City* default_city,
                     const ml::PolicyNetwork* custom_policy) {
    // No policy head is trained for where a new hero appears, so this is an
    // explicit rule rather than an untrained proxy: the own city under the
    // highest visible threat, else the game's default (the capital).
    (void)custom_policy;
    belief::Update(game, side);
    const SideBelief& belief = belief::Ensure(side);
    City* best = default_city;
    float best_threat = 0.0F;
    for (City* city : game::SideCities(game, side)) {
        float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->index]
                               : 0.0F;
        if (threat > best_threat) {
            best_threat = threat;
            best = city;
        }
    }
    return best;
}

}  // namespace w2::ai::modern
