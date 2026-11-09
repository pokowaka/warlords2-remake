#include "warlords/ai/groups.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"

namespace w2::ai::groups {

using namespace core;

/** @brief The roles a rally city may be picked from: building, stopped, member, weak, nowhere. */
static const std::set<int> kRallyRoles = {5, 8, 6, 4, 14};

/** @brief The side's cities. */
static std::vector<City*> Own(const Game& game, const Side& side) {
    return cities::Own(game, side);
}

/** @brief The city with an index, or null for kNone. */
static City* CityOrNull(Game& game, int index) {
    return index != kNone ? game.map->City(index) : nullptr;
}
/** @brief The city in a group's slot, or null when the slot is empty. */
static City* CityOrNull(Game& game, const std::map<int, int>& slots, int slot) {
    auto entry = slots.find(slot);
    return entry == slots.end() ? nullptr : CityOrNull(game, entry->second);
}

bool Free(Game& game, Side& side, const City& city, bool members) {
    AIData& data = Data(game, side);
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group->rally == city.Index()) return false;
    }
    int role = Role(data, city);
    if (role == kStop || role == kBuilding) return true;
    return members && role == kMember;
}

bool IsRally(Game& game, Side& side, const City& city) {
    AIData& data = Data(game, side);
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group->rally == city.Index()) return true;
    }
    return false;
}

void Cancel(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    City* rally = CityOrNull(game, group->rally);
    if (rally) SetRole(data, *rally, kStop);
    for (int slot = 4; slot >= 1; slot--) {
        City* member = CityOrNull(game, group->members, slot);
        if (member) SetRole(data, *member, kStop);
    }
    data.groups[group_index] = EmptyGroup();
}

int SackValue(const Game& game, const City& city) {
    int value = 0;
    for (size_t i = 1; i < city.Slots().size(); i++)
        value += std::abs(game.types.ById(city.SlotAt(i).type)->price) / 2;
    return value;
}

void Pillage(Game& game, Side& side, City& city) {
    game::Pillage(game, side, city, {});
    if (hooks.on_spoils) hooks.on_spoils(game, side, city, "pillaged");
}

void Sack(Game& game, Side& side, City& city) {
    if (SackValue(game, city) == 0) return Pillage(game, side, city);
    game::Sack(game, side, city, {});
    if (hooks.on_spoils) hooks.on_spoils(game, side, city, "sacked");
}

City* Raze(Game& game, Side& side, City& city, bool force, Sel* sel) {
    if (!force) {
        auto [neighbours, neighbour_dists] = FindNeighbours(game, city);
        int own_nearby = 0;
        for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
            if (neighbours[j]->OwnerIndex() == side.Index() && neighbour_dists[j] < 45)
                own_nearby++;
        }
        if (own_nearby > 2) return nullptr;
    }
    if (SackValue(game, city) >= 400) {
        Sack(game, side, city);
        return nullptr;
    }
    game::Raze(game, side, city, sel ? sel->armies : std::vector<Army*>{});
    if (hooks.on_spoils) hooks.on_spoils(game, side, city, "razed");
    if (!sel || sel->armies.empty()) return nullptr;
    Flood flood = FloodFrom(game, side.Index(), sel->leader->X(), sel->leader->Y(), 15, sel);
    return BestCity(game, *sel, flood, true);
}

bool EarlyVengeance(Game& game, int side_index, City& city, int previous_owner) {
    Side& side = *game.map->Side(side_index);
    AIData& data = Data(game, side);
    if (side.IsComputerControlled() && previous_owner != kNone && previous_owner != kNeutral &&
        !IsComputer(game, previous_owner) && data.early != 0 && game.turn < 10 &&
        data.quest_city != city.Index()) {
        if (SackValue(game, city) < 200)
            Raze(game, side, city, false);
        else
            Sack(game, side, city);
        return true;
    }
    return false;
}

/**
 * @brief Rolls what the group will do to the cities it takes: raze, else
 *        sack, else pillage, each a per-mille chance plus the side's bonus.
 */
static void RollSpoils(Game& game, Side& side, Group& group) {
    AIData& data = Data(game, side);
    int bias = data.own * data.per_city;
    if (!side.IsComputerControlled())
        bias += data.bonus_human;
    else if (side.Level() == 2)
        bias += data.bonus_warlord;
    else if (side.Level() == 1)
        bias += data.bonus_lord;
    else if (side.Level() == 0)
        bias += data.bonus_knight;
    group.flags = 0;
    if (data.raze != 0 && game.rng.Dice(1, 1000, 0) < data.raze + bias) {
        group.flags = kGfRaze;
        return;
    }
    if (side.Gold() < 100) bias += data.poor;
    if (data.sack != 0 && game.rng.Dice(1, 1000, 0) < data.sack + bias) {
        group.flags = kGfSack;
        return;
    }
    if (data.pillage != 0 && game.rng.Dice(1, 1000, 0) < data.pillage + bias)
        group.flags = kGfPillage;
}

