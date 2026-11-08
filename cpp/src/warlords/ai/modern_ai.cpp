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

enum class TargetKind : int {
    kHoldGarrison = 0,
    kNeutralCity = 1,
    kEnemyCity = 2,
    kEnemyStack = 3,
    kRuinOrSage = 4,
    kTempleOrItem = 5,
    kFogFrontier = 6,
    kReinforceCity = 7,
};

// May `me` choose an attack on `owner` (at `city`, or a field stack when null)?
// Same rule the actuator enforces: no declaration needed, or DiploNet is no
// longer proposing peace to that side (so the attack declares war).
bool Attackable(const Game& g, int me, int owner, const City* city) {
    if (owner == NONE) return true;
    return !diplomacy::mustDeclare(g, me, owner, city) ||
           diplomacy::proposal(g, me, owner) != diplomacy::PEACE;
}

struct CandidateTarget {
    TargetKind kind = TargetKind::kHoldGarrison;
    int x = 0, y = 0;
    City* city = nullptr;
    Site* site = nullptr;
    Item* item = nullptr;
    int owner = NONE;
    float value = 0.0F;
    bool is_quest_or_own_cap = false;
    bool is_enemy_capital = false;
    bool is_water = false;
    float enemy_threat = 0.0F;
    float nearby_friendly_str = 0.0F;
    int assigned_stacks = 0;
    float assigned_strength = 0.0F;
    std::string label;
};

ml::StackVec BuildStackFeatures(const Game& g, const std::vector<Army*>& stack, int prev_min_move,
                                float origin_rem_guard_str = 10.0F, float origin_threat = 0.0F) {
    ml::StackVec s{};
    if (stack.empty()) return s;
    int total_str = 0, min_move = 99, total_upkeep = 0, blessed = 0;
    bool has_hero = false, has_flier = false, all_fly = true, has_siege = false,
         has_special = false;
    for (Army* a : stack) {
        total_str += a->strength;
        min_move = std::min(min_move, a->moves);
        total_upkeep += a->upkeep;
        if (!a->blessings.empty()) blessed++;
        if (a->hero()) has_hero = true;
        const ArmyType* at = g.types.byId(a->type);
        if (at) {
            if (at->flies)
                has_flier = true;
            else
                all_fly = false;
            if (at->siege) has_siege = true;
            if (at->bonus[48] != 0 || at->bonus[52] != 0) has_special = true;
        } else {
            all_fly = false;
        }
    }
    if (min_move == 99) min_move = 0;
    int hero_cmd = combat::heroBonus(g, stack);
    float speed_drop = (prev_min_move > 0 && min_move < prev_min_move)
                               ? static_cast<float>(prev_min_move - min_move) / 20.0F
                               : 0.0F;

    s[0] = static_cast<float>(stack.size()) / 8.0F;
    s[1] = std::tanh(static_cast<float>(total_str) / 40.0F);
    s[2] = static_cast<float>(total_str) / static_cast<float>(stack.size() * 9);
    s[3] = std::tanh(static_cast<float>(min_move) / 16.0F);
    s[4] = has_hero ? 1.0F : 0.0F;
    s[5] = has_flier ? 1.0F : 0.0F;
    s[6] = all_fly ? 1.0F : 0.0F;
    s[7] = has_siege ? 1.0F : 0.0F;
    s[8] = has_special ? 1.0F : 0.0F;
    s[9] = static_cast<float>(blessed) / static_cast<float>(stack.size());
    s[10] = std::tanh(static_cast<float>(total_upkeep) / 20.0F);
    s[11] = std::tanh(static_cast<float>(hero_cmd) / 3.0F);
    s[12] = (stack.size() == 1) ? 1.0F : 0.0F;
    s[13] = speed_drop;
    s[14] = std::tanh(origin_rem_guard_str / 10.0F);
    s[15] = std::tanh(origin_threat / 20.0F);
    return s;
}

int TargetPathCost(const Game& g, const core::Flood& flood, const CandidateTarget& tgt) {
    if (tgt.kind == TargetKind::kHoldGarrison) return 0;
    if (tgt.city) {
        int best = core::UNREACHED;
        for (int dy = -1; dy <= 2; dy++) {
            for (int dx = -1; dx <= 2; dx++) {
                int tx = tgt.city->x + dx, ty = tgt.city->y + dy;
                if (tx >= 0 && ty >= 0 && tx < g.map->width && ty < g.map->height) {
                    int d = core::floodAt(flood, tx, ty);
                    if (d < best) best = d;
                }
            }
        }
        return best;
    }
    return core::floodAt(flood, tgt.x, tgt.y);
}

ml::TargetVec BuildTargetFeatures(Game& g, const Side& side, const std::vector<Army*>& stack,
                                  const CandidateTarget& tgt, float nearby_friendly_str,
                                  float caution_bias, int path_cost) {
    ml::TargetVec t{};
    t[static_cast<int>(tgt.kind)] = 1.0F;

    int sx = stack[0]->x, sy = stack[0]->y;
    int min_move = std::max(2, move::stackMoves(stack));
    int eff_cost = path_cost >= 0 ? path_cost : (move::distance(sx, sy, tgt.x, tgt.y) * 2);
    float turns_to_reach = static_cast<float>(eff_cost) / static_cast<float>(std::max(4, min_move));

    float win_now = 0.0F, win_converged = 0.0F, surv_ratio = 0.0F;
    float seen_fraction = 1.0F;
    bool is_combat = (tgt.kind == TargetKind::kNeutralCity || tgt.kind == TargetKind::kEnemyCity ||
                      tgt.kind == TargetKind::kEnemyStack);
    if (is_combat) {
        auto est = belief::estimateCombat(g, side, stack, tgt.x, tgt.y, caution_bias);
        win_now = est.winProb;
        surv_ratio = est.expectedSurvivors / static_cast<float>(std::max<size_t>(1, stack.size()));
        seen_fraction = est.seenTileFraction;

        float my_str = 0.0F;
        for (Army* a : stack) my_str += static_cast<float>(a->strength);
        int free_slots = std::max(0, rules::MAX_STACK - static_cast<int>(stack.size()));
        float add_str = std::min(nearby_friendly_str + tgt.assigned_strength,
                                 static_cast<float>(free_slots) * 5.0F);
        if (add_str > 0.5F && est.estimatedDefenderStrength > 0.0F) {
            float combined_ratio =
                    (my_str + add_str) / (my_str + add_str + est.estimatedDefenderStrength);
            win_converged = std::clamp(std::max(win_now, combined_ratio), 0.0F, 1.0F);
        } else {
            win_converged = win_now;
        }
    }

    bool flies = (move::modeOf(g, stack) == move::FLYING);
    bool at_sea = false;
    for (Army* a : stack) {
        if (a->atSea) at_sea = true;
    }
    int tile_dist = std::max(1, move::distance(sx, sy, tgt.x, tgt.y));
    bool requires_boat_embark =
            !flies && !at_sea && (tgt.is_water || (eff_cost >= tile_dist * 2 + 24));

    t[8] = 1.0F / (1.0F + turns_to_reach);
    t[9] = (turns_to_reach <= 1.0F) ? 1.0F : 0.0F;
    t[10] = win_now;
    t[11] = win_converged;
    t[12] = surv_ratio;
    t[13] = std::tanh(tgt.value / 30.0F);
    t[14] = tgt.is_quest_or_own_cap ? 1.0F : 0.0F;
    t[15] = tgt.is_enemy_capital ? 1.0F : 0.0F;
    t[16] = std::tanh(tgt.enemy_threat / 25.0F);
    t[17] = std::tanh((tgt.assigned_strength + nearby_friendly_str) / 25.0F);
    t[18] = std::tanh(static_cast<float>(tgt.assigned_stacks) / 2.0F);
    t[19] = requires_boat_embark ? 1.0F : 0.0F;
    t[20] = seen_fraction;
    return t;
}

void RunDiplomacyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       std::array<bool, 8>& decided) {
    if (g.map->options.diplomacy == 0) return;
    int me = side.index;
    const SideBelief& b = belief::ensure(side);
    const auto& prof = personality::forSide(side);

    int total_vis_cities = 0, max_vis_cities = 0, leader_side = NONE;
    for (int s = 0; s < 8; s++) {
        total_vis_cities += b.visibleCitiesBySide[s];
        if (b.visibleCitiesBySide[s] > max_vis_cities) {
            max_vis_cities = b.visibleCitiesBySide[s];
            leader_side = s;
        }
    }

    int active_wars = 0;
    int primary_enemy = NONE;
    for (int s = 0; s < 8; s++) {
        if (s != me && g.map->sides[s].inUse && g.map->sides[s].alive &&
            (diplomacy::state(g, me, s) == diplomacy::WAR ||
             (decided[s] && diplomacy::proposal(g, me, s) != diplomacy::PEACE))) {
            active_wars++;
            if (primary_enemy == NONE && (diplomacy::state(g, me, s) == diplomacy::WAR ||
                                          diplomacy::proposal(g, me, s) == diplomacy::WAR)) {
                primary_enemy = s;
            }
        }
    }

    float total_my_mobile_str = 0.0F;
    for (Army* a : g.armies) {
        if (a->owner == me && a->x != NONE && !a->transit)
            total_my_mobile_str += static_cast<float>(a->strength);
    }

    auto my_cities_list = game::sideCities(g, side);
    std::array<float, 8> rival_min_dist;
    rival_min_dist.fill(120.0F);
    int closest_rival = NONE;
    float closest_rival_dist = 999.0F;
    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = g.map->sides[other_idx];
        if (other_idx == me || !other.inUse || !other.alive) continue;
        if (b.visibleCitiesBySide[other_idx] == 0 && b.theirBorderStrength[other_idx] == 0.0F)
            continue;
        float md = 120.0F;
        for (City* mc : my_cities_list) {
            for (City* vc : b.visibleCities) {
                if (vc->ownerIndex == other_idx) {
                    md = std::min(md,
                                  static_cast<float>(move::distance(mc->x, mc->y, vc->x, vc->y)));
                }
            }
        }
        rival_min_dist[other_idx] = md;
        if (md < closest_rival_dist) {
            closest_rival_dist = md;
            closest_rival = other_idx;
        }
    }

    int wars_committed = active_wars;

    for (int other_idx = 0; other_idx < 8; other_idx++) {
        Side& other = g.map->sides[other_idx];
        if (other_idx == me || !other.inUse || !other.alive || decided[other_idx]) continue;
        int cur_st = diplomacy::state(g, me, other_idx);
        if (g.map->options.hiddenMap != 0 && b.visibleCitiesBySide[other_idx] == 0 &&
            b.theirBorderStrength[other_idx] == 0.0F && cur_st != diplomacy::WAR) {
            continue;
        }
        decided[other_idx] = true;

        auto att = diplomacy::computeAttitude(g, me, other_idx);
        bool cur_allied = diplomacy::isAllied(g, me, other_idx);
        float min_dist = rival_min_dist[other_idx];

        int shared_wars = 0;
        for (int s = 0; s < 8; s++) {
            if (s != me && s != other_idx && g.map->sides[s].alive &&
                diplomacy::state(g, me, s) == diplomacy::WAR &&
                diplomacy::state(g, other_idx, s) == diplomacy::WAR) {
                shared_wars++;
            }
        }

        int grudge = 0;
        if (side.ai) {
            grudge = side.ai->citiesLost[other_idx] * 8 + side.ai->heroesKilled[other_idx] * 6 +
                     side.ai->lost[other_idx] * 2;
        }

        int other_wars = wars_committed - (cur_st == diplomacy::WAR ? 1 : 0);
        float eff_my_str =
                std::max(b.myBorderStrength[other_idx],
                         total_my_mobile_str * std::clamp(1.1F - min_dist / 45.0F, 0.0F, 0.8F));
        bool is_leader_or_closest = (leader_side == other_idx &&
                                     b.visibleCitiesBySide[other_idx] * 3 >= total_vis_cities) ||
                                    (other_idx == closest_rival);

        ml::RivalVec rv{};
        rv[0] = cur_allied ? 1.0F : 0.0F;
        rv[1] = (!cur_allied && cur_st == diplomacy::PEACE) ? 1.0F : 0.0F;
        rv[2] = (cur_st == diplomacy::INTERMEDIATE) ? 1.0F : 0.0F;
        rv[3] = (cur_st == diplomacy::WAR) ? 1.0F : 0.0F;
        rv[4] = static_cast<float>(diplomacy::truceTurns(g, me, other_idx)) / 15.0F;
        rv[5] = static_cast<float>(att.trustScore) / 100.0F;
        rv[6] = static_cast<float>(att.totalScore) / 100.0F;
        rv[7] = std::tanh(eff_my_str / 35.0F);
        rv[8] = std::tanh(b.theirBorderStrength[other_idx] / 35.0F);
        rv[9] = std::tanh((eff_my_str - b.theirBorderStrength[other_idx]) / 20.0F);
        rv[10] = static_cast<float>(b.visibleCitiesBySide[other_idx]) /
                 static_cast<float>(std::max(1, total_vis_cities));
        rv[11] = is_leader_or_closest ? 1.0F : 0.0F;
        rv[12] = std::min(1.0F, static_cast<float>(shared_wars) / 2.0F);
        rv[13] = std::tanh(static_cast<float>(grudge) / 30.0F);
        rv[14] = (side.capital && side.capital->ownerIndex == other_idx) ? 1.0F : 0.0F;
        rv[15] = std::tanh(static_cast<float>(diplomacy::goldOffer(g, other_idx, me)) / 200.0F);
        rv[16] = std::tanh(min_dist / 45.0F);
        rv[17] = std::tanh(static_cast<float>(other_wars) / 3.0F);

        ml::DiploInVec d_in = ml::packDiploInput(ctx, rv);
        auto raw = policy.diploNet.Forward(d_in);
        std::vector<float> stance_vec = {raw[0], raw[1], raw[2], raw[3]};
        ml::DiploStepRecord rec;
        rec.input = d_in;
        int best_choice = ml::selectFromLogits(stance_vec, sampler, &rec.oldStanceLogProb);
        rec.chosenStance = best_choice;
        float best_logit = stance_vec[best_choice];

        if (best_choice == 0) {
            diplomacy::propose(g, me, other_idx, diplomacy::PEACE);
            diplomacy::proposeAlliance(g, me, other_idx, true);
        } else if (best_choice == 1) {
            diplomacy::propose(g, me, other_idx, diplomacy::PEACE);
            diplomacy::proposeAlliance(g, me, other_idx, false);
        } else if (best_choice == 2) {
            diplomacy::propose(g, me, other_idx, diplomacy::INTERMEDIATE);
            if (cur_st != diplomacy::WAR) wars_committed++;
        } else {
            diplomacy::propose(g, me, other_idx, diplomacy::WAR);
            if (cur_st != diplomacy::WAR) wars_committed++;
            if (primary_enemy == NONE) primary_enemy = other_idx;
        }

        // Binary sub-decision (output 4): sweeten a peace offer to a side we are at war with
        bool friendly = (best_choice == 0 || best_choice == 1);
        diplomacy::setGoldOffer(g, me, other_idx, 0);
        if (friendly && cur_st == diplomacy::WAR && side.gold >= 100) {
            rec.hasTribute = true;
            rec.tributeChosen = ml::selectBinary(raw[4], sampler, &rec.oldTributeLogProb);
            if (rec.tributeChosen) {
                int offer = std::clamp(side.gold / 4 / 25 * 25, 50, std::min(300, side.gold / 2));
                diplomacy::setGoldOffer(g, me, other_idx, offer);
            }
        }

        // Binary sub-decision (output 5): ask a friendly side to join our war
        diplomacy::setWarTarget(g, me, other_idx, NONE);
        if (friendly && primary_enemy != NONE && primary_enemy != other_idx) {
            rec.hasJointWar = true;
            rec.jointWarChosen = ml::selectBinary(raw[5], sampler, &rec.oldJointWarLogProb);
            if (rec.jointWarChosen) diplomacy::setWarTarget(g, me, other_idx, primary_enemy);
        }

        if (sampler && sampler->currentTurn) {
            sampler->currentTurn->diploSteps.push_back(rec);
        }

        const char* stance_names[4] = {"ALLIED", "PEACE", "UNEASY", "WAR"};
        side.aiThoughts.push_back(AIThought{
            "Diplomacy", side.name, other.name, side.capX, side.capY, other.capX, other.capY,
            best_logit, 0.0F, false,
            fmt("Stance -> %s (Attitude %+d, Trust %+d): %s", stance_names[best_choice],
                att.totalScore, att.trustScore, att.stanceSummary.c_str())});

        if (!other.computer && other.missives.empty() && g.turn > 1 &&
            g.turn - other.lastMissiveTurn[me] >= 4) {
            bool send_missive = false;
            DiplomaticMissive m;
            m.fromSide = me;
            m.toSide = other_idx;
            m.reasonSummary = att.whatItTakes;

            if (best_choice == 0 && !cur_allied) {
                send_missive = true;
                m.proposedState = diplomacy::PEACE;
                m.proposeAlliance = true;
                m.jointWarTarget = diplomacy::warTarget(g, me, other_idx);
                m.headline = fmt("Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
                m.body = fmt("\"%s\" We propose a full Military Alliance with shared vision!",
                             prof.motto.c_str());
            } else if (best_choice == 2 && cur_st == diplomacy::PEACE &&
                       b.theirBorderStrength[other_idx] > 6.0F) {
                send_missive = true;
                m.proposedState = diplomacy::INTERMEDIATE;
                m.headline =
                        fmt("Border Warning from %s (%s)", side.name.c_str(), prof.name.c_str());
                m.body = "Your armies mass along our frontier. Withdraw them or our peace shall "
                         "end!";
            } else if (best_choice == 2 && cur_st == diplomacy::PEACE && rv[9] > 0.35F) {
                // Human-only flavour (never reached in all-AI training): demand tribute
                // when our border forces clearly outmatch theirs.
                send_missive = true;
                m.proposedState = diplomacy::PEACE;
                m.goldDemand = std::max(75, att.peaceGoldNeeded > 0 ? att.peaceGoldNeeded : 100);
                m.headline =
                        fmt("Tribute Demand from %s (%s)", side.name.c_str(), prof.name.c_str());
                m.body = fmt("\"%s\" Pay %d gold in tribute to preserve the peace between us!",
                             prof.motto.c_str(), m.goldDemand);
            } else if (best_choice == 1 && cur_st == diplomacy::WAR) {
                send_missive = true;
                m.proposedState = diplomacy::PEACE;
                int offered = diplomacy::goldOffer(g, me, other_idx);
                m.goldDemand = -offered;
                m.headline = fmt("Peace Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
                m.body = offered > 0 ? fmt("We offer %d gold and a 10-turn truce to end this war.",
                                           offered)
                                     : "Let us lay down arms and sign a 10-turn truce.";
            }

            if (send_missive) {
                other.missives.push_back(std::move(m));
                other.lastMissiveTurn[me] = g.turn;
            }
        }
    }
}

void DecideCaptureSpoils(Game& g, Side& side, City& city, const std::vector<Army*>& attackers,
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx,
                         ml::RolloutSampler* sampler) {
    if (city.razed || city.ownerIndex != side.index) return;
    if (ai::questCapture(g, side, city, attackers)) return;

    const SideBelief& b = belief::ensure(side);
    float local_threat = city.index < static_cast<int>(b.cityThreat1Turn.size())
                                 ? b.cityThreat1Turn[city.index]
                                 : 0.0F;
    float dist_enemy = city.index < static_cast<int>(b.cityNearestEnemyDist.size())
                               ? b.cityNearestEnemyDist[city.index]
                               : 60.0F;
    int sack_gold = ai::groups::sackValue(g, city);
    int pillage_gold =
            city.slots.size() >= 2 ? std::abs(g.types.byId(city.slots.back().type)->price) / 2 : 0;

    auto make_spoil_in = [&](float ongoing_income, float instant_gold, float diplo_pen) {
        ml::EconVec e{};
        e[6] = 1.0F;
        e[7] = ongoing_income / 35.0F;
        e[14] = std::tanh(local_threat / 25.0F);
        e[15] = std::tanh(dist_enemy / 50.0F);
        e[18] = std::tanh(instant_gold / 250.0F);
        e[19] = diplo_pen / 40.0F;
        return ml::packEconInput(ctx, e);
    };

    bool is_any_capital = false;
    for (int s = 0; s < 8; s++) {
        if (g.map->sides[s].capital == &city) is_any_capital = true;
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
        scores[i] = policy.econNet.Forward(cand_inputs[i])[0];
    }
    float log_prob = 0.0F;
    int pick = ml::selectFromLogits(scores, sampler, &log_prob);
    if (sampler && sampler->currentTurn && cand_inputs.size() > 1) {
        sampler->currentTurn->econSteps.push_back(
                ml::EconStepRecord{std::move(cand_inputs), {}, pick, log_prob});
    }

    int best_mode = modes[pick];
    if (best_mode == 1) {
        game::pillage(g, side, city, attackers);
        if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "pillaged");
    } else if (best_mode == 2) {
        game::sack(g, side, city, attackers);
        if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "sacked");
    } else if (best_mode == 3) {
        game::raze(g, side, city, attackers);
        if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "razed");
    } else {
        game::occupy(g, side, city, attackers);
    }
}

