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
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"

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

bool Raidable(Game& game, int raider_index, const City& city, bool fair_fog) {
    int owner_index = city.OwnerIndex();
    if (city.IsRazed() || owner_index < 0 || owner_index >= 8 || owner_index == raider_index) {
        return false;
    }
    if (fair_fog && !ai::belief::IsCityVisible(game, raider_index, city)) return false;
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
    opponents::RaidEmptyCities(game, side, Base().IsFairFog());
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
    opponents::GarrisonCities(game, side, Base().IsFairFog());
}

nlohmann::ordered_json WardenPlayer::SaveState() const {
    nlohmann::ordered_json obj = Base().SaveState();
    obj["kind"] = "warden";
    return obj;
}

// ------------------------------------------------------------------ WarbandPlayer

WarbandPlayer::WarbandPlayer(SharedClassicPlayer base) : ClassicDecoratorPlayer(std::move(base)) {
    Base().SetFairFog(true);
}

WarbandPlayer::WarbandPlayer(Game& game, Side& side) : ClassicDecoratorPlayer(game, side) {
    Base().SetFairFog(true);
}

void WarbandPlayer::PlayTurn(Game& game, Side& side, IAiTurnEvents* events) {
    ScopedAiTurnEvents turn_scope(game, events);
    Base().SetFairFog(true);
    opponents::RunWarbandPrePhase(game, side);
    if (events && events->IsAborted()) return;
    Base().PlayTurn(game, side, events);
    if (events && events->IsAborted()) return;
    opponents::GarrisonCities(game, side, /*fair_fog=*/true);
}

