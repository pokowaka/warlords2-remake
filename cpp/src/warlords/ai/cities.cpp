#include "warlords/ai/cities.hpp"

#include <algorithm>
#include <set>

#include "util/util.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"

namespace w2::ai::cities {

using namespace core;

/// The garrison kept on a city's first tile, by how many armies the city
/// holds; 8 from thirteen armies up.
static const int kKeep[13] = {0, 0, 0, 0, 3, 3, 4, 5, 5, 6, 7, 7, 7};

/// The city's four tiles, as offsets from its corner, in the order the
/// garrison fills them: (x+1, y) for the keepers first.
static const int kTileDx[4] = {1, 0, 0, 1};
static const int kTileDy[4] = {0, 0, 1, 1};  ///< See kTileDx.

/// The city roles that make a city try a quick attack.
static const std::set<int> kQuickRoles = {5, 8, 6, 4, 14, 7};

std::vector<City*> Own(const Game& game, const Side& side) {
    std::vector<City*> owned;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == side.index) owned.push_back(&city);
    }
    return owned;
}

std::vector<NeutralNeighbour> NeutralNeighbours(Game& game, Side& side, const City& city) {
    AIData& data = Data(game, side);
    auto [neighbours, neighbour_dists] = FindNeighbours(game, city);
    std::vector<NeutralNeighbour> neutrals;
    for (size_t j = 0; j < neighbours.size(); j++) {
        City* neighbour = neighbours[j];
        if (neighbour->index != data.quest_city && Standing(*neighbour) &&
            neighbour->owner_index == kNone &&
            (neutrals.empty() || j < 2 || neighbour_dists[j] < 20)) {
            neutrals.push_back(
                    NeutralNeighbour{neighbour, neighbour_dists[j], static_cast<int>(j) + 1});
        }
    }
    return neutrals;
}

int NearestArmy(const Game& game, const Side& side, int x, int y, int of_side) {
    int best = 1000;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->owner != kNone && !army->transit) {
            bool counts = of_side == kNone ? army->owner != side.index : army->owner == of_side;
            if (counts && Explored(game, side.index, army->x, army->y)) {
                int distance = Dist(x, y, army->x, army->y);
                if (distance < best) best = distance;
            }
        }
    }
    return best;
}

/** @brief A production slot's strength for the AI: siege types count 2 more. */
static int SlotStrength(const Game& game, const Slot& slot) {
    int strength = slot.strength;
    if (game.types.ById(slot.type)->siege) strength += 2;
    return strength;
}

std::pair<bool, int> BuildsWell(Game& game, Side& side, const City& city) {
    AIData& data = Data(game, side);
    int slot = rules::BestSlot(city.slots, 3, game.types, side.enhanced);
    if (slot == kNone) return {false, kNone};
    int strength = SlotStrength(game, city.slots[slot]);
    if ((data.own < 12 || strength > 2) && (data.own < 8 || strength > 1)) return {true, slot};
    return {false, slot};
}

/**
 * @brief The production slot to buy a new type over, from 0.
 *
 * The first empty slot, else the weakest one under strength 10.
 * @return The slot, or kNone when all four hold strong types.
 */
static int SlotToReplace(const Game& game, const City& city) {
    if (static_cast<int>(city.slots.size()) < rules::kProductionSlots)
        return static_cast<int>(city.slots.size());
    int best = kNone, least = 10;
    for (size_t i = 0; i < city.slots.size(); i++) {
        int strength = SlotStrength(game, city.slots[i]);
        if (strength < least) {
            best = static_cast<int>(i);
            least = strength;
        }
    }
    return best;
}

bool Building(const City& city) {
    if (city.producing == kNone) return false;
    if (city.producing >= static_cast<int>(city.slots.size())) return false;
    const Slot& slot = city.slots[city.producing];
    return city.countdown != 0 && city.countdown != slot.time;
}

/**
 * @brief Sets a city's production to a slot, if the side can afford it.
 *
 * Refused after turn 5 when the side has less gold than the type's cost
 * plus 30.
 * @return Whether the production was set.
 */
static bool SetProduction(Game& game, Side& side, City& city, int slot) {
    if (slot < 0 || slot >= static_cast<int>(city.slots.size())) return false;
    if (side.gold < city.slots[slot].cost + 30 && game.turn > 5) return false;
    game::SetProduction(game, city, slot);
    return true;
}

/** @brief Has a city build the best type it has for a production purpose. */
static void ProduceFor(Game& game, Side& side, City& city, int purpose) {
    int slot = rules::BestSlot(city.slots, purpose, game.types, side.enhanced);
    if (slot != kNone) SetProduction(game, side, city, slot);
}

void Vector(Game& game, City& city, const City* dest) {
    if (!Building(city))
        city.vector_to = kNone;
    else
        city.vector_to = dest ? dest->index : kNone;
}

/**
 * @brief Buys a production type into a city, if the side has 30 gold over its price.
 *
 * The city starts afresh: no production, no vector, and role kBuilding.
 * @return Whether the type was bought.
 */
