/**
 * @file
 * @brief Implementation of `w2::ai::EvaluateSideProposal`.
 */
#include "warlords/ai/attitude.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "absl/strings/str_format.h"

#include "warlords/ai/belief.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"

namespace w2::ai {

AttitudeBreakdown EvaluateSideProposal(const Game& game, SideId ai_side_id, SideId target_side_id,
                                       const SideBelief* belief, const BattleGrudge* grudge) {
    int ai_side = ai_side_id.Value();
    int target_side = target_side_id.Value();
    AttitudeBreakdown breakdown;
    breakdown.from_side = ai_side;
    breakdown.to_side = target_side;
    if (ai_side < 0 || target_side < 0 || ai_side >= 8 || target_side >= 8 ||
        ai_side == target_side) {
        return breakdown;
    }

    const Side& own_side = game.map->sides[ai_side];
    const Side& their_side = game.map->sides[target_side];
    const auto& profile = personality::ForSide(own_side);
    breakdown.personality_name = profile.name;
    breakdown.personality_archetype = profile.archetype;
    breakdown.motto = profile.motto;
    breakdown.trust_score = diplomacy::Trust(game, ai_side, target_side);
    breakdown.truce_remaining = diplomacy::TruceTurns(game, ai_side, target_side);

    int total_standing_cities = 0;
    int their_cities = 0, leader_cities = 0, leader_side = kNone;
    int cities_taken_from_me = 0;
    std::array<int, 8> side_cities{};
    for (const auto& city : game.map->cities) {
        if (city.IsRazed() || !belief::IsCityVisible(game, ai_side, city)) continue;
        total_standing_cities++;
        int owner_idx = city.OwnerIndex();
        if (owner_idx >= 0 && owner_idx < 8) {
            side_cities[owner_idx]++;
            if (side_cities[owner_idx] > leader_cities) {
                leader_cities = side_cities[owner_idx];
                leader_side = owner_idx;
            }
        }
        if (owner_idx == target_side) {
            their_cities++;
            if (city.Claim() == ai_side_id) cities_taken_from_me++;
        }
    }

    auto push_factor = [&](const std::string& label, int delta) {
        if (delta == 0) return;
        breakdown.factors.push_back(AttitudeFactor{label, delta});
        breakdown.total_score += delta;
    };

    int base_disp = static_cast<int>(std::round((profile.traits[personality::kHonour] -
                                                 profile.traits[personality::kAggression] * 0.6F) *
                                                20.0F));
    push_factor("Personality (" + profile.archetype + ")", base_disp);

    if (breakdown.trust_score != 0) {
        push_factor("Treaty and trust track record", breakdown.trust_score / 2);
    }
    if (diplomacy::IsAllied(game, ai_side, target_side)) {
        push_factor("Sworn military alliance", 35);
    } else if (breakdown.truce_remaining > 0) {
        push_factor(
                absl::StrFormat("Active peace truce (%d turns left)", breakdown.truce_remaining),
                15);
    }

    int shared_wars = 0;
    for (int third_side = 0; third_side < 8; third_side++) {
        if (third_side != ai_side && third_side != target_side &&
            game.map->sides[third_side].IsAlive()) {
            if (diplomacy::State(game, ai_side, third_side) == diplomacy::kWar &&
                diplomacy::State(game, target_side, third_side) == diplomacy::kWar) {
                shared_wars++;
            }
        }
    }
    if (shared_wars > 0) {
        int bonus = shared_wars *
                    static_cast<int>(std::round(
                            18.0F + 10.0F * profile.traits[personality::kCoalitionDrive]));
        push_factor(absl::StrFormat("Fighting %d common %s", shared_wars,
                                    shared_wars == 1 ? "enemy" : "enemies"),
                    bonus);
    }

    float their_border_str = 0.0F, my_border_str = 0.0F;
    if (belief) {
        their_border_str = belief->their_border_strength[target_side];
        my_border_str = belief->my_border_strength[target_side];
    }
    if (their_border_str > 4.0F) {
        int border_pen = -std::min(40, static_cast<int>(std::round(their_border_str * 1.2F)));
        push_factor("Your armies massed near our borders", border_pen);
    } else if (my_border_str > their_border_str + 12.0F &&
               profile.traits[personality::kAggression] > 0.2F) {
        push_factor("Our strike forces outmatch your border garrison", -18);
    }

    if (grudge) {
        int feud = grudge->cities_lost[target_side] * 10 + grudge->heroes_killed[target_side] * 8 +
                   grudge->lost[target_side] * 3;
        if (feud > 0) {
            push_factor("Blood feud (cities and heroes lost)", -std::min(50, feud));
        }
    }
    if (cities_taken_from_me > 0) {
        push_factor(absl::StrFormat("You hold %d of our claimed cities", cities_taken_from_me),
                    -cities_taken_from_me * 8);
    }
    const City* own_capital = own_side.CapitalCity(*game.map);
    if (own_capital && own_capital->OwnerIndex() == target_side) {
        push_factor("You occupy our ancestral Capital!", -60);
    }

    float their_share = static_cast<float>(their_cities) /
                        static_cast<float>(std::max(1, total_standing_cities));
    if (leader_side == target_side && their_share >= 0.30F) {
        int hegemony_pen = -static_cast<int>(
                std::round((their_share - 0.25F) * 120.0F *
                           (1.0F + 0.5F * profile.traits[personality::kCoalitionDrive])));
        push_factor(absl::StrFormat("Fear of your growing hegemony (%d%% of cities)",
                                    static_cast<int>(std::round(their_share * 100.0F))),
                    hegemony_pen);
    } else if (leader_side != kNone && leader_side != ai_side && leader_side != target_side &&
               static_cast<float>(leader_cities) /
                               static_cast<float>(std::max(1, total_standing_cities)) >=
                       0.32F) {
        push_factor("Need for coalition against the runaway leader", 22);
    }

    if (their_side.DiploScore() > 60) {
        int atrocity_pen = -std::min(35, (their_side.DiploScore() - 40) / 4);
        if (profile.traits[personality::kHonour] > 0.0F) {
            push_factor("Your reputation for razing and treachery", atrocity_pen);
        }
    }

    int offered_to_ai = diplomacy::GoldOffer(game, target_side, ai_side);
    if (offered_to_ai > 0) {
        int gold_bonus =
                static_cast<int>(std::round(static_cast<float>(offered_to_ai) / 8.0F *
                                            (1.0F + 0.6F * profile.traits[personality::kAvarice])));
        push_factor(absl::StrFormat("Gold tribute offered (%d gold)", offered_to_ai), gold_bonus);
    }

    breakdown.total_score = std::clamp(breakdown.total_score, -100, 100);

    int cur_state = diplomacy::State(game, ai_side, target_side);
    if (diplomacy::IsAllied(game, ai_side, target_side)) {
        breakdown.stance_summary = "Allied Partner";
        breakdown.what_it_takes = "Alliance is strong. Coordinate strikes against shared foes!";
        breakdown.peace_gold_needed = 0;
        breakdown.ally_gold_needed = 0;
    } else if (own_capital && own_capital->OwnerIndex() == target_side) {
        breakdown.stance_summary = "Mortal Enemy (Capital Held)";
        breakdown.what_it_takes = "Refuses Peace while you hold " + own_capital->Name() + "!";
        breakdown.peace_gold_needed = kNone;
        breakdown.ally_gold_needed = kNone;
    } else {
        float greed_mult = std::max(0.5F, 1.2F - 0.5F * profile.traits[personality::kAvarice]);
        if (breakdown.total_score >= 35) {
            breakdown.stance_summary = "Friendly, Cooperative";
            breakdown.peace_gold_needed = 0;
            breakdown.ally_gold_needed = 0;
            breakdown.what_it_takes = "Willing to accept Peace or forge a full Alliance!";
        } else if (breakdown.total_score >= 0) {
            breakdown.stance_summary =
                    (cur_state == diplomacy::kWar) ? "Weary of War" : "Cautious, Pragmatic";
            breakdown.peace_gold_needed = 0;
            breakdown.ally_gold_needed = std::max(
                    50,
                    static_cast<int>(std::round((35 - breakdown.total_score) * 8.0F / greed_mult)) /
                            25 * 25);
            breakdown.what_it_takes =
                    absl::StrFormat("Accepts Peace freely; Alliance for ~%d gold (or shared war).",
                                    breakdown.ally_gold_needed);
        } else if (breakdown.total_score >= -45) {
            breakdown.stance_summary = "Hostile, Distrustful";
            breakdown.peace_gold_needed =
                    std::max(50, static_cast<int>(std::round((-breakdown.total_score + 10) * 6.0F /
                                                             greed_mult)) /
                                         25 * 25);
            breakdown.ally_gold_needed = kNone;
            breakdown.what_it_takes =
                    absl::StrFormat("Demands %d gold tribute (or border withdrawal) for Peace.",
                                    breakdown.peace_gold_needed);
        } else {
            breakdown.stance_summary = "Implacable Warpath";
            if (profile.traits[personality::kAvarice] > 0.5F && breakdown.total_score > -75) {
                breakdown.peace_gold_needed = std::max(
                        150,
                        static_cast<int>(std::round((-breakdown.total_score) * 8.0F)) / 50 * 50);
                breakdown.what_it_takes =
                        absl::StrFormat("Only a heavy tribute of %d gold will stay their hand!",
                                        breakdown.peace_gold_needed);
            } else {
                breakdown.peace_gold_needed = kNone;
                breakdown.what_it_takes =
                        "Refuses Peace: determined to settle grievances on the battlefield!";
            }
            breakdown.ally_gold_needed = kNone;
        }
    }

    return breakdown;
}

}  // namespace w2::ai