nlohmann::ordered_json WarbandPlayer::SaveState() const {
    nlohmann::ordered_json obj = Base().SaveState();
    obj["kind"] = "warband";
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

int RaidEmptyCities(Game& game, Side& side, bool fair_fog) {
    const int raider_index = side.Index();
    constexpr int kMaxTriesPerCity = 4;
    constexpr int kMoveCostPerTile = 2;

    std::vector<City*> targets;
    for (auto& city : game.GetMap().Cities()) {
        if (Raidable(game, raider_index, city, fair_fog)) targets.push_back(&city);
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
        if (!Raidable(game, raider_index, *target, fair_fog)) continue;
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

int GarrisonCities(Game& game, Side& side, bool fair_fog) {
    const int side_index = side.Index();
    const bool check_fog = fair_fog && game.GetMap().GameOptions().hidden_map != 0;
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
                army->InTransit() || diplomacy::IsAllied(game, side_index, army->OwnerIndex())) {
                continue;
            }
            if (check_fog && !ai::belief::IsTileVisible(game, side_index, army->X(), army->Y())) {
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

int RunWarbandPrePhase(Game& game, Side& side) {
    const int side_idx = side.Index();
    Map& map = game.GetMap();
    std::vector<City*> own_cities = game::SideCities(game, side);
    if (own_cities.empty()) return 0;

    // Early elite citadel slot purchase + rear production vectoring
    City* citadel = side.CapitalCity(map);
    if (!citadel || !citadel->OwnedBy(side_idx) || citadel->IsRazed()) {
        citadel = own_cities.front();
        for (City* cand : own_cities) {
            if (cand->Income() > citadel->Income()) citadel = cand;
        }
    }
    if (side.Gold() >= 350 && citadel != nullptr) {
        int elite_slots = 0;
        int replace_price = (!citadel->Slots().empty()) ? citadel->SlotAt(0).price : 0;
        for (const auto& slot : citadel->Slots()) {
            const ArmyType* atype = game.ArmyTypes().ById(slot.type);
            if (slot.strength >= 6 || (atype && atype->flies) || slot.move >= 14) elite_slots++;
        }
        if (elite_slots < 2) {
            const ArmyType* best_buy = nullptr;
            int best_rating = -1;
            const bool full_slots = citadel->SlotCount() >= rules::kProductionSlots;
            for (const ArmyType* atype : game::BuyableTypes(game)) {
                int price = std::abs(atype->price);
                if (price <= 0 || price + 80 > side.Gold()) continue;
                if (full_slots && price <= replace_price) continue;
                if (!game::CannotBuy(game, side, *citadel, *atype).empty()) continue;
                int rating = atype->strength * 4 + (atype->flies ? 8 : 0) +
                             (atype->move >= 14 ? 4 : 0) - std::max(0, atype->time - 2) * 2;
                if (rating > best_rating) {
                    best_rating = rating;
                    best_buy = atype;
                }
            }
            if (best_buy != nullptr) {
                int slot_idx = game::BuySlot(*citadel);
                game::BuyProduction(game, side, *citadel, slot_idx, best_buy->id);
                game::SortProduction(*citadel);
                for (int si = 0; si < citadel->SlotCount(); ++si) {
                    if (citadel->SlotAt(si).type == best_buy->id) {
                        game::SetProduction(game, *citadel, si);
                        break;
                    }
                }
            }
        }
        int vec_cnt = static_cast<int>(game::VectoredTo(game, citadel, &side).size());
        for (City* feeder : own_cities) {
            if (vec_cnt >= 3) break;
            if (feeder == citadel || !game::Producing(*feeder) || feeder->IsVectored()) continue;
            if (move::Distance(feeder->X(), feeder->Y(), citadel->X(), citadel->Y()) <= 36) {
                game::Vector(game, *feeder, citadel);
                vec_cnt++;
            }
        }
    }

    ai::belief::Update(game, side);
    const SideBelief& belief = ai::belief::Ensure(side);
    int rallied_or_marched = GarrisonCities(game, side, /*fair_fog=*/true);

    // Lock the weakest non-Hero defender in each owned city so ClassicAiPlayer never vacates it
    for (City* owned_city : own_cities) {
        if (!owned_city || owned_city->IsRazed() || !owned_city->OwnedBy(side_idx)) continue;
        Army* weakest_guard = nullptr;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (Army* occ : game::ArmiesAt(game, owned_city->X() + dx, owned_city->Y() + dy)) {
                    if (!occ->OwnedBy(side_idx) || occ->Hero() || !occ->OnMap()) continue;
                    if (!weakest_guard || occ->Strength() < weakest_guard->Strength() ||
                        (occ->Strength() == weakest_guard->Strength() &&
                         occ->MaxMoves() < weakest_guard->MaxMoves())) {
                        weakest_guard = occ;
                    }
                }
            }
        }
        if (weakest_guard != nullptr) {
            weakest_guard->SetMoves(0);
        }
    }

    std::vector<Army*> heroes;
    for (Army* army : game.Armies()) {
        if (army->OwnedBy(side_idx) && army->Hero() && army->OnMap() && army->Moves() >= 2) {
            heroes.push_back(army);
        }
    }

    for (Army* hero : heroes) {
        if (!game.Alive(hero) || !hero->OnMap() || hero->Moves() < 2) continue;
        // Assemble nearby spare units onto the Hero's tile up to 8 units
        for (Army* mate : game.Armies()) {
            if (!game.Alive(hero) || !hero->OnMap()) break;
            if (!mate->OwnedBy(side_idx) || mate == hero || !mate->OnMap() || mate->Moves() < 2 ||
                mate->Hero()) {
                continue;
            }
            if (mate->X() == hero->X() && mate->Y() == hero->Y()) continue;
            if (static_cast<int>(game::ArmiesAt(game, hero->X(), hero->Y()).size()) >=
                rules::kMaxStack) {
                break;
            }
            if (LastDefender(game, side_idx, mate)) continue;
            int dist = move::Distance(mate->X(), mate->Y(), hero->X(), hero->Y());
            if (dist > 6 || dist * 2 > mate->Moves() + 2) continue;
            auto path = move::FindPath(game, {mate}, mate->X(), mate->Y(), hero->X(), hero->Y());
            if (!path || path->empty()) continue;
            move::WalkResult walk = move::Walk(game, {mate}, *path);
            if (walk.steps > 0) {
                ai::Walked(game, {mate}, walk);
                rallied_or_marched++;
            }
        }

        if (!game.Alive(hero) || !hero->OnMap()) continue;
        std::vector<Army*> tile_units;
        for (Army* occ : game::ArmiesAt(game, hero->X(), hero->Y())) {
            if (occ->OwnedBy(side_idx) && occ->Moves() >= 2) tile_units.push_back(occ);
        }
        ai::core::Sel sel = ai::core::Select(game, tile_units);
        if (sel.armies.size() < 2 || sel.min_moves < 2) continue;

        // Visit a nearby visible Temple if at least half the warband lacks its blessing
        if (sel.armies.size() >= 3 && sel.min_moves >= 4) {
            for (const Site& site_obj : map.Sites()) {
                if (site_obj.content != site::kTemple) continue;
                if (!ai::belief::IsSiteVisible(game, side_idx, site_obj)) continue;
                int dist = move::Distance(hero->X(), hero->Y(), site_obj.x, site_obj.y);
                if (dist > 5 || dist * 2 > sel.min_moves) continue;
                int unblessed = 0;
                for (const Army* member : sel.armies) {
                    if (!member->HasBlessing(site_obj.temple_index)) unblessed++;
                }
                if (unblessed * 2 < static_cast<int>(sel.armies.size())) continue;
                auto path = move::FindPath(game, sel.armies, sel.leader->X(), sel.leader->Y(),
                                           site_obj.x, site_obj.y);
                if (!path) continue;
                if (!path->empty()) {
                    move::WalkResult walk = move::Walk(game, sel.armies, *path);
                    if (walk.steps > 0) ai::Walked(game, sel.armies, walk);
                    ai::core::Refresh(game, sel);
                }
                if (!sel.armies.empty() && sel.leader && sel.leader->X() == site_obj.x &&
                    sel.leader->Y() == site_obj.y) {
                    site::Search(game, sel.armies, site_obj.x, site_obj.y, false);
                    rallied_or_marched += static_cast<int>(sel.armies.size());
                }
                break;
            }
        }

        ai::core::Refresh(game, sel);
        const size_t min_siege_stack = (game.Turn() <= 10) ? 3U : 4U;
        if (sel.armies.size() < min_siege_stack || sel.min_moves < 2 || !sel.leader) continue;

        // March the concentrated Hero warband against the best visible target with >= 65% fair odds
        City* best_target = nullptr;
        float best_value = -1.0F;
        for (City* cand_city : belief.visible_cities) {
            if (!cand_city || cand_city->IsRazed() || cand_city->OwnedBy(side_idx)) continue;
            int owner_idx = cand_city->OwnerIndex();
            if (owner_idx >= 0 && diplomacy::IsAllied(game, side_idx, owner_idx)) continue;
            if (owner_idx >= 0 && diplomacy::MustDeclare(game, side_idx, owner_idx, cand_city)) {
                continue;
            }
            int dist = move::Distance(sel.leader->X(), sel.leader->Y(), cand_city->X(),
                                      cand_city->Y());
            if (dist > 14) continue;
            auto est = ai::belief::EstimateCombat(game, side, sel.armies, cand_city->X(),
                                                  cand_city->Y());
            if (est.win_prob < 0.65F) continue;
            float val = est.win_prob * (20.0F + static_cast<float>(cand_city->Income())) -
                        static_cast<float>(dist) * 1.2F;
            if (val > best_value) {
                best_value = val;
                best_target = cand_city;
            }
        }
        if (best_target != nullptr) {
            int dest_x = best_target->X() + (best_target->X() < sel.leader->X() ? 1 : 0);
            int dest_y = best_target->Y() + (best_target->Y() < sel.leader->Y() ? 1 : 0);
            auto path = move::FindPath(game, sel.armies, sel.leader->X(), sel.leader->Y(), dest_x,
                                       dest_y);
            if (path && !path->empty()) {
                move::WalkResult walk = move::Walk(game, sel.armies, *path);
                if (walk.steps > 0) ai::Walked(game, sel.armies, walk);
                ai::core::Refresh(game, sel);
                if (walk.stopped == move::WalkStop::kAttack && walk.attack &&
                    !sel.armies.empty()) {
                    ai::core::Fight(game, sel, walk.attack->x, walk.attack->y);
                }
                for (Army* member : sel.armies) {
                    if (game.Alive(member)) {
                        member->SetMoves(0);
                        rallied_or_marched++;
                    }
                }
            }
        }
    }
    return rallied_or_marched;
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
    if (kind == Kind::kWarband) {
        if (dynamic_cast<const WarbandPlayer*>(side.ComputerPlayer()) != nullptr) return;
        side.SetComputerPlayer(std::make_shared<WarbandPlayer>(game, side));
        return;
    }
    const bool want_fair = (kind == Kind::kFairClassic);
    if (auto* existing = dynamic_cast<ClassicAiPlayer*>(side.ComputerPlayer())) {
        existing->SetFairFog(want_fair);
        return;
    }
    auto classic_player = ExtractOrMakeClassic(game, side);
    classic_player->SetFairFog(want_fair);
    side.SetComputerPlayer(std::move(classic_player));
}

void PlayTurn(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen,
              IAiTurnEvents* events) {
    InstallOpponent(game, side, kind, frozen);
    ai::PlayTurn(game, side, events);
}

}  // namespace opponents
}  // namespace w2