static bool BuyType(Game& game, Side& side, City& city, int type_id) {
    const ArmyType* type = game.types.ById(type_id);
    if (!type) return false;
    if (!(side.gold > type->price + 30)) return false;
    int slot = SlotToReplace(game, city);
    if (slot == kNone) return false;
    game::BuyProduction(game, side, city, slot, type_id);
    game::SortProduction(city);
    AIData& data = Data(game, side);
    data.bought++;
    city.producing = kNone;
    city.countdown = 0;
    city.vector_to = kNone;
    SetRole(data, city, kBuilding);
    return true;
}

/** @brief Buys the first flying type the side can afford into a city. */
static void BuyFlier(Game& game, Side& side, City& city) {
    int slot = SlotToReplace(game, city);
    if (slot == kNone) return;
    AIData& data = Data(game, side);
    for (int type_id = 0; type_id <= 28; type_id++) {
        const ArmyType* type = game.types.ById(type_id);
        if (type && type->flies && !type->IsMagical() && type->price + 30 <= side.gold) {
            game::BuyProduction(game, side, city, slot, type_id);
            game::SortProduction(city);
            SetCflag(data, city, kCfFlier);
            city.producing = kNone;
            city.countdown = 0;
            city.vector_to = kNone;
            return;
        }
    }
}

/** @brief Flags every standing city that can build a flying type. */
static void MarkFliers(Game& game, AIData& data) {
    for (auto& city : game.map->cities) {
        if (Standing(city)) {
            ClearCflag(data, city, kCfFlier);
            for (auto& slot : city.slots) {
                if (game.types.ById(slot.type)->flies) SetCflag(data, city, kCfFlier);
            }
        }
    }
}

void ClearUnseen(Game& game, Side& side) {
    if (game.map->options.hidden_map == 0) return;
    AIData& data = Data(game, side);
    for (auto& city : game.map->cities) {
        if (Cflag(data, city, kCfUnseen)) {
            bool seen = false;
            for (int x = city.x - 1; x <= city.x + 2; x++) {
                for (int y = city.y - 1; y <= city.y + 2; y++) {
                    if (x >= 0 && y >= 0 && x < game.map->width && y < game.map->height &&
                        game::Seen(game, side.index, x, y))
                        seen = true;
                }
            }
            if (seen) ClearCflag(data, city, kCfUnseen);
        }
    }
}

void Evaluate(Game& game, Side& side, int side_index) {
    AIData& data = Data(game, side);
    MarkFliers(game, data);
    ClearUnseen(game, side);
    data.own = 0;
    data.enemy = 0;
    data.neutral = 0;
    data.unseen = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (Standing(city)) {
            if (city.owner_index == side_index) {
                data.own++;
                data.held[city.index] = Get(data.held, city.index) + 1;
                if (Role(data, city) == 0) SetRole(data, city, kJustTaken);
            } else {
                if (Cflag(data, city, kCfUnseen)) data.unseen++;
                SetRole(data, city, 0);
                data.held[city.index] = 0;
                if (city.owner_index == kNone)
                    data.neutral++;
                else
                    data.enemy++;
            }
        } else {
            SetRole(data, city, 0);
            data.held[city.index] = 0;
        }
    }
    if (data.own != 0) {
        for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
            City& city = game.map->cities[i];
            if (Role(data, city) == kStop && data.unseen != 0 && data.explorers < 5 &&
                Cflag(data, city, kCfFlier)) {
                SetRole(data, city, kExploreR2);
            }
            if (Role(data, city) == kJustTaken) {
                SetRole(data, city,
                        !NeutralNeighbours(game, side, city).empty() ? kNearNeutral : kBuilding);
            }
        }
        data.turns++;
    }
}

int Wanted(Game& game, const Side& side, const City& city) {
    auto neighbours = FindNeighbours(game, city).first;
    int foreign = 0, war = 0;
    for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
        City* neighbour = neighbours[j];
        if (neighbour->owner_index != kNone && neighbour->owner_index != side.index &&
            Standing(*neighbour)) {
            foreign++;
            if (State(game, side.index, neighbour->owner_index) == 2) war++;
        }
    }
    if (war != 0) return 8;
    if (foreign >= 2) return 4;
    if (foreign == 1) return 3;
    return 2;
}

/**
 * @brief Puts a city's armies in the order they stand in, keepers first.
 *
 * The keepers stand on the city's first tile. Each pick takes the strongest
 * army of a kind not yet kept (a hero, a flier, the special abilities, a
 * magical army, woods and hills movers), so the keepers are a varied force.
 * A rally city's keepers are its strike force: they must have the group's
 * movement, and only heroes and fliers when it attacks by air.
 * @return {the armies in order, how many to keep on the first tile}.
 */
