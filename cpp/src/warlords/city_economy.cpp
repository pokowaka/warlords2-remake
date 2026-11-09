/**
 * @file
 * @brief Implementation of `CityEconomy`.
 */
#include "warlords/city_economy.hpp"

#include <algorithm>
#include <cstdlib>
#include <variant>

#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {
constexpr int kTransitTurns = 2;
}  // namespace

int CityEconomy::Income(const Side& side) const {
    int total = 0;
    auto cities = game::SideCities(game_, side);
    for (City* city : cities) total += city->Income();
    int per_city = game::ItemBonus(game_, side, rules::kItemGoldPerCity);
    return total + static_cast<int>(cities.size()) * per_city;
}

int CityEconomy::Upkeep(const Side& side) const {
    int total = 0;
    for (Army* army : game_.Armies()) {
        if (army->OwnedBy(side.Id()) && !army->InTransit()) {
            int cost = army->Upkeep();
            if (army->IsAtSea()) cost = std::max(rules::kSeaMinUpkeep, cost);
            total += cost;
        }
    }
    return total;
}

void CityEconomy::ApplyIncome(Side& side) {
    side.SetIncome(Income(side));
    side.SetUpkeepTotal(Upkeep(side));
    side.SetGold(side.Gold() + side.NetIncome());
}

bool CityEconomy::Deliver(Army* army, City& city) {
    auto [x, y] = game::FreeTileIn(game_, city, false);
    if (x == kNone || city.Owner() != army->Owner()) {
        if (army->IsReturning() || army->HomeCity() == city.Id()) return false;
        City* home = game_.GetMap().CityById(army->HomeCity());
        VectorTarget home_dest = home ? VectorTarget{home->Id()} : VectorTarget{};
        army->SendIntoTransit(Transit{kTransitTurns, home_dest}, /*returning=*/true);
        return true;
    }
    army->ArriveAt(TilePos(x, y));
    return true;
}

std::vector<Produced> CityEconomy::RunProduction(Side& side) {
    std::vector<Produced> arrived, built;
    for (int i = static_cast<int>(game_.Armies().size()) - 1; i >= 0; i--) {
        Army* army = game_.Armies()[i];
        if (army->InTransit() && army->OwnedBy(side.Id())) {
            if (army->TickTransit()) {
                VectorTarget dest_target = army->TransitState()->dest;
                if (std::holds_alternative<PlantedStandard>(dest_target)) {
                    auto [std_x, std_y] = StandardAt(side);
                    army->ClearTransit();
                    auto here = game::ArmiesAt(game_, std_x, std_y);
                    if (std_x != kNone && (here.empty() || here[0]->OwnedBy(side.Id())) &&
                        static_cast<int>(here.size()) < rules::kMaxStack) {
                        army->ArriveAt(TilePos(std_x, std_y));
                        arrived.push_back(Produced{ProducedKind::kArrived, army->TypeIndex(),
                                                   CityId::None(), true});
                    } else {
                        VectorTarget home_dest = army->HomeCity().IsValid()
                                                         ? VectorTarget{army->HomeCity()}
                                                         : VectorTarget{};
                        army->SendIntoTransit(Transit{kTransitTurns, home_dest},
                                              /*returning=*/true);
                    }
                } else {
                    const auto* dest_city_id = std::get_if<CityId>(&dest_target);
                    City* dest = dest_city_id ? game_.GetMap().CityById(*dest_city_id) : nullptr;
                    army->ClearTransit();
                    if (!dest || !Deliver(army, *dest))
                        army->SetDisbanded(true);
                    else if (!army->InTransit())
                        arrived.push_back(Produced{ProducedKind::kArrived, army->TypeIndex(),
                                                   dest->Id(), false});
                }
            }
        }
    }

    for (City* city : game::SideCities(game_, side)) {
        if (city->ProducingSlot() != kNone && city->Countdown() > 0) {
            if (city->TickCountdown()) {
                if (side.Gold() > 0) {
                    bool vectored = city->IsVectored() && city->VectoredCityId() != city->Id();
                    auto [x, y] = game::FreeTileIn(game_, *city, false);
                    if (x != kNone || vectored) {
                        const Slot& slot = city->SlotAt(static_cast<size_t>(city->ProducingSlot()));
                        Army army = rules::ArmyFromSlot(slot, side.IsEnhanced());
                        army.SetOwner(side.Id());
                        army.SetHomeCity(city->Id());
                        army.SetMoves(army.MaxMoves());
                        army.PlaceAt(TilePos(x, y));
                        if (vectored) {
                            army.SendIntoTransit(Transit{kTransitTurns, city->VectorDestination()});
                        }
                        game_.Add(army);
                        built.push_back(
                                Produced{vectored ? ProducedKind::kSent : ProducedKind::kBuilt,
                                         army.TypeIndex(), city->Id(), false});
                    }
                }
            }
        }
    }

    game_.Armies().RemoveDisbanded();
    TurnReport& news = game_.TurnNews(side.Id());
    news.produced = std::move(arrived);
    for (const auto& entry : built) news.produced.push_back(entry);
    return built;
}

