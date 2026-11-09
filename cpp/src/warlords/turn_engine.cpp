/**
 * @file
 * @brief Implementation of `TurnEngine`.
 */
#include "warlords/turn_engine.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "warlords/armytype.hpp"
#include "warlords/city_economy.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

bool CanFlyOff(const Game& game, const std::vector<Army*>& armies) {
    bool hero_flight = false, all_fly = true, non_heroes_fly = true, free_flier = false;
    for (Army* army : armies) {
        bool flies = game.ArmyTypes().ById(army->Type())->flies;
        if (army->Hero()) {
            for (Item* item : army->Items()) {
                if (item->type == rules::kItemFlight) hero_flight = true;
            }
        }
        if (!flies || army->IsAtSea()) all_fly = false;
        if (!army->Hero() && !flies) non_heroes_fly = false;
        if (flies && !army->IsAtSea()) free_flier = true;
    }
    return hero_flight || all_fly || (non_heroes_fly && free_flier);
}

}  // namespace

Fallen TurnEngine::EliminateSide(Side& side, bool boxed) {
    ArmyStack mine;
    for (int i = static_cast<int>(game_.Armies().size()) - 1; i >= 0; i--) {
        if (game_.Armies()[i]->OwnedBy(side.Id())) mine.push_back(game_.Armies()[i]);
    }
    Disband(side, mine);
    history::RecordDeed(game_, &side, DeedType::kSideVanquished, side.Index(), 0, "");
    int line = game_.GetRng().Dice(1, game::kFallenLines, -1);
    game_.AppendLog(side.Name() + " has been eliminated.");
    side.SetAlive(false);
    side.SetGold(0);
    game_.Diplomacy().ResetFallenSide(side.Id(), diplomacy::kIntermediate);
    return Fallen{&side, line, boxed};
}

std::vector<Fallen> TurnEngine::EliminateFallen() {
    bool human = false;
    for (Side* side : game_.Sides()) {
        if (side->IsAlive() && !side->IsComputerControlled()) human = true;
    }
    std::vector<Fallen> fallen;
    for (Side* side : game_.Sides()) {
        if (side->IsAlive() && game::SideCities(game_, *side).empty())
            fallen.push_back(EliminateSide(*side, human || !side->IsComputerControlled()));
    }
    return fallen;
}

void TurnEngine::ResetMovement(Side& side) {
    for (Army* army : game_.Armies()) {
        if (army->OwnedBy(side.Id())) {
            army->ResetTurnMarks();
        }
    }
    for (int i = static_cast<int>(game_.Armies().size()) - 1; i >= 0; i--) {
        Army* army = game_.Armies()[i];
        if (army->OwnedBy(side.Id()) && !army->InTransit()) {
            int carry = std::min(army->Moves(), rules::kMoveCarry);
            army->SetMoves((army->IsAtSea() ? rules::kSeaMoves : army->MaxMoves()) + carry);
            if (army->HasTarget() && *army->Target() == army->Position()) army->ClearTarget();
        }
    }
    for (int i = static_cast<int>(game_.Armies().size()) - 1; i >= 0; i--) {
        Army* hero = game_.Armies()[i];
        if (!hero->OwnedBy(side.Id()) || !hero->Hero() || hero->InTransit()) continue;
        bool double_move = false;
        for (Item* item : hero->Items()) {
            if (item->type == rules::kItemDoubleMove) double_move = true;
        }
        if (!double_move) continue;
        auto here = game::ArmiesAt(game_, hero->X(), hero->Y());
        if (static_cast<int>(here.size()) > rules::kMaxStack) here.resize(rules::kMaxStack);
        for (Army* army : here) army->AddMoves(army->MaxMoves());
    }
    for (Army* army : game_.Armies()) {
        if (army->OwnedBy(side.Id()) && army->Moves() > rules::kMaxMove)
            army->SetMoves(rules::kMaxMove);
    }
}

void TurnEngine::Encamp(Side& side) {
    static const std::set<int> kEncampOn = {move::kPlain,  move::kForest, move::kHills,
                                            move::kBridge, move::kMarsh,  move::kTower};
    const Map& map = game_.GetMap();
    for (Army* army : game_.Armies()) {
        if (army->OwnedBy(side.Id()) && army->IsFortified() && army->OnMap() &&
            army->Moves() >= army->MaxMoves()) {
            bool road = scn::RoadAt(map, army->X(), army->Y()) % 32 != 0;
            if (road || kEncampOn.count(scn::TerrainAt(map, army->X(), army->Y()))) {
                game_.PlaceTower(army->Position());
            }
        }
    }
}

void TurnEngine::ComeAshore(Side& side) {
    for (Army* army : game_.Armies()) {
        if (army->OwnedBy(side.Id()) && army->IsAtSea() && army->OnMap() &&
            game::CityAt(game_, army->X(), army->Y())) {
            army->ComeAshore();
        }
    }
}