static std::pair<std::vector<Army*>, int> Arrange(Game& game, Side& side, const City& city,
                                                  const std::vector<Army*>& list) {
    AIData& data = Data(game, side);
    int army_count = static_cast<int>(list.size());
    int keep_wanted = army_count < 13 ? kKeep[army_count] : 8;
    // Bits for the kinds of army already picked; 0xfff once the keepers are
    // all picked.
    int mask = 0, min_max = 0, keep = 0;
    bool attack = false;
    std::vector<Army*> pool = list;
    if (Role(data, city) == kRally) {
        int fliers = 0, heroes = 0;
        for (int i = army_count - 1; i >= 0; i--) {
            Army* army = pool[i];
            if (IsHero(army))
                heroes++;
            else if (Flies(game, army) && (army->strength > 3 || fliers == 0))
                fliers++;
        }
        if (fliers != 0 && heroes != 0) fliers += heroes;
        if ((fliers > 3 && army_count < 17) || (fliers > 2 && army_count < 9)) attack = true;
        for (int group_index = kMaxGroups; group_index >= 1; group_index--) {
            const Group& group = *data.groups[group_index];
            if (group.active != 0 && group.rally == city.index) {
                if (Has(group.flags, kGfMovE12))
                    min_max = 12;
                else if (Has(group.flags, kGfMovE16))
                    min_max = 16;
                else
                    min_max = group.size;
                if (Has(group.flags, kGfFly)) attack = true;
            }
        }
    }
    // A city held only a few turns lets some armies go rather than keep them.
    int release_wanted = 3 - Get(data.held, city.index) / 3;
    if (release_wanted > 0 && release_wanted < army_count &&
        army_count - keep_wanted < release_wanted)
        keep_wanted = army_count - release_wanted;

    std::vector<Army*> out;
    for (;;) {
        int best_keep = -1, best_keep_i = kNone, best_other = -1, best_other_i = kNone;
        for (int i = army_count - 1; i >= 0; i--) {
            Army* army = pool[i];
            if (army) {
                int score = army->strength;
                if (static_cast<int>(out.size()) < keep_wanted) {
                    int ability = Ability(game, army);
                    if (!Has(mask, 1) && IsHero(army)) {
                        score += 1000;
                    } else if (min_max <= army->max_moves) {
                        if (!Has(mask, 0x10) && Flies(game, army))
                            score += 900;
                        else if (!attack) {
                            if (!Has(mask, 2) && ability == 2)
                                score += 800;
                            else if (!Has(mask, 4) && ability == 3)
                                score += 700;
                            else if (!Has(mask, 0x80) && Magical(game, army))
                                score += 600;
                            else if (!Has(mask, 8) && ability == 1)
                                score += 500;
                            else if (!Has(mask, 0x20) && Woods(game, army))
                                score += 400;
                            else if (!Has(mask, 0x40) && Hills(game, army))
                                score += 300;
                        }
                    }
                } else {
                    mask = 0xfff;
                }
                if (Has(mask, 1) && IsHero(army)) score = 1;
                if (Has(mask, 0x80) && Magical(game, army)) score = 2;
                if (army->max_moves < min_max || (attack && !Flies(game, army) && !IsHero(army))) {
                    if (best_other < score) {
                        best_other = score;
                        best_other_i = i;
                    }
                } else if (best_keep < score) {
                    best_keep = score;
                    best_keep_i = i;
                }
            }
        }
        if (best_keep_i == kNone && best_other_i == kNone) break;
        if (mask != 0xfff) {
            Army* army = pool[best_keep_i != kNone ? best_keep_i : best_other_i];
            int ability = Ability(game, army);
            if (IsHero(army))
                mask = Set(mask, 1);
            else {
                if (ability == 2) mask = Set(mask, 2);
                if (ability == 3) mask = Set(mask, 4);
                if (Flies(game, army)) mask = Set(mask, 0x10);
                if (Magical(game, army)) mask = Set(mask, 0x80);
                if (ability == 1) mask = Set(mask, 8);
                if (Woods(game, army)) mask = Set(mask, 0x20);
                if (Hills(game, army)) mask = Set(mask, 0x40);
            }
        }
        if (best_keep_i == kNone) {
            out.push_back(pool[best_other_i]);
            pool[best_other_i] = nullptr;
        } else {
            out.push_back(pool[best_keep_i]);
            pool[best_keep_i] = nullptr;
            keep++;
        }
    }
    if (keep_wanted < keep) keep = keep_wanted;
    return {out, keep};
}

/** @brief Disbands one army of a side. */
static void Disband(Game& game, Side& side, Army* army) {
    game::Disband(game, side, {army});
}

/**
 * @brief Hands items round between the heroes in one city.
 *
 * First, for each of three item types in turn, a hero with more than one
 * passes the spares to heroes with none. Then battle, command and standard
 * items flow from heroes with more to the hero with fewest; each hero passes
 * on only what it started with, so nothing goes back and forth.
 */