void Prepare(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int active = 0;
    for (int group_index = data.max_groups; group_index >= 1; group_index--)
        if (data.groups[group_index]->active != 0) active++;
    data.min_strength = 0;
    data.fly_cities = 0;
    data.strong_cities = 0;
    data.fast_cities = 0;
    // The strength of what each city builds well; the weakest of the best
    // eight sets the strength an army needs to count as strong.
    std::vector<int> strengths;
    for (City* city : Own(game, side)) {
        ClearCflag(data, *city, kCfMovE12);
        ClearCflag(data, *city, kCfFlygroup);
        ClearCflag(data, *city, kCfMovE16);
        if (Role(data, *city) != kRally) {
            auto [good, slot_index] = cities::BuildsWell(game, side, *city);
            if (good) {
                const Slot& slot = city->SlotAt(slot_index);
                strengths.push_back(slot.strength + (game.types.ById(slot.type)->siege ? 2 : 0));
            }
        }
    }
    LuaSort(strengths, [](int first, int second) { return first > second; });
    int least = 100;
    for (size_t i = 0; i < std::min<size_t>(8, strengths.size()); i++)
        if (strengths[i] < least) least = strengths[i];
    data.min_strength = std::min(4, least);

    auto strong = [&](const Slot& slot) {
        return game.types.ById(slot.type)->siege || data.min_strength <= slot.strength;
    };

    // Fliers for the fourth group, strong fast armies for the third, fast
    // armies for the second; four cities each are enough, and the marks on
    // the cities over four are taken off again.
    if (active == 4) {
        for (City* city : Own(game, side)) {
            if (Free(game, side, *city, true) && Cflag(data, *city, kCfFlier)) {
                bool builds = false;
                for (const auto& slot : city->Slots())
                    if (game.types.ById(slot.type)->flies && slot.strength > 4) builds = true;
                if (builds) {
                    data.fly_cities++;
                    SetCflag(data, *city, kCfFlygroup);
                }
            }
        }
    }
    bool fly_enough = data.fly_cities >= 4;
    int to_clear = fly_enough ? data.fly_cities - 4 : data.fly_cities;
    if (active > 2) {
        for (City* city : Own(game, side)) {
            if (Free(game, side, *city, true)) {
                bool skip = false;
                if (Cflag(data, *city, kCfFlygroup)) {
                    if (to_clear == 0)
                        skip = true;
                    else {
                        ClearCflag(data, *city, kCfFlygroup);
                        to_clear--;
                    }
                }
                if (!skip) {
                    bool builds = false;
                    for (const auto& slot : city->Slots())
                        if (strong(slot) && slot.move > 15) builds = true;
                    if (builds) {
                        data.strong_cities++;
                        SetCflag(data, *city, kCfMovE16);
                    }
                }
            }
        }
    }
    bool many_strong = data.strong_cities > 7;
    int fast_builders = 0;
    bool strong_enough = data.strong_cities >= 4;
    to_clear = strong_enough ? data.strong_cities - 4 : data.strong_cities;
    if (active > 1) {
        for (City* city : Own(game, side)) {
            if (Free(game, side, *city, true) && !Cflag(data, *city, kCfFlygroup)) {
                bool skip = false;
                if (Cflag(data, *city, kCfMovE16)) {
                    if (to_clear == 0)
                        skip = true;
                    else {
                        ClearCflag(data, *city, kCfMovE16);
                        to_clear--;
                    }
                }
                if (!skip) {
                    bool builds = false;
                    for (const auto& slot : city->Slots()) {
                        if (strong(slot) && slot.move > 11) {
                            fast_builders++;
                            if (!many_strong || slot.move > 15) builds = true;
                        }
                    }
                    if (builds) {
                        data.fast_cities++;
                        SetCflag(data, *city, kCfMovE12);
                    }
                }
            }
        }
    }
    bool fast_enough = data.fast_cities >= 4;
    to_clear = fast_enough ? data.fast_cities - 4 : data.fast_cities;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.OwnerIndex() == side.Index() && !Cflag(data, city, kCfFlygroup) &&
            !Cflag(data, city, kCfMovE16) && Cflag(data, city, kCfMovE12) && to_clear != 0) {
            ClearCflag(data, city, kCfMovE12);
            to_clear--;
        }
    }
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group_index <= 4) {
            group->size = 8;
            if (group_index == 1) {
                group->size = fast_builders < 8 ? 8 : 12;
            } else if (group_index == 2) {
                group->flags = Clear(group->flags, kGfMovE12);
                if (fast_enough) {
                    group->flags = Set(group->flags, kGfMovE12);
                    group->size = 12;
                }
            } else if (group_index == 3) {
                group->flags = Clear(group->flags, kGfMovE16);
                if (strong_enough) {
                    group->flags = Set(group->flags, kGfMovE16);
                    group->size = 16;
                }
            } else if (group_index == 4) {
                group->flags = Clear(group->flags, kGfFly);
                if (fly_enough) {
                    group->flags = Set(group->flags, kGfFly);
                    group->size = 12;
                }
            }
        }
    }
}

