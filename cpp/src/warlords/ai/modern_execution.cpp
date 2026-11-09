/**
 * @file
 * @brief Modern AI stack mission execution, city-capture spoils decisions, and coalition sizing.
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
 * @brief Uses EconNet to choose whether to occupy, pillage, sack, or raze a newly captured `city`.
 */
void DecideCaptureSpoils(Game& game, Side& side, City& city, const std::vector<Army*>& attackers,
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx,
                         ml::RolloutSampler* sampler) {
    if (city.IsRazed() || city.OwnerIndex() != side.Index()) return;
    if (ai::QuestCapture(game, side, city, attackers)) return;

    const SideBelief& belief = belief::Ensure(side);
    float local_threat = city.Index() < static_cast<int>(belief.city_threat1_turn.size())
                                 ? belief.city_threat1_turn[city.Index()]
                                 : 0.0F;
    float dist_enemy = city.Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city.Index()]
                               : 60.0F;
    int sack_gold = ai::groups::SackValue(game, city);
    int pillage_gold =
            city.Slots().size() >= 2
                    ? std::abs(game.ArmyTypes().ById(city.Slots().back().type)->price) / 2
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

    const Map& map = game.GetMap();
    bool is_any_capital = false;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        if (map.Sides()[side_idx].CapitalCity(map) == &city) is_any_capital = true;
    }

    std::vector<int> modes = {0};
    std::vector<ml::EconInVec> cand_inputs = {
        make_spoil_in(static_cast<float>(city.Income()), 0.0F, 0.0F)};
    if (pillage_gold > 0) {
        modes.push_back(1);
        cand_inputs.push_back(make_spoil_in(static_cast<float>(city.Income()) * 0.8F,
                                            static_cast<float>(pillage_gold), 4.0F));
    }
    if (sack_gold > 0) {
        modes.push_back(2);
        cand_inputs.push_back(make_spoil_in(static_cast<float>(city.Income()) * 0.5F,
                                            static_cast<float>(sack_gold), 15.0F));
    }
    if (!is_any_capital) {
        modes.push_back(3);
        cand_inputs.push_back(make_spoil_in(0.0F, 0.0F, 38.0F));
    }

    std::vector<float> scores(cand_inputs.size());
    for (size_t i = 0; i < cand_inputs.size(); i++) {
        scores[i] = policy.EconHead().Forward(cand_inputs[i])[0];
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
                         const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                         MovedArmySet& moved_armies) {
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [&](Army* army) { return !game.Alive(army) || army->Moves() <= 0; }),
                stack.end());
    if (stack.empty()) return false;

    if (tgt.kind == TargetKind::kHoldGarrison) {
        for (Army* army : stack) moved_armies.insert(army->Id());
        return true;
    }

    int our_side = side.Index();
    std::map<Army*, int> start_moves;
    for (Army* army : stack) start_moves[army] = army->Moves();

    // A coalition drawn from one owned 2x2 city may stand on different corners.
    // Members walk (by the normal rules, paying moves) onto the leader's tile;
    // any that cannot join without breaking the 8-army stack limit stay behind
    // and remain free for later waves.
    {
        int leader_x = stack[0]->X(), leader_y = stack[0]->Y();
        int on_leader_tile = static_cast<int>(game::ArmiesAt(game, leader_x, leader_y).size());
        std::vector<Army*> joined;
        for (Army* army : stack) {
            if (army->X() == leader_x && army->Y() == leader_y) {
                joined.push_back(army);
                continue;
            }
            if (on_leader_tile + 1 > rules::kMaxStack) continue;
            auto path = move::FindPath(game, {army}, army->X(), army->Y(), leader_x, leader_y);
            if (!path || path->empty()) continue;
            move::WalkResult join_result = move::Walk(game, {army}, *path);
            if (game.Alive(army) && army->X() == leader_x && army->Y() == leader_y &&
                join_result.steps > 0) {
                ai::Walked(game, {army}, join_result);
                joined.push_back(army);
                on_leader_tile++;
            }
        }
        stack = std::move(joined);
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [&](Army* army) { return army->Moves() <= 0; }),
                    stack.end());
        if (stack.empty()) return false;
    }

    int dest_x = tgt.x, dest_y = tgt.y;
    if (tgt.city && !stage_near) {
        if (tgt.city->OwnerIndex() == our_side) {
            for (int oy = 0; oy <= 1; oy++) {
                for (int ox = 0; ox <= 1; ox++) {
                    int tile_x = tgt.city->X() + ox, tile_y = tgt.city->Y() + oy;
                    if (static_cast<int>(game::ArmiesAt(game, tile_x, tile_y).size() +
                                         stack.size()) <= rules::kMaxStack) {
                        dest_x = tile_x;
                        dest_y = tile_y;
                        break;
                    }
                }
            }
        } else {
            dest_x = tgt.city->X() + (tgt.city->X() < stack[0]->X() ? 1 : 0);
            dest_y = tgt.city->Y() + (tgt.city->Y() < stack[0]->Y() ? 1 : 0);
        }
    }

    const Map& map = game.GetMap();
    if (stage_near && (tgt.kind == TargetKind::kEnemyCity || tgt.kind == TargetKind::kNeutralCity ||
                       tgt.kind == TargetKind::kEnemyStack)) {
        int best_sx = dest_x, best_sy = dest_y;
        int best_dist = 999;
        int sx0 = stack[0]->X(), sy0 = stack[0]->Y();
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int rad = std::max(std::abs(dx), std::abs(dy));
                if (rad < 1 || rad > 2) continue;
                int next_x = tgt.x + dx, next_y = tgt.y + dy;
                if (next_x < 0 || next_y < 0 || next_x >= map.Width() || next_y >= map.Height())
                    continue;
                if (tgt.city && game::CityAt(game, next_x, next_y) == tgt.city) continue;
                if (!belief::IsTileVisible(game, our_side, next_x, next_y)) continue;
                int terr = map.TerrainTypeOf(map.TileWord(next_x, next_y));
                if (terr == move::kWater || terr == move::kShore || terr == move::kMountains)
                    continue;
                auto occ = game::ArmiesAt(game, next_x, next_y);
                if (!occ.empty() &&
                    (occ[0]->OwnerIndex() != our_side ||
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
        int leader_x = sel.leader->X(), leader_y = sel.leader->Y();
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
        if (is_fog_mission && map.GameOptions().hidden_map != 0) {
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int tile_x = leader_x + ox, tile_y = leader_y + oy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() &&
                        tile_y < map.Height() &&
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
        if (walk_result.stopped == move::WalkStop::kAttack && walk_result.attack && !stage_near) {
            attack_x = walk_result.attack->x;
            attack_y = walk_result.attack->y;
            att_city = walk_result.attack->city;
        } else if (walk_result.stopped == move::WalkStop::kAtPeace && !stage_near &&
                   walk_result.steps < static_cast<int>(walk_steps.size())) {
            const move::Step& ahead = walk_steps[walk_result.steps];
            attack_x = ahead.x;
            attack_y = ahead.y;
            att_city = game::CityAt(game, attack_x, attack_y);
        }

        if (attack_x != kNone && attack_y != kNone) {
            auto def_armies = game::ArmiesAt(game, attack_x, attack_y);
            int def_owner = def_armies.empty() ? (att_city ? att_city->OwnerIndex() : kNone)
                                               : def_armies[0]->OwnerIndex();

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
                    core::Collect(game, our_side, sel.leader->X(), sel.leader->Y(), 1);
            if (combined_here.empty()) combined_here = sel.armies;

            core::Sel comb_sel = core::Select(game, combined_here);
            int prev_owner = att_city ? att_city->OwnerIndex() : kNone;
            Battle b_res = core::Fight(game, comb_sel, attack_x, attack_y);
            made_progress = true;
            belief::RecordBattleObservation(game, our_side, attack_x, attack_y);
            if (att_city && att_city->OwnerIndex() == our_side && prev_owner != our_side &&
                !comb_sel.armies.empty()) {
                DecideCaptureSpoils(game, side, *att_city, comb_sel.armies, policy, ctx, sampler);
            }
            sel = comb_sel;

            if (!b_res.won || sel.armies.empty()) {
                break;
            }
            if (tgt.city && tgt.city->OwnerIndex() == our_side) {
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

        if (is_fog_mission && walk_result.steps > 0 && map.GameOptions().hidden_map != 0) {
            int seen_box_after = 0;
            for (int oy = -7; oy <= 7; oy++) {
                for (int ox = -7; ox <= 7; ox++) {
                    int tile_x = leader_x + ox, tile_y = leader_y + oy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() &&
                        tile_y < map.Height() &&
                        belief::IsTileVisible(game, our_side, tile_x, tile_y)) {
                        seen_box_after++;
                    }
                }
            }
            if (seen_box_after > seen_box_before ||
                (sel.leader->X() == dest_x && sel.leader->Y() == dest_y)) {
                // Yield back to the neural wave loop as soon as fog movement reveals new map tiles,
                // allowing MissionNet itself to evaluate newly uncovered castles or sites!
                completed_objective = true;
                break;
            }
        }

        if (walk_result.steps == 0 || walk_result.stopped == move::WalkStop::kOutOfMoves ||
            walk_result.stopped == move::WalkStop::kBlocked ||
            walk_result.stopped == move::WalkStop::kAtPeace) {
            if (stage_near && walk_result.steps > 0) {
                // Arrived at or advanced toward staging position
                made_progress = true;
            }
            break;
        }
    }

    // Interact with open temple or ruin/sage if standing on one with moves left
    if (!sel.armies.empty() && sel.leader && sel.min_moves > 0) {
        int final_x = sel.leader->X(), final_y = sel.leader->Y();
        Site* site = core::SiteAt(game, final_x, final_y);
        if (site && core::SiteOpen(*site)) {
            if (site->content == site::kTemple) {
                site::Search(game, sel.armies, final_x, final_y, false);
                made_progress = true;
                completed_objective = true;
                if (map.GameOptions().quests != 0 && sel.hero &&
                    (!side.ActiveQuest() || !side.ActiveQuest()->HasTarget())) {
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
        bool spent_moves = (found != start_moves.end() && army->Moves() < found->second);
        if (completed_objective && spent_moves && army->Moves() >= 2 && !stage_near &&
            tgt.kind != TargetKind::kReinforceCity) {
            // Allow remaining movement points to be dispatched in the next wave of this turn
            moved_armies.erase(army->Id());
        } else if (made_progress) {
            moved_armies.insert(army->Id());
        } else {
            moved_armies.erase(army->Id());
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
            if (seed_flies ? core::Flies(game, escort) : (escort->Moves() >= 12)) {
                fast_count++;
            } else {
                break;
            }
        }
        add_unique(fast_count);
    }
    return sizes;
}

}  // namespace w2::ai::modern