static void ShareItems(Game& game, AIData& data, const std::vector<Army*>& heroes) {
    auto count = [](Army* holder, const std::set<int>& types) {
        int held = 0;
        for (Item* item : holder->items)
            if (types.count(item->type)) held++;
        return held;
    };
    auto give = [&](Army* giver, Army* receiver, const std::set<int>& types) {
        auto& items = giver->items;
        for (size_t i = 0; i < items.size(); i++) {
            Item* item = items[i];
            if (types.count(item->type)) {
                items.erase(items.begin() + i);
                receiver->items.push_back(item);
                data.items_passed++;
                return true;
            }
        }
        return false;
    };
    for (const std::set<int>& item_types : {std::set<int>{6}, std::set<int>{5}, std::set<int>{7}}) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (Army* hero : heroes) {
                if (count(hero, item_types) > 1) {
                    for (Army* other : heroes) {
                        if (other != hero && count(other, item_types) == 0 &&
                            count(hero, item_types) > 1) {
                            if (give(hero, other, item_types)) moved = true;
                        }
                    }
                }
            }
        }
    }
    // battle, command and standard items: each hero passes on only what it
    // started with, so nothing goes back and forth
    std::set<int> battle_types = {1, 2, 8};
    std::vector<std::vector<Item*>> start(heroes.size());
    std::vector<int> own_count(heroes.size());
    for (size_t i = 0; i < heroes.size(); i++) {
        for (Item* item : heroes[i]->items)
            if (battle_types.count(item->type)) start[i].push_back(item);
        own_count[i] = static_cast<int>(start[i].size());
    }
    bool moved = true;
    while (moved) {
        moved = false;
        for (size_t i = 0; i < heroes.size(); i++) {
            Army* hero = heroes[i];
            if (!start[i].empty()) {
                int least = own_count[i], receiver = kNone;
                for (size_t j = 0; j < heroes.size(); j++) {
                    if (j != i && own_count[j] < least) {
                        least = own_count[j];
                        receiver = static_cast<int>(j);
                    }
                }
                if (receiver != kNone) {
                    Item* item = start[i].front();
                    start[i].erase(start[i].begin());
                    auto found = std::find(hero->items.begin(), hero->items.end(), item);
                    if (found != hero->items.end()) hero->items.erase(found);
                    heroes[receiver]->items.push_back(item);
                    data.items_passed++;
                    own_count[i]--;
                    own_count[receiver]++;
                    moved = true;
                }
            }
        }
    }
}

bool Garrison(Game& game, Side& side, City& city, bool force) {
    AIData& data = Data(game, side);
    int city_x = city.x, city_y = city.y;
    int last = Get(data.garrison, city.index);
    SetCflag(data, city, kCfCleaned);
    data.garrison[city.index] = 0;
    std::vector<Army*> list, heroes;
    int ordered = 0, fliers = 0, magic = 0;
    Army* last_hero = nullptr;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        if (i >= static_cast<int>(game.armies.size())) continue;
        Army* army = game.armies[i];
        if (army && !army->transit && army->x != kNone && army->x >= city_x &&
            army->x <= city_x + 1 && army->y >= city_y && army->y <= city_y + 1) {
            if (army->owner != side.index) {
                Side* owner_side = game.map->Side(army->owner == kNone ? 8 : army->owner);
                Disband(game, owner_side ? *owner_side : side, army);
            } else {
                data.garrison[city.index]++;
                if (army->ai_order == kOrderCity && army->ai_dest == city.index) ClearOrder(army);
                if (army->ai_order != 0) ordered++;
                if (IsHero(army) && heroes.size() < 8) {
                    heroes.push_back(army);
                    last_hero = army;
                }
                if (Flies(game, army)) fliers++;
                if (Magical(game, army)) magic++;
                if (list.size() < 32)
                    list.push_back(army);
                else
                    Disband(game, side, army);
            }
        }
    }
    if (last_hero) {
        Sel selection;
        selection.hero = last_hero;
        selection.armies = {last_hero};
        PickUp(game, selection);
    }
    if (heroes.size() > 1) ShareItems(game, data, heroes);
    ClearCflag(data, city, kCfHero);
    ClearCflag(data, city, kCfMagic);
    if (!heroes.empty()) SetCflag(data, city, kCfHero);
    if (magic != 0) SetCflag(data, city, kCfMagic);
    if (list.size() > 16) {
        last = 0;
        ordered = 0;
    }

    int old_role = Role(data, city);
    if (!((ordered < 3 || old_role == kRally) &&
          (force || !heroes.empty() || Get(data.garrison, city.index) != last ||
           game.rng.Dice(1, 6, -1) == 0 ||
           !game::ArmiesAt(game, city_x + 1, city_y + 1).empty()))) {
        return true;
    }
    data.keep[city.index] = 0;
    if (old_role == kWeak || old_role == kBuilding || old_role == kStop) {
        int want = Wanted(game, side, city);
        if (list.size() < 2)
            SetRole(data, city, kWeak);
        else if (static_cast<int>(list.size()) < want)
            SetRole(data, city, kBuilding);
        else
            SetRole(data, city, kStop);
    }
    auto [sorted, keep_count] = Arrange(game, side, city, list);
    int army_count = static_cast<int>(sorted.size());
    // A broke side disbands a quiet, long-held city's armies beyond its heroes,
    // its magical armies and four more.
    if (!force && side.gold < 300 && side.income < side.upkeep_total * 2) {
        if (Role(data, city) != kRally && Get(data.held, city.index) > 10 &&
            static_cast<int>(heroes.size()) + magic + 4 < army_count &&
            NearestArmy(game, side, city_x, city_y) > 10) {
            for (int i = static_cast<int>(heroes.size()) + magic + 4; i < army_count; i++) {
                if (sorted[i]) {
                    Disband(game, side, sorted[i]);
                    sorted[i] = nullptr;
                }
            }
        }
    }
    if (army_count > 24) {
        for (int i = 24; i < army_count; i++) {
            if (sorted[i]) {
                Disband(game, side, sorted[i]);
                sorted[i] = nullptr;
            }
        }
    }
    if ((game.map->options.quick_start == 0 || game.turn > 2) && army_count < 2)
        SetRole(data, city, kWeak);
    // An exploring city sends out its fliers, leaving at least two armies home.
    if (game.map->options.hidden_map != 0 && Role(data, city) == kExploreR2 && data.explorers < 5) {
        int to_send = fliers;
        int short_by = 2 - (army_count - fliers);
        if (short_by > 0) to_send = fliers - short_by;
        if (game.map->options.quick_start != 0 && game.turn < 3) to_send = 1;
        for (int i = 0; i < army_count; i++) {
            Army* army = sorted[i];
            if (to_send > 0 && army && Alive(game, army) && Flies(game, army)) {
                data.explorers++;
                army->ai_explore = true;
                moves::Explore(game, side, army, nullptr);
                sorted[i] = nullptr;
                to_send--;
            }
        }
    }
    // Stand the armies on the city's tiles: the keepers on the first, then
    // eight to a tile. A city after neutrals keeps nobody back.
    int tile, per_tile;
    int role = Role(data, city);
    if (role == kNearNeutral || role == kTakingNeutral) {
        tile = 1;
        per_tile = 8;
    } else {
        tile = keep_count == 0 ? 1 : 0;
        per_tile = keep_count == 0 ? 8 : keep_count;
    }
    for (int i = 0; i < army_count; i++) {
        Army* army = sorted[i];
        if (army && Alive(game, army) && tile <= 3) {
            if (tile == 0) data.keep[city.index] = Get(data.keep, city.index) + 1;
            army->x = city_x + kTileDx[tile];
            army->y = city_y + kTileDy[tile];
            army->at_sea = false;
            army->target.reset();
            army->group = 0;
            army->ai_group = 0;
            army->done = false;
            per_tile--;
            if (per_tile == 0) {
                tile++;
                per_tile = 8;
            }
        }
    }
    return true;
}

