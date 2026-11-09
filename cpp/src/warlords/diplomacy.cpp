#include "warlords/diplomacy.hpp"

#include <algorithm>
#include <cmath>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"

namespace w2::diplomacy {

const char* const kStateNames[3] = {"peace", "uneasy", "war"};
const char* const kTitles[8] = {"Statesman", "Diplomat",  "Pragmatist", "Politician",
                                "Deceiver",  "Scoundrel", "Turncoat",   "Running Dog"};

const std::vector<int>& RatingRanks(int n) {
    static const std::vector<int> kRanks[9] = {{},
                                               {1},
                                               {1, 8},
                                               {1, 4, 8},
                                               {1, 2, 6, 8},
                                               {1, 2, 4, 6, 8},
                                               {1, 2, 4, 6, 7, 8},
                                               {1, 2, 4, 5, 6, 7, 8},
                                               {1, 2, 3, 4, 5, 6, 7, 8}};
    if (n < 1 || n > 8) return kRanks[8];
    return kRanks[n];
}

/** @brief Whether both are real side indices (0-7) and different from each other. */
static bool ValidPair(int from_side, int to_side) {
    return from_side >= 0 && to_side >= 0 && from_side < 8 && to_side < 8 && from_side != to_side;
}

void Init(Game& game) {
    int start_state = game.map->options.diplomacy != 0 ? kPeace : kWar;
    game.diplomacy.Reset(start_state);
}

int State(const Game& game, int from_side, int to_side) {
    return game.diplomacy.State(SideId(from_side), SideId(to_side));
}

bool MayAttack(const Game& game, int from_side, int to_side) {
    if (from_side == kNone || to_side == kNone) return true;  // neutrals are fair game
    return State(game, from_side, to_side) != kPeace;
}

void Propose(Game& game, int from_side, int to_side, int proposed_state) {
    game.diplomacy.Propose(SideId(from_side), SideId(to_side), proposed_state);
}

int Proposal(const Game& game, int from_side, int to_side) {
    return game.diplomacy.Proposal(SideId(from_side), SideId(to_side));
}

bool IsAllied(const Game& game, int from_side, int to_side) {
    return game.diplomacy.IsAllied(SideId(from_side), SideId(to_side));
}

void SetAllied(Game& game, int from_side, int to_side, bool allied) {
    game.diplomacy.SetAllied(SideId(from_side), SideId(to_side), allied);
}

bool ProposalAllied(const Game& game, int from_side, int to_side) {
    return game.diplomacy.ProposalAllied(SideId(from_side), SideId(to_side));
}

void ProposeAlliance(Game& game, int from_side, int to_side, bool propose) {
    game.diplomacy.ProposeAlliance(SideId(from_side), SideId(to_side), propose);
}

int TruceTurns(const Game& game, int from_side, int to_side) {
    return game.diplomacy.TruceTurns(SideId(from_side), SideId(to_side));
}

void SetTruce(Game& game, int from_side, int to_side, int turns) {
    game.diplomacy.SetTruce(SideId(from_side), SideId(to_side), turns);
}

int GoldOffer(const Game& game, int from_side, int to_side) {
    return game.diplomacy.GoldOffer(SideId(from_side), SideId(to_side));
}

void SetGoldOffer(Game& game, int from_side, int to_side, int gold) {
    game.diplomacy.SetGoldOffer(SideId(from_side), SideId(to_side), gold);
}

int WarTarget(const Game& game, int from_side, int to_side) {
    return game.diplomacy.WarTarget(SideId(from_side), SideId(to_side)).Value();
}

void SetWarTarget(Game& game, int from_side, int to_side, int target_side) {
    game.diplomacy.SetWarTarget(SideId(from_side), SideId(to_side), SideId(target_side));
}

int Trust(const Game& game, int from_side, int to_side) {
    return game.diplomacy.Trust(SideId(from_side), SideId(to_side));
}

void AddTrust(Game& game, int from_side, int to_side, int delta) {
    game.diplomacy.AddTrust(SideId(from_side), SideId(to_side), delta);
}

int AlliedTargetCity(const Game& game, int side) {
    return game.diplomacy.AlliedTargetCity(SideId(side)).Value();
}

void SetAlliedTargetCity(Game& game, int side, int city_index) {
    game.diplomacy.SetAlliedTargetCity(SideId(side), CityId(city_index));
}

namespace {
/**
 * @brief Carries out the terms attached to a peace or alliance just agreed.
 *
 * Pays the gold tribute either side offered (the first side's offer wins, and
 * the payer earns some trust), then drags both sides into the joint war one of
 * them asked for. Clears the offers and war targets afterwards.
 * @param messages Receives a message for each payment and each war joined.
 */
void SettleDealTransfers(Game& game, Side& side, Side& other, std::vector<std::string>& messages) {
    int side_index = side.index, other_index = other.index;
    int side_offer = GoldOffer(game, side_index, other_index);
    int other_offer = GoldOffer(game, other_index, side_index);
    if (side_offer > 0 && side.gold > 0) {
        int paid = std::min(side.gold, side_offer);
        side.gold -= paid;
        other.gold += paid;
        AddTrust(game, other_index, side_index, std::min(30, paid / 10 + 5));
        messages.push_back(
                absl::StrFormat("%s paid %d gold in tribute to %s.", side.name, paid, other.name));
    } else if (other_offer > 0 && other.gold > 0) {
        int paid = std::min(other.gold, other_offer);
        other.gold -= paid;
        side.gold += paid;
        AddTrust(game, side_index, other_index, std::min(30, paid / 10 + 5));
        messages.push_back(
                absl::StrFormat("%s paid %d gold in tribute to %s.", other.name, paid, side.name));
    }
    SetGoldOffer(game, side_index, other_index, 0);
    SetGoldOffer(game, other_index, side_index, 0);

    int war_target = WarTarget(game, side_index, other_index);
    if (war_target == kNone) war_target = WarTarget(game, other_index, side_index);
    if (war_target >= 0 && war_target < 8 && war_target != side_index &&
        war_target != other_index && game.map->sides[war_target].alive) {
        for (int participant : {side_index, other_index}) {
            if (State(game, participant, war_target) != kWar) {
                game.diplomacy.SetSymmetricState(SideId(participant), SideId(war_target), kWar);
                game.diplomacy.SetSymmetricProposal(SideId(participant), SideId(war_target), kWar);
                SetAllied(game, participant, war_target, false);
                messages.push_back(absl::StrFormat("%s joined the war against %s!",
                                                   game.map->sides[participant].name,
                                                   game.map->sides[war_target].name));
            }
        }
    }
    SetWarTarget(game, side_index, other_index, kNone);
    SetWarTarget(game, other_index, side_index, kNone);
}
}  // namespace

std::vector<std::string> Apply(Game& game, Side& side) {
    std::vector<std::string> messages;
    int side_index = side.index;
    for (int other_index = 0; other_index < 8; other_index++) {
        Side* other = game.map->Side(other_index);
        if (other_index != side_index && other) {
            if (!side.classic_ai && TruceTurns(game, side_index, other_index) > 0 &&
                State(game, side_index, other_index) == kPeace) {
                SetTruce(game, side_index, other_index,
                         TruceTurns(game, side_index, other_index) - 1);
            }
            int current = State(game, side_index, other_index);
            int wanted = Proposal(game, side_index, other_index);
            if (wanted != current) {
                if (wanted > current) {  // escalation: at once
                    bool was_allied = IsAllied(game, side_index, other_index);
                    int truce_left = TruceTurns(game, side_index, other_index);
                    SetAllied(game, side_index, other_index, false);
                    SetTruce(game, side_index, other_index, 0);
                    game.diplomacy.SetSymmetricState(SideId(side_index), SideId(other_index),
                                                     wanted);
                    if (Proposal(game, other_index, side_index) < wanted)
                        game.diplomacy.SetProposal(SideId(other_index), SideId(side_index), wanted);
                    if (!side.classic_ai && (was_allied || (truce_left > 0 && wanted == kWar))) {
                        // Breaking an alliance or a truce: the victim and every
                        // onlooker trust the breaker less.
                        AddTrust(game, other_index, side_index, -50);
                        for (int bystander = 0; bystander < 8; bystander++) {
                            if (bystander != side_index && bystander != other_index)
                                AddTrust(game, bystander, side_index, -20);
                        }
                    }
                    if (wanted == kWar) {
                        history::RecordDeed(game, &side, DeedType::kAtWar, side_index, other_index,
                                            "");
                        messages.push_back("War declared with " + other->name + "!");
                    }
                } else if (Proposal(game, other_index, side_index) <= wanted) {
                    // de-escalation: only when both sides want it
                    game.diplomacy.SetSymmetricState(SideId(side_index), SideId(other_index),
                                                     wanted);
                    if (wanted == kPeace) {
                        history::RecordDeed(game, &side, DeedType::kAtPeace, side_index,
                                            other_index, "");
                        messages.push_back("Peace negotiated with " + other->name + "!");
                        if (!side.classic_ai || !other->classic_ai) {
                            SetTruce(game, side_index, other_index, 10);
                            AddTrust(game, side_index, other_index, 15);
                            AddTrust(game, other_index, side_index, 15);
                            SettleDealTransfers(game, side, *other, messages);
                        }
                    }
                }
            }
            // Check mutual Alliance upgrade when at Peace
            if (State(game, side_index, other_index) == kPeace &&
                !IsAllied(game, side_index, other_index) &&
                ProposalAllied(game, side_index, other_index) &&
                ProposalAllied(game, other_index, side_index)) {
                SetAllied(game, side_index, other_index, true);
                SetTruce(game, side_index, other_index, 15);
                AddTrust(game, side_index, other_index, 25);
                AddTrust(game, other_index, side_index, 25);
                SettleDealTransfers(game, side, *other, messages);
                messages.push_back("Alliance forged with " + other->name + "!");
            }
        }
    }
    return messages;
}

bool MustDeclare(const Game& game, int from_side, int to_side, const City* city) {
    if (game.map->options.diplomacy == 0 || from_side == kNone || to_side == kNone) return false;
    int current_state = State(game, from_side, to_side);
    return current_state == kPeace || (current_state == kIntermediate && city && city->index > 0);
}

void AttackAnyway(Game& game, int from_side, int to_side) {
    if (from_side == kNone || to_side == kNone) return;
    Side* side = game.map->Side(from_side);
    if (!side) return;
    int current_state = State(game, from_side, to_side);
    if (current_state == kPeace)
        game::AddDiploScore(game, *side, game.rng.Dice(1, 100, 100));
    else if (current_state == kIntermediate)
        game::AddDiploScore(game, *side, game.rng.Dice(1, 15, 10));
    SetAllied(game, from_side, to_side, false);
    SetTruce(game, from_side, to_side, 0);
    AddTrust(game, to_side, from_side, -60);
    for (int bystander = 0; bystander < 8; bystander++) {
        if (bystander != from_side && bystander != to_side)
            AddTrust(game, bystander, from_side, -25);
    }
    game.diplomacy.SetSymmetricState(SideId(from_side), SideId(to_side), kWar);
    game.diplomacy.SetSymmetricProposal(SideId(from_side), SideId(to_side), kWar);
    history::RecordDeed(game, side, DeedType::kTreachery, from_side, to_side, "");
}

void ScoreUpdate(Game& game, Side& side) {
    int side_index = side.index;
    for (int other_index = 0; other_index < 8; other_index++) {
        if (other_index != side_index) {
            int proposed = Proposal(game, side_index, other_index),
                current_state = State(game, side_index, other_index);
            // Only an overture toward peace that the other side has not yet
            // matched counts.
            if (proposed < current_state && proposed < Proposal(game, other_index, side_index))
                AddScore(game, side, current_state, proposed);
        }
    }
}

int AddScore(Game& game, Side& side, int from_state, int to_state) {
    int gain = kNone;
    if (from_state == kWar && to_state == kPeace)
        gain = game.rng.Dice(1, 10, 10);
    else if (to_state < from_state)
        gain = game.rng.Dice(1, 2, 1);
    if (gain != kNone) game::AddDiploScore(game, side, gain);
    return gain;
}

std::map<int, std::string> Ratings(const Game& game) {
    /** @brief A living side and what it is ranked by. */
    struct RankedSide {
        Side* side;  ///< The side being ranked.
        int score;   ///< Its diplomatic score; lower ranks better.
        int seq;     ///< Its place in the side list, to break ties.
    };
    std::vector<RankedSide> order;
    // only the sides still in the game are ranked
    for (size_t i = 0; i < game.sides.size(); i++) {
        if (game.sides[i]->alive)
            order.push_back(
                    RankedSide{game.sides[i], game.sides[i]->diplo_score, static_cast<int>(i)});
    }
    LuaSort(order, [](const RankedSide& lhs, const RankedSide& rhs) {
        return lhs.score != rhs.score ? lhs.score < rhs.score : lhs.seq < rhs.seq;
    });
    const std::vector<int>& ranks = RatingRanks(static_cast<int>(order.size()));
    std::map<int, std::string> titles;
    for (size_t i = 0; i < order.size(); i++) {
        int rank = i < ranks.size() ? ranks[i] : 8;
        titles[order[i].side->index] = kTitles[rank - 1];
    }
    return titles;
}

AttitudeBreakdown ComputeAttitude(const Game& game, int ai_side, int target_side) {
    AttitudeBreakdown breakdown;
    breakdown.from_side = ai_side;
    breakdown.to_side = target_side;
    if (!ValidPair(ai_side, target_side)) return breakdown;

    const Side& own_side = game.map->sides[ai_side];
    const Side& their_side = game.map->sides[target_side];
    const auto& profile = ai::personality::ForSide(own_side);
    breakdown.personality_name = profile.name;
    breakdown.personality_archetype = profile.archetype;
    breakdown.motto = profile.motto;
    breakdown.trust_score = Trust(game, ai_side, target_side);
    breakdown.truce_remaining = TruceTurns(game, ai_side, target_side);

    // Only cities the AI side can actually see count, so the attitude score (which
    // feeds the diplomacy policy) never leaks the true map under Hidden Map.
    int total_standing_cities = 0;
    int their_cities = 0, leader_cities = 0, leader_side = kNone;
    int cities_taken_from_me = 0;
    std::array<int, 8> side_cities{};
    for (const auto& city : game.map->cities) {
        if (city.razed || !ai::belief::IsCityVisible(game, ai_side, city)) continue;
        total_standing_cities++;
        if (city.owner_index >= 0 && city.owner_index < 8) {
            side_cities[city.owner_index]++;
            if (side_cities[city.owner_index] > leader_cities) {
                leader_cities = side_cities[city.owner_index];
                leader_side = city.owner_index;
            }
        }
        if (city.owner_index == target_side) {
            their_cities++;
            if (city.claim == ai_side) cities_taken_from_me++;
        }
    }

    auto push_factor = [&](const std::string& label, int delta) {
        if (delta == 0) return;
        breakdown.factors.push_back(AttitudeFactor{label, delta});
        breakdown.total_score += delta;
    };

    // Personality disposition baseline
    int base_disp =
            static_cast<int>(std::round((profile.traits[ai::personality::kHonour] -
                                         profile.traits[ai::personality::kAggression] * 0.6F) *
                                        20.0F));
    push_factor("Personality (" + profile.archetype + ")", base_disp);

    // Relational trust & treaty history
    if (breakdown.trust_score != 0) {
        push_factor("Treaty and trust track record", breakdown.trust_score / 2);
    }
    if (IsAllied(game, ai_side, target_side)) {
        push_factor("Sworn military alliance", 35);
    } else if (breakdown.truce_remaining > 0) {
        push_factor(
                absl::StrFormat("Active peace truce (%d turns left)", breakdown.truce_remaining),
                15);
    }

    // Shared enemies vs opposing wars
    int shared_wars = 0;
    for (int third_side = 0; third_side < 8; third_side++) {
        if (third_side != ai_side && third_side != target_side &&
            game.map->sides[third_side].alive) {
            if (State(game, ai_side, third_side) == kWar &&
                State(game, target_side, third_side) == kWar)
                shared_wars++;
        }
    }
    if (shared_wars > 0) {
        int bonus = shared_wars *
                    static_cast<int>(std::round(
                            18.0F + 10.0F * profile.traits[ai::personality::kCoalitionDrive]));
        push_factor(absl::StrFormat("Fighting %d common %s", shared_wars,
                                    shared_wars == 1 ? "enemy" : "enemies"),
                    bonus);
    }

    // Border military pressure (from fair-info belief if available)
    float their_border_str = 0.0F, my_border_str = 0.0F;
    if (own_side.belief) {
        their_border_str = own_side.belief->their_border_strength[target_side];
        my_border_str = own_side.belief->my_border_strength[target_side];
    }
    if (their_border_str > 4.0F) {
        int border_pen = -std::min(40, static_cast<int>(std::round(their_border_str * 1.2F)));
        push_factor("Your armies massed near our borders", border_pen);
    } else if (my_border_str > their_border_str + 12.0F &&
               profile.traits[ai::personality::kAggression] > 0.2F) {
        push_factor("Our strike forces outmatch your border garrison", -18);
    }

    // Past battle casualties & lost cities
    if (own_side.ai) {
        int grudge = own_side.ai->cities_lost[target_side] * 10 +
                     own_side.ai->heroes_killed[target_side] * 8 +
                     own_side.ai->lost[target_side] * 3;
        if (grudge > 0) {
            push_factor("Blood feud (cities and heroes lost)", -std::min(50, grudge));
        }
    }
    if (cities_taken_from_me > 0) {
        push_factor(absl::StrFormat("You hold %d of our claimed cities", cities_taken_from_me),
                    -cities_taken_from_me * 8);
    }
    if (own_side.capital && own_side.capital->owner_index == target_side) {
        push_factor("You occupy our ancestral Capital!", -60);
    }

    // Balance of power / anti-runaway coalition logic
    float their_share = static_cast<float>(their_cities) /
                        static_cast<float>(std::max(1, total_standing_cities));
    if (leader_side == target_side && their_share >= 0.30F) {
        int hegemony_pen = -static_cast<int>(
                std::round((their_share - 0.25F) * 120.0F *
                           (1.0F + 0.5F * profile.traits[ai::personality::kCoalitionDrive])));
        push_factor(absl::StrFormat("Fear of your growing hegemony (%d%% of cities)",
                                    static_cast<int>(std::round(their_share * 100.0F))),
                    hegemony_pen);
    } else if (leader_side != kNone && leader_side != ai_side && leader_side != target_side &&
               static_cast<float>(leader_cities) /
                               static_cast<float>(std::max(1, total_standing_cities)) >=
                       0.32F) {
        push_factor("Need for coalition against the runaway leader", 22);
    }

    // Diplomatic reputation / atrocities
    if (their_side.diplo_score > 60) {
        int atrocity_pen = -std::min(35, (their_side.diplo_score - 40) / 4);
        if (profile.traits[ai::personality::kHonour] > 0.0F) {
            push_factor("Your reputation for razing and treachery", atrocity_pen);
        }
    }

    // Active tribute offer on the table
    int offered_to_ai = GoldOffer(game, target_side, ai_side);
    if (offered_to_ai > 0) {
        int gold_bonus = static_cast<int>(
                std::round(static_cast<float>(offered_to_ai) / 8.0F *
                           (1.0F + 0.6F * profile.traits[ai::personality::kAvarice])));
        push_factor(absl::StrFormat("Gold tribute offered (%d gold)", offered_to_ai), gold_bonus);
    }

    breakdown.total_score = std::clamp(breakdown.total_score, -100, 100);

    // Compute clear stance summary and "What It Takes" forecast
    int cur_state = State(game, ai_side, target_side);
    if (IsAllied(game, ai_side, target_side)) {
        breakdown.stance_summary = "Allied Partner";
        breakdown.what_it_takes = "Alliance is strong. Coordinate strikes against shared foes!";
        breakdown.peace_gold_needed = 0;
        breakdown.ally_gold_needed = 0;
    } else if (own_side.capital && own_side.capital->owner_index == target_side) {
        breakdown.stance_summary = "Mortal Enemy (Capital Held)";
        breakdown.what_it_takes = "Refuses Peace while you hold " + own_side.capital->name + "!";
        breakdown.peace_gold_needed = kNone;
        breakdown.ally_gold_needed = kNone;
    } else {
        float greed_mult = std::max(0.5F, 1.2F - 0.5F * profile.traits[ai::personality::kAvarice]);
        if (breakdown.total_score >= 35) {
            breakdown.stance_summary = "Friendly, Cooperative";
            breakdown.peace_gold_needed = 0;
            breakdown.ally_gold_needed = 0;
            breakdown.what_it_takes = "Willing to accept Peace or forge a full Alliance!";
        } else if (breakdown.total_score >= 0) {
            breakdown.stance_summary = (cur_state == kWar) ? "Weary of War" : "Cautious, Pragmatic";
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
            if (profile.traits[ai::personality::kAvarice] > 0.5F && breakdown.total_score > -75) {
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

}  // namespace w2::diplomacy
