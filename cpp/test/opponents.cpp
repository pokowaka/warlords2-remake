/**
 * @file
 * @brief Implementation of `RaiderPlayer`, `WardenPlayer`, `FrozenPlayer`, and training-opponent
 * helpers.
 */
#include "opponents.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/move.hpp"

namespace w2 {

namespace {

int Defenders(Game& game, const City& city) {
    int count = 0;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            for (Army* army : game::ArmiesAt(game, city.X() + dx, city.Y() + dy)) {
                if (army->Owner() == city.Owner()) count++;
            }
        }
    }
    return count;
}

bool Raidable(Game& game, int raider_index, const City& city) {
    int owner_index = city.OwnerIndex();
    if (city.IsRazed() || owner_index < 0 || owner_index >= 8 || owner_index == raider_index) {
        return false;
    }
    if (!game.GetMap().Sides()[owner_index].IsAlive()) return false;
    if (diplomacy::IsAllied(game, raider_index, owner_index)) return false;
    if (diplomacy::MustDeclare(game, raider_index, owner_index, &city)) return false;
    return Defenders(game, city) == 0;
}

bool LastDefender(Game& game, int raider_index, Army* army) {
    City* home = game::CityAt(game, army->X(), army->Y());
    if (!home || !home->OwnedBy(raider_index)) return false;
    return Defenders(game, *home) <= 1;
}

std::shared_ptr<ClassicAiPlayer> ExtractOrMakeClassic(Game& game, Side& side) {
    if (auto direct = std::dynamic_pointer_cast<ClassicAiPlayer>(side.ComputerPlayerShared())) {
        return direct;
    }
    if (ClassicAiPlayer* existing = ClassicAiPlayer::FindMutableOn(side)) {
        return std::make_shared<ClassicAiPlayer>(existing->Data(), existing->NeighbourTable(),
                                                 &game.Armies());
    }
    auto created = ai::MakeComputerPlayer(true, game, side);
    if (auto casted = std::dynamic_pointer_cast<ClassicAiPlayer>(created)) {
        return casted;
    }
    return std::make_shared<ClassicAiPlayer>();
}

}  // namespace

// ------------------------------------------------------------------ ClassicDecoratorPlayer

ClassicDecoratorPlayer::ClassicDecoratorPlayer(SharedClassicPlayer base)
        : base_(base ? std::move(base) : std::make_shared<ClassicAiPlayer>()) {}

ClassicDecoratorPlayer::ClassicDecoratorPlayer(Game& game, Side& side)
        : base_(ExtractOrMakeClassic(game, side)) {}

void ClassicDecoratorPlayer::OnTurnStarted(Side& side) {
    base_->OnTurnStarted(side);
}

AttitudeBreakdown ClassicDecoratorPlayer::EvaluateProposal(const Game& game, SideId ai_side,
                                                           SideId proposer_side) const {
    return base_->EvaluateProposal(game, ai_side, proposer_side);
}

City* ClassicDecoratorPlayer::ChooseHeroCity(Game& game, Side& side, City* fallback) {
    return base_->ChooseHeroCity(game, side, fallback);
}

bool ClassicDecoratorPlayer::OnQuestCapture(Game& game, Side& side, City& city,
                                            const ArmyStack& stack) {
    return base_->OnQuestCapture(game, side, city, stack);
}

void ClassicDecoratorPlayer::RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos,
                                          int heroes_lost, int armies_lost, bool all_lost,
                                          bool city_tile) {
    base_->RecordBattle(game, defender, attacker, pos, heroes_lost, armies_lost, all_lost,
                        city_tile);
}

void ClassicDecoratorPlayer::RecordBattleObservation(Game& game, SideId observer, TilePos pos) {
    base_->RecordBattleObservation(game, observer, pos);
}

absl::Status ClassicDecoratorPlayer::LoadState(const nlohmann::ordered_json& state,
                                               const Game& game) {
    return base_->LoadState(state, game);
}

// ------------------------------------------------------------------ RaiderPlayer

RaiderPlayer::RaiderPlayer(SharedClassicPlayer base) : ClassicDecoratorPlayer(std::move(base)) {}

RaiderPlayer::RaiderPlayer(Game& game, Side& side) : ClassicDecoratorPlayer(game, side) {}

void RaiderPlayer::PlayTurn(Game& game, Side& side, IAiTurnEvents* events) {
    ScopedAiTurnEvents turn_scope(game, events);
    opponents::RaidEmptyCities(game, side);
    if (events && events->IsAborted()) return;
    Base().PlayTurn(game, side, events);
}

nlohmann::ordered_json RaiderPlayer::SaveState() const {
    nlohmann::ordered_json obj = Base().SaveState();
    obj["kind"] = "raider";
    return obj;
}