void Clean(Game& game, Side& side) {
    AIData& data = Data(game, side);
    for (City* city : Own(game, side)) {
        if (!Cflag(data, *city, kCfCleaned)) Garrison(game, side, *city, false);
    }
}

/**
 * @brief Sends a stack from a city at the neutral cities round it.
 *
 * One target after another: each time the stack goes for the seen neutral
 * neighbour it has 75% odds or more at, scored by distance, odds and how
 * many are already heading there. A city taken becomes the next starting
 * point unless the side is cautious or an enemy is near.
 * @param count How many armies the stack adds to its target's heading count.
 * @param heading Stacks (armies) on their way, by city index.
 */
static void TakeNeutrals(Game& game, Side& side, City& city, int count, std::map<int, int>& heading,
                         std::vector<Army*> stack) {
    AIData& data = Data(game, side);
    bool neutral_on = game.map->options.neutral_cities != 0;
    // Without neutral garrisons every neutral city is a sure thing.
    int odds = 100;
    City* from = &city;
    City* last_from = nullptr;
    City* last_pick = nullptr;
    for (;;) {
        std::vector<Army*> keep;
        for (Army* army : stack)
            if (Alive(game, army) && army->owner == side.index) keep.push_back(army);
        stack = keep;
        if (stack.empty()) return;
        Sel sel = Select(game, stack);
        auto neutrals = NeutralNeighbours(game, side, *from);
        if (neutrals.empty()) return;
        City* best = nullptr;
        int best_score = 1000;
        for (int j = static_cast<int>(neutrals.size()) - 1; j >= 0; j--) {
            const NeutralNeighbour& entry = neutrals[j];
            City* neutral = entry.city;
            if (!Cflag(data, *neutral, kCfUnseen) && Get(heading, neutral->index) < 3) {
                if (neutral_on) odds = Odds(game, sel, neutral->x, neutral->y);
                if (odds > 74) {
                    int score = game.rng.Dice(1, 10, 0) + entry.dist +
                                Get(heading, neutral->index) * 10 + (entry.dist >= 41 ? 10 : 0) +
                                (entry.dist >= 51 ? 30 : 0) + (100 - odds);
                    if (score < best_score) {
                        best = neutral;
                        best_score = score;
                    }
                }
            }
        }
        if (!best) break;
        heading[best->index] = Get(heading, best->index) + count;
        last_from = &city;
        last_pick = best;
        auto ordered = Order(game, stack, kOrderCity, best->index, 0x80);
        if (ordered)
            MoveTo(game, *ordered, ordered->leader->target->first, ordered->leader->target->second);
        if (best->owner_index != side.index) return;
        if (data.cautious != 0) return;
        from = best;
        if (NearestArmy(game, side, best->x, best->y) < 10) return;
    }
    // Matches the reference: last_from is always the starting city here.
    if (game.map->options.hidden_map != 0 && last_from == &city && last_pick) {
        Order(game, stack, kOrderCity, last_pick->index, 0x80);
    }
}

/**
 * @brief How many armies a city holds back from going out.
 * @return {extra for nearby enemies and caution, up to 2 for cities lost,
 *     the distance to the nearest enemy army}.
 */