bool CityEconomy::Producing(const City& city) const {
    return city.ProducingSlot() != kNone && city.Countdown() > 0;
}

void CityEconomy::SetProduction(City& city, int slot_index) {
    city.SelectProduction(slot_index);
}

bool CityEconomy::SetCityProduction(const Side* who, City& city, int slot) {
    if (slot < 0 || slot >= city.SlotCount()) return false;
    if ((who ? who->Gold() : 0) < city.SlotAt(static_cast<size_t>(slot)).cost + 30 &&
        game_.Turn() > 5)
        return false;
    SetProduction(city, slot);
    return true;
}

void CityEconomy::ProduceNeutralGarrison(City& city, const Side* who) {
    if (const Slot* active = city.ActiveSlot()) {
        Army army = rules::ArmyFromSlot(*active, who && who->IsEnhanced());
        army.PlaceAt(city.Position());
        army.SetOwner(city.Owner());
        army.SetMoves(0);
        army.SetHomeCity(city.Id());
        game_.Add(army);
    }
    city.SetProducingSlot(kNone);
    city.SetCountdown(0);
}

void CityEconomy::NeutralProduction(const Side* who) {
    Map& map = game_.GetMap();
    if (!(map.GameOptions().neutral_cities > 1)) return;
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (city.IsNeutral() && !city.IsRazed() && city.HasBeenAttacked()) {
            if (city.Countdown() != 0) {
                if (city.TickCountdown()) ProduceNeutralGarrison(city, who);
            }
            const Slot* slot = city.ActiveSlot();
            bool building = slot && city.Countdown() != 0 && city.Countdown() != slot->time;
            if (!building &&
                std::min(static_cast<int>(game::ArmiesAt(game_, city.X(), city.Y()).size()),
                         rules::kMaxStack) < 4) {
                int best = rules::BestSlot(city.Slots(), 2, game_.ArmyTypes(),
                                           who && who->IsEnhanced());
                if (best != kNone) SetCityProduction(who, city, best);
            }
        }
    }
}

std::vector<City*> CityEconomy::VectoredTo(const City* dest, const Side* side) const {
    std::vector<City*> out;
    for (auto& city : game_.GetMap().Cities()) {
        if (!dest) {
            if (city.IsVectoredToStandard() && side && city.OwnedBy(side->Id()))
                out.push_back(&city);
        } else if (city.VectoredCityId() == dest->Id() && &city != dest) {
            out.push_back(&city);
        }
    }
    return out;
}

CityEconomy::CoordPair CityEconomy::StandardAt(const Side& side) const {
    const auto items = game_.GetMap().Items();
    if (side.Index() < static_cast<int>(items.size())) {
        const Item& item = items[side.Index()];
        if (item.status == 1 && item.planted && item.x != kNone) return {item.x, item.y};
    }
    return {kNone, kNone};
}

bool CityEconomy::PlantFlag(Side& side, Army* hero) {
    Map& map = game_.GetMap();
    int terrain = scn::TerrainAt(map, hero->X(), hero->Y());
    if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
        terrain == move::kSite)
        return false;
    for (const auto& item : map.Items()) {
        if (item.planted && item.x == hero->X() && item.y == hero->Y()) return false;
    }
    if (side.Index() >= map.ItemCount()) return false;
    Item* standard = &map.Items()[side.Index()];
    if (hero->RemoveItem(standard)) {
        standard->status = 1;
        standard->x = hero->X();
        standard->y = hero->Y();
        standard->planted = true;
        return true;
    }
    return false;
}

bool CityEconomy::VectorToStandard(City& city, const Side& side) {
    if (StandardAt(side).first == kNone) return false;
    if (!city.IsVectoredToStandard() &&
        static_cast<int>(VectoredTo(nullptr, &side).size()) >= game::kMaxVectoredTo)
        return false;
    city.VectorToStandard();
    return true;
}

bool CityEconomy::Vector(City& city, City* dest_city) {
    if (dest_city == &city) dest_city = nullptr;
    if (dest_city && city.VectoredCityId() != dest_city->Id() &&
        static_cast<int>(VectoredTo(dest_city).size()) >= game::kMaxVectoredTo) {
        return false;
    }
    if (dest_city) {
        city.VectorToCity(dest_city->Id());
    } else {
        city.ClearVector();
    }
    return true;
}

