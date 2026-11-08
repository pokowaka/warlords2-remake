/**
 * @file
 * @brief Headless tests for the rules core.
 *
 * The Lua remake's love2d/test/run.lua, ported (by way of web/test/run.js).
 * No SDL, no graphics. Run from the repository root:
 *
 *     bazel-bin/cpp/w2test
 *
 * Every assertion cites the rule it checks; docs/rules.md is the spec. Where
 * the Lua counts from 1 these count from 0, so a city's first production
 * slot is slot 0 here.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/aicard.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/cues.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/mapeval.hpp"
#include "warlords/move.hpp"
#include "warlords/pck.hpp"
#include "warlords/preview.hpp"
#include "warlords/quest.hpp"
#include "warlords/randommap.hpp"
#include "warlords/report.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"
#include "warlords/scenarios.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"
#include "warlords/uidata.hpp"

using namespace w2;
namespace movement = w2::move;

/// The game data directory, relative to the repository root.
static const std::string kData = "original";
/// Every scenario shipped with the original game; most tests run once per scenario.
static const std::vector<std::string> kScenarios = {"ERYTHEA", "ILLURIA", "DRAGON", "HADESHA",
                                                    "ISLADIA", "SORCERY", "TUTORIA"};

/// How many assertions passed and failed so far.
static int passed = 0, failed = 0;

/**
 * @brief Records one assertion, printing @p what when @p cond is false.
 */