static std::tuple<int, int, int> HoldBack(Game& game, Side& side, const City& city) {
    AIData& data = Data(game, side);
    int near = NearestArmy(game, side, city.x, city.y);
    int extra = (near < 5 ? 2 : 0) + (near < 15 ? 1 : 0) + (data.cautious != 0 ? 1 : 0);
    int lost = 0;
    for (int i = 0; i < 8; i++) lost += data.cities_lost[i];
    return {extra, std::min(2, lost), near};
}

/**
 * @brief The idle armies on a city's corner tile that may go out.
 *
 * Skips the first `reserve` that pass `test` (held back), takes up to eight
 * more, the fastest first.
 */
template <class Test>
static std::vector<Army*> Idle(Game& game, Side& side, const City& city, int reserve, Test test) {
    std::vector<Army*> found;
    for (Army* army : OnTile(game, side.index, city.x, city.y)) {
        if (army->ai_group == 0 && !army->done && army->ai_order == 0 && test(army)) {
            if (reserve > 0)
                reserve--;
            else if (found.size() < 8)
                found.push_back(army);
        }
    }
    LuaSort(found, [](Army* lhs, Army* rhs) { return lhs->max_moves > rhs->max_moves; });
    return found;
}

/**
 * @brief Sends a city's idle armies at its neutral neighbours.
 *
 * In stacks of eight, or one by one when neutral cities are undefended.
 * @return True with the map hidden: the city then sends explorers.
 */
static bool AttackNeutrals(Game& game, Side& side, City& city, std::map<int, int>& heading) {
    auto [extra, lost, near] = HoldBack(game, side, city);
    (void)near;
    auto list = Idle(game, side, city, extra + lost, [](Army*) { return true; });
    if (list.empty()) return false;
    size_t per_stack = game.map->options.neutral_cities != 0 ? 8 : 1;
    size_t next = 0;
    for (;;) {
        std::vector<Army*> stack;
        while (next < list.size() && stack.size() < per_stack) {
            stack.push_back(list[next]);
            next++;
        }
        if (stack.empty()) break;
        if (extra + lost == 0 && game.map->options.hidden_map != 0 &&
            NearestArmy(game, side, city.x, city.y) < 15)
            break;
        TakeNeutrals(game, side, city, static_cast<int>(stack.size()), heading, stack);
    }
    return game.map->options.hidden_map != 0;
}

/**
 * @brief Sends a city's idle armies to explore, one by one.
 *
 * Heroes stay home while an assault group is gathering.
 * @param fliers_only Send only flying armies.
 */
static void SendExplorers(Game& game, Side& side, City& city, bool fliers_only) {
    AIData& data = Data(game, side);
    bool grouping = false;
    for (int i = 1; i <= kMaxGroups; i++)
        if (data.groups[i]->active != 0) grouping = true;
    auto [extra, lost, near] = HoldBack(game, side, city);
    (void)near;
    auto list = Idle(game, side, city, extra + lost, [&](Army* candidate) {
        if (IsHero(candidate) && grouping) return false;
        if (fliers_only && !Flies(game, candidate)) return false;
        return true;
    });
    for (Army* army : list) {
        if (extra + lost == 0 && game.map->options.hidden_map != 0 &&
            NearestArmy(game, side, city.x, city.y) < 15)
            return;
        if (Alive(game, army)) moves::Explore(game, side, army, &city);
    }
}

City* OpenNeutral(Game& game, Side& side, const City& city, std::map<int, int>& heading) {
    auto neutrals = NeutralNeighbours(game, side, city);
    for (int j = static_cast<int>(neutrals.size()) - 1; j >= 0; j--) {
        City* neutral = neutrals[j].city;
        if (Get(heading, neutral->index) <= 2) return neutral;
    }
    return nullptr;
}

/**
 * @brief One pass of the neutral phase.
 *
 * The first pass covers every own city, later ones only cities just taken.
 * A city with neutral neighbours (or, early in a game without Quick Start, a
 * building city) sends its idle armies out; its role then says whether it is
 * taking neutrals, near them, or weak.
 * @return How many cities acted.
 */
static int NeutralPass(Game& game, Side& side, int pass, std::map<int, int>& heading) {
    AIData& data = Data(game, side);
    int acted = 0;
    for (City* city : Own(game, side)) {
        if (city->owner_index == side.index && (pass == 0 || Role(data, *city) == kJustTaken)) {
            if (Role(data, *city) == kJustTaken) {
                Garrison(game, side, *city, true);
                SetRole(data, *city, kNearNeutral);
            }
            bool act = !NeutralNeighbours(game, side, *city).empty();
            if (!act) {
                int role = Role(data, *city);
                if (role == kNearNeutral || role == kTakingNeutral) SetRole(data, *city, kWeak);
                if (game.map->options.quick_start == 0 && Role(data, *city) == kBuilding &&
                    game.turn < 6)
                    act = true;
            }
            if (act) {
                acted++;
                if (!AttackNeutrals(game, side, *city, heading)) {
                    SetRole(data, *city,
                            OpenNeutral(game, side, *city, heading) ? kNearNeutral : kWeak);
                } else {
                    SendExplorers(game, side, *city, game.turn > 10);
                    SetRole(data, *city, kTakingNeutral);
                }
            }
        }
    }
    return acted;
}

