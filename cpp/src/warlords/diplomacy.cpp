/**
 * @file
 * @brief Implementation of `w2::diplomacy` rules and treaty transfers.
 */
#include "warlords/diplomacy.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
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

namespace {

bool ValidPair(int from_side, int to_side) {
    return from_side >= 0 && to_side >= 0 && from_side < 8 && to_side < 8 && from_side != to_side;
}

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

void Init(Game& game) {
    int start_state = game.map->options.diplomacy != 0 ? kPeace : kWar;
    game.diplomacy.Reset(start_state);
}

int State(const Game& game, int from_side, int to_side) {
    return game.diplomacy.State(SideId(from_side), SideId(to_side));
}

bool MayAttack(const Game& game, int from_side, int to_side) {
    if (from_side == kNone || to_side == kNone) return true;
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

std::vector<std::string> Apply(Game& game, Side& side) {
    std::vector<std::string> messages;
    int side_index = side.index;
    bool classic = side.IsClassicAi();
    for (int other_index = 0; other_index < 8; other_index++) {
        Side* other = game.map->Side(other_index);
        if (other_index != side_index && other) {
            if (!classic && TruceTurns(game, side_index, other_index) > 0 &&
                State(game, side_index, other_index) == kPeace) {
                SetTruce(game, side_index, other_index,
                         TruceTurns(game, side_index, other_index) - 1);
            }
            int current = State(game, side_index, other_index);
            int wanted = Proposal(game, side_index, other_index);
            if (wanted != current) {
                if (wanted > current) {
                    bool was_allied = IsAllied(game, side_index, other_index);
                    int truce_left = TruceTurns(game, side_index, other_index);
                    SetAllied(game, side_index, other_index, false);
                    SetTruce(game, side_index, other_index, 0);
                    game.diplomacy.SetSymmetricState(SideId(side_index), SideId(other_index),
                                                     wanted);
                    if (Proposal(game, other_index, side_index) < wanted)
                        game.diplomacy.SetProposal(SideId(other_index), SideId(side_index), wanted);
                    if (!classic && (was_allied || (truce_left > 0 && wanted == kWar))) {
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
                    game.diplomacy.SetSymmetricState(SideId(side_index), SideId(other_index),
                                                     wanted);
                    if (wanted == kPeace) {
                        history::RecordDeed(game, &side, DeedType::kAtPeace, side_index,
                                            other_index, "");
                        messages.push_back("Peace negotiated with " + other->name + "!");
                        if (!classic || !other->IsClassicAi()) {
                            SetTruce(game, side_index, other_index, 10);
                            AddTrust(game, side_index, other_index, 15);
                            AddTrust(game, other_index, side_index, 15);
                            SettleDealTransfers(game, side, *other, messages);
                        }
                    }
                }
            }
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
            int proposed = Proposal(game, side_index, other_index);
            int current_state = State(game, side_index, other_index);
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
    struct RankedSide {
        Side* side;
        int score;
        int seq;
    };
    std::vector<RankedSide> order;
    for (size_t i = 0; i < game.sides.size(); i++) {
        if (game.sides[i]->alive) {
            order.push_back(
                    RankedSide{game.sides[i], game.sides[i]->diplo_score, static_cast<int>(i)});
        }
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
    if (own_side.computer_player) {
        return own_side.computer_player->EvaluateProposal(game, SideId(ai_side),
                                                          SideId(target_side));
    }
    return breakdown;
}

}  // namespace w2::diplomacy