// ------------------------------------------------------------------ WardenPlayer

WardenPlayer::WardenPlayer(SharedClassicPlayer base) : ClassicDecoratorPlayer(std::move(base)) {}

WardenPlayer::WardenPlayer(Game& game, Side& side) : ClassicDecoratorPlayer(game, side) {}

void WardenPlayer::PlayTurn(Game& game, Side& side, IAiTurnEvents* events) {
    ScopedAiTurnEvents turn_scope(game, events);
    Base().PlayTurn(game, side, events);
    if (events && events->IsAborted()) return;
    opponents::GarrisonCities(game, side);
}

nlohmann::ordered_json WardenPlayer::SaveState() const {
    nlohmann::ordered_json obj = Base().SaveState();
    obj["kind"] = "warden";
    return obj;
}

// ------------------------------------------------------------------ FrozenPlayer

FrozenPlayer::FrozenPlayer(const ai::ml::PolicyNetwork* frozen_policy)
        : frozen_policy_(frozen_policy)
        , inner_(std::make_unique<ModernMlPlayer>(frozen_policy, nullptr)) {}

void FrozenPlayer::PlayTurn(Game& game, Side& side, IAiTurnEvents* events) {
    inner_->SetCustomPolicy(frozen_policy_);
    inner_->PlayTurn(game, side, events);
}

AttitudeBreakdown FrozenPlayer::EvaluateProposal(const Game& game, SideId ai_side,
                                                 SideId proposer_side) const {
    return inner_->EvaluateProposal(game, ai_side, proposer_side);
}

City* FrozenPlayer::ChooseHeroCity(Game& game, Side& side, City* fallback) {
    return inner_->ChooseHeroCity(game, side, fallback);
}

bool FrozenPlayer::OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) {
    return inner_->OnQuestCapture(game, side, city, stack);
}

void FrozenPlayer::RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos,
                                int heroes_lost, int armies_lost, bool all_lost, bool city_tile) {
    inner_->RecordBattle(game, defender, attacker, pos, heroes_lost, armies_lost, all_lost,
                         city_tile);
}

void FrozenPlayer::RecordBattleObservation(Game& game, SideId observer, TilePos pos) {
    inner_->RecordBattleObservation(game, observer, pos);
}

nlohmann::ordered_json FrozenPlayer::SaveState() const {
    nlohmann::ordered_json obj = inner_->SaveState();
    obj["kind"] = "frozen";
    return obj;
}

absl::Status FrozenPlayer::LoadState(const nlohmann::ordered_json& state, const Game& game) {
    return inner_->LoadState(state, game);
}

// ------------------------------------------------------------------ opponents helpers