void Neutral(Game& game, Side& side) {
    // Armies already on their way, by the city they are heading for.
    std::map<int, int> heading;
    for (Army* army : Armies(game, side.index)) {
        if (army->ai_order == kOrderCity && army->ai_dest != kNone &&
            army->ai_dest < static_cast<int>(game.map->cities.size())) {
            heading[army->ai_dest] = Get(heading, army->ai_dest) + 1;
        }
    }
    for (int pass = 0; pass <= 9; pass++) {
        if (NeutralPass(game, side, pass, heading) == 0) break;
    }
}

/**
 * @brief A strong garrison strikes at a weak neighbour.
 *
 * The armies with 8 moves or more on the keepers' tile (on the next tile for
 * a rally city, and only with more than 11 armies) go for the enemy or
 * neutral neighbour they reach soonest with good odds (65% for a broke
 * side, else 85%). A small stack stays home when a neighbour it cannot beat
 * is near.
 */
static void QuickStrike(Game& game, Side& side, City& city) {
    AIData& data = Data(game, side);
    int role = Role(data, city);
    int x = city.x;
    int y = city.y;
    if (role != kRally) x++;
    if (!(role != kRally || Get(data.garrison, city.index) > 11)) return;
    int need = (side.gold < 40 && side.income < side.upkeep_total) ? 65 : 85;
    auto list = Collect(game, side.index, x, y, 8);
    int army_count = static_cast<int>(list.size());
    if (army_count == 0) return;
    if (army_count < 4 && (role == kMember || role == kRally)) return;
    Sel sel = Select(game, list);
    auto [neighbours, neighbour_dists] = FindNeighbours(game, city);
    bool danger = false;
    City* best = nullptr;
    int best_odds = 0, best_turns = 100;
    for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
        City* target = neighbours[j];
        if (!Cflag(data, *target, kCfUnseen) && target->owner_index != side.index &&
            Standing(*target) && target->index != data.quest_city &&
            (target->owner_index == kNone || State(game, side.index, target->owner_index) == 2)) {
            int odds = Odds(game, sel, target->x, target->y);
            if (odds == 0) {
                if ((army_count < 4 && neighbour_dists[j] < 25) ||
                    (army_count < 6 && neighbour_dists[j] < 15))
                    danger = true;
            } else if (odds >= need) {
                int turns = neighbour_dists[j] / std::max(1, sel.min_moves - 2) + 1;
                if (turns < best_turns || (turns == best_turns && best_odds < odds)) {
                    best = target;
                    best_odds = odds;
                    best_turns = turns;
                }
            }
        }
    }
    if (!danger && best) {
        auto ordered = Order(game, list, kOrderCity, best->index, 0);
        if (ordered)
            MoveTo(game, *ordered, ordered->leader->target->first, ordered->leader->target->second);
    }
}

void QuickAttack(Game& game, Side& side) {
    AIData& data = Data(game, side);
    if (data.cautious != 0) return;
    for (City* city : Own(game, side)) {
        if (city->owner_index == side.index && kQuickRoles.count(Role(data, *city)) &&
            Get(data.garrison, city->index) > 3) {
            QuickStrike(game, side, *city);
        }
    }
}

void UpdateHide(Game& game, Side& side) {
    AIData& data = Data(game, side);
    ClearUnseen(game, side);
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (Role(data, city) == kTakingNeutral) {
            for (auto& entry : NeutralNeighbours(game, side, city)) {
                if (!Cflag(data, *entry.city, kCfUnseen)) {
                    SetRole(data, city, kNearNeutral);
                    break;
                }
            }
        }
    }
}

/**
 * @brief The city that should buy a new production type.
 *
 * Among cities that build nothing worth having, the one held longest
 * (cities near neutrals count four turns younger, and only after three).
 * Also moves cities between kStop and kNowhere by whether they build well.
 * @return The city, or null.
 */
static City* CityForType(Game& game, Side& side) {
    AIData& data = Data(game, side);
    City* best = nullptr;
    int best_score = -1;
    for (City* city : Own(game, side)) {
        int role = Role(data, *city);
        bool candidate = false;
        int younger = 0;
        if (role == kTakingNeutral || role == kNearNeutral) {
            if (Get(data.held, city->index) > 3) {
                younger = 4;
                candidate = !BuildsWell(game, side, *city).first;
            }
        } else if (role == kWeak || role == kBuilding) {
            candidate = !BuildsWell(game, side, *city).first;
        } else if (role == kStop) {
            if (!BuildsWell(game, side, *city).first) {
                SetRole(data, *city, kNowhere);
                candidate = true;
            }
        } else if (role == kNowhere) {
            if (!BuildsWell(game, side, *city).first)
                candidate = true;
            else
                SetRole(data, *city, kStop);
        }
        if (candidate) {
            int score = Get(data.held, city->index) - younger;
            if (best_score < score) {
                best = city;
                best_score = score;
            }
        }
    }
    return best;
}

void Rebuild(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int limit = std::max(
            5, static_cast<int>(FloorDiv(static_cast<long>(data.rebuild_limit) *
                                                 static_cast<long>(game.map->cities.size()),
                                         80)));
    if ((data.heroes > 4 || data.own <= limit * data.heroes) && side.gold > 499) {
        int type_id = side.gold < 2001 ? data.rebuild_type : data.rebuild_type_rich;
        City* city = CityForType(game, side);
        if (city && side.gold > 499) BuyType(game, side, *city, type_id);
    }
}