/** @brief Drops staged stacks that are gone or have left the group. */
static void CleanStaged(Game& game, Side& side, int group_index) {
    auto group = Data(game, side).groups[group_index];
    for (int slot = 1; slot <= 4; slot++) {
        auto entry = group->staged.find(slot);
        if (entry == group->staged.end()) continue;
        Army* army = entry->second;
        if (army && !(Alive(game, army) && army->OwnerIndex() == side.Index() &&
                      OrderOf(game, army).group == group_index))
            group->staged.erase(entry);
    }
}

/**
 * @brief Re-checks the plan: drops targets that changed hands or are the
 *        quest's, and adds the rally's neighbours of the target side.
 * @return The number of targets.
 */
static int CheckPlan(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    int targets = 0;
    for (int slot = 1; slot <= 6; slot++) {
        City* target = CityOrNull(game, group->cities, slot);
        if (target) {
            if (data.quest_city == target->Index() || Owner(*target) != group->target)
                group->cities.erase(slot);
            else
                targets++;
        }
    }
    City* rally = CityOrNull(game, group->rally);
    if (!rally) return targets;
    auto [neighbours, neighbour_dists] = FindNeighbours(game, *rally);
    for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
        City* neighbour = neighbours[j];
        if (Owner(*neighbour) == group->target && !Cflag(data, *neighbour, kCfUnseen)) {
            bool have = false;
            for (int slot = 1; slot <= 6; slot++)
                if (HasKey(group->cities, slot) && group->cities[slot] == neighbour->Index())
                    have = true;
            if (!have) {
                targets++;
                // A free slot, else the one of the farthest target when this
                // one is nearer.
                int chosen = kNone;
                bool worst_set = false;
                int worst = 0;
                for (int slot = 6; slot >= 1; slot--) {
                    if (!HasKey(group->cities, slot)) {
                        chosen = slot;
                        worst = -1;
                        worst_set = true;
                        break;
                    }
                    if ((worst_set ? worst : 0) < Get(group->dist, slot)) {
                        chosen = slot;
                        worst = Get(group->dist, slot);
                        worst_set = true;
                    }
                }
                if (chosen != kNone && (worst == -1 || neighbour_dists[j] < worst)) {
                    group->cities[chosen] = neighbour->Index();
                    group->dist[chosen] = neighbour_dists[j];
                }
            }
        }
    }
    return targets;
}

/**
 * @brief With one target left, adds its neighbours of the target side, and
 *        moves the rally to one of the side's own cities next to it.
 */
static void AdjustRally(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    int only_target = kNone, count = 0;
    for (int slot = 1; slot <= 6; slot++)
        if (HasKey(group->cities, slot)) {
            only_target = group->cities[slot];
            count++;
        }
    if (count != 1) return;
    City* target = CityOrNull(game, only_target);
    if (!target) return;
    auto neighbours = FindNeighbours(game, *target).first;
    City* next = nullptr;
    bool rally_near = false;
    for (size_t j = 0; j < neighbours.size(); j++) {
        City* neighbour = neighbours[j];
        if (!Cflag(data, *neighbour, kCfUnseen)) {
            if (Owner(*neighbour) == group->target) {
                for (int slot = 1; slot <= 6; slot++) {
                    if (!HasKey(group->cities, slot)) {
                        group->cities[slot] = neighbour->Index();
                        break;
                    }
                }
            } else if (neighbour->OwnerIndex() == side.Index()) {
                if (neighbour->Index() == group->rally) rally_near = true;
                if (!IsRally(game, side, *neighbour) && !next) next = neighbour;
            }
        }
    }
    if (!rally_near && next) group->rally = next->Index();
}

/**
 * @brief Moves the rally to the side's nearest neighbouring city that is no
 *        other group's rally; cancels the group when there is none.
 * @return Whether the group goes on.
 */
static bool Relocate(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    City* rally = CityOrNull(game, group->rally);
    City* best = nullptr;
    if (rally) {
        auto neighbours = FindNeighbours(game, *rally).first;
        int best_distance = 1000;
        for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
            City* neighbour = neighbours[j];
            if (neighbour->OwnerIndex() == side.Index() && !IsRally(game, side, *neighbour)) {
                int distance = Dist(neighbour->X(), neighbour->Y(), rally->X(), rally->Y());
                if (distance < best_distance) {
                    best = neighbour;
                    best_distance = distance;
                }
            }
        }
    }
    if (!best) {
        Cancel(game, side, group_index);
        return false;
    }
    group->rally = best->Index();
    return true;
}

/**
 * @brief Deals with a city the group has taken: garrisons it, strikes it off
 *        the targets, and makes it the rally when it is nearer the enemy.
 * @return Whether the group goes on.
 */
