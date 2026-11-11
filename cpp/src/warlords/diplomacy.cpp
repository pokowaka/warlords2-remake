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
    int side_index = side.Index(), other_index = other.Index();
    int side_offer = GoldOffer(game, side_index, other_index);
    int other_offer = GoldOffer(game, other_index, side_index);
    if (side_offer > 0 && side.Gold() > 0) {
        int paid = std::min(side.Gold(), side_offer);
        side.AddGold(-paid);
        other.AddGold(paid);
        AddTrust(game, other_index, side_index, std::min(30, paid / 10 + 5));
        if (other.ComputerPlayer()) {
            other.ComputerPlayer()->RecordTributeReceived(side.Id(), paid);
        }
        messages.push_back(absl::StrFormat("%s paid %d gold in tribute to %s.", side.Name(), paid,
                                           other.Name()));
    } else if (other_offer > 0 && other.Gold() > 0) {
        int paid = std::min(other.Gold(), other_offer);
        other.AddGold(-paid);
        side.AddGold(paid);
        AddTrust(game, side_index, other_index, std::min(30, paid / 10 + 5));
        if (side.ComputerPlayer()) {
            side.ComputerPlayer()->RecordTributeReceived(other.Id(), paid);
        }
        messages.push_back(absl::StrFormat("%s paid %d gold in tribute to %s.", other.Name(), paid,
                                           side.Name()));
    }
    SetGoldOffer(game, side_index, other_index, 0);
    SetGoldOffer(game, other_index, side_index, 0);

    int war_target = WarTarget(game, side_index, other_index);
    if (war_target == kNone) war_target = WarTarget(game, other_index, side_index);
    const auto& map_sides = game.GetMap().Sides();
    if (war_target >= 0 && war_target < 8 && war_target != side_index &&
        war_target != other_index && map_sides[war_target].IsAlive()) {
        for (int participant : {side_index, other_index}) {
            if (State(game, participant, war_target) != kWar) {
                game.Diplomacy().SetSymmetricState(SideId(participant), SideId(war_target), kWar);
                game.Diplomacy().SetSymmetricProposal(SideId(participant), SideId(war_target),
                                                      kWar);
                SetAllied(game, participant, war_target, false);
                messages.push_back(absl::StrFormat("%s joined the war against %s!",
                                                   map_sides[participant].Name(),
                                                   map_sides[war_target].Name()));
            }
        }
    }
    SetWarTarget(game, side_index, other_index, kNone);
    SetWarTarget(game, other_index, side_index, kNone);
}

}  // namespace

void Init(Game& game) {
    int start_state = game.GetMap().GameOptions().diplomacy != 0 ? kPeace : kWar;
    game.Diplomacy().Reset(start_state);
}

int State(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().State(SideId(from_side), SideId(to_side));
}

bool MayAttack(const Game& game, int from_side, int to_side) {
    if (from_side == kNone || to_side == kNone) return true;
    return State(game, from_side, to_side) != kPeace;
}

void Propose(Game& game, int from_side, int to_side, int proposed_state) {
    game.Diplomacy().Propose(SideId(from_side), SideId(to_side), proposed_state);
}

int Proposal(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().Proposal(SideId(from_side), SideId(to_side));
}

bool IsAllied(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().IsAllied(SideId(from_side), SideId(to_side));
}

void SetAllied(Game& game, int from_side, int to_side, bool allied) {
    game.Diplomacy().SetAllied(SideId(from_side), SideId(to_side), allied);
}

bool ProposalAllied(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().ProposalAllied(SideId(from_side), SideId(to_side));
}

void ProposeAlliance(Game& game, int from_side, int to_side, bool propose) {
    game.Diplomacy().ProposeAlliance(SideId(from_side), SideId(to_side), propose);
}

int TruceTurns(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().TruceTurns(SideId(from_side), SideId(to_side));
}

void SetTruce(Game& game, int from_side, int to_side, int turns) {
    game.Diplomacy().SetTruce(SideId(from_side), SideId(to_side), turns);
}

int GoldOffer(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().GoldOffer(SideId(from_side), SideId(to_side));
}

void SetGoldOffer(Game& game, int from_side, int to_side, int gold) {
    game.Diplomacy().SetGoldOffer(SideId(from_side), SideId(to_side), gold);
}

int WarTarget(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().WarTarget(SideId(from_side), SideId(to_side)).Value();
}

void SetWarTarget(Game& game, int from_side, int to_side, int target_side) {
    game.Diplomacy().SetWarTarget(SideId(from_side), SideId(to_side), SideId(target_side));
}

int Trust(const Game& game, int from_side, int to_side) {
    return game.Diplomacy().Trust(SideId(from_side), SideId(to_side));
}

void AddTrust(Game& game, int from_side, int to_side, int delta) {
    game.Diplomacy().AddTrust(SideId(from_side), SideId(to_side), delta);
}

int AlliedTargetCity(const Game& game, int side) {
    return game.Diplomacy().AlliedTargetCity(SideId(side)).Value();
}

void SetAlliedTargetCity(Game& game, int side, int city_index) {
    game.Diplomacy().SetAlliedTargetCity(SideId(side), CityId(city_index));
}