City* CityEconomy::NearestCity(int x, int y, const Side* side, const Side* seer) const {
    City* best = nullptr;
    int best_d = kNone;
    for (auto& city : game_.GetMap().Cities()) {
        bool allowed = !side || city.OwnedBy(side->Id());
        if (allowed && seer) {
            int seer_idx = seer->Index();
            allowed = game::Seen(game_, seer_idx, city.X(), city.Y()) ||
                      game::Seen(game_, seer_idx, city.X() + 1, city.Y()) ||
                      game::Seen(game_, seer_idx, city.X(), city.Y() + 1) ||
                      game::Seen(game_, seer_idx, city.X() + 1, city.Y() + 1);
        }
        if (allowed) {
            int dist = move::Distance(x, y, city.X(), city.Y());
            if (best_d == kNone || dist < best_d) {
                best = &city;
                best_d = dist;
            }
        }
    }
    return best;
}

CityEconomy::BuyableTypeList CityEconomy::BuyableTypes() const {
    BuyableTypeList out;
    for (const auto* army_type : game_.ArmyTypes().List()) {
        if (army_type->price >= 0) out.push_back(army_type);
    }
    return out;
}

std::string CityEconomy::CannotBuy(const Side& side, const City& city,
                                   const ArmyType& army_type) const {
    for (const auto& slot : city.Slots()) {
        if (slot.type == army_type.id) return "has it";
    }
    if (side.Gold() < army_type.price) return "too dear";
    return "";
}

int CityEconomy::BuySlot(const City& city) const {
    if (city.SlotCount() < rules::kProductionSlots) return city.SlotCount();
    return 0;
}

void CityEconomy::BuyProduction(Side& side, City& city, int slot_index, int type_id) {
    const ArmyType* army_type = game_.ArmyTypes().ById(type_id);
    bool building = city.ProducingSlot() != kNone;
    int was = city.ProducingSlot();
    Slot slot{army_type->id,   army_type->name, army_type->strength,       army_type->time,
              army_type->cost, army_type->move, std::abs(army_type->price)};
    city.SetSlot(static_cast<size_t>(slot_index), std::move(slot));
    city.SetDefence(rules::CityDefence(city.SlotCount()));
    side.AddGold(-army_type->price);
    if (building && was == slot_index) {
        city.StopProduction();
    }
}

void CityEconomy::SortProduction(City& city) {
    city.SortSlotsByPrice();
}

void CityEconomy::RenameCity(City& city, const std::string& name) {
    city.Rename(name);
}

int CityEconomy::SlotPillageValue(const Slot& slot) const {
    return std::abs(game_.ArmyTypes().ById(slot.type)->price) / 2;
}

void CityEconomy::RecomputeCityAfterSpoils(City& city) {
    city.SetDefence(rules::CityDefence(city.SlotCount()));
}

std::shared_ptr<QuestResult> CityEconomy::Occupy(Side& side, City& city, const ArmyStack& stack) {
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    return quest::Notify(game_, side, "occupy", event);
}

game::Spoils CityEconomy::Pillage(Side& side, City& city, const ArmyStack& stack) {
    game::Spoils out;
    if (city.Slots().empty()) return out;
    Slot slot = city.PopLastSlot();
    int gold = SlotPillageValue(slot);
    out.gold = gold;
    out.lost.emplace_back(slot.type, gold);
    side.AddGold(gold);
    game::AddDiploScore(game_, side, game_.GetRng().Dice(1, 5, 0));
    RecomputeCityAfterSpoils(city);
    quest::Event event;
    event.city = &city;
    event.gold = gold;
    event.stack = stack;
    quest::Notify(game_, side, "pillage", event);
    return out;
}

game::Spoils CityEconomy::Sack(Side& side, City& city, const ArmyStack& stack) {
    game::Spoils out;
    if (city.Slots().size() < 2) return out;
    int gold = 0;
    for (size_t i = 1; i < city.Slots().size(); i++) {
        int val = SlotPillageValue(city.SlotAt(i));
        out.lost.emplace_back(city.SlotAt(i).type, val);
        gold += val;
    }
    city.TruncateSlots(1);
    out.gold = gold;
    side.AddGold(gold);
    game::AddDiploScore(game_, side, game_.GetRng().Dice(1, 10, 5));
    RecomputeCityAfterSpoils(city);
    quest::Event event;
    event.city = &city;
    event.gold = gold;
    event.stack = stack;
    quest::Notify(game_, side, "pillage", event);
    return out;
}

void CityEconomy::MakeRuins(City& city) {
    city.Raze(city.Owner());
    Map& map = game_.GetMap();
    for (auto& other_city : map.Cities()) {
        if (other_city.VectoredCityId() == city.Id()) {
            other_city.StopProduction();
        }
    }
    scn::SetCityTiles(map, city);
    game_.GetPathfinder().Invalidate();
}

void CityEconomy::Raze(Side& side, City& city, const ArmyStack& stack, bool own_city) {
    MakeRuins(city);
    if (own_city) {
        game::AddDiploScore(game_, side, game_.GetRng().Dice(1, 25, 25));
        return;
    }
    game::AddDiploScore(game_, side, game_.GetRng().Dice(1, 15, 10));
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    quest::Notify(game_, side, "raze", event);
}

}  // namespace w2