bool ExecuteStackMission(Game& g, Side& side, std::vector<Army*>& stack, CandidateTarget& tgt,
                         bool stage_near, const ml::PolicyNetwork& policy,
                         const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [&](Army* a) { return !g.alive(a) || a->moves <= 0; }),
                stack.end());
    if (stack.empty()) return false;

    if (tgt.kind == TargetKind::kHoldGarrison) {
        for (Army* a : stack) a->aiMoved = true;
        return true;
    }

    int me = side.index;
    std::map<Army*, int> start_moves;
    for (Army* a : stack) start_moves[a] = a->moves;

    // A coalition drawn from one owned 2x2 city may stand on different corners.
    // Members walk (by the normal rules, paying moves) onto the leader's tile;
    // any that cannot join without breaking the 8-army stack limit stay behind
    // and remain free for later waves.
    {
        int lx = stack[0]->x, ly = stack[0]->y;
        int on_leader_tile = static_cast<int>(game::armiesAt(g, lx, ly).size());
        std::vector<Army*> joined;
        for (Army* a : stack) {
            if (a->x == lx && a->y == ly) {
                joined.push_back(a);
                continue;
            }
            if (on_leader_tile + 1 > rules::MAX_STACK) continue;
            auto path = move::findPath(g, {a}, a->x, a->y, lx, ly);
            if (!path || path->empty()) continue;
            move::WalkResult jr = move::walk(g, {a}, *path);
            if (g.alive(a) && a->x == lx && a->y == ly && jr.steps > 0) {
                ai::walked(g, {a}, jr);
                joined.push_back(a);
                on_leader_tile++;
            }
        }
        stack = std::move(joined);
        stack.erase(
                std::remove_if(stack.begin(), stack.end(), [&](Army* a) { return a->moves <= 0; }),
                stack.end());
        if (stack.empty()) return false;
    }

    int dest_x = tgt.x, dest_y = tgt.y;
    if (tgt.city && !stage_near) {
        if (tgt.city->ownerIndex == me) {
            for (int oy = 0; oy <= 1; oy++) {
                for (int ox = 0; ox <= 1; ox++) {
                    int tx = tgt.city->x + ox, ty = tgt.city->y + oy;
                    if (static_cast<int>(game::armiesAt(g, tx, ty).size() + stack.size()) <=
                        rules::MAX_STACK) {
                        dest_x = tx;
                        dest_y = ty;
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
                int nx = tgt.x + dx, ny = tgt.y + dy;
                if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height) continue;
                if (tgt.city && game::cityAt(g, nx, ny) == tgt.city) continue;
                if (!belief::isTileVisible(g, me, nx, ny)) continue;
                int terr = g.map->terrainType[g.map->tiles[ny * g.map->width + nx] & 0xff];
                if (terr == move::WATER || terr == move::SHORE || terr == move::MOUNTAINS) continue;
                auto occ = game::armiesAt(g, nx, ny);
                if (!occ.empty() &&
                    (occ[0]->owner != me ||
                     static_cast<int>(occ.size() + stack.size()) > rules::MAX_STACK)) {
                    continue;
                }
                int d = move::distance(sx0, sy0, nx, ny) - (!occ.empty() ? 15 : 0);
                if (d < best_dist) {
                    best_dist = d;
                    best_sx = nx;
                    best_sy = ny;
                }
            }
        }
        dest_x = best_sx;
        dest_y = best_sy;
    }

    core::Sel sel = core::select(g, stack);
    if (sel.armies.empty() || !sel.leader) return false;

    bool is_fog_mission = (tgt.kind == TargetKind::kFogFrontier);
    bool made_progress = false;
    bool completed_objective = false;

    for (int pass = 0; pass < 24 && !sel.armies.empty() && sel.minMoves > 0; pass++) {
        int lx = sel.leader->x, ly = sel.leader->y;
        if (lx == dest_x && ly == dest_y) {
            if (is_fog_mission) completed_objective = true;
            break;
        }

        auto path = move::findPath(g, sel.armies, lx, ly, dest_x, dest_y);
        if (!path || path->empty()) break;

        std::vector<move::Step> walk_steps = *path;
        if (is_fog_mission && walk_steps.size() > 6) {
            walk_steps.resize(6);
        }

        int seen_box_before = 0;
        if (is_fog_mission && g.map->options.hiddenMap != 0) {
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int ux = lx + ox, uy = ly + oy;
                    if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
                        belief::isTileVisible(g, me, ux, uy)) {
                        seen_box_before++;
                    }
                }
            }
        }

        move::WalkResult r = move::walk(g, sel.armies, walk_steps);
        if (r.steps > 0) {
            made_progress = true;
            ai::walked(g, sel.armies, r);
        }
        core::refresh(g, sel);
        core::pickUp(g, sel);

        int ax = NONE, ay = NONE;
        City* att_city = nullptr;
        if (r.stopped == "attack" && r.attack && !stage_near) {
            ax = r.attack->x;
            ay = r.attack->y;
            att_city = r.attack->city;
        } else if (r.stopped == "at peace" && !stage_near &&
                   r.steps < static_cast<int>(walk_steps.size())) {
            const move::Step& ahead = walk_steps[r.steps];
            ax = ahead.x;
            ay = ahead.y;
            att_city = game::cityAt(g, ax, ay);
        }

        if (ax != NONE && ay != NONE) {
            auto def_armies = game::armiesAt(g, ax, ay);
            int def_owner = def_armies.empty() ? (att_city ? att_city->ownerIndex : NONE)
                                               : def_armies[0]->owner;

            // Breaking a treaty is DiploNet's call, not the mission's: an attack that
            // needs a declaration (the game's own mustDeclare rule) only goes ahead
            // when this side is no longer proposing peace to the defender.
            if (diplomacy::mustDeclare(g, me, def_owner, att_city)) {
                if (diplomacy::proposal(g, me, def_owner) == diplomacy::PEACE) break;
                diplomacy::attackAnyway(g, me, def_owner);
            }

            // Gather friendly units on the launching tile so converged allies fight as one combined
            // force
            std::vector<Army*> combined_here =
                    core::collect(g, me, sel.leader->x, sel.leader->y, 1);
            if (combined_here.empty()) combined_here = sel.armies;

            core::Sel comb_sel = core::select(g, combined_here);
            int prev_owner = att_city ? att_city->ownerIndex : NONE;
            Battle b_res = core::fight(g, comb_sel, ax, ay);
            made_progress = true;
            belief::recordBattleObservation(g, me, ax, ay);
            if (att_city && att_city->ownerIndex == me && prev_owner != me &&
                !comb_sel.armies.empty()) {
                DecideCaptureSpoils(g, side, *att_city, comb_sel.armies, policy, ctx, sampler);
            }
            sel = comb_sel;

            if (!b_res.won || sel.armies.empty()) {
                break;
            }
            if (tgt.city && tgt.city->ownerIndex == me) {
                completed_objective = true;
                break;
            }
            if (tgt.kind == TargetKind::kEnemyStack && ax == tgt.x && ay == tgt.y) {
                completed_objective = true;
                break;
            }
            // Won an intermediate road/blocker battle and still have moves: keep advancing toward
            // dest!
            continue;
        }

        if (is_fog_mission && r.steps > 0 && g.map->options.hiddenMap != 0) {
            int seen_box_after = 0;
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int ux = lx + ox, uy = ly + oy;
                    if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
                        belief::isTileVisible(g, me, ux, uy)) {
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

        if (r.steps == 0 || r.stopped == "out of moves" || r.stopped == "blocked" ||
            r.stopped == "at peace") {
            if (stage_near && r.steps > 0) {
                // Arrived at or advanced toward staging position
                made_progress = true;
            }
            break;
        }
    }

    // Interact with open temple or ruin/sage if standing on one with moves left
    if (!sel.armies.empty() && sel.leader && sel.minMoves > 0) {
        int fx = sel.leader->x, fy = sel.leader->y;
        Site* s = core::siteAt(g, fx, fy);
        if (s && core::siteOpen(*s)) {
            if (s->content == site::TEMPLE) {
                site::search(g, sel.armies, fx, fy, false);
                made_progress = true;
                completed_objective = true;
                if (g.map->options.quests != 0 && sel.hero &&
                    (!side.quest || !side.quest->hasTarget())) {
                    quest::assign(g, side, sel.hero, true, core::flies(g, sel.hero));
                }
            } else if (sel.hero && g.alive(sel.hero)) {
                site::search(g, sel.armies, fx, fy, false);
                made_progress = true;
                completed_objective = true;
            }
        }
        core::pickUp(g, sel);
    }

    for (Army* a : sel.armies) {
        if (!g.alive(a)) continue;
        auto it = start_moves.find(a);
        bool spent_moves = (it != start_moves.end() && a->moves < it->second);
        if (completed_objective && spent_moves && a->moves >= 2 && !stage_near &&
            tgt.kind != TargetKind::kReinforceCity) {
            // Allow remaining movement points to be dispatched in the next wave of this turn
            a->aiMoved = false;
        } else {
            a->aiMoved = made_progress;
        }
    }
    return made_progress;
}