void TurnEngine::FollowCapital(Side& side) {
    if (!side.CurrentCapitalPos().has_value()) {
        side.SetCurrentCapitalPos(side.OriginCapitalPos());
    }
    int playing = 0;
    for (Side* other : game_.Sides()) {
        if (other->IsAlive()) playing++;
    }
    TilePos origin = side.OriginCapitalPos();
    if (playing != 1) {
        City* cap_city = game::CityAt(game_, origin.x, origin.y);
        if (cap_city && cap_city->IsRazed()) cap_city = nullptr;
        if (!game::SideCities(game_, side).empty() &&
            (!cap_city || !cap_city->OwnedBy(side.Id()))) {
            TilePos cur = side.CapitalPosition();
            City* nearest = CityEconomy(game_).NearestCity(cur.x, cur.y, &side);
            if (nearest) {
                side.SetCurrentCapitalPos(nearest->Position());
            }
            return;
        }
    }
    side.SetCurrentCapitalPos(origin);
}

void TurnEngine::NotifyTurnQuest(Side& side) {
    auto quest_result = quest::Notify(game_, side, "turn");
    if (quest_result && !quest_result->failed.empty())
        game_.AppendLog("Quest abandoned: " + quest_result->failed + ".");
}

bool TurnEngine::StartTurn(Side& side) {
    CityEconomy economy(game_);
    TurnReport& news = game_.TurnNews(side.Id());
    ComeAshore(side);
    FollowCapital(side);
    game::TidyExplored(game_, side.Index());
    news.diplo_news = diplomacy::Apply(game_, side);
    for (const auto& msg : news.diplo_news) game_.AppendLog(msg);
    news.hero_offer.reset();
    side.SetProductionDue(false);
    if (side.IsComputerControlled()) {
        if (game::SideCities(game_, side).empty()) return false;
        auto offer = hero::Offer(game_, side);
        if (offer) hero::Recruit(game_, side, *offer);
        hero::CheckPromotions(game_, side);
        economy.ApplyIncome(side);
        economy.RunProduction(side);
        ResetMovement(side);
        game::TidyTowers(game_);
        Encamp(side);
        if (side.ComputerPlayer()) side.ComputerPlayer()->OnTurnStarted(side);
        NotifyTurnQuest(side);
        return true;
    }
    ResetMovement(side);
    game::TidyTowers(game_);
    Encamp(side);
    economy.ApplyIncome(side);
    NotifyTurnQuest(side);
    news.promoted = hero::CheckPromotions(game_, side);
    news.hero_offer = hero::Offer(game_, side);
    side.SetProductionDue(true);
    return true;
}

void TurnEngine::HumanProduction(Side& side) {
    if (!side.IsProductionDue()) return;
    side.SetProductionDue(false);
    if (game_.Turn() == 1) return;
    CityEconomy economy(game_);
    for (const auto& entry : economy.RunProduction(side)) {
        City* city = game_.GetMap().CityById(entry.city);
        if (city) {
            if (const Slot* slot = city->ActiveSlot()) {
                city->SetCountdown(slot->time);
            }
        }
    }
}

Side* TurnEngine::AdvanceToNextSide(Side* leaving) {
    for (int step = 0; step < 3 * 8 + 1; step++) {
        if (game_.Schedule().AtRoundEnd()) {
            game_.AdvanceTurn();
            game_.Schedule().StartNewRound(game_.GetMap().GameOptions().random_turns != 0,
                                           game_.GetRng());
            EndRound(leaving);
            if (game_.EndingState().over) {
                game_.ClearCurrentSide();
                return nullptr;
            }
            history::Record(game_);
        }
        SideId scheduled_side = game_.Schedule().Advance();
        Side* side = game_.GetMap().SideById(scheduled_side);
        if (side && side->IsActive()) {
            game_.SetCurrentSide(side);
            if (StartTurn(*side)) return side;
        }
    }
    game_.ClearCurrentSide();
    return nullptr;
}

Side* TurnEngine::EndTurn() {
    Side* leaving = game_.CurrentSide();
    if (leaving && leaving->IsAlive() && (leaving->IsComputerControlled() || !game_.IsWon()))
        diplomacy::ScoreUpdate(game_, *leaving);
    game_.SetEndingState(Ending{});
    return AdvanceToNextSide(leaving);
}

Side* TurnEngine::Begin() {
    game_.Schedule().RewindCursor();
    game_.SetEndingState(Ending{});
    return AdvanceToNextSide(nullptr);
}

Side* TurnEngine::NextHuman() const {
    for (int i = std::max(game_.Schedule().Cursor(), 0); i <= 7; i++) {
        Side* side = game_.GetMap().SideById(game_.Schedule().SideAt(i));
        if (side && side->IsHuman()) return side;
    }
    for (Side* side : game_.Sides()) {
        if (side->IsAlive() && !side->IsComputerControlled()) return side;
    }
    return nullptr;
}