static bool TakeCity(Game& game, Side& side, int group_index, City& city) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    if (group->rally == city.Index()) return false;
    cities::Garrison(game, side, city, true);
    for (int slot = 1; slot <= 6; slot++) {
        if (HasKey(group->cities, slot) && group->cities[slot] == city.Index()) {
            group->cities.erase(slot);
            break;
        }
    }
    // How close a city is to the target side's cities, nearer counting more.
    auto near = [&](const City& place) {
        auto [neighbours, neighbour_dists] = FindNeighbours(game, place);
        int closeness = 0;
        for (size_t j = 0; j < neighbours.size(); j++) {
            if (Owner(*neighbours[j]) == group->target) {
                if (neighbour_dists[j] < 10) closeness++;
                if (neighbour_dists[j] < 20) closeness++;
                if (neighbour_dists[j] < 30) closeness++;
                if (neighbour_dists[j] < 50) closeness++;
            }
        }
        return closeness;
    };
    int here = near(city);
    City* rally = CityOrNull(game, group->rally);
    int there = rally ? near(*rally) : 0;
    City* pick = there < here ? &city : rally;
    if (pick) {
        bool have = false;
        for (int slot = 1; slot <= 6; slot++)
            if (HasKey(group->taken, slot) && group->taken[slot] == pick->Index()) have = true;
        if (!have) {
            for (int slot = 1; slot <= 6; slot++) {
                if (!HasKey(group->taken, slot)) {
                    group->taken[slot] = pick->Index();
                    break;
                }
            }
        }
    }
    if (here == 0 && there == 0) {
        Cancel(game, side, group_index);
        return false;
    }
    group->rally = pick->Index();
    SetRole(data, city, kRally);
    FollowUp(game, side, group_index);
    return true;
}

int March(Game& game, Side& side, int group_index, std::vector<Army*> list, City* city) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    Army* lead = list.empty() ? nullptr : list[0];
    // A razed target gives way to the nearest standing one.
    if (lead && Alive(game, lead) && !Standing(*city)) {
        City* best = nullptr;
        int best_distance = 1000;
        for (int slot = 1; slot <= 6; slot++) {
            City* target = CityOrNull(game, group->cities, slot);
            if (target) {
                int distance = Dist(lead->X(), lead->Y(), target->X(), target->Y());
                if (distance < best_distance && Standing(*target)) {
                    best = target;
                    best_distance = distance;
                }
            }
        }
        if (best) city = best;
    }
    for (;;) {
        int targets = 0;
        for (int slot = 1; slot <= 6; slot++)
            if (HasKey(group->cities, slot)) targets++;
        std::vector<Army*> keep;
        for (Army* army : list)
            if (Alive(game, army) && army->OwnerIndex() == side.Index()) keep.push_back(army);
        list = keep;
        int previous_owner = Owner(*city);
        auto sel = Order(game, list, kOrderCity, city->Index(), 0);
        if (!sel) {
            CleanStaged(game, side, group_index);
            return 3;
        }
        int code = MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
        if (code == 1) {
            game::Disband(game, side, sel->armies);
            CleanStaged(game, side, group_index);
            return 3;
        }
        CleanStaged(game, side, group_index);
        if (city->OwnerIndex() != side.Index()) return 3;
        if (previous_owner == side.Index()) return 3;
        // Taken. Raze needs two targets left and never hits the capital.
        bool capital = side.CapitalCity(*game.map) == city;
        int flags = group->flags;
        if (!capital && targets >= 2 && Has(flags, kGfRaze)) {
            City* next_city = Raze(game, side, *city, false, &*sel);
            if (!next_city) return 1;
            city = next_city;
            list = sel->armies;
        } else {
            if (!capital && Has(flags, kGfSack))
                Sack(game, side, *city);
            else if (!capital && Has(flags, kGfPillage))
                Pillage(game, side, *city);
            TakeCity(game, side, group_index, *city);
            return 1;
        }
    }
}

/**
 * @brief Looks for a stack of the target side worth hitting near a staged
 *        stack, and goes for it.
 * @return 2 if it went, else 0.
 */
static int HitArmies(Game& game, Side& side, int group_index, std::vector<Army*>& list, int target,
                     int range, const Flood& flood, const Sel& sel) {
    (void)group_index;
    Army* lead = list.empty() ? nullptr : list[0];
    if (!lead || lead->IsAtSea()) return 0;
    int best_x = kNone, best_y = kNone, best_odds = 0;
    for (int x = lead->X() - range; x <= lead->X() + range - 1; x++) {
        for (int y = lead->Y() - range; y <= lead->Y() + range - 1; y++) {
            if (x >= 0 && y >= 0 && x < game.map->width && y < game.map->height) {
                auto here = game::ArmiesAt(game, x, y);
                int terrain = Terrain(game, x, y);
                if (!here.empty() && here[0]->OwnerIndex() == target && terrain != move::kCity &&
                    terrain != move::kShore && terrain != move::kWater &&
                    FloodAt(flood, x, y) <= sel.min_moves - 1 &&
                    game::Seen(game, side.Index(), x, y)) {
                    int count = static_cast<int>(here.size());
                    int strength = 0, heroes = 0;
                    for (Army* army : here) {
                        strength += army->Strength();
                        if (IsHero(army)) heroes++;
                    }
                    int odds = Odds(game, sel, x, y);
                    if (odds > 75 && ((count > 2 && strength > 10) || heroes != 0) &&
                        best_odds < odds) {
                        best_x = x;
                        best_y = y;
                        best_odds = odds;
                    }
                }
            }
        }
    }
    if (best_x == kNone) return 0;
    const ClassicArmyOrder& lead_ord = OrderOf(game, lead);
    int dest = lead_ord.order == kOrderCity ? lead_ord.dest : kNone;
    auto stack = Order(game, list, kOrderRoam, 0, 0x20);
    if (stack) {
        stack->leader->SetTarget(TilePos(best_x, best_y));
        MoveTo(game, *stack, best_x, best_y);
        // Back to the city it was going for.
        for (Army* army : list) {
            if (Alive(game, army) && army->OwnerIndex() == side.Index() && dest != kNone) {
                ClassicArmyOrder& ord = MutableOrderOf(game, army);
                ord.order = kOrderCity;
                ord.dest = dest;
            }
        }
    }
    return 2;
}