/**
 * @brief Picks the member cities of an assault group.
 *
 * Up to four free cities that build well enough, fast enough for the group
 * (and flying for a flying group); a stronger producer replaces the weakest
 * member once four are found. Members get role kMember.
 */
static void PickMembers(Game& game, Side& side, Group& group) {
    AIData& data = Data(game, side);
    int min_max = Has(group.flags, kGfMovE12) ? 12 : Has(group.flags, kGfMovE16) ? 16 : group.size;
    // 1-based, as the group's member slots are
    std::map<int, City*> members;
    std::map<int, int> strength;
    int count = 0;
    for (City* city : Own(game, side)) {
        if (groups::Free(game, side, *city, false)) {
            int city_flags = Get(data.flags, city->index);
            bool fits = (!Has(group.flags, kGfMovE12) || Has(city_flags, kCfMovE12)) &&
                        (!Has(group.flags, kGfMovE16) || Has(city_flags, kCfMovE16)) &&
                        (!Has(group.flags, kGfFly) || Has(city_flags, kCfFlygroup));
            // A city fit for a special group is saved for one.
            bool plain = Has(group.flags, kGfMovE16) || Has(group.flags, kGfMovE12) ||
                         Has(group.flags, kGfFly) ||
                         !(Has(city_flags, kCfMovE12) || Has(city_flags, kCfFlygroup) ||
                           Has(city_flags, kCfMovE16));
            if (fits && plain) {
                auto [good, slot_index] = BuildsWell(game, side, *city);
                if (good && min_max <= city->slots[slot_index].move) {
                    const Slot& slot = city->slots[slot_index];
                    int target_slot = kNone;
                    if (count < 4) {
                        target_slot = count + 1;
                    } else {
                        int least = 100;
                        for (int member = 1; member <= 4; member++) {
                            if (strength.count(member) && strength[member] < least) {
                                least = strength[member];
                                target_slot = member;
                            }
                        }
                        if (target_slot != kNone && !(least <= slot.strength)) target_slot = kNone;
                    }
                    if (target_slot != kNone) {
                        if (!members.count(target_slot)) count++;
                        members[target_slot] = city;
                        strength[target_slot] = slot.strength;
                    }
                }
            }
        }
    }
    group.members.clear();
    for (int member = 1; member <= count; member++) {
        City* city = members[member];
        group.members[member] = city->index;
        SetRole(data, *city, kMember);
    }
}

void Production(Game& game, Side& side) {
    AIData& data = Data(game, side);
    for (City* city : Own(game, side)) {
        if (Role(data, *city) == kMember) {
            SetRole(data, *city, kStop);
            city->vector_to = kNone;
        }
    }
    for (int group_index = kMaxGroups; group_index >= 1; group_index--) {
        if (data.groups[group_index]->active != 0)
            PickMembers(game, side, *data.groups[group_index]);
    }
    if (game.map->options.hidden_map != 0) {
        // a city whose neutral neighbours are all unseen goes looking
        for (City* city : Own(game, side)) {
            if (Role(data, *city) == kNearNeutral) {
                auto neutrals = NeutralNeighbours(game, side, *city);
                if (!neutrals.empty()) {
                    bool seen = false;
                    for (auto& entry : neutrals)
                        if (!Cflag(data, *entry.city, kCfUnseen)) seen = true;
                    if (!seen) SetRole(data, *city, kTakingNeutral);
                }
            }
        }
    }
    // The role picks the production purpose (see docs/re/ai.md, Production).
    for (City* city : Own(game, side)) {
        city->vector_to = kNone;
        if (!Building(*city)) {
            if (side.gold < 40 && side.income < side.upkeep_total) return;
            int role = Role(data, *city);
            int purpose = 3;
            if (role == kTakingNeutral || role == kExplorer) {
                if (!Cflag(data, *city, kCfFlier)) BuyFlier(game, side, *city);
                purpose = 4;
            } else if (role == kNearNeutral) {
                purpose = game.map->options.neutral_cities != 0 ? 2 : 1;
            } else if (role == kWeak) {
                purpose = 2;
            } else if (role == kExploreR2) {
                purpose = 4;
            } else if (role == kStop) {
                purpose = kNone;
                city->producing = kNone;
                city->countdown = 0;
            }
            if (purpose != kNone) ProduceFor(game, side, *city, purpose);
        }
    }
}

void Vectoring(Game& game, Side& side) {
    AIData& data = Data(game, side);
    for (int group_index = data.max_groups; group_index >= 1; group_index--) {
        Group& group = *data.groups[group_index];
        City* rally = group.rally != kNone ? game.map->City(group.rally) : nullptr;
        if (group.active != 0 && rally && rally->owner_index == side.index) {
            SetRole(data, *rally, kRally);
            for (int member = 4; member >= 1; member--) {
                City* member_city = group.members.count(member)
                                            ? game.map->City(group.members[member])
                                            : nullptr;
                if (member_city && Role(data, *member_city) == kMember)
                    Vector(game, *member_city, rally);
            }
        }
    }
}

}  // namespace w2::ai::cities