std::vector<std::string> Apply(Game& game, Side& side) {
    std::vector<std::string> messages;
    int side_index = side.Index();
    bool classic = side.IsClassicAi();
    for (int other_index = 0; other_index < 8; other_index++) {
        Side* other = game.GetMap().SideAt(other_index);
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
                    game.Diplomacy().SetSymmetricState(SideId(side_index), SideId(other_index),
                                                       wanted);
                    if (Proposal(game, other_index, side_index) < wanted)
                        game.Diplomacy().SetProposal(SideId(other_index), SideId(side_index),
                                                     wanted);
                    if (!classic && (was_allied || (truce_left > 0 && wanted == kWar))) {
                        AddTrust(game, other_index, side_index, -50);
                        if (other->ComputerPlayer()) {
                            other->ComputerPlayer()->RecordBetrayal(side.Id(), was_allied,
                                                                    game.Turn());
                        }
                        for (int bystander = 0; bystander < 8; bystander++) {
                            if (bystander != side_index && bystander != other_index)
                                AddTrust(game, bystander, side_index, -20);
                        }
                    }
                    if (wanted == kWar) {
                        history::RecordDeed(game, &side, DeedType::kAtWar, side_index, other_index,
                                            "");
                        messages.push_back("War declared with " + other->Name() + "!");
                    }
                } else if (Proposal(game, other_index, side_index) <= wanted) {
                    game.Diplomacy().SetSymmetricState(SideId(side_index), SideId(other_index),
                                                       wanted);
                    if (wanted == kPeace) {
                        history::RecordDeed(game, &side, DeedType::kAtPeace, side_index,
                                            other_index, "");
                        messages.push_back("Peace negotiated with " + other->Name() + "!");
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
                messages.push_back("Alliance forged with " + other->Name() + "!");
            }
        }
    }
    return messages;
}

bool MustDeclare(const Game& game, int from_side, int to_side, const City* city) {
    if (game.GetMap().GameOptions().diplomacy == 0 || from_side == kNone || to_side == kNone)
        return false;
    int current_state = State(game, from_side, to_side);
    return current_state == kPeace || (current_state == kIntermediate && city && city->Index() > 0);
}

void AttackAnyway(Game& game, int from_side, int to_side) {
    if (from_side == kNone || to_side == kNone) return;
    Side* side = game.GetMap().SideAt(from_side);
    if (!side) return;
    int current_state = State(game, from_side, to_side);
    bool was_allied = IsAllied(game, from_side, to_side);
    if (current_state == kPeace)
        game::AddDiploScore(game, *side, game.GetRng().Dice(1, 100, 100));
    else if (current_state == kIntermediate)
        game::AddDiploScore(game, *side, game.GetRng().Dice(1, 15, 10));
    SetAllied(game, from_side, to_side, false);
    SetTruce(game, from_side, to_side, 0);
    AddTrust(game, to_side, from_side, -60);
    if (current_state == kPeace || current_state == kIntermediate) {
        if (Side* victim = game.GetMap().SideAt(to_side); victim && victim->ComputerPlayer()) {
            victim->ComputerPlayer()->RecordBetrayal(SideId(from_side), was_allied, game.Turn());
        }
    }
    for (int bystander = 0; bystander < 8; bystander++) {
        if (bystander != from_side && bystander != to_side)
            AddTrust(game, bystander, from_side, -25);
    }
    game.Diplomacy().SetSymmetricState(SideId(from_side), SideId(to_side), kWar);
    game.Diplomacy().SetSymmetricProposal(SideId(from_side), SideId(to_side), kWar);
    history::RecordDeed(game, side, DeedType::kTreachery, from_side, to_side, "");
}

void ScoreUpdate(Game& game, Side& side) {
    int side_index = side.Index();
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
        gain = game.GetRng().Dice(1, 10, 10);
    else if (to_state < from_state)
        gain = game.GetRng().Dice(1, 2, 1);
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
    for (size_t i = 0; i < game.Sides().size(); i++) {
        if (game.Sides()[i]->IsAlive()) {
            order.push_back(RankedSide{game.Sides()[i], game.Sides()[i]->DiploScore(),
                                       static_cast<int>(i)});
        }
    }
    LuaSort(order, [](const RankedSide& lhs, const RankedSide& rhs) {
        return lhs.score != rhs.score ? lhs.score < rhs.score : lhs.seq < rhs.seq;
    });
    const std::vector<int>& ranks = RatingRanks(static_cast<int>(order.size()));
    std::map<int, std::string> titles;
    for (size_t i = 0; i < order.size(); i++) {
        int rank = i < ranks.size() ? ranks[i] : 8;
        titles[order[i].side->Index()] = kTitles[rank - 1];
    }
    return titles;
}

AttitudeBreakdown ComputeAttitude(const Game& game, int ai_side, int target_side) {
    AttitudeBreakdown breakdown;
    breakdown.from_side = ai_side;
    breakdown.to_side = target_side;
    if (!ValidPair(ai_side, target_side)) return breakdown;
    const Side& own_side = game.GetMap().Sides()[ai_side];
    if (own_side.ComputerPlayer()) {
        return own_side.ComputerPlayer()->EvaluateProposal(game, SideId(ai_side),
                                                           SideId(target_side));
    }
    return breakdown;
}

}  // namespace w2::diplomacy