/**
 * @brief A staged stack's step: hit a stack of the target side nearby, and
 *        after that march on the nearest target city it can take.
 * @return HitArmies' or March's code.
 */
static int StagedStep(Game& game, Side& side, int group_index, std::vector<Army*>& list, int target,
                      const Flood& flood, const Sel& sel) {
    City* nearest = nullptr;
    int nearest_distance = 1000;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.OwnerIndex() == target) {
            int distance = std::get<0>(CityDistance(game, flood, city));
            if (distance < 50 && distance < sel.min_moves) {
                int odds = Odds(game, sel, city.X(), city.Y());
                if (odds > 75 && distance < nearest_distance) {
                    nearest = &city;
                    nearest_distance = distance;
                }
            }
        }
    }
    int result = HitArmies(game, side, group_index, list, target, 15, flood, sel);
    if (result != 0 && nearest) result = March(game, side, group_index, list, nearest);
    return result;
}

/** @brief Moves the group's staged stacks on. */
static void Gather(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    for (int slot = 4; slot >= 1; slot--) {
        bool again = true;
        while (again) {
            again = false;
            CleanStaged(game, side, group_index);
            auto entry = group->staged.find(slot);
            Army* lead = entry == group->staged.end() ? nullptr : entry->second;
            if (!lead) break;
            const ClassicArmyOrder& lead_ord = OrderOf(game, lead);
            auto list = CollectOrdered(game, side.Index(), lead->X(), lead->Y(), lead_ord.group,
                                       lead_ord.order, 0);
            if (list.empty()) break;
            Sel sel = Select(game, list);
            Flood flood = FloodFrom(game, side.Index(), lead->X(), lead->Y(), 15, &sel);
            std::vector<std::array<int, 3>> before;
            for (Army* army : list) before.push_back({army->X(), army->Y(), army->Moves()});
            size_t army_count = game.armies.size();
            int result = StagedStep(game, side, group_index, list, group->target, flood, sel);
            if (result == 0) {
                Army* last = list.empty() ? nullptr : list.back();
                if (last && Alive(game, last)) {
                    const ClassicArmyOrder& last_ord = OrderOf(game, last);
                    if (last_ord.order == kOrderCity && CityOrNull(game, last_ord.dest)) {
                        March(game, side, group_index, list, CityOrNull(game, last_ord.dest));
                    }
                }
            } else if (result == 2) {
                // The original goes round again for as long as a stack is found to
                // strike. One that cannot take a step would be found every time --
                // until the odds' dice fall short, which for a strong stack is
                // never -- so a pass that changed nothing ends it here.
                again = game.armies.size() != army_count;
                for (size_t i = 0; i < list.size() && i < before.size(); i++) {
                    Army* army = list[i];
                    if (army->X() != before[i][0] || army->Y() != before[i][1] ||
                        army->Moves() != before[i][2])
                        again = true;
                }
            }
        }
    }
}

/**
 * @brief Strikes from the rally city at the best target, staging the
 *        stack's lead.
 * @return Whether a stack went.
 */
static bool Strike(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    City* rally = CityOrNull(game, group->rally);
    if (!rally) return false;
    auto list = Collect(game, side.Index(), rally->X() + 1, rally->Y(), 0);
    int count = static_cast<int>(list.size());
    if (count == 0) return false;
    Sel sel = Select(game, list);
    City* best = nullptr;
    int best_score = -1;
    for (int slot = 1; slot <= 6; slot++) {
        City* target = CityOrNull(game, group->cities, slot);
        if (target) {
            int distance = HasKey(group->dist, slot) ? group->dist[slot] : -1;
            int turns = distance / std::max(1, sel.min_moves - 2) +
                        1;  // truncating, as the original divides
            int odds = Odds(game, sel, target->X(), target->Y());
            int score = (turns < 11 ? 10 - turns : 0) + odds + Get(group->bonus, slot) +
                        game.rng.Dice(1, 4, 0);
            if (turns == 1) score += 100;
            if ((odds > 75 || count > 7) && best_score < score) {
                best = target;
                best_score = score;
            }
        }
    }
    if (!best) return false;
    // The stack's lead -- a hero, else the strongest -- is staged.
    int free_slot = kNone;
    for (int slot = 4; slot >= 1; slot--) {
        auto entry = group->staged.find(slot);
        if (entry == group->staged.end() || !entry->second) {
            free_slot = slot;
            break;
        }
    }
    if (free_slot != kNone) {
        Army* pick = nullptr;
        int strongest = -1;
        for (int i = static_cast<int>(list.size()) - 1; i >= 0; i--) {
            Army* army = list[i];
            if (IsHero(army)) {
                pick = army;
                break;
            }
            if (strongest < army->Strength()) {
                pick = army;
                strongest = army->Strength();
            }
        }
        group->staged[free_slot] = pick;
    }
    for (Army* army : list) MutableOrderOf(game, army).group = group_index;
    March(game, side, group_index, list, best);
    return true;
}

