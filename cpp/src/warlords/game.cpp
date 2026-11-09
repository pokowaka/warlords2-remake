/**
 * @file
 * @brief Implementation of `Game` aggregate accessors, fog-of-war queries, and `w2::game` service
 * wrappers.
 */
#include "warlords/game.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"

#include "warlords/armytype.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/site.hpp"

namespace w2 {

Game::Game() : pathfinder_(std::make_unique<Pathfinder>()) {}
Game::~Game() = default;
Game::Game(Game&&) noexcept = default;
Game& Game::operator=(Game&&) noexcept = default;

Pathfinder& Game::GetPathfinder() {
    if (!pathfinder_) pathfinder_ = std::make_unique<Pathfinder>();
    return *pathfinder_;
}

const Pathfinder& Game::GetPathfinder() const {
    ABSL_DCHECK(pathfinder_ != nullptr);
    return *pathfinder_;
}

void Game::SetCityOwner(City& city, SideId owner) {
    city.SetOwner(owner);
    GetPathfinder().Invalidate();
}

namespace game {

std::vector<Army*> ArmiesAt(const Game& game, int x, int y) {
    return game.ArmiesOnTile(x, y);
}

City* CityAt(const Game& game, int x, int y) {
    return game.HasMap() ? const_cast<City*>(game.GetMap().CityAtFootprint(x, y)) : nullptr;
}

void SetCityOwner(Game& game, City& city, int side_index) {
    game.SetCityOwner(city, SideId(side_index));
}

std::vector<City*> SideCities(const Game& game, const Side& side) {
    std::vector<City*> out;
    for (const auto& city : game.GetMap().Cities()) {
        if (city.OwnedBy(side.Id())) out.push_back(const_cast<City*>(&city));
    }
    return out;
}

std::vector<Army*> SideArmies(const Game& game, const Side& side) {
    std::vector<Army*> out;
    for (Army* army : game.Armies()) {
        if (army->OwnedBy(side.Id())) out.push_back(army);
    }
    return out;
}

int ItemBonus(const Game& game, const Side& side, int item_type) {
    int total = 0;
    for (Army* army : game.Armies()) {
        if (army->OwnedBy(side.Id())) {
            for (Item* item : army->Items()) {
                if (item->type == item_type) total += std::max(1, item->value);
            }
        }
    }
    return total;
}

std::pair<int, int> FreeTileIn(Game& game, const City& city, bool spill,
                               std::pair<int, int>* last) {
    static const int kOffsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const int map_width = game.GetMap().Width();
    const int map_height = game.GetMap().Height();
    for (const auto& offset : kOffsets) {
        int x = city.X() + offset[0], y = city.Y() + offset[1];
        if (x < map_width && y < map_height &&
            static_cast<int>(ArmiesAt(game, x, y).size()) < rules::kMaxStack) {
            return {x, y};
        }
    }
    if (!spill) return {kNone, kNone};
    int last_x = kNone, last_y = kNone;
    for (int i = 0; i < 20; i++) {
        int x = city.X() + game.GetRng().Dice(1, 3, -2);
        int y = city.Y() + game.GetRng().Dice(1, 3, -2);
        if (x >= 0 && y >= 0 && x < map_width && y < map_height) {
            last_x = x;
            last_y = y;
            if (ArmiesAt(game, x, y).empty()) return {x, y};
        }
    }
    if (last) *last = {last_x, last_y};
    return {kNone, kNone};
}

bool TowerAt(const Game& game, int x, int y) {
    return game.HasTower(x, y);
}

void TidyTowers(Game& game) {
    game.ClearAbandonedTowers();
}

bool HeroWithDoubleMoveAt(const Game& game, const Side& side, int x, int y) {
    for (Army* army : game.Armies()) {
        if (army->OwnedBy(side.Id()) && army->OnMap() && army->X() == x && army->Y() == y &&
            army->Hero()) {
            for (Item* item : army->Items()) {
                if (item->type == rules::kItemDoubleMove) return true;
            }
        }
    }
    return false;
}

Sign* SignAt(Game& game, int x, int y) {
    return game.GetMap().SignAt(x, y);
}

void AddDiploScore(Game& game, Side& side, int points) {
    (void)game;
    side.AddDiploScore(points);
}

bool HasActiveHuman(const Game& game) {
    for (const Side* side : game.Sides()) {
        if (side && side->IsAlive() && !side->IsComputerControlled() &&
            !SideCities(game, *side).empty())
            return true;
    }
    return false;
}

int ViewerSide(const Game& game, const Side* player) {
    if (!HasActiveHuman(game)) return kNone;
    return player ? player->Index() : kNone;
}

bool Seen(const Game& game, int side, int x, int y) {
    const Map& map = game.GetMap();
    if (map.GameOptions().hidden_map == 0) return true;
    if (!map.InBounds(x, y)) return false;
    if (side == kNone) return true;
    int map_width = map.Width(), map_height = map.Height();
    SideId side_id(side);
    if (game.Explored().IsExplored(side_id, map_width, map_height, x, y)) return true;
    if (side >= 0 && side < 8) {
        for (int ally = 0; ally < 8; ally++) {
            SideId ally_id(ally);
            if (ally != side && game.Diplomacy().AlliedFlag(side_id, ally_id) &&
                game.Explored().IsExplored(ally_id, map_width, map_height, x, y)) {
                return true;
            }
        }
    }
    return false;
}

int Reveal(Game& game, int side, int x, int y, bool flying) {
    const Map& map = game.GetMap();
    if (map.GameOptions().hidden_map == 0) return 0;
    int map_width = map.Width(), map_height = map.Height();
    SideId side_id(side);
    game.Explored().EnsureSide(side_id, map_width, map_height);
    bool on_city = map.CityAtFootprint(x, y) != nullptr;
    int radius = (flying || on_city) ? 2 : 1;
    int found = 0;
    for (int tile_y = y - radius; tile_y <= y + radius; tile_y++) {
        for (int tile_x = x - radius; tile_x <= x + radius; tile_x++) {
            if (game.Explored().MarkExplored(side_id, map_width, map_height, tile_x, tile_y))
                found++;
        }
    }
    return found;
}

const int kFogDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
const int kFogDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
const int kFogCell[256] = {
    255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255,
    255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255,
    255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255,
    255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255,
    255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,   3,
    12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  4,   4,
    4,   4,   4,   4,   4,   13,  4,   4,   4,   4,   8,   8,   8,   9,   255, 255, 255, 255, 255,
    255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,
    255, 255, 255, 255, 3,   3,   3,   12,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255,
    255, 255, 255, 255, 10,  255, 255, 255, 255, 255, 255, 255, 10,  255, 255, 255, 255, 3,   3,
    3,   12,  255, 11,  255, 11,  255, 11,  255, 1,   255, 11,  255, 11,  255, 11,  255, 1,   255,
    11,  255, 11,  255, 11,  255, 1,   255, 11,  255, 11,  3,   6,   3,   2,   255, 11,  255, 11,
    255, 11,  255, 1,   255, 11,  255, 11,  255, 11,  255, 1,   4,   5,   4,   5,   4,   5,   4,
    0,   4,   5,   4,   5,   8,   7,   8,   14,
};

int FogCell(const Game& game, int side, int x, int y) {
    if (Seen(game, side, x, y)) return kNone;
    const Map& map = game.GetMap();
    int bits = 0;
    for (int i = 0; i < 8; i++) {
        int nbr_x = x + kFogDx[i], nbr_y = y + kFogDy[i];
        if (!map.InBounds(nbr_x, nbr_y) || !Seen(game, side, nbr_x, nbr_y)) bits += 1 << i;
    }
    int cell = kFogCell[bits];
    return cell == kFogNone ? kNone : cell;
}

void TidyExplored(Game& game, int side) {
    const Map& map = game.GetMap();
    if (map.GameOptions().hidden_map == 0) return;
    int map_width = map.Width(), map_height = map.Height();
    SideId side_id(side);
    game.Explored().EnsureSide(side_id, map_width, map_height);
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < map_width; x++) {
            for (int y = 0; y < map_height; y++) {
                if (!game.Explored().IsExplored(side_id, map_width, map_height, x, y)) {
                    int bits = 0;
                    for (int i = 0; i < 8; i++) {
                        int nbr_x = x + kFogDx[i], nbr_y = y + kFogDy[i];
                        if (!game.Explored().IsExplored(side_id, map_width, map_height, nbr_x,
                                                        nbr_y))
                            bits += 1 << i;
                    }
                    if (kFogCell[bits] == kFogNone)
                        game.Explored().MarkExplored(side_id, map_width, map_height, x, y);
                }
            }
        }
    }
}

