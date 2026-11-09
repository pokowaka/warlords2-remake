/**
 * @file
 * @brief Implementation of `CityEconomy`.
 */
#include "warlords/city_economy.hpp"

#include <algorithm>
#include <cstdlib>

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
    for (City* city : cities) total += city->income;
    int per_city = game::ItemBonus(game_, side, rules::kItemGoldPerCity);
    return total + static_cast<int>(cities.size()) * per_city;
}

int CityEconomy::Upkeep(const Side& side) const {
    int total = 0;
    for (Army* army : game_.armies) {
        if (army->owner == side.index && !army->transit) {
            int cost = army->upkeep;
            if (army->at_sea) cost = std::max(rules::kSeaMinUpkeep, cost);
            total += cost;
        }
    }
    return total;
}

void CityEconomy::ApplyIncome(Side& side) {
    side.income = Income(side);
    side.upkeep_total = Upkeep(side);
    side.gold = std::max(0, side.gold + side.income - side.upkeep_total);
}

bool CityEconomy::Deliver(Army* army, City& city) {
    auto [x, y] = game::FreeTileIn(game_, city, false);
    if (x == kNone || city.owner_index != army->owner) {
        if (army->returning || army->home_city == city.index) return false;
        City* home = game_.map->City(army->home_city);
        army->returning = true;
        army->transit = Transit{kTransitTurns, home ? home->index : kNone};
        return true;
    }
    army->transit.reset();
    army->returning = false;
    game_.armies.Relocate(army, TilePos(x, y));
    return true;
}

std::vector<Produced> CityEconomy::RunProduction(Side& side) {
    std::vector<Produced> arrived, built;
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        Army* army = game_.armies[i];
        if (army->transit && army->owner == side.index) {
            army->transit->turns--;
            if (army->transit->turns <= 0) {
                if (army->transit->dest == game::kStandard) {
                    auto [std_x, std_y] = StandardAt(side);
                    army->transit.reset();
                    if (std_x != kNone &&
                        static_cast<int>(game::ArmiesAt(game_, std_x, std_y).size()) <
                                rules::kMaxStack) {
                        army->returning = false;
                        game_.armies.Relocate(army, TilePos(std_x, std_y));
                        arrived.push_back(
                                Produced{ProducedKind::kArrived, army->type, CityId::None(), true});
                    } else {
                        army->returning = true;
                        army->transit = Transit{kTransitTurns, army->home_city};
                    }
                } else {
                    City* dest = game_.map->City(army->transit->dest);
                    army->transit.reset();
                    if (!dest || !Deliver(army, *dest))
                        army->disbanded = true;
                    else if (!army->transit)
                        arrived.push_back(Produced{ProducedKind::kArrived, army->type,
                                                   CityId(dest->index), false});
                }
            }
        }
    }

    for (City* city : game::SideCities(game_, side)) {
        if (city->producing != kNone && city->countdown > 0) {
            city->countdown--;
            if (city->countdown <= 0) {
                city->countdown = 0;
                if (side.gold > 0) {
                    bool vectored = city->vector_to != kNone && city->vector_to != city->index;
                    auto [x, y] = game::FreeTileIn(game_, *city, false);
                    if (x != kNone || vectored) {
                        const Slot& slot = city->slots[city->producing];
                        Army army = rules::ArmyFromSlot(slot, side.enhanced);
                        army.owner = side.index;
                        army.home_city = city->index;
                        army.moves = army.max_moves;
                        army.x = x;
                        army.y = y;
                        if (vectored) {
                            army.transit = Transit{kTransitTurns, city->vector_to};
                            army.x = kNone;
                            army.y = kNone;
                        }
                        game_.Add(army);
                        built.push_back(
                                Produced{vectored ? ProducedKind::kSent : ProducedKind::kBuilt,
                                         army.type, CityId(city->index), false});
                    }
                }
            }
        }
    }

    game_.armies.RemoveDisbanded();
    TurnReport& news = game_.TurnNews(SideId(side.index));
    news.produced = std::move(arrived);
    for (const auto& entry : built) news.produced.push_back(entry);
    return built;
}

bool CityEconomy::Producing(const City& city) const {
    return city.producing != kNone && city.countdown > 0;
}

void CityEconomy::SetProduction(City& city, int slot_index) {
    city.producing = slot_index;
    city.countdown = slot_index != kNone ? city.slots[slot_index].time : 0;
    if (slot_index == kNone) city.vector_to = kNone;
}

bool CityEconomy::SetCityProduction(const Side* who, City& city, int slot) {
    if (slot < 0 || slot >= static_cast<int>(city.slots.size())) return false;
    if ((who ? who->gold : 0) < city.slots[slot].cost + 30 && game_.turn > 5) return false;
    SetProduction(city, slot);
    return true;
}