void FollowUp(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    for (int slot = 1; slot <= 6; slot++) {
        City* taken = CityOrNull(game, group->taken, slot);
        if (taken && taken->Index() != group->rally && taken->OwnerIndex() == side.Index()) {
            auto [neighbours, neighbour_dists] = FindNeighbours(game, *taken);
            int threats = 0;
            for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
                int owner = neighbours[j]->OwnerIndex();
                if (owner != kNone && owner != side.Index() && neighbour_dists[j] < 25) threats++;
            }
            if (threats == 0) {
                cities::Garrison(game, side, *taken, true);
                auto list = CollectOrdered(game, side.Index(), taken->X() + 1, taken->Y(), 0,
                                           group->size, 0);
                if (list.size() > 3) {
                    auto sel = Order(game, list, kOrderCity, group->rally, 0);
                    if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
                }
            }
        }
    }
}

/** @brief Cancels the longest-running group. */
static void CancelOldest(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int oldest = kNone, age = -1;
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && age < group->active) {
            oldest = group_index;
            age = group->active;
        }
    }
    if (oldest != kNone) Cancel(game, side, oldest);
}

/**
 * @brief A group's turn.
 *
 * A lost capital takes precedence: with no group going for its taker, the
 * oldest group is cancelled.
 *
 * @return Whether the group is still active afterwards.
 */
static bool RunGroup(Game& game, Side& side, int group_index) {
    AIData& data = Data(game, side);
    auto group = data.groups[group_index];
    City* capital = side.CapitalCity(*game.map);
    if (capital && !capital->IsNeutral() && capital->OwnerIndex() != side.Index()) {
        bool going = false;
        for (int other_index = data.max_groups; other_index >= 1; other_index--) {
            auto other = data.groups[other_index];
            if (other->active != 0 && other->target == capital->OwnerIndex()) going = true;
        }
        if (!going) {
            CancelOldest(game, side);
            return false;
        }
    }
    if (CheckPlan(game, side, group_index) == 0) {
        Cancel(game, side, group_index);
        return false;
    }
    AdjustRally(game, side, group_index);
    bool shared = false;
    for (int other_index = data.max_groups; other_index >= 1; other_index--) {
        auto other = data.groups[other_index];
        if (other->active != 0 && other_index != group_index && other->rally == group->rally)
            shared = true;
    }
    City* rally = CityOrNull(game, group->rally);
    if ((!shared && rally && rally->OwnerIndex() == side.Index()) ||
        Relocate(game, side, group_index)) {
        Gather(game, side, group_index);
        if (data.groups[group_index]->active != 0) {
            City* current_rally = CityOrNull(game, data.groups[group_index]->rally);
            if (current_rally) cities::Garrison(game, side, *current_rally, true);
            if (Strike(game, side, group_index)) {
                current_rally = CityOrNull(game, data.groups[group_index]->rally);
                if (current_rally) cities::Garrison(game, side, *current_rally, true);
            }
            if (data.groups[group_index]->active != 0) {
                FollowUp(game, side, group_index);
                return true;
            }
        }
    }
    return false;
}

void Assault(Game& game, Side& side) {
    AIData& data = Data(game, side);
    if (data.turns == 0) return;
    Prepare(game, side);
    for (City* city : Own(game, side)) {
        if (Role(data, *city) == kRally) SetRole(data, *city, kStop);
    }
    for (int group_index = 1; group_index <= data.max_groups; group_index++) {
        auto group = data.groups[group_index];
        if (group->active != 0) {
            City* rally = CityOrNull(game, group->rally);
            if (rally) SetRole(data, *rally, kRally);
            if (RunGroup(game, side, group_index)) data.groups[group_index]->active++;
        }
    }
}

/** @brief The enemy city with most of its own side's cities round it; ties go by a coin. */
static City* Hub(Game& game, int enemy) {
    City* best = nullptr;
    int most = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.OwnerIndex() == enemy) {
            int friends = 1;
            auto neighbours = FindNeighbours(game, city).first;
            for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--)
                if (neighbours[j]->OwnerIndex() == enemy) friends++;
            if (friends > most || (friends == most && game.rng.Dice(1, 2, -1) != 0)) {
                best = &city;
                most = friends;
            }
        }
    }
    return best;
}

/**
 * @brief Distances over the neighbour graph from a city.
 * @param reached Set for every city the graph reaches.
 * @param dist The summed neighbour distances to each reached city.
 */