void RevealStart(Game& game, const Side& side) {
    if (game.GetMap().GameOptions().hidden_map == 0) return;
    for (City* city : SideCities(game, side))
        Reveal(game, side.Index(), city->X(), city->Y(), false);
    for (Army* army : SideArmies(game, side)) {
        if (!army->InTransit()) Reveal(game, side.Index(), army->X(), army->Y(), false);
    }
    TidyExplored(game, side.Index());
}

int Income(const Game& game, const Side& side) {
    return CityEconomy(const_cast<Game&>(game)).Income(side);
}
int Upkeep(const Game& game, const Side& side) {
    return CityEconomy(const_cast<Game&>(game)).Upkeep(side);
}
void FollowCapital(Game& game, Side& side) {
    TurnEngine(game).FollowCapital(side);
}
bool StartTurn(Game& game, Side& side) {
    return TurnEngine(game).StartTurn(side);
}
void HumanProduction(Game& game, Side& side) {
    TurnEngine(game).HumanProduction(side);
}
bool Producing(const City& city) {
    return city.ProducingSlot() != kNone && city.Countdown() > 0;
}
Side* EndTurn(Game& game) {
    return TurnEngine(game).EndTurn();
}
Side* Begin(Game& game) {
    return TurnEngine(game).Begin();
}
Side* NextHuman(Game& game) {
    return TurnEngine(game).NextHuman();
}
void Disband(Game& game, Side& side, const std::vector<Army*>& armies) {
    TurnEngine(game).Disband(side, armies);
}
int Loot(const Game& game, const Side* loser) {
    return CombatResolver(const_cast<Game&>(game)).Loot(loser);
}
Battle DecideAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    return CombatResolver(game).DecideAttack(stack, x, y);
}
void ChargeAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    CombatResolver(game).ChargeAttack(stack, x, y);
}
int SinkStranded(Game& game, Side& side, int x, int y) {
    return TurnEngine(game).SinkStranded(side, x, y);
}
Battle ResolveAttack(Game& game, const std::vector<Army*>& stack, int x, int y) {
    return CombatResolver(game).ResolveAttack(stack, x, y);
}
Battle& ApplyAttack(Game& game, Battle& result) {
    return CombatResolver(game).ApplyAttack(result);
}
std::shared_ptr<QuestResult> Occupy(Game& game, Side& side, City& city,
                                    const std::vector<Army*>& stack) {
    return CityEconomy(game).Occupy(side, city, stack);
}
Spoils Pillage(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    return CityEconomy(game).Pillage(side, city, stack);
}
Spoils Sack(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    return CityEconomy(game).Sack(side, city, stack);
}
void Raze(Game& game, Side& side, City& city, const std::vector<Army*>& stack, bool own_city) {
    CityEconomy(game).Raze(side, city, stack, own_city);
}
void Resign(Game& game, Side& side) {
    TurnEngine(game).Resign(side);
}
void AcceptSurrender(Game& game, Side& side) {
    TurnEngine(game).AcceptSurrender(side);
}
bool SurrenderPending(const Game& game) {
    return TurnEngine(const_cast<Game&>(game)).SurrenderPending();
}
std::vector<Fallen> EliminateFallen(Game& game) {
    return TurnEngine(game).EliminateFallen();
}
Ending EndRound(Game& game, Side* last) {
    return TurnEngine(game).EndRound(last);
}
bool SetCityProduction(Game& game, const Side* who, City& city, int slot) {
    return CityEconomy(game).SetCityProduction(who, city, slot);
}
void NeutralProduction(Game& game, const Side* who) {
    CityEconomy(game).NeutralProduction(who);
}
Ending CheckEnd(Game& game) {
    return TurnEngine(game).CheckEnd();
}
std::shared_ptr<SearchResult> SearchHere(Game& game, const std::vector<Army*>& stack, bool human) {
    if (stack.empty()) return nullptr;
    return site::Search(game, stack, stack[0]->X(), stack[0]->Y(), human);
}
std::string DescribeSearch(const SearchResult* result) {
    return SiteExplorer::DescribeSearch(result);
}
void SetProduction(Game& game, City& city, int slot_index) {
    CityEconomy(game).SetProduction(city, slot_index);
}
std::vector<City*> VectoredTo(Game& game, const City* dest, const Side* side) {
    return CityEconomy(game).VectoredTo(dest, side);
}
std::pair<int, int> StandardAt(const Game& game, const Side& side) {
    return CityEconomy(const_cast<Game&>(game)).StandardAt(side);
}
bool PlantFlag(Game& game, Side& side, Army* hero) {
    return CityEconomy(game).PlantFlag(side, hero);
}
bool VectorToStandard(Game& game, City& city, const Side& side) {
    return CityEconomy(game).VectorToStandard(city, side);
}
bool Vector(Game& game, City& city, City* dest_city) {
    return CityEconomy(game).Vector(city, dest_city);
}
City* NearestCity(Game& game, int x, int y, const Side* side, const Side* seer) {
    return CityEconomy(game).NearestCity(x, y, side, seer);
}
std::vector<const ArmyType*> BuyableTypes(const Game& game) {
    return CityEconomy(const_cast<Game&>(game)).BuyableTypes();
}
std::string CannotBuy(const Game& game, const Side& side, const City& city,
                      const ArmyType& army_type) {
    return CityEconomy(const_cast<Game&>(game)).CannotBuy(side, city, army_type);
}
int BuySlot(const City& city) {
    return city.SlotCount() < rules::kProductionSlots ? city.SlotCount() : 0;
}
void BuyProduction(Game& game, Side& side, City& city, int slot_index, int type_id) {
    CityEconomy(game).BuyProduction(side, city, slot_index, type_id);
}
void SortProduction(City& city) {
    CityEconomy::SortProduction(city);
}
void RenameCity(Game&, City& city, const std::string& name) {
    CityEconomy::RenameCity(city, name);
}

}  // namespace game
}  // namespace w2