void CityEconomy::ProduceNeutralGarrison(City& city, const Side* who) {
    if (city.producing != kNone && city.producing < static_cast<int>(city.slots.size())) {
        Army army = rules::ArmyFromSlot(city.slots[city.producing], who && who->enhanced);
        army.x = city.x;
        army.y = city.y;
        army.owner = city.owner_index;
        army.moves = 0;
        army.home_city = city.index;
        game_.Add(army);
    }
    city.producing = kNone;
    city.countdown = 0;
}

void CityEconomy::NeutralProduction(const Side* who) {
    if (!(game_.map->options.neutral_cities > 1)) return;
    for (int i = static_cast<int>(game_.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game_.map->cities[i];
        if (city.owner_index == kNone && !city.razed && city.attacked) {
            if (city.countdown != 0) {
                city.countdown--;
                if (city.countdown == 0) ProduceNeutralGarrison(city, who);
            }
            const Slot* slot =
                    city.producing != kNone && city.producing < static_cast<int>(city.slots.size())
                            ? &city.slots[city.producing]
                            : nullptr;
            bool building = slot && city.countdown != 0 && city.countdown != slot->time;
            if (!building &&
                std::min(static_cast<int>(game::ArmiesAt(game_, city.x, city.y).size()),
                         rules::kMaxStack) < 4) {
                int best = rules::BestSlot(city.slots, 2, game_.types, who && who->enhanced);
                if (best != kNone) SetCityProduction(who, city, best);
            }
        }
    }
}

std::vector<City*> CityEconomy::VectoredTo(const City* dest, const Side* side) const {
    std::vector<City*> out;
    for (auto& city : game_.map->cities) {
        if (!dest) {
            if (city.vector_to == game::kStandard && side && city.owner_index == side->index)
                out.push_back(&city);
        } else if (city.vector_to == dest->index && &city != dest) {
            out.push_back(&city);
        }
    }
    return out;
}

CityEconomy::CoordPair CityEconomy::StandardAt(const Side& side) const {
    if (side.index < static_cast<int>(game_.map->items.size())) {
        const Item& item = game_.map->items[side.index];
        if (item.status == 1 && item.planted && item.x != kNone) return {item.x, item.y};
    }
    return {kNone, kNone};
}

bool CityEconomy::PlantFlag(Side& side, Army* hero) {
    int terrain = scn::TerrainAt(*game_.map, hero->x, hero->y);
    if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
        terrain == move::kSite)
        return false;
    for (const auto& item : game_.map->items) {
        if (item.planted && item.x == hero->x && item.y == hero->y) return false;
    }
    if (side.index >= static_cast<int>(game_.map->items.size())) return false;
    Item* standard = &game_.map->items[side.index];
    auto item_it = std::find(hero->items.begin(), hero->items.end(), standard);
    if (item_it != hero->items.end()) {
        hero->items.erase(item_it);
        standard->status = 1;
        standard->x = hero->x;
        standard->y = hero->y;
        standard->planted = true;
        return true;
    }
    return false;
}

bool CityEconomy::VectorToStandard(City& city, const Side& side) {
    if (StandardAt(side).first == kNone) return false;
    if (city.vector_to != game::kStandard &&
        static_cast<int>(VectoredTo(nullptr, &side).size()) >= game::kMaxVectoredTo)
        return false;
    city.vector_to = game::kStandard;
    return true;
}

bool CityEconomy::Vector(City& city, City* dest_city) {
    if (dest_city == &city) dest_city = nullptr;
    if (dest_city && city.vector_to != dest_city->index &&
        static_cast<int>(VectoredTo(dest_city).size()) >= game::kMaxVectoredTo) {
        return false;
    }
    city.vector_to = dest_city ? dest_city->index : kNone;
    return true;
}