std::vector<size_t> BuildCoalitionSizes(const Game& g, Army* seed,
                                        const std::vector<Army*>& escorts) {
    std::vector<size_t> sizes = {1};
    auto add_unique = [&](size_t m) {
        if (m >= 1 && m <= escorts.size() + 1 &&
            std::find(sizes.begin(), sizes.end(), m) == sizes.end()) {
            sizes.push_back(m);
        }
    };
    if (!escorts.empty()) {
        add_unique(2);
        add_unique(4);
        add_unique(escorts.size());
        add_unique(escorts.size() + 1);

        // Also include fast/flying-matching prefix size if distinct
        bool seed_flies = core::flies(g, seed);
        size_t fast_count = 1;
        for (Army* e : escorts) {
            if (seed_flies ? core::flies(g, e) : (e->moves >= 12)) {
                fast_count++;
            } else {
                break;
            }
        }
        add_unique(fast_count);
    }
    return sizes;
}

void RunMilitaryAndExplorationPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx, const PauseCallback& pause,
                                    ml::RolloutSampler* sampler,
                                    std::vector<MissionSnapshot>* snapshots = nullptr) {
    int me = side.index;
    SideBelief& b = belief::ensure(side);
    float caution = 0.0F;

    std::vector<CandidateTarget> targets;
    targets.reserve(80);

    for (City* c : b.visibleCities) {
        if (c->razed) continue;
        float threat = c->index < static_cast<int>(b.cityThreat2Turn.size())
                               ? b.cityThreat2Turn[c->index]
                               : 0.0F;
        bool is_quest_or_own = (side.capital == c) || (side.quest && side.quest->city == c);
        bool is_enemy_cap = false;
        for (int s = 0; s < 8; s++) {
            if (s != me && g.map->sides[s].capital == c) is_enemy_cap = true;
        }

        if (c->ownerIndex == me) {
            float existing_garrison_str = 0.0F;
            float stationary_garrison_str = 0.0F;
            int existing_count = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    for (Army* a : game::armiesAt(g, c->x + dx, c->y + dy)) {
                        if (a->owner == me) {
                            existing_garrison_str += static_cast<float>(a->strength);
                            existing_count++;
                            if (a->moves <= 0 || a->aiMoved) {
                                stationary_garrison_str += static_cast<float>(a->strength);
                            }
                        }
                    }
                }
            }
            CandidateTarget hold;
            hold.kind = TargetKind::kHoldGarrison;
            hold.x = c->x;
            hold.y = c->y;
            hold.city = c;
            hold.owner = me;
            hold.value = static_cast<float>(c->income) + static_cast<float>(c->defence) * 3.0F;
            hold.is_quest_or_own_cap = is_quest_or_own;
            hold.enemy_threat = threat;
            hold.assigned_strength = stationary_garrison_str;
            hold.label = "Hold " + c->name;
            targets.push_back(std::move(hold));

            if (existing_count < 32) {
                CandidateTarget reinf;
                reinf.kind = TargetKind::kReinforceCity;
                reinf.x = c->x;
                reinf.y = c->y;
                reinf.city = c;
                reinf.owner = me;
                reinf.value = static_cast<float>(c->income) + static_cast<float>(c->defence) * 3.0F;
                reinf.is_quest_or_own_cap = is_quest_or_own;
                reinf.enemy_threat = threat;
                reinf.assigned_strength = existing_garrison_str;
                reinf.label = "Reinforce " + c->name;
                targets.push_back(std::move(reinf));
            }
        } else if (c->ownerIndex == NONE) {
            CandidateTarget neut;
            neut.kind = TargetKind::kNeutralCity;
            neut.x = c->x;
            neut.y = c->y;
            neut.city = c;
            neut.owner = NONE;
            neut.value = static_cast<float>(c->income) + static_cast<float>(c->slots.size()) * 5.0F;
            neut.is_quest_or_own_cap = is_quest_or_own;
            neut.is_enemy_capital = is_enemy_cap;
            neut.enemy_threat = threat;
            neut.label = "Capture Neutral " + c->name;
            targets.push_back(std::move(neut));
        } else if (Attackable(g, me, c->ownerIndex, c)) {
            CandidateTarget enem;
            enem.kind = TargetKind::kEnemyCity;
            enem.x = c->x;
            enem.y = c->y;
            enem.city = c;
            enem.owner = c->ownerIndex;
            enem.value = static_cast<float>(c->income) + static_cast<float>(c->defence) * 6.0F;
            if (diplomacy::alliedTargetCity(g, c->ownerIndex) == c->index) enem.value += 10.0F;
            enem.is_quest_or_own_cap = is_quest_or_own;
            enem.is_enemy_capital = is_enemy_cap;
            enem.enemy_threat = threat;
            enem.label = "Siege " + c->name;
            targets.push_back(std::move(enem));
        }
    }

    for (const auto& [tile_key, obs] : b.stacksByTile) {
        if (obs.owner == me || obs.owner == NONE) continue;
        if (game::cityAt(g, obs.x, obs.y)) continue;
        if (!Attackable(g, me, obs.owner, nullptr)) continue;
        CandidateTarget st;
        st.kind = TargetKind::kEnemyStack;
        st.x = obs.x;
        st.y = obs.y;
        st.owner = obs.owner;
        st.value = static_cast<float>(obs.visibleCount) * 4.0F;
        st.is_quest_or_own_cap =
                (side.quest && side.quest->side && side.quest->side->index == obs.owner);
        st.label = fmt("Intercept stack (%d,%d)", obs.x, obs.y);
        targets.push_back(std::move(st));
    }

    for (Site* s : b.visibleSites) {
        if (!core::siteOpen(*s)) continue;
        CandidateTarget st;
        st.x = s->x;
        st.y = s->y;
        st.site = s;
        if (s->content == site::TEMPLE) {
            st.kind = TargetKind::kTempleOrItem;
            st.value = 18.0F;
            st.label = "Bless at " + s->name;
        } else {
            st.kind = TargetKind::kRuinOrSage;
            st.value = s->rich ? 28.0F : 22.0F;
            st.label = "Search " + s->name;
        }
        targets.push_back(std::move(st));
    }

    for (Item* it : b.visibleGroundItems) {
        CandidateTarget gt;
        gt.kind = TargetKind::kTempleOrItem;
        gt.x = it->x;
        gt.y = it->y;
        gt.item = it;
        gt.value = 20.0F;
        gt.is_quest_or_own_cap = (side.quest && side.quest->item == it);
        gt.label = "Pick up " + it->name;
        targets.push_back(std::move(gt));
    }

    for (const auto& fc : b.fogFrontiers) {
        CandidateTarget ft;
        ft.kind = TargetKind::kFogFrontier;
        ft.x = fc.x;
        ft.y = fc.y;
        ft.is_water = fc.isWater;
        ft.value = static_cast<float>(fc.regionUnseenCount) * 2.0F + (fc.onRoad ? 6.0F : 0.0F);
        ft.label = fmt("Explore %s (%d,%d)", fc.isWater ? "Sea" : "Fog", fc.x, fc.y);
        targets.push_back(std::move(ft));
    }

    if (targets.empty()) return;

    for (auto& t : targets) {
        if (t.kind == TargetKind::kNeutralCity || t.kind == TargetKind::kEnemyCity ||
            t.kind == TargetKind::kEnemyStack) {
            float near_str = 0.0F;
            for (Army* a : g.armies) {
                if (a->owner == me && a->x != NONE && !a->transit && a->moves > 0 &&
                    move::distance(a->x, a->y, t.x, t.y) <= 14) {
                    near_str += static_cast<float>(a->strength);
                }
            }
            t.nearby_friendly_str = near_str;
        }
    }

    struct LocGroup {
        int x = 0, y = 0;
        City* city = nullptr;
        std::vector<Army*> units;
    };
    std::map<int, LocGroup> groups_by_loc;
    for (Army* a : g.armies) {
        if (a->owner != me || a->x == NONE || a->transit || a->moves <= 0 || a->aiMoved) continue;
        City* c = game::cityAt(g, a->x, a->y);
        int key = c ? -(c->index + 1) : (a->y * g.map->width + a->x);
        auto& lg = groups_by_loc[key];
        lg.x = a->x;
        lg.y = a->y;
        lg.city = c;
        lg.units.push_back(a);
    }

    int best_enemy_city_target = NONE;
    float best_enemy_city_score = -1e9F;

    for (auto& [loc_key, lg] : groups_by_loc) {
        std::vector<Army*> pool = lg.units;
        std::sort(pool.begin(), pool.end(), [&](Army* a, Army* b2) {
            if (a->hero() != b2->hero()) return a->hero() > b2->hero();
            bool af = core::flies(g, a), bf = core::flies(g, b2);
            if (af != bf) return af > bf;
            int sa = a->strength * 2 + a->maxMoves;
            int sb = b2->strength * 2 + b2->maxMoves;
            if (sa != sb) return sa > sb;
            return a->moves > b2->moves;
        });

        core::Flood land_flood = core::floodFrom(g, me, lg.x, lg.y, 72, move::LAND, me);
        // The flying flood only matters to a coalition that can fly; skipping it
        // for flightless groups also keeps land stacks off fog only fliers can reach
        bool any_flier = false;
        for (Army* a : pool) any_flier = any_flier || core::flies(g, a);
        core::Flood fly_flood = any_flier ? core::floodFrom(g, me, lg.x, lg.y, 72, move::FLYING, me)
                                          : core::Flood{};
        auto fly_dist = [&](int x, int y) {
            return any_flier ? core::floodAt(fly_flood, x, y) : core::UNREACHED;
        };

        // Ensure local reachable fog frontiers (land or coastal sea) are included near this stack
        if (g.map->options.hiddenMap != 0) {
            int reachable_fog_count = 0;
            for (const auto& t : targets) {
                if (t.kind == TargetKind::kFogFrontier &&
                    core::floodAt(land_flood, t.x, t.y) < core::UNREACHED) {
                    reachable_fog_count++;
                }
            }
            if (reachable_fog_count < 4) {
                int w = g.map->width, h = g.map->height;
                for (int r = 1; r <= 28 && reachable_fog_count < 6; r++) {
                    for (int dy = -r; dy <= r && reachable_fog_count < 6; dy++) {
                        for (int dx = -r; dx <= r && reachable_fog_count < 6; dx++) {
                            if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                            int fx = lg.x + dx, fy = lg.y + dy;
                            if (fx < 0 || fy < 0 || fx >= w || fy >= h) continue;
                            int fterr = g.map->terrainType[g.map->tiles[fy * w + fx] & 0xff];
                            if (fterr == move::MOUNTAINS) continue;
                            if (core::floodAt(land_flood, fx, fy) >= core::UNREACHED) continue;
                            int adj_u = 0;
                            for (int oy = -1; oy <= 1; oy++) {
                                for (int ox = -1; ox <= 1; ox++) {
                                    int ux = fx + ox, uy = fy + oy;
                                    if (ux >= 0 && uy >= 0 && ux < w && uy < h &&
                                        !belief::isTileVisible(g, me, ux, uy)) {
                                        adj_u++;
                                    }
                                }
                            }
                            if (adj_u >= 1) {
                                bool is_w = (fterr == move::WATER || fterr == move::SHORE);
                                bool dup = false;
                                for (const auto& t : targets) {
                                    if (t.kind == TargetKind::kFogFrontier && t.is_water == is_w &&
                                        move::distance(fx, fy, t.x, t.y) < 3) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (!dup) {
                                    CandidateTarget ft;
                                    ft.kind = TargetKind::kFogFrontier;
                                    ft.x = fx;
                                    ft.y = fy;
                                    ft.is_water = is_w;
                                    bool road = !is_w && (scn::roadAt(*g.map, fx, fy) % 0x20 != 0);
                                    ft.value =
                                            static_cast<float>(adj_u) * 2.0F + (road ? 6.0F : 0.0F);
                                    ft.label =
                                            fmt("Explore %s (%d,%d)", is_w ? "Sea" : "Fog", fx, fy);
                                    targets.push_back(std::move(ft));
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
        if (g.map->options.hiddenMap != 0) {
            std::vector<std::pair<int, size_t>> land_fogs, sea_fogs;
            for (size_t ti = 0; ti < targets.size(); ti++) {
                if (targets[ti].kind != TargetKind::kFogFrontier) continue;
                int d_land = core::floodAt(land_flood, targets[ti].x, targets[ti].y);
                int d_fly = fly_dist(targets[ti].x, targets[ti].y);
                int best_d = std::min(d_land, d_fly);
                if (best_d >= core::UNREACHED) continue;
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
            if (!g.alive(seed) || seed->moves <= 0 || seed->aiMoved) continue;

            std::vector<Army*> eligible_escorts;
            for (auto it = pool.begin(); it != pool.end();) {
                Army* cand = *it;
                if (!g.alive(cand) || cand->moves <= 0 || cand->aiMoved) {
                    it = pool.erase(it);
                    continue;
                }
                if (eligible_escorts.size() + 1 < rules::MAX_STACK) {
                    eligible_escorts.push_back(cand);
                }
                ++it;
            }

            float rem_group_str = static_cast<float>(seed->strength);
            for (Army* rem : pool) rem_group_str += static_cast<float>(rem->strength);

            float committed_city_guard_str = 0.0F;
            float origin_threat = 0.0F;
            if (lg.city && lg.city->ownerIndex == me) {
                if (lg.city->index < static_cast<int>(b.cityThreat2Turn.size())) {
                    origin_threat = b.cityThreat2Turn[lg.city->index];
                }
                for (int dy = 0; dy <= 1; dy++) {
                    for (int dx = 0; dx <= 1; dx++) {
                        for (Army* a : game::armiesAt(g, lg.city->x + dx, lg.city->y + dy)) {
                            if (a->owner == me && (a->moves <= 0 || a->aiMoved)) {
                                committed_city_guard_str += static_cast<float>(a->strength);
                            }
                        }
                    }
                }
            }

            std::vector<size_t> sizes = BuildCoalitionSizes(g, seed, eligible_escorts);

            struct PrebuiltStack {
                size_t size = 1;
                std::vector<Army*> units;
                ml::StackVec sv{};
                bool flies = false;
                float str = 0.0F;
            };
            std::vector<PrebuiltStack> pre_stacks;
            pre_stacks.reserve(sizes.size());
            for (size_t sz : sizes) {
                PrebuiltStack ps;
                ps.size = sz;
                ps.units.push_back(seed);
                for (size_t i = 0; i + 1 < sz && i < eligible_escorts.size(); i++) {
                    ps.units.push_back(eligible_escorts[i]);
                }
                for (Army* a : ps.units) ps.str += static_cast<float>(a->strength);
                float rem_guard_after_leave = (lg.city && lg.city->ownerIndex == me)
                                                      ? (committed_city_guard_str +
                                                         std::max(0.0F, rem_group_str - ps.str))
                                                      : 20.0F;
                ps.sv = BuildStackFeatures(g, ps.units, seed->moves, rem_guard_after_leave,
                                           origin_threat);
                ps.flies = (move::modeOf(g, ps.units) == move::FLYING);
                pre_stacks.push_back(std::move(ps));
            }

            struct JointOption {
                size_t target_idx = 0;
                size_t stack_opt_idx = 0;
                ml::MissionInVec in_vec{};
                ml::TargetVec tv{};
                ml::MissionOutput out{};
            };
            std::vector<JointOption> options;
            options.reserve(targets.size() * pre_stacks.size());

            // Scoring options only reads the board: answer per-tile army queries
            // (combat estimates) from one index instead of scanning every army.
            // The scope closes before the chosen stack moves.
            {
                belief::ScopedTileIndex scoring_index(g);
                for (size_t ti = 0; ti < targets.size(); ti++) {
                    if (blocked_targets.count(ti)) continue;
                    const CandidateTarget& tgt = targets[ti];

                    if ((tgt.kind == TargetKind::kNeutralCity ||
                         tgt.kind == TargetKind::kEnemyCity) &&
                        tgt.city && (tgt.city->ownerIndex == me || tgt.city->razed)) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kEnemyStack &&
                        belief::isTileVisible(g, me, tgt.x, tgt.y)) {
                        // Only re-check stacks still in view; one that walked into fog stays a
                        // belief
                        auto cur_at = belief::armiesAt(g, tgt.x, tgt.y);
                        if (cur_at.empty() || cur_at[0]->owner == me) continue;
                    }
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.site &&
                        !core::siteOpen(*tgt.site))
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item &&
                        (tgt.item->status != 1 || tgt.item->x == NONE)) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kFogFrontier && g.map->options.hiddenMap != 0) {
                        if (tgt.assigned_stacks >= 1) continue;
                        if (!local_fog_indices.empty() && !local_fog_indices.count(ti)) continue;
                        bool still_has_unseen = false;
                        for (int oy = -1; oy <= 1 && !still_has_unseen; oy++) {
                            for (int ox = -1; ox <= 1 && !still_has_unseen; ox++) {
                                int ux = tgt.x + ox, uy = tgt.y + oy;
                                if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
                                    !belief::isTileVisible(g, me, ux, uy)) {
                                    still_has_unseen = true;
                                }
                            }
                        }
                        if (!still_has_unseen) continue;
                    }

                    if (tgt.kind == TargetKind::kHoldGarrison && tgt.city != lg.city) continue;
                    if (tgt.kind == TargetKind::kReinforceCity && tgt.city == lg.city) continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && !seed->hero()) continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.item && !seed->hero())
                        continue;
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.site &&
                        !seed->blessings.empty() && !seed->hero()) {
                        continue;
                    }
                    if (tgt.kind == TargetKind::kTempleOrItem && tgt.assigned_stacks >= 1) continue;
                    if (tgt.kind == TargetKind::kRuinOrSage && tgt.assigned_stacks >= 1) continue;

                    bool is_combat_tgt = (tgt.kind == TargetKind::kNeutralCity ||
                                          tgt.kind == TargetKind::kEnemyCity ||
                                          tgt.kind == TargetKind::kEnemyStack);
                    for (size_t si = 0; si < pre_stacks.size(); si++) {
                        const PrebuiltStack& ps = pre_stacks[si];
                        const core::Flood& active_flood = ps.flies ? fly_flood : land_flood;
                        int p_cost = TargetPathCost(g, active_flood, tgt);
                        if (p_cost >= core::UNREACHED) continue;

                        float near_str = 0.0F;
                        ml::StackVec opt_sv = ps.sv;
                        if (tgt.kind == TargetKind::kHoldGarrison) {
                            near_str = rem_group_str;
                            opt_sv[14] = std::tanh((committed_city_guard_str + ps.str) / 10.0F);
                        } else if (is_combat_tgt) {
                            near_str = std::max(0.0F, tgt.nearby_friendly_str - rem_group_str);
                        }

                        ml::TargetVec tv = BuildTargetFeatures(g, side, ps.units, tgt, near_str,
                                                               caution, p_cost);
                        ml::MissionInVec m_in = ml::packMissionInput(ctx, opt_sv, tv);
                        auto raw = policy.missionNet.Forward(m_in);
                        options.push_back(
                                JointOption{ti, si, m_in, tv, ml::MissionOutput{raw[0], raw[1]}});
                    }
                }
            }  // scoringIndex

            if (options.empty()) {
                seed->aiMoved = true;
                continue;
            }

            std::array<int, 8> kind_counts{};
            for (const auto& op : options) {
                kind_counts[static_cast<int>(targets[op.target_idx].kind)]++;
            }
            std::vector<float> logits(options.size());
            std::vector<float> log_mult(options.size());
            for (size_t i = 0; i < options.size(); i++) {
                logits[i] = options[i].out.utility;
                int kc = std::max(
                        1, kind_counts[static_cast<int>(targets[options[i].target_idx].kind)]);
                log_mult[i] = std::log(static_cast<float>(kc));
            }
            if (snapshots) {
                // Dry run: record this group's first decision and move on, touching nothing
                MissionSnapshot snap;
                snap.candidates.reserve(options.size());
                for (const auto& op : options) {
                    const CandidateTarget& t = targets[op.target_idx];
                    snap.candidates.push_back(op.in_vec);
                    snap.kind.push_back(static_cast<int>(t.kind));
                    snap.cityIndex.push_back(t.city ? t.city->index : NONE);
                    snap.tx.push_back(t.x);
                    snap.ty.push_back(t.y);
                    snap.stackSize.push_back(
                            static_cast<int>(pre_stacks[op.stack_opt_idx].units.size()));
                }
                snap.logMult = log_mult;
                snap.seed = reinterpret_cast<std::uintptr_t>(seed);
                for (Army* a : lg.units) snap.group.push_back(reinterpret_cast<std::uintptr_t>(a));
                snap.startX = lg.x;
                snap.startY = lg.y;
                snap.startCity = (lg.city && lg.city->ownerIndex == me) ? lg.city->index : NONE;
                snapshots->push_back(std::move(snap));
                break;
            }
            float chosen_log_prob = 0.0F;
            int chosen_opt_idx = ml::selectFromLogits(logits, sampler, &chosen_log_prob, &log_mult);
            const JointOption& chosen_opt = options[chosen_opt_idx];
            CandidateTarget& chosen_tgt = targets[chosen_opt.target_idx];
            std::vector<Army*> current_stack = pre_stacks[chosen_opt.stack_opt_idx].units;
            float stack_str = pre_stacks[chosen_opt.stack_opt_idx].str;

            // The per-stack critic only matters while recording a training rollout;
            // it reuses the privileged features computed as the turn began.
            const bool recording = sampler && sampler->currentTurn;
            ml::StackValueInVec pre_group_state =
                    recording ? ml::packStackValueInput(ctx, pre_stacks.back().sv,
                                                        sampler->currentTurn->startCritic)
                              : ml::StackValueInVec{};
            float pre_group_value = recording ? policy.evaluateStackValue(pre_group_state) : 0.0F;

            std::vector<std::uintptr_t> chosen_unit_ids;
            std::vector<std::uintptr_t> pool_unit_ids;
            if (sampler && sampler->currentTurn) {
                chosen_unit_ids.reserve(current_stack.size());
                for (Army* a : current_stack)
                    chosen_unit_ids.push_back(reinterpret_cast<std::uintptr_t>(a));
                const auto& full_pool = pre_stacks.back().units;
                pool_unit_ids.reserve(full_pool.size());
                for (Army* a : full_pool)
                    pool_unit_ids.push_back(reinterpret_cast<std::uintptr_t>(a));
            }

            for (size_t i = 1; i < current_stack.size(); i++) {
                auto it = std::find(pool.begin(), pool.end(), current_stack[i]);
                if (it != pool.end()) pool.erase(it);
            }

            bool is_combat_target = (chosen_tgt.kind == TargetKind::kNeutralCity ||
                                     chosen_tgt.kind == TargetKind::kEnemyCity ||
                                     chosen_tgt.kind == TargetKind::kEnemyStack);
            bool stage_near = false;
            float stage_log_prob = 0.0F;
            if (is_combat_target) {
                stage_near =
                        ml::selectBinary(chosen_opt.out.stageNearLogit, sampler, &stage_log_prob);
            }

            float score_before =
                    (sampler && sampler->currentTurn) ? ml::computeEmpireScore(g, me) : 0.0F;

            if (chosen_tgt.kind == TargetKind::kHoldGarrison) {
                for (Army* a : current_stack) a->aiMoved = true;
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

                bool ok = ExecuteStackMission(g, side, current_stack, chosen_tgt, stage_near,
                                              policy, ctx, sampler);
                if (!ok) {
                    blocked_targets.insert(chosen_opt.target_idx);
                    chosen_tgt.assigned_stacks = std::max(0, chosen_tgt.assigned_stacks - 1);
                    chosen_tgt.assigned_strength =
                            std::max(0.0F, chosen_tgt.assigned_strength - stack_str);
                    for (Army* a : current_stack) {
                        if (g.alive(a) && a->moves > 0) a->aiMoved = true;
                    }
                } else {
                    if (!current_stack.empty() && g.alive(current_stack[0])) {
                        float inv_disc =
                                chosen_opt.tv[8] > 1e-4F ? (1.0F / chosen_opt.tv[8] - 1.0F) : 0.0F;
                        side.aiThoughts.push_back(AIThought{
                            "Mission",
                            fmt("%d-unit stack (%s)", static_cast<int>(current_stack.size()),
                                current_stack[0]->name.c_str()),
                            chosen_tgt.label, current_stack[0]->x, current_stack[0]->y,
                            chosen_tgt.x, chosen_tgt.y, chosen_opt.out.utility, chosen_opt.tv[10],
                            stage_near,
                            fmt("U=%.2f Win=%.0f%% Turns=%.1f %s", chosen_opt.out.utility,
                                chosen_opt.tv[10] * 100.0F, inv_disc,
                                stage_near ? "[STAGE FOR SYNC]" : "[EXECUTE]")});
                    }

                    // Add newly revealed neutral or enemy cities immediately so remaining stacks
                    // can act on them
                    for (auto& c : g.map->cities) {
                        if (c.razed || c.ownerIndex == me || !belief::isCityVisible(g, me, c))
                            continue;
                        bool exists = false;
                        for (const auto& t : targets) {
                            if (t.city == &c) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            bool is_enemy_cap = false;
                            for (int s = 0; s < 8; s++) {
                                if (s != me && g.map->sides[s].capital == &c) is_enemy_cap = true;
                            }
                            if (c.ownerIndex == NONE) {
                                CandidateTarget neut;
                                neut.kind = TargetKind::kNeutralCity;
                                neut.x = c.x;
                                neut.y = c.y;
                                neut.city = &c;
                                neut.owner = NONE;
                                neut.value = static_cast<float>(c.income) +
                                             static_cast<float>(c.slots.size()) * 5.0F;
                                neut.is_enemy_capital = is_enemy_cap;
                                neut.label = "Capture Neutral " + c.name;
                                targets.push_back(std::move(neut));
                            } else if (Attackable(g, me, c.ownerIndex, &c)) {
                                CandidateTarget enem;
                                enem.kind = TargetKind::kEnemyCity;
                                enem.x = c.x;
                                enem.y = c.y;
                                enem.city = &c;
                                enem.owner = c.ownerIndex;
                                enem.value = static_cast<float>(c.income) +
                                             static_cast<float>(c.defence) * 6.0F;
                                enem.is_enemy_capital = is_enemy_cap;
                                enem.label = "Siege " + c.name;
                                targets.push_back(std::move(enem));
                            }
                        }
                    }
                }
            }

            // Departure credit (training only): did this move leave its own city empty?
            int vacated_city = NONE;
            if (recording && lg.city && lg.city->ownerIndex == me &&
                chosen_tgt.kind != TargetKind::kHoldGarrison) {
                bool any_left = false;
                for (int dy = 0; dy <= 1 && !any_left; dy++) {
                    for (int dx = 0; dx <= 1 && !any_left; dx++) {
                        for (Army* a : game::armiesAt(g, lg.city->x + dx, lg.city->y + dy)) {
                            if (a->owner == me) {
                                any_left = true;
                                break;
                            }
                        }
                    }
                }
                if (!any_left) vacated_city = lg.city->index;
            }

            if (sampler && sampler->currentTurn && (options.size() > 1 || is_combat_target)) {
                float score_after = ml::computeEmpireScore(g, me);
                ml::MissionStepRecord rec;
                rec.candidates.reserve(options.size());
                for (const auto& op : options) rec.candidates.push_back(op.in_vec);
                rec.logMult = std::move(log_mult);
                rec.chosenIdx = chosen_opt_idx;
                rec.oldLogProb = chosen_log_prob;
                rec.hasStageChoice = is_combat_target;
                rec.stageChosen = stage_near;
                rec.oldStageLogProb = stage_log_prob;
                rec.immediateDeltaScore = score_after - score_before;
                rec.stackValueIn = pre_group_state;
                rec.stackValuePred = pre_group_value;
                rec.unitIds = std::move(chosen_unit_ids);
                rec.poolUnitIds = std::move(pool_unit_ids);
                rec.vacatedCity = vacated_city;
                sampler->currentTurn->missionSteps.push_back(std::move(rec));
            }
        }
    }

    if (snapshots) return;
    diplomacy::setAlliedTargetCity(g, me, best_enemy_city_target);
    if (pause) pause("missions");
}

void RunEconomyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                     const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    int me = side.index;
    const SideBelief& b = belief::ensure(side);

    auto make_hold_gold_in = [&](const ml::ContextVec& cur_ctx) {
        ml::EconVec e{};
        e[0] = 1.0F;
        e[10] = std::tanh(static_cast<float>(game::upkeep(g, side) - game::income(g, side)) /
                          40.0F);
        return ml::packEconInput(cur_ctx, e);
    };

    // Army retention vs disbanding: only offered when upkeep exceeds income, the
    // one situation where the rules make disbanding worth anything. Keeping an
    // irreversible action off the table otherwise stops an untrained policy from
    // dissolving its own army during early training.
    std::vector<Army*> my_armies = game::sideArmies(g, side);
    bool over_budget = game::upkeep(g, side) > game::income(g, side);
    if (over_budget && my_armies.size() > 3) {
        for (Army* a : my_armies) {
            if (!g.alive(a) || a->hero() || a->transit || a->x == NONE) continue;
            City* c = game::cityAt(g, a->x, a->y);
            if (!c || c->ownerIndex != me) continue;
            int city_armies = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    city_armies += static_cast<int>(game::armiesAt(g, c->x + dx, c->y + dy).size());
                }
            }
            if (city_armies <= 2) continue;

            const ArmyType* at = g.types.byId(a->type);
            float threat = (c->index < static_cast<int>(b.cityThreat2Turn.size()))
                                   ? b.cityThreat2Turn[c->index]
                                   : 0.0F;
            float dist_e = (c->index < static_cast<int>(b.cityNearestEnemyDist.size()))
                                   ? b.cityNearestEnemyDist[c->index]
                                   : 100.0F;

            ml::ContextVec cur_ctx = ml::buildContext(g, side);

            ml::EconVec e{};
            e[5] = 1.0F;
            e[7] = static_cast<float>(a->strength) / 9.0F;
            e[8] = static_cast<float>(a->maxMoves) / 24.0F;
            e[10] = static_cast<float>(a->upkeep) / 15.0F;
            e[11] = (at && at->flies) ? 1.0F : 0.0F;
            e[12] = (at && (at->siege || at->bonus[48] != 0)) ? 1.0F : 0.0F;
            e[13] = (a->maxMoves >= 14 && (!at || !at->flies)) ? 1.0F : 0.0F;
            e[14] = std::tanh(threat / 25.0F);
            e[15] = std::tanh(dist_e / 60.0F);
            e[ml::ECON_CURRENT] = 1.0F;  // keeping the army is the status quo
            ml::EconInVec keep_in = ml::packEconInput(cur_ctx, e);

            ml::EconVec disband_vec = e;
            disband_vec[10] = -e[10];
            disband_vec[ml::ECON_CURRENT] = 0.0F;
            ml::EconInVec disband_in = ml::packEconInput(cur_ctx, disband_vec);

            std::vector<ml::EconInVec> cands = {keep_in, disband_in};
            std::vector<float> logits = {policy.econNet.Forward(keep_in)[0],
                                         policy.econNet.Forward(disband_in)[0]};
            float lp = 0.0F;
            int pick = ml::selectFromLogits(logits, sampler, &lp);
            if (sampler && sampler->currentTurn) {
                sampler->currentTurn->econSteps.push_back(
                        ml::EconStepRecord{std::move(cands), {}, pick, lp});
            }
            if (pick == 1) {
                game::disband(g, side, {a});
            }
        }
    }

    std::vector<City*> own_cities = game::sideCities(g, side);

    // Empire-wide slot upgrade rounds (up to 3 purchases/turn): each round asks EconNet
    // to compare HoldGold against every legal (city, unit) upgrade, stopping immediately
    // as soon as EconNet chooses HoldGold so multi-city empires neither hoard 10k gold
    // nor suffer 1 - p^N treasury-draining coin-flip cascades across N cities.
    auto menu = game::buyableTypes(g);
    for (int buy_round = 0; buy_round < 3 && !own_cities.empty() && side.gold >= 250; buy_round++) {
        struct BuyOption {
            City* city = nullptr;
            int type_id = NONE;
            ml::EconInVec in_vec{};
        };
        ml::ContextVec buy_ctx = ml::buildContext(g, side);
        std::vector<BuyOption> buy_opts;
        buy_opts.push_back(BuyOption{nullptr, NONE, make_hold_gold_in(buy_ctx)});

        for (City* c : own_cities) {
            bool full_slots = static_cast<int>(c->slots.size()) >= rules::PRODUCTION_SLOTS;
            int replace_price = (!c->slots.empty()) ? c->slots[0].price : 0;
            int best_existing_str = 0;
            for (const auto& sl : c->slots)
                best_existing_str = std::max(best_existing_str, sl.strength);
            float threat = c->index < static_cast<int>(b.cityThreat2Turn.size())
                                   ? b.cityThreat2Turn[c->index]
                                   : 0.0F;
            float dist_e = c->index < static_cast<int>(b.cityNearestEnemyDist.size())
                                   ? b.cityNearestEnemyDist[c->index]
                                   : 100.0F;
            for (const ArmyType* at : menu) {
                int price = std::abs(at->price);
                if (price <= 0 || price + 100 > side.gold) continue;
                if (full_slots && price <= replace_price) continue;
                if (!game::cannotBuy(g, side, *c, *at).empty()) continue;

                ml::EconVec e{};
                e[3] = 1.0F;
                e[7] = static_cast<float>(at->strength) / 9.0F;
                e[8] = static_cast<float>(at->move) / 24.0F;
                e[9] = static_cast<float>(at->time) / 6.0F;
                e[10] = static_cast<float>(price) /
                        std::max(100.0F, static_cast<float>(side.gold - 80));
                e[11] = at->flies ? 1.0F : 0.0F;
                e[12] = (at->siege || at->bonus[48] != 0) ? 1.0F : 0.0F;
                e[13] = (at->move >= 14 && !at->flies) ? 1.0F : 0.0F;
                e[14] = std::tanh(threat / 25.0F);
                e[15] = std::tanh(dist_e / 60.0F);
                e[16] = static_cast<float>(c->slots.size()) /
                        static_cast<float>(rules::PRODUCTION_SLOTS);
                e[17] = static_cast<float>(at->strength - best_existing_str) / 9.0F;
                e[18] = static_cast<float>(at->strength * at->strength) /
                        static_cast<float>(std::max(1, at->time * 16));
                buy_opts.push_back(BuyOption{c, at->id, ml::packEconInput(buy_ctx, e)});
            }
        }

        if (buy_opts.size() <= 1) break;
        std::vector<float> logits(buy_opts.size());
        std::vector<float> log_mult(buy_opts.size(), 0.0F);
        for (size_t i = 0; i < buy_opts.size(); i++) {
            logits[i] = policy.econNet.Forward(buy_opts[i].in_vec)[0];
            if (i > 0 && buy_opts.size() > 2)
                log_mult[i] = std::log(static_cast<float>(buy_opts.size() - 1));
        }
        float lp = 0.0F;
        int pick = ml::selectFromLogits(logits, sampler, &lp, &log_mult);
        if (sampler && sampler->currentTurn) {
            std::vector<ml::EconInVec> cands;
            cands.reserve(buy_opts.size());
            for (const auto& bo : buy_opts) cands.push_back(bo.in_vec);
            sampler->currentTurn->econSteps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, lp});
        }
        if (pick == 0 || !buy_opts[pick].city) break;

        City* best_buy_city = buy_opts[pick].city;
        int bought_type = buy_opts[pick].type_id;
        int slot_idx = game::buySlot(*best_buy_city);
        game::buyProduction(g, side, *best_buy_city, slot_idx, bought_type);
        game::sortProduction(*best_buy_city);
        for (int si = 0; si < static_cast<int>(best_buy_city->slots.size()); si++) {
            if (best_buy_city->slots[si].type == bought_type) {
                game::setProduction(g, *best_buy_city, si);
                break;
            }
        }
    }

    // City production macro-action: queried whenever a city has no active build
    // or has just completed a build (`c->countdown == slot.time`, matching
    // game.cpp:1331), so multi-turn unit builds complete without mid-countdown
    // cancellation while cities remain free to switch units after each build.
    ml::ContextVec prod_ctx = ml::buildContext(g, side);
    for (City* c : own_cities) {
        bool mid_countdown =
                (c->producing >= 0 && c->producing < static_cast<int>(c->slots.size()) &&
                 c->countdown > 0 && c->countdown < c->slots[c->producing].time);
        if (c->slots.empty() || mid_countdown) continue;
        float threat = c->index < static_cast<int>(b.cityThreat2Turn.size())
                               ? b.cityThreat2Turn[c->index]
                               : 0.0F;
        float dist_e = c->index < static_cast<int>(b.cityNearestEnemyDist.size())
                               ? b.cityNearestEnemyDist[c->index]
                               : 100.0F;
        int gar_count = 0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                gar_count += static_cast<int>(game::armiesAt(g, c->x + dx, c->y + dy).size());
            }
        }

        ml::EconVec stop_vec{};
        stop_vec[2] = 1.0F;
        stop_vec[10] = std::tanh(static_cast<float>(game::upkeep(g, side) - game::income(g, side)) /
                                 30.0F);
        stop_vec[14] = std::tanh(threat / 25.0F);
        stop_vec[15] = std::tanh(dist_e / 60.0F);
        stop_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
        stop_vec[ml::ECON_CURRENT] = (c->producing == NONE) ? 1.0F : 0.0F;

        std::vector<int> slot_indices = {NONE};
        std::vector<ml::EconInVec> cands = {ml::packEconInput(prod_ctx, stop_vec)};
        for (size_t si = 0; si < c->slots.size(); si++) {
            const Slot& sl = c->slots[si];
            const ArmyType* at = g.types.byId(sl.type);
            bool is_flier = at && at->flies;
            bool is_siege_or_spec = at && (at->siege || at->bonus[48] != 0 || at->bonus[52] != 0);
            bool is_cav = sl.move >= 14 && !is_flier;

            ml::EconVec e{};
            e[1] = 1.0F;
            e[7] = static_cast<float>(sl.strength) / 9.0F;
            e[8] = static_cast<float>(sl.move) / 24.0F;
            e[9] = static_cast<float>(sl.time) / 6.0F;
            e[10] = static_cast<float>(sl.cost) / 20.0F;
            e[11] = is_flier ? 1.0F : 0.0F;
            e[12] = is_siege_or_spec ? 1.0F : 0.0F;
            e[13] = is_cav ? 1.0F : 0.0F;
            e[14] = std::tanh(threat / 25.0F);
            e[15] = std::tanh(dist_e / 60.0F);
            e[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
            e[18] = static_cast<float>(sl.strength * sl.strength) /
                    static_cast<float>(std::max(1, sl.time * 16));
            e[ml::ECON_CURRENT] = (c->producing == static_cast<int>(si)) ? 1.0F : 0.0F;
            slot_indices.push_back(static_cast<int>(si));
            cands.push_back(ml::packEconInput(prod_ctx, e));
        }

        if (cands.size() == 1) {
            game::setProduction(g, *c, NONE);
            continue;
        }

        std::vector<float> logits(cands.size());
        std::vector<float> log_mult(cands.size(), 0.0F);
        for (size_t i = 0; i < cands.size(); i++) {
            logits[i] = policy.econNet.Forward(cands[i])[0];
            if (i > 0 && cands.size() > 2)
                log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
        }
        float lp = 0.0F;
        int pick = ml::selectFromLogits(logits, sampler, &lp, &log_mult);
        if (sampler && sampler->currentTurn) {
            sampler->currentTurn->econSteps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, lp});
        }

        if (pick == 0) {
            game::setProduction(g, *c, NONE);
        } else if (c->producing != slot_indices[pick] || !game::producing(*c)) {
            game::setProduction(g, *c, slot_indices[pick]);
        }
    }

    // City vectoring: re-evaluated each turn, but the vector already set is a
    // flagged candidate (ECON_CURRENT) rather than wiped, so keeping it is a choice
    for (City* src : own_cities) {
        if (!game::producing(*src) || src->producing < 0 ||
            src->producing >= static_cast<int>(src->slots.size())) {
            src->vectorTo = NONE;
            continue;
        }
        const Slot& sl = src->slots[src->producing];
        float src_threat = src->index < static_cast<int>(b.cityThreat2Turn.size())
                                   ? b.cityThreat2Turn[src->index]
                                   : 0.0F;
        float src_dist_e = src->index < static_cast<int>(b.cityNearestEnemyDist.size())
                                   ? b.cityNearestEnemyDist[src->index]
                                   : 100.0F;

        ml::EconVec keep_here{};
        keep_here[4] = 1.0F;
        keep_here[14] = std::tanh(src_threat / 25.0F);
        keep_here[15] = std::tanh(src_dist_e / 60.0F);
        keep_here[17] = 0.0F;
        keep_here[ml::ECON_CURRENT] = src->vectorTo == NONE ? 1.0F : 0.0F;

        std::vector<City*> dest_cities = {nullptr};
        std::vector<ml::EconInVec> cands = {ml::packEconInput(prod_ctx, keep_here)};

        for (City* dst : own_cities) {
            if (dst == src) continue;
            bool is_current = (src->vectorTo == dst->index);
            if (!is_current &&
                static_cast<int>(game::vectoredTo(g, dst).size()) >= game::MAX_VECTORED_TO)
                continue;
            int v_turns = std::max(1, move::distance(src->x, src->y, dst->x, dst->y) / 14 + 1);
            if (v_turns > 4) continue;
            float dst_threat = dst->index < static_cast<int>(b.cityThreat2Turn.size())
                                       ? b.cityThreat2Turn[dst->index]
                                       : 0.0F;
            float dst_dist_e = dst->index < static_cast<int>(b.cityNearestEnemyDist.size())
                                       ? b.cityNearestEnemyDist[dst->index]
                                       : 100.0F;
            float threat_gain =
                    (dst_threat - src_threat) / 25.0F + (src_dist_e - dst_dist_e) / 45.0F;

            ml::EconVec v{};
            v[4] = 1.0F;
            v[7] = static_cast<float>(sl.strength) / 9.0F;
            v[8] = static_cast<float>(sl.move) / 24.0F;
            v[9] = static_cast<float>(v_turns) / 4.0F;
            v[14] = std::tanh(dst_threat / 25.0F);
            v[15] = std::tanh(dst_dist_e / 60.0F);
            v[17] = std::tanh(threat_gain);
            v[ml::ECON_CURRENT] = is_current ? 1.0F : 0.0F;
            dest_cities.push_back(dst);
            cands.push_back(ml::packEconInput(prod_ctx, v));
        }

        if (cands.size() > 1) {
            std::vector<float> logits(cands.size());
            std::vector<float> log_mult(cands.size(), 0.0F);
            for (size_t i = 0; i < cands.size(); i++) {
                logits[i] = policy.econNet.Forward(cands[i])[0];
                if (i > 0 && cands.size() > 2)
                    log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
            }
            float lp = 0.0F;
            int pick = ml::selectFromLogits(logits, sampler, &lp, &log_mult);
            if (sampler && sampler->currentTurn) {
                sampler->currentTurn->econSteps.push_back(
                        ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, lp});
            }
            City* best_dest = dest_cities[pick];
            game::vector(g, *src, best_dest);
            if (best_dest) {
                side.aiThoughts.push_back(
                        AIThought{"Economy", src->name, best_dest->name, src->x, src->y,
                                  best_dest->x, best_dest->y, logits[pick], 1.0F, false,
                                  fmt("Vectoring %s from %s -> %s", sl.name.c_str(),
                                      src->name.c_str(), best_dest->name.c_str())});
            }
        }
    }
}

}  // namespace