int TurnEngine::SinkStranded(Side& side, int x, int y) {
    const Map& map = game_.GetMap();
    auto wet = [&](int tile_x, int tile_y) {
        return scn::TerrainAt(map, tile_x, tile_y) == move::kWater ||
               scn::IsCrossing(map, tile_x, tile_y);
    };
    std::map<int, ArmyStack> tiles;
    for (Army* army : game_.Armies()) {
        if (!army->OwnedBy(side.Id()) || !army->OnMap() || army->X() < 0 || army->Y() < 0) continue;
        if (x != kNone && (army->X() != x || army->Y() != y)) continue;
        if (wet(army->X(), army->Y())) tiles[army->Y() * map.Width() + army->X()].push_back(army);
    }
    ArmyStack dead;
    for (auto& [tile_key, here] : tiles) {
        if (CanFlyOff(game_, here)) continue;
        for (Army* army : here) {
            if (!army->IsAtSea() && !army->Hero() && !game_.ArmyTypes().ById(army->Type())->flies)
                dead.push_back(army);
        }
    }
    game_.Armies().RemoveAll(dead);
    return static_cast<int>(dead.size());
}

void TurnEngine::Disband(Side& side, const ArmyStack& armies) {
    for (Army* army : armies) {
        if (army->Hero()) {
            hero::DropItems(game_, army, army->X(), army->Y(), side.Index());
            if (side.ActiveQuest() && army->Id().IsValid() &&
                side.ActiveQuest()->HeroId() == army->Id()) {
                side.ClearQuest();
            }
        }
    }
    game_.Armies().RemoveAll(armies);
}

void TurnEngine::Resign(Side& side) {
    CityEconomy economy(game_);
    Map& map = game_.GetMap();
    for (auto& city : map.Cities()) {
        if (!city.IsRazed() && city.OwnedBy(side.Id())) economy.MakeRuins(city);
    }
    Disband(side, game::SideArmies(game_, side));
    if (map.GameOptions().hidden_map != 0) {
        game_.Explored().ResetSide(side.Id(), map.Width(), map.Height(), true);
    }
}

void TurnEngine::AcceptSurrender(Side& side) {
    for (Side* other : game_.Sides()) {
        if (other->IsComputerControlled()) other->SetAlive(false);
    }
    game_.SetWon(true);
    history::RecordDeed(game_, &side, DeedType::kSideVictorious, side.Index(), 0, "");
}

bool TurnEngine::SurrenderPending() const {
    return game_.SurrenderOffered() && !game_.SurrenderShown() && !game_.IsWon();
}

Ending TurnEngine::EndRound(Side* last) {
    auto fallen = EliminateFallen();
    Ending ending = CheckEnd();
    ending.fallen = fallen;
    if (!ending.message.empty()) game_.AppendLog(ending.message);
    game_.SetEndingState(ending);
    CityEconomy(game_).NeutralProduction(last ? last : game_.GetMap().SideAt(game::kSetupPlayer));
    return ending;
}

Ending TurnEngine::CheckEnd() {
    std::vector<Side*> humans, computers;
    bool fallen_human = false;
    for (Side* side : game_.Sides()) {
        if (side->IsAlive()) {
            (side->IsComputerControlled() ? computers : humans).push_back(side);
        } else if (!side->IsComputerControlled()) {
            side->SetComputerControlled(true);
            fallen_human = true;
        }
    }
    int standing = 0;
    for (const auto& city : game_.GetMap().Cities()) {
        if (!city.IsRazed()) standing++;
    }

    Ending ending;
    if (humans.empty() && computers.empty()) {
        game_.SetOver(true);
        ending.over = true;
        ending.message = "Alas! No more players are left!";
        return ending;
    }
    if (!game_.IsWon() && humans.empty() && fallen_human && !game_.NoHumansSaid()) {
        game_.SetNoHumansSaid(true);
        ending.no_humans = true;
    }
    if (!game_.IsWon() && humans.size() == 1 && computers.empty()) {
        int mine = static_cast<int>(game::SideCities(game_, *humans[0]).size());
        if (mine * 2 > standing) {
            game_.SetWon(true);
            history::RecordDeed(game_, humans[0], DeedType::kSideVictorious, humans[0]->Index(), 0,
                                "");
            ending.won = true;
            ending.winner = humans[0];
            ending.message = humans[0]->Name() + " rules the world!";
        }
    }
    if (!game_.IsWon() && humans.empty() && computers.size() == 1) {
        Side* winner = computers[0];
        game_.SetWon(true);
        winner->SetComputerControlled(false);
        history::RecordDeed(game_, winner, DeedType::kSideVictorious, winner->Index(), 0, "");
        ending.triumph = true;
        ending.winner = winner;
        ending.message = winner->Name() + " has triumphed!";
    }
    if (!game_.IsWon() && humans.size() == 1 && !computers.empty() && !game_.SurrenderOffered()) {
        int mine = static_cast<int>(game::SideCities(game_, *humans[0]).size());
        int biggest = 0;
        for (Side* side : computers)
            biggest = std::max(biggest, static_cast<int>(game::SideCities(game_, *side).size()));
        int all = game_.GetMap().CityCount();
        if (mine > all / 2 && mine > biggest + all / 8) {
            game_.SetSurrenderOffered(true);
            ending.surrender = true;
            ending.message = "Your enemies offer their surrender!";
        }
    }
    return ending;
}

}  // namespace w2