static void Spread(Game& game, const City& from, std::vector<char>& reached,
                   std::vector<int>& dist) {
    size_t city_count = game.map->cities.size();
    reached.assign(city_count, 0);
    dist.assign(city_count, 0);
    std::vector<char> done(city_count, 0);
    reached[from.Index()] = 1;
    dist[from.Index()] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = static_cast<int>(city_count) - 1; i >= 0; i--) {
            City& city = game.map->cities[i];
            if (reached[city.Index()] && !done[city.Index()]) {
                changed = true;
                done[city.Index()] = 1;
                auto [neighbours, neighbour_dists] = FindNeighbours(game, city);
                for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
                    City* neighbour = neighbours[j];
                    int via = dist[city.Index()] + neighbour_dists[j];
                    if (!reached[neighbour->Index()]) {
                        reached[neighbour->Index()] = 1;
                        dist[neighbour->Index()] = via;
                    } else if (via < dist[neighbour->Index()]) {
                        dist[neighbour->Index()] = via;
                    }
                }
            }
        }
    }
}

/**
 * @brief Plans a group against a side: the rally is the own city nearest
 *        the enemy's hub, the targets the six enemy cities nearest the rally.
 * @return The rally city, or null.
 */
static City* Plan(Game& game, Side& side, Group& group, int enemy) {
    AIData& data = Data(game, side);
    group.cities.clear();
    group.dist.clear();
    City* hub = Hub(game, enemy);
    if (!hub) return nullptr;
    std::vector<char> reached;
    std::vector<int> dist;
    Spread(game, *hub, reached, dist);
    City* rally = nullptr;
    long best = 1000000000;
    for (City* city : Own(game, side)) {
        if (kRallyRoles.count(Role(data, *city)) && reached[city->Index()] &&
            dist[city->Index()] < best) {
            rally = city;
            best = dist[city->Index()];
        }
    }
    if (!rally) {
        // Not connected by land: the nearest as the crow flies, with the hub
        // as the only target.
        if (!Cflag(data, *hub, kCfUnseen)) {
            long best_distance = 1000000000;
            for (City* city : Own(game, side)) {
                if (kRallyRoles.count(Role(data, *city))) {
                    int distance = core::Dist(city->X(), city->Y(), hub->X(), hub->Y());
                    if (distance < best_distance) {
                        rally = city;
                        best_distance = distance;
                    }
                }
            }
            if (rally) {
                group.cities[1] = hub->Index();
                group.dist[1] = 100;
            }
        }
        return rally;
    }
    Spread(game, *rally, reached, dist);
    int targets = 0;
    for (int slot = 1; slot <= 6; slot++) {
        City* pick = nullptr;
        long pick_distance = 1000000000;
        for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
            City& city = game.map->cities[i];
            if (city.OwnerIndex() == enemy && !Cflag(data, city, kCfUnseen) &&
                reached[city.Index()] && dist[city.Index()] < pick_distance) {
                pick = &city;
                pick_distance = dist[city.Index()];
            }
        }
        if (!pick) break;
        group.cities[slot] = pick->Index();
        group.dist[slot] = static_cast<int>(pick_distance);
        reached[pick->Index()] = 0;
        targets++;
    }
    if (targets == 0 && !Cflag(data, *hub, kCfUnseen)) {
        group.cities[1] = hub->Index();
        group.dist[1] = 100;
        targets = 1;
    }
    if (targets == 0) return nullptr;
    return rally;
}