void playTurn(Game& g, Side& side, const PauseCallback& pause,
              const ml::PolicyNetwork* custom_policy, ml::RolloutSampler* sampler) {
    const ml::PolicyNetwork& policy = custom_policy ? *custom_policy : ml::activePolicy();
    // Outside training the policy is still sampled, as in training, but at the
    // deployment temperature and from a generator seeded by the game state (never
    // advancing the game's own dice), so play stays reproducible. Taking the single
    // best-scoring option instead would follow a different policy from the one
    // trained: for a spread-out distribution its mode is not its typical behaviour.
    ml::RolloutSampler deploy_sampler;
    if (!sampler) {
        deploy_sampler.temperature = ml::deployTemperature();
        deploy_sampler.rngState = (g.rng.state * 2654435761U) ^
                                  static_cast<uint32_t>(g.turn * 40503U) ^
                                  static_cast<uint32_t>((side.index + 1) * 2246822519U);
        sampler = &deploy_sampler;
    }
    for (Army* a : g.armies) {
        if (a->owner == side.index) a->aiMoved = false;
    }
    side.aiThoughts.clear();

    std::array<bool, 8> diplo_decided{};
    for (int wave = 0; wave < 6; wave++) {
        belief::update(g, side);
        ml::ContextVec wave_ctx = ml::buildContext(g, side);
        if (wave == 0) {
            ml::CriticVec critic = ml::buildCriticFeatures(g, side);
            float v = policy.evaluateValue(wave_ctx, critic);
            side.lastBoardValue = v;
            if (sampler && sampler->currentTurn) {
                sampler->currentTurn->startCtx = wave_ctx;
                sampler->currentTurn->startCritic = critic;
                sampler->currentTurn->valuePred = v;
                sampler->currentTurn->startScore = ml::computeEmpireScore(g, side.index);
            }
        }
        RunDiplomacyPhase(g, side, policy, wave_ctx, sampler, diplo_decided);
        if (wave == 0 && pause) pause("diplomacy");
        RunMilitaryAndExplorationPhase(g, side, policy, wave_ctx, wave == 0 ? pause : nullptr,
                                       sampler);
        bool any_left = false;
        for (Army* a : g.armies) {
            if (a->owner == side.index && a->x != NONE && !a->transit && !a->aiMoved &&
                a->moves >= 2) {
                any_left = true;
                break;
            }
        }
        if (!any_left) break;
    }

    // A city emptied by one move but re-garrisoned by a later one was not left open
    if (sampler && sampler->currentTurn) {
        for (auto& ms : sampler->currentTurn->missionSteps) {
            if (ms.vacatedCity == NONE || ms.vacatedCity >= static_cast<int>(g.map->cities.size()))
                continue;
            const City& vc = g.map->cities[ms.vacatedCity];
            for (int dy = 0; dy <= 1 && ms.vacatedCity != NONE; dy++) {
                for (int dx = 0; dx <= 1 && ms.vacatedCity != NONE; dx++) {
                    for (Army* a : game::armiesAt(g, vc.x + dx, vc.y + dy)) {
                        if (a->owner == side.index) {
                            ms.vacatedCity = NONE;
                            break;
                        }
                    }
                }
            }
        }
    }

    belief::update(g, side);
    ml::ContextVec ctx = ml::buildContext(g, side);
    side.lastBoardValue = policy.evaluateValue(ctx, ml::buildCriticFeatures(g, side));

    RunEconomyPhase(g, side, policy, ctx, sampler);
    if (pause) pause("economy");
}

