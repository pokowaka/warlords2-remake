/**
 * @file
 * @brief Implementation of `w2::DiplomacyMatrix`.
 */
#include "warlords/model/diplomacy_state.hpp"

#include <algorithm>
#include <cstddef>

namespace w2 {
namespace {

constexpr int kPeaceState = 0;
constexpr int kWarState = 2;

bool ValidSide(SideId side) {
    return side.Value() >= 0 && side.Value() < 8;
}

bool ValidPairIncludeSelf(SideId from_side, SideId to_side) {
    return ValidSide(from_side) && ValidSide(to_side);
}

bool ValidDistinctPair(SideId from_side, SideId to_side) {
    return ValidSide(from_side) && ValidSide(to_side) && from_side != to_side;
}

size_t PairKey(SideId from_side, SideId to_side) {
    return static_cast<size_t>(from_side.Value() * 8 + to_side.Value());
}

}  // namespace

DiplomacyMatrix::DiplomacyMatrix() {
    war_target_.fill(kNone);
    allied_target_city_.fill(kNone);
}

void DiplomacyMatrix::Reset(int initial_state) {
    state_.fill(initial_state);
    proposed_.fill(initial_state);
    allied_.fill(false);
    propose_allied_.fill(false);
    truce_turns_.fill(0);
    gold_offer_.fill(0);
    war_target_.fill(kNone);
    trust_.fill(0);
    allied_target_city_.fill(kNone);
}

int DiplomacyMatrix::State(SideId from_side, SideId to_side) const {
    if (!ValidPairIncludeSelf(from_side, to_side)) return kWarState;
    int stored = state_[PairKey(from_side, to_side)];
    return stored == kNone ? kWarState : stored;
}

void DiplomacyMatrix::SetState(SideId from_side, SideId to_side, int state) {
    if (!ValidPairIncludeSelf(from_side, to_side)) return;
    state_[PairKey(from_side, to_side)] = state;
}

void DiplomacyMatrix::SetSymmetricState(SideId first_side, SideId second_side, int state) {
    SetState(first_side, second_side, state);
    SetState(second_side, first_side, state);
}

int DiplomacyMatrix::Proposal(SideId from_side, SideId to_side) const {
    if (!ValidPairIncludeSelf(from_side, to_side)) return State(from_side, to_side);
    int stored = proposed_[PairKey(from_side, to_side)];
    return stored == kNone ? State(from_side, to_side) : stored;
}

void DiplomacyMatrix::SetProposal(SideId from_side, SideId to_side, int proposed_state) {
    if (!ValidPairIncludeSelf(from_side, to_side)) return;
    proposed_[PairKey(from_side, to_side)] = proposed_state;
}

void DiplomacyMatrix::SetSymmetricProposal(SideId first_side, SideId second_side,
                                           int proposed_state) {
    SetProposal(first_side, second_side, proposed_state);
    SetProposal(second_side, first_side, proposed_state);
}

void DiplomacyMatrix::Propose(SideId from_side, SideId to_side, int proposed_state) {
    if (!ValidDistinctPair(from_side, to_side)) return;
    size_t key = PairKey(from_side, to_side);
    proposed_[key] = proposed_state;
    if (proposed_state != kPeaceState) {
        propose_allied_[key] = false;
    }
}

bool DiplomacyMatrix::IsAllied(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return false;
    return State(from_side, to_side) == kPeaceState && allied_[PairKey(from_side, to_side)];
}

bool DiplomacyMatrix::AlliedFlag(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return false;
    return allied_[PairKey(from_side, to_side)];
}

void DiplomacyMatrix::SetAllied(SideId first_side, SideId second_side, bool allied) {
    if (!ValidDistinctPair(first_side, second_side)) return;
    size_t forward_key = PairKey(first_side, second_side);
    size_t reverse_key = PairKey(second_side, first_side);
    if (allied) {
        state_[forward_key] = kPeaceState;
        state_[reverse_key] = kPeaceState;
        proposed_[forward_key] = kPeaceState;
        proposed_[reverse_key] = kPeaceState;
    }
    allied_[forward_key] = allied;
    allied_[reverse_key] = allied;
    propose_allied_[forward_key] = allied;
    propose_allied_[reverse_key] = allied;
}

bool DiplomacyMatrix::ProposalAllied(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return false;
    return propose_allied_[PairKey(from_side, to_side)];
}

void DiplomacyMatrix::ProposeAlliance(SideId from_side, SideId to_side, bool propose) {
    if (!ValidDistinctPair(from_side, to_side)) return;
    size_t key = PairKey(from_side, to_side);
    propose_allied_[key] = propose;
    if (propose) {
        proposed_[key] = kPeaceState;
    }
}

int DiplomacyMatrix::TruceTurns(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return 0;
    return truce_turns_[PairKey(from_side, to_side)];
}

void DiplomacyMatrix::SetTruce(SideId first_side, SideId second_side, int turns) {
    if (!ValidDistinctPair(first_side, second_side)) return;
    int clamped = std::max(0, turns);
    truce_turns_[PairKey(first_side, second_side)] = clamped;
    truce_turns_[PairKey(second_side, first_side)] = clamped;
}

int DiplomacyMatrix::GoldOffer(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return 0;
    return gold_offer_[PairKey(from_side, to_side)];
}

void DiplomacyMatrix::SetGoldOffer(SideId from_side, SideId to_side, int gold) {
    if (!ValidDistinctPair(from_side, to_side)) return;
    gold_offer_[PairKey(from_side, to_side)] = gold;
}

SideId DiplomacyMatrix::WarTarget(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return SideId::None();
    return SideId(war_target_[PairKey(from_side, to_side)]);
}

void DiplomacyMatrix::SetWarTarget(SideId from_side, SideId to_side, SideId target_side) {
    if (!ValidDistinctPair(from_side, to_side)) return;
    war_target_[PairKey(from_side, to_side)] = target_side.Value();
}

int DiplomacyMatrix::Trust(SideId from_side, SideId to_side) const {
    if (!ValidDistinctPair(from_side, to_side)) return 0;
    return trust_[PairKey(from_side, to_side)];
}

void DiplomacyMatrix::AddTrust(SideId from_side, SideId to_side, int delta) {
    if (!ValidDistinctPair(from_side, to_side)) return;
    size_t key = PairKey(from_side, to_side);
    trust_[key] = std::clamp(trust_[key] + delta, -100, 100);
}

CityId DiplomacyMatrix::AlliedTargetCity(SideId side) const {
    if (!ValidSide(side)) return CityId::None();
    return CityId(allied_target_city_[static_cast<size_t>(side.Value())]);
}

void DiplomacyMatrix::SetAlliedTargetCity(SideId side, CityId city_id) {
    if (!ValidSide(side)) return;
    allied_target_city_[static_cast<size_t>(side.Value())] = city_id.Value();
}

void DiplomacyMatrix::ResetFallenSide(SideId fallen_side, int neutral_state) {
    if (!ValidSide(fallen_side)) return;
    for (int other = 0; other < 8; ++other) {
        SideId other_id(other);
        SetSymmetricState(fallen_side, other_id, neutral_state);
        SetSymmetricProposal(fallen_side, other_id, neutral_state);
    }
}

}  // namespace w2