City* CityEconomy::NearestCity(int x, int y, const Side* side, const Side* seer) const {
    City* best = nullptr;
    int best_d = kNone;
    for (auto& city : game_.map->cities) {
        bool allowed = !side || city.owner_index == side->index;
        if (allowed && seer) {
            int seer_idx = seer->index;
            allowed = game::Seen(game_, seer_idx, city.x, city.y) ||
                      game::Seen(game_, seer_idx, city.x + 1, city.y) ||
                      game::Seen(game_, seer_idx, city.x, city.y + 1) ||
                      game::Seen(game_, seer_idx, city.x + 1, city.y + 1);
        }
        if (allowed) {
            int dist = move::Distance(x, y, city.x, city.y);
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
    for (const auto* army_type : game_.types.List()) {
        if (army_type->price >= 0) out.push_back(army_type);
    }
    return out;
}

std::string CityEconomy::CannotBuy(const Side& side, const City& city,
                                   const ArmyType& army_type) const {
    for (const auto& slot : city.slots) {
        if (slot.type == army_type.id) return "has it";
    }
    if (side.gold < army_type.price) return "too dear";
    return "";
}

int CityEconomy::BuySlot(const City& city) const {
    if (static_cast<int>(city.slots.size()) < rules::kProductionSlots)
        return static_cast<int>(city.slots.size());
    return 0;
}

void CityEconomy::BuyProduction(Side& side, City& city, int slot_index, int type_id) {
    const ArmyType* army_type = game_.types.ById(type_id);
    bool building = city.producing != kNone;
    int was = city.producing;
    Slot slot{army_type->id,   army_type->name, army_type->strength,       army_type->time,
              army_type->cost, army_type->move, std::abs(army_type->price)};
    if (slot_index >= static_cast<int>(city.slots.size())) city.slots.resize(slot_index + 1);
    city.slots[slot_index] = slot;
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    side.gold -= army_type->price;
    if (building && was == slot_index) {
        city.producing = kNone;
        city.countdown = 0;
        city.vector_to = kNone;
    }
}

void CityEconomy::SortProduction(City& city) {
    int building = city.producing;
    auto& slots = city.slots;
    for (int i = 1; i < static_cast<int>(slots.size()); i++) {
        int pos = i;
        while (pos > 0 && slots[pos].price < slots[pos - 1].price) {
            std::swap(slots[pos], slots[pos - 1]);
            if (building == pos)
                building = pos - 1;
            else if (building == pos - 1)
                building = pos;
            pos--;
        }
    }
    if (city.producing != kNone) city.producing = building;
}

void CityEconomy::RenameCity(City& city, const std::string& name) {
    city.name = name;
}

int CityEconomy::SlotPillageValue(const Slot& slot) const {
    return std::abs(game_.types.ById(slot.type)->price) / 2;
}

void CityEconomy::RecomputeCityAfterSpoils(City& city) {
    city.defence = rules::CityDefence(static_cast<int>(city.slots.size()));
    if (city.producing != kNone && city.producing >= static_cast<int>(city.slots.size())) {
        city.producing = kNone;
        city.countdown = 0;
    }
}

std::shared_ptr<QuestResult> CityEconomy::Occupy(Side& side, City& city, const ArmyStack& stack) {
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    return quest::Notify(game_, side, "occupy", event);
}

game::Spoils CityEconomy::Pillage(Side& side, City& city, const ArmyStack& stack) {
    game::Spoils out;
    if (city.slots.empty()) return out;
    Slot slot = city.slots.back();
    city.slots.pop_back();
    int gold = SlotPillageValue(slot);
    out.gold = gold;
    out.lost.emplace_back(slot.type, gold);
    side.gold += gold;
    game::AddDiploScore(game_, side, game_.rng.Dice(1, 5, 0));
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
    if (city.slots.size() < 2) return out;
    int gold = 0;
    for (size_t i = 1; i < city.slots.size(); i++) {
        int val = SlotPillageValue(city.slots[i]);
        out.lost.emplace_back(city.slots[i].type, val);
        gold += val;
    }
    city.slots.resize(1);
    out.gold = gold;
    side.gold += gold;
    game::AddDiploScore(game_, side, game_.rng.Dice(1, 10, 5));
    RecomputeCityAfterSpoils(city);
    quest::Event event;
    event.city = &city;
    event.gold = gold;
    event.stack = stack;
    quest::Notify(game_, side, "pillage", event);
    return out;
}

void CityEconomy::MakeRuins(City& city) {
    city.razed = true;
    city.razed_by = city.owner_index == kNone ? 0 : city.owner_index;
    city.owner_index = kNone;
    city.claim = rules::kNeutral;
    city.slots.clear();
    city.producing = kNone;
    city.countdown = 0;
    city.vector_to = kNone;
    city.defence = 0;
    city.income = 0;
    for (auto& other_city : game_.map->cities) {
        if (other_city.vector_to == city.index) {
            other_city.vector_to = kNone;
            other_city.producing = kNone;
            other_city.countdown = 0;
        }
    }
    scn::SetCityTiles(*game_.map, city);
    game_.GetPathfinder().Invalidate();
}

void CityEconomy::Raze(Side& side, City& city, const ArmyStack& stack, bool own_city) {
    MakeRuins(city);
    if (own_city) {
        game::AddDiploScore(game_, side, game_.rng.Dice(1, 25, 25));
        return;
    }
    game::AddDiploScore(game_, side, game_.rng.Dice(1, 15, 10));
    quest::Event event;
    event.city = &city;
    event.stack = stack;
    quest::Notify(game_, side, "raze", event);
}

}  // namespace w2