int PickEnemy(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int side_index = side.Index();
    City* capital = side.CapitalCity(*game.map);
    int capital_owner = capital ? Owner(*capital) : kNeutral;
    // Per side: cities seen, score, cities owned, whether we hold its
    // capital, cities we took from it, groups aimed at it, and a die.
    int held[8], score[8], count[8], cap_held[8], from_them[8], targeted[8], roll[8];
    for (int other = 0; other < 8; other++) {
        held[other] = 0;
        score[other] = 0;
        count[other] = 0;
        cap_held[other] = 0;
        from_them[other] = 0;
        targeted[other] = 0;
        roll[other] = game.rng.Dice(1, 10, 0);
    }
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        int owner = city.OwnerIndex();
        if (owner != kNone) {
            count[owner]++;
            if (!Cflag(data, city, kCfUnseen)) held[owner]++;
            int claim = city.Claim().Value();
            if (owner == side_index && claim >= 0 && claim < 8) from_them[claim]++;
        }
    }
    int n_groups = 0;
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group->target != kNone) {
            if (group->target >= 0 && group->target < 8) targeted[group->target]++;
            n_groups++;
        }
    }
    if (capital_owner != kNeutral && capital_owner != side_index) score[capital_owner] += 20;
    int others = 0;
    for (int other = 7; other >= 0; other--) {
        if (count[other] != 0) {
            if (other != side_index) others++;
            City* theirs = game.map->sides[other].CapitalCity(*game.map);
            if (theirs && theirs->OwnerIndex() == side_index) cap_held[other] = 1;
        }
    }
    int constant;
    if (!side.IsComputerControlled())
        constant = data.die_human;
    else if (side.Level() == 2)
        constant = data.die_warlord;
    else if (side.Level() == 1)
        constant = data.die_lord;
    else
        constant = data.die_knight;
    for (int other = 7; other >= 0; other--) {
        const Side& other_side = game.map->sides[other];
        score[other] += roll[other] + cap_held[other] * 15 + from_them[other] * 4 +
                        data.heroes_killed[other] * 4 + data.armies_killed[other] +
                        data.battles[other] + data.lost[other] * 2 + data.city_battles[other] * 2 +
                        data.cities_lost[other] * 2 + constant;
        score[other] += std::abs(side.DiploScore() - other_side.DiploScore()) / 8;
        score[other] += std::abs(count[side_index] - count[other]) / 4;
    }
    bool proposing_war = false;
    for (int other = 7; other >= 0; other--) {
        if (InPlay(game, other) && other != side_index && Proposal(game, side_index, other) == 2)
            proposing_war = true;
    }
    if (game.greatest && side.IsComputerControlled()) {
        auto fights = FightingHumans(game);
        for (int other = 7; other >= 0; other--) {
            if (InPlay(game, other) && IsComputer(game, other) &&
                (fights[side_index] || fights[other]))
                score[other] = 0;
        }
    }
    int early = game.map->options.quick_start != 0 ? 4 : 8;
    for (int other = 7; other >= 0; other--) {
        int fought = data.battles[other] + data.city_battles[other];
        if (diplo::Spared(game, side, other)) score[other] = 0;
        if (proposing_war && Proposal(game, side_index, other) == 0) score[other] = 0;
        if (n_groups == 0 && held[other] == 0) score[other] = 0;
        if (game.turn < early && fought == 0) score[other] = 0;
        if (others > 1 && count[other] / 4 < targeted[other]) score[other] = 0;
    }
    score[side_index] = 0;
    int pick = kNone, top = 0;
    for (int other = 7; other >= 0; other--) {
        if (count[other] != 0 && top < score[other]) {
            pick = other;
            top = score[other];
        }
    }
    if (capital_owner != kNeutral && capital_owner != side_index) {
        // Matches the reference: the original compares the capital's holder
        // with the groups-per-side counts, not with the groups' targets.
        bool found = false;
        for (int group_index = data.max_groups - 1; group_index >= 0; group_index--)
            if (group_index < 8 && capital_owner == targeted[group_index]) found = true;
        if (!found) pick = capital_owner;
    }
    if (pick != kNone && held[pick] == 0) pick = kNone;
    return pick;
}

std::array<bool, 8> FightingHumans(const Game& game) {
    std::array<bool, 8> out{};
    for (int computer = 0; computer < 8; computer++) {
        if (InPlay(game, computer) && IsComputer(game, computer)) {
            for (int human = 0; human < 8; human++) {
                if (InPlay(game, human) && !IsComputer(game, human) &&
                    State(game, computer, human) == 2)
                    out[computer] = true;
            }
        }
    }
    return out;
}

void AssaultXx(Game& game, Side& side) {
    AIData& data = Data(game, side);
    if (data.turns == 0) return;
    int quiet = 0;
    for (City* city : Own(game, side)) {
        int role = Role(data, *city);
        if (role == kBuilding || role == kStop) quiet++;
    }
    int active = 0;
    for (int group_index = 1; group_index <= kMaxGroups; group_index++)
        if (data.groups[group_index]->active != 0) active++;
    if (!(active < data.max_groups && quiet != 0 && (active == 0 || quiet > 2))) return;
    int slot = kNone;
    for (int group_index = data.max_groups; group_index >= 1; group_index--)
        if (data.groups[group_index]->active == 0) slot = group_index;
    if (slot == kNone) return;
    data.groups[slot] = EmptyGroup();
    int enemy = PickEnemy(game, side);
    if (enemy == kNone) return;
    auto group = data.groups[slot];
    City* rally = Plan(game, side, *group, enemy);
    if (!rally) {
        data.groups[slot] = EmptyGroup();
        return;
    }
    group->active = 1;
    group->target = enemy;
    group->rally = rally->Index();
    SetRole(data, *rally, kRally);
    RollSpoils(game, side, *group);
    Propose(game, side.Index(), enemy, 2);
    // The rally city is no group's member any more.
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        auto other = data.groups[group_index];
        if (other->active != 0) {
            for (int member = 1; member <= 4; member++) {
                if (HasKey(other->members, member) && other->members[member] == rally->Index()) {
                    other->members.erase(member);
                    rally->ClearVector();
                }
            }
        }
    }
    // Each target's bonus: its own side's cities round it.
    for (int target_slot = 1; target_slot <= 6; target_slot++) {
        City* target = CityOrNull(game, group->cities, target_slot);
        if (target) {
            int friends = 0;
            for (City* neighbour : FindNeighbours(game, *target).first)
                if (neighbour->OwnerIndex() == enemy) friends++;
            group->bonus[target_slot] = friends;
        }
    }
    Prepare(game, side);
}

}  // namespace w2::ai::groups