std::vector<MissionSnapshot> snapshotMissions(Game& g, Side& side,
                                              const ml::PolicyNetwork& policy) {
    std::vector<MissionSnapshot> out;
    std::vector<bool> moved_before;
    for (Army* a : g.armies) moved_before.push_back(a->aiMoved);
    for (Army* a : g.armies) {
        if (a->owner == side.index) a->aiMoved = false;
    }
    belief::update(g, side);
    ml::ContextVec ctx = ml::buildContext(g, side);
    RunMilitaryAndExplorationPhase(g, side, policy, ctx, nullptr, nullptr, &out);
    for (size_t i = 0; i < g.armies.size() && i < moved_before.size(); i++)
        g.armies[i]->aiMoved = moved_before[i];
    return out;
}

City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* custom_policy) {
    // No policy head is trained for where a new hero appears, so this is an
    // explicit rule rather than an untrained proxy: the own city under the
    // highest visible threat, else the game's default (the capital).
    (void)custom_policy;
    belief::update(g, side);
    const SideBelief& b = belief::ensure(side);
    City* best = dflt;
    float best_threat = 0.0F;
    for (City* c : game::sideCities(g, side)) {
        float threat = c->index < static_cast<int>(b.cityThreat2Turn.size())
                               ? b.cityThreat2Turn[c->index]
                               : 0.0F;
        if (threat > best_threat) {
            best_threat = threat;
            best = c;
        }
    }
    return best;
}

}  // namespace w2::ai::modern