static void Ok(bool cond, const std::string& what) {
    if (cond)
        passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

/** @brief Formats a bool for a failure message. */
static std::string Show(bool value) {
    return value ? "true" : "false";
}
/** @brief Formats an integer for a failure message; kNone shows as "nil", as in the Lua. */
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
static std::string Show(T value) {
    return std::is_signed_v<T> && static_cast<long long>(value) == kNone ? "nil"
                                                                          : std::to_string(value);
}
/** @brief Formats an enum as its underlying number for a failure message. */
template <class T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
static std::string Show(T value) {
    return std::to_string(static_cast<long long>(value));
}
/** @brief Formats a string for a failure message (as is). */
static std::string Show(const std::string& value) {
    return value;
}
/** @brief Formats a C string for a failure message; null shows as "nil". */
static std::string Show(const char* value) {
    return value ? value : "nil";
}
/** @brief Formats a pointer for a failure message; null shows as "nil". */
template <class T>
static std::string Show(T* pointer) {
    return pointer ? Fmt("%p", static_cast<const void*>(pointer)) : "nil";
}

/**
 * @brief Records an equality assertion, printing both values when they differ.
 */
template <class A, class B>
static void Eq(const A& got, const B& want, const std::string& what) {
    if (got == want)
        passed++;
    else {
        failed++;
        printf("  FAIL  %s: got %s, want %s\n", what.c_str(), Show(got).c_str(),
               Show(want).c_str());
    }
}
/** @brief Eq for a string against a literal, so both compare as strings. */
static void Eq(const std::string& got, const char* want, const std::string& what) {
    Eq(got, std::string(want), what);
}

/**
 * @brief The test-side view of game::NewGameOptions, with defaults.
 */
struct GameOpts {
    double seed = 0;  ///< The random seed for the game.
    std::vector<std::pair<std::string, int>> options;  ///< Game options by name, with values.
    std::map<int, game::SideSetup> sides;  ///< Per-side setup by side index; unlisted sides keep the default.
    bool greatest = false;  ///< Start with the greatest armies.
};
/** @brief Starts a new game of @p scenario from the original data. */
static std::unique_ptr<Game> NewGame(const std::string& scenario,
                                     GameOpts game_options = GameOpts()) {
    game::NewGameOptions new_game_options;
    new_game_options.seed = game_options.seed;
    new_game_options.options = game_options.options;
    new_game_options.sides = game_options.sides;
    new_game_options.greatest = game_options.greatest;
    return game::NewGame(kData, scenario, new_game_options);
}
/** @brief Game options with only the seed set. */
static GameOpts Seed(double seed) {
    GameOpts game_options;
    game_options.seed = seed;
    return game_options;
}
/**
 * @brief Ends the turn as the interface does.
 *
 * A human's turn opens with the hero offer left standing, and then its
 * cities build.
 * @return The side whose turn it is now.
 */
static Side* EndTurn(Game& game) {
    Side* side = game::EndTurn(game);
    if (side && !side->computer) game::HumanProduction(game, *side);
    return side;
}
/**
 * @brief Plays a human side by the computer player, as some tests do.
 *
 * It takes the hero it is offered first, as a computer does.
 */
static void PlayAi(Game& game, Side& side) {
    if (!side.computer && side.hero_offer) {
        hero::Recruit(game, side, *side.hero_offer);
        side.hero_offer.reset();
    }
    ai::PlayTurn(game, side);
}
/** @brief Game options with the seed and the named game options set. */
static GameOpts SeedWith(double seed, std::vector<std::pair<std::string, int>> options) {
    GameOpts game_options;
    game_options.seed = seed;
    game_options.options = options;
    return game_options;
}
/** @brief A side's setup: human or computer, its level, and whether it is out of play. */
static game::SideSetup SideSetup(bool computer, int level = kNone, bool off = false) {
    game::SideSetup setup;
    setup.computer = computer;
    setup.level = level;
    setup.off = off;
    return setup;
}

/**
 * @brief Places a new army of @p type at (@p x, @p y) for side @p owner.
 * @return The army as the game stores it.
 */
static Army* AddArmy(Game& game, int x, int y, int owner, int type, int strength, int moves = 0,
                     int max_moves = 0) {
    Army army;
    army.x = x;
    army.y = y;
    army.owner = owner;
    army.type = type;
    army.strength = strength;
    army.moves = moves;
    army.max_moves = max_moves;
    army.name = game.types.ById(type) ? game.types.ById(type)->name : "";
    return game.Add(army);
}

// ------------------------------------------------------------------ dice

/** @brief The dice roll within their bounds and a seed reproduces them. */
static void TestDice() {
    printf("dice\n");
    Rng rng(1234);
    int lowest = 1 << 30, highest = -(1 << 30);
    for (int i = 0; i < 20000; i++) {
        int roll = rng.Dice(1, 100, 0);
        lowest = std::min(lowest, roll);
        highest = std::max(highest, roll);
    }
    Eq(lowest, 1, "1d100 lower bound");
    Eq(highest, 100, "1d100 upper bound");
    lowest = 1 << 30;
    highest = -(1 << 30);
    for (int i = 0; i < 20000; i++) {
        int roll = rng.Dice(3, 500, 500);
        lowest = std::min(lowest, roll);
        highest = std::max(highest, roll);
    }
    Ok(lowest >= 503, "3d500+500 lower bound");
    Ok(highest <= 2000, "3d500+500 upper bound");
    std::set<int> seen;
    for (int i = 0; i < 2000; i++) seen.insert(rng.Dice(1, 5, -1));
    for (int i = 0; i <= 4; i++) Ok(seen.count(i), "dice(1,5,-1) reaches " + std::to_string(i));
    Ok(!seen.count(5), "dice(1,5,-1) stays below 5");
    Rng first_rng(7), second_rng(7);
    bool same = true;
    for (int i = 0; i < 100; i++)
        if (first_rng.Dice(2, 6, 0) != second_rng.Dice(2, 6, 0)) same = false;
    Ok(same, "a seed reproduces the sequence");
}

/** @brief ARMYTYPE.DAT loads with the documented types and stats. */
static void TestArmyTypes() {
    printf("ARMYTYPE.DAT\n");
    Types types;
    armytype::Load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    Eq(static_cast<int>(types.all.size()), 29, "29 army types");
    Ok(types.ById(armytype::kHero) != nullptr, "type 28 exists");
    Eq(types.ById(armytype::kHero)->name, "Hero", "type 28 is the Hero");
    Eq(types.ById(armytype::kScouts)->name, "Scouts", "type 11 is Scouts");
    const ArmyType* heavy_cavalry = nullptr;
    for (auto& army_type : types.all)
        if (army_type.name == "Heavy Cav.") heavy_cavalry = &army_type;
    Ok(heavy_cavalry != nullptr, "Heavy Cav. is in the table");
    if (heavy_cavalry) {
        Eq(heavy_cavalry->strength, 4, "Heavy Cav. strength");
        Eq(heavy_cavalry->time, 3, "Heavy Cav. production time");
        Eq(heavy_cavalry->cost, 8, "Heavy Cav. cost");
        Eq(heavy_cavalry->move, 16, "Heavy Cav. base move");
    }
    bool any_fly = false;
    for (auto& army_type : types.all)
        if (army_type.flies) any_fly = true;
    Ok(any_fly, "some types fly");
    const ArmyType* elephants = nullptr;
    for (auto& army_type : types.all)
        if (army_type.name == "Elephants") elephants = &army_type;
    Ok(elephants != nullptr, "Elephants are in the table");
    if (elephants)
        Eq(elephants->bonus[50], -1, "Elephants take 1 off the enemy stack: the field is signed");
}

/** @brief Production slots, their nudged stats, slot choice and city defence. */
static void TestProduction() {
    printf("production slots and garrisons\n");
    Types types;
    armytype::Load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    Rng rng(42);
    auto slots = rules::CitySlots({4, 11, 2}, types, rng);
    Eq(static_cast<int>(slots.size()), 3, "one slot per production type");
    bool sorted = true;
    for (size_t i = 1; i < slots.size(); i++)
        if (slots[i].price < slots[i - 1].price) sorted = false;
    Ok(sorted, "slots are sorted by price");
    bool ok_bounds = true;
    for (int seed = 1; seed <= 2000; seed++) {
        Rng seeded_rng(seed);
        for (auto& slot : rules::CitySlots({1, 2, 3, 4}, types, seeded_rng)) {
            const ArmyType* base = types.ById(slot.type);
            if (slot.strength < 1 || slot.strength > 9) ok_bounds = false;
            if (slot.strength < base->strength - 1 || slot.strength > base->strength + 1)
                ok_bounds = false;
            if (slot.move < 6) ok_bounds = false;
            if (slot.move > base->move + 4) ok_bounds = false;
            if (slot.time < 1 || slot.time > base->time + 1) ok_bounds = false;
        }
    }
    Ok(ok_bounds, "nudged stats stay within the documented range");
    bool any_flier = false;
    for (auto& slot : slots)
        if (types.ById(slot.type)->flies) any_flier = true;
    int best4 = rules::BestSlot(slots, 4, types, false);
    if (any_flier)
        Ok(best4 != kNone, "purpose 4 finds a flier");
    else
        Ok(best4 == kNone, "purpose 4 refuses a city with no fliers");
    int fast = rules::BestSlot(slots, 6, types, false);
    int strong = rules::BestSlot(slots, 3, types, false);
    Ok(fast != kNone && strong != kNone, "purposes 3 and 6 always choose something");
    Eq(rules::CityDefence(2), 1, "fewer than 3 types: defence 1");
    Eq(rules::CityDefence(3), 2, "3 types: defence 2");
    Eq(rules::CityDefence(4), 2, "4 types: defence 2");
    Slot slot{4, "x", 8, 2, 8, 12, 0};
    Eq(rules::ArmyFromSlot(slot, true).strength, 9, "Enhanced caps strength at 9");
    Eq(rules::ArmyFromSlot(slot, false).strength, 8, "no Enhanced, no bonus");
    Eq(rules::ArmyFromSlot(slot, false).upkeep, 4, "upkeep is half the slot cost");
}

/** @brief A new game of @p scenario starts as the rules say and a seed rebuilds it. */
static void TestGame(const std::string& scenario) {
    printf("game: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(3));
    Game& game = *game_ptr;
    Ok(game.sides.size() >= 2, "at least two sides in play");
    Eq(static_cast<int>(game.armies.size()), static_cast<int>(game.map->cities.size()),
       "exactly one starting army per city");
    for (Side* side : game.sides) {
        Eq(static_cast<int>(game::SideCities(game, *side).size()), 1,
           side->name + " owns only its capital");
        Eq(game::SideCities(game, *side)[0]->index, side->capital->index,
           side->name + "'s city is its capital");
    }
    for (auto& city : game.map->cities) {
        for (auto& slot : city.slots)
            Ok(slot.type != armytype::kNavy, city.name + " does not produce Navy");
        Eq(city.defence, rules::CityDefence(static_cast<int>(city.slots.size())),
           city.name + " defence");
        Ok(city.slots.size() <= 4, city.name + " has at most 4 slots");
    }
    for (Army* army : game.armies) {
        Ok(game.types.ById(army->type) != nullptr, "army has a real type");
        Eq(army->moves, 0, "a starting garrison has no moves");
    }
    std::map<int, int> per_tile;
    for (Army* army : game.armies) {
        int tile = army->y * game.map->width + army->x;
        per_tile[tile]++;
        Ok(per_tile[tile] <= rules::kMaxStack, "stack limit holds");
    }
    auto rebuilt = NewGame(scenario, Seed(3));
    bool same = rebuilt->armies.size() == game.armies.size();
    for (size_t i = 0; i < game.armies.size() && same; i++) {
        Army* army = game.armies[i];
        Army* rebuilt_army = rebuilt->armies[i];
        if (rebuilt_army->type != army->type || rebuilt_army->strength != army->strength)
            same = false;
    }
    Ok(same, "the same seed rebuilds the same game");
}

/**
 * @brief The turn loop: moves refresh and carry over, cities build, and a
 * side with no cities leaves the game as the round ends.
 */
static void TestTurnLoop(const std::string& scenario) {
    printf("turn loop: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(5));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Ok(side != nullptr, "the first side is to play");
    for (Army* army : game::SideArmies(game, *side))
        Eq(army->moves, army->max_moves, "a garrison starts the turn with its full move");
    int gold0 = side->gold;
    Eq(side->income, side->capital->income, "income is the capital's income");
    Ok(side->gold == std::max(0, gold0), "gold was applied");
    {
        Army* one = game::SideArmies(game, *side)[0];
        one->done = true;
        one->offered = true;
        one->fortified = true;
        for (size_t i = 0; i < game.sides.size(); i++) EndTurn(game);
        Eq(one->done, false, "done for this turn wears off with the turn");
        Eq(one->offered, false, "and so does having been offered this pass");
        Eq(one->fortified, true, "but digging in outlasts it");
        one->fortified = false;
    }
    Army* carried = game::SideArmies(game, *side)[0];
    carried->moves = 5;
    int before = carried->max_moves;
    for (size_t i = 0; i < game.sides.size(); i++) EndTurn(game);
    Eq(carried->moves, before + rules::kMoveCarry, "at most 2 unused moves carry over");
    carried->moves = 1;
    for (size_t i = 0; i < game.sides.size(); i++) EndTurn(game);
    Eq(carried->moves, before + 1, "carry-over below the cap is exact");

    City* city = side->capital;
    game::SetProduction(game, *city, 0);
    int want = city->slots[0].time;
    size_t armies_before = game::SideArmies(game, *side).size();
    for (size_t i = 0; i < want * game.sides.size(); i++) EndTurn(game);
    Ok(game::SideArmies(game, *side).size() == armies_before + 1,
       Fmt("%s produced after %d turns", city->name.c_str(), want));
    Army* built = nullptr;
    for (Army* army : game::SideArmies(game, *side))
        if (army->home_city == city->index && army != carried) built = army;
    Ok(built != nullptr, "the new army belongs to the city that built it");
    if (built) {
        Eq(built->upkeep, city->slots[0].cost / 2, "upkeep is half the slot cost");
        Eq(built->x, city->x, "the new army stands in its city");
    }
    // a side with no cities stays in play until the round ends, then goes,
    // with its armies, its gold and its diplomacy
    Side* victim = game.sides.back();
    for (City* victim_city : game::SideCities(game, *victim)) victim_city->owner_index = kNone;
    victim->gold = 500;
    game.diplomacy.state[static_cast<size_t>(victim->index) * 8] = diplomacy::kWar;
    Ok(!game::SideArmies(game, *victim).empty(), "the beaten side still has armies");
    int round_of = game.turn;
    while (game.turn == round_of) {
        Ok(victim->alive, "a side with no cities is in play until the round ends");
        EndTurn(game);
    }
    Ok(!victim->alive, "a side with no cities is eliminated as the round ends");
    Eq(static_cast<int>(game::SideArmies(game, *victim).size()), 0,
       "the eliminated side's armies are gone");
    Eq(victim->gold, 0, "the eliminated side's gold is gone");
    Eq(diplomacy::State(game, victim->index, 0), diplomacy::kIntermediate,
       "the eliminated side's diplomacy is reset to uneasy");
    Ok(!game.ending.fallen.empty() && game.ending.fallen[0].side == victim,
       "the elimination is announced");
    auto second_game = NewGame(scenario, Seed(6));
    game::Begin(*second_game);
    Eq(second_game->turn, 1, "the game starts on turn 1");
    for (size_t i = 0; i < second_game->sides.size(); i++) EndTurn(*second_game);
    Eq(second_game->turn, 2, "a full round advances the turn");
}

/** @brief Vectoring: armies sent from a city arrive two turns later, at most four to a city. */
static void TestVectoring(const std::string& scenario) {
    printf("vectoring: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(9));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* from = side->capital;
    City* dest = nullptr;
    for (auto& city : game.map->cities)
        if (!dest && city.owner_index == kNone && !city.slots.empty()) dest = &city;
    Ok(dest != nullptr, "found a neutral city to capture");
    if (!dest) return;
    dest->owner_index = side->index;
    for (Army* army : game::ArmiesAt(game, dest->x, dest->y)) army->owner = side->index;
    game::SetProduction(game, *from, 0);
    game::Vector(game, *from, dest);
    int time = from->slots[0].time;
    size_t at_dest = game::ArmiesAt(game, dest->x, dest->y).size();
    for (size_t i = 0; i < (time + 2) * game.sides.size(); i++) EndTurn(game);
    Ok(game::ArmiesAt(game, dest->x, dest->y).size() > at_dest,
       "a vectored army arrives two turns after it is built");
    {
        // to the turn: the road moves before the cities build, so the one
        // sent this turn shows under "Turn after" and lands two turns on
        auto round = [&]() {
            for (size_t i = 0; i < game.sides.size(); i++) EndTurn(game);
        };
        Army* sent = nullptr;
        for (int i = 0; i < 10 && !sent; i++) {
            round();
            bool any = false;
            for (auto& entry : side->produced)
                if (entry.kind == "sent") any = true;
            if (!any) continue;
            for (Army* army : game.armies) {
                if (army->transit && army->owner == side->index &&
                    army->transit->dest == dest->index && army->transit->turns == 2)
                    sent = army;
            }
        }
        Ok(sent != nullptr, "a city sends an army that is two turns out");
        if (sent) {
            round();
            Ok(sent->transit && sent->transit->turns == 1,
               "a turn later it is still on the road, due next turn");
            round();
            Ok(!sent->transit && sent->x != kNone, "and the turn after that it has arrived");
        }
    }
    game::SetProduction(game, *from, kNone);
    Ok(from->vector_to == kNone, "vectoring only sticks while the city is building");
    std::vector<City*> senders;
    for (auto& city : game.map->cities) {
        if (&city != dest && senders.size() < 5) {
            city.owner_index = side->index;
            senders.push_back(&city);
        }
    }
    for (int i = 0; i < 4; i++)
        Ok(game::Vector(game, *senders[i], dest),
           "vector " + std::to_string(i + 1) + " is taken");
    Ok(!game::Vector(game, *senders[4], dest), "a fifth is refused");
    Eq(senders[4]->vector_to, kNone, "and the fifth city keeps no vector");
    Eq(static_cast<int>(game::VectoredTo(game, dest).size()), 4,
       "four cities send to the destination");
    Ok(game::Vector(game, *senders[0], dest), "a city already sending there may be set again");
    Ok(game::Vector(game, *senders[0], senders[0]),
       "vectoring a city to itself lifts the vector");
    Eq(senders[0]->vector_to, kNone, "and leaves it with none");
    Eq(game::NearestCity(game, dest->x, dest->y, side), dest, "a click on a city picks it");
    Eq(game::NearestCity(game, dest->x + 1, dest->y - 1, side), dest,
       "and a click next to it too");
}

/** @brief Buying production: price, the bought slot's stats, sorting and saving. */
static void TestBuyProduction(const std::string& scenario) {
    printf("buying production: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(17));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* city = side->capital;
    side->gold = 5000;
    auto list = game::BuyableTypes(game);
    Ok(!list.empty() && list.size() <= 20, "the Build Production screen has room for every type");
    for (auto* army_type : list) Ok(army_type->price >= 0, army_type->name + " is for sale");
    Eq(game::CannotBuy(game, *side, *city, *game.types.ById(city->slots[0].type)), "has it",
       "a type already built is greyed out");
    int slot_count = static_cast<int>(city->slots.size());
    Eq(game::BuySlot(*city), slot_count < 4 ? slot_count : 0, "the starting slot");
    const ArmyType* pick = nullptr;
    for (auto* army_type : list)
        if (game::CannotBuy(game, *side, *city, *army_type).empty() &&
            (!pick || army_type->price > pick->price))
            pick = army_type;
    Ok(pick != nullptr, "something to buy");
    if (!pick) return;
    if (slot_count == 4) {
        city->slots.pop_back();
        slot_count = 3;
    }
    game::SetProduction(game, *city, 0);
    Slot building = city->slots[0];
    int gold = side->gold;
    game::BuyProduction(game, *side, *city, slot_count, pick->id);
    Eq(side->gold, gold - pick->price, "the price comes off the treasury");
    const Slot& slot = city->slots[slot_count];
    Eq(slot.type, pick->id, "the bought type fills the slot");
    Eq(slot.strength, pick->strength, "a bought type takes ARMYTYPE's strength unmodified");
    Eq(slot.time, pick->time, "and its time");
    Eq(slot.move, pick->move, "and its move");
    Eq(city->defence, rules::CityDefence(static_cast<int>(city->slots.size())),
       "defence follows the number of types");
    Eq(city->producing, 0, "buying into a new slot leaves production alone");
    game::SortProduction(*city);
    for (size_t i = 1; i < city->slots.size(); i++)
        Ok(city->slots[i - 1].price <= city->slots[i].price, "sorted by price, cheapest first");
    Ok(city->slots[city->producing] == building, "the city builds the same type after sorting");
    game::Vector(game, *city,
                 &game.map->cities[0] != city ? &game.map->cities[0] : &game.map->cities[1]);
    const ArmyType* other = nullptr;
    for (auto* army_type : list)
        if (!other && game::CannotBuy(game, *side, *city, *army_type).empty()) other = army_type;
    game::BuyProduction(game, *side, *city, city->producing, other->id);
    Eq(city->producing, kNone, "production stops when its type is bought over");
    Eq(city->vector_to, kNone, "and the vector goes with it");
    game::RenameCity(game, *city, "Newtown");
    auto loaded = save::Decode(save::Encode(game), kData);
    Eq(loaded->map->cities[city->index].name, "Newtown", "a renamed city keeps its name");
    Eq(loaded->map->cities[city->index].slots.back().type, city->slots.back().type,
       "bought production survives a save");
}

/** @brief The reports: City, Army, Gold, Winning and Production figures. */
static void TestReports(const std::string& scenario) {
    printf("reports: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(23));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    auto city_figures = report::ComputeFigures(game, *side, report::kCity);
    Eq(city_figures.result, static_cast<int>(game::SideCities(game, *side).size()),
       "the City report counts the side's cities");
    Eq(city_figures.max % 2, 0, "the top of the scale is even");
    int unused = 0;
    for (int i = 0; i < 8; i++)
        if (city_figures.out[i]) unused++;
    Eq(unused, 8 - static_cast<int>(game.sides.size()), "only the sides in the game get a bar");
    auto army_figures = report::ComputeFigures(game, *side, report::kArmy);
    int mine = 0;
    for (Army* army : game.armies)
        if (army->owner == side->index) mine++;
    Eq(army_figures.result, mine, "the Army report counts the side's armies");
    Eq(report::ComputeFigures(game, *side, report::kGold).result, side->gold,
       "the Gold report is the treasury");
    auto winning_figures = report::ComputeFigures(game, *side, report::kWinning);
    Eq(winning_figures.max, 100, "the Winning report's scale is fixed");
    Ok(winning_figures.result >= 0 && winning_figures.result <= 7,
       "and says where the side comes");
    side->gold = 100000;
    Eq(report::ComputeFigures(game, *side, report::kWinning).result, 0,
       "a rich enough side comes first");
    Eq(report::Score(game, *side), 500, "and its score stops at 500");
    City* city = side->capital;
    game::SetProduction(game, *city, 0);
    city->countdown = 1;
    int turns = 0;
    do {
        EndTurn(game);
        turns++;
    } while (!(game.side == side || turns > 20));
    Ok(!side->produced.empty(), "the side's production is logged");
    if (!side->produced.empty()) {
        Eq(side->produced[0].city, city->index, "with the city that built it");
        Eq(side->produced[0].kind, "built", "kept at home");
    }
    Eq(report::ComputeFigures(game, *side, report::kProduction).result,
       static_cast<int>(side->produced.size()), "the Production report counts what was built");
}

/** @brief Disbanding a stack, signs, fight orders across a save, and resigning. */
static void TestDisband(const std::string& scenario) {
    printf("disband: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(29));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* hero = hero::Recruit(game, *side, *side->hero_offer).hero;
    auto stack = game::ArmiesAt(game, hero->x, hero->y);
    hero->strength = 5;
    auto fight = combat::StackStrengths(game, {hero}, hero->x, hero->y);
    Eq(fight[hero], 5 + combat::kHeroTable[5] + 1,
       "View > Stack: a hero's strength, its table value and its standard");
    size_t army_count = game.armies.size();
    side->quest = std::make_shared<Quest>();
    side->quest->type = quest::kOccupy;
    side->quest->hero = hero;
    Item* standard = hero->items[0];
    game::Disband(game, *side, stack);
    Eq(game.armies.size(), army_count - stack.size(), "the stack is gone");
    Eq(static_cast<int>(game::ArmiesAt(game, hero->x, hero->y).size()), 0, "from its tile");
    Eq(standard->status, 1, "the hero's standard lies on the ground");
    Ok(!side->quest, "and the quest is lost with the hero");
    Sign* sign = game::SignAt(game, 18, 12);
    Ok(sign != nullptr, "a sign at (18, 12)");
    if (sign) {
        Eq(sign->lines[0], "Traveller! Pray excuse the ", "its first line");
        Eq(sign->lines[1], "sulphur fumes. Keep On!", "and its second");
        sign->lines[0] = "Hello there";
    }
    auto back = save::Decode(save::Encode(game), kData);
    Eq(game::SignAt(*back, 18, 12)->lines[0], "Hello there", "an edited sign survives a save");
    game.map->fight_order[side->index][3] = 26;
    back = save::Decode(save::Encode(game), kData);
    Eq(back->map->fight_order[side->index][3], 26, "so does an edited fight order");
    int diplo = side->diplo_score;
    game::Resign(game, *side);
    Eq(static_cast<int>(game::SideCities(game, *side).size()), 0,
       "a side that resigns holds no city");
    Eq(static_cast<int>(game::SideArmies(game, *side).size()), 0, "and has no army");
    Ok(side->capital->razed, "its capital is ruins");
    Eq(side->diplo_score, diplo, "burning its own on resigning costs nothing");
}

/** @brief The deed slots, the round's history record, and the hero and standard tallies. */
static void TestHistory(const std::string& scenario) {
    printf("history: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(41));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    history::RecordDeed(game, side, DeedType::kFound, history::kFoundSage, 0, "Hero");
    history::RecordDeed(game, side, DeedType::kCityWon, side->index, 3, side->name);
    history::RecordDeed(game, side, DeedType::kHeroEmerges, 1, 0, "Hero");
    Eq(static_cast<int>(game.deeds[side->index].size()), 2, "two deeds a side");
    std::vector<history::DeedType> types = {game.deeds[side->index][0].type,
                                            game.deeds[side->index][1].type};
    std::sort(types.begin(), types.end());
    Eq(types[0], DeedType::kHeroEmerges, "a lower type pushes out the higher");
    Eq(types[1], DeedType::kCityWon, "and the lower of the two stays");
    history::RecordDeed(game, side, DeedType::kAtPeace, 0, 1, "");
    Eq(static_cast<int>(game.deeds[side->index].size()), 2, "a higher type does not get in");
    int guard = 0;
    while (game.turn < 3 && guard < 50) {
        EndTurn(game);
        guard++;
    }
    Ok(!game.history.empty(), "a record for the round");
    if (!game.history.empty()) {
        const auto& record = game.history[0];
        Eq(record.owners.size(), game.map->cities.size(), "every city's owner");
        Ok(record.events.size() >= 2, "with the deeds");
    }
    Ok(!game.deeds.count(side->index) || game.deeds[side->index].empty(),
       "and the slots cleared");
    auto back = save::Decode(save::Encode(game), kData);
    Eq(back->history.size(), game.history.size(), "history survives a save");
    Side* foe = game.sides[1];
    Item standard;
    standard.index = foe->index;
    Army dead;
    dead.owner = foe->index;
    dead.type = 28;
    dead.items = {&standard};
    history::Tally(game, &dead, side->index);
    Eq(history::Triumph(game, side->index, foe->index, history::Loss::kHeroes), 1,
       "a hero killed");
    Eq(history::Triumph(game, side->index, foe->index, history::Loss::kStandards), 1,
       "and the standard it carried");
    Eq(history::Triumph(game, foe->index, foe->index, history::Loss::kHeroes), 1,
       "the loser counts its own loss");
}

/**
 * @brief The new-game setup: options, sides left out, Quick Start, and that a
 * save keeps all of it.
 */
static void TestSetup() {
    printf("new-game setup\n");
    GameOpts setup = SeedWith(5, {{"quests", 0}});
    setup.sides[0] = SideSetup(false);
    setup.sides[1] = SideSetup(true, 2);
    setup.sides[2] = SideSetup(false, kNone, true);
    auto game_ptr = NewGame("ERYTHEA", setup);
    Game& game = *game_ptr;
    Eq(game.map->options.quests, 0, "the options chosen");
    Ok(!game.map->sides[0].computer, "side 0 human");
    Ok(game.map->sides[1].computer && game.map->sides[1].level == 2, "side 1 a Warlord");
    Ok(!game.map->sides[2].in_use, "side 2 left out");
    City* capital = game.map->sides[2].capital;
    Eq(capital ? capital->owner_index : kNone, kNone, "and its capital neutral");
    for (Side* side : game.sides) Ok(side->index != 2, "not among the sides in play");
    // a save keeps the setup, as the original's keeps the .SCN image it was
    // made in (block 2, docs/formats/save.md): a side Off stays out, and a
    // retyped name stays retyped
    game.map->sides[0].name = "Ozymandias";
    auto back = save::Decode(save::Encode(game), kData);
    Ok(!back->map->sides[2].in_use && !back->map->sides[2].alive,
       "a side Off stays out once loaded");
    Eq(static_cast<int>(back->sides.size()), static_cast<int>(game.sides.size()),
       "as many sides in play once loaded");
    for (Side* side : back->sides) Ok(side->index != 2, "the side Off not among them");
    Eq(back->map->sides[0].name, std::string("Ozymandias"), "a retyped name kept once loaded");
    Eq(back->map->sides[1].level, 2, "and a computer's level");
    // and the rest of the setup: every option, a computer's character, I am
    // the Greatest, and what the hidden map has uncovered
    std::vector<std::pair<std::string, int>> options = {
        {"neutralCities", 2},   {"diplomacy", 0},      {"quests", 0},        {"hiddenMap", 1},
        {"viewEnemies", 0},     {"viewProduction", 1}, {"intenseCombat", 1}, {"quickStart", 1},
        {"militaryAdvisor", 0}, {"randomTurns", 1}};
    GameOpts full_setup = SeedWith(5, options);
    full_setup.greatest = true;
    full_setup.sides[1] = SideSetup(true, 1);
    full_setup.sides[1].card = 3;
    auto full = NewGame("ERYTHEA", full_setup);
    game::Reveal(*full, 0, 50, 80, true);
    full->surrender_offered = true;
    auto loaded = save::Decode(save::Encode(*full), kData);
    Ok(game::SurrenderPending(*loaded), "a surrender not yet put to the player kept once loaded");
    for (auto& [name, value] : options)
        Eq(*loaded->map->options.Field(name), value, "option " + name + " kept once loaded");
    Ok(loaded->greatest, "I am the Greatest kept once loaded");
    Eq(loaded->map->sides[1].card, 3, "a computer's character kept once loaded");
    Eq(loaded->map->sides[1].level, 1, "and its level");
    Ok(loaded->map->sides[1].computer, "and that it is a computer");
    Ok(game::Seen(*loaded, 0, 50, 80), "the hidden map's uncovered tiles kept once loaded");
    Ok(!game::Seen(*loaded, 0, 90, 20), "and the covered ones covered");
    GameOpts quick_setup = SeedWith(5, {{"quickStart", 1}});
    quick_setup.sides[2] = SideSetup(false, kNone, true);
    auto quick = NewGame("ERYTHEA", quick_setup);
    std::map<int, int> counts;
    int neutral = 0;
    for (auto& city : quick->map->cities) {
        if (city.owner_index == kNone)
            neutral++;
        else
            counts[city.owner_index]++;
    }
    Eq(neutral, 0, "with Quick Start no city is left neutral");
    Ok(!counts.count(2), "a side left out gets none");
    int least = 1 << 30, most = 0;
    for (Side* side : quick->sides) {
        least = std::min(least, counts[side->index]);
        most = std::max(most, counts[side->index]);
        Ok(side->capital->owner_index == side->index, "each side keeps its capital");
    }
    Ok(most - least <= 1, "and the cities go round evenly");
    for (Army* army : quick->armies) {
        City* city = game::CityAt(*quick, army->x, army->y);
        if (city) Eq(army->owner, city->owner_index, "a dealt city's garrison is its owner's");
    }
    GameOpts slow_setup = Seed(5);
    slow_setup.sides[2] = SideSetup(false, kNone, true);
    auto slow = NewGame("ERYTHEA", slow_setup);
    neutral = 0;
    for (auto& city : slow->map->cities)
        if (city.owner_index == kNone) neutral++;
    Ok(neutral > 0, "without it the other cities stay neutral");
}

/** @brief The sage: rich sites in reach, gold shown nearest first, the gem and the map patch. */
static void TestSage(const std::string& scenario) {
    printf("sage: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(31));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    int hero_x = side->capital->x, hero_y = side->capital->y;
    for (auto& map_site : game.map->sites) {
        map_site.rich = false;
        map_site.revealed = 0xff;
        map_site.searched = false;
    }
    Eq(static_cast<int>(site::SageList(game, *side, hero_x, hero_y).size()), 0,
       "no rich site unshown: nothing to tell");
    auto distance_squared = [&](const Site* target) {
        return (target->x - hero_x) * (target->x - hero_x) +
               (target->y - hero_y) * (target->y - hero_y);
    };
    std::vector<Site*> near;
    for (auto& map_site : game.map->sites)
        if (std::sqrt(static_cast<double>(distance_squared(&map_site))) < site::kSageRange)
            near.push_back(&map_site);
    std::stable_sort(near.begin(), near.end(), [&](Site* left, Site* right) {
        return distance_squared(left) < distance_squared(right);
    });
    Ok(near.size() >= 3, "three sites within reach");
    if (near.size() < 3) return;
    Site* far_gold = near[2];
    Site* near_gold = near[0];
    Site* item_site = near[1];
    for (Site* rich_site : {far_gold, near_gold, item_site}) {
        rich_site->rich = true;
        rich_site->revealed = 0;
    }
    far_gold->content = site::kGold;
    near_gold->content = site::kGold;
    item_site->content = site::kItem;
    item_site->item = game.map->items[9].index;
    auto list = site::SageList(game, *side, hero_x, hero_y);
    Eq(static_cast<int>(list.size()), 2, "gold once however many, and the item");
    const site::SageEntry* gold = nullptr;
    for (auto& entry : list)
        if (entry.kind == "gold") gold = &entry;
    Ok(gold != nullptr, "a gold entry");
    if (!gold) return;
    site::SageEntry gold_entry = *gold;
    Eq(site::SageShow(game, *side, gold_entry, hero_x, hero_y), near_gold,
       "the nearest gold site is shown");
    Ok(((near_gold->revealed >> side->index) & 1) == 1, "and marked shown to the side");
    Eq(site::SageShow(game, *side, gold_entry, hero_x, hero_y), far_gold, "then the next one");
    auto items = site::SageList(game, *side, hero_x, hero_y);
    Eq(static_cast<int>(items.size()), 1, "only the item is left");
    if (!items.empty()) Eq(items[0].item, &game.map->items[9], "by its record");
    int before = side->gold;
    int gem = site::SageGem(game, *side);
    Ok(gem >= 503 && gem <= 2000, "a gem is 3d500 + 500");
    Eq(side->gold, before + gem, "and is paid");
    game.map->options.hidden_map = 1;
    game.explored.clear();
    auto patch = site::SageMap(game, *side, 3, 3);
    int patch_x = patch[0], patch_y = patch[1], patch_width = patch[2], patch_height = patch[3];
    Eq(patch_x, 0, "the patch is kept on the map");
    Ok(patch_width >= 16 && patch_width <= 25 && patch_height >= 16 && patch_height <= 25,
       "16-25 tiles a side");
    Ok(game::Seen(game, side->index, patch_x + patch_width - 1, patch_y + patch_height - 1),
       "and uncovered");
}

/** @brief A hero's items: the standard, taking and dropping, planting the flag and vectoring to it. */
static void TestHeroItems(const std::string& scenario) {
    printf("hero items: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(29));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* hero = hero::Recruit(game, *side, *side->hero_offer).hero;
    Eq(hero->items[0], &game.map->items[side->index],
       "the first hero carries the side's own standard record");
    Eq(hero->items[0]->status, 3, "which is carried");
    Eq(static_cast<int>(hero::ItemsHere(game, hero).size()), 1, "nothing else is listed yet");
    Item* item = &game.map->items[8];
    item->status = 1;
    item->x = hero->x;
    item->y = hero->y;
    auto list = hero::ItemsHere(game, hero);
    Eq(static_cast<int>(list.size()), 2, "an item on the hero's tile is listed");
    if (list.size() > 1) Eq(list[1], item, "after what the hero carries");
    hero::TakeItem(game, hero, item);
    Eq(item->status, 3, "taking an item carries it");
    Eq(static_cast<int>(hero->items.size()), 2, "the hero has it");
    hero::DropItem(game, hero, item);
    Eq(item->status, 1, "dropping puts it on the ground");
    Eq(item->x, hero->x, "where the hero stands");
    Eq(static_cast<int>(hero->items.size()), 1, "and the hero no longer has it");
    Ok(!game::PlantFlag(game, *side, hero), "no flag in a city");
    int flag_x = kNone, flag_y = kNone;
    for (int dy = -3; dy <= 4; dy++) {
        for (int dx = -3; dx <= 4; dx++) {
            int x = hero->x + dx, y = hero->y + dy;
            int terrain = scn::TerrainAt(*game.map, x, y);
            if (flag_x == kNone && (terrain == movement::kPlain || terrain == movement::kRoad) &&
                !game::CityAt(game, x, y)) {
                flag_x = x;
                flag_y = y;
            }
        }
    }
    Ok(flag_x != kNone, "found open ground near the capital");
    hero->x = flag_x;
    hero->y = flag_y;
    Ok(game::PlantFlag(game, *side, hero), "the flag is planted");
    Eq(game::StandardAt(game, *side).first, flag_x, "where the hero stands");
    Ok(!game::PlantFlag(game, *side, hero), "and only once");
    City* city = side->capital;
    game::SetProduction(game, *city, 0);
    city->countdown = 1;
    Ok(game::VectorToStandard(game, *city, *side), "a city may vector to the standard");
    size_t before = game::ArmiesAt(game, flag_x, flag_y).size();
    for (size_t i = 0; i < 3 * game.sides.size() + 1; i++) EndTurn(game);
    Ok(game::ArmiesAt(game, flag_x, flag_y).size() > before,
       "a vectored army arrives at the standard");
    hero::TakeItem(game, hero, &game.map->items[side->index]);
    Eq(game::StandardAt(game, *side).first, kNone, "a standard picked up is no longer planted");
}

/**
 * @brief Movement: terrain costs, step costs by kind of mover, a stack's pace,
 * walking, attacking a city, and the route preview.
 */
static void TestMovement(const std::string& scenario) {
    printf("movement: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(11));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    std::vector<Army*> stack = {army};
    Eq(movement::kCost[movement::kMountains], 0, "mountains are impassable");
    Eq(movement::kCost[movement::kRoad], 1, "a road costs 1");
    Eq(movement::kCost[movement::kHills], 6, "hills cost 6");
    auto& grid = movement::Grid(game, side->index);
    int roaded = 0;
    bool road_cost_ok = true;
    for (int y = 0; y < game.map->height; y++) {
        for (int x = 0; x < game.map->width; x++) {
            if (scn::RoadAt(*game.map, x, y) % 0x20 != 0) {
                roaded++;
                if (grid[y * game.map->width + x] % 8 != 1) road_cost_ok = false;
            }
        }
    }
    Ok(roaded > 0, "the scenario has roads");
    Ok(road_cost_ok, "every road tile costs 1");
    const int water_flag = movement::kWaterF;
    const int land = 2, water = 2 | water_flag;
    // A step that cannot be taken shows as kNone.
    auto unwrap_cost = [](std::optional<int> cost) { return cost ? *cost : kNone; };
    Eq(unwrap_cost(movement::StepCost(land, water, movement::kLand, false, false, 10)), kNone,
       "a land stack cannot enter open water");
    Eq(unwrap_cost(movement::StepCost(land, 0, movement::kLand, false, false, 10)), kNone,
       "nothing enters a cost-0 tile on land");
    Eq(unwrap_cost(
           movement::StepCost(land, 6 | movement::kHillsF, movement::kLand, false, false, 10)),
       6, "hills cost 6 without the bonus");
    Eq(unwrap_cost(
           movement::StepCost(land, 6 | movement::kHillsF, movement::kLand, false, true, 10)),
       2, "the hills bonus brings them to 2");
    Eq(unwrap_cost(
           movement::StepCost(land, 4 | movement::kForestF, movement::kLand, true, false, 10)),
       2, "the woods bonus brings forest to 2");
    const int cross = 1 | water_flag | movement::kCrossF;
    Eq(unwrap_cost(movement::StepCost(land, cross, movement::kLand, false, false, 10)), 1,
       "a crossing tile is free of the water charge");
    Eq(unwrap_cost(movement::StepCost(cross, water, movement::kLand, false, false, 10)), 2 + 10,
       "stepping from a crossing into water costs the penalty");
    Eq(unwrap_cost(movement::StepCost(cross, water, movement::kLand, false, false, 20)), 2 + 20,
       "a move aimed at water pays 20");
    Eq(unwrap_cost(movement::StepCost(land, 0, movement::kFlying, false, false, 10)), 2,
       "a flier crosses mountains at 2");
    Eq(unwrap_cost(movement::StepCost(land, water, movement::kFlying, false, false, 10)), 2,
       "a flier crosses water at 2");
    Eq(unwrap_cost(movement::StepCost(land, 1, movement::kFlying, false, false, 10)), 1,
       "a flier pays 1 on a road");
    Eq(unwrap_cost(
           movement::StepCost(land, 6 | movement::kHillsF, movement::kFlying, false, false, 10)),
       2, "a flier pays 2 over hills");
    Eq(unwrap_cost(
           movement::StepCost(land, 0 + movement::kCityF, movement::kFlying, false, false, 10)),
       kNone, "a flier cannot cross a city that is not its own (1555:08bf)");
    Eq(movement::Distance(5, 5, 6, 6), 1, "a diagonal neighbour is one away");
    Eq(movement::Distance(5, 5, 7, 6), 2, "a knight's move is two");
    Eq(movement::Direction(5, 5, 6, 4), 1, "up and right is north-east");
    Eq(movement::Direction(5, 5, 9, 4), 1, "however far right");
    Eq(movement::Direction(5, 5, 5, 9), 4, "straight down is south");
    Eq(unwrap_cost(movement::StepCost(water, land, movement::kBoat, false, false, 10)), kNone,
       "a boat cannot go ashore");
    Eq(unwrap_cost(movement::StepCost(water, water, movement::kBoat, false, false, 10)), 2,
       "a boat moves on water");
    army->moves = 7;
    Army slow_army, fast_army;
    slow_army.moves = 3;
    fast_army.moves = 9;
    Eq(movement::StackMoves({army, &slow_army, &fast_army}), 3,
       "a stack moves at the pace of its slowest army");

    army->moves = army->max_moves;
    int from_x = army->x, from_y = army->y;
    bool found = false;
    int target_x = 0, target_y = 0;
    for (int dx = -6; dx <= 6; dx++) {
        for (int dy = -6; dy <= 6; dy++) {
            int x = from_x + dx, y = from_y + dy;
            if (!found && x >= 0 && y >= 0 && x < game.map->width && y < game.map->height &&
                !game::CityAt(game, x, y)) {
                auto found_path = movement::FindPath(game, stack, from_x, from_y, x, y);
                if (found_path && found_path->size() >= 2 &&
                    found_path->back().cost < movement::kPastShore &&
                    !movement::Has(movement::Grid(game, side->index)[y * game.map->width + x],
                                   movement::kWaterF)) {
                    found = true;
                    target_x = x;
                    target_y = y;
                }
            }
        }
    }
    Ok(found, "found somewhere to walk to");
    if (found) {
        int before = army->moves;
        auto result = movement::MoveTo(game, stack, target_x, target_y);
        Ok(result.steps > 0, "the stack moved");
        Eq(army->moves, std::max(0, before - result.spent), "the cost was charged to the army");
        Ok(army->x != from_x || army->y != from_y, "the army is somewhere else");
    }
    army->moves = 0;
    auto move_result = movement::MoveTo(game, stack, from_x, from_y + 1);
    Eq(move_result.steps, 0, "an army with no moves stays put");
    City* enemy = nullptr;
    for (auto& city : game.map->cities)
        if (!enemy && city.owner_index != side->index) enemy = &city;
    Ok(enemy != nullptr, "there is a city to attack");
    if (enemy) {
        army->moves = 99;
        army->x = enemy->x;
        army->y = enemy->y - 1;
        auto path = movement::FindPath(game, stack, army->x, army->y, enemy->x, enemy->y);
        if (path && !path->empty()) {
            auto walk = movement::Walk(game, stack, *path);
            Eq(walk.stopped, "attack", "stepping into an enemy city starts an attack");
            Ok(walk.attack && walk.attack->city == enemy, "the attack names the city");
            Eq(army->x, enemy->x, "the attacker has not entered the city");
            Ok(army->y == enemy->y - 1, "the attacker stayed where it was");
        }
        auto& grid2 = movement::Grid(game, side->index);
        Eq(grid2[enemy->y * game.map->width + enemy->x] % 8, 0,
           "an enemy city is impassable in the cost grid");
    }
    {
        Army* scout = AddArmy(game, army->x, army->y, side->index, 11, 9, 20, 20);
        scout->upkeep = 1;
        int far_x = 0, far_y = 0;
        std::optional<movement::Preview> route;
        for (int distance = 12; distance <= 30; distance += 2) {
            far_x = std::min(game.map->width - 2, scout->x + distance);
            far_y = scout->y;
            route = movement::PreviewRoute(game, {scout}, far_x, far_y);
            if (route && route->reach < static_cast<int>(route->path.size())) break;
        }
        if (route) {
            Ok(!route->path.empty(), "a stack under orders has a route to draw");
            Ok(route->reach <= static_cast<int>(route->path.size()),
               "it can walk no more of it than there is");
            Eq(route->path.back().x, far_x, "the route ends where it was sent");
            Eq(route->path.back().y, far_y, "on that row");
            int prev_x = scout->x, prev_y = scout->y;
            bool straight = true;
            for (auto& step : route->path) {
                if (std::max(std::abs(step.x - prev_x), std::abs(step.y - prev_y)) != 1)
                    straight = false;
                prev_x = step.x;
                prev_y = step.y;
            }
            Ok(straight, "and it is a chain of single steps");
            move_result = movement::MoveTo(game, {scout}, far_x, far_y);
            Eq(static_cast<int>(move_result.walked.size()), move_result.steps,
               "a walk reports the tiles it covered, one per step taken");
            if (move_result.steps > 0) {
                Eq(move_result.walked.back().first, scout->x, "ending where the stack now stands");
                Eq(move_result.walked.back().second, scout->y, "on that row");
            }
            auto left = movement::PreviewRoute(game, {scout}, far_x, far_y);
            if (left)
                Ok(left->reach < static_cast<int>(left->path.size()),
                   "what is left of it is out of reach this turn");
        }
        game.Remove(scout);
    }
}

/** @brief A stack cannot stop on a tile that already holds a full stack. */
static void TestStackLimit(const std::string& scenario) {
    printf("stack limit: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(13));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    int target_x = army->x, target_y = army->y - 1;
    if (target_y < 0) {
        target_x = army->x;
        target_y = army->y + 1;
    }
    for (int i = 0; i < rules::kMaxStack; i++) {
        Army* filler = AddArmy(game, target_x, target_y, side->index, army->type, 1, 10, 10);
        filler->home_city = army->home_city;
    }
    Eq(static_cast<int>(game::ArmiesAt(game, target_x, target_y).size()), rules::kMaxStack,
       "the tile is full");
    army->moves = 99;
    auto move_result = movement::MoveTo(game, {army}, target_x, target_y);
    Eq(move_result.steps, 0, "a stack cannot stop on a full tile");
    Ok(army->x != target_x || army->y != target_y, "the army stayed off the full tile");
}

/**
 * @brief Combat: terrain classes, fortification, strength modifiers, battle
 * lines, resolving a battle, the odds, and the Military Advisor.
 */
static void TestCombat(const std::string& scenario) {
    printf("combat: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(17));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Eq(combat::kHeroTable[0], 0, "a strength-0 hero commands nothing");
    Eq(combat::kHeroTable[4], 1, "strength 4 gives +1");
    Eq(combat::kHeroTable[9], 3, "strength 9 gives +3");
    Eq(combat::kCity, 0, "city is class 0");
    Eq(combat::kOpen, 1, "open is class 1");
    Eq(combat::kWoods, 2, "woods is class 2");
    Eq(combat::kHills, 3, "hills is class 3");
    std::set<int> seen;
    for (int y = 0; y < game.map->height; y += 7) {
        for (int x = 0; x < game.map->width; x += 7) {
            int cls = combat::TerrainClass(game, x, y);
            int terrain = scn::TerrainAt(*game.map, x, y);
            seen.insert(cls);
            if (terrain == movement::kForest) Eq(cls, combat::kWoods, "forest fights as woods");
            if (terrain == movement::kHills || terrain == movement::kMountains)
                Eq(cls, combat::kHills, "hills and mountains fight as hills");
            if (terrain == movement::kCity || terrain == movement::kSite)
                Eq(cls, combat::kCity, "cities and sites fight as city");
            if (terrain == movement::kPlain || terrain == movement::kMarsh ||
                terrain == movement::kWater)
                Eq(cls, combat::kOpen, "plain, marsh and water fight as open");
        }
    }
    Ok(seen.count(combat::kOpen), "the map has open ground");
    Eq(game.map->combat_cap, 5, "the combat cap is 5");
    const auto& row = game.map->fight_order[0];
    std::set<int> used;
    for (int type = 0; type <= 28; type++) used.insert(row[type]);
    Eq(static_cast<int>(used.size()), 29, "fight order is a permutation of 29 ranks");
    City* neutral = nullptr;
    for (auto& city : game.map->cities)
        if (!neutral && city.owner_index == kNone && city.defence == 2) neutral = &city;
    if (neutral) {
        int cls = combat::TerrainClass(game, neutral->x, neutral->y);
        Eq(combat::Fortify(game, {}, neutral->x, neutral->y, cls), 1,
           "a neutral city's defence 2 fortifies for 1");
        game::SetCityOwner(game, *neutral, game.sides.back()->index);
        Eq(combat::Fortify(game, {}, neutral->x, neutral->y, cls), 2,
           "an owned city's defence 2 fortifies for 2");
        game::SetCityOwner(game, *neutral, kNone);
    }
    const ArmyType* siege = nullptr;
    for (auto& army_type : game.types.all)
        if (army_type.bonus[52] == combat::kSiege) siege = &army_type;
    if (siege && neutral) {
        int cls = combat::TerrainClass(game, neutral->x, neutral->y);
        Army siege_army;
        siege_army.type = siege->id;
        Eq(combat::Fortify(game, {&siege_army}, neutral->x, neutral->y, cls), 0,
           "a Siege attacker cancels the city bonus");
    }
    Army boat;
    boat.type = 11;
    boat.strength = 9;
    boat.at_sea = true;
    Eq(combat::Strength(game, boat, 5, combat::kOpen, movement::kWater), 4,
       "an army at sea on water fights at 4");
    Eq(combat::Strength(game, boat, 5, combat::kOpen, movement::kShore), 4,
       "an army at sea on shore fights at 4");
    boat.at_sea = false;
    Ok(combat::Strength(game, boat, 5, combat::kOpen, movement::kPlain) > 4,
       "ashore it fights normally");
    Army strong;
    strong.type = 11;
    strong.strength = 9;
    Eq(combat::Strength(game, strong, 99, combat::kOpen, movement::kPlain), 15,
       "strength is capped at 15");
    Army* lone_attacker = game::SideArmies(game, *side)[0];
    City* enemy = nullptr;
    for (auto& city : game.map->cities)
        if (!enemy && city.owner_index != side->index) enemy = &city;
    auto lines = combat::BattleLines(game, {lone_attacker}, enemy->x, enemy->y);
    Eq(static_cast<int>(lines.attackers.size()), 1, "the attacking line is the stack");
    Ok(!lines.defenders.empty(), "the city has defenders");
    Eq(lines.city, enemy, "the battle names the city");
    for (Army* defender : lines.defenders)
        Ok(defender->owner != side->index, "no defender belongs to the attacker");
    bool sorted = true;
    for (size_t i = 1; i < lines.defenders.size(); i++) {
        const auto& owner_row =
            game.map->fight_order[lines.def_owner == kNone ? 8 : lines.def_owner];
        if (owner_row[lines.defenders[i]->type] < owner_row[lines.defenders[i - 1]->type])
            sorted = false;
    }
    Ok(sorted, "the defending line is sorted by fight order");
    auto battle = combat::Resolve(game, lines.attackers, lines.defenders, enemy->x, enemy->y);
    Ok(!battle.log.empty(), "the battle logged at least one death");
    Eq(battle.log.size(), battle.dead_attackers.size() + battle.dead_defenders.size(),
       "one log entry per dead army");
    Ok(battle.won == battle.defenders.empty(), "the attacker wins only if every defender is dead");
    // How many of 200 battles the attackers win, n_atk of them against n_def.
    auto odds = [&](int atk_strength, int n_atk, int def_strength, int n_def) {
        std::vector<Army> attackers(n_atk), defenders(n_def);
        std::vector<Army*> attacker_ptrs, defender_ptrs;
        for (auto& attacker : attackers) {
            attacker.type = 11;
            attacker.strength = atk_strength;
            attacker.owner = side->index;
            attacker_ptrs.push_back(&attacker);
        }
        for (auto& defender : defenders) {
            defender.type = 11;
            defender.strength = def_strength;
            defender.owner = kNone;
            defender_ptrs.push_back(&defender);
        }
        int wins = 0;
        for (int i = 0; i < 200; i++)
            if (combat::Resolve(game, attacker_ptrs, defender_ptrs, 0, 0).won) wins++;
        return wins;
    };
    Ok(odds(9, 8, 1, 1) > 190, "eight strong armies beat one weak one");
    Ok(odds(1, 1, 9, 8) < 10, "one weak army loses to eight strong ones");
    int many = odds(3, 8, 9, 1);
    Ok(many > 150, "many weak armies beat one strong one: " + std::to_string(many));
    Eq(combat::kAdviceBattles, 19, "the advisor fights 19 battles");
    auto [verdict, wins] =
        combat::Advise(game, lines.attackers, lines.defenders, enemy->x, enemy->y);
    Ok(!verdict.empty(), "the advisor has a verdict");
    Ok(wins >= 0 && wins <= 19, "the advisor's win count is in range");
    Eq(verdict, std::string(combat::kAdvice[wins / 2]), "the verdict is wins/2 into the table");

    // The Military Advisor evaluates any of the 4 tiles of a 2x2 city consistently
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            auto l_city = combat::BattleLines(game, {lone_attacker}, enemy->x + dx, enemy->y + dy);
            Eq(l_city.city, enemy, "combat lines identify the 2x2 city from any tile");
            Eq(static_cast<int>(l_city.defenders.size()),
               static_cast<int>(lines.defenders.size()),
               "defenders are gathered across all 4 tiles");
        }
    }
    Eq(game.map->options.military_advisor, 1, "the military advisor is enabled by default");
}

/**
 * @brief Capturing a city: walking at it ends in an assault, the attack is
 * decided and then applied once, and the loser's vectoring and gold follow.
 */
static void TestCapture(const std::string& scenario) {
    printf("capture: %s\n", scenario.c_str());
    auto game_ptr = NewGame(scenario, Seed(23));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* target = nullptr;
    for (auto& city : game.map->cities)
        if (!target && city.owner_index == kNone) target = &city;
    Ok(target != nullptr, "found a neutral city");
    std::vector<Army*> stack;
    for (int i = 0; i < 8; i++) {
        Army* attacker = AddArmy(game, target->x, target->y - 1, side->index, 11, 9, 20, 20);
        attacker->upkeep = 1;
        stack.push_back(attacker);
    }
    {
        Army* far = AddArmy(game, 0, 0, side->index, 11, 9, 20, 20);
        far->upkeep = 1;
        movement::WalkResult walk;
        for (auto offset : std::vector<std::pair<int, int>>{
                 {-3, -3}, {3, -3}, {-3, 3}, {3, 3}, {0, -3}, {0, 3}, {-3, 0}, {3, 0}}) {
            far->x = target->x + offset.first;
            far->y = target->y + offset.second;
            far->moves = 20;
            walk = movement::MoveTo(game, {far}, target->x, target->y);
            if (walk.stopped == "attack") break;
        }
        Eq(walk.stopped, "attack", "walking at a city ends in an assault");
        if (walk.attack) {
            Eq(walk.attack->x, target->x, "on the city's own tile");
            Eq(walk.attack->y, target->y, "and row");
            Eq(walk.attack->city, target, "which is the city");
        }
        Ok(std::max(std::abs(far->x - target->x), std::abs(far->y - target->y)) == 1,
           "and the stack is left standing beside it, not in it");
        Ok(far->x != target->x || far->y != target->y, "it did not walk in");
        Eq(target->owner_index, kNone, "and the city is still the defender's");
        game.Remove(far);
    }
    int before = static_cast<int>(game::SideCities(game, *side).size());
    // a city of the loser's vectoring here stops building; the target is
    // neutral, so stand one in for the purpose
    City* sender = nullptr;
    for (auto& city : game.map->cities)
        if (!sender && &city != target && !city.slots.empty()) sender = &city;
    sender->producing = 0;
    sender->countdown = 2;
    sender->vector_to = target->index;
    Battle battle;
    {
        int tile_index = target->y * kMapW + target->x;
        int tile = game.map->tiles[tile_index];
        size_t armies = game.armies.size();
        int from = stack[0]->x;
        battle = game::DecideAttack(game, stack, target->x, target->y);
        Ok(battle.won, "the fight is decided");
        Eq(target->owner_index, kNone, "but the city is not yet taken");
        Eq(game.map->tiles[tile_index], tile, "nor its castle repainted");
        Eq(game.armies.size(), armies, "nor the dead removed");
        Eq(stack[0]->x, from, "nor the survivors moved in");
        Eq(battle.captured, static_cast<City*>(nullptr), "and nothing is said to be captured");
        game::ApplyAttack(game, battle);
        Ok(game.map->tiles[tile_index] != tile, "applied, the castle is in the victor's colours");
        int owner = target->owner_index;
        size_t army_count = game.armies.size();
        game::ApplyAttack(game, battle);
        Eq(game.armies.size(), army_count, "and applying again changes nothing");
        Eq(target->owner_index, owner, "and the owner stays");
    }
    Ok(battle.won, "the overwhelming stack took the city");
    Eq(sender->vector_to, kNone, "a city that sent its armies to the captured one stops sending");
    Eq(sender->producing, kNone, "and stops building");
    Eq(battle.lines.attackers.size(), stack.size(), "every attacker is in the line");
    Eq(battle.lines.city, target, "and the line knows it was a city");
    size_t down = 0;
    for (int loser : battle.log) {
        Ok(loser == 0 || loser == 1, "the log says which side lost an army");
        down++;
    }
    Eq(down, battle.dead_attackers.size() + battle.dead_defenders.size(),
       "the log has one entry per army that fell");
    Eq(target->owner_index, side->index, "the city changed hands");
    Eq(static_cast<int>(game::SideCities(game, *side).size()), before + 1,
       "the side owns one more city");
    Eq(target->producing, kNone, "a captured city is not building anything");
    for (Army* survivor : battle.attackers) Eq(survivor->x, target->x, "the survivors moved in");
    for (Army* dead : battle.dead_defenders) Ok(!game.Alive(dead), "a dead defender is removed");
    Side* victim = game.sides.back();
    victim->gold = 400;
    Eq(game::Loot(game, victim), 200, "one city: half its gold");
    City* extra = nullptr;
    for (auto& city : game.map->cities)
        if (!extra && city.owner_index == kNone && &city != target) extra = &city;
    if (extra) {
        game::SetCityOwner(game, *extra, victim->index);
        Eq(game::Loot(game, victim), (400 / 2) / 2, "two cities: half the per-city share");
    }
}

static void TestTutorialHero() {
    printf("tutorial: a hero cannot die attacking neutrals\n");
    auto gp = NewGame("TUTORIA", Seed(31));
    Game& g = *gp;
    Side* side = game::Begin(g);
    Eq(g.map->options.tutorial, 1, "TUTORIA sets the tutorial flag");
    Ok(!side->computer, "the tutorial player is human");
    Army h;
    h.type = armytype::kHero;
    h.strength = 1;
    h.owner = side->index;
    std::vector<Army> defender_pool(8);
    std::vector<Army*> defenders;
    for (auto& d : defender_pool) {
        d.type = 11;
        d.strength = 9;
        d.owner = kNone;
        defenders.push_back(&d);
    }
    int deaths = 0;
    for (int i = 0; i < 50; i++)
        deaths += static_cast<int>(combat::Resolve(g, {&h}, defenders, 0, 0).dead_attackers.size());
    Eq(deaths, 0, "the tutorial hero never dies against neutrals");
    g.map->options.tutorial = 0;
    deaths = 0;
    for (int i = 0; i < 50; i++)
        deaths += static_cast<int>(combat::Resolve(g, {&h}, defenders, 0, 0).dead_attackers.size());
    Ok(deaths > 0, "without the tutorial flag the hero can die");
}

static void TestCityChoices(const std::string& scenario) {
    printf("pillage, sack and raze: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(29));
    Game& g = *gp;
    Side* side = game::Begin(g);
    auto captured = [&](size_t want) -> City* {
        for (auto& c : g.map->cities) {
            if (c.owner_index == kNone && c.slots.size() >= want) {
                game::SetCityOwner(g, c, side->index);
                return &c;
            }
        }
        return nullptr;
    };
    City* city = captured(2);
    Ok(city != nullptr, "found a city with something to pillage");
    if (city) {
        size_t n = city->slots.size();
        Slot dear = city->slots.back();
        int want = std::abs(g.types.ById(dear.type)->price) / 2;
        int gold0 = side->gold, score0 = side->diplo_score;
        auto got = game::Pillage(g, *side, *city, {});
        Eq(got.gold, want, "pillage pays half the type's purchase price");
        Eq(side->gold, gold0 + want, "the gold was paid");
        Eq(city->slots.size(), n - 1, "pillage removed one type");
        Eq(city->defence, rules::CityDefence(static_cast<int>(city->slots.size())),
           "defence was recomputed");
        int d = side->diplo_score - score0;
        Ok(d >= 1 && d <= 5, "pillage costs 1d5 diplomatic score: " + std::to_string(d));
    }
    City* city2 = captured(3);
    if (city2) {
        Slot cheapest = city2->slots[0];
        int want = 0;
        for (size_t i = 1; i < city2->slots.size(); i++)
            want += std::abs(g.types.ById(city2->slots[i].type)->price) / 2;
        int gold0 = side->gold, score0 = side->diplo_score;
        size_t n2 = city2->slots.size();
        auto got = game::Sack(g, *side, *city2, {});
        Eq(got.gold, want, "sack pays for every type it strips");
        Eq(got.lost.size(), n2 - 1, "sack lists every type it strips");
        int sum = 0;
        for (auto& l : got.lost) sum += l.second;
        Eq(sum, want, "and what each was worth adds up to the gold");
        Eq(side->gold, gold0 + want, "the gold was paid");
        Eq(static_cast<int>(city2->slots.size()), 1, "only the cheapest type is left");
        Ok(city2->slots[0] == cheapest, "and it is the cheapest one");
        Eq(city2->defence, 1, "one type means defence 1");
        int d = side->diplo_score - score0;
        Ok(d >= 6 && d <= 15, "sack costs 1d10+5 diplomatic score: " + std::to_string(d));
    }
    City* city3 = captured(1);
    if (city3) {
        int before = static_cast<int>(game::SideCities(g, *side).size());
        City* other = nullptr;
        for (auto& c : g.map->cities)
            if (!other && c.owner_index == side->index && &c != city3) other = &c;
        if (other) game::Vector(g, *other, city3);
        int score0 = side->diplo_score;
        game::Raze(g, *side, *city3, {});
        Eq(city3->owner_index, kNone, "a razed city is neutral");
        Eq(static_cast<int>(city3->slots.size()), 0, "a razed city produces nothing");
        Eq(city3->income, 0, "a razed city earns nothing");
        Eq(static_cast<int>(game::SideCities(g, *side).size()), before - 1,
           "the side no longer owns it");
        if (other) Eq(other->vector_to, kNone, "vectoring to it was cancelled");
        int d = side->diplo_score - score0;
        Ok(d >= 11 && d <= 25, "raze costs 1d15+10 diplomatic score: " + std::to_string(d));
        int rb = movement::Grid(g, side->index)[city3->y * g.map->width + city3->x];
        Ok(!movement::Has(rb, movement::kCityF) && rb % 8 != 0,
           "a razed city's ruins can be walked over: " + std::to_string(rb));
    }
    City empty;
    empty.index = -1;
    Eq(game::Pillage(g, *side, empty, {}).gold, 0, "pillaging an empty city pays nothing");
    City one;
    one.index = -1;
    one.slots.push_back(Slot{11, "", 0, 0, 0, 0, 0});
    Eq(game::Sack(g, *side, one, {}).gold, 0, "sacking a one-type city pays nothing");
}

static void TestHeroes(const std::string& scenario) {
    printf("heroes: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(37));
    Game& g = *gp;
    Side* side = game::Begin(g);
    auto offer = side->hero_offer;
    Ok(offer != nullptr, "a hero offers itself on turn 1");
    if (!offer) return;
    Eq(offer->price, 0, "the first hero is free");
    Eq(offer->city, side->capital, "the first hero appears at the capital");
    const auto& names = hero::Names(g, *side);
    Eq(static_cast<int>(names.size()), 100, "a side has a hundred candidate heroes");
    Ok(!offer->name.empty(), "the offer carries a name");
    bool known = false;
    for (auto& n : names)
        if (n.first == offer->name) known = true;
    Ok(known, "and the name comes from that side's file");
    bool heroine = false;
    for (auto& n : hero::Names(g, g.map->sides[0]))
        if (n.second) heroine = true;
    Ok(!heroine, "side 0 has no heroines to offer");
    auto hired = hero::Recruit(g, *side, *offer);
    Army* h = hired.hero;
    auto& allies = hired.allies;
    Eq(h->type, armytype::kHero, "the recruit is a hero");
    Eq(h->name, offer->name, "the hero keeps the name it was offered under");
    Eq(h->strength, 5, "a new hero has strength 5");
    Eq(h->max_moves, 14, "a new hero has 14 movement");
    Eq(h->moves, 14, "a new hero joins with its moves already full");
    Eq(static_cast<int>(allies.size()), 0, "the first hero brings no allies");
    Eq(static_cast<int>(h->items.size()), 1, "the first hero carries one item");
    Eq(h->items[0]->type, rules::kItemStandard, "and it is the side's standard");
    h->experience = 15;
    auto promoted = hero::CheckPromotions(g, *side);
    Eq(static_cast<int>(promoted.size()), 1, "15 experience promotes a Hero");
    Eq(h->level, 2, "the hero is now level 2");
    Eq(h->title, "Cavalier", "level 2 is a Cavalier");
    Eq(h->strength, 6, "promotion adds a strength");
    Eq(h->max_moves, 16, "promotion adds 2 movement");
    Eq(static_cast<int>(hero::CheckPromotions(g, *side).size()), 0,
       "no second promotion on the same experience");
    h->experience = 60;
    hero::CheckPromotions(g, *side);
    Eq(h->level, 3, "60 experience promotes one step only");
    hero::CheckPromotions(g, *side);
    Eq(h->title, "Paladin", "the next check reaches Paladin");
    Eq(static_cast<int>(hero::CheckPromotions(g, *side).size()), 0,
       "a Paladin is not promoted again");
    hero::AddExperience(g, h, 99);
    Eq(h->experience, rules::kMaxHeroXp, "experience is capped at 60");
    auto [type, n] = hero::Allies(g);
    Ok(type != nullptr, "the allies have a type");
    Ok(n >= 1 && n <= 3, "1 to 3 allies arrive");
    g.turn = 4;
    HeroOffer later;
    later.city = side->capital;
    auto escort = hero::Recruit(g, *side, later).allies;
    Ok(!escort.empty(), "a later hero arrives with an escort");
    for (Army* a : escort) {
        Eq(a->moves, a->max_moves, "an ally joins with its moves already full");
        Eq(a->upkeep, 0, "an ally costs no upkeep");
    }
    g.turn = 1;
    Eq(hero::kMaxPerSide, 5, "a side may hold 5 heroes");
    Eq(hero::kMaxInGame, 40, "the game holds 40 heroes");
    bool blocked = true;
    for (int i = 0; i < 5; i++) AddArmy(g, 0, 0, side->index, armytype::kHero, 5);
    g.turn = 2;
    for (int i = 0; i < 50; i++)
        if (hero::Offer(g, *side)) blocked = false;
    Ok(blocked, "no hero is offered once the side is at its limit");
    Item fire;
    fire.name = "Firesword";
    fire.type = rules::kItemBattle;
    fire.value = 1;
    Army carrier;
    carrier.type = armytype::kHero;
    carrier.owner = side->index;
    carrier.items = {&fire};
    int dx = kNone, dy = kNone;
    for (int y = 0; y < g.map->height && dx == kNone; y++)
        for (int x = 0; x < g.map->width; x++)
            if (dx == kNone && scn::TerrainAt(*g.map, x, y) == movement::kPlain) {
                dx = x;
                dy = y;
            }
    auto dropped = hero::DropItems(g, &carrier, dx, dy);
    Eq(static_cast<int>(dropped.size()), 1, "the item was dropped");
    if (!dropped.empty()) {
        Eq(dropped[0]->status, 1, "a dropped item lies on the ground");
        Eq(dropped[0]->x, dx, "it lies where the hero fell");
    }
    Eq(static_cast<int>(carrier.items.size()), 0, "the dead hero carries nothing");
    int wx = kNone, wy = kNone;
    for (int y = 0; y < g.map->height && wx == kNone; y++)
        for (int x = 0; x < g.map->width; x++)
            if (wx == kNone && scn::TerrainAt(*g.map, x, y) == movement::kWater) {
                wx = x;
                wy = y;
            }
    if (wx != kNone) {
        Item ice;
        ice.name = "Icesword";
        ice.type = rules::kItemBattle;
        ice.value = 1;
        ice.status = 3;
        Army drowner;
        drowner.type = armytype::kHero;
        drowner.owner = side->index;
        drowner.items = {&ice};
        Eq(static_cast<int>(hero::DropItems(g, &drowner, wx, wy).size()), 0,
           "nothing is dropped in water");
        Eq(ice.status, 0, "an item lost at sea is gone for good");
    }
}

static void TestHeroExperienceBug() {
    printf("hero experience: the original's bug\n");
    auto gp = NewGame("ERYTHEA", Seed(41));
    Game& g = *gp;
    auto run = [&]() {
        Army def_hero, att;
        def_hero.type = armytype::kHero;
        def_hero.strength = 5;
        def_hero.owner = 1;
        att.type = 11;
        att.strength = 3;
        att.owner = 0;
        Battle b;
        hero::BattleExperience(g, {&att}, {&def_hero}, b, false);
        return def_hero.experience;
    };
    rules::bugs.hero_experience_reads_attacker_types = true;
    Eq(run(), 0, "with the bug on, a defending hero facing a non-hero gains nothing");
    rules::bugs.hero_experience_reads_attacker_types = false;
    Eq(run(), 1, "with the bug off, the defender is credited");
    rules::bugs.hero_experience_reads_attacker_types = true;
    Army h;
    h.type = armytype::kHero;
    h.strength = 5;
    h.owner = 0;
    Battle none;
    hero::BattleExperience(g, {&h}, {}, none, false);
    Eq(h.experience, 1, "an attacking hero gains 1 in the open");
    hero::BattleExperience(g, {&h}, {}, none, true);
    Eq(h.experience, 3, "attacking a city is worth 2");
    Army dead;
    dead.type = armytype::kHero;
    dead.strength = 5;
    dead.owner = 0;
    Battle died;
    died.dead_by_army.insert(&dead);
    hero::BattleExperience(g, {&dead}, {}, died, true);
    Eq(dead.experience, 0, "a hero that died gains nothing");
}

static void TestAiGame(const std::string& scenario, int turns) {
    printf("AI game: %s, %d turns\n", scenario.c_str(), turns);
    auto gp = NewGame(scenario, Seed(101));
    Game& g = *gp;
    Side* side = game::Begin(g);
    std::map<int, int> owned_at_start;
    for (Side* s : g.sides)
        owned_at_start[s->index] = static_cast<int>(game::SideCities(g, *s).size());
    while (side && g.turn <= turns) {
        PlayAi(g, *side);
        side = EndTurn(g);
    }
    Ok(g.turn > turns || side == nullptr, "the game ran to the turn limit");
    std::map<int, int> per_tile;
    for (Army* a : g.armies) {
        Ok(g.types.ById(a->type) != nullptr, "every army has a real type");
        if (!a->transit) {
            Ok(a->x != kNone && a->y != kNone, "a placed army has a position");
            Ok(a->x >= 0 && a->x < g.map->width && a->y >= 0 && a->y < g.map->height,
               "every army is on the map");
            per_tile[a->y * g.map->width + a->x]++;
        }
        Ok(a->moves >= 0, "movement never goes negative");
        Ok(a->moves <= rules::kMaxMove, "movement never exceeds the cap");
    }
    int worst = 0;
    for (auto& [k, v] : per_tile) worst = std::max(worst, v);
    Ok(worst <= rules::kMaxStack,
       "no tile ever holds more than 8 armies: " + std::to_string(worst));
    for (Side* s : g.sides) {
        Ok(s->gold >= 0, s->name + " never goes into debt");
        if (!s->alive) Ok(game::SideCities(g, *s).empty(), "a dead side owns nothing");
    }
    for (auto& c : g.map->cities) {
        if (c.owner_index != kNone)
            Ok(g.map->Side(c.owner_index) != nullptr, "a city's owner is a real side");
        Ok(c.slots.size() <= 4, "a city never gains production slots");
    }
    bool moved = false;
    int captures = 0;
    for (Side* s : g.sides) {
        int now = static_cast<int>(game::SideCities(g, *s).size());
        if (now != owned_at_start[s->index]) moved = true;
        captures += std::max(0, now - owned_at_start[s->index]);
    }
    Ok(moved, "cities changed hands over " + std::to_string(turns) + " turns");
    Ok(captures > 0, Fmt("the computer players took %d cities", captures));
    Ok(g.armies.size() > g.map->cities.size(),
       Fmt("armies were built: %d from %d cities", static_cast<int>(g.armies.size()),
           static_cast<int>(g.map->cities.size())));
}

static void TestSlots() {
    printf("army slots\n");
    // a stand-in game: the fight-order table and the army list are all the
    // model wants
    struct Stand {
        Game g;
        std::vector<Army*> a;
    };
    auto stack = [](int n) {
        auto s = std::make_unique<Stand>();
        s->g.map = std::make_unique<Map>();
        s->g.map->fight_order.assign(9, std::vector<int>(29, 0));
        for (int i = 1; i <= n; i++) {
            Army a;
            a.type = i;
            a.moves = 10 + i;
            a.group = 0;
            s->g.map->fight_order[0][i] = n - i;  // the first army ranks highest
            s->a.push_back(s->g.Add(a));
        }
        return s;
    };
    {
        auto st = stack(3);
        auto& a = st->a;
        auto s = slots::Build(st->g, a, 0);
        Eq(s.n, 3, "three armies fill three slots");
        Eq(s.army[0], a[0], "the highest in the fight order takes the first slot");
        Ok(s.in_group[0] && !s.in_group[1] && !s.in_group[2],
           "clicking a tile selects one army, not the stack");
        Eq(s.group[0], 0, "ungrouped armies are each their own group");
        Eq(s.group[2], 2, "numbered off down the bar");
        Eq(s.mark[0], slots::kTick, "the one that moves is ticked");
        Eq(s.mark[1], slots::kCross, "the others are crossed");
        Eq(slots::Moves(s), 11, "and Group Move is that army's own");
        Eq(slots::Grouped(s), false, "so the Grp button is not green yet");
        Eq(static_cast<int>(slots::Selected(s).size()), 1, "one army moves");
    }
    {
        auto st = stack(3);
        auto s = slots::Build(st->g, st->a, 0);
        slots::Toggle(s, st->g, 1);
        Ok(s.in_group[1] && s.group[1] == s.group[0], "a click adds an army to the group");
        Eq(s.mark[1], slots::kNomark, "only the head of a group is marked");
        Eq(slots::Moves(s), 11, "the group moves at its slowest army's pace");
        slots::Toggle(s, st->g, 1);
        Ok(!s.in_group[1], "clicking it again drops it out");
        Ok(s.group[1] != s.group[0], "into a group of its own");
        slots::Toggle(s, st->g, 0);
        Ok(s.in_group[0], "the last army of a group cannot be dropped");
    }
    {
        auto st = stack(3);
        auto s = slots::Build(st->g, st->a, 0);
        slots::All(s, st->g);
        Eq(static_cast<int>(slots::Selected(s).size()), 3, "Grp takes the whole stack");
        Eq(slots::Grouped(s), true, "and turns the button green");
        Eq(s.mark[1], slots::kNomark, "one mark for the one group");
        slots::Single(s, st->g);
        Eq(static_cast<int>(slots::Selected(s).size()), 1, "and Grp again breaks it up");
        Eq(slots::Grouped(s), false, "leaving the button red");
        slots::PickGroup(s, st->g, 2);
        Ok(s.in_group[2] && !s.in_group[0], "a mark picks out that group alone");
        Eq(s.mark[2], slots::kTick, "which is then the ticked one");
    }
    {
        auto st = stack(3);
        auto s = slots::Build(st->g, st->a, 0);
        slots::All(s, st->g);
        s.in_group[2] = false;
        slots::PickGroup(s, st->g, 0);
        Ok(s.in_group[2], "picking a group takes all of it");
    }
    {
        auto st = stack(3);
        auto& a = st->a;
        Game& g = st->g;
        auto s = slots::Build(g, a, 0);
        slots::All(s, g);
        slots::Commit(s, g);
        int id = a[0]->group;
        Ok(id != slots::kUngrouped && id == a[1]->group && id == a[2]->group,
           "a group of more than one is written back into its armies");
        auto again = slots::Build(g, a, 0);
        Eq(static_cast<int>(slots::Selected(again).size()), 3,
           "so picking it up again takes the whole group");
        Eq(slots::Grouped(again), true, "with the Grp button still green");
        Eq(again.group[0], again.group[2], "and all three in one group in the bar");
        slots::Commit(again, g);
        Eq(a[0]->group, id, "picking it up does not renumber it");
        slots::Toggle(again, g, 2);
        slots::Commit(again, g);
        auto third = slots::Build(g, a, 0);
        Eq(static_cast<int>(slots::Selected(third).size()), 2,
           "dropping one out leaves the rest grouped");
        Eq(a[2]->group, slots::kUngrouped, "and the one dropped out is on its own");
        Eq(a[0]->group, id, "while the group keeps its id");
        slots::Single(third, g);
        slots::Commit(third, g);
        Eq(a[0]->group, slots::kUngrouped, "and breaking it up clears the grouping");
        Eq(static_cast<int>(slots::Selected(slots::Build(g, a, 0)).size()), 1,
           "so a click picks up one army again");
    }
    {
        auto st = stack(3);
        auto& a = st->a;
        auto s = slots::Build(st->g, a, 0);
        slots::All(s, st->g);
        auto left = slots::Keep(s, st->g, {a[0], a[2]});
        Ok(left.has_value(), "the survivors are still a selection");
        if (left) {
            Eq(left->n, 2, "the dead leave the bar");
            Ok(left->in_group[0] && left->in_group[1], "the survivors keep moving together");
        }
        Ok(!slots::Keep(s, st->g, {}).has_value(), "and a wiped-out stack is no selection at all");
    }
}

static void TestScreenLayout() {
    printf("screen layout\n");
    auto ui = uidata::Load(kData);
    Eq(ui.bitmaps[0], "startup.pck", "bitmap 0 is the startup screen");
    Eq(ui.bitmaps[4], "button.pck", "bitmap 4 is the button sheet");
    Eq(ui.bitmaps[78], "movebar7.pck", "there are 79 bitmaps, 0 to 78");
    Eq(static_cast<int>(ui.joins.size()), 36, "JOIN.DAT names 36 dialogs");
    Eq(ui.joins[0].button, 0, "dialog 0 uses button group 0");
    Eq(ui.joins[0].area, 2, "dialog 0 uses area screen 2");
    Eq(ui.joins[9].button, 11, "dialog 9 takes button group 11");
    Eq(ui.joins[12].button, 9, "and dialog 12 takes group 9");
    auto d = uidata::FindDialog(ui, uidata::kMainScreen);
    Eq(static_cast<int>(d.controls.size()), 43, "the main screen has 43 controls");
    Eq(static_cast<int>(d.regions.size()), 6, "and 6 regions");
    std::map<int, std::array<int, 4>> want = {{1, {400, 30, 224, 312}},
                                              {2, {16, 30, 360, 360}},
                                              {3, {0, 0, 640, 18}},
                                              {9, {16, 408, 360, 56}}};
    for (auto& r : d.regions) {
        auto w = want.find(r.id);
        if (w != want.end()) {
            Eq(r.x, w->second[0], Fmt("region %d x", r.id));
            Eq(r.y, w->second[1], Fmt("region %d y", r.id));
            Eq(r.w, w->second[2], Fmt("region %d width", r.id));
            Eq(r.h, w->second[3], Fmt("region %d height", r.id));
        }
    }
    Eq(ui.shortcut_names[507], "Move All", "UDB.DAT names menu item 507");
    Eq(ui.shortcut_names[535], "End Turn", "and item 535");
    const char* want_sc[4] = {"Search", "Move All", "Heroes", "End Turn"};
    for (int i = 0; i < 4; i++) {
        Eq(ui.shortcut_names[ui.shortcuts[i]], want_sc[i],
           Fmt("button %d (control %d) is %s", i, 179 + i, want_sc[i]));
    }
    {
        auto img = pck::Decode(kData + "/PICS/MENUBUTT.PCK");
        int w = img->w, h = img->h;
        Eq(w, 320, "MENUBUTT.PCK is 320 wide");
        Eq(h, 197, "and 197 tall: 7 rows 28 apart, plus the last row's border");
        std::set<std::string> cells;
        int n = 0;
        for (auto& [id, item] : ui.shortcut_items) {
            Eq(item.w, 32, Fmt("item %d icon width", id));
            Eq(item.h, 29, Fmt("item %d icon height", id));
            for (int st = 0; st <= 2; st++) {
                auto src = item.src[st];
                Eq(src.x % 32, 0, Fmt("item %d state %d sits on a column", id, st));
                Eq(src.y % 28, 0, Fmt("item %d state %d sits on a row", id, st));
                Ok(src.x + 32 <= w && src.y + 29 <= h,
                   Fmt("item %d state %d is inside the sheet", id, st));
                std::string key = Fmt("%d,%d", src.x, src.y);
                Ok(!cells.count(key), "cell " + key + " is used once");
                cells.insert(key);
                n++;
            }
        }
        Eq(n, 63, "21 items, three cells each");
        auto& end_turn = ui.shortcut_items[535];
        Eq(end_turn.src[uidata::kNormal].x, 0, "End Turn rests at the sheet's origin");
        Eq(end_turn.src[uidata::kNormal].y, 0, "at the top of it");
        Eq(end_turn.src[uidata::kActive].x, 32, "and lights up one cell right");
    }
    {
        auto img = pck::Decode(kData + "/PICS/ABITS.PCK");
        int w = img->w, h = img->h;
        const auto& px = img->px;
        Eq(w, 480, "ABITS.PCK is 480 wide");
        Eq(h, 40, "and 40 tall");
        const int bg = 3;
        std::set<int> seen;
        for (int cell = 0; cell <= 8; cell++) {
            std::map<int, int> count;
            for (int y = 0; y <= 29; y++) {
                for (int x = cell * 32; x <= cell * 32 + 31; x++) {
                    int v = px[y * w + x];
                    if (v != bg) count[v]++;
                }
            }
            int best = kNone, best_n = 0;
            for (auto& [v, c] : count)
                if (c > best_n) {
                    best = v;
                    best_n = c;
                }
            Ok(best_n > 40, Fmt("ring %d has ink", cell));
            if (cell > 0) {
                Ok(!seen.count(best), Fmt("ring %d has its own colour", cell));
                seen.insert(best);
            }
        }
        int ring_row = 0, strip_row = 0;
        for (int x = 0; x <= 287; x++) {
            if (px[15 * w + x] != bg) ring_row++;
            if (px[38 * w + x] != bg) strip_row++;
        }
        Ok(strip_row > ring_row * 2,
           "row 38 is far denser than row 15, so it is not part of the rings");
        struct Icon {
            int sx, sy;
            const char* name;
        };
        for (Icon ic : {Icon{344, 0, "cities"}, Icon{344, 20, "treasury"}, Icon{384, 0, "income"},
                        Icon{384, 20, "upkeep"}}) {
            Ok(ic.sx >= 288, Fmt("the %s icon is past the nine rings", ic.name));
            Ok(ic.sx + 40 <= w && ic.sy + 20 <= h, Fmt("the %s icon is inside the sheet", ic.name));
            int ink = 0;
            for (int y = ic.sy; y <= ic.sy + 19; y++)
                for (int x = ic.sx; x <= ic.sx + 39; x++)
                    if (px[y * w + x] != bg) ink++;
            Ok(ink > 40, Fmt("the %s icon has ink", ic.name));
            int edge = 0;
            for (int y = ic.sy; y <= ic.sy + 19; y++)
                if (px[y * w + ic.sx - 1] != bg) edge++;
            Eq(edge, 0, Fmt("the %s icon starts on a clear column", ic.name));
        }
    }
    std::map<std::string, std::pair<int, int>> sizes;
    std::set<std::string> missing;
    int checked = 0;
    for (auto& [gid, group] : ui.buttons) {
        for (auto& c : group.controls) {
            if (c.bitmap != 0 && c.w > 0) {
                std::string name = ui.bitmaps[c.bitmap];
                if (!sizes.count(name) && !missing.count(name)) {
                    bool found = false;
                    for (const char* dir : {"/PICS/", "/TERRAIN0/", "/"}) {
                        std::string p = kData + dir + Upper(name);
                        if (FileExists(p)) {
                            auto im = pck::Decode(p);
                            sizes[name] = {im->w, im->h};
                            found = true;
                            break;
                        }
                    }
                    if (!found) missing.insert(name);
                }
                Ok(sizes.count(name), "bitmap resolves to a file: " + name);
                if (sizes.count(name)) {
                    auto [sw, sh] = sizes[name];
                    for (int st = 0; st <= 2; st++) {
                        Ok(c.src[st].x + c.w <= sw && c.src[st].y + c.h <= sh,
                           Fmt("control %d state %d lies inside %s", c.id, st, name.c_str()));
                        checked++;
                    }
                }
            }
        }
    }
    Ok(checked >= 400, Fmt("checked %d source rects", checked));
}

static void TestCityCastles() {
    printf("city castles\n");
    auto gp = NewGame("TUTORIA", Seed(11));
    Game& g = *gp;
    Map& map = *g.map;
    std::map<int, int> expected_by_level = {{-1, 96}, {0, 98},  {1, 100}, {2, 102}, {3, 104},
                                            {4, 106}, {5, 108}, {6, 128}, {7, 130}};
    City& city = map.cities[0];
    for (auto [owner, base] : expected_by_level) {
        city.owner_index = owner >= 0 ? owner : kNone;
        city.razed = false;
        Ok(scn::CityTileBase(city) == base, Fmt("owner %d takes castle block %d", owner, base));
        scn::SetCityTiles(map, city);
        Ok(scn::TileAt(map, city.x, city.y) == base, "top-left cell is the block");
        Ok(scn::TileAt(map, city.x + 1, city.y) == base + 1, "top-right is +1");
        Ok(scn::TileAt(map, city.x, city.y + 1) == base + 16, "bottom-left is +16");
        Ok(scn::TileAt(map, city.x + 1, city.y + 1) == base + 17, "bottom-right is +17");
        for (int dx = 0; dx <= 1; dx++)
            for (int dy = 0; dy <= 1; dy++)
                Ok(scn::TerrainAt(map, city.x + dx, city.y + dy) == 10,
                   Fmt("castle cell %d,%d is city terrain", dx, dy));
    }
    for (int owner = 0; owner <= 7; owner++) {
        city.owner_index = kNone;
        city.razed = true;
        city.razed_by = owner;
        Ok(scn::CityTileBase(city) == 0xa0 + 2 * owner,
           Fmt("ruins of side %d take block %d", owner, 0xa0 + 2 * owner));
        scn::SetCityTiles(map, city);
        for (int dx = 0; dx <= 1; dx++)
            for (int dy = 0; dy <= 1; dy++)
                Ok(scn::TerrainAt(map, city.x + dx, city.y + dy) == 11,
                   Fmt("ruin cell %d,%d is ruins terrain", dx, dy));
    }
    auto g2p = NewGame("TUTORIA", Seed(12));
    Game& g2 = *g2p;
    City& c2 = g2.map->cities[0];
    for (Side* s : g2.sides) {
        game::SetCityOwner(g2, c2, s->index);
        scn::SetCityTiles(*g2.map, c2);
    }
    auto mine_list = game::SideCities(g2, *g2.sides[0]);
    City* mine = mine_list.empty() ? nullptr : mine_list[0];
    Ok(mine != nullptr, "the first side holds a city to raze");
    if (mine) {
        int was = mine->owner_index;
        game::Raze(g2, *g2.sides[0], *mine, {});
        Ok(mine->razed_by == was, "raze remembers who held the city");
        Ok(scn::TileAt(*g2.map, mine->x, mine->y) == 0xa0 + 2 * was,
           "raze restamps the map with that side's ruins");
    }
}

static void TestSites(const std::string& scenario) {
    printf("sites: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(53));
    Game& g = *gp;
    int rich = 0;
    for (auto& s : g.map->sites) {
        Ok(s.content != site::kEmpty, "no site is left empty");
        if (s.rich) {
            rich++;
            Ok(s.type != site::kTemple, "a temple is never rich");
        }
        if (s.content == site::kAllies) {
            Ok(s.ally_type != kNone, "an ally ruin knows what joins");
            Ok(g.types.ById(s.ally_type) != nullptr, "and it is a real army type");
        }
        // an allies site keeps its allies' type where the guardian would be,
        // and nothing guards it (66d4:0000, 6536:01ab)
        if (s.content == site::kAllies)
            Eq(s.guardian, kNone, "nothing guards an allies ruin");
        else if (s.content != site::kTemple)
            Ok(s.guardian >= 1 && s.guardian <= 9, "a ruin has a guardian");
    }
    Eq(rich, static_cast<int>(g.map->sites.size()) * 3 / 10, "three in ten sites are rich");
    std::set<int> placed;
    for (auto& s : g.map->sites) {
        if (s.content == site::kItem) {
            Ok(!placed.count(s.item), "an item is hidden in only one ruin");
            placed.insert(s.item);
        }
    }
    for (auto& it : g.map->items)
        if (placed.count(it.index)) Eq(it.status, 2, "a hidden item is marked hidden");
    std::map<int, Item*> by_index;
    for (auto& it : g.map->items) by_index[it.index] = &it;
    for (auto& s : g.map->sites) {
        if (s.content == site::kItem)
            Eq(site::ItemReserved(*by_index[s.item]), s.rich, "a reserved item needs a rich ruin");
    }
    if (g.map->item_pool) {
        std::set<std::string> names;
        for (auto& p : *g.map->item_pool) names.insert(p.name);
        for (int i = 8; i <= 21; i++)
            if (by_index.count(i))
                Ok(names.count(by_index[i]->name),
                   "item " + std::to_string(i) + " came from the pool");
        for (int i = 0; i <= 7; i++)
            if (by_index.count(i))
                Eq(by_index[i]->type, rules::kItemStandard, "records 0-7 are standards");
    }
    Site* ruin = nullptr;
    for (auto& s : g.map->sites)
        if (!ruin && s.content == site::kGold) ruin = &s;
    if (ruin) {
        Side* side = g.sides[0];
        Army* grunt = AddArmy(g, ruin->x, ruin->y, side->index, 11, 3, 10, 10);
        auto r = site::Search(g, {grunt}, ruin->x, ruin->y);
        Eq(r->kind, "no hero", "a stack without a hero cannot search a ruin");
        Ok(!ruin->searched, "and the ruin is not used up");
        Army* h = AddArmy(g, ruin->x, ruin->y, side->index, armytype::kHero, 9, 14, 14);
        int gold0 = side->gold;
        r = site::Search(g, {h, grunt}, ruin->x, ruin->y);
        Ok(r->kind == "gold" || r->kind == "killed", "a hero gets a result: " + r->kind);
        if (r->kind == "gold") {
            Ok(r->gold >= 503 && r->gold <= 4000,
               "the gold is in range: " + std::to_string(r->gold));
            Eq(side->gold, gold0 + r->gold, "the gold was paid");
            Eq(h->experience, 3, "searching is worth 3 experience");
        }
        if (r->kind == "gold") {
            Ok(ruin->searched, "a searched ruin is used up");
            Ok(site::Search(g, {h}, ruin->x, ruin->y) == nullptr, "and cannot be searched again");
        } else {
            Ok(!ruin->searched, "a hero who dies leaves the ruin to be searched again");
        }
    }
    Site* temple = nullptr;
    for (auto& s : g.map->sites)
        if (!temple && s.content == site::kTemple && s.temple_index == 0) temple = &s;
    if (temple) {
        Side* side = g.sides[0];
        Army a;
        a.x = temple->x;
        a.y = temple->y;
        a.owner = side->index;
        a.type = 11;
        a.strength = 3;
        a.moves = 0;
        Ok(site::Search(g, {&a}, temple->x, temple->y) == nullptr,
           "searching needs a move left (6536:0000)");
        a.moves = 10;
        auto r = site::Search(g, {&a}, temple->x, temple->y, true);
        Eq(r->kind, "temple", "with no hero a temple blesses at once, a human's too");
        Eq(r->kind, "temple", "a temple blesses");
        Eq(r->blessed, 1, "one army was blessed");
        Eq(a.strength, 4, "the blessing adds a strength");
        Eq(site::Search(g, {&a}, temple->x, temple->y)->blessed, 0,
           "the same temple does not bless twice");
        a.strength = 9;
        a.blessings.clear();
        site::Search(g, {&a}, temple->x, temple->y);
        Eq(a.strength, 9, "a blessing never passes 9");
    }
    Army strong, weak;
    strong.strength = 9;
    weak.strength = 1;
    int survived = 0, died = 0;
    for (int i = 0; i < 400; i++) {
        if (site::SurvivesGuardian(g, strong, 3)) survived++;
        if (!site::SurvivesGuardian(g, weak, 8)) died++;
    }
    Eq(survived, 400, "a strong hero always beats a weak guardian");
    Ok(died > 0, "a weak hero sometimes dies to a strong one");
}

// A stack that takes a city from the sea is still at sea until its side's
// next turn begins, then ashore at no cost in movement (8cc6:089d); armies
// left on water without a boat drown (1b62:0ea0).
static void TestShips() {
    printf("ships\n");
    auto gp = NewGame("ERYTHEA", Seed(3));
    Game& g = *gp;
    Side* side = game::Begin(g);
    City* city = game::SideCities(g, *side)[0];
    Army* sailor = AddArmy(g, city->x, city->y, side->index, 2, 3, 0, 14);
    sailor->at_sea = true;
    for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
    Ok(!sailor->at_sea, "an army at sea in a city is ashore when its turn begins");
    Eq(sailor->moves, sailor->max_moves, "with its own movement, not the 20 at sea");

    int wx = kNone, wy = kNone;
    for (int y = 0; y < g.map->height && wx == kNone; y++) {
        for (int x = 0; x < g.map->width && wx == kNone; x++) {
            if (scn::TerrainAt(*g.map, x, y) == movement::kWater &&
                !scn::IsCrossing(*g.map, x, y) && game::ArmiesAt(g, x, y).empty()) {
                wx = x;
                wy = y;
            }
        }
    }
    auto at = [&](Army* a) {
        return std::find(g.armies.begin(), g.armies.end(), a) != g.armies.end();
    };
    Army* walker = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    Army* boat = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    boat->at_sea = true;
    Army* hero = AddArmy(g, wx, wy, side->index, armytype::kHero, 5, 14, 14);
    Eq(game::SinkStranded(g, *side, wx, wy), 1, "on the water the army without a boat sinks");
    Ok(!at(walker), "it is gone");
    Ok(at(boat) && at(hero), "one at sea, and a hero, do not sink");
    int flyer = kNone;
    for (auto& t : g.types.all)
        if (flyer == kNone && t.flies) flyer = t.id;
    Army* bird = AddArmy(g, wx, wy, side->index, flyer, 3, 14, 14);
    Army* walker2 = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    Eq(game::SinkStranded(g, *side), 1, "as the turn opens every tile is looked at");
    Ok(at(bird) && !at(walker2), "and only the army that can neither fly nor sail goes");
    Item wings;
    wings.type = rules::kItemFlight;
    hero->items.push_back(&wings);
    Army* carried = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    Eq(game::SinkStranded(g, *side, wx, wy), 0, "a hero's flight item carries the whole stack");
    hero->items.clear();
    Eq(game::SinkStranded(g, *side, wx, wy), 1, "without it the walker drowns");
    Ok(!at(carried), "");
    Army* ashore = AddArmy(g, city->x, city->y, side->index, 2, 3, 14, 14);
    Eq(game::SinkStranded(g, *side, city->x, city->y), 0, "nothing sinks on land");
    Ok(at(ashore), "");
}

// A step is taken while the walk's cost so far fits in the moves left
// (1555:18be), with no floor; an attack is fought from beside the tile and
// needs one move, then costs the tile's movement, 2 at least (67cc:0000).
static void TestLastMove() {
    printf("the last move point\n");
    auto gp = NewGame("ERYTHEA", Seed(11));
    Game& g = *gp;
    Side* side = game::Begin(g);
    Army* army = game::SideArmies(g, *side)[0];
    int rx = kNone, ry = kNone;
    for (int y = 1; y < g.map->height - 1 && rx == kNone; y++) {
        for (int x = 1; x < g.map->width - 1 && rx == kNone; x++) {
            if (scn::RoadAt(*g.map, x, y) % 0x20 == 0 || scn::RoadAt(*g.map, x + 1, y) % 0x20 == 0)
                continue;
            if (game::CityAt(g, x, y) || game::CityAt(g, x + 1, y)) continue;
            if (!game::ArmiesAt(g, x, y).empty() || !game::ArmiesAt(g, x + 1, y).empty()) continue;
            rx = x;
            ry = y;
        }
    }
    Ok(rx != kNone, "found two road tiles side by side");
    if (rx != kNone) {
        army->x = rx;
        army->y = ry;
        army->at_sea = false;
        army->moves = 1;
        movement::Invalidate(g);
        auto r = movement::MoveTo(g, {army}, rx + 1, ry);
        Eq(r.steps, 1, "one move left still buys a step along a road");
        Eq(army->x, rx + 1, "and the army is there");
        Eq(army->moves, 0, "with nothing left");
    }
    City* enemy = nullptr;
    for (auto& c : g.map->cities)
        if (!enemy && c.owner_index != side->index) enemy = &c;
    Army* a = AddArmy(g, enemy->x, enemy->y - 1, side->index, 11, 3, 1, 20);
    game::ChargeAttack(g, {a}, enemy->x, enemy->y);
    Eq(a->moves, 0, "an attack takes the last move");
    a->moves = 10;
    game::ChargeAttack(g, {a}, enemy->x, enemy->y);
    Eq(a->moves, 8, "a city costs 1 to enter but 2 to attack");
}

/**
 * @brief Checks diplomacy: asking before an attack, war and peace proposals,
 * the diplomatic score and its titles, and walking past or into a side at
 * peace.
 */
static void TestDiplomacy() {
    printf("diplomacy\n");
    namespace d = diplomacy;
    {
        // Attacking a tile asks first, and "attack anyway" goes to war.
        auto ask_game = NewGame("ERYTHEA", SeedWith(65, {{"diplomacy", 1}}));
        City* city = &ask_game->map->cities[1];
        Ok(d::MustDeclare(*ask_game, 0, 1, nullptr), "attacking armies at peace asks first");
        Ok(!d::MustDeclare(*ask_game, 0, kNone, city), "attacking a neutral never asks");
        ask_game->diplomacy.state[0 * 8 + 1] = d::kIntermediate;
        Ok(!d::MustDeclare(*ask_game, 0, 1, nullptr), "armies of a side merely uneasy are fair game");
        Ok(d::MustDeclare(*ask_game, 0, 1, city), "but its cities need war");
        Ok(!d::MustDeclare(*ask_game, 0, 1, &ask_game->map->cities[0]),
           "except city 0, which the original's > 0 test misses");
        ask_game->diplomacy.state[0 * 8 + 1] = d::kPeace;
        int before = ask_game->sides[0]->diplo_score;
        d::AttackAnyway(*ask_game, 0, 1);
        int cost = ask_game->sides[0]->diplo_score - before;
        Ok(cost >= 101 && cost <= 200, Fmt("attacking at peace costs 1d100+100 score: %d", cost));
        Eq(d::State(*ask_game, 0, 1), d::kWar, "and it is war");
        Eq(d::State(*ask_game, 1, 0), d::kWar, "both ways");
        Eq(d::Proposal(*ask_game, 1, 0), d::kWar, "the proposals too");
        Ok(!d::MustDeclare(*ask_game, 0, 1, city), "so the attack goes ahead");
        auto off_opts = NewGame("ERYTHEA", Seed(65));
        Ok(!d::MustDeclare(*off_opts, 0, 1, city), "with Diplomacy off nothing asks");
    }
    auto off = NewGame("ERYTHEA", Seed(61));
    Eq(off->map->options.diplomacy, 0, "the shipped scenarios have diplomacy off");
    Eq(d::State(*off, 0, 1), d::kWar, "with the option off every pair starts at war");
    Ok(d::MayAttack(*off, 0, 1), "and may attack freely");
    auto game_ptr = NewGame("ERYTHEA", SeedWith(61, {{"diplomacy", 1}}));
    Game& game = *game_ptr;
    Eq(d::State(game, 0, 1), d::kPeace, "with the option on every pair starts at peace");
    Ok(!d::MayAttack(game, 0, 1), "a side at peace may not attack");
    Ok(d::MayAttack(game, 0, kNone), "neutrals may always be attacked");
    Eq(d::State(game, 0, 0), d::kPeace, "a side is at peace with itself");
    d::Propose(game, 0, 1, d::kWar);
    auto msgs = d::Apply(game, *game.sides[0]);
    Eq(d::State(game, 0, 1), d::kWar, "declaring war takes effect at once");
    Eq(d::State(game, 1, 0), d::kWar, "and binds the other side too");
    Ok(!msgs.empty() && Contains(msgs[0], "War declared"), "and is announced");
    Eq(d::Proposal(game, 1, 0), d::kWar, "the other side's proposal is raised to match");
    auto peace_game_ptr = NewGame("ERYTHEA", SeedWith(62, {{"diplomacy", 1}}));
    Game& peace_game = *peace_game_ptr;
    d::Propose(peace_game, 0, 1, d::kWar);
    d::Apply(peace_game, *peace_game.sides[0]);
    peace_game.diplomacy.proposal[1 * 8 + 0] = kNone;  // forget their matching proposal
    d::Propose(peace_game, 0, 1, d::kPeace);
    d::Apply(peace_game, *peace_game.sides[0]);
    Eq(d::State(peace_game, 0, 1), d::kWar, "one-sided peace does not land");
    d::Propose(peace_game, 0, 1, d::kPeace);
    d::Propose(peace_game, 1, 0, d::kPeace);
    auto peace_msgs = d::Apply(peace_game, *peace_game.sides[0]);
    Eq(d::State(peace_game, 0, 1), d::kPeace, "matched proposals make peace");
    Ok(!peace_msgs.empty() && Contains(peace_msgs[0], "Peace negotiated"), "and it is announced");
    {
        auto score_game_ptr = NewGame("ERYTHEA", SeedWith(63, {{"diplomacy", 1}}));
        Game& score_game = *score_game_ptr;
        d::Propose(score_game, 0, 1, d::kWar);
        d::Apply(score_game, *score_game.sides[0]);
        d::Propose(score_game, 0, 1, d::kPeace);
        int before = score_game.sides[0]->diplo_score;
        d::ScoreUpdate(score_game, *score_game.sides[0]);
        int gain = score_game.sides[0]->diplo_score - before;
        Ok(gain >= 11 && gain <= 20,
           "peace offered from war is worth 1d10+10: " + std::to_string(gain));
        d::Propose(score_game, 1, 0, d::kPeace);
        before = score_game.sides[0]->diplo_score;
        d::ScoreUpdate(score_game, *score_game.sides[0]);
        Eq(score_game.sides[0]->diplo_score, before, "not once the other side offers it too");
    }
    auto rating_game_ptr = NewGame("ERYTHEA", Seed(63));
    Game& rating_game = *rating_game_ptr;
    for (size_t i = 0; i < rating_game.sides.size(); i++)
        rating_game.sides[i]->diplo_score = static_cast<int>(i + 1) * 10;
    auto ratings = d::Ratings(rating_game);
    Eq(ratings[rating_game.sides[0]->index], "Statesman", "the lowest score is the Statesman");
    Eq(ratings[rating_game.sides.back()->index], "Running Dog", "the highest is the Running Dog");
    const auto& ranks = d::RatingRanks(2);
    Eq(static_cast<int>(ranks.size()), 2, "two sides take two titles");
    Eq(std::string(d::kTitles[ranks[0] - 1]), "Statesman", "best of two");
    Eq(std::string(d::kTitles[ranks[1] - 1]), "Running Dog", "worst of two");
    auto walk_game_ptr = NewGame("ERYTHEA", SeedWith(64, {{"diplomacy", 1}}));
    Game& walk_game = *walk_game_ptr;
    Side* side = game::Begin(walk_game);
    Side* victim = nullptr;
    for (Side* candidate : walk_game.sides)
        if (candidate->index != side->index) victim = candidate;
    City* city = victim->capital;
    Army* army = AddArmy(walk_game, city->x, city->y - 1, side->index, 11, 3, 20, 20);
    army->upkeep = 1;
    auto path = movement::FindPath(walk_game, {army}, army->x, army->y, city->x, city->y);
    if (path && !path->empty()) {
        auto into_city = movement::Walk(walk_game, {army}, *path);
        Eq(into_city.stopped, "at peace", "a stack at peace cannot walk into their city");
        Eq(army->x, city->x, "and has not moved into it");
    }

    // but a stack of a side at peace in the open is passed over: the walk may
    // not stop on it, and goes on beyond it
    Side* other = victim;
    int map_width = walk_game.map->width;
    auto open = [&](int x, int y) {
        int terrain = scn::TerrainAt(*walk_game.map, x, y);
        return terrain != movement::kWater && terrain != movement::kShore &&
               terrain != movement::kCity && terrain != movement::kBridge &&
               !walk_game.map->city_tile[y * map_width + x] &&
               game::ArmiesAt(walk_game, x, y).empty();
    };
    int start_x = kNone, start_y = kNone;
    for (int y = 10; y <= walk_game.map->height - 10; y++)
        for (int x = 10; x <= map_width - 10; x++)
            if (start_x == kNone && open(x, y) && open(x + 1, y) && open(x + 2, y)) {
                start_x = x;
                start_y = y;
            }
    Army* walker = AddArmy(walk_game, start_x, start_y, side->index, 11, 3, 20, 20);
    Army* blocker = AddArmy(walk_game, start_x + 1, start_y, other->index, 11, 3, 20, 20);
    walker->upkeep = blocker->upkeep = 1;
    movement::Path steps = {{start_x + 1, start_y, 2}, {start_x + 2, start_y, 2}};
    auto past_walk = movement::Walk(walk_game, {walker}, steps);
    Eq(walker->x, start_x + 2, "a stack at peace in the open is passed over");
    Eq(walker->moves, 16, "paying for the step past it");
    Eq(past_walk.stopped, "arrived", "and the walk arrives");
    walker->x = start_x;
    walker->moves = 20;
    walk_game.diplomacy.state[side->index * 8 + other->index] = d::kWar;
    auto war_walk = movement::Walk(walk_game, {walker}, steps);
    Eq(war_walk.stopped, "attack", "a stack at war is attacked");
    Eq(walker->x, start_x, "from where the walker stands");

    // ruins are open ground: a fight there is for no city, and takes none
    City* ruin = nullptr;
    for (auto& candidate : walk_game.map->cities)
        if (!ruin && candidate.owner_index != kNone && candidate.owner_index != side->index)
            ruin = &candidate;
    game::Raze(walk_game, walk_game.map->sides[ruin->owner_index], *ruin, {}, true);
    std::vector<Army*> on_ruin;
    for (Army* occupant : walk_game.armies)
        if (occupant->x != kNone && occupant->x >= ruin->x && occupant->x <= ruin->x + 1 &&
            occupant->y >= ruin->y && occupant->y <= ruin->y + 1)
            on_ruin.push_back(occupant);
    for (Army* occupant : on_ruin) walk_game.Remove(occupant);
    walk_game.diplomacy.state[side->index * 8 + other->index] = d::kWar;
    AddArmy(walk_game, ruin->x, ruin->y, other->index, 11, 1, 20, 20);
    AddArmy(walk_game, ruin->x + 1, ruin->y + 1, other->index, 11, 1, 20, 20);
    std::vector<Army*> strike;
    for (int i = 0; i < 8; i++)
        strike.push_back(AddArmy(walk_game, ruin->x - 1, ruin->y, side->index, 11, 9, 20, 20));
    auto fight = game::ResolveAttack(walk_game, strike, ruin->x, ruin->y);
    Eq(fight.lines.city, static_cast<City*>(nullptr), "a fight on ruins is not for a city");
    Eq(static_cast<int>(fight.lines.defenders.size()), 1, "and only the tile fought for defends");
    Eq(fight.captured, static_cast<City*>(nullptr), "nothing is captured");
    Eq(ruin->owner_index, kNone, "the ruins belong to nobody");
}

/**
 * @brief Makes a quest for a hero to occupy or raze a city.
 * @param type The quest type, e.g. quest::kOccupy or quest::kRaze.
 * @param hero The hero on the quest.
 * @param target The city the quest is about.
 * @return The new quest, with nothing done yet.
 */
static std::shared_ptr<Quest> CityQuest(int type, Army* hero, City* target) {
    auto city_quest = std::make_shared<Quest>();
    city_quest->type = type;
    city_quest->hero = hero;
    city_quest->target_kind = "city";
    city_quest->city = target;
    city_quest->done = 0;
    return city_quest;
}

/**
 * @brief Checks quests: assignment, the type table, how each quest type is
 * completed or failed, and the rewards.
 */
static void TestQuests() {
    printf("quests\n");
    auto off = NewGame("ERYTHEA", Seed(71));
    Side* side_off = game::Begin(*off);
    Army hero_off;
    hero_off.x = 10;
    hero_off.y = 10;
    hero_off.owner = side_off->index;
    hero_off.type = armytype::kHero;
    hero_off.strength = 5;
    Ok(quest::Assign(*off, *side_off, &hero_off) == nullptr, "no quests when the option is off");
    auto game_ptr = NewGame("ERYTHEA", SeedWith(71, {{"quests", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* hero =
        AddArmy(game, side->capital->x, side->capital->y, side->index, armytype::kHero, 5);
    auto quest1 = quest::Assign(game, *side, hero);
    Ok(quest1 != nullptr, "a quest is assigned");
    if (quest1) {
        Ok(quest1->type >= 0 && quest1->type <= 6, "with a known type");
        Ok(quest1->HasTarget(), "and a target");
        Ok(!quest::Describe(*quest1).empty(), "a quest describes itself");
    }
    Ok(quest::Assign(game, *side, hero) == nullptr, "only one quest at a time");
    std::map<int, int> counts;
    for (int quest_type : quest::kTypeTable) counts[quest_type]++;
    Eq(counts[0], 1, "type 0 has one slot");
    Eq(counts[4], 2, "type 4 has two");
    Eq(counts[6], 2, "type 6 has two");
    Eq(static_cast<int>(quest::kTypeTable.size()), 10, "the table is rolled with 1d10");
    // the Lua's cities[lua_index]
    auto city_at_lua_index = [&](int lua_index) { return &game.map->cities[lua_index - 1]; };
    quest::Event event;
    side->quest = CityQuest(quest::kOccupy, hero, city_at_lua_index(2));
    side->gold = 5000;
    event = quest::Event();
    event.city = city_at_lua_index(2);
    event.stack = {hero};
    auto result = quest::Notify(game, *side, "occupy", event);
    Ok(result && result->failed.empty(), "occupying the target completes the quest");
    Ok(!side->quest, "and the quest is cleared");
    Eq(hero->experience, quest::kExperience, "the hero gains 10 experience");
    Ok(result && result->reward, "a reward was given");
    bool was_computer = side->computer;
    side->computer = false;
    side->quest_news.reset();
    side->quest = CityQuest(quest::kOccupy, hero, city_at_lua_index(2));
    result = quest::Notify(game, *side, "occupy", event);
    Ok(side->quest_news == result, "the human keeps the news to be shown");
    side->computer = true;
    side->quest_news.reset();
    side->quest = CityQuest(quest::kOccupy, hero, city_at_lua_index(2));
    quest::Notify(game, *side, "occupy", event);
    Ok(!side->quest_news, "the computer's quest ends unannounced");
    side->computer = was_computer;
    side->quest_news.reset();
    hero->experience = 0;
    side->quest = CityQuest(quest::kOccupy, hero, city_at_lua_index(3));
    event = quest::Event();
    event.city = city_at_lua_index(3);
    result = quest::Notify(game, *side, "occupy", event);
    Ok(result && !result->failed.empty(), "occupying without the hero fails the quest");
    if (result) Eq(result->why, 0x22, "said as 'Alas! The city was not taken by thy hero!'");
    Eq(hero->experience, 0, "and pays no experience");
    City* raze_target = city_at_lua_index(5);
    int target_owner = raze_target->owner_index;
    bool target_razed = raze_target->razed;
    side->quest = CityQuest(quest::kRaze, hero, raze_target);
    raze_target->razed = true;
    result = quest::Notify(game, *side, "turn");
    Ok(result && !result->failed.empty() && result->why == 0x21,
       "a raze quest's city razed by another fails it");
    raze_target->razed = false;
    raze_target->owner_index = side->index;
    side->quest = CityQuest(quest::kRaze, hero, raze_target);
    result = quest::Notify(game, *side, "turn");
    Ok(result && !result->failed.empty() && result->why == 0x2a,
       "one taken and kept fails it too");
    raze_target->razed = target_razed;
    raze_target->owner_index = target_owner;
    side->quest = CityQuest(quest::kOccupy, hero, city_at_lua_index(4));
    event = quest::Event();
    event.city = city_at_lua_index(4);
    event.stack = {hero};
    result = quest::Notify(game, *side, "raze", event);
    Ok(result && !result->failed.empty(), "razing a city you were to keep fails the quest");
    Side* victim = game.sides.back();
    auto slaughter = [&](int required) {
        auto slaughter_quest = std::make_shared<Quest>();
        slaughter_quest->type = quest::kSlaughter;
        slaughter_quest->hero = hero;
        slaughter_quest->target_kind = "side";
        slaughter_quest->side = victim;
        slaughter_quest->required = required;
        return slaughter_quest;
    };
    side->quest = slaughter(3);
    Army slain_first, slain_second;
    slain_first.owner = victim->index;
    slain_second.owner = victim->index;
    event = quest::Event();
    event.stack = {hero};
    event.killed = {&slain_first, &slain_second};
    Ok(quest::Notify(game, *side, "battle", event) == nullptr, "two of three is not enough");
    Eq(side->quest->done, 2, "the count rises");
    result = quest::Notify(game, *side, "battle", event);
    Ok(result && result->failed.empty(), "the third kill completes it");
    side->quest = slaughter(2);
    event.stack = {};
    quest::Notify(game, *side, "battle", event);
    Eq(side->quest->done, 0, "kills away from the hero do not count");
    {
        auto pillage_quest = std::make_shared<Quest>();
        pillage_quest->type = quest::kPillageGold;
        pillage_quest->hero = hero;
        pillage_quest->target_kind = "none";
        pillage_quest->required = 100;
        side->quest = pillage_quest;
    }
    event = quest::Event();
    event.stack = {hero};
    event.gold = 60;
    quest::Notify(game, *side, "pillage", event);
    Eq(side->quest->done, 60, "pillaged gold counts");
    result = quest::Notify(game, *side, "pillage", event);
    Ok(result && result->failed.empty(), "reaching the total completes it");
    Item item;
    item.index = 99;
    item.name = "Testsword";
    item.type = rules::kItemBattle;
    item.value = 1;
    item.status = 3;
    hero->items = {&item};
    {
        auto item_quest = std::make_shared<Quest>();
        item_quest->type = quest::kRetrieveItem;
        item_quest->hero = hero;
        item_quest->target_kind = "item";
        item_quest->item = &item;
        side->quest = item_quest;
    }
    event = quest::Event();
    event.hero = hero;
    result = quest::Notify(game, *side, "item", event);
    Ok(result && result->failed.empty(), "carrying the item completes the quest");
    Ok(std::find(hero->items.begin(), hero->items.end(), &item) == hero->items.end(),
       "the quest item is taken away (the reward may add another)");
    Eq(item.status, 0, "it leaves play");
    Army prey;
    prey.type = armytype::kHero;
    prey.owner = victim->index;
    {
        auto slay_quest = std::make_shared<Quest>();
        slay_quest->type = quest::kSlayHero;
        slay_quest->hero = hero;
        slay_quest->target_kind = "army";
        slay_quest->army = &prey;
        side->quest = slay_quest;
    }
    event = quest::Event();
    event.killed = {&prey};
    Ok(quest::Notify(game, *side, "battle", event) == nullptr,
       "someone else killing the quarry does not count");
    event.stack = {hero};
    result = quest::Notify(game, *side, "battle", event);
    Ok(result && result->failed.empty(), "the hero killing the quarry completes it");
    Army ghost;
    ghost.type = armytype::kHero;
    {
        auto ghost_quest = std::make_shared<Quest>();
        ghost_quest->type = quest::kPillageGold;
        ghost_quest->hero = &ghost;
        ghost_quest->target_kind = "none";
        ghost_quest->required = 1;
        side->quest = ghost_quest;
    }
    result = quest::Notify(game, *side, "turn");
    Ok(result && !result->failed.empty(), "a quest with a dead hero is abandoned");
    side->gold = 50;
    Quest with_hero;
    with_hero.hero = hero;
    auto reward = quest::ChooseReward(game, *side, &with_hero);
    Eq(reward->kind, "gold", "a side with no gold is given gold");
    Ok(reward->gold >= 1002 && reward->gold <= 3000,
       "2d1000+1000: " + std::to_string(reward->gold));
    game.turn = 20;
    side->gold = 5000;
    reward = quest::ChooseReward(game, *side, &with_hero);
    Eq(reward->kind, "allies", "a small side past turn 15 is given allies");
    Ok(!reward->armies.empty() && reward->armies.size() <= 8, "1d3+5 allies arrive");
}

/**
 * @brief Checks how a game ends: the last side standing, no players left,
 * sides put out at the round's end, winning with more than half the cities,
 * and the offer of surrender.
 */
static void TestEndGame() {
    printf("end of the game\n");
    auto game_ptr = NewGame("ERYTHEA", Seed(81));
    Game& game = *game_ptr;
    for (Side* side : game.sides) side->computer = true;
    Side* winner = game.sides[0];
    for (auto& city : game.map->cities)
        city.owner_index = city.owner_index != kNone ? winner->index : kNone;
    for (size_t i = 1; i < game.sides.size(); i++) game.sides[i]->alive = false;
    auto result = game::CheckEnd(game);
    Ok(result.triumph, "the last side standing has triumphed");
    Ok(!result.over, "and the game goes on, to be looked over");
    Eq(result.winner, winner, "the last side standing wins");
    Ok(!winner->computer, "and is switched to human control");
    Ok(game.won, "the game-won flag is set");
    Ok(!result.no_humans, "a game that never had a human does not say the last one fell");
    auto cityless = NewGame("ERYTHEA", Seed(82));
    for (auto& city : cityless->map->cities) city.owner_index = kNone;
    Ok(!game::CheckEnd(*cityless).over, "sides still in the game are players, cities or not");
    auto round_end = game::EndRound(*cityless);
    Ok(round_end.over, "with no cities owned the game is over at the round's end");
    Eq(round_end.winner, static_cast<Side*>(nullptr), "and nobody won");
    Ok(Contains(round_end.message, "No more players"), "with the right message");
    Eq(round_end.fallen.size(), cityless->sides.size(), "every side fell");

    // the round's end puts a side with no city out
    auto fall_game_ptr = NewGame("ERYTHEA", SeedWith(86, {{"diplomacy", 1}}));
    Game& fall_game = *fall_game_ptr;
    for (size_t i = 0; i < fall_game.sides.size(); i++) fall_game.sides[i]->computer = i > 0;
    game::Begin(fall_game);
    Side* doomed = fall_game.sides[1];
    Side* other = fall_game.sides[2];
    for (City* city : game::SideCities(fall_game, *doomed)) city->owner_index = other->index;
    fall_game.diplomacy.state[doomed->index * 8 + other->index] = diplomacy::kWar;
    fall_game.diplomacy.state[other->index * 8 + doomed->index] = diplomacy::kWar;
    Ok(!game::SideArmies(fall_game, *doomed).empty(), "the doomed side still has armies");
    EndTurn(fall_game);
    Eq(fall_game.side, other, "a computer side with no city has no turn");
    Ok(doomed->alive, "but it is not out before the round's end");
    while (fall_game.turn == 1) EndTurn(fall_game);
    Ok(!doomed->alive, "the round's end puts it out");
    Eq(static_cast<int>(game::SideArmies(fall_game, *doomed).size()), 0, "with all its armies");
    Eq(doomed->gold, 0, "and no gold");
    Eq(diplomacy::State(fall_game, other->index, doomed->index), diplomacy::kIntermediate,
       "others stand uneasy with it");
    Eq(diplomacy::Proposal(fall_game, doomed->index, other->index), diplomacy::kIntermediate,
       "both ways, proposals too");
    const Fallen* fallen = fall_game.ending.fallen.empty() ? nullptr : &fall_game.ending.fallen[0];
    Ok(fallen && fallen->side == doomed, "its fall is told");
    Ok(fallen && fallen->line >= 0 && fallen->line < game::kFallenLines, "in a line of group 11");
    Ok(fallen && fallen->boxed, "in a box while a human plays");

    // a human left with no city still plays its turn until the round's end
    auto human_fall_ptr = NewGame("ERYTHEA", Seed(87));
    Game& human_fall = *human_fall_ptr;
    for (size_t i = 0; i < human_fall.sides.size(); i++)
        human_fall.sides[i]->computer = i + 1 < human_fall.sides.size();
    game::Begin(human_fall);
    Side* me7 = human_fall.sides.back();
    for (City* city : game::SideCities(human_fall, *me7))
        city->owner_index = human_fall.sides[0]->index;
    for (size_t i = 1; i < human_fall.sides.size(); i++) EndTurn(human_fall);
    Eq(human_fall.side, me7, "the human's turn comes round without a city");
    Ok(me7->alive, "and it is still in the game");
    EndTurn(human_fall);
    Ok(!me7->alive, "the round's end puts it out");
    Ok(me7->computer, "a fallen human is the computer's from then on");
    Ok(human_fall.ending.no_humans, "the last human's fall is said");
    Ok(human_fall.no_humans_said, "once");
    for (size_t i = 0; i < human_fall.sides.size(); i++) EndTurn(human_fall);
    Ok(!human_fall.ending.no_humans, "and only once");
    auto half_game_ptr = NewGame("ERYTHEA", Seed(83));
    Game& half_game = *half_game_ptr;
    for (Side* side : half_game.sides) side->computer = false;
    Side* holder = half_game.sides[0];
    for (size_t i = 1; i < half_game.sides.size(); i++) half_game.sides[i]->alive = false;
    int standing = static_cast<int>(half_game.map->cities.size()), given = 0;
    for (auto& city : half_game.map->cities) city.owner_index = kNone;
    for (auto& city : half_game.map->cities)
        if (given < standing / 2) {
            city.owner_index = holder->index;
            given++;
        }
    Ok(!game::CheckEnd(half_game).over, "exactly half is not enough");
    for (auto& city : half_game.map->cities)
        if (city.owner_index == kNone) {
            city.owner_index = holder->index;
            break;
        }
    auto majority = game::CheckEnd(half_game);
    Ok(majority.won, "more than half wins");
    Ok(!majority.over, "and the game goes on, to be looked over");
    Eq(majority.winner, holder, "and names the winner");
    Ok(!game::CheckEnd(half_game).won, "which is said once");
    auto surrender_game_ptr = NewGame("ERYTHEA", Seed(84));
    Game& surrender_game = *surrender_game_ptr;
    Side* human = surrender_game.sides[0];
    human->computer = false;
    for (size_t i = 1; i < surrender_game.sides.size(); i++)
        surrender_game.sides[i]->computer = true;
    for (auto& city : surrender_game.map->cities) city.owner_index = kNone;
    Side* rival = surrender_game.sides[1];
    int city_count = static_cast<int>(surrender_game.map->cities.size());
    for (int i = 0; i < city_count; i++) {
        City& city = surrender_game.map->cities[i];
        if (i + 1 <= city_count * 3 / 4)
            city.owner_index = human->index;
        else if (i + 1 == city_count)
            city.owner_index = rival->index;
    }
    auto offer = game::CheckEnd(surrender_game);
    Ok(offer.surrender, "a dominant human is offered surrender");
    Ok(!offer.over, "but the game is not over");
    Ok(surrender_game.surrender_offered, "and the flag is set");
    Ok(!game::CheckEnd(surrender_game).surrender, "the offer is made once");
    surrender_game.ending = Ending{};
    Ok(game::SurrenderPending(surrender_game),
       "the offer waits through the computer turns that clear the round's finding");
    surrender_game.surrender_shown = true;
    Ok(!game::SurrenderPending(surrender_game), "and is put to the player once");
    surrender_game.surrender_shown = false;
    auto ruins_game_ptr = NewGame("ERYTHEA", Seed(85));
    Game& ruins_game = *ruins_game_ptr;
    Side* side5 = ruins_game.sides[0];
    for (size_t i = 1; i < ruins_game.sides.size(); i++) ruins_game.sides[i]->alive = false;
    for (Side* side : ruins_game.sides) side->computer = false;
    for (auto& city : ruins_game.map->cities) city.owner_index = kNone;
    ruins_game.map->cities[0].owner_index = side5->index;
    for (size_t i = 1; i < ruins_game.map->cities.size(); i++)
        ruins_game.map->cities[i].razed = true;
    Ok(game::CheckEnd(ruins_game).won, "one city among ruins is still more than half");
    game::AcceptSurrender(surrender_game, *human);
    Ok(surrender_game.won, "surrender accepted wins the game");
    Ok(!rival->alive, "and the computer sides are out");
}

// ------------------------------------------------------------ saving a game

/**
 * @brief Checks that a saved game reads back whole: armies, cities, sides,
 * sites, the quest, items and diplomacy, and that the reloaded game plays on.
 */
static void TestSave() {
    printf("save and load\n");
    auto game_ptr = NewGame("ERYTHEA", SeedWith(91, {{"quests", 1}, {"diplomacy", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    while (side && game.turn <= 8) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    Army* hero = nullptr;
    for (Army* army : game.armies)
        if (army->type == armytype::kHero) hero = army;
    Ok(hero != nullptr, "the game has a hero to save");
    if (!hero) return;
    Side* owner = game.map->Side(hero->owner);
    owner->quest = CityQuest(quest::kOccupy, hero, &game.map->cities[4]);
    hero->items = {&game.map->items[0]};
    game.map->items[0].status = 3;
    game.armies[0]->group = 4;
    game.armies[1]->group = 4;
    game.armies[0]->fortified = true;
    std::string text = save::Encode(game);
    Ok(text.size() > 1000, "the save has content");
    auto loaded_ptr = save::Decode(text, kData);
    Game& loaded = *loaded_ptr;
    Eq(loaded.turn, game.turn, "the turn survives");
    Eq(loaded.current, game.current, "whose turn it is survives");
    Eq(loaded.armies.size(), game.armies.size(), "every army survives");
    Eq(loaded.rng.state, game.rng.state, "the dice carry on where they left off");
    for (size_t i = 0; i < game.armies.size() && i < loaded.armies.size(); i++) {
        Army* saved = game.armies[i];
        Army* restored = loaded.armies[i];
        std::string label = std::to_string(i);
        Eq(restored->x, saved->x, "army " + label + " keeps its place");
        Eq(restored->type, saved->type, "army " + label + " keeps its type");
        Eq(restored->strength, saved->strength, "army " + label + " keeps its strength");
        Eq(restored->moves, saved->moves, "army " + label + " keeps its movement");
        Eq(restored->owner, saved->owner, "army " + label + " keeps its owner");
        Eq(restored->group, saved->group, "army " + label + " keeps the group it moves with");
        Eq(restored->fortified, saved->fortified, "army " + label + " stays dug in");
    }
    for (size_t i = 0; i < game.map->cities.size(); i++) {
        const City& saved = game.map->cities[i];
        const City& restored = loaded.map->cities[i];
        std::string label = std::to_string(i);
        Eq(restored.owner_index, saved.owner_index, "city " + label + " keeps its owner");
        Eq(restored.producing, saved.producing, "city " + label + " keeps its production");
        Eq(restored.slots.size(), saved.slots.size(), "city " + label + " keeps its slots");
    }
    for (size_t i = 0; i < game.sides.size(); i++) {
        Side* saved = game.sides[i];
        Eq(loaded.sides[i]->gold, saved->gold, saved->name + " keeps its gold");
        Eq(loaded.sides[i]->alive, saved->alive, saved->name + " keeps its standing");
        Eq(loaded.sides[i]->diplo_score, saved->diplo_score, saved->name + " keeps its score");
    }
    for (size_t i = 0; i < game.map->sites.size(); i++) {
        const Site& saved = game.map->sites[i];
        const Site& restored = loaded.map->sites[i];
        std::string label = std::to_string(i);
        Eq(restored.content, saved.content, "site " + label + " keeps its contents");
        Eq(restored.searched, saved.searched, "site " + label + " remembers being searched");
        Eq(restored.rich, saved.rich, "site " + label + " keeps its rich flag");
    }
    Side* owner2 = loaded.map->Side(owner->index);
    Ok(owner2->quest != nullptr, "the quest survives");
    if (owner2->quest) {
        Eq(owner2->quest->type, quest::kOccupy, "with its type");
        Eq(owner2->quest->city ? owner2->quest->city->index : kNone, game.map->cities[4].index,
           "and its target city");
        Ok(owner2->quest->hero != nullptr, "and its hero");
        if (owner2->quest->hero) Eq(owner2->quest->hero->type, armytype::kHero, "which is a hero");
    }
    bool carried = false;
    for (Army* army : loaded.armies)
        if (!army->items.empty()) carried = true;
    Ok(carried, "carried items survive");
    Eq(diplomacy::State(loaded, 0, 1), diplomacy::State(game, 0, 1),
       "the diplomatic state survives");
    Side* side2 = loaded.sides[loaded.current];
    int before = loaded.turn;
    for (size_t i = 0; i < loaded.sides.size() * 2; i++) {
        if (!side2) break;
        PlayAi(loaded, *side2);
        side2 = EndTurn(loaded);
    }
    Ok(loaded.turn > before, "the reloaded game plays on");
    // a file round trip
    std::string path = (std::filesystem::temp_directory_path() / "w2-test-save.json").string();
    WriteFile(path, text);
    auto from_file = save::Decode(MustRead(path), kData);
    Eq(from_file->armies.size(), game.armies.size(), "a save written to a file reads back");
    RemoveFile(path);
}

// ------------------------------------------------------------- the hidden map

/**
 * @brief Checks the hidden map: what a side sees at the start, how far a
 * stack sees, that paths stay out of the dark, and whose map a computer
 * opening the game plays to.
 */
static void TestHiddenMap() {
    printf("hidden map\n");
    auto off = NewGame("ERYTHEA", Seed(101));
    Ok(game::Seen(*off, 0, 5, 5), "with the option off every tile is seen");
    Eq(game::Reveal(*off, 0, 5, 5, false), 0, "and nothing is revealed");
    auto game_ptr = NewGame("ERYTHEA", SeedWith(101, {{"hiddenMap", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Ok(game::Seen(game, side->index, side->capital->x, side->capital->y), "its capital is seen");
    Ok(!game::Seen(game, side->index, 0, 0), "the far corner is not");
    auto dark_game_ptr = NewGame("ERYTHEA", SeedWith(102, {{"hiddenMap", 1}}));
    Game& dark_game = *dark_game_ptr;
    int open_x = kNone, open_y = kNone;
    for (int y = 20; y <= 60; y++)
        for (int x = 20; x <= 60; x++)
            if (open_x == kNone && !game::CityAt(dark_game, x, y)) {
                open_x = x;
                open_y = y;
            }
    // side 20 is nobody, so its map starts completely dark
    Eq(game::Reveal(dark_game, 20, open_x, open_y, false), 9, "in the open a stack sees 3x3");
    Eq(game::Reveal(dark_game, 20, open_x, open_y, false), 0,
       "seeing it again reveals nothing new");
    Eq(game::Reveal(dark_game, 20, open_x + 20, open_y, true), 25, "a flying stack sees 5x5");
    City& city = dark_game.map->cities[0];
    Eq(game::Reveal(dark_game, 21, city.x, city.y, false), 25, "standing on a city sees 5x5");
    Army* army = game::SideArmies(game, *side)[0];
    army->moves = 99;
    City* unseen = nullptr;
    for (auto& candidate : game.map->cities)
        if (!unseen && !game::Seen(game, side->index, candidate.x, candidate.y) &&
            candidate.owner_index != side->index)
            unseen = &candidate;
    Ok(unseen != nullptr, "there is a city the side has not seen");
    if (unseen)
        Ok(!movement::FindPath(game, {army}, army->x, army->y, unseen->x, unseen->y),
           "a side cannot path into the dark");
    auto count = [&]() {
        int seen = 0;
        auto explored = game.explored.find(side->index);
        if (explored != game.explored.end())
            for (uint8_t tile_seen : explored->second)
                if (tile_seen) seen++;
        return seen;
    };
    int before = count();
    bool found = false;
    int target_x = 0, target_y = 0;
    for (int dx = -3; dx <= 3; dx++) {
        for (int dy = -3; dy <= 3; dy++) {
            int x = army->x + dx, y = army->y + dy;
            if (!found && x >= 0 && y >= 0 && game::Seen(game, side->index, x, y) &&
                (x != army->x || y != army->y)) {
                auto path = movement::FindPath(game, {army}, army->x, army->y, x, y);
                if (path && !path->empty() && path->back().cost < movement::kPastShore) {
                    found = true;
                    target_x = x;
                    target_y = y;
                }
            }
        }
    }
    if (found) {
        movement::MoveTo(game, {army}, target_x, target_y);
        int after = count();
        Ok(after > before, Fmt("walking uncovered %d more tiles", after - before));
    }
    Side* other = nullptr;
    for (Side* candidate : game.sides)
        if (candidate->index != side->index) other = candidate;
    Ok(!game::Seen(game, other->index, side->capital->x, side->capital->y),
       "another side has not seen our capital");
    // a computer that opens the game plays to the next human's hidden map
    auto computer_first_ptr = NewGame("ERYTHEA", SeedWith(103, {{"hiddenMap", 1}}));
    Game& computer_first = *computer_first_ptr;
    for (size_t i = 0; i < computer_first.sides.size(); i++)
        computer_first.sides[i]->computer = i != 2;
    Side* first = game::Begin(computer_first);
    Ok(first == computer_first.sides[0], "a computer opens the game");
    Ok(game::NextHuman(computer_first) == computer_first.sides[2],
       "the screen is the human's who follows it");
    computer_first.sides[2]->computer = true;
    Ok(game::NextHuman(computer_first) == nullptr, "and nobody's with no human playing");
}

// --------------------------------------------------------------------- bugs

/**
 * @brief Checks that the original's bugs are reproduced by default and that
 * every bug-compatibility flag is read somewhere in the engine.
 */
static void TestBugFlags() {
    printf("bug compatibility\n");
    Ok(rules::bugs.hero_experience_reads_attacker_types,
       "the original's bugs are reproduced by default");
    // every flag must actually control something: a flag nothing reads is a
    // promise the engine does not keep
    std::set<std::string> used;
    for (const char* module : {"hero", "combat", "game", "move", "ai", "site", "quest"}) {
        auto text = ReadFile(std::string("cpp/src/warlords/") + module + ".cpp");
        if (!text) continue;
        std::regex flag_read("rules::bugs\\.(\\w+)");
        for (auto it = std::sregex_iterator(text->begin(), text->end(), flag_read);
             it != std::sregex_iterator(); ++it)
            used.insert((*it)[1]);
    }
    for (const char* name : {"hero_experience_reads_attacker_types"})
        Ok(used.count(name), std::string("the ") + name + " flag is read by the engine");
}

// ------------------------------------------------------------ the computer AI

/**
 * @brief Checks the map evaluator (warlords/mapeval, docs/mapeval.md) and the
 * world makers it judges.
 *
 * The hand-made maps break no hard rule, and each rule fires when a map is
 * broken on purpose. Also covers fair capitals, the scenario generator's
 * worlds and names, New Scenario's list and a generated world's menu picture.
 */
static void TestMapEval() {
    printf("map evaluator\n");
    for (const char* scenario : {"ERYTHEA", "HADESHA", "ISLADIA", "DRAGON", "SORCERY"}) {
        auto map = scn::Load(kData + "/" + scenario, scenario);
        auto evaluation = mapeval::Evaluate(*map, scenario);
        std::string first = "";
        for (auto& issue : evaluation.issues)
            if (mapeval::IsHard(issue.check)) {
                first = issue.check + ": " + issue.detail;
                break;
            }
        Ok(evaluation.Passed(), std::string(scenario) + " breaks no hard rule " + first);
        Ok(evaluation.Metric("clarkEvansR") > 1.3,
           std::string(scenario) + "'s cities are spread out (Clark-Evans R > 1.3)");
    }
    auto map = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
    Map& broken = *map;
    auto index_of = [&](int x, int y) { return y * kMapW + x; };
    auto tile_of = [&](int type) {
        for (int tile = 0; tile < 256; tile++)
            if (broken.terrain_type[tile] == type) return tile;
        return -1;
    };
    // a lone road piece on open plain, away from everything
    int lone_x = -1, lone_y = -1;
    for (int y = 3; y < kMapH - 3 && lone_x < 0; y++)
        for (int x = 3; x < kMapW - 3 && lone_x < 0; x++) {
            bool clear = true;
            for (int dy = -2; dy <= 2 && clear; dy++)
                for (int dx = -2; dx <= 2 && clear; dx++)
                    clear = scn::TerrainAt(broken, x + dx, y + dy) == movement::kPlain &&
                            scn::RoadAt(broken, x + dx, y + dy) == 0;
            if (clear) {
                lone_x = x;
                lone_y = y;
            }
        }
    Ok(lone_x >= 0, "found open plain for a stray road");
    broken.roads[index_of(lone_x, lone_y)] = 1;  // piece 1: east-west
    // a city on top of another, a name used twice, a signpost that lies
    broken.cities[1].x = broken.cities[0].x + 1;
    broken.cities[1].y = broken.cities[0].y;
    broken.cities[3].name = broken.cities[2].name;
    broken.signs[0].lines = {broken.cities[5].name, "999 leagues north"};
    // a ruin walled in by mountains
    const Site& walled = broken.sites[0];
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = walled.x + dx, y = walled.y + dy;
            if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= kMapW || y >= kMapH) continue;
            broken.tiles[index_of(x, y)] = tile_of(movement::kMountains);
            broken.roads[index_of(x, y)] = 0;
        }
    auto broken_eval = mapeval::Evaluate(broken, "broken");
    Ok(!broken_eval.Passed(), "a broken map fails");
    for (const char* check : {"road-to-nowhere", "dead-end", "city-gap", "city-terrain",
                              "city-name", "signpost", "unreachable"})
        Ok(broken_eval.Count(check) > 0, std::string("the broken map trips ") + check);
    // a second side's capital next door to the first's
    {
        auto near = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
        City* home = near->sides[0].capital;
        City* next = nullptr;
        for (auto& candidate : near->cities)
            if (&candidate != home &&
                (!next || std::hypot(candidate.x - home->x, candidate.y - home->y) <
                                  std::hypot(next->x - home->x, next->y - home->y)))
                next = &candidate;
        near->sides[1].capital = next;
        Ok(mapeval::Evaluate(*near, "near").Count("capital-gap") > 0,
           "two capitals side by side trip capital-gap");
    }
    // the fair capitals: the same world, its capitals chosen for a fair start
    {
        Rng classic_rng(11), fair_rng(11);
        randommap::Options options;
        options.data_dir = kData;
        options.rng = &classic_rng;
        auto classic_files = randommap::GenerateNow(options);
        options.rng = &fair_rng;
        options.fair_capitals = true;
        auto fair_files = randommap::GenerateNow(options);
        Ok(classic_files["SCN"] != fair_files["SCN"], "fair capitals make a different start");
        auto fair = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", fair_files);
        std::set<City*> capitals;
        for (auto& fair_side : fair->sides) capitals.insert(fair_side.capital);
        Eq(static_cast<int>(capitals.size()), 8, "fair capitals: eight, one a side");
        auto fair_eval = mapeval::Evaluate(*fair, "fair#11");
        Eq(fair_eval.Count("capital-gap"), 0, "fair capitals are never side by side");
        Ok(fair_eval.Metric("remoteCityFrac") <= 0.05,
           "fair capitals leave hardly a city far from every capital");
        Ok(fair_eval.Metric("capitalGapMinTurns") >= 2.5,
           "fair capitals are as far apart as the designers' (2.5 turns)");
    }
    // the reference and the score: the designers' own maps score nothing
    std::vector<mapeval::Evaluation> refs;
    for (const char* scenario : {"ERYTHEA", "HADESHA"})
        refs.push_back(mapeval::Evaluate(*scn::Load(kData + "/" + scenario, scenario), scenario));
    auto ref = mapeval::MakeReference(refs);
    for (auto& designed : refs)
        Eq(mapeval::ScoreAgainst(designed, ref).penalty, 0.0,
           designed.name + " sits inside its own reference");
    // a generated world is evaluated like any other, straight from memory
    Rng rng(1);
    randommap::Options options;
    options.data_dir = kData;
    options.rng = &rng;
    auto files = randommap::GenerateNow(options);
    auto made = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", files);
    auto made_eval = mapeval::Evaluate(*made, "classic#1");
    Eq(static_cast<int>(made_eval.Metric("cities")), 80, "a generated world is evaluated");
    Ok(mapeval::ToJson(made_eval).Dump().find("\"metrics\"") != std::string::npos,
       "the evaluation writes as JSON");
    // the scenario generator (warlords/scengen): every shape, a few seeds, and
    // every world it makes keeps the hard rules
    for (auto shape :
         {scengen::Shape::kContinents, scengen::Shape::kPangaea, scengen::Shape::kIslands}) {
        for (int seed = 1; seed <= 3; seed++) {
            Rng rng(seed), again(seed);
            randommap::Options options;
            options.data_dir = kData;
            options.rng = &rng;
            options.scenario = true;
            options.world.shape = shape;
            auto files = randommap::GenerateNow(options);
            options.rng = &again;
            Ok(files == randommap::GenerateNow(options),
               std::string("the scenario generator is deterministic: ") +
                       scengen::ShapeName(shape));
            auto world = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", files);
            auto evaluation = mapeval::Evaluate(
                    *world, std::string(scengen::ShapeName(shape)) + "#" + std::to_string(seed));
            std::string first;
            for (auto& issue : evaluation.issues)
                if (mapeval::IsHard(issue.check)) {
                    first = ": " + issue.check + " " + issue.detail;
                    break;
                }
            Ok(evaluation.Passed(), evaluation.name + " keeps every hard rule" + first);
            Eq(static_cast<int>(evaluation.Metric("cities")), 80,
               evaluation.name + " has the 80 cities asked for");
            Ok(evaluation.Metric("cityRoadCoverage") >= 0.74,
               evaluation.name + " puts its cities on roads, as the designers do");
            Eq(world->options.military_advisor, 1,
               evaluation.name + " reads as an ordinary scenario");
        }
    }
    // names: no wider than "The Frozen Steppe" in CHANCE17, the font New
    // Scenario's list draws them in, measured from the font's own metrics
    {
        std::string fin = *ReadFile(kData + "/CHANCE17.FIN");
        int count = static_cast<uint8_t>(fin[0]), first_char = static_cast<uint8_t>(fin[1]);
        auto width = [&](const std::string& text) {
            int pixels = 0;
            for (unsigned char glyph : text)
                if (glyph >= first_char && glyph < first_char + count)
                    pixels += static_cast<uint8_t>(fin[11 + count + (glyph - first_char)]);
            return pixels;
        };
        int widest = 0;
        bool fit = true;
        for (auto shape :
             {scengen::Shape::kContinents, scengen::Shape::kPangaea, scengen::Shape::kIslands})
            for (long seed = 1; seed <= 600; seed++) {
                auto title = scengen::MakeTitle(shape, seed);
                widest = std::max(widest, width(title.name));
                if (title.name.empty() || title.name.size() > 20 || title.text.empty() ||
                    title.text.size() > 30)
                    fit = false;
            }
        Ok(widest <= width("The Frozen Steppe"),
           "every name fits New Scenario's list box (" + std::to_string(widest) + " px)");
        Ok(fit, "and its 20 characters, and its line its 30");
    }
    // New Scenario's list: SCENARIO.DAT's six, then a generated world's folder
    // with its NAME.INF (warlords/scenarios), in a data folder of its own
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / "w2test-scenarios";
        fs::remove_all(root);
        fs::create_directories(root / "DATA");
        fs::create_directories(root / "GEN7");
        fs::create_directories(root / "NOINFO");
        fs::copy_file(kData + "/DATA/SCENARIO.DAT", root / "DATA" / "SCENARIO.DAT");
        scenarios::Entry made{"Continents 7", "GEN7", "A new world, seed 7", 80, 40, 8, true};
        WriteFile((root / "GEN7" / "GEN7.INF").string(), scenarios::Info(made));
        WriteFile((root / "GEN7" / "GEN7.SCN").string(), "");
        WriteFile((root / "NOINFO" / "NOINFO.SCN").string(), "");
        Ok(scenarios::Remember(root.string(), "NOINFO") &&
                   scenarios::Remember(root.string(), "GEN7") &&
                   scenarios::Remember(root.string(), "GEN7"),
           "SCENGEN.LST remembers the generated folders");
        Eq(*ReadFile((root / "DATA" / "SCENGEN.LST").string()), std::string("GEN7\nNOINFO\n"),
           "once each, sorted");
        auto list = scenarios::List(root.string());
        Eq(static_cast<int>(list.size()), 7,
           "SCENARIO.DAT's six and the one listed world with a NAME.INF");
        Eq(list[3].dir, std::string("Erythea"), "SCENARIO.DAT's own order first");
        Eq(list.back().name, std::string("Continents 7"),
           "the generated world after them, by its NAME.INF");
        Eq(list.back().cities, 80, "with its cities");
        Ok(list.back().generated && !list[0].generated, "and marked as generated");
        auto parsed = scenarios::ParseInfo(scenarios::Info(made), "GEN7");
        Ok(parsed && parsed->text == made.text && parsed->players == 8,
           "NAME.INF reads back as written");
        fs::remove_all(root);
    }
    // the start menu's picture for a generated world: a .PCK the game reads
    {
        auto map = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
        auto pic = preview::MenuPicture(*map, kData);
        Eq(pic.w, 264, "the menu picture fills the frame's width");
        Eq(pic.h, 225, "and its height");
        std::string path =
                (std::filesystem::temp_directory_path() / "w2test-SCENARIO.PCK").string();
        WriteFile(path, pck::Encode(pic));
        auto read = pck::Decode(path);
        Ok(read->w == pic.w && read->h == pic.h && read->px == pic.px,
           "the .PCK it is written as decodes to the same picture");
        std::filesystem::remove(path);
        std::string png = preview::Png(*map, kData);
        Ok(png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "the preview is a PNG");
    }
}

/**
 * @brief Checks the random map generator (warlords/randommap.cpp,
 * docs/re/random_map.md): what it makes, and a game played on it and saved.
 */
static void TestRandomMap() {
    printf("random map\n");
    auto make = [](int seed, std::array<int, 4> sliders, bool allies) {
        Rng rng(seed);
        randommap::Options options;
        options.data_dir = kData;
        options.rng = &rng;
        options.sliders = sliders;
        options.allies = allies;
        return randommap::GenerateNow(options);
    };
    auto made = make(7, {{3, 3, 2, 3}}, false), remade = make(7, {{3, 3, 2, 3}}, false);
    for (const char* ext : randommap::kFiles) {
        Ok(made.count(ext) && !made[ext].empty(),
           std::string("the generator writes RANDOM.") + ext);
        Ok(made[ext] == remade[ext], std::string("the same dice make the same RANDOM.") + ext);
    }
    Eq(static_cast<int>(made["SCN"].size()), 12001,
       "RANDOM.SCN is Erythea's .SCN rewritten (4fef:1001)");
    Eq(static_cast<int>(made["MAP"].size()), 2 * 112 * 156, "RANDOM.MAP is a word a tile");
    randommap::Install(kData, made);
    auto map = scn::Load(kData + "/RANDOM", "RANDOM");
    Eq(static_cast<int>(map->cities.size()), 80,
       "Cities at its third place gives RANDOM.DAT's 80 (4bed:0000)");
    Eq(static_cast<int>(map->sites.size()), 40, "forty sites (513d:003a)");
    Ok(map->signs.size() >= 41 && map->signs.size() <= 70, "1d30+40 signposts (4fef:113d)");
    std::set<City*> capitals;
    for (auto& side : map->sides) {
        Ok(side.capital && side.capital->owner == &side,
           side.name + " holds its capital (513d:0689)");
        capitals.insert(side.capital);
    }
    Eq(static_cast<int>(capitals.size()), 8, "eight capitals, one a side");
    for (auto& city : map->cities) {
        Eq(map->terrain_type[scn::TileAt(*map, city.x, city.y) % 256], 10,
           city.name + " stands on a castle tile");
        Ok(!city.produces.empty(), city.name + " makes something (513d:161d)");
        bool capital = city.owner != nullptr;
        Ok(city.income >= (capital ? 33 : 15) && city.income <= (capital ? 40 : 28),
           city.name + "'s income is value x 2 + 1d8 + 14 (513d:1171)");
        auto description = map->city_text.find(city.index);
        Ok(description != map->city_text.end() &&
                   StartsWith(description->second[0], city.name + " is a"),
           city.name + " has its description");
    }
    Types types;
    armytype::Load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    auto magical = [&](int type) {
        return types.ById(type) && types.ById(type)->bonus[48] != 0;
    };
    bool none = true;
    for (auto& city : map->cities)
        for (int produced : city.produces)
            if (magical(produced)) none = false;
    Ok(none, "without allies no city makes a magical army (513d:161d, 1c1d)");
    int allies = 0;
    for (int seed = 1; seed <= 10; seed++) {
        randommap::Install(kData, make(seed, {{3, 3, 2, 3}}, true));
        auto allied = scn::Load(kData + "/RANDOM", "RANDOM");
        for (auto& city : allied->cities)
            for (int produced : city.produces)
                if (magical(produced)) allies++;
    }
    Ok(allies > 0, "with allies on, 2d3 cities may make them (513d:1b3f)");
    randommap::Install(kData, make(3, {{0, 0, 6, 0}}, false));
    Eq(static_cast<int>(scn::Load(kData + "/RANDOM", "RANDOM")->cities.size()), 100,
       "Cities at its last place: 80 + 20");
    randommap::Install(kData, make(3, {{6, 6, 0, 6}}, false));
    Eq(static_cast<int>(scn::Load(kData + "/RANDOM", "RANDOM")->cities.size()), 70,
       "Cities at its first place: 80 - 10");
    Rng slider_rng(5);
    auto rolled = randommap::Settle({{7, 7, 7, 7}}, slider_rng);
    bool in_range = true;
    for (int slider : rolled)
        if (slider < 0 || slider > 6) in_range = false;
    Ok(in_range, "a slider left to chance is rolled 1d7-1 (7bab:10e8)");
    // the shipped RANDOM folder is the original's last world, left alone
    std::ifstream disk(kData + "/RANDOM/RANDOM.SCN", std::ios::binary);
    if (disk) {
        std::string on_disk((std::istreambuf_iterator<char>(disk)),
                            std::istreambuf_iterator<char>());
        Ok(on_disk != made["SCN"], "the made world is kept in memory, not written over RANDOM.SCN");
    }

    // a game on it, and a save that carries the world with it
    randommap::Install(kData, made);
    game::NewGameOptions options;
    options.seed = 4;
    auto game_ptr = game::NewGame(kData, "RANDOM", options);
    Game& game = *game_ptr;
    for (Side* player : game.sides) player->computer = true;
    Side* side = game::Begin(game);
    while (side && game.turn <= 6) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    Eq(game.turn, 7, "the computer players play a random world");
    std::string text = save::Encode(game);
    randommap::Install(kData, make(8, {{3, 3, 2, 3}}, false));
    auto reloaded = save::Decode(text, kData);
    Ok(reloaded->map->tiles == game.map->tiles,
       "a saved random world comes back with its own map");
    bool names = reloaded->map->cities.size() == game.map->cities.size();
    for (size_t i = 0; names && i < game.map->cities.size(); i++)
        names = reloaded->map->cities[i].name == game.map->cities[i].name;
    Ok(names, "... and its own cities");
    Eq(reloaded->map->signs.size(), game.map->signs.size(), "... and its own signposts");
}

/**
 * @brief Checks the computer players: the character cards, how a level and a
 * card set up a side's AI, city claims, diplomacy and fog for the computers,
 * the AI helpers, and that the AI's data survives a save.
 */
static void TestComputerPlayers() {
    printf("computer players\n");
    namespace core = ai::core;
    namespace groups = ai::groups;
    for (int level = 0; level <= 2; level++)
        Eq(aicard::Count(kData, level), 9, "nine cards for level " + std::to_string(level));
    auto warlord_card = *aicard::Load(kData, 2, 0);
    Eq(warlord_card.groups, 4, "the Standard Warlord runs four assault groups");
    Eq(warlord_card.raze, 5, "and razes 5 in 1000");
    Eq(warlord_card.human_share, 3, "and turns on a human with 3x10+1d10 percent");
    Eq(warlord_card.solidarity, 1, "and stands with the other computers");
    auto knight_card = *aicard::Load(kData, 0, 0);
    Eq(knight_card.cautious, 1, "the Standard Knight is cautious");
    Eq(knight_card.groups, 1, "and runs one group");
    Eq(aicard::Describe(kData, 2, 1)->first, "Attila the Hun",
       "a card's name is the first line of its .DSC");
    auto lords = aicard::Deck(kData, 1);
    Eq(static_cast<int>(lords.size()), 9, "the setup screen lists a Lord's nine characters");
    Eq(lords[0], "Standard Lord", "the Standard one first");
    Eq(lords[2], "Roland the Rabid", "each by its .DSC's first line");
    {
        // a character chosen on the setup screen is the card the side plays
        GameOpts rabid_opts = Seed(70);
        rabid_opts.sides[0] = SideSetup(false, 0);
        game::SideSetup rabid = SideSetup(true, 1);
        rabid.card = 2;
        rabid.name = "Rabids";
        rabid_opts.sides[1] = rabid;
        auto rabid_game = NewGame("ERYTHEA", rabid_opts);
        Side& rabid_side = rabid_game->map->sides[1];
        Eq(rabid_side.card, 2, "the side keeps its character");
        Eq(rabid_side.name, "Rabids", "and the name it was given");
        Eq(rabid_side.ai->max_groups, 2, "Roland the Rabid runs two assault groups");
        Eq(rabid_side.ai->raze, 14, "razes 14 in 1000");
        Eq(rabid_side.ai->sack, 100, "sacks 100");
        Eq(rabid_side.ai->early, 1, "and takes early vengeance on a human");
        Eq(rabid_game->map->sides[2].ai->max_groups, 4,
           "a side left alone plays its Standard card");
    }
    Eq(Rng(5).Dice(1, 0, 3), 3, "dice(1, 0, 3) is 3");
    Eq(core::Dist(0, 0, 3, 4), 5, "distance is Euclidean");
    Eq(core::Dist(0, 0, 1, 1), 1, "and truncated");

    GameOpts opts = Seed(71);
    for (int i = 0; i <= 7; i++) opts.sides[i] = SideSetup(true, i % 3);
    opts.sides[0] = SideSetup(false, 0);
    auto game_ptr = NewGame("ERYTHEA", opts);
    Game& game = *game_ptr;
    Side& knight = game.map->sides[3];
    Side& lord = game.map->sides[1];
    Side& warlord = game.map->sides[2];
    Eq(knight.level, 0, "side 3 plays as a Knight");
    Eq(knight.ai->max_groups, 1, "a Knight runs one group");
    Eq(knight.ai->cautious, 1, "and is cautious");
    Ok(!knight.ai->bold, "and never attacks a side it is not at war with");
    Eq(lord.ai->max_groups, 3, "a Lord three");
    Ok(lord.ai->bold, "a Lord is bold -- the level sets it and a card cannot clear it");
    Eq(warlord.ai->max_groups, 4, "a Warlord four");
    Ok(warlord.ai->human_share >= 31 && warlord.ai->human_share <= 40,
       "a Warlord turns on a human holding 31-40% of the world: " +
               std::to_string(warlord.ai->human_share));
    Ok(knight.ai->human_share >= 81 && knight.ai->human_share <= 90, "a Knight at 81-90%");
    Eq(game.map->fight_order[3][8], warlord_card.fight_order[8],
       "the card sets the side's fight order");
    int unclaimed = 0;
    for (auto& city : game.map->cities)
        if (city.claim == kNone || city.claim == core::kNeutral) unclaimed++;
    Eq(unclaimed, 0, "the computers claim every city between them (623c:010b)");
    for (Side* player : game.sides)
        Eq(player->capital->claim, player->index, player->name + " claims its capital");
    for (Side* player : game.sides)
        Ok(player->diplo_score >= 1 && player->diplo_score <= 8,
           "a diplomatic score starts at 1d8");

    GameOpts greatest_opts = opts;
    greatest_opts.seed = 72;
    greatest_opts.greatest = true;
    greatest_opts.options = {{"diplomacy", 1}};
    auto greatest_ptr = NewGame("ERYTHEA", greatest_opts);
    Game& greatest = *greatest_ptr;
    Ok(greatest.map->sides[0].diplo_score > 400,
       "I am the Greatest: a human's score starts above 400");
    Ok(greatest.map->sides[1].diplo_score <= 8, "a computer's does not");
    greatest.diplomacy.state[1 * 8 + 0] = diplomacy::kWar;
    greatest.diplomacy.state[0 * 8 + 1] = diplomacy::kWar;
    auto fights = groups::FightingHumans(greatest);
    Ok(fights[1], "a computer at war with the human is marked");
    ai::diplo::Phase(greatest, greatest.map->sides[1]);
    Eq(diplomacy::Proposal(greatest, 1, 2), diplomacy::kPeace,
       "and no computer proposes war on it");

    GameOpts hidden_opts = opts;
    hidden_opts.seed = 73;
    hidden_opts.options = {{"hiddenMap", 1}};
    auto hidden_ptr = NewGame("ERYTHEA", hidden_opts);
    Game& hidden = *hidden_ptr;
    Side& human_s = hidden.map->sides[0];
    Side& comp = hidden.map->sides[1];
    auto hgrid = movement::Grid(hidden, human_s.index);
    auto hpd = movement::Prepare(hidden, hgrid, human_s.index, movement::kLand, 0, 0);
    auto cpd = movement::Prepare(hidden, hgrid, comp.index, movement::kLand, 0, 0);
    int fogged = 0;
    for (int k2 = 0; k2 < hidden.map->width * hidden.map->height; k2++) {
        if (hpd[k2] == movement::kShut && cpd[k2] != movement::kShut) fogged++;
    }
    Ok(fogged > 1000, "unseen ground blocks the human's paths only: " + std::to_string(fogged));

    Army* army = nullptr;
    for (Army* candidate : game.armies)
        if (candidate->owner == warlord.index) army = candidate;
    auto sel = core::Select(game, {army});
    Eq(core::Odds(game, sel, 0, 0), 100, "nothing to fight is a sure thing");
    auto [neighbours, distances] = core::FindNeighbours(game, game.map->cities[0]);
    Eq(static_cast<int>(neighbours.size()), 6, "a city has six neighbours");
    for (int distance : distances) Ok(distance < 100, "each within reach");

    Eq(groups::PickEnemy(game, warlord), kNone, "no enemy is picked in the first turns");
    warlord.ai->battles[4] = 3;
    warlord.ai->cities_lost[4] = 2;
    Ok(groups::PickEnemy(game, warlord) != kNone, "a side that has fought us can be picked");

    City* city = &game.map->cities[0];
    for (auto& candidate : game.map->cities)
        if (&candidate != knight.capital && &candidate != warlord.capital) {
            city = &candidate;
            break;
        }
    city->owner_index = warlord.index;
    int before = warlord.diplo_score;
    Ok(groups::EarlyVengeance(game, warlord.index, *city, 0),
       "a Warlord takes vengeance on a human's city");
    Ok(warlord.diplo_score > before, "and it is an atrocity");
    Ok(!groups::EarlyVengeance(game, knight.index, *city, 0), "a Knight does not");

    Side* side = game::Begin(game);
    while (side && game.turn <= 12) {
        if (side->computer) PlayAi(game, *side);
        side = EndTurn(game);
    }
    auto grp = warlord.ai->groups[1];
    grp->active = 2;
    grp->target = 1;
    grp->rally = warlord.capital->index;
    grp->staged[2] = game.armies.back();
    auto reloaded = save::Decode(save::Encode(game), kData);
    auto& reloaded_ai = *reloaded->map->sides[warlord.index].ai;
    Eq(reloaded_ai.max_groups, warlord.ai->max_groups, "the AI data survives a save");
    Eq(reloaded_ai.groups[1]->rally, warlord.capital->index, "and its groups");
    Eq(reloaded_ai.groups[1]->staged[2], reloaded->armies.back(), "with their staged armies");
    Eq(reloaded->map->cities[4].claim, game.map->cities[4].claim, "and the cities' claims");
}

// --------------------------------------------------------------- encampments

/**
 * @brief Checks encampments: a defended stack on open ground becomes one as
 * its turn opens, fights as a fortification of 1, and none forms in a city.
 */
static void TestEncampment() {
    printf("encampments\n");
    auto game_ptr = NewGame("ERYTHEA", Seed(5));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* cap = side->capital;
    Army* army = nullptr;
    for (Army* candidate : game.armies)
        if (!army && candidate->owner == side->index && !candidate->transit) army = candidate;
    int x = kNone, y = kNone;
    for (int radius = 2; radius <= 8 && x == kNone; radius++) {
        for (int dx = -radius; dx <= radius && x == kNone; dx++) {
            for (int dy = -radius; dy <= radius && x == kNone; dy++) {
                int tile_x = cap->x + dx, tile_y = cap->y + dy;
                if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width - 1 &&
                    tile_y < game.map->height &&
                    scn::TerrainAt(*game.map, tile_x, tile_y) == movement::kPlain &&
                    scn::TerrainAt(*game.map, tile_x + 1, tile_y) == movement::kPlain &&
                    scn::RoadAt(*game.map, tile_x, tile_y) % 32 == 0 &&
                    game::ArmiesAt(game, tile_x, tile_y).empty() &&
                    game::ArmiesAt(game, tile_x + 1, tile_y).empty()) {
                    x = tile_x;
                    y = tile_y;
                }
            }
        }
    }
    Ok(army != nullptr && x != kNone, "an army and open ground to encamp on");
    if (!(army && x != kNone)) return;
    army->x = x;
    army->y = y;
    Ok(!game::TowerAt(game, x, y), "a stack on open ground is no encampment");
    game::StartTurn(game, *side);
    Ok(!game::TowerAt(game, x, y), "nor after a turn in the army cycle");
    army->fortified = true;
    game::StartTurn(game, *side);
    Ok(game::TowerAt(game, x, y), "a defended stack encamps as its turn opens");
    Eq(combat::Fortify(game, {}, x, y, combat::TerrainClass(game, x, y)), 1,
       "an encampment fights as a fortification of 1");
    auto walk = movement::MoveTo(game, {army}, x + 1, y);
    Eq(walk.steps, 1, "the encamped stack walks off");
    Ok(!game::TowerAt(game, x, y), "and the empty tile is no encampment any more");
    army->x = cap->x;
    army->y = cap->y;
    game::StartTurn(game, *side);
    Ok(!game::TowerAt(game, cap->x, cap->y), "a defended stack in a city does not encamp");
}

// ------------------------------------------------------------- going to sea

static void TestSea() {
    printf("going to sea\n");
    auto gp = NewGame("ERYTHEA", Seed(81));
    Game* g = gp.get();
    int lx = kNone, ly = kNone, wx = kNone, wy = kNone;
    for (int y = 1; y <= g->map->height - 2; y++) {
        for (int x = 1; x <= g->map->width - 2; x++) {
            int t2 = scn::TerrainAt(*g->map, x + 1, y);
            if (lx == kNone && scn::TerrainAt(*g->map, x, y) == movement::kPlain &&
                (t2 == movement::kWater || t2 == movement::kShore) &&
                game::ArmiesAt(*g, x, y).empty() && game::ArmiesAt(*g, x + 1, y).empty()) {
                lx = x;
                ly = y;
                wx = x + 1;
                wy = y;
            }
        }
    }
    Ok(lx != kNone, "Erythea has a plain beside the sea");
    const ArmyType* dragon = nullptr;
    for (auto& t : g->types.all)
        if (t.flies && Contains(t.name, "Dragon")) dragon = &t;
    auto army = [&](int type_id) { return AddArmy(*g, lx, ly, 0, type_id, 5, 20, 20); };
    Army* hero = army(armytype::kHero);
    Army* d = army(dragon->id);
    std::vector<Army*> stack = {hero, d};
    Eq(movement::ModeOf(*g, stack), movement::kFlying, "a hero with a dragon flies");
    movement::Walk(*g, stack, {movement::Step{wx, wy, 2}});
    Eq(hero->x, wx, "and flies out over the water");
    Ok(!d->at_sea && !hero->at_sea, "without either going to sea");
    Eq(movement::ModeOf(*g, stack), movement::kFlying, "so it still flies");
    movement::Walk(*g, stack, {movement::Step{lx, ly, 2}});
    Eq(hero->x, lx, "and can come back to land");
    Army* foot = army(1);
    movement::SettleSea(*g, {foot}, wx, wy, movement::kLand, false);
    Ok(foot->at_sea, "a land army ending on water is at sea");
    movement::SettleSea(*g, {foot}, lx, ly, movement::kLand, true);
    Ok(!foot->at_sea, "and ashore again on land");
    movement::SettleSea(*g, {hero, d, foot}, wx, wy, movement::kLand, false);
    Ok(!d->at_sea, "a flier never goes to sea");
    Ok(hero->at_sea && foot->at_sea, "the hero and the footman with it do");
    hero->at_sea = false;
    foot->at_sea = false;
    movement::SettleSea(*g, {foot}, wx, wy, movement::kBoat, false);
    Ok(!foot->at_sea, "a boat move leaves the flag as it was");
    hero->x = wx;
    hero->y = wy;
    d->x = wx;
    d->y = wy;
    hero->at_sea = true;
    d->at_sea = true;
    g->Remove(foot);
    auto g2 = save::Decode(save::Encode(*g), kData);
    for (Army* a : g2->armies)
        if (a->x == wx && a->y == wy) Ok(!a->at_sea, "a loaded flier and its hero are not at sea");

    auto gp2 = NewGame("ERYTHEA", Seed(81));
    g = gp2.get();
    City* mirea = nullptr;
    for (auto& c : g->map->cities)
        if (c.name == "Mirea") mirea = &c;
    Ok(mirea && mirea->owner_index == 0 && movement::IsPort(*g, *mirea), "Mirea is side 0's port");
    if (!mirea) return;
    auto& grid = movement::Grid(*g, 0);
    auto byte = [&](int x, int y) { return grid[y * g->map->width + x]; };
    Eq(scn::TerrainAt(*g->map, 86, 7), movement::kShore, "the shore runs past Mirea");
    Ok(movement::Has(byte(86, 7), movement::kWaterF), "and a shore tile is water to a walker");
    Ok(!movement::StepCost(byte(83, 9), byte(82, 9), movement::kLand, false, false, 10).has_value(),
       "so a land stack cannot step onto it from a plain");
    Army* sailor = AddArmy(*g, mirea->x, mirea->y, 0, 1, 5, 20, 20);
    auto r = movement::MoveTo(*g, {sailor}, 86, 4);
    Eq(sailor->y, 7, "leaving port it stops on the first sea tile");
    Eq(scn::TerrainAt(*g->map, sailor->x, sailor->y), movement::kShore, "the shore");
    Ok(sailor->at_sea, "at sea");
    Eq(r.spent, movement::kCost[movement::kShore], "paying the tile's cost and no water charge");
    Eq(sailor->moves, 0, "and going to sea uses up the rest of its move");
    Eq(r.stopped, "out of moves", "which ends the walk");
    sailor->moves = 20;
    r = movement::MoveTo(*g, {sailor}, 86, 4);
    Eq(sailor->y, 4, "next turn it sails out into open water");
    Ok(sailor->at_sea, "still at sea");
    Eq(sailor->moves, 20 - (7 - 4) * movement::kCost[movement::kWater], "paying 1 a tile");
    auto p = movement::FindPath(*g, {sailor}, 86, 4, 86, 5);
    Eq(p ? (*p)[0].cost : kNone, movement::kCost[movement::kWater],
       "sailing on costs no water charge either");
    r = movement::MoveTo(*g, {sailor}, mirea->x, mirea->y);
    Eq(sailor->x * 1000 + sailor->y, mirea->x * 1000 + mirea->y, "it sails back into port");
    Ok(!sailor->at_sea, "and comes ashore there");
    Eq(sailor->moves, 0, "which uses up its move too");
    City* kuuria = nullptr;
    for (auto& c : g->map->cities)
        if (c.name == "Kuuria") kuuria = &c;
    int sx = kNone, sy = kNone;
    for (int x = kuuria->x - 1; x <= kuuria->x + 2; x++) {
        for (int y = kuuria->y - 1; y <= kuuria->y + 2; y++) {
            int t = scn::TerrainAt(*g->map, x, y);
            if (sx == kNone && (t == movement::kWater || t == movement::kShore) &&
                !g->map->crossing[y * g->map->width + x] && game::ArmiesAt(*g, x, y).empty()) {
                sx = x;
                sy = y;
            }
        }
    }
    Ok(sx != kNone, "Kuuria has open water beside it");
    sailor->x = sx;
    sailor->y = sy;
    sailor->at_sea = true;
    sailor->moves = 20;
    Ok(movement::FindPath(*g, {sailor}, sx, sy, kuuria->x, kuuria->y).has_value(),
       "a stack at sea can path into the port it attacks");
}

// --------------------------------------------------------------------- sound

// Rules checked against WARLORD2.EXE in web-verification.md, one by one.
static void TestVerified() {
    printf("verified against the original\n");
    auto mk = [](Game& g, int x, int y, int owner, int type, int strength = kNone) {
        const ArmyType* t = g.types.ById(type);
        Army a;
        a.x = x;
        a.y = y;
        a.owner = owner;
        a.type = type;
        a.name = t->name;
        a.strength = strength != kNone ? strength : t->strength;
        a.max_moves = t->move;
        a.moves = t->move;
        a.upkeep = 1;
        return g.Add(a);
    };

    // the army table: the lowest free slot (1b62:0309), tiles read from the top
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        size_t n = g.armies.size();
        Army* third = g.armies[2];
        g.Remove(third);
        Army* a = mk(g, 0, 0, 0, 11);
        Eq(a->slot, 2, "a new army takes the lowest free slot");
        Eq(static_cast<int>(std::find(g.armies.begin(), g.armies.end(), a) - g.armies.begin()), 2,
           "and stands in slot order");
        Eq(g.armies.size(), n, "the table is as long as before");
        Army* b = mk(g, 0, 0, 0, 11);
        Eq(game::ArmiesAt(g, 0, 0)[0], b, "a tile is read from the top of the table down");
    }

    // P1: the production nudges are dice(1,100) < p
    {
        Rng r(3);
        int hits = 0;
        for (int i = 0; i < 20000; i++)
            if (r.Chance(10)) hits++;
        Ok(hits > 20000 * 0.08 && hits < 20000 * 0.10,
           "chance(10) is 9 in 100: " + std::to_string(hits));
        Rng t(1);
        bool never = true;
        for (int i = 0; i < 5000; i++)
            if (t.Chance(1)) never = false;
        Ok(never, "chance(1) never comes up");
    }

    // P2, P3, P5: no spilling round a full city; a city that cannot build
    // goes idle; a human's city that builds starts the next
    {
        auto gp = NewGame("ERYTHEA", Seed(2));
        Game& g = *gp;
        Side* side = game::Begin(g);
        City* city = side->capital;
        game::SetProduction(g, *city, 0);
        city->countdown = 1;
        for (auto d : std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
            while (game::ArmiesAt(g, city->x + d.first, city->y + d.second).size() < 8) {
                mk(g, city->x + d.first, city->y + d.second, side->index, 11);
            }
        }
        size_t n = g.armies.size();
        side->production_due = true;
        g.turn = 2;
        game::HumanProduction(g, *side);
        Eq(g.armies.size(), n, "a full city builds nothing: no spilling round it");
        Eq(city->countdown, 0, "its countdown stays at 0");
        Eq(game::Producing(*city), false, "and it is idle");
        for (Army* a : game::ArmiesAt(g, city->x + 1, city->y + 1)) g.Remove(a);
        side->production_due = true;
        game::HumanProduction(g, *side);
        Eq(g.armies.size(), n - 8, "an idle city does not try again");
        game::SetProduction(g, *city, 0);
        city->countdown = 1;
        side->production_due = true;
        game::HumanProduction(g, *side);
        Eq(g.armies.size(), n - 7, "set again, it builds");
        Eq(static_cast<int>(game::ArmiesAt(g, city->x + 1, city->y + 1).size()), 1,
           "on the tile with room");
        Eq(city->countdown, city->slots[0].time, "and starts the next (6f8c:06bd)");
        Army* built = game::ArmiesAt(g, city->x + 1, city->y + 1)[0];
        Eq(built->moves, built->max_moves, "a new army has its full move (6f8c:0dd7)");
    }

    // P7: the city's tiles fill x first
    {
        auto gp = NewGame("ERYTHEA", Seed(2));
        Game& g = *gp;
        City& city = g.map->cities[0];
        for (Army* a : std::vector<Army*>(g.armies)) {
            if (a->x >= city.x && a->x <= city.x + 1 && a->y >= city.y && a->y <= city.y + 1)
                g.Remove(a);
        }
        for (int i = 0; i < 8; i++) mk(g, city.x, city.y, 0, 11);
        auto [x, y] = game::FreeTileIn(g, city, false);
        Eq(x, city.x + 1, "after (x,y) comes (x+1,y)");
        Eq(y, city.y, "on the same row");
    }

    // T1: Random Turns shuffles the order at each round's end
    {
        auto gp = NewGame("ERYTHEA", SeedWith(4, {{"randomTurns", 1}}));
        Game& g = *gp;
        for (Side* s : g.sides) s->computer = true;
        Side* side = game::Begin(g);
        const std::array<int, 8> plain{0, 1, 2, 3, 4, 5, 6, 7};
        bool shuffled = false;
        while (side && g.turn <= 6) {
            if (g.turn_order != plain) shuffled = true;
            Eq(side->index, g.turn_order[g.order_at], "the side to play is the order's");
            side = game::EndTurn(g);
        }
        Ok(shuffled, "with Random Turns the order is shuffled (8065:1732)");
        auto sorted = g.turn_order;
        std::sort(sorted.begin(), sorted.end());
        Ok(sorted == plain, "it is still every side once");
        auto g2p = NewGame("ERYTHEA", Seed(4));
        Game& g2 = *g2p;
        game::Begin(g2);
        for (int i = 0; i < 30; i++) game::EndTurn(g2);
        Ok(g2.turn_order == plain, "without it the order stays");
    }

    // C7: a neutral city that beats off an attack builds, Neutral Cities >= 2
    {
        auto gp = NewGame("ERYTHEA", SeedWith(5, {{"neutralCities", 2}}));
        Game& g = *gp;
        Side* side = game::Begin(g);
        City* city = nullptr;
        for (auto& c : g.map->cities)
            if (!city && c.owner_index == kNone && !c.slots.empty()) city = &c;
        for (Army* a : game::ArmiesAt(g, city->x, city->y)) a->strength = 9;
        Army* weak = mk(g, city->x - 1, city->y, side->index, 11, 1);
        Battle r = game::DecideAttack(g, {weak}, city->x, city->y);
        Ok(city->attacked & (1 << side->index), "an attacked city is marked (67cc:02b1)");
        if (!r.won) {
            game::ApplyAttack(g, r);
            size_t before = game::ArmiesAt(g, city->x, city->y).size();
            for (size_t i = 0; i < 12 * g.sides.size(); i++) EndTurn(g);
            Ok(game::ArmiesAt(g, city->x, city->y).size() > before,
               "the neutral city builds (6bd8:025a)");
            Ok(game::ArmiesAt(g, city->x, city->y).size() <= 4 || before >= 4,
               "while its tile holds fewer than 4");
        }
    }

    // C10: a medal now and then for a hero's stack that wins
    {
        int medals = 0;
        for (int sd = 1; sd <= 60 && medals == 0; sd++) {
            auto gp = NewGame("ERYTHEA", Seed(sd));
            Game& g = *gp;
            Side* side = game::Begin(g);
            City* city = nullptr;
            for (auto& c : g.map->cities)
                if (!city && c.owner_index == kNone) city = &c;
            for (Army* a : game::ArmiesAt(g, city->x, city->y)) a->strength = 1;
            while (game::ArmiesAt(g, city->x, city->y).size() < 2)
                mk(g, city->x, city->y, kNone, 11, 1);
            std::vector<Army*> stack = {
                mk(g, city->x - 1, city->y, side->index, armytype::kHero, 9),
                mk(g, city->x - 1, city->y, side->index, 11, 9),
                mk(g, city->x - 1, city->y, side->index, 11, 9)};
            Battle b = game::DecideAttack(g, stack, city->x, city->y);
            Battle& r = game::ApplyAttack(g, b);
            if (r.medal) {
                medals++;
                Eq(r.medal->medals, 1, "a first medal");
                Ok(r.medal->type != armytype::kHero, "for an ordinary army, not the hero");
            }
        }
        Ok(medals > 0, "a hero's stack wins medals (67cc:2274)");
    }

    // S1: nothing guards an allies ruin
    {
        bool tried = false;
        for (int sd = 1; sd <= 20 && !tried; sd++) {
            auto gp = NewGame("ERYTHEA", Seed(sd));
            Game& g = *gp;
            Side* side = game::Begin(g);
            Site* s = nullptr;
            for (auto& x : g.map->sites)
                if (!s && x.content == site::kAllies) s = &x;
            if (!s) continue;
            tried = true;
            Army* h = mk(g, s->x, s->y, side->index, armytype::kHero, 1);
            auto r = site::Search(g, {h}, s->x, s->y, false);
            Eq(r->kind, "allies", "a hero always comes out of an allies ruin with allies");
            Ok(g.Alive(h), "alive");
        }
        Ok(tried, "found an allies ruin");
    }

    // Q1, Q4: a computer's quest types; Archons for a reward
    {
        auto gp = NewGame("ERYTHEA", SeedWith(8, {{"quests", 1}}));
        Game& g = *gp;
        Side* side = game::Begin(g);
        Army* h = mk(g, side->capital->x, side->capital->y, side->index, armytype::kHero);
        std::set<int> seen;
        for (int i = 0; i < 40; i++) {
            side->quest.reset();
            seen.insert(quest::Assign(g, *side, h, true)->type);
        }
        bool only = true;
        for (int t : seen) {
            if (t != quest::kOccupy && t != quest::kRaze && t != quest::kSlaughter &&
                t != quest::kPillageGold)
                only = false;
        }
        Ok(only, "a computer only gets occupy, raze, slaughter or gold (quest_assign(1))");
        bool archons = false;
        Quest q;
        q.hero = h;
        for (int i = 0; i < 60 && !archons; i++) {
            side->gold = 500;
            auto r = quest::ChooseReward(g, *side, &q);
            if (r->kind == "allies") {
                archons = true;
                Eq(r->type->id, quest::kRewardAlly, "a quest's allies are Archons");
            }
        }
        Ok(archons, "a quest can give allies");
    }

    // H1: no hero once the surrender has been offered
    {
        auto gp = NewGame("ERYTHEA", Seed(9));
        Game& g = *gp;
        Side* side = game::Begin(g);
        g.surrender_offered = true;
        side->gold = 100000;
        bool any = false;
        for (int i = 0; i < 50; i++)
            if (hero::Offer(g, *side)) any = true;
        Ok(!any, "no hero offers itself after the surrender is offered");
    }

    // M1: a cached route is taken by a stack standing on it
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        Side* side = game::Begin(g);
        Army* a = game::SideArmies(g, *side)[0];
        City* far = nullptr;
        for (auto& c : g.map->cities)
            if (!far && movement::Distance(c.x, c.y, a->x, a->y) > 20) far = &c;
        auto p1 = movement::FindPath(g, {a}, a->x, a->y, far->x, far->y);
        Ok(p1 && p1->size() > 3, "a long route");
        bool cached = false;
        for (auto& e : g.path_cache->slots)
            if (e && e->dx == far->x && e->dy == far->y) cached = true;
        Ok(cached, "is kept (1555:16f0)");
        auto mid = (*p1)[2];
        auto p2 = movement::FindPath(g, {a}, mid.x, mid.y, far->x, far->y);
        Eq(p2->size(), p1->size() - 3, "from a tile on it the rest of it is taken");
        bool same = true;
        for (size_t i = 0; i < p2->size(); i++) {
            if ((*p2)[i].x != (*p1)[i + 3].x || (*p2)[i].y != (*p1)[i + 3].y) same = false;
        }
        Ok(same, "step for step");
    }

    // A1: the computer's flood goes no further than its rounds
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        City& c = g.map->cities[0];
        auto f = ai::core::FloodFrom(g, 0, c.x, c.y, 5, nullptr, kNone);
        bool beyond = false;
        for (int y = 0; y < g.map->height; y++) {
            for (int x = 0; x < g.map->width; x++) {
                if (std::max(std::abs(x - c.x), std::abs(y - c.y)) > 5 &&
                    ai::core::FloodAt(f, x, y) != ai::core::kUnreached) {
                    beyond = true;
                }
            }
        }
        Ok(!beyond, "a flood of 5 rounds reaches no further than 5 (1555:0373)");
    }

    // a walk that leaves fewer than 2 moves is out of moves (1a8b:09d1): the
    // interface then puts the stack down
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        Side* side = game::Begin(g);
        int fx = kNone, fy = kNone;
        auto free_at = [&](int tx, int ty) {
            return scn::TerrainAt(*g.map, tx, ty) == movement::kPlain && !game::CityAt(g, tx, ty) &&
                   game::ArmiesAt(g, tx, ty).empty() && scn::RoadAt(*g.map, tx, ty) % 32 == 0;
        };
        for (int y = 1; y < g.map->height - 1 && fx == kNone; y++)
            for (int x = 1; x < g.map->width - 1 && fx == kNone; x++)
                if (free_at(x, y) && free_at(x + 1, y)) {
                    fx = x;
                    fy = y;
                }
        Army* a = mk(g, fx, fy, side->index, 11);
        int cost = (*movement::FindPath(g, {a}, a->x, a->y, fx + 1, fy))[0].cost;
        a->moves = cost + 1;
        Eq(movement::MoveTo(g, {a}, fx + 1, fy).out_of_moves, true,
           "one move left after the walk: out of moves");
        a->moves = cost + 2;
        Eq(movement::MoveTo(g, {a}, fx, fy).out_of_moves, false,
           "two left after arriving: not out of moves");
    }

    // a walk to sea is shown as it set off, on land, to its end (1a8b:04c8)
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        Side* side = game::Begin(g);
        Army* a = mk(g, 71, 11, side->index, 4);
        a->moves = 40;
        auto r = movement::MoveTo(g, {a}, 70, 14);
        Ok(a->x == 70 && a->y == 14, "over the bridge to the shore");
        Eq(r.sea_before, false, "it set off on land, and is shown so");
        Eq(a->at_sea, true, "and is at sea once the walk is done");
        a->moves = 40;
        Eq(movement::MoveTo(g, {a}, 71, 11).sea_before, true, "coming ashore it is shown at sea");
    }

    // the item quest uncovers round its ruin two tiles out when the searching
    // stack flies (8611:1298, DS:1e9c), one otherwise
    for (bool flying : {false, true}) {
        auto gp = NewGame("ERYTHEA", SeedWith(2, {{"quests", 1}, {"hiddenMap", 1}}));
        Game& g = *gp;
        Side* side = game::Begin(g);
        Site* s = nullptr;
        for (auto& x : g.map->sites) {
            if (s || x.content != site::kItem) continue;
            for (auto& it : g.map->items)
                if (it.index == x.item && !site::ItemReserved(it)) s = &x;
        }
        Army* h = mk(g, s->x, s->y, side->index, armytype::kHero);
        side->quest.reset();
        auto q = quest::Assign(g, *side, h, false, flying);
        // a retrieve quest is not certain; take the first try that is one
        for (int i = 0; i < 200 && q && q->type != quest::kRetrieveItem; i++) {
            side->quest.reset();
            q = quest::Assign(g, *side, h, false, flying);
        }
        if (!q || q->type != quest::kRetrieveItem) {
            Ok(false, "an item quest was given");
            continue;
        }
        Site* at = nullptr;
        for (auto& x : g.map->sites)
            if (x.content == site::kItem && x.item == q->item->index) at = &x;
        int ring = 0, seen = 0;
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (std::max(std::abs(dx), std::abs(dy)) == 2) {
                    ring++;
                    if (game::Seen(g, side->index, at->x + dx, at->y + dy)) seen++;
                }
        if (flying)
            Eq(seen, ring, "a flier's quest uncovers two tiles round the ruin");
        else
            Ok(game::Seen(g, side->index, at->x + 1, at->y + 1), "a walker's uncovers one");
    }
}

// What plays when. The OPL synthesis is checked in the sound step.
static void TestSound() {
    printf("sound\n");
    auto files = uidata::LoadStrings(kData + "/DATA/FILE.DAT");
    auto first = [](int) { return 1; };
    auto [name, loop] = cues::Song(files, cues::kTitle, {}, first);
    Eq(name, "STARTUP.XMI", "the start screens play STARTUP.XMI");
    Ok(loop, "and it starts again when it ends");
    Side human;
    human.computer = false;
    human.alive = true;
    std::tie(name, loop) = cues::Song(files, cues::kComputer, {&human}, first);
    Eq(name, "INT2.XMI", "a computer's turn beside a human plays FILE.DAT group 11");
    Ok(!loop, "once");
    Eq(cues::Song(files, cues::kBegin, {}, first).first, "INT22.XMI", "the war begins to INT22");
    Game g;
    g.map = std::make_unique<Map>();
    g.turn = 1;
    Side side;
    side.index = 0;
    side.gold = 500;
    g.map->cities.resize(6);
    for (auto& c : g.map->cities) c.owner_index = 0;
    Eq(cues::AdvisorCue(g, side, first), kNone, "the advisor never speaks on turn 1");
    Eq(side.advisor.mark, 5, "but he has marked six cities down as five");
    g.turn = 2;
    Eq(cues::AdvisorCue(g, side, first), kNone, "nothing new, not a seventh turn: silence");
    for (int i = 0; i < 5; i++) g.map->cities[i].owner_index = 1;
    Eq(cues::AdvisorCue(g, side, first), 40, "down to one city: 'Thy sorry efforts...' (VLOSE05)");
    Eq(side.advisor.dir, 2, "and he remembers saying so");
    for (int i = 0; i < 5; i++) g.map->cities[i].owner_index = 0;
    Eq(cues::AdvisorCue(g, side, first), 47, "back to six: 'Thou art doing well... so far!'");
    g.turn = 7;
    side.gold = 50;
    Eq(cues::AdvisorCue(g, side, first), 54, "on a seventh turn, short of gold: VGOLD00");
}

static void TestModernAi() {
    printf("modern learnable AI & diplomacy\n");

    // 1. All 27 personality profiles exist and have bounded 10D traits
    for (int lv = 0; lv < 3; lv++) {
        for (int c = 0; c < 9; c++) {
            const auto& p = ai::personality::ProfileFor(lv, c);
            Ok(!p.name.empty() && !p.archetype.empty() && !p.motto.empty(),
               "card profile has name, archetype, motto");
            for (float t : p.traits) {
                Ok(t >= -1.0F && t <= 1.0F, "trait in [-1, 1]");
            }
        }
    }

    // 2. Fair-information fog-of-war pathblocking when classicAi == false
    {
        auto gp = NewGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Game& g = *gp;
        Side* s0 = g.map->Side(0);
        s0->computer = true;
        s0->classic_ai = false;
        int ux = kNone, uy = kNone;
        for (int y = 5; y < g.map->height - 5 && ux == kNone; y++) {
            for (int x = 5; x < g.map->width - 5 && ux == kNone; x++) {
                if (!game::Seen(g, 0, x, y) && scn::TerrainAt(*g.map, x, y) == movement::kPlain) {
                    ux = x;
                    uy = y;
                }
            }
        }
        Ok(ux != kNone, "found unseen plain tile under hiddenMap");
        auto grid = movement::Grid(g, 0);
        auto prep = movement::Prepare(g, grid, 0, movement::kLand, 0, 0);
        int idx = uy * g.map->width + ux;
        Eq(prep[idx], movement::kShut, "modern AI cannot pathfind through unseen fog tiles");
    }

    // 3. ALLIED shared fog-of-war vision, truce timers, tribute transfer, and joint war pact
    {
        auto gp = NewGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}}));
        Game& g = *gp;
        int tx = 50, ty = 50;
        Eq(game::Seen(g, 0, tx, ty), false, "side 0 has not seen (50,50)");
        game::Reveal(g, 1, tx, ty, false);
        Eq(game::Seen(g, 1, tx, ty), true, "side 1 sees (50,50)");
        Eq(game::Seen(g, 0, tx, ty), false, "unallied side 0 still cannot see side 1's tile");

        int gold0_before = g.map->sides[0].gold;
        int gold1_before = g.map->sides[1].gold;
        diplomacy::Propose(g, 0, 1, diplomacy::kPeace);
        diplomacy::ProposeAlliance(g, 0, 1, true);
        diplomacy::SetGoldOffer(g, 0, 1, 50);
        diplomacy::SetWarTarget(g, 0, 1, 2);

        diplomacy::Propose(g, 1, 0, diplomacy::kPeace);
        diplomacy::ProposeAlliance(g, 1, 0, true);
        diplomacy::Apply(g, g.map->sides[1]);

        Eq(diplomacy::IsAllied(g, 0, 1), true, "sides 0 and 1 are now ALLIED");
        Eq(diplomacy::TruceTurns(g, 0, 1), 15, "15-turn alliance truce active");
        Eq(g.map->sides[0].gold, gold0_before - 50, "tribute deducted from proposer");
        Eq(g.map->sides[1].gold, gold1_before + 50, "tribute credited to recipient");
        Eq(diplomacy::State(g, 1, 2), diplomacy::kWar, "joint war target activated");
        Eq(game::Seen(g, 0, tx, ty), true, "ALLIED side 0 now shares side 1's fog-of-war vision");

        auto bd = diplomacy::ComputeAttitude(g, 1, 0);
        Ok(!bd.factors.empty() && !bd.what_it_takes.empty(),
           "attitude ledger produces itemized factors and forecast");
    }

    // 4. SideBelief under viewEnemies == 0 and RNG preservation in estimateCombat
    {
        auto gp = NewGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Game& g = *gp;
        Side& s0 = g.map->sides[0];
        s0.computer = true;
        s0.classic_ai = false;
        ai::belief::Update(g, s0);
        const SideBelief& b = ai::belief::Ensure(s0);
        for (City* c : b.visible_cities) {
            Ok(ai::belief::IsCityVisible(g, 0, *c),
               "all visibleCities in SideBelief are genuinely seen by side 0");
        }
        uint32_t rng_before = g.rng.state;
        std::vector<Army*> my_armies = game::SideArmies(g, s0);
        if (!my_armies.empty() && !b.visible_cities.empty()) {
            std::vector<Army*> test_stack = {my_armies[0]};
            auto est = ai::belief::EstimateCombat(g, s0, test_stack, b.visible_cities[0]->x,
                                                  b.visible_cities[0]->y);
            Ok(est.win_prob >= 0.0F && est.win_prob <= 1.0F,
               "estimateCombat returns probability in [0, 1]");
        }
        Eq(g.rng.state, rng_before, "estimateCombat never mutates Game::rng");
    }

    // 5. PolicyNetwork 4 heads and JSON serialization roundtrip
    {
        ai::ml::PolicyNetwork net;
        auto gp = NewGame("TUTORIA", Seed(1));
        Game& g = *gp;
        Side& s0 = g.map->sides[0];
        s0.classic_ai = false;
        ai::belief::Update(g, s0);
        auto ctx = ai::ml::BuildContext(g, s0);
        float v = net.EvaluateValue(ctx, ai::ml::BuildCriticFeatures(g, s0));
        Ok(std::isfinite(v) && std::abs(v) < 5.0F,
           "linear value head gives a finite, small estimate at random init");

        auto flat1 = net.Flatten();
        Eq(static_cast<int>(flat1.size()), ai::ml::PolicyNetwork::kTotalParams,
           "flattened parameter count matches TOTAL_PARAMS");
        std::filesystem::path tmp_path =
                std::filesystem::temp_directory_path() / "w2_test_weights.json";
        Ok(net.SaveJson(tmp_path), "saved PolicyNetwork JSON");
        ai::ml::PolicyNetwork net2;
        Ok(net2.LoadJson(tmp_path), "loaded PolicyNetwork JSON");
        std::filesystem::remove(tmp_path);
        auto flat2 = net2.Flatten();
        float max_diff = 0.0F;
        for (size_t i = 0; i < flat1.size(); i++) {
            max_diff = std::max(max_diff, std::abs(flat1[i] - flat2[i]));
        }
        Ok(max_diff < 1e-4F, "PolicyNetwork JSON roundtrip preserves weights");
    }

    // 6. Multi-turn game execution with 100% Learnable AI on TUTORIA
    {
        GameOpts opts = SeedWith(3, {{"hiddenMap", 1}, {"viewEnemies", 0}});
        for (int i = 0; i < 8; i++) {
            opts.sides[i].computer = true;
            opts.sides[i].level = 2;
        }
        auto gp = NewGame("TUTORIA", opts);
        Game& g = *gp;
        for (auto& s : g.map->sides) {
            if (s.in_use) s.classic_ai = false;
        }
        Side* cur = game::Begin(g);
        int turns_played = 0;
        int worst_stack = 0;
        while (cur && !g.over && g.turn <= 8) {
            ai::PlayTurn(g, *cur);
            turns_played++;
            std::map<int, int> per_tile;
            for (Army* a : g.armies) {
                if (a->x != kNone && !a->transit)
                    worst_stack = std::max(worst_stack, ++per_tile[a->y * g.map->width + a->x]);
            }
            cur = game::EndTurn(g);
        }
        Ok(turns_played >= 4,
           "modern learnable AI plays multiple turns cleanly under hiddenMap + viewEnemies=0");
        Ok(worst_stack <= rules::kMaxStack, "modern AI never puts more than 8 armies on one tile");
    }

    // Tabula rasa initialization, honest partial-visibility belief, enemy distance, and island
    // frontiers
    {
        ai::ml::PolicyNetwork fresh_net;
        float max_skip = 0.0F;
        for (float w : fresh_net.mission_net.w_skip) max_skip = std::max(max_skip, std::abs(w));
        Ok(max_skip <= 0.25F, "PolicyNetwork starts tabula rasa with zero hand-coded skip priors");

        auto gp =
                NewGame("ERYTHEA",
                        SeedWith(7, {{"hiddenMap", 1}, {"viewEnemies", 0}, {"neutralCities", 2}}));
        Game& g = *gp;
        Side& s7 = g.map->sides[7];
        s7.computer = true;
        s7.classic_ai = false;

        // Reveal a neutral city's top-left (x, y) tile while leaving (x + 1, y + 1) unseen
        City* target_neutral = nullptr;
        for (auto& c : g.map->cities) {
            if (c.owner_index == kNone && !c.razed && !game::Seen(g, 7, c.x + 1, c.y + 1)) {
                target_neutral = &c;
                break;
            }
        }
        Ok(target_neutral != nullptr, "found unseen neutral city on ERYTHEA");
        if (target_neutral) {
            // Remove any garrison on (x, y) so (x, y) is visibly empty while (x+1, y+1) is in fog
            for (Army* a : game::ArmiesAt(g, target_neutral->x, target_neutral->y)) {
                a->x = kNone;
                a->y = kNone;
            }
            g.explored[7][target_neutral->y * g.map->width + target_neutral->x] = true;
            Eq(game::Seen(g, 7, target_neutral->x, target_neutral->y), true,
               "top-left neutral city tile is seen");
            Eq(game::Seen(g, 7, target_neutral->x + 1, target_neutral->y + 1), false,
               "bottom-right neutral city tile is unseen");
            ai::belief::Update(g, s7);
            std::vector<Army*> my_armies = game::SideArmies(g, s7);
            Ok(!my_armies.empty(), "side 7 has starting army");
            auto est = ai::belief::EstimateCombat(g, s7, {my_armies[0]}, target_neutral->x,
                                                  target_neutral->y);
            Eq(est.estimated_defender_count, 0,
               "partially-seen neutral city with visibly empty (x,y) does not fabricate 3 phantom "
               "defenders");
        }

        // With no rival cities or stacks visible to side 7, cityNearestEnemyDist should not treat
        // fog/neutrals as enemies
        const SideBelief& b7 = ai::belief::Ensure(s7);
        if (s7.capital && s7.capital->index < static_cast<int>(b7.city_nearest_enemy_dist.size())) {
            Ok(b7.city_nearest_enemy_dist[s7.capital->index] > 50.0F,
               "cityNearestEnemyDist ignores neutral cities and fog frontiers when no rivals are "
               "visible");
        }

        // Reveal only side 7's home island land tiles (x < 27, y < 32) while surrounding sea
        // remains unseen
        g.explored[7].assign(static_cast<size_t>(g.map->width) * g.map->height, false);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 27; x++) {
                int terr = scn::TerrainAt(*g.map, x, y);
                if (terr != movement::kWater && terr != movement::kShore) {
                    game::Reveal(g, 7, x, y, false);
                }
            }
        }
        ai::belief::Update(g, s7);
        bool has_water_frontier = false;
        for (const auto& fc : ai::belief::Ensure(s7).fog_frontiers) {
            if (fc.is_water) has_water_frontier = true;
        }
        Ok(has_water_frontier,
           "island faction generates coastal/water fog frontiers after exploring local island "
           "land");

        // Analytical backpropagation finite-difference gradient check on ResidualMLP
        ai::ml::MissionInVec test_in{};
        for (size_t i = 0; i < test_in.size(); i++) {
            test_in[i] = std::sin(static_cast<float>(i + 1) * 0.37F);
        }
        std::array<float, ai::ml::kMissionOutDim> d_out = {0.75F, -0.45F};
        ai::ml::PolicyNetwork::MissionNet grad_analytical{};
        grad_analytical.Zero();
        fresh_net.mission_net.Backward(test_in, d_out, grad_analytical);
        std::vector<float> flat_grad;
        grad_analytical.ToFlat(flat_grad);
        std::vector<float> base_params;
        fresh_net.mission_net.ToFlat(base_params);

        float max_grad_err = 0.0F;
        constexpr float kEps = 1e-3F;
        for (size_t idx = 0; idx < base_params.size(); idx += 17) {
            std::vector<float> plus_p = base_params;
            std::vector<float> minus_p = base_params;
            plus_p[idx] += kEps;
            minus_p[idx] -= kEps;
            ai::ml::PolicyNetwork::MissionNet net_plus{}, net_minus{};
            net_plus.FromFlat(plus_p, 0);
            net_minus.FromFlat(minus_p, 0);
            auto y_plus = net_plus.Forward(test_in);
            auto y_minus = net_minus.Forward(test_in);
            float loss_plus = d_out[0] * y_plus[0] + d_out[1] * y_plus[1];
            float loss_minus = d_out[0] * y_minus[0] + d_out[1] * y_minus[1];
            float num_grad = (loss_plus - loss_minus) / (2.0F * kEps);
            max_grad_err = std::max(max_grad_err, std::abs(num_grad - flat_grad[idx]));
        }
        Ok(max_grad_err < 1e-3F,
           "ResidualMLP analytical backward matches finite-difference gradients");

        // Hidden-map potential shaping strictly rewards revealing unseen castles, while capture
        // dominates discovery
        City* unseen_neutral = nullptr;
        for (auto& c : g.map->cities) {
            if (c.owner_index == kNone && !ai::belief::IsCityVisible(g, 7, c)) {
                unseen_neutral = &c;
                break;
            }
        }
        Ok(unseen_neutral != nullptr, "found an unseen neutral castle on hidden-map ERYTHEA");
        if (unseen_neutral) {
            float phi_hidden = ai::ml::ComputeEmpireScore(g, 7);
            game::Reveal(g, 7, unseen_neutral->x, unseen_neutral->y, false);
            float phi_revealed = ai::ml::ComputeEmpireScore(g, 7);
            int prev_owner = unseen_neutral->owner_index;
            unseen_neutral->owner_index = 7;
            float phi_captured = ai::ml::ComputeEmpireScore(g, 7);
            unseen_neutral->owner_index = prev_owner;

            float discovery_gain = phi_revealed - phi_hidden;
            float capture_gain = phi_captured - phi_revealed;
            Ok(discovery_gain > 0.0F,
               "Hidden-map potential increases monotonically when fog exploration uncovers a "
               "castle");
            Ok(capture_gain > 5.0F * discovery_gain,
               "Castle capture potential gain strictly dominates mere reconnaissance discovery");
        }

        // Recorded MissionStepRecord carries 16D StackVec with origin-city guard/threat
        // observability
        s7.gold = 200;
        for (Army* a : game::SideArmies(g, s7)) {
            a->moves = a->max_moves;
            a->ai_moved = false;
        }
        ai::ml::RolloutSampler rec_sampler;
        ai::ml::TurnStepRecord rec_turn;
        rec_sampler.current_turn = &rec_turn;
        ai::modern::PlayTurn(g, s7, nullptr, &fresh_net, &rec_sampler);
        Ok(!rec_turn.mission_steps.empty() && !rec_turn.mission_steps[0].candidates.empty(),
           "RolloutSampler records joint (target, coalition) MissionStepRecord candidates");

        // Multiplicity-balanced sampling gives equal family probability to 1 Hold vs 40 Fog options
        // at zero utility
        std::vector<float> zero_logits(41, 0.0F);
        std::vector<float> test_log_mult(41, std::log(40.0F));
        test_log_mult[0] = 0.0F;
        ai::ml::RolloutSampler mult_sampler;
        mult_sampler.rng_state = 12345U;
        mult_sampler.temperature = 1.0F;
        int hold_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::SelectFromLogits(zero_logits, &mult_sampler, nullptr, &test_log_mult) ==
                0) {
                hold_picks++;
            }
        }
        Ok(hold_picks >= 900 && hold_picks <= 1100,
           "multiplicity-balanced selectFromLogits assigns 50/50 family mass to 1 Hold vs 40 Fog "
           "options at zero utility");

        // Greedy play picks the mode of the training distribution: with every option
        // scored equally, the single Hold beats each of 40 Fog options (whose
        // individual probability is 1/80), at any temperature.
        Eq(ai::ml::SelectFromLogits(zero_logits, nullptr, nullptr, &test_log_mult), 0,
           "greedy selectFromLogits applies the multiplicity prior (argmax of u - logMult)");
        std::vector<float> fog_favoured = zero_logits;
        fog_favoured[7] =
                std::log(40.0F) + 0.1F;  // one fog option slightly above Hold in policy logits
        Eq(ai::ml::SelectFromLogits(fog_favoured, nullptr, nullptr, &test_log_mult), 7,
           "greedy selectFromLogits picks a fog option once its u - logMult exceeds Hold's");
        mult_sampler.temperature = 0.35F;
        int fog_seven_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::SelectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 7)
                fog_seven_picks++;
        }
        hold_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::SelectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 0)
                hold_picks++;
        }
        Ok(fog_seven_picks > hold_picks,
           "the sampled mode at low temperature agrees with the greedy choice");

        // Binary decisions: greedy returns the mode, sampling follows sigmoid(logit / T)
        Eq(ai::ml::SelectBinary(0.3F, nullptr), true, "greedy selectBinary is logit > 0");
        Eq(ai::ml::SelectBinary(-0.3F, nullptr), false,
           "greedy selectBinary is false for logit < 0");
        mult_sampler.temperature = 1.0F;
        int yes = 0;
        for (int trial = 0; trial < 2000; trial++)
            yes += ai::ml::SelectBinary(std::log(3.0F), &mult_sampler) ? 1 : 0;
        Ok(yes >= 1400 && yes <= 1600,
           "sampled selectBinary with logit ln 3 says yes about 75% of the time");

        // Spectator visibility: AI-vs-AI games reveal the whole map to the viewer while keeping
        // per-AI fog intact
        {
            GameOpts human_opts = SeedWith(11, {{"hiddenMap", 1}});
            for (int i = 0; i < 8; i++) human_opts.sides[i].computer = (i > 0);
            auto gp_human = NewGame("ERYTHEA", human_opts);
            Game& gh = *gp_human;
            Eq(game::HasActiveHuman(gh), true, "game with human player reports active human");
            Eq(game::ViewerSide(gh, gh.sides[0]), gh.sides[0]->index,
               "human viewerSide uses human side index");
            Eq(game::Seen(gh, game::ViewerSide(gh, gh.sides[0]), 0, 0), false,
               "human viewer still has fog-of-war on unseen corner");

            GameOpts ai_opts = SeedWith(11, {{"hiddenMap", 1}});
            for (int i = 0; i < 8; i++) ai_opts.sides[i].computer = true;
            auto gp_ai = NewGame("ERYTHEA", ai_opts);
            Game& ga = *gp_ai;
            Eq(game::HasActiveHuman(ga), false, "all-AI game reports no active human");
            Eq(game::ViewerSide(ga, ga.sides[0]), kNone,
               "all-AI viewerSide returns NONE (spectator full map)");
            Eq(game::Seen(ga, game::ViewerSide(ga, ga.sides[0]), 0, 0), true,
               "spectator viewer sees unseen corner (0,0)");
            Eq(game::FogCell(ga, game::ViewerSide(ga, ga.sides[0]), 0, 0), kNone,
               "spectator viewer draws no fog shroud");
            Eq(game::Seen(ga, ga.sides[0]->index, 0, 0), false,
               "individual AI side still obeys its own Hidden Map fog");
        }

        // Classic AI vs Modern AI default toggle, mid-game switch, and save/load preservation
        {
            bool prev_default = ai::DefaultClassicAi();
            ai::SetDefaultClassicAi(true);
            auto gp_classic = NewGame("ERYTHEA", Seed(13));
            Eq(gp_classic->sides[0]->classic_ai, true,
               "defaultClassicAi(true) initializes sides with Classic 1993 AI");
            std::string classic_json = save::Encode(*gp_classic);

            ai::SetDefaultClassicAi(false);
            auto gp_modern = NewGame("ERYTHEA", Seed(13));
            Eq(gp_modern->sides[0]->classic_ai, false,
               "defaultClassicAi(false) initializes sides with Modern RL AI");
            std::string modern_json = save::Encode(*gp_modern);

            // Decoding a Classic AI save while the app default is Modern AI must keep Classic AI
            auto decoded_classic = save::Decode(classic_json, kData);
            Eq(decoded_classic->sides[0]->classic_ai, true,
               "Classic AI save stays Classic AI even when app default is Modern AI");

            // Decoding a Modern AI save while the app default is Classic AI must keep Modern AI
            ai::SetDefaultClassicAi(true);
            auto decoded_modern = save::Decode(modern_json, kData);
            Eq(decoded_modern->sides[0]->classic_ai, false,
               "Modern AI save stays Modern AI even when app default is Classic AI");

            // Mid-game live switch between Classic AI and Modern AI: alternate engines
            // turn by turn for several rounds and check the game stays legal
            for (Side* s : decoded_modern->sides) s->computer = true;
            Side* cur = game::Begin(*decoded_modern);
            Ok(cur != nullptr, "mid-game switch test game begins");
            int switched_turns = 0, worst_tile = 0;
            bool positions_valid = true;
            while (cur && !decoded_modern->over && decoded_modern->turn <= 6) {
                bool classic = (decoded_modern->turn % 2 == 0);
                for (auto& s : decoded_modern->map->sides) s.classic_ai = classic;
                ai::PlayTurn(*decoded_modern, *cur);
                switched_turns++;
                std::map<int, int> per_tile;
                for (Army* a : decoded_modern->armies) {
                    if (a->x == kNone || a->transit) continue;
                    if (a->x < 0 || a->y < 0 || a->x >= decoded_modern->map->width ||
                        a->y >= decoded_modern->map->height) {
                        positions_valid = false;
                    } else {
                        worst_tile = std::max(worst_tile,
                                              ++per_tile[a->y * decoded_modern->map->width + a->x]);
                    }
                }
                cur = game::EndTurn(*decoded_modern);
            }
            Ok(switched_turns >= 20,
               "Classic and Modern AI alternate over several rounds without stalling");
            Ok(positions_valid, "every army stays on the map while engines alternate");
            Ok(worst_tile <= rules::kMaxStack, "no tile exceeds 8 armies while engines alternate");

            ai::SetDefaultClassicAi(prev_default);
        }

        // Small-map scaling (activeSides + cropMargin) and thread-local in-memory random maps
        {
            Rng map_rng(4242.0);
            randommap::Options ro;
            ro.data_dir = kData;
            ro.rng = &map_rng;
            ro.sliders = {3, 3, 2, 3};
            auto rfiles = randommap::GenerateNow(ro);

            game::NewGameOptions so;
            so.seed = 4242.0;
            so.memory_files = &rfiles;
            so.active_sides = 2;
            so.crop_margin = 14;
            so.options = {{"hiddenMap", 1}, {"diplomacy", 0}, {"neutralCities", 1}, {"quests", 1}};
            for (int i = 0; i < 8; i++) {
                so.sides[i].computer = true;
                so.sides[i].level = 2;
            }
            auto gp_small = game::NewGame(kData, "RANDOM", so);
            Game& gs = *gp_small;
            Eq(static_cast<int>(gs.sides.size()), 2,
               "cropped 1v1 random map keeps exactly 2 active sides");
            Ok(gs.map->cities.size() >= 2 && gs.map->cities.size() < 45,
               "cropped 1v1 random map trims city count");
            bool cities_in_box = true;
            for (const auto& c : gs.map->cities) {
                if (c.x < gs.map->active_x0 || c.x + 1 > gs.map->active_x1 ||
                    c.y < gs.map->active_y0 || c.y + 1 > gs.map->active_y1) {
                    cities_in_box = false;
                }
            }
            Ok(cities_in_box, "all cropped cities sit inside the active island box");
            Eq(scn::TerrainAt(*gs.map, 0, 0), movement::kWater,
               "outer cropped border tile (0,0) is converted to water");
            Eq(game::Seen(gs, gs.sides[0]->index, 0, 0), true,
               "outer cropped water is pre-marked seen under Hidden Map");

            gs.sides[0]->classic_ai = false;
            gs.sides[1]->classic_ai = true;
            Side* cur = game::Begin(gs);
            while (cur && !gs.over && !gs.won && gs.turn <= 10) {
                if (cur->alive) ai::PlayTurn(gs, *cur);
                cur = game::EndTurn(gs);
            }
            Ok(gs.turn > 5 || gs.won || gs.over,
               "1v1 cropped random map runs Modern vs Classic cleanly");
        }

        // 5-head PolicyNetwork (v6: 30-wide context, privileged critics; 9700 params),
        // v5/v4/v3 compatibility, and temperature-multiplicity invariant
        {
            namespace ml = ai::ml;
            using PN = ml::PolicyNetwork;
            Eq(PN::kTotalParams, 9700, "v6 PolicyNetwork has 9700 parameters");
            Eq(PN::kLegacyV5Params, 8866, "legacy v5 parameter count remains 8866");
            Eq(PN::kLegacyV4Params, 8066, "legacy v4 parameter count remains 8066");
            Eq(PN::kLegacyV3Params, 6729, "legacy v3 parameter count remains 6729");
            ml::PolicyNetwork net;
            Ok(net.LoadJson("cpp/ai_weights.json"), "installed cpp/ai_weights.json loads cleanly");
            std::vector<float> flat = net.Flatten();
            Eq(static_cast<int>(flat.size()), 9700,
               "flattened parameter vector matches TOTAL_PARAMS");

            // Legacy flat vectors synthesised from a random v6 network by dropping
            // the input columns an older format did not have
            ml::PolicyNetwork src;
            src.InitRandomWeights(7);
            auto added_ctx = [](int c) { return c >= ml::kLegacyGlobalDim && c < ml::kGlobalDim; };
            auto narrow = [](const auto& mlp, auto dropped, std::vector<float>& out) {
                using M = std::decay_t<decltype(mlp)>;
                for (int r = 0; r < M::kOutDim; r++)
                    for (int c = 0; c < M::kInDim; c++)
                        if (!dropped(c)) out.push_back(mlp.w_skip[r * M::kInDim + c]);
                for (int r = 0; r < M::kH1Dim; r++)
                    for (int c = 0; c < M::kInDim; c++)
                        if (!dropped(c)) out.push_back(mlp.w1[r * M::kInDim + c]);
                out.insert(out.end(), mlp.b1.begin(), mlp.b1.end());
                out.insert(out.end(), mlp.w2.begin(), mlp.w2.end());
                out.insert(out.end(), mlp.b2.begin(), mlp.b2.end());
                out.insert(out.end(), mlp.w3.begin(), mlp.w3.end());
                out.insert(out.end(), mlp.b3.begin(), mlp.b3.end());
            };
            auto critic_dropped = [&](int in) {
                return [&, in](int c) { return added_ctx(c) || c >= in - ml::kCriticDim; };
            };
            std::vector<float> v5, v4;
            for (auto* out : {&v5, &v4}) {
                narrow(src.diplo_net, added_ctx, *out);
                narrow(src.mission_net, added_ctx, *out);
                narrow(src.econ_net, added_ctx, *out);
            }
            narrow(src.value_net, added_ctx, v5);
            narrow(src.stack_value_net, added_ctx, v5);
            narrow(src.value_net, critic_dropped(ml::kValueInDim), v4);
            size_t v3_size = v4.size();
            narrow(src.stack_value_net, critic_dropped(ml::kStackValueInDim), v4);
            Eq(static_cast<int>(v5.size()), 8866, "synthesised v5 vector has 8866 parameters");
            Eq(static_cast<int>(v4.size()), 8066, "synthesised v4 vector has 8066 parameters");
            Eq(static_cast<int>(v3_size), 6729, "synthesised v3 prefix has 6729 parameters");

            // Inputs with the columns a format lacked set to zero: the migrated
            // network must compute exactly what the source network computes
            ml::ContextVec cx{};
            for (int i = 0; i < ml::kContextDim; i++)
                cx[i] = added_ctx(i) ? 0.0F : 0.1F * static_cast<float>(i % 7) - 0.3F;
            ml::CriticVec some_crit{}, zero_crit{};
            for (int i = 0; i < ml::kCriticDim; i++)
                some_crit[i] = 0.05F * static_cast<float>(i + 1);
            ml::StackVec sv{};
            sv[3] = 0.4F;
            ml::TargetVec tv{};
            tv[2] = 1.0F;
            tv[10] = 0.6F;
            ml::EconVec ev{};
            ev[1] = 1.0F;
            ev[7] = 0.5F;
            ml::RivalVec rv{};
            rv[0] = 0.3F;
            auto close = [](float a, float b) { return std::abs(a - b) < 1e-5F; };

            ml::PolicyNetwork m5;
            m5.InitRandomWeights(99);
            Ok(!m5.Flatten().empty(), "scratch network");
            m5.UnflattenLegacy(v5);
            Ok(close(m5.ScoreMission(cx, sv, tv).utility, src.ScoreMission(cx, sv, tv).utility),
               "v5 MissionNet migrates function-preserving");
            Ok(close(m5.ScoreEconomy(cx, ev), src.ScoreEconomy(cx, ev)),
               "v5 EconNet migrates function-preserving");
            Ok(close(m5.ScoreDiplomacy(cx, rv).stance_logits[3],
                     src.ScoreDiplomacy(cx, rv).stance_logits[3]),
               "v5 DiploNet migrates function-preserving");
            Ok(close(m5.EvaluateValue(cx, some_crit), src.EvaluateValue(cx, some_crit)),
               "v5 ValueNet (with privileged inputs) migrates function-preserving");
            Ok(close(m5.EvaluateStackValue(ml::PackStackValueInput(cx, sv, some_crit)),
                     src.EvaluateStackValue(ml::PackStackValueInput(cx, sv, some_crit))),
               "v5 StackValueNet migrates function-preserving");
            ml::ContextVec cx_new = cx;
            cx_new[ml::kLegacyGlobalDim] = 0.7F;
            Ok(close(m5.ScoreMission(cx_new, sv, tv).utility, m5.ScoreMission(cx, sv, tv).utility),
               "migrated network ignores the added context features until trained");

            ml::PolicyNetwork m4;
            m4.UnflattenLegacy(v4);
            Ok(close(m4.ScoreMission(cx, sv, tv).utility, src.ScoreMission(cx, sv, tv).utility),
               "v4 MissionNet migrates function-preserving");
            Ok(close(m4.EvaluateValue(cx, some_crit), src.EvaluateValue(cx, zero_crit)),
               "v4 ValueNet ignores the privileged inputs until trained");
            Ok(close(m4.EvaluateStackValue(ml::PackStackValueInput(cx, sv, some_crit)),
                     src.EvaluateStackValue(ml::PackStackValueInput(cx, sv, zero_crit))),
               "v4 StackValueNet ignores the privileged inputs until trained");

            // v3: no StackValueNet in the file -> zero-initialised
            std::vector<float> v3(v4.begin(), v4.begin() + static_cast<long>(v3_size));
            ml::PolicyNetwork m3;
            m3.InitRandomWeights(99);
            m3.UnflattenLegacy(v3);
            ml::StackValueInVec zero_sv{};
            Eq(m3.EvaluateStackValue(zero_sv), 0.0F,
               "legacy v3 weights zero-initialize StackValueNet");

            // At T = 0.5 with equal utilities (0.0), a singleton family and a 20-item family
            // with logMult = ln(20) receive equal 50/50 probability mass (z_i = u_i/T - logMult_i)
            std::vector<float> u(21, 0.0F);
            std::vector<float> lm(21, std::log(20.0F));
            lm[0] = 0.0F;
            ai::ml::RolloutSampler samp;
            samp.rng_state = 1234567U;
            samp.temperature = 0.5F;
            int picks0 = 0;
            for (int i = 0; i < 2000; i++) {
                if (ai::ml::SelectFromLogits(u, &samp, nullptr, &lm) == 0) picks0++;
            }
            Ok(picks0 > 880 && picks0 < 1120,
               "multiplicity prior stays balanced across families at T = 0.5");
        }
    }
}

// --------------------------------------------------------------------- main

int main() {
    if (!FileExists(kData + "/TERRAIN0/ARMYTYPE.DAT")) {
        printf("cannot find the game data in %s: run from the repository root\n", kData.c_str());
        return 1;
    }
    TestDice();
    TestArmyTypes();
    TestProduction();
    TestBugFlags();
    for (auto& s : kScenarios)
        if (FileExists(kData + "/" + s + "/" + s + ".SCN")) TestGame(s);
    TestTurnLoop("ERYTHEA");
    TestVectoring("ERYTHEA");
    TestBuyProduction("ERYTHEA");
    TestReports("ERYTHEA");
    TestHeroItems("ERYTHEA");
    TestSage("ERYTHEA");
    TestSetup();
    TestHistory("ERYTHEA");
    TestDisband("ERYTHEA");
    TestMovement("ERYTHEA");
    TestMovement("ISLADIA");
    TestLastMove();
    TestShips();
    TestStackLimit("ERYTHEA");
    TestSea();
    TestEncampment();
    TestCombat("ERYTHEA");
    TestCapture("ERYTHEA");
    TestCityChoices("ERYTHEA");
    TestHeroes("ERYTHEA");
    TestHeroExperienceBug();
    TestDiplomacy();
    TestQuests();
    TestEndGame();
    TestHiddenMap();
    TestSave();
    TestSites("ERYTHEA");
    TestSites("DRAGON");
    TestSlots();
    TestScreenLayout();
    TestCityCastles();
    TestComputerPlayers();
    TestRandomMap();
    TestMapEval();
    TestAiGame("TUTORIA", 30);
    TestAiGame("ERYTHEA", 25);
    if (FileExists(kData + "/TUTORIA/TUTORIA.SCN")) TestTutorialHero();
    TestVerified();
    TestSound();
    TestModernAi();
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