namespace opponents {

int RaidEmptyCities(Game& game, Side& side) {
    const int raider_index = side.Index();
    constexpr int kMaxTriesPerCity = 4;
    constexpr int kMoveCostPerTile = 2;

    std::vector<City*> targets;
    for (auto& city : game.GetMap().Cities()) {
        if (Raidable(game, raider_index, city)) targets.push_back(&city);
    }
    auto learner_owned = [&](const City* city) {
        return !game.GetMap().Sides()[city->OwnerIndex()].IsClassicAi();
    };
    std::sort(targets.begin(), targets.end(), [&](City* lhs, City* rhs) {
        if (learner_owned(lhs) != learner_owned(rhs)) return learner_owned(lhs);
        return lhs->Income() > rhs->Income();
    });

    int taken = 0;
    for (City* target : targets) {
        if (!Raidable(game, raider_index, *target)) continue;
        std::vector<Army*> raiders;
        for (Army* army : game.Armies()) {
            if (!army->OwnedBy(raider_index) || army->X() == kNone || army->InTransit() ||
                army->Moves() <= 0 || army->Hero()) {
                continue;
            }
            int distance = move::Distance(army->X(), army->Y(), target->X(), target->Y());
            if (distance * kMoveCostPerTile > army->Moves() + 2) continue;
            if (LastDefender(game, raider_index, army)) continue;
            raiders.push_back(army);
        }
        std::sort(raiders.begin(), raiders.end(), [](Army* lhs, Army* rhs) {
            if (lhs->Strength() != rhs->Strength()) return lhs->Strength() < rhs->Strength();
            return lhs->Moves() > rhs->Moves();
        });
        int tries = 0;
        for (Army* raider : raiders) {
            if (tries++ >= kMaxTriesPerCity || target->OwnedBy(raider_index)) break;
            if (!game.Alive(raider) || raider->Moves() <= 0) continue;
            int dest_x = target->X() + (target->X() < raider->X() ? 1 : 0);
            int dest_y = target->Y() + (target->Y() < raider->Y() ? 1 : 0);
            auto path = move::FindPath(game, {raider}, raider->X(), raider->Y(), dest_x, dest_y);
            if (!path || path->empty()) continue;
            move::WalkResult walk = move::Walk(game, {raider}, *path);
            if (walk.steps > 0) ai::Walked(game, {raider}, walk);
            if (walk.stopped == move::WalkStop::kAttack && walk.attack &&
                walk.attack->city == target && game.Alive(raider)) {
                ai::core::Sel sel = ai::core::Select(game, {raider});
                if (!sel.armies.empty()) ai::core::Fight(game, sel, walk.attack->x, walk.attack->y);
            }
            if (target->OwnedBy(raider_index)) taken++;
        }
    }
    return taken;
}

int GarrisonCities(Game& game, Side& side) {
    const int side_index = side.Index();
    constexpr int kNearEnemy = 8;
    constexpr int kMoveCostPerTile = 2;
    struct Need {
        City* city;
        int want;
        int have;
    };
    std::vector<Need> needs;
    for (auto& city : game.GetMap().Cities()) {
        if (city.IsRazed() || !city.OwnedBy(side_index)) continue;
        bool threatened = false;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() < 0 || army->OwnedBy(side_index) || army->X() == kNone ||
                diplomacy::IsAllied(game, side_index, army->OwnerIndex())) {
                continue;
            }
            if (move::Distance(army->X(), army->Y(), city.X(), city.Y()) <= kNearEnemy) {
                threatened = true;
                break;
            }
        }
        needs.push_back({&city, threatened ? 2 : 1, Defenders(game, city)});
    }
    std::sort(needs.begin(), needs.end(), [](const Need& lhs, const Need& rhs) {
        if ((lhs.have == 0) != (rhs.have == 0)) return lhs.have == 0;
        return lhs.want > rhs.want;
    });
    auto spare = [&](Army* army) {
        City* home = game::CityAt(game, army->X(), army->Y());
        if (!home || !home->OwnedBy(side_index)) return true;
        for (const Need& need : needs) {
            if (need.city == home) return Defenders(game, *home) > need.want;
        }
        return Defenders(game, *home) > 1;
    };
    int moved = 0;
    for (Need& need : needs) {
        while (Defenders(game, *need.city) < need.want) {
            Army* best = nullptr;
            int best_dist = std::numeric_limits<int>::max();
            for (Army* army : game.Armies()) {
                if (!army->OwnedBy(side_index) || army->X() == kNone || army->InTransit() ||
                    army->Moves() <= 0 || army->Hero()) {
                    continue;
                }
                if (game::CityAt(game, army->X(), army->Y()) == need.city) continue;
                int dist = move::Distance(army->X(), army->Y(), need.city->X(), need.city->Y());
                if (dist * kMoveCostPerTile > army->Moves() + 2 || dist >= best_dist ||
                    !spare(army)) {
                    continue;
                }
                best = army;
                best_dist = dist;
            }
            if (!best) break;
            auto path = move::FindPath(game, {best}, best->X(), best->Y(), need.city->X(),
                                       need.city->Y());
            if (!path || path->empty()) {
                best->SetMoves(0);
                continue;
            }
            move::WalkResult walk = move::Walk(game, {best}, *path);
            if (walk.steps > 0) ai::Walked(game, {best}, walk);
            if (!game.Alive(best) || game::CityAt(game, best->X(), best->Y()) != need.city) break;
            moved++;
        }
    }
    return moved;
}

void InstallOpponent(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen) {
    if (kind == Kind::kFrozen && frozen != nullptr) {
        if (const auto* existing = dynamic_cast<const FrozenPlayer*>(side.ComputerPlayer());
            existing != nullptr && existing->Policy() == frozen) {
            return;
        }
        side.SetComputerPlayer(std::make_shared<FrozenPlayer>(frozen));
        return;
    }
    if (kind == Kind::kRaider) {
        if (dynamic_cast<const RaiderPlayer*>(side.ComputerPlayer()) != nullptr) return;
        side.SetComputerPlayer(std::make_shared<RaiderPlayer>(game, side));
        return;
    }
    if (kind == Kind::kWarden) {
        if (dynamic_cast<const WardenPlayer*>(side.ComputerPlayer()) != nullptr) return;
        side.SetComputerPlayer(std::make_shared<WardenPlayer>(game, side));
        return;
    }
    if (dynamic_cast<const ClassicAiPlayer*>(side.ComputerPlayer()) != nullptr) return;
    side.SetComputerPlayer(ExtractOrMakeClassic(game, side));
}

void PlayTurn(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen,
              IAiTurnEvents* events) {
    InstallOpponent(game, side, kind, frozen);
    ai::PlayTurn(game, side, events);
}

}  // namespace opponents
}  // namespace w2
