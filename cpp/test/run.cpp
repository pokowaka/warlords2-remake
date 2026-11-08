// Headless tests for the rules core: the Lua remake's love2d/test/run.lua,
// ported (by way of web/test/run.js). No SDL, no graphics.
//
//     cpp/build/w2test            -- from the repository root
//
// Every assertion cites the rule it checks; docs/rules.md is the spec. Where
// the Lua counts from 1 these count from 0, so a city's first production
// slot is slot 0 here.
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

static const std::string kData = "original";
static const std::vector<std::string> kScenarios = {"ERYTHEA", "ILLURIA", "DRAGON", "HADESHA",
                                                    "ISLADIA", "SORCERY", "TUTORIA"};

static int passed = 0, failed = 0;

static void Ok(bool cond, const std::string& what) {
    if (cond)
        passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

static std::string Show(bool v) {
    return v ? "true" : "false";
}
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
static std::string Show(T v) {
    return std::is_signed_v<T> && static_cast<long long>(v) == NONE ? "nil" : std::to_string(v);
}
template <class T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
static std::string Show(T v) {
    return std::to_string(static_cast<long long>(v));
}
static std::string Show(const std::string& v) {
    return v;
}
static std::string Show(const char* v) {
    return v ? v : "nil";
}
template <class T>
static std::string Show(T* p) {
    return p ? fmt("%p", static_cast<const void*>(p)) : "nil";
}

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
static void Eq(const std::string& got, const char* want, const std::string& what) {
    Eq(got, std::string(want), what);
}

struct GameOpts {
    double seed = 0;
    std::vector<std::pair<std::string, int>> options;
    std::map<int, game::SideSetup> sides;
    bool greatest = false;
};
static std::unique_ptr<Game> NewGame(const std::string& scenario, GameOpts o = GameOpts()) {
    game::NewGameOptions n;
    n.seed = o.seed;
    n.options = o.options;
    n.sides = o.sides;
    n.greatest = o.greatest;
    return game::newGame(kData, scenario, n);
}
static GameOpts Seed(double s) {
    GameOpts o;
    o.seed = s;
    return o;
}
// The turn passes as the interface passes it: a human's turn opens with the
// hero offer left standing, and then its cities build (8cc6:04bd(1)).
static Side* EndTurn(Game& g) {
    Side* side = game::endTurn(g);
    if (side && !side->computer) game::humanProduction(g, *side);
    return side;
}
// A human side played by the computer player, as some tests do: it takes
// the hero it is offered first, as a computer does.
static void PlayAi(Game& g, Side& side) {
    if (!side.computer && side.heroOffer) {
        hero::recruit(g, side, *side.heroOffer);
        side.heroOffer.reset();
    }
    ai::playTurn(g, side);
}
static GameOpts SeedWith(double s, std::vector<std::pair<std::string, int>> options) {
    GameOpts o;
    o.seed = s;
    o.options = options;
    return o;
}
static game::SideSetup SideSetup(bool computer, int level = NONE, bool off = false) {
    game::SideSetup s;
    s.computer = computer;
    s.level = level;
    s.off = off;
    return s;
}

static Army* AddArmy(Game& g, int x, int y, int owner, int type, int strength, int moves = 0,
                     int max_moves = 0) {
    Army a;
    a.x = x;
    a.y = y;
    a.owner = owner;
    a.type = type;
    a.strength = strength;
    a.moves = moves;
    a.maxMoves = max_moves;
    a.name = g.types.byId(type) ? g.types.byId(type)->name : "";
    return g.add(a);
}

// ------------------------------------------------------------------ dice

static void TestDice() {
    printf("dice\n");
    Rng r(1234);
    int lo = 1 << 30, hi = -(1 << 30);
    for (int i = 0; i < 20000; i++) {
        int v = r.dice(1, 100, 0);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    Eq(lo, 1, "1d100 lower bound");
    Eq(hi, 100, "1d100 upper bound");
    lo = 1 << 30;
    hi = -(1 << 30);
    for (int i = 0; i < 20000; i++) {
        int v = r.dice(3, 500, 500);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    Ok(lo >= 503, "3d500+500 lower bound");
    Ok(hi <= 2000, "3d500+500 upper bound");
    std::set<int> seen;
    for (int i = 0; i < 2000; i++) seen.insert(r.dice(1, 5, -1));
    for (int i = 0; i <= 4; i++) Ok(seen.count(i), "dice(1,5,-1) reaches " + std::to_string(i));
    Ok(!seen.count(5), "dice(1,5,-1) stays below 5");
    Rng a(7), b(7);
    bool same = true;
    for (int i = 0; i < 100; i++)
        if (a.dice(2, 6, 0) != b.dice(2, 6, 0)) same = false;
    Ok(same, "a seed reproduces the sequence");
}

static void TestArmyTypes() {
    printf("ARMYTYPE.DAT\n");
    Types t;
    armytype::load(kData + "/TERRAIN0/ARMYTYPE.DAT", t);
    Eq(static_cast<int>(t.all.size()), 29, "29 army types");
    Ok(t.byId(armytype::HERO) != nullptr, "type 28 exists");
    Eq(t.byId(armytype::HERO)->name, "Hero", "type 28 is the Hero");
    Eq(t.byId(armytype::SCOUTS)->name, "Scouts", "type 11 is Scouts");
    const ArmyType* hcav = nullptr;
    for (auto& a : t.all)
        if (a.name == "Heavy Cav.") hcav = &a;
    Ok(hcav != nullptr, "Heavy Cav. is in the table");
    if (hcav) {
        Eq(hcav->strength, 4, "Heavy Cav. strength");
        Eq(hcav->time, 3, "Heavy Cav. production time");
        Eq(hcav->cost, 8, "Heavy Cav. cost");
        Eq(hcav->move, 16, "Heavy Cav. base move");
    }
    bool any_fly = false;
    for (auto& a : t.all)
        if (a.flies) any_fly = true;
    Ok(any_fly, "some types fly");
    const ArmyType* eleph = nullptr;
    for (auto& a : t.all)
        if (a.name == "Elephants") eleph = &a;
    Ok(eleph != nullptr, "Elephants are in the table");
    if (eleph)
        Eq(eleph->bonus[50], -1, "Elephants take 1 off the enemy stack: the field is signed");
}

static void TestProduction() {
    printf("production slots and garrisons\n");
    Types t;
    armytype::load(kData + "/TERRAIN0/ARMYTYPE.DAT", t);
    Rng r(42);
    auto sl = rules::citySlots({4, 11, 2}, t, r);
    Eq(static_cast<int>(sl.size()), 3, "one slot per production type");
    bool sorted = true;
    for (size_t i = 1; i < sl.size(); i++)
        if (sl[i].price < sl[i - 1].price) sorted = false;
    Ok(sorted, "slots are sorted by price");
    bool ok_bounds = true;
    for (int n = 1; n <= 2000; n++) {
        Rng rn(n);
        for (auto& s : rules::citySlots({1, 2, 3, 4}, t, rn)) {
            const ArmyType* base = t.byId(s.type);
            if (s.strength < 1 || s.strength > 9) ok_bounds = false;
            if (s.strength < base->strength - 1 || s.strength > base->strength + 1)
                ok_bounds = false;
            if (s.move < 6) ok_bounds = false;
            if (s.move > base->move + 4) ok_bounds = false;
            if (s.time < 1 || s.time > base->time + 1) ok_bounds = false;
        }
    }
    Ok(ok_bounds, "nudged stats stay within the documented range");
    bool any_flier = false;
    for (auto& s : sl)
        if (t.byId(s.type)->flies) any_flier = true;
    int best4 = rules::bestSlot(sl, 4, t, false);
    if (any_flier)
        Ok(best4 != NONE, "purpose 4 finds a flier");
    else
        Ok(best4 == NONE, "purpose 4 refuses a city with no fliers");
    int fast = rules::bestSlot(sl, 6, t, false);
    int strong = rules::bestSlot(sl, 3, t, false);
    Ok(fast != NONE && strong != NONE, "purposes 3 and 6 always choose something");
    Eq(rules::cityDefence(2), 1, "fewer than 3 types: defence 1");
    Eq(rules::cityDefence(3), 2, "3 types: defence 2");
    Eq(rules::cityDefence(4), 2, "4 types: defence 2");
    Slot slot{4, "x", 8, 2, 8, 12, 0};
    Eq(rules::armyFromSlot(slot, true).strength, 9, "Enhanced caps strength at 9");
    Eq(rules::armyFromSlot(slot, false).strength, 8, "no Enhanced, no bonus");
    Eq(rules::armyFromSlot(slot, false).upkeep, 4, "upkeep is half the slot cost");
}

static void TestGame(const std::string& scenario) {
    printf("game: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(3));
    Game& g = *gp;
    Ok(g.sides.size() >= 2, "at least two sides in play");
    Eq(static_cast<int>(g.armies.size()), static_cast<int>(g.map->cities.size()),
       "exactly one starting army per city");
    for (Side* s : g.sides) {
        Eq(static_cast<int>(game::sideCities(g, *s).size()), 1, s->name + " owns only its capital");
        Eq(game::sideCities(g, *s)[0]->index, s->capital->index,
           s->name + "'s city is its capital");
    }
    for (auto& c : g.map->cities) {
        for (auto& slot : c.slots)
            Ok(slot.type != armytype::NAVY, c.name + " does not produce Navy");
        Eq(c.defence, rules::cityDefence(static_cast<int>(c.slots.size())), c.name + " defence");
        Ok(c.slots.size() <= 4, c.name + " has at most 4 slots");
    }
    for (Army* a : g.armies) {
        Ok(g.types.byId(a->type) != nullptr, "army has a real type");
        Eq(a->moves, 0, "a starting garrison has no moves");
    }
    std::map<int, int> per_tile;
    for (Army* a : g.armies) {
        int k = a->y * g.map->width + a->x;
        per_tile[k]++;
        Ok(per_tile[k] <= rules::MAX_STACK, "stack limit holds");
    }
    auto hp = NewGame(scenario, Seed(3));
    bool same = hp->armies.size() == g.armies.size();
    for (size_t i = 0; i < g.armies.size() && same; i++) {
        Army* a = g.armies[i];
        Army* b = hp->armies[i];
        if (b->type != a->type || b->strength != a->strength) same = false;
    }
    Ok(same, "the same seed rebuilds the same game");
}

static void TestTurnLoop(const std::string& scenario) {
    printf("turn loop: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(5));
    Game& g = *gp;
    Side* side = game::begin(g);
    Ok(side != nullptr, "the first side is to play");
    for (Army* a : game::sideArmies(g, *side))
        Eq(a->moves, a->maxMoves, "a garrison starts the turn with its full move");
    int gold0 = side->gold;
    Eq(side->income, side->capital->income, "income is the capital's income");
    Ok(side->gold == std::max(0, gold0), "gold was applied");
    {
        Army* one = game::sideArmies(g, *side)[0];
        one->done = true;
        one->offered = true;
        one->fortified = true;
        for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
        Eq(one->done, false, "done for this turn wears off with the turn");
        Eq(one->offered, false, "and so does having been offered this pass");
        Eq(one->fortified, true, "but digging in outlasts it");
        one->fortified = false;
    }
    Army* a = game::sideArmies(g, *side)[0];
    a->moves = 5;
    int before = a->maxMoves;
    for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
    Eq(a->moves, before + rules::MOVE_CARRY, "at most 2 unused moves carry over");
    a->moves = 1;
    for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
    Eq(a->moves, before + 1, "carry-over below the cap is exact");

    City* city = side->capital;
    game::setProduction(g, *city, 0);
    int want = city->slots[0].time;
    size_t n0 = game::sideArmies(g, *side).size();
    for (size_t i = 0; i < want * g.sides.size(); i++) EndTurn(g);
    Ok(game::sideArmies(g, *side).size() == n0 + 1,
       fmt("%s produced after %d turns", city->name.c_str(), want));
    Army* built = nullptr;
    for (Army* army : game::sideArmies(g, *side))
        if (army->homeCity == city->index && army != a) built = army;
    Ok(built != nullptr, "the new army belongs to the city that built it");
    if (built) {
        Eq(built->upkeep, city->slots[0].cost / 2, "upkeep is half the slot cost");
        Eq(built->x, city->x, "the new army stands in its city");
    }
    // a side with no cities stays in play until the round ends (8065:18ab),
    // then goes, with its armies, its gold and its diplomacy
    Side* victim = g.sides.back();
    for (City* c : game::sideCities(g, *victim)) c->ownerIndex = NONE;
    victim->gold = 500;
    g.diplomacy.state[static_cast<size_t>(victim->index) * 8] = diplomacy::WAR;
    Ok(!game::sideArmies(g, *victim).empty(), "the beaten side still has armies");
    int round_of = g.turn;
    while (g.turn == round_of) {
        Ok(victim->alive, "a side with no cities is in play until the round ends");
        EndTurn(g);
    }
    Ok(!victim->alive, "a side with no cities is eliminated as the round ends");
    Eq(static_cast<int>(game::sideArmies(g, *victim).size()), 0,
       "the eliminated side's armies are gone");
    Eq(victim->gold, 0, "the eliminated side's gold is gone");
    Eq(diplomacy::state(g, victim->index, 0), diplomacy::INTERMEDIATE,
       "the eliminated side's diplomacy is reset to uneasy");
    Ok(!g.ending.fallen.empty() && g.ending.fallen[0].side == victim,
       "the elimination is announced");
    auto g2 = NewGame(scenario, Seed(6));
    game::begin(*g2);
    Eq(g2->turn, 1, "the game starts on turn 1");
    for (size_t i = 0; i < g2->sides.size(); i++) EndTurn(*g2);
    Eq(g2->turn, 2, "a full round advances the turn");
}

static void TestVectoring(const std::string& scenario) {
    printf("vectoring: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(9));
    Game& g = *gp;
    Side* side = game::begin(g);
    City* from = side->capital;
    City* dest = nullptr;
    for (auto& c : g.map->cities)
        if (!dest && c.ownerIndex == NONE && !c.slots.empty()) dest = &c;
    Ok(dest != nullptr, "found a neutral city to capture");
    if (!dest) return;
    dest->ownerIndex = side->index;
    for (Army* army : game::armiesAt(g, dest->x, dest->y)) army->owner = side->index;
    game::setProduction(g, *from, 0);
    game::vector(g, *from, dest);
    int time = from->slots[0].time;
    size_t at_dest = game::armiesAt(g, dest->x, dest->y).size();
    for (size_t i = 0; i < (time + 2) * g.sides.size(); i++) EndTurn(g);
    Ok(game::armiesAt(g, dest->x, dest->y).size() > at_dest,
       "a vectored army arrives two turns after it is built");
    {
        // to the turn: the road moves before the cities build (6f8c:0000), so
        // the one sent this turn shows under "Turn after" and lands two turns on
        auto round = [&]() {
            for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
        };
        Army* sent = nullptr;
        for (int i = 0; i < 10 && !sent; i++) {
            round();
            bool any = false;
            for (auto& e : side->produced)
                if (e.kind == "sent") any = true;
            if (!any) continue;
            for (Army* a : g.armies) {
                if (a->transit && a->owner == side->index && a->transit->dest == dest->index &&
                    a->transit->turns == 2)
                    sent = a;
            }
        }
        Ok(sent != nullptr, "a city sends an army that is two turns out");
        if (sent) {
            round();
            Ok(sent->transit && sent->transit->turns == 1,
               "a turn later it is still on the road, due next turn");
            round();
            Ok(!sent->transit && sent->x != NONE, "and the turn after that it has arrived");
        }
    }
    game::setProduction(g, *from, NONE);
    Ok(from->vectorTo == NONE, "vectoring only sticks while the city is building");
    std::vector<City*> senders;
    for (auto& c : g.map->cities) {
        if (&c != dest && senders.size() < 5) {
            c.ownerIndex = side->index;
            senders.push_back(&c);
        }
    }
    for (int i = 0; i < 4; i++)
        Ok(game::vector(g, *senders[i], dest), "vector " + std::to_string(i + 1) + " is taken");
    Ok(!game::vector(g, *senders[4], dest), "a fifth is refused");
    Eq(senders[4]->vectorTo, NONE, "and the fifth city keeps no vector");
    Eq(static_cast<int>(game::vectoredTo(g, dest).size()), 4,
       "four cities send to the destination");
    Ok(game::vector(g, *senders[0], dest), "a city already sending there may be set again");
    Ok(game::vector(g, *senders[0], senders[0]), "vectoring a city to itself lifts the vector");
    Eq(senders[0]->vectorTo, NONE, "and leaves it with none");
    Eq(game::nearestCity(g, dest->x, dest->y, side), dest, "a click on a city picks it");
    Eq(game::nearestCity(g, dest->x + 1, dest->y - 1, side), dest, "and a click next to it too");
}

static void TestBuyProduction(const std::string& scenario) {
    printf("buying production: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(17));
    Game& g = *gp;
    Side* side = game::begin(g);
    City* city = side->capital;
    side->gold = 5000;
    auto list = game::buyableTypes(g);
    Ok(!list.empty() && list.size() <= 20, "the Build Production screen has room for every type");
    for (auto* a : list) Ok(a->price >= 0, a->name + " is for sale");
    Eq(game::cannotBuy(g, *side, *city, *g.types.byId(city->slots[0].type)), "has it",
       "a type already built is greyed out");
    int n0 = static_cast<int>(city->slots.size());
    Eq(game::buySlot(*city), n0 < 4 ? n0 : 0, "the starting slot");
    const ArmyType* pick = nullptr;
    for (auto* a : list)
        if (game::cannotBuy(g, *side, *city, *a).empty() && (!pick || a->price > pick->price))
            pick = a;
    Ok(pick != nullptr, "something to buy");
    if (!pick) return;
    if (n0 == 4) {
        city->slots.pop_back();
        n0 = 3;
    }
    game::setProduction(g, *city, 0);
    Slot building = city->slots[0];
    int gold = side->gold;
    game::buyProduction(g, *side, *city, n0, pick->id);
    Eq(side->gold, gold - pick->price, "the price comes off the treasury");
    const Slot& slot = city->slots[n0];
    Eq(slot.type, pick->id, "the bought type fills the slot");
    Eq(slot.strength, pick->strength, "a bought type takes ARMYTYPE's strength unmodified");
    Eq(slot.time, pick->time, "and its time");
    Eq(slot.move, pick->move, "and its move");
    Eq(city->defence, rules::cityDefence(static_cast<int>(city->slots.size())),
       "defence follows the number of types");
    Eq(city->producing, 0, "buying into a new slot leaves production alone");
    game::sortProduction(*city);
    for (size_t i = 1; i < city->slots.size(); i++)
        Ok(city->slots[i - 1].price <= city->slots[i].price, "sorted by price, cheapest first");
    Ok(city->slots[city->producing] == building, "the city builds the same type after sorting");
    game::vector(g, *city, &g.map->cities[0] != city ? &g.map->cities[0] : &g.map->cities[1]);
    const ArmyType* other = nullptr;
    for (auto* a : list)
        if (!other && game::cannotBuy(g, *side, *city, *a).empty()) other = a;
    game::buyProduction(g, *side, *city, city->producing, other->id);
    Eq(city->producing, NONE, "production stops when its type is bought over");
    Eq(city->vectorTo, NONE, "and the vector goes with it");
    game::renameCity(g, *city, "Newtown");
    auto h = save::decode(save::encode(g), kData);
    Eq(h->map->cities[city->index].name, "Newtown", "a renamed city keeps its name");
    Eq(h->map->cities[city->index].slots.back().type, city->slots.back().type,
       "bought production survives a save");
}

static void TestReports(const std::string& scenario) {
    printf("reports: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(23));
    Game& g = *gp;
    Side* side = game::begin(g);
    auto f = report::figures(g, *side, report::CITY);
    Eq(f.result, static_cast<int>(game::sideCities(g, *side).size()),
       "the City report counts the side's cities");
    Eq(f.max % 2, 0, "the top of the scale is even");
    int unused = 0;
    for (int i = 0; i < 8; i++)
        if (f.out[i]) unused++;
    Eq(unused, 8 - static_cast<int>(g.sides.size()), "only the sides in the game get a bar");
    auto a = report::figures(g, *side, report::ARMY);
    int mine = 0;
    for (Army* army : g.armies)
        if (army->owner == side->index) mine++;
    Eq(a.result, mine, "the Army report counts the side's armies");
    Eq(report::figures(g, *side, report::GOLD).result, side->gold,
       "the Gold report is the treasury");
    auto w = report::figures(g, *side, report::WINNING);
    Eq(w.max, 100, "the Winning report's scale is fixed");
    Ok(w.result >= 0 && w.result <= 7, "and says where the side comes");
    side->gold = 100000;
    Eq(report::figures(g, *side, report::WINNING).result, 0, "a rich enough side comes first");
    Eq(report::score(g, *side), 500, "and its score stops at 500");
    City* city = side->capital;
    game::setProduction(g, *city, 0);
    city->countdown = 1;
    int turns = 0;
    do {
        EndTurn(g);
        turns++;
    } while (!(g.side == side || turns > 20));
    Ok(!side->produced.empty(), "the side's production is logged");
    if (!side->produced.empty()) {
        Eq(side->produced[0].city, city->index, "with the city that built it");
        Eq(side->produced[0].kind, "built", "kept at home");
    }
    Eq(report::figures(g, *side, report::PRODUCTION).result,
       static_cast<int>(side->produced.size()), "the Production report counts what was built");
}

static void TestDisband(const std::string& scenario) {
    printf("disband: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(29));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* h = hero::recruit(g, *side, *side->heroOffer).hero;
    auto stack = game::armiesAt(g, h->x, h->y);
    h->strength = 5;
    auto fight = combat::stackStrengths(g, {h}, h->x, h->y);
    Eq(fight[h], 5 + combat::HERO_TABLE[5] + 1,
       "View > Stack: a hero's strength, its table value and its standard");
    size_t n = g.armies.size();
    side->quest = std::make_shared<Quest>();
    side->quest->type = quest::OCCUPY;
    side->quest->hero = h;
    Item* std = h->items[0];
    game::disband(g, *side, stack);
    Eq(g.armies.size(), n - stack.size(), "the stack is gone");
    Eq(static_cast<int>(game::armiesAt(g, h->x, h->y).size()), 0, "from its tile");
    Eq(std->status, 1, "the hero's standard lies on the ground");
    Ok(!side->quest, "and the quest is lost with the hero");
    Sign* sg = game::signAt(g, 18, 12);
    Ok(sg != nullptr, "a sign at (18, 12)");
    if (sg) {
        Eq(sg->lines[0], "Traveller! Pray excuse the ", "its first line");
        Eq(sg->lines[1], "sulphur fumes. Keep On!", "and its second");
        sg->lines[0] = "Hello there";
    }
    auto back = save::decode(save::encode(g), kData);
    Eq(game::signAt(*back, 18, 12)->lines[0], "Hello there", "an edited sign survives a save");
    g.map->fightOrder[side->index][3] = 26;
    back = save::decode(save::encode(g), kData);
    Eq(back->map->fightOrder[side->index][3], 26, "so does an edited fight order");
    int diplo = side->diploScore;
    game::resign(g, *side);
    Eq(static_cast<int>(game::sideCities(g, *side).size()), 0, "a side that resigns holds no city");
    Eq(static_cast<int>(game::sideArmies(g, *side).size()), 0, "and has no army");
    Ok(side->capital->razed, "its capital is ruins");
    Eq(side->diploScore, diplo, "burning its own on resigning costs nothing");
}

static void TestHistory(const std::string& scenario) {
    printf("history: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(41));
    Game& g = *gp;
    Side* side = game::begin(g);
    history::deed(g, side, DeedType::FOUND, history::FOUND_SAGE, 0, "Hero");
    history::deed(g, side, DeedType::CITY_WON, side->index, 3, side->name);
    history::deed(g, side, DeedType::HERO_EMERGES, 1, 0, "Hero");
    Eq(static_cast<int>(g.deeds[side->index].size()), 2, "two deeds a side");
    std::vector<history::DeedType> types = {g.deeds[side->index][0].type,
                                            g.deeds[side->index][1].type};
    std::sort(types.begin(), types.end());
    Eq(types[0], DeedType::HERO_EMERGES, "a lower type pushes out the higher");
    Eq(types[1], DeedType::CITY_WON, "and the lower of the two stays");
    history::deed(g, side, DeedType::AT_PEACE, 0, 1, "");
    Eq(static_cast<int>(g.deeds[side->index].size()), 2, "a higher type does not get in");
    int guard = 0;
    while (g.turn < 3 && guard < 50) {
        EndTurn(g);
        guard++;
    }
    Ok(!g.history.empty(), "a record for the round");
    if (!g.history.empty()) {
        const auto& r = g.history[0];
        Eq(r.owners.size(), g.map->cities.size(), "every city's owner");
        Ok(r.events.size() >= 2, "with the deeds");
    }
    Ok(!g.deeds.count(side->index) || g.deeds[side->index].empty(), "and the slots cleared");
    auto back = save::decode(save::encode(g), kData);
    Eq(back->history.size(), g.history.size(), "history survives a save");
    Side* foe = g.sides[1];
    Item std;
    std.index = foe->index;
    Army dead;
    dead.owner = foe->index;
    dead.type = 28;
    dead.items = {&std};
    history::tally(g, &dead, side->index);
    Eq(history::triumph(g, side->index, foe->index, history::Loss::HEROES), 1, "a hero killed");
    Eq(history::triumph(g, side->index, foe->index, history::Loss::STANDARDS), 1,
       "and the standard it carried");
    Eq(history::triumph(g, foe->index, foe->index, history::Loss::HEROES), 1,
       "the loser counts its own loss");
}

static void TestSetup() {
    printf("new-game setup\n");
    GameOpts o = SeedWith(5, {{"quests", 0}});
    o.sides[0] = SideSetup(false);
    o.sides[1] = SideSetup(true, 2);
    o.sides[2] = SideSetup(false, NONE, true);
    auto gp = NewGame("ERYTHEA", o);
    Game& g = *gp;
    Eq(g.map->options.quests, 0, "the options chosen");
    Ok(!g.map->sides[0].computer, "side 0 human");
    Ok(g.map->sides[1].computer && g.map->sides[1].level == 2, "side 1 a Warlord");
    Ok(!g.map->sides[2].inUse, "side 2 left out");
    City* cap = g.map->sides[2].capital;
    Eq(cap ? cap->ownerIndex : NONE, NONE, "and its capital neutral");
    for (Side* s : g.sides) Ok(s->index != 2, "not among the sides in play");
    // a save keeps the setup, as the original's keeps the .SCN image it was
    // made in (block 2, docs/formats/save.md): a side Off stays out, and a
    // retyped name stays retyped
    g.map->sides[0].name = "Ozymandias";
    auto back = save::decode(save::encode(g), kData);
    Ok(!back->map->sides[2].inUse && !back->map->sides[2].alive,
       "a side Off stays out once loaded");
    Eq(static_cast<int>(back->sides.size()), static_cast<int>(g.sides.size()),
       "as many sides in play once loaded");
    for (Side* s : back->sides) Ok(s->index != 2, "the side Off not among them");
    Eq(back->map->sides[0].name, std::string("Ozymandias"), "a retyped name kept once loaded");
    Eq(back->map->sides[1].level, 2, "and a computer's level");
    // and the rest of the setup: every option, a computer's character, I am
    // the Greatest, and what the hidden map has uncovered
    std::vector<std::pair<std::string, int>> options = {
        {"neutralCities", 2},   {"diplomacy", 0},      {"quests", 0},        {"hiddenMap", 1},
        {"viewEnemies", 0},     {"viewProduction", 1}, {"intenseCombat", 1}, {"quickStart", 1},
        {"militaryAdvisor", 0}, {"randomTurns", 1}};
    GameOpts f = SeedWith(5, options);
    f.greatest = true;
    f.sides[1] = SideSetup(true, 1);
    f.sides[1].card = 3;
    auto full = NewGame("ERYTHEA", f);
    game::reveal(*full, 0, 50, 80, true);
    full->surrenderOffered = true;
    auto loaded = save::decode(save::encode(*full), kData);
    Ok(game::surrenderPending(*loaded), "a surrender not yet put to the player kept once loaded");
    for (auto& [k, v] : options)
        Eq(*loaded->map->options.field(k), v, "option " + k + " kept once loaded");
    Ok(loaded->greatest, "I am the Greatest kept once loaded");
    Eq(loaded->map->sides[1].card, 3, "a computer's character kept once loaded");
    Eq(loaded->map->sides[1].level, 1, "and its level");
    Ok(loaded->map->sides[1].computer, "and that it is a computer");
    Ok(game::seen(*loaded, 0, 50, 80), "the hidden map's uncovered tiles kept once loaded");
    Ok(!game::seen(*loaded, 0, 90, 20), "and the covered ones covered");
    GameOpts q = SeedWith(5, {{"quickStart", 1}});
    q.sides[2] = SideSetup(false, NONE, true);
    auto quick = NewGame("ERYTHEA", q);
    std::map<int, int> counts;
    int neutral = 0;
    for (auto& c : quick->map->cities) {
        if (c.ownerIndex == NONE)
            neutral++;
        else
            counts[c.ownerIndex]++;
    }
    Eq(neutral, 0, "with Quick Start no city is left neutral");
    Ok(!counts.count(2), "a side left out gets none");
    int least = 1 << 30, most = 0;
    for (Side* s : quick->sides) {
        least = std::min(least, counts[s->index]);
        most = std::max(most, counts[s->index]);
        Ok(s->capital->ownerIndex == s->index, "each side keeps its capital");
    }
    Ok(most - least <= 1, "and the cities go round evenly");
    for (Army* a : quick->armies) {
        City* c = game::cityAt(*quick, a->x, a->y);
        if (c) Eq(a->owner, c->ownerIndex, "a dealt city's garrison is its owner's");
    }
    GameOpts sl = Seed(5);
    sl.sides[2] = SideSetup(false, NONE, true);
    auto slow = NewGame("ERYTHEA", sl);
    neutral = 0;
    for (auto& c : slow->map->cities)
        if (c.ownerIndex == NONE) neutral++;
    Ok(neutral > 0, "without it the other cities stay neutral");
}

static void TestSage(const std::string& scenario) {
    printf("sage: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(31));
    Game& g = *gp;
    Side* side = game::begin(g);
    int hx = side->capital->x, hy = side->capital->y;
    for (auto& s : g.map->sites) {
        s.rich = false;
        s.revealed = 0xff;
        s.searched = false;
    }
    Eq(static_cast<int>(site::sageList(g, *side, hx, hy).size()), 0,
       "no rich site unshown: nothing to tell");
    auto d2 = [&](const Site* s) { return (s->x - hx) * (s->x - hx) + (s->y - hy) * (s->y - hy); };
    std::vector<Site*> near;
    for (auto& s : g.map->sites)
        if (std::sqrt(static_cast<double>(d2(&s))) < site::SAGE_RANGE) near.push_back(&s);
    std::stable_sort(near.begin(), near.end(), [&](Site* a, Site* b) { return d2(a) < d2(b); });
    Ok(near.size() >= 3, "three sites within reach");
    if (near.size() < 3) return;
    Site* g1 = near[2];
    Site* g2 = near[0];
    Site* isite = near[1];
    for (Site* s : {g1, g2, isite}) {
        s->rich = true;
        s->revealed = 0;
    }
    g1->content = site::GOLD;
    g2->content = site::GOLD;
    isite->content = site::ITEM;
    isite->item = g.map->items[9].index;
    auto list = site::sageList(g, *side, hx, hy);
    Eq(static_cast<int>(list.size()), 2, "gold once however many, and the item");
    const site::SageEntry* gold = nullptr;
    for (auto& e : list)
        if (e.kind == "gold") gold = &e;
    Ok(gold != nullptr, "a gold entry");
    if (!gold) return;
    site::SageEntry gold_e = *gold;
    Eq(site::sageShow(g, *side, gold_e, hx, hy), g2, "the nearest gold site is shown");
    Ok(((g2->revealed >> side->index) & 1) == 1, "and marked shown to the side");
    Eq(site::sageShow(g, *side, gold_e, hx, hy), g1, "then the next one");
    auto items = site::sageList(g, *side, hx, hy);
    Eq(static_cast<int>(items.size()), 1, "only the item is left");
    if (!items.empty()) Eq(items[0].item, &g.map->items[9], "by its record");
    int before = side->gold;
    int n = site::sageGem(g, *side);
    Ok(n >= 503 && n <= 2000, "a gem is 3d500 + 500");
    Eq(side->gold, before + n, "and is paid");
    g.map->options.hiddenMap = 1;
    g.explored.clear();
    auto r = site::sageMap(g, *side, 3, 3);
    int x0 = r[0], y0 = r[1], w = r[2], h = r[3];
    Eq(x0, 0, "the patch is kept on the map");
    Ok(w >= 16 && w <= 25 && h >= 16 && h <= 25, "16-25 tiles a side");
    Ok(game::seen(g, side->index, x0 + w - 1, y0 + h - 1), "and uncovered");
}

static void TestHeroItems(const std::string& scenario) {
    printf("hero items: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(29));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* h = hero::recruit(g, *side, *side->heroOffer).hero;
    Eq(h->items[0], &g.map->items[side->index],
       "the first hero carries the side's own standard record");
    Eq(h->items[0]->status, 3, "which is carried");
    Eq(static_cast<int>(hero::itemsHere(g, h).size()), 1, "nothing else is listed yet");
    Item* it = &g.map->items[8];
    it->status = 1;
    it->x = h->x;
    it->y = h->y;
    auto list = hero::itemsHere(g, h);
    Eq(static_cast<int>(list.size()), 2, "an item on the hero's tile is listed");
    if (list.size() > 1) Eq(list[1], it, "after what the hero carries");
    hero::takeItem(g, h, it);
    Eq(it->status, 3, "taking an item carries it");
    Eq(static_cast<int>(h->items.size()), 2, "the hero has it");
    hero::dropItem(g, h, it);
    Eq(it->status, 1, "dropping puts it on the ground");
    Eq(it->x, h->x, "where the hero stands");
    Eq(static_cast<int>(h->items.size()), 1, "and the hero no longer has it");
    Ok(!game::plantFlag(g, *side, h), "no flag in a city");
    int fx = NONE, fy = NONE;
    for (int dy = -3; dy <= 4; dy++) {
        for (int dx = -3; dx <= 4; dx++) {
            int x = h->x + dx, y = h->y + dy;
            int t = scn::terrainAt(*g.map, x, y);
            if (fx == NONE && (t == movement::PLAIN || t == movement::ROAD) &&
                !game::cityAt(g, x, y)) {
                fx = x;
                fy = y;
            }
        }
    }
    Ok(fx != NONE, "found open ground near the capital");
    h->x = fx;
    h->y = fy;
    Ok(game::plantFlag(g, *side, h), "the flag is planted");
    Eq(game::standardAt(g, *side).first, fx, "where the hero stands");
    Ok(!game::plantFlag(g, *side, h), "and only once");
    City* city = side->capital;
    game::setProduction(g, *city, 0);
    city->countdown = 1;
    Ok(game::vectorToStandard(g, *city, *side), "a city may vector to the standard");
    size_t before = game::armiesAt(g, fx, fy).size();
    for (size_t i = 0; i < 3 * g.sides.size() + 1; i++) EndTurn(g);
    Ok(game::armiesAt(g, fx, fy).size() > before, "a vectored army arrives at the standard");
    hero::takeItem(g, h, &g.map->items[side->index]);
    Eq(game::standardAt(g, *side).first, NONE, "a standard picked up is no longer planted");
}

static void TestMovement(const std::string& scenario) {
    printf("movement: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(11));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* army = game::sideArmies(g, *side)[0];
    std::vector<Army*> stack = {army};
    Eq(movement::COST[movement::MOUNTAINS], 0, "mountains are impassable");
    Eq(movement::COST[movement::ROAD], 1, "a road costs 1");
    Eq(movement::COST[movement::HILLS], 6, "hills cost 6");
    auto& grid = movement::grid(g, side->index);
    int roaded = 0;
    bool road_cost_ok = true;
    for (int y = 0; y < g.map->height; y++) {
        for (int x = 0; x < g.map->width; x++) {
            if (scn::roadAt(*g.map, x, y) % 0x20 != 0) {
                roaded++;
                if (grid[y * g.map->width + x] % 8 != 1) road_cost_ok = false;
            }
        }
    }
    Ok(roaded > 0, "the scenario has roads");
    Ok(road_cost_ok, "every road tile costs 1");
    const int water_flag = movement::WATER_F;
    const int land = 2, water = 2 | water_flag;
    auto sc = [](std::optional<int> v) { return v ? *v : NONE; };
    Eq(sc(movement::stepCost(land, water, movement::LAND, false, false, 10)), NONE,
       "a land stack cannot enter open water");
    Eq(sc(movement::stepCost(land, 0, movement::LAND, false, false, 10)), NONE,
       "nothing enters a cost-0 tile on land");
    Eq(sc(movement::stepCost(land, 6 | movement::HILLS_F, movement::LAND, false, false, 10)), 6,
       "hills cost 6 without the bonus");
    Eq(sc(movement::stepCost(land, 6 | movement::HILLS_F, movement::LAND, false, true, 10)), 2,
       "the hills bonus brings them to 2");
    Eq(sc(movement::stepCost(land, 4 | movement::FOREST_F, movement::LAND, true, false, 10)), 2,
       "the woods bonus brings forest to 2");
    const int cross = 1 | water_flag | movement::CROSS_F;
    Eq(sc(movement::stepCost(land, cross, movement::LAND, false, false, 10)), 1,
       "a crossing tile is free of the water charge");
    Eq(sc(movement::stepCost(cross, water, movement::LAND, false, false, 10)), 2 + 10,
       "stepping from a crossing into water costs the penalty");
    Eq(sc(movement::stepCost(cross, water, movement::LAND, false, false, 20)), 2 + 20,
       "a move aimed at water pays 20");
    Eq(sc(movement::stepCost(land, 0, movement::FLYING, false, false, 10)), 2,
       "a flier crosses mountains at 2");
    Eq(sc(movement::stepCost(land, water, movement::FLYING, false, false, 10)), 2,
       "a flier crosses water at 2");
    Eq(sc(movement::stepCost(land, 1, movement::FLYING, false, false, 10)), 1,
       "a flier pays 1 on a road");
    Eq(sc(movement::stepCost(land, 6 | movement::HILLS_F, movement::FLYING, false, false, 10)), 2,
       "a flier pays 2 over hills");
    Eq(sc(movement::stepCost(land, 0 + movement::CITY_F, movement::FLYING, false, false, 10)), NONE,
       "a flier cannot cross a city that is not its own (1555:08bf)");
    Eq(movement::distance(5, 5, 6, 6), 1, "a diagonal neighbour is one away");
    Eq(movement::distance(5, 5, 7, 6), 2, "a knight's move is two");
    Eq(movement::direction(5, 5, 6, 4), 1, "up and right is north-east");
    Eq(movement::direction(5, 5, 9, 4), 1, "however far right");
    Eq(movement::direction(5, 5, 5, 9), 4, "straight down is south");
    Eq(sc(movement::stepCost(water, land, movement::BOAT, false, false, 10)), NONE,
       "a boat cannot go ashore");
    Eq(sc(movement::stepCost(water, water, movement::BOAT, false, false, 10)), 2,
       "a boat moves on water");
    army->moves = 7;
    Army m3, m9;
    m3.moves = 3;
    m9.moves = 9;
    Eq(movement::stackMoves({army, &m3, &m9}), 3, "a stack moves at the pace of its slowest army");

    army->moves = army->maxMoves;
    int from_x = army->x, from_y = army->y;
    bool found = false;
    int tx = 0, ty = 0;
    for (int dx = -6; dx <= 6; dx++) {
        for (int dy = -6; dy <= 6; dy++) {
            int x = from_x + dx, y = from_y + dy;
            if (!found && x >= 0 && y >= 0 && x < g.map->width && y < g.map->height &&
                !game::cityAt(g, x, y)) {
                auto p = movement::findPath(g, stack, from_x, from_y, x, y);
                if (p && p->size() >= 2 && p->back().cost < movement::PAST_SHORE &&
                    !movement::has(movement::grid(g, side->index)[y * g.map->width + x],
                                   movement::WATER_F)) {
                    found = true;
                    tx = x;
                    ty = y;
                }
            }
        }
    }
    Ok(found, "found somewhere to walk to");
    if (found) {
        int before = army->moves;
        auto r = movement::moveTo(g, stack, tx, ty);
        Ok(r.steps > 0, "the stack moved");
        Eq(army->moves, std::max(0, before - r.spent), "the cost was charged to the army");
        Ok(army->x != from_x || army->y != from_y, "the army is somewhere else");
    }
    army->moves = 0;
    auto r = movement::moveTo(g, stack, from_x, from_y + 1);
    Eq(r.steps, 0, "an army with no moves stays put");
    City* enemy = nullptr;
    for (auto& c : g.map->cities)
        if (!enemy && c.ownerIndex != side->index) enemy = &c;
    Ok(enemy != nullptr, "there is a city to attack");
    if (enemy) {
        army->moves = 99;
        army->x = enemy->x;
        army->y = enemy->y - 1;
        auto path = movement::findPath(g, stack, army->x, army->y, enemy->x, enemy->y);
        if (path && !path->empty()) {
            auto res = movement::walk(g, stack, *path);
            Eq(res.stopped, "attack", "stepping into an enemy city starts an attack");
            Ok(res.attack && res.attack->city == enemy, "the attack names the city");
            Eq(army->x, enemy->x, "the attacker has not entered the city");
            Ok(army->y == enemy->y - 1, "the attacker stayed where it was");
        }
        auto& grid2 = movement::grid(g, side->index);
        Eq(grid2[enemy->y * g.map->width + enemy->x] % 8, 0,
           "an enemy city is impassable in the cost grid");
    }
    {
        Army* a = AddArmy(g, army->x, army->y, side->index, 11, 9, 20, 20);
        a->upkeep = 1;
        int far_x = 0, far_y = 0;
        std::optional<movement::Preview> route;
        for (int d = 12; d <= 30; d += 2) {
            far_x = std::min(g.map->width - 2, a->x + d);
            far_y = a->y;
            route = movement::preview(g, {a}, far_x, far_y);
            if (route && route->reach < static_cast<int>(route->path.size())) break;
        }
        if (route) {
            Ok(!route->path.empty(), "a stack under orders has a route to draw");
            Ok(route->reach <= static_cast<int>(route->path.size()),
               "it can walk no more of it than there is");
            Eq(route->path.back().x, far_x, "the route ends where it was sent");
            Eq(route->path.back().y, far_y, "on that row");
            int px = a->x, py = a->y;
            bool straight = true;
            for (auto& step : route->path) {
                if (std::max(std::abs(step.x - px), std::abs(step.y - py)) != 1) straight = false;
                px = step.x;
                py = step.y;
            }
            Ok(straight, "and it is a chain of single steps");
            r = movement::moveTo(g, {a}, far_x, far_y);
            Eq(static_cast<int>(r.walked.size()), r.steps,
               "a walk reports the tiles it covered, one per step taken");
            if (r.steps > 0) {
                Eq(r.walked.back().first, a->x, "ending where the stack now stands");
                Eq(r.walked.back().second, a->y, "on that row");
            }
            auto left = movement::preview(g, {a}, far_x, far_y);
            if (left)
                Ok(left->reach < static_cast<int>(left->path.size()),
                   "what is left of it is out of reach this turn");
        }
        g.remove(a);
    }
}

static void TestStackLimit(const std::string& scenario) {
    printf("stack limit: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(13));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* army = game::sideArmies(g, *side)[0];
    int tx = army->x, ty = army->y - 1;
    if (ty < 0) {
        tx = army->x;
        ty = army->y + 1;
    }
    for (int i = 0; i < rules::MAX_STACK; i++) {
        Army* a = AddArmy(g, tx, ty, side->index, army->type, 1, 10, 10);
        a->homeCity = army->homeCity;
    }
    Eq(static_cast<int>(game::armiesAt(g, tx, ty).size()), rules::MAX_STACK, "the tile is full");
    army->moves = 99;
    auto r = movement::moveTo(g, {army}, tx, ty);
    Eq(r.steps, 0, "a stack cannot stop on a full tile");
    Ok(army->x != tx || army->y != ty, "the army stayed off the full tile");
}

static void TestCombat(const std::string& scenario) {
    printf("combat: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(17));
    Game& g = *gp;
    Side* side = game::begin(g);
    Eq(combat::HERO_TABLE[0], 0, "a strength-0 hero commands nothing");
    Eq(combat::HERO_TABLE[4], 1, "strength 4 gives +1");
    Eq(combat::HERO_TABLE[9], 3, "strength 9 gives +3");
    Eq(combat::CITY, 0, "city is class 0");
    Eq(combat::OPEN, 1, "open is class 1");
    Eq(combat::WOODS, 2, "woods is class 2");
    Eq(combat::HILLS, 3, "hills is class 3");
    std::set<int> seen;
    for (int y = 0; y < g.map->height; y += 7) {
        for (int x = 0; x < g.map->width; x += 7) {
            int cls = combat::terrainClass(g, x, y);
            int t = scn::terrainAt(*g.map, x, y);
            seen.insert(cls);
            if (t == movement::FOREST) Eq(cls, combat::WOODS, "forest fights as woods");
            if (t == movement::HILLS || t == movement::MOUNTAINS)
                Eq(cls, combat::HILLS, "hills and mountains fight as hills");
            if (t == movement::CITY || t == movement::SITE)
                Eq(cls, combat::CITY, "cities and sites fight as city");
            if (t == movement::PLAIN || t == movement::MARSH || t == movement::WATER)
                Eq(cls, combat::OPEN, "plain, marsh and water fight as open");
        }
    }
    Ok(seen.count(combat::OPEN), "the map has open ground");
    Eq(g.map->combatCap, 5, "the combat cap is 5");
    const auto& row = g.map->fightOrder[0];
    std::set<int> used;
    for (int t = 0; t <= 28; t++) used.insert(row[t]);
    Eq(static_cast<int>(used.size()), 29, "fight order is a permutation of 29 ranks");
    City* neutral = nullptr;
    for (auto& c : g.map->cities)
        if (!neutral && c.ownerIndex == NONE && c.defence == 2) neutral = &c;
    if (neutral) {
        int cls = combat::terrainClass(g, neutral->x, neutral->y);
        Eq(combat::fortify(g, {}, neutral->x, neutral->y, cls), 1,
           "a neutral city's defence 2 fortifies for 1");
        game::setCityOwner(g, *neutral, g.sides.back()->index);
        Eq(combat::fortify(g, {}, neutral->x, neutral->y, cls), 2,
           "an owned city's defence 2 fortifies for 2");
        game::setCityOwner(g, *neutral, NONE);
    }
    const ArmyType* siege = nullptr;
    for (auto& a : g.types.all)
        if (a.bonus[52] == combat::SIEGE) siege = &a;
    if (siege && neutral) {
        int cls = combat::terrainClass(g, neutral->x, neutral->y);
        Army s;
        s.type = siege->id;
        Eq(combat::fortify(g, {&s}, neutral->x, neutral->y, cls), 0,
           "a Siege attacker cancels the city bonus");
    }
    Army boat;
    boat.type = 11;
    boat.strength = 9;
    boat.atSea = true;
    Eq(combat::strength(g, boat, 5, combat::OPEN, movement::WATER), 4,
       "an army at sea on water fights at 4");
    Eq(combat::strength(g, boat, 5, combat::OPEN, movement::SHORE), 4,
       "an army at sea on shore fights at 4");
    boat.atSea = false;
    Ok(combat::strength(g, boat, 5, combat::OPEN, movement::PLAIN) > 4,
       "ashore it fights normally");
    Army strong;
    strong.type = 11;
    strong.strength = 9;
    Eq(combat::strength(g, strong, 99, combat::OPEN, movement::PLAIN), 15,
       "strength is capped at 15");
    Army* a1 = game::sideArmies(g, *side)[0];
    City* enemy = nullptr;
    for (auto& c : g.map->cities)
        if (!enemy && c.ownerIndex != side->index) enemy = &c;
    auto l = combat::lines(g, {a1}, enemy->x, enemy->y);
    Eq(static_cast<int>(l.attackers.size()), 1, "the attacking line is the stack");
    Ok(!l.defenders.empty(), "the city has defenders");
    Eq(l.city, enemy, "the battle names the city");
    for (Army* d : l.defenders) Ok(d->owner != side->index, "no defender belongs to the attacker");
    bool sorted = true;
    for (size_t i = 1; i < l.defenders.size(); i++) {
        const auto& owner_row = g.map->fightOrder[l.defOwner == NONE ? 8 : l.defOwner];
        if (owner_row[l.defenders[i]->type] < owner_row[l.defenders[i - 1]->type]) sorted = false;
    }
    Ok(sorted, "the defending line is sorted by fight order");
    auto r = combat::resolve(g, l.attackers, l.defenders, enemy->x, enemy->y);
    Ok(!r.log.empty(), "the battle logged at least one death");
    Eq(r.log.size(), r.deadAttackers.size() + r.deadDefenders.size(),
       "one log entry per dead army");
    Ok(r.won == r.defenders.empty(), "the attacker wins only if every defender is dead");
    auto odds = [&](int atk_strength, int n_atk, int def_strength, int n_def) {
        std::vector<Army> attackers(n_atk), defenders(n_def);
        std::vector<Army*> pa, pd;
        for (auto& a : attackers) {
            a.type = 11;
            a.strength = atk_strength;
            a.owner = side->index;
            pa.push_back(&a);
        }
        for (auto& d : defenders) {
            d.type = 11;
            d.strength = def_strength;
            d.owner = NONE;
            pd.push_back(&d);
        }
        int wins = 0;
        for (int i = 0; i < 200; i++)
            if (combat::resolve(g, pa, pd, 0, 0).won) wins++;
        return wins;
    };
    Ok(odds(9, 8, 1, 1) > 190, "eight strong armies beat one weak one");
    Ok(odds(1, 1, 9, 8) < 10, "one weak army loses to eight strong ones");
    int many = odds(3, 8, 9, 1);
    Ok(many > 150, "many weak armies beat one strong one: " + std::to_string(many));
    Eq(combat::ADVICE_BATTLES, 19, "the advisor fights 19 battles");
    auto [verdict, wins] = combat::advise(g, l.attackers, l.defenders, enemy->x, enemy->y);
    Ok(!verdict.empty(), "the advisor has a verdict");
    Ok(wins >= 0 && wins <= 19, "the advisor's win count is in range");
    Eq(verdict, std::string(combat::ADVICE[wins / 2]), "the verdict is wins/2 into the table");

    // The Military Advisor evaluates any of the 4 tiles of a 2x2 city consistently
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            auto l_city = combat::lines(g, {a1}, enemy->x + dx, enemy->y + dy);
            Eq(l_city.city, enemy, "combat lines identify the 2x2 city from any tile");
            Eq(static_cast<int>(l_city.defenders.size()), static_cast<int>(l.defenders.size()),
               "defenders are gathered across all 4 tiles");
        }
    }
    Eq(g.map->options.militaryAdvisor, 1, "the military advisor is enabled by default");
}

static void TestCapture(const std::string& scenario) {
    printf("capture: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(23));
    Game& g = *gp;
    Side* side = game::begin(g);
    City* target = nullptr;
    for (auto& c : g.map->cities)
        if (!target && c.ownerIndex == NONE) target = &c;
    Ok(target != nullptr, "found a neutral city");
    std::vector<Army*> stack;
    for (int i = 0; i < 8; i++) {
        Army* a = AddArmy(g, target->x, target->y - 1, side->index, 11, 9, 20, 20);
        a->upkeep = 1;
        stack.push_back(a);
    }
    {
        Army* far = AddArmy(g, 0, 0, side->index, 11, 9, 20, 20);
        far->upkeep = 1;
        movement::WalkResult walk;
        for (auto off : std::vector<std::pair<int, int>>{
                 {-3, -3}, {3, -3}, {-3, 3}, {3, 3}, {0, -3}, {0, 3}, {-3, 0}, {3, 0}}) {
            far->x = target->x + off.first;
            far->y = target->y + off.second;
            far->moves = 20;
            walk = movement::moveTo(g, {far}, target->x, target->y);
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
        Eq(target->ownerIndex, NONE, "and the city is still the defender's");
        g.remove(far);
    }
    int before = static_cast<int>(game::sideCities(g, *side).size());
    // a city of the loser's vectoring here stops building (67cc:0a6b); the
    // target is neutral, so stand one in for the purpose
    City* sender = nullptr;
    for (auto& c : g.map->cities)
        if (!sender && &c != target && !c.slots.empty()) sender = &c;
    sender->producing = 0;
    sender->countdown = 2;
    sender->vectorTo = target->index;
    Battle r;
    {
        int i = target->y * MAP_W + target->x;
        int tile = g.map->tiles[i];
        size_t armies = g.armies.size();
        int from = stack[0]->x;
        r = game::decideAttack(g, stack, target->x, target->y);
        Ok(r.won, "the fight is decided");
        Eq(target->ownerIndex, NONE, "but the city is not yet taken");
        Eq(g.map->tiles[i], tile, "nor its castle repainted");
        Eq(g.armies.size(), armies, "nor the dead removed");
        Eq(stack[0]->x, from, "nor the survivors moved in");
        Eq(r.captured, static_cast<City*>(nullptr), "and nothing is said to be captured");
        game::applyAttack(g, r);
        Ok(g.map->tiles[i] != tile, "applied, the castle is in the victor's colours");
        int owner = target->ownerIndex;
        size_t n = g.armies.size();
        game::applyAttack(g, r);
        Eq(g.armies.size(), n, "and applying again changes nothing");
        Eq(target->ownerIndex, owner, "and the owner stays");
    }
    Ok(r.won, "the overwhelming stack took the city");
    Eq(sender->vectorTo, NONE, "a city that sent its armies to the captured one stops sending");
    Eq(sender->producing, NONE, "and stops building");
    Eq(r.lines.attackers.size(), stack.size(), "every attacker is in the line");
    Eq(r.lines.city, target, "and the line knows it was a city");
    size_t down = 0;
    for (int s : r.log) {
        Ok(s == 0 || s == 1, "the log says which side lost an army");
        down++;
    }
    Eq(down, r.deadAttackers.size() + r.deadDefenders.size(),
       "the log has one entry per army that fell");
    Eq(target->ownerIndex, side->index, "the city changed hands");
    Eq(static_cast<int>(game::sideCities(g, *side).size()), before + 1,
       "the side owns one more city");
    Eq(target->producing, NONE, "a captured city is not building anything");
    for (Army* a : r.attackers) Eq(a->x, target->x, "the survivors moved in");
    for (Army* dead : r.deadDefenders) Ok(!g.alive(dead), "a dead defender is removed");
    Side* victim = g.sides.back();
    victim->gold = 400;
    Eq(game::loot(g, victim), 200, "one city: half its gold");
    City* extra = nullptr;
    for (auto& c : g.map->cities)
        if (!extra && c.ownerIndex == NONE && &c != target) extra = &c;
    if (extra) {
        game::setCityOwner(g, *extra, victim->index);
        Eq(game::loot(g, victim), (400 / 2) / 2, "two cities: half the per-city share");
    }
}

static void TestTutorialHero() {
    printf("tutorial: a hero cannot die attacking neutrals\n");
    auto gp = NewGame("TUTORIA", Seed(31));
    Game& g = *gp;
    Side* side = game::begin(g);
    Eq(g.map->options.tutorial, 1, "TUTORIA sets the tutorial flag");
    Ok(!side->computer, "the tutorial player is human");
    Army h;
    h.type = armytype::HERO;
    h.strength = 1;
    h.owner = side->index;
    std::vector<Army> defender_pool(8);
    std::vector<Army*> defenders;
    for (auto& d : defender_pool) {
        d.type = 11;
        d.strength = 9;
        d.owner = NONE;
        defenders.push_back(&d);
    }
    int deaths = 0;
    for (int i = 0; i < 50; i++)
        deaths += static_cast<int>(combat::resolve(g, {&h}, defenders, 0, 0).deadAttackers.size());
    Eq(deaths, 0, "the tutorial hero never dies against neutrals");
    g.map->options.tutorial = 0;
    deaths = 0;
    for (int i = 0; i < 50; i++)
        deaths += static_cast<int>(combat::resolve(g, {&h}, defenders, 0, 0).deadAttackers.size());
    Ok(deaths > 0, "without the tutorial flag the hero can die");
}

static void TestCityChoices(const std::string& scenario) {
    printf("pillage, sack and raze: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(29));
    Game& g = *gp;
    Side* side = game::begin(g);
    auto captured = [&](size_t want) -> City* {
        for (auto& c : g.map->cities) {
            if (c.ownerIndex == NONE && c.slots.size() >= want) {
                game::setCityOwner(g, c, side->index);
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
        int want = std::abs(g.types.byId(dear.type)->price) / 2;
        int gold0 = side->gold, score0 = side->diploScore;
        auto got = game::pillage(g, *side, *city, {});
        Eq(got.gold, want, "pillage pays half the type's purchase price");
        Eq(side->gold, gold0 + want, "the gold was paid");
        Eq(city->slots.size(), n - 1, "pillage removed one type");
        Eq(city->defence, rules::cityDefence(static_cast<int>(city->slots.size())),
           "defence was recomputed");
        int d = side->diploScore - score0;
        Ok(d >= 1 && d <= 5, "pillage costs 1d5 diplomatic score: " + std::to_string(d));
    }
    City* city2 = captured(3);
    if (city2) {
        Slot cheapest = city2->slots[0];
        int want = 0;
        for (size_t i = 1; i < city2->slots.size(); i++)
            want += std::abs(g.types.byId(city2->slots[i].type)->price) / 2;
        int gold0 = side->gold, score0 = side->diploScore;
        size_t n2 = city2->slots.size();
        auto got = game::sack(g, *side, *city2, {});
        Eq(got.gold, want, "sack pays for every type it strips");
        Eq(got.lost.size(), n2 - 1, "sack lists every type it strips");
        int sum = 0;
        for (auto& l : got.lost) sum += l.second;
        Eq(sum, want, "and what each was worth adds up to the gold");
        Eq(side->gold, gold0 + want, "the gold was paid");
        Eq(static_cast<int>(city2->slots.size()), 1, "only the cheapest type is left");
        Ok(city2->slots[0] == cheapest, "and it is the cheapest one");
        Eq(city2->defence, 1, "one type means defence 1");
        int d = side->diploScore - score0;
        Ok(d >= 6 && d <= 15, "sack costs 1d10+5 diplomatic score: " + std::to_string(d));
    }
    City* city3 = captured(1);
    if (city3) {
        int before = static_cast<int>(game::sideCities(g, *side).size());
        City* other = nullptr;
        for (auto& c : g.map->cities)
            if (!other && c.ownerIndex == side->index && &c != city3) other = &c;
        if (other) game::vector(g, *other, city3);
        int score0 = side->diploScore;
        game::raze(g, *side, *city3, {});
        Eq(city3->ownerIndex, NONE, "a razed city is neutral");
        Eq(static_cast<int>(city3->slots.size()), 0, "a razed city produces nothing");
        Eq(city3->income, 0, "a razed city earns nothing");
        Eq(static_cast<int>(game::sideCities(g, *side).size()), before - 1,
           "the side no longer owns it");
        if (other) Eq(other->vectorTo, NONE, "vectoring to it was cancelled");
        int d = side->diploScore - score0;
        Ok(d >= 11 && d <= 25, "raze costs 1d15+10 diplomatic score: " + std::to_string(d));
        int rb = movement::grid(g, side->index)[city3->y * g.map->width + city3->x];
        Ok(!movement::has(rb, movement::CITY_F) && rb % 8 != 0,
           "a razed city's ruins can be walked over: " + std::to_string(rb));
    }
    City empty;
    empty.index = -1;
    Eq(game::pillage(g, *side, empty, {}).gold, 0, "pillaging an empty city pays nothing");
    City one;
    one.index = -1;
    one.slots.push_back(Slot{11, "", 0, 0, 0, 0, 0});
    Eq(game::sack(g, *side, one, {}).gold, 0, "sacking a one-type city pays nothing");
}

static void TestHeroes(const std::string& scenario) {
    printf("heroes: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(37));
    Game& g = *gp;
    Side* side = game::begin(g);
    auto offer = side->heroOffer;
    Ok(offer != nullptr, "a hero offers itself on turn 1");
    if (!offer) return;
    Eq(offer->price, 0, "the first hero is free");
    Eq(offer->city, side->capital, "the first hero appears at the capital");
    const auto& names = hero::names(g, *side);
    Eq(static_cast<int>(names.size()), 100, "a side has a hundred candidate heroes");
    Ok(!offer->name.empty(), "the offer carries a name");
    bool known = false;
    for (auto& n : names)
        if (n.first == offer->name) known = true;
    Ok(known, "and the name comes from that side's file");
    bool heroine = false;
    for (auto& n : hero::names(g, g.map->sides[0]))
        if (n.second) heroine = true;
    Ok(!heroine, "side 0 has no heroines to offer");
    auto hired = hero::recruit(g, *side, *offer);
    Army* h = hired.hero;
    auto& allies = hired.allies;
    Eq(h->type, armytype::HERO, "the recruit is a hero");
    Eq(h->name, offer->name, "the hero keeps the name it was offered under");
    Eq(h->strength, 5, "a new hero has strength 5");
    Eq(h->maxMoves, 14, "a new hero has 14 movement");
    Eq(h->moves, 14, "a new hero joins with its moves already full");
    Eq(static_cast<int>(allies.size()), 0, "the first hero brings no allies");
    Eq(static_cast<int>(h->items.size()), 1, "the first hero carries one item");
    Eq(h->items[0]->type, rules::ITEM_STANDARD, "and it is the side's standard");
    h->experience = 15;
    auto promoted = hero::checkPromotions(g, *side);
    Eq(static_cast<int>(promoted.size()), 1, "15 experience promotes a Hero");
    Eq(h->level, 2, "the hero is now level 2");
    Eq(h->title, "Cavalier", "level 2 is a Cavalier");
    Eq(h->strength, 6, "promotion adds a strength");
    Eq(h->maxMoves, 16, "promotion adds 2 movement");
    Eq(static_cast<int>(hero::checkPromotions(g, *side).size()), 0,
       "no second promotion on the same experience");
    h->experience = 60;
    hero::checkPromotions(g, *side);
    Eq(h->level, 3, "60 experience promotes one step only");
    hero::checkPromotions(g, *side);
    Eq(h->title, "Paladin", "the next check reaches Paladin");
    Eq(static_cast<int>(hero::checkPromotions(g, *side).size()), 0,
       "a Paladin is not promoted again");
    hero::addExperience(g, h, 99);
    Eq(h->experience, rules::MAX_HERO_XP, "experience is capped at 60");
    auto [type, n] = hero::allies(g);
    Ok(type != nullptr, "the allies have a type");
    Ok(n >= 1 && n <= 3, "1 to 3 allies arrive");
    g.turn = 4;
    HeroOffer later;
    later.city = side->capital;
    auto escort = hero::recruit(g, *side, later).allies;
    Ok(!escort.empty(), "a later hero arrives with an escort");
    for (Army* a : escort) {
        Eq(a->moves, a->maxMoves, "an ally joins with its moves already full");
        Eq(a->upkeep, 0, "an ally costs no upkeep");
    }
    g.turn = 1;
    Eq(hero::MAX_PER_SIDE, 5, "a side may hold 5 heroes");
    Eq(hero::MAX_IN_GAME, 40, "the game holds 40 heroes");
    bool blocked = true;
    for (int i = 0; i < 5; i++) AddArmy(g, 0, 0, side->index, armytype::HERO, 5);
    g.turn = 2;
    for (int i = 0; i < 50; i++)
        if (hero::offer(g, *side)) blocked = false;
    Ok(blocked, "no hero is offered once the side is at its limit");
    Item fire;
    fire.name = "Firesword";
    fire.type = rules::ITEM_BATTLE;
    fire.value = 1;
    Army carrier;
    carrier.type = armytype::HERO;
    carrier.owner = side->index;
    carrier.items = {&fire};
    int dx = NONE, dy = NONE;
    for (int y = 0; y < g.map->height && dx == NONE; y++)
        for (int x = 0; x < g.map->width; x++)
            if (dx == NONE && scn::terrainAt(*g.map, x, y) == movement::PLAIN) {
                dx = x;
                dy = y;
            }
    auto dropped = hero::dropItems(g, &carrier, dx, dy);
    Eq(static_cast<int>(dropped.size()), 1, "the item was dropped");
    if (!dropped.empty()) {
        Eq(dropped[0]->status, 1, "a dropped item lies on the ground");
        Eq(dropped[0]->x, dx, "it lies where the hero fell");
    }
    Eq(static_cast<int>(carrier.items.size()), 0, "the dead hero carries nothing");
    int wx = NONE, wy = NONE;
    for (int y = 0; y < g.map->height && wx == NONE; y++)
        for (int x = 0; x < g.map->width; x++)
            if (wx == NONE && scn::terrainAt(*g.map, x, y) == movement::WATER) {
                wx = x;
                wy = y;
            }
    if (wx != NONE) {
        Item ice;
        ice.name = "Icesword";
        ice.type = rules::ITEM_BATTLE;
        ice.value = 1;
        ice.status = 3;
        Army drowner;
        drowner.type = armytype::HERO;
        drowner.owner = side->index;
        drowner.items = {&ice};
        Eq(static_cast<int>(hero::dropItems(g, &drowner, wx, wy).size()), 0,
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
        def_hero.type = armytype::HERO;
        def_hero.strength = 5;
        def_hero.owner = 1;
        att.type = 11;
        att.strength = 3;
        att.owner = 0;
        Battle b;
        hero::battleExperience(g, {&att}, {&def_hero}, b, false);
        return def_hero.experience;
    };
    rules::bugs.heroExperienceReadsAttackerTypes = true;
    Eq(run(), 0, "with the bug on, a defending hero facing a non-hero gains nothing");
    rules::bugs.heroExperienceReadsAttackerTypes = false;
    Eq(run(), 1, "with the bug off, the defender is credited");
    rules::bugs.heroExperienceReadsAttackerTypes = true;
    Army h;
    h.type = armytype::HERO;
    h.strength = 5;
    h.owner = 0;
    Battle none;
    hero::battleExperience(g, {&h}, {}, none, false);
    Eq(h.experience, 1, "an attacking hero gains 1 in the open");
    hero::battleExperience(g, {&h}, {}, none, true);
    Eq(h.experience, 3, "attacking a city is worth 2");
    Army dead;
    dead.type = armytype::HERO;
    dead.strength = 5;
    dead.owner = 0;
    Battle died;
    died.deadByArmy.insert(&dead);
    hero::battleExperience(g, {&dead}, {}, died, true);
    Eq(dead.experience, 0, "a hero that died gains nothing");
}

static void TestAiGame(const std::string& scenario, int turns) {
    printf("AI game: %s, %d turns\n", scenario.c_str(), turns);
    auto gp = NewGame(scenario, Seed(101));
    Game& g = *gp;
    Side* side = game::begin(g);
    std::map<int, int> owned_at_start;
    for (Side* s : g.sides)
        owned_at_start[s->index] = static_cast<int>(game::sideCities(g, *s).size());
    while (side && g.turn <= turns) {
        PlayAi(g, *side);
        side = EndTurn(g);
    }
    Ok(g.turn > turns || side == nullptr, "the game ran to the turn limit");
    std::map<int, int> per_tile;
    for (Army* a : g.armies) {
        Ok(g.types.byId(a->type) != nullptr, "every army has a real type");
        if (!a->transit) {
            Ok(a->x != NONE && a->y != NONE, "a placed army has a position");
            Ok(a->x >= 0 && a->x < g.map->width && a->y >= 0 && a->y < g.map->height,
               "every army is on the map");
            per_tile[a->y * g.map->width + a->x]++;
        }
        Ok(a->moves >= 0, "movement never goes negative");
        Ok(a->moves <= rules::MAX_MOVE, "movement never exceeds the cap");
    }
    int worst = 0;
    for (auto& [k, v] : per_tile) worst = std::max(worst, v);
    Ok(worst <= rules::MAX_STACK,
       "no tile ever holds more than 8 armies: " + std::to_string(worst));
    for (Side* s : g.sides) {
        Ok(s->gold >= 0, s->name + " never goes into debt");
        if (!s->alive) Ok(game::sideCities(g, *s).empty(), "a dead side owns nothing");
    }
    for (auto& c : g.map->cities) {
        if (c.ownerIndex != NONE)
            Ok(g.map->side(c.ownerIndex) != nullptr, "a city's owner is a real side");
        Ok(c.slots.size() <= 4, "a city never gains production slots");
    }
    bool moved = false;
    int captures = 0;
    for (Side* s : g.sides) {
        int now = static_cast<int>(game::sideCities(g, *s).size());
        if (now != owned_at_start[s->index]) moved = true;
        captures += std::max(0, now - owned_at_start[s->index]);
    }
    Ok(moved, "cities changed hands over " + std::to_string(turns) + " turns");
    Ok(captures > 0, fmt("the computer players took %d cities", captures));
    Ok(g.armies.size() > g.map->cities.size(),
       fmt("armies were built: %d from %d cities", static_cast<int>(g.armies.size()),
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
        s->g.map->fightOrder.assign(9, std::vector<int>(29, 0));
        for (int i = 1; i <= n; i++) {
            Army a;
            a.type = i;
            a.moves = 10 + i;
            a.group = 0;
            s->g.map->fightOrder[0][i] = n - i;  // the first army ranks highest
            s->a.push_back(s->g.add(a));
        }
        return s;
    };
    {
        auto st = stack(3);
        auto& a = st->a;
        auto s = slots::build(st->g, a, 0);
        Eq(s.n, 3, "three armies fill three slots");
        Eq(s.army[0], a[0], "the highest in the fight order takes the first slot");
        Ok(s.inGroup[0] && !s.inGroup[1] && !s.inGroup[2],
           "clicking a tile selects one army, not the stack");
        Eq(s.group[0], 0, "ungrouped armies are each their own group");
        Eq(s.group[2], 2, "numbered off down the bar");
        Eq(s.mark[0], slots::TICK, "the one that moves is ticked");
        Eq(s.mark[1], slots::CROSS, "the others are crossed");
        Eq(slots::moves(s), 11, "and Group Move is that army's own");
        Eq(slots::grouped(s), false, "so the Grp button is not green yet");
        Eq(static_cast<int>(slots::selected(s).size()), 1, "one army moves");
    }
    {
        auto st = stack(3);
        auto s = slots::build(st->g, st->a, 0);
        slots::toggle(s, st->g, 1);
        Ok(s.inGroup[1] && s.group[1] == s.group[0], "a click adds an army to the group");
        Eq(s.mark[1], slots::NOMARK, "only the head of a group is marked");
        Eq(slots::moves(s), 11, "the group moves at its slowest army's pace");
        slots::toggle(s, st->g, 1);
        Ok(!s.inGroup[1], "clicking it again drops it out");
        Ok(s.group[1] != s.group[0], "into a group of its own");
        slots::toggle(s, st->g, 0);
        Ok(s.inGroup[0], "the last army of a group cannot be dropped");
    }
    {
        auto st = stack(3);
        auto s = slots::build(st->g, st->a, 0);
        slots::all(s, st->g);
        Eq(static_cast<int>(slots::selected(s).size()), 3, "Grp takes the whole stack");
        Eq(slots::grouped(s), true, "and turns the button green");
        Eq(s.mark[1], slots::NOMARK, "one mark for the one group");
        slots::single(s, st->g);
        Eq(static_cast<int>(slots::selected(s).size()), 1, "and Grp again breaks it up");
        Eq(slots::grouped(s), false, "leaving the button red");
        slots::pickGroup(s, st->g, 2);
        Ok(s.inGroup[2] && !s.inGroup[0], "a mark picks out that group alone");
        Eq(s.mark[2], slots::TICK, "which is then the ticked one");
    }
    {
        auto st = stack(3);
        auto s = slots::build(st->g, st->a, 0);
        slots::all(s, st->g);
        s.inGroup[2] = false;
        slots::pickGroup(s, st->g, 0);
        Ok(s.inGroup[2], "picking a group takes all of it");
    }
    {
        auto st = stack(3);
        auto& a = st->a;
        Game& g = st->g;
        auto s = slots::build(g, a, 0);
        slots::all(s, g);
        slots::commit(s, g);
        int id = a[0]->group;
        Ok(id != slots::UNGROUPED && id == a[1]->group && id == a[2]->group,
           "a group of more than one is written back into its armies");
        auto again = slots::build(g, a, 0);
        Eq(static_cast<int>(slots::selected(again).size()), 3,
           "so picking it up again takes the whole group");
        Eq(slots::grouped(again), true, "with the Grp button still green");
        Eq(again.group[0], again.group[2], "and all three in one group in the bar");
        slots::commit(again, g);
        Eq(a[0]->group, id, "picking it up does not renumber it");
        slots::toggle(again, g, 2);
        slots::commit(again, g);
        auto third = slots::build(g, a, 0);
        Eq(static_cast<int>(slots::selected(third).size()), 2,
           "dropping one out leaves the rest grouped");
        Eq(a[2]->group, slots::UNGROUPED, "and the one dropped out is on its own");
        Eq(a[0]->group, id, "while the group keeps its id");
        slots::single(third, g);
        slots::commit(third, g);
        Eq(a[0]->group, slots::UNGROUPED, "and breaking it up clears the grouping");
        Eq(static_cast<int>(slots::selected(slots::build(g, a, 0)).size()), 1,
           "so a click picks up one army again");
    }
    {
        auto st = stack(3);
        auto& a = st->a;
        auto s = slots::build(st->g, a, 0);
        slots::all(s, st->g);
        auto left = slots::keep(s, st->g, {a[0], a[2]});
        Ok(left.has_value(), "the survivors are still a selection");
        if (left) {
            Eq(left->n, 2, "the dead leave the bar");
            Ok(left->inGroup[0] && left->inGroup[1], "the survivors keep moving together");
        }
        Ok(!slots::keep(s, st->g, {}).has_value(), "and a wiped-out stack is no selection at all");
    }
}

static void TestScreenLayout() {
    printf("screen layout\n");
    auto ui = uidata::load(kData);
    Eq(ui.bitmaps[0], "startup.pck", "bitmap 0 is the startup screen");
    Eq(ui.bitmaps[4], "button.pck", "bitmap 4 is the button sheet");
    Eq(ui.bitmaps[78], "movebar7.pck", "there are 79 bitmaps, 0 to 78");
    Eq(static_cast<int>(ui.joins.size()), 36, "JOIN.DAT names 36 dialogs");
    Eq(ui.joins[0].button, 0, "dialog 0 uses button group 0");
    Eq(ui.joins[0].area, 2, "dialog 0 uses area screen 2");
    Eq(ui.joins[9].button, 11, "dialog 9 takes button group 11");
    Eq(ui.joins[12].button, 9, "and dialog 12 takes group 9");
    auto d = uidata::dialog(ui, uidata::MAIN_SCREEN);
    Eq(static_cast<int>(d.controls.size()), 43, "the main screen has 43 controls");
    Eq(static_cast<int>(d.regions.size()), 6, "and 6 regions");
    std::map<int, std::array<int, 4>> want = {{1, {400, 30, 224, 312}},
                                              {2, {16, 30, 360, 360}},
                                              {3, {0, 0, 640, 18}},
                                              {9, {16, 408, 360, 56}}};
    for (auto& r : d.regions) {
        auto w = want.find(r.id);
        if (w != want.end()) {
            Eq(r.x, w->second[0], fmt("region %d x", r.id));
            Eq(r.y, w->second[1], fmt("region %d y", r.id));
            Eq(r.w, w->second[2], fmt("region %d width", r.id));
            Eq(r.h, w->second[3], fmt("region %d height", r.id));
        }
    }
    Eq(ui.shortcutNames[507], "Move All", "UDB.DAT names menu item 507");
    Eq(ui.shortcutNames[535], "End Turn", "and item 535");
    const char* want_sc[4] = {"Search", "Move All", "Heroes", "End Turn"};
    for (int i = 0; i < 4; i++) {
        Eq(ui.shortcutNames[ui.shortcuts[i]], want_sc[i],
           fmt("button %d (control %d) is %s", i, 179 + i, want_sc[i]));
    }
    {
        auto img = pck::decode(kData + "/PICS/MENUBUTT.PCK");
        int w = img->w, h = img->h;
        Eq(w, 320, "MENUBUTT.PCK is 320 wide");
        Eq(h, 197, "and 197 tall: 7 rows 28 apart, plus the last row's border");
        std::set<std::string> cells;
        int n = 0;
        for (auto& [id, item] : ui.shortcutItems) {
            Eq(item.w, 32, fmt("item %d icon width", id));
            Eq(item.h, 29, fmt("item %d icon height", id));
            for (int st = 0; st <= 2; st++) {
                auto src = item.src[st];
                Eq(src.x % 32, 0, fmt("item %d state %d sits on a column", id, st));
                Eq(src.y % 28, 0, fmt("item %d state %d sits on a row", id, st));
                Ok(src.x + 32 <= w && src.y + 29 <= h,
                   fmt("item %d state %d is inside the sheet", id, st));
                std::string key = fmt("%d,%d", src.x, src.y);
                Ok(!cells.count(key), "cell " + key + " is used once");
                cells.insert(key);
                n++;
            }
        }
        Eq(n, 63, "21 items, three cells each");
        auto& end_turn = ui.shortcutItems[535];
        Eq(end_turn.src[uidata::NORMAL].x, 0, "End Turn rests at the sheet's origin");
        Eq(end_turn.src[uidata::NORMAL].y, 0, "at the top of it");
        Eq(end_turn.src[uidata::ACTIVE].x, 32, "and lights up one cell right");
    }
    {
        auto img = pck::decode(kData + "/PICS/ABITS.PCK");
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
            int best = NONE, best_n = 0;
            for (auto& [v, c] : count)
                if (c > best_n) {
                    best = v;
                    best_n = c;
                }
            Ok(best_n > 40, fmt("ring %d has ink", cell));
            if (cell > 0) {
                Ok(!seen.count(best), fmt("ring %d has its own colour", cell));
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
            Ok(ic.sx >= 288, fmt("the %s icon is past the nine rings", ic.name));
            Ok(ic.sx + 40 <= w && ic.sy + 20 <= h, fmt("the %s icon is inside the sheet", ic.name));
            int ink = 0;
            for (int y = ic.sy; y <= ic.sy + 19; y++)
                for (int x = ic.sx; x <= ic.sx + 39; x++)
                    if (px[y * w + x] != bg) ink++;
            Ok(ink > 40, fmt("the %s icon has ink", ic.name));
            int edge = 0;
            for (int y = ic.sy; y <= ic.sy + 19; y++)
                if (px[y * w + ic.sx - 1] != bg) edge++;
            Eq(edge, 0, fmt("the %s icon starts on a clear column", ic.name));
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
                        std::string p = kData + dir + upper(name);
                        if (fileExists(p)) {
                            auto im = pck::decode(p);
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
                           fmt("control %d state %d lies inside %s", c.id, st, name.c_str()));
                        checked++;
                    }
                }
            }
        }
    }
    Ok(checked >= 400, fmt("checked %d source rects", checked));
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
        city.ownerIndex = owner >= 0 ? owner : NONE;
        city.razed = false;
        Ok(scn::cityTileBase(city) == base, fmt("owner %d takes castle block %d", owner, base));
        scn::setCityTiles(map, city);
        Ok(scn::tileAt(map, city.x, city.y) == base, "top-left cell is the block");
        Ok(scn::tileAt(map, city.x + 1, city.y) == base + 1, "top-right is +1");
        Ok(scn::tileAt(map, city.x, city.y + 1) == base + 16, "bottom-left is +16");
        Ok(scn::tileAt(map, city.x + 1, city.y + 1) == base + 17, "bottom-right is +17");
        for (int dx = 0; dx <= 1; dx++)
            for (int dy = 0; dy <= 1; dy++)
                Ok(scn::terrainAt(map, city.x + dx, city.y + dy) == 10,
                   fmt("castle cell %d,%d is city terrain", dx, dy));
    }
    for (int owner = 0; owner <= 7; owner++) {
        city.ownerIndex = NONE;
        city.razed = true;
        city.razedBy = owner;
        Ok(scn::cityTileBase(city) == 0xa0 + 2 * owner,
           fmt("ruins of side %d take block %d", owner, 0xa0 + 2 * owner));
        scn::setCityTiles(map, city);
        for (int dx = 0; dx <= 1; dx++)
            for (int dy = 0; dy <= 1; dy++)
                Ok(scn::terrainAt(map, city.x + dx, city.y + dy) == 11,
                   fmt("ruin cell %d,%d is ruins terrain", dx, dy));
    }
    auto g2p = NewGame("TUTORIA", Seed(12));
    Game& g2 = *g2p;
    City& c2 = g2.map->cities[0];
    for (Side* s : g2.sides) {
        game::setCityOwner(g2, c2, s->index);
        scn::setCityTiles(*g2.map, c2);
    }
    auto mine_list = game::sideCities(g2, *g2.sides[0]);
    City* mine = mine_list.empty() ? nullptr : mine_list[0];
    Ok(mine != nullptr, "the first side holds a city to raze");
    if (mine) {
        int was = mine->ownerIndex;
        game::raze(g2, *g2.sides[0], *mine, {});
        Ok(mine->razedBy == was, "raze remembers who held the city");
        Ok(scn::tileAt(*g2.map, mine->x, mine->y) == 0xa0 + 2 * was,
           "raze restamps the map with that side's ruins");
    }
}

static void TestSites(const std::string& scenario) {
    printf("sites: %s\n", scenario.c_str());
    auto gp = NewGame(scenario, Seed(53));
    Game& g = *gp;
    int rich = 0;
    for (auto& s : g.map->sites) {
        Ok(s.content != site::EMPTY, "no site is left empty");
        if (s.rich) {
            rich++;
            Ok(s.type != site::TEMPLE, "a temple is never rich");
        }
        if (s.content == site::ALLIES) {
            Ok(s.allyType != NONE, "an ally ruin knows what joins");
            Ok(g.types.byId(s.allyType) != nullptr, "and it is a real army type");
        }
        // an allies site keeps its allies' type where the guardian would be,
        // and nothing guards it (66d4:0000, 6536:01ab)
        if (s.content == site::ALLIES)
            Eq(s.guardian, NONE, "nothing guards an allies ruin");
        else if (s.content != site::TEMPLE)
            Ok(s.guardian >= 1 && s.guardian <= 9, "a ruin has a guardian");
    }
    Eq(rich, static_cast<int>(g.map->sites.size()) * 3 / 10, "three in ten sites are rich");
    std::set<int> placed;
    for (auto& s : g.map->sites) {
        if (s.content == site::ITEM) {
            Ok(!placed.count(s.item), "an item is hidden in only one ruin");
            placed.insert(s.item);
        }
    }
    for (auto& it : g.map->items)
        if (placed.count(it.index)) Eq(it.status, 2, "a hidden item is marked hidden");
    std::map<int, Item*> by_index;
    for (auto& it : g.map->items) by_index[it.index] = &it;
    for (auto& s : g.map->sites) {
        if (s.content == site::ITEM)
            Eq(site::itemReserved(*by_index[s.item]), s.rich, "a reserved item needs a rich ruin");
    }
    if (g.map->itemPool) {
        std::set<std::string> names;
        for (auto& p : *g.map->itemPool) names.insert(p.name);
        for (int i = 8; i <= 21; i++)
            if (by_index.count(i))
                Ok(names.count(by_index[i]->name),
                   "item " + std::to_string(i) + " came from the pool");
        for (int i = 0; i <= 7; i++)
            if (by_index.count(i))
                Eq(by_index[i]->type, rules::ITEM_STANDARD, "records 0-7 are standards");
    }
    Site* ruin = nullptr;
    for (auto& s : g.map->sites)
        if (!ruin && s.content == site::GOLD) ruin = &s;
    if (ruin) {
        Side* side = g.sides[0];
        Army* grunt = AddArmy(g, ruin->x, ruin->y, side->index, 11, 3, 10, 10);
        auto r = site::search(g, {grunt}, ruin->x, ruin->y);
        Eq(r->kind, "no hero", "a stack without a hero cannot search a ruin");
        Ok(!ruin->searched, "and the ruin is not used up");
        Army* h = AddArmy(g, ruin->x, ruin->y, side->index, armytype::HERO, 9, 14, 14);
        int gold0 = side->gold;
        r = site::search(g, {h, grunt}, ruin->x, ruin->y);
        Ok(r->kind == "gold" || r->kind == "killed", "a hero gets a result: " + r->kind);
        if (r->kind == "gold") {
            Ok(r->gold >= 503 && r->gold <= 4000,
               "the gold is in range: " + std::to_string(r->gold));
            Eq(side->gold, gold0 + r->gold, "the gold was paid");
            Eq(h->experience, 3, "searching is worth 3 experience");
        }
        if (r->kind == "gold") {
            Ok(ruin->searched, "a searched ruin is used up");
            Ok(site::search(g, {h}, ruin->x, ruin->y) == nullptr, "and cannot be searched again");
        } else {
            Ok(!ruin->searched, "a hero who dies leaves the ruin to be searched again");
        }
    }
    Site* temple = nullptr;
    for (auto& s : g.map->sites)
        if (!temple && s.content == site::TEMPLE && s.templeIndex == 0) temple = &s;
    if (temple) {
        Side* side = g.sides[0];
        Army a;
        a.x = temple->x;
        a.y = temple->y;
        a.owner = side->index;
        a.type = 11;
        a.strength = 3;
        a.moves = 0;
        Ok(site::search(g, {&a}, temple->x, temple->y) == nullptr,
           "searching needs a move left (6536:0000)");
        a.moves = 10;
        auto r = site::search(g, {&a}, temple->x, temple->y, true);
        Eq(r->kind, "temple", "with no hero a temple blesses at once, a human's too");
        Eq(r->kind, "temple", "a temple blesses");
        Eq(r->blessed, 1, "one army was blessed");
        Eq(a.strength, 4, "the blessing adds a strength");
        Eq(site::search(g, {&a}, temple->x, temple->y)->blessed, 0,
           "the same temple does not bless twice");
        a.strength = 9;
        a.blessings.clear();
        site::search(g, {&a}, temple->x, temple->y);
        Eq(a.strength, 9, "a blessing never passes 9");
    }
    Army strong, weak;
    strong.strength = 9;
    weak.strength = 1;
    int survived = 0, died = 0;
    for (int i = 0; i < 400; i++) {
        if (site::survivesGuardian(g, strong, 3)) survived++;
        if (!site::survivesGuardian(g, weak, 8)) died++;
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
    Side* side = game::begin(g);
    City* city = game::sideCities(g, *side)[0];
    Army* sailor = AddArmy(g, city->x, city->y, side->index, 2, 3, 0, 14);
    sailor->atSea = true;
    for (size_t i = 0; i < g.sides.size(); i++) EndTurn(g);
    Ok(!sailor->atSea, "an army at sea in a city is ashore when its turn begins");
    Eq(sailor->moves, sailor->maxMoves, "with its own movement, not the 20 at sea");

    int wx = NONE, wy = NONE;
    for (int y = 0; y < g.map->height && wx == NONE; y++) {
        for (int x = 0; x < g.map->width && wx == NONE; x++) {
            if (scn::terrainAt(*g.map, x, y) == movement::WATER && !scn::isCrossing(*g.map, x, y) &&
                game::armiesAt(g, x, y).empty()) {
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
    boat->atSea = true;
    Army* hero = AddArmy(g, wx, wy, side->index, armytype::HERO, 5, 14, 14);
    Eq(game::sinkStranded(g, *side, wx, wy), 1, "on the water the army without a boat sinks");
    Ok(!at(walker), "it is gone");
    Ok(at(boat) && at(hero), "one at sea, and a hero, do not sink");
    int flyer = NONE;
    for (auto& t : g.types.all)
        if (flyer == NONE && t.flies) flyer = t.id;
    Army* bird = AddArmy(g, wx, wy, side->index, flyer, 3, 14, 14);
    Army* walker2 = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    Eq(game::sinkStranded(g, *side), 1, "as the turn opens every tile is looked at");
    Ok(at(bird) && !at(walker2), "and only the army that can neither fly nor sail goes");
    Item wings;
    wings.type = rules::ITEM_FLIGHT;
    hero->items.push_back(&wings);
    Army* carried = AddArmy(g, wx, wy, side->index, 2, 3, 14, 14);
    Eq(game::sinkStranded(g, *side, wx, wy), 0, "a hero's flight item carries the whole stack");
    hero->items.clear();
    Eq(game::sinkStranded(g, *side, wx, wy), 1, "without it the walker drowns");
    Ok(!at(carried), "");
    Army* ashore = AddArmy(g, city->x, city->y, side->index, 2, 3, 14, 14);
    Eq(game::sinkStranded(g, *side, city->x, city->y), 0, "nothing sinks on land");
    Ok(at(ashore), "");
}

// A step is taken while the walk's cost so far fits in the moves left
// (1555:18be), with no floor; an attack is fought from beside the tile and
// needs one move, then costs the tile's movement, 2 at least (67cc:0000).
static void TestLastMove() {
    printf("the last move point\n");
    auto gp = NewGame("ERYTHEA", Seed(11));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* army = game::sideArmies(g, *side)[0];
    int rx = NONE, ry = NONE;
    for (int y = 1; y < g.map->height - 1 && rx == NONE; y++) {
        for (int x = 1; x < g.map->width - 1 && rx == NONE; x++) {
            if (scn::roadAt(*g.map, x, y) % 0x20 == 0 || scn::roadAt(*g.map, x + 1, y) % 0x20 == 0)
                continue;
            if (game::cityAt(g, x, y) || game::cityAt(g, x + 1, y)) continue;
            if (!game::armiesAt(g, x, y).empty() || !game::armiesAt(g, x + 1, y).empty()) continue;
            rx = x;
            ry = y;
        }
    }
    Ok(rx != NONE, "found two road tiles side by side");
    if (rx != NONE) {
        army->x = rx;
        army->y = ry;
        army->atSea = false;
        army->moves = 1;
        movement::invalidate(g);
        auto r = movement::moveTo(g, {army}, rx + 1, ry);
        Eq(r.steps, 1, "one move left still buys a step along a road");
        Eq(army->x, rx + 1, "and the army is there");
        Eq(army->moves, 0, "with nothing left");
    }
    City* enemy = nullptr;
    for (auto& c : g.map->cities)
        if (!enemy && c.ownerIndex != side->index) enemy = &c;
    Army* a = AddArmy(g, enemy->x, enemy->y - 1, side->index, 11, 3, 1, 20);
    game::chargeAttack(g, {a}, enemy->x, enemy->y);
    Eq(a->moves, 0, "an attack takes the last move");
    a->moves = 10;
    game::chargeAttack(g, {a}, enemy->x, enemy->y);
    Eq(a->moves, 8, "a city costs 1 to enter but 2 to attack");
}

static void TestDiplomacy() {
    printf("diplomacy\n");
    namespace d = diplomacy;
    {
        // attack_tile asks first (67cc:00b2), and "attack anyway" goes to war
        // (67cc:20a8)
        auto gw = NewGame("ERYTHEA", SeedWith(65, {{"diplomacy", 1}}));
        City* city = &gw->map->cities[1];
        Ok(d::mustDeclare(*gw, 0, 1, nullptr), "attacking armies at peace asks first");
        Ok(!d::mustDeclare(*gw, 0, NONE, city), "attacking a neutral never asks");
        gw->diplomacy.state[0 * 8 + 1] = d::INTERMEDIATE;
        Ok(!d::mustDeclare(*gw, 0, 1, nullptr), "armies of a side merely uneasy are fair game");
        Ok(d::mustDeclare(*gw, 0, 1, city), "but its cities need war");
        Ok(!d::mustDeclare(*gw, 0, 1, &gw->map->cities[0]),
           "except city 0, which the original's > 0 test misses");
        gw->diplomacy.state[0 * 8 + 1] = d::PEACE;
        int before = gw->sides[0]->diploScore;
        d::attackAnyway(*gw, 0, 1);
        int cost = gw->sides[0]->diploScore - before;
        Ok(cost >= 101 && cost <= 200, fmt("attacking at peace costs 1d100+100 score: %d", cost));
        Eq(d::state(*gw, 0, 1), d::WAR, "and it is war");
        Eq(d::state(*gw, 1, 0), d::WAR, "both ways");
        Eq(d::proposal(*gw, 1, 0), d::WAR, "the proposals too");
        Ok(!d::mustDeclare(*gw, 0, 1, city), "so the attack goes ahead");
        auto off_opts = NewGame("ERYTHEA", Seed(65));
        Ok(!d::mustDeclare(*off_opts, 0, 1, city), "with Diplomacy off nothing asks");
    }
    auto off = NewGame("ERYTHEA", Seed(61));
    Eq(off->map->options.diplomacy, 0, "the shipped scenarios have diplomacy off");
    Eq(d::state(*off, 0, 1), d::WAR, "with the option off every pair starts at war");
    Ok(d::mayAttack(*off, 0, 1), "and may attack freely");
    auto gp = NewGame("ERYTHEA", SeedWith(61, {{"diplomacy", 1}}));
    Game& g = *gp;
    Eq(d::state(g, 0, 1), d::PEACE, "with the option on every pair starts at peace");
    Ok(!d::mayAttack(g, 0, 1), "a side at peace may not attack");
    Ok(d::mayAttack(g, 0, NONE), "neutrals may always be attacked");
    Eq(d::state(g, 0, 0), d::PEACE, "a side is at peace with itself");
    d::propose(g, 0, 1, d::WAR);
    auto msgs = d::apply(g, *g.sides[0]);
    Eq(d::state(g, 0, 1), d::WAR, "declaring war takes effect at once");
    Eq(d::state(g, 1, 0), d::WAR, "and binds the other side too");
    Ok(!msgs.empty() && contains(msgs[0], "War declared"), "and is announced");
    Eq(d::proposal(g, 1, 0), d::WAR, "the other side's proposal is raised to match");
    auto g2p = NewGame("ERYTHEA", SeedWith(62, {{"diplomacy", 1}}));
    Game& g2 = *g2p;
    d::propose(g2, 0, 1, d::WAR);
    d::apply(g2, *g2.sides[0]);
    g2.diplomacy.proposal[1 * 8 + 0] = NONE;  // forget their matching proposal
    d::propose(g2, 0, 1, d::PEACE);
    d::apply(g2, *g2.sides[0]);
    Eq(d::state(g2, 0, 1), d::WAR, "one-sided peace does not land");
    d::propose(g2, 0, 1, d::PEACE);
    d::propose(g2, 1, 0, d::PEACE);
    auto m2 = d::apply(g2, *g2.sides[0]);
    Eq(d::state(g2, 0, 1), d::PEACE, "matched proposals make peace");
    Ok(!m2.empty() && contains(m2[0], "Peace negotiated"), "and it is announced");
    {
        auto g3p = NewGame("ERYTHEA", SeedWith(63, {{"diplomacy", 1}}));
        Game& g3 = *g3p;
        d::propose(g3, 0, 1, d::WAR);
        d::apply(g3, *g3.sides[0]);
        d::propose(g3, 0, 1, d::PEACE);
        int before = g3.sides[0]->diploScore;
        d::scoreUpdate(g3, *g3.sides[0]);
        int gain = g3.sides[0]->diploScore - before;
        Ok(gain >= 11 && gain <= 20,
           "peace offered from war is worth 1d10+10: " + std::to_string(gain));
        d::propose(g3, 1, 0, d::PEACE);
        before = g3.sides[0]->diploScore;
        d::scoreUpdate(g3, *g3.sides[0]);
        Eq(g3.sides[0]->diploScore, before, "not once the other side offers it too");
    }
    auto g3p = NewGame("ERYTHEA", Seed(63));
    Game& g3 = *g3p;
    for (size_t i = 0; i < g3.sides.size(); i++)
        g3.sides[i]->diploScore = static_cast<int>(i + 1) * 10;
    auto r = d::ratings(g3);
    Eq(r[g3.sides[0]->index], "Statesman", "the lowest score is the Statesman");
    Eq(r[g3.sides.back()->index], "Running Dog", "the highest is the Running Dog");
    const auto& ranks = d::ratingRanks(2);
    Eq(static_cast<int>(ranks.size()), 2, "two sides take two titles");
    Eq(std::string(d::TITLES[ranks[0] - 1]), "Statesman", "best of two");
    Eq(std::string(d::TITLES[ranks[1] - 1]), "Running Dog", "worst of two");
    auto g4p = NewGame("ERYTHEA", SeedWith(64, {{"diplomacy", 1}}));
    Game& g4 = *g4p;
    Side* side = game::begin(g4);
    Side* victim = nullptr;
    for (Side* s : g4.sides)
        if (s->index != side->index) victim = s;
    City* city = victim->capital;
    Army* army = AddArmy(g4, city->x, city->y - 1, side->index, 11, 3, 20, 20);
    army->upkeep = 1;
    auto path = movement::findPath(g4, {army}, army->x, army->y, city->x, city->y);
    if (path && !path->empty()) {
        auto r2 = movement::walk(g4, {army}, *path);
        Eq(r2.stopped, "at peace", "a stack at peace cannot walk into their city");
        Eq(army->x, city->x, "and has not moved into it");
    }

    // but a stack of a side at peace in the open is passed over: the walk may
    // not stop on it, and goes on beyond it (1a8b:07f9)
    Side* other = victim;
    int map_width = g4.map->width;
    auto open = [&](int x, int y) {
        int t = scn::terrainAt(*g4.map, x, y);
        return t != movement::WATER && t != movement::SHORE && t != movement::CITY &&
               t != movement::BRIDGE && !g4.map->cityTile[y * map_width + x] &&
               game::armiesAt(g4, x, y).empty();
    };
    int sx = NONE, sy = NONE;
    for (int y = 10; y <= g4.map->height - 10; y++)
        for (int x = 10; x <= map_width - 10; x++)
            if (sx == NONE && open(x, y) && open(x + 1, y) && open(x + 2, y)) {
                sx = x;
                sy = y;
            }
    Army* walker = AddArmy(g4, sx, sy, side->index, 11, 3, 20, 20);
    Army* blocker = AddArmy(g4, sx + 1, sy, other->index, 11, 3, 20, 20);
    walker->upkeep = blocker->upkeep = 1;
    movement::Path steps = {{sx + 1, sy, 2}, {sx + 2, sy, 2}};
    auto r3 = movement::walk(g4, {walker}, steps);
    Eq(walker->x, sx + 2, "a stack at peace in the open is passed over");
    Eq(walker->moves, 16, "paying for the step past it");
    Eq(r3.stopped, "arrived", "and the walk arrives");
    walker->x = sx;
    walker->moves = 20;
    g4.diplomacy.state[side->index * 8 + other->index] = d::WAR;
    auto r4 = movement::walk(g4, {walker}, steps);
    Eq(r4.stopped, "attack", "a stack at war is attacked");
    Eq(walker->x, sx, "from where the walker stands");

    // ruins are open ground: a fight there is for no city, and takes none
    City* ruin = nullptr;
    for (auto& c : g4.map->cities)
        if (!ruin && c.ownerIndex != NONE && c.ownerIndex != side->index) ruin = &c;
    game::raze(g4, g4.map->sides[ruin->ownerIndex], *ruin, {}, true);
    std::vector<Army*> on_ruin;
    for (Army* a : g4.armies)
        if (a->x != NONE && a->x >= ruin->x && a->x <= ruin->x + 1 && a->y >= ruin->y &&
            a->y <= ruin->y + 1)
            on_ruin.push_back(a);
    for (Army* a : on_ruin) g4.remove(a);
    g4.diplomacy.state[side->index * 8 + other->index] = d::WAR;
    AddArmy(g4, ruin->x, ruin->y, other->index, 11, 1, 20, 20);
    AddArmy(g4, ruin->x + 1, ruin->y + 1, other->index, 11, 1, 20, 20);
    std::vector<Army*> strike;
    for (int i = 0; i < 8; i++)
        strike.push_back(AddArmy(g4, ruin->x - 1, ruin->y, side->index, 11, 9, 20, 20));
    auto fight = game::resolveAttack(g4, strike, ruin->x, ruin->y);
    Eq(fight.lines.city, static_cast<City*>(nullptr), "a fight on ruins is not for a city");
    Eq(static_cast<int>(fight.lines.defenders.size()), 1, "and only the tile fought for defends");
    Eq(fight.captured, static_cast<City*>(nullptr), "nothing is captured");
    Eq(ruin->ownerIndex, NONE, "the ruins belong to nobody");
}

static std::shared_ptr<Quest> CityQuest(int type, Army* h, City* target) {
    auto q = std::make_shared<Quest>();
    q->type = type;
    q->hero = h;
    q->targetKind = "city";
    q->city = target;
    q->done = 0;
    return q;
}

static void TestQuests() {
    printf("quests\n");
    auto off = NewGame("ERYTHEA", Seed(71));
    Side* side_off = game::begin(*off);
    Army hero_off;
    hero_off.x = 10;
    hero_off.y = 10;
    hero_off.owner = side_off->index;
    hero_off.type = armytype::HERO;
    hero_off.strength = 5;
    Ok(quest::assign(*off, *side_off, &hero_off) == nullptr, "no quests when the option is off");
    auto gp = NewGame("ERYTHEA", SeedWith(71, {{"quests", 1}}));
    Game& g = *gp;
    Side* side = game::begin(g);
    Army* h = AddArmy(g, side->capital->x, side->capital->y, side->index, armytype::HERO, 5);
    auto quest1 = quest::assign(g, *side, h);
    Ok(quest1 != nullptr, "a quest is assigned");
    if (quest1) {
        Ok(quest1->type >= 0 && quest1->type <= 6, "with a known type");
        Ok(quest1->hasTarget(), "and a target");
        Ok(!quest::describe(*quest1).empty(), "a quest describes itself");
    }
    Ok(quest::assign(g, *side, h) == nullptr, "only one quest at a time");
    std::map<int, int> counts;
    for (int t : quest::TYPE_TABLE) counts[t]++;
    Eq(counts[0], 1, "type 0 has one slot");
    Eq(counts[4], 2, "type 4 has two");
    Eq(counts[6], 2, "type 6 has two");
    Eq(static_cast<int>(quest::TYPE_TABLE.size()), 10, "the table is rolled with 1d10");
    auto city_at_lua_index = [&](int i) { return &g.map->cities[i - 1]; };  // the Lua's cities[i]
    quest::Event ev;
    side->quest = CityQuest(quest::OCCUPY, h, city_at_lua_index(2));
    side->gold = 5000;
    ev = quest::Event();
    ev.city = city_at_lua_index(2);
    ev.stack = {h};
    auto r = quest::event(g, *side, "occupy", ev);
    Ok(r && r->failed.empty(), "occupying the target completes the quest");
    Ok(!side->quest, "and the quest is cleared");
    Eq(h->experience, quest::EXPERIENCE, "the hero gains 10 experience");
    Ok(r && r->reward, "a reward was given");
    bool was_computer = side->computer;
    side->computer = false;
    side->questNews.reset();
    side->quest = CityQuest(quest::OCCUPY, h, city_at_lua_index(2));
    r = quest::event(g, *side, "occupy", ev);
    Ok(side->questNews == r, "the human keeps the news to be shown");
    side->computer = true;
    side->questNews.reset();
    side->quest = CityQuest(quest::OCCUPY, h, city_at_lua_index(2));
    quest::event(g, *side, "occupy", ev);
    Ok(!side->questNews, "the computer's quest ends unannounced");
    side->computer = was_computer;
    side->questNews.reset();
    h->experience = 0;
    side->quest = CityQuest(quest::OCCUPY, h, city_at_lua_index(3));
    ev = quest::Event();
    ev.city = city_at_lua_index(3);
    r = quest::event(g, *side, "occupy", ev);
    Ok(r && !r->failed.empty(), "occupying without the hero fails the quest");
    if (r) Eq(r->why, 0x22, "said as 'Alas! The city was not taken by thy hero!'");
    Eq(h->experience, 0, "and pays no experience");
    City* c5 = city_at_lua_index(5);
    int owner5 = c5->ownerIndex;
    bool razed5 = c5->razed;
    side->quest = CityQuest(quest::RAZE, h, c5);
    c5->razed = true;
    r = quest::event(g, *side, "turn");
    Ok(r && !r->failed.empty() && r->why == 0x21, "a raze quest's city razed by another fails it");
    c5->razed = false;
    c5->ownerIndex = side->index;
    side->quest = CityQuest(quest::RAZE, h, c5);
    r = quest::event(g, *side, "turn");
    Ok(r && !r->failed.empty() && r->why == 0x2a, "one taken and kept fails it too");
    c5->razed = razed5;
    c5->ownerIndex = owner5;
    side->quest = CityQuest(quest::OCCUPY, h, city_at_lua_index(4));
    ev = quest::Event();
    ev.city = city_at_lua_index(4);
    ev.stack = {h};
    r = quest::event(g, *side, "raze", ev);
    Ok(r && !r->failed.empty(), "razing a city you were to keep fails the quest");
    Side* victim = g.sides.back();
    auto slaughter = [&](int required) {
        auto q = std::make_shared<Quest>();
        q->type = quest::SLAUGHTER;
        q->hero = h;
        q->targetKind = "side";
        q->side = victim;
        q->required = required;
        return q;
    };
    side->quest = slaughter(3);
    Army d1, d2;
    d1.owner = victim->index;
    d2.owner = victim->index;
    ev = quest::Event();
    ev.stack = {h};
    ev.killed = {&d1, &d2};
    Ok(quest::event(g, *side, "battle", ev) == nullptr, "two of three is not enough");
    Eq(side->quest->done, 2, "the count rises");
    r = quest::event(g, *side, "battle", ev);
    Ok(r && r->failed.empty(), "the third kill completes it");
    side->quest = slaughter(2);
    ev.stack = {};
    quest::event(g, *side, "battle", ev);
    Eq(side->quest->done, 0, "kills away from the hero do not count");
    {
        auto q = std::make_shared<Quest>();
        q->type = quest::PILLAGE_GOLD;
        q->hero = h;
        q->targetKind = "none";
        q->required = 100;
        side->quest = q;
    }
    ev = quest::Event();
    ev.stack = {h};
    ev.gold = 60;
    quest::event(g, *side, "pillage", ev);
    Eq(side->quest->done, 60, "pillaged gold counts");
    r = quest::event(g, *side, "pillage", ev);
    Ok(r && r->failed.empty(), "reaching the total completes it");
    Item item;
    item.index = 99;
    item.name = "Testsword";
    item.type = rules::ITEM_BATTLE;
    item.value = 1;
    item.status = 3;
    h->items = {&item};
    {
        auto q = std::make_shared<Quest>();
        q->type = quest::RETRIEVE_ITEM;
        q->hero = h;
        q->targetKind = "item";
        q->item = &item;
        side->quest = q;
    }
    ev = quest::Event();
    ev.hero = h;
    r = quest::event(g, *side, "item", ev);
    Ok(r && r->failed.empty(), "carrying the item completes the quest");
    Ok(std::find(h->items.begin(), h->items.end(), &item) == h->items.end(),
       "the quest item is taken away (the reward may add another)");
    Eq(item.status, 0, "it leaves play");
    Army prey;
    prey.type = armytype::HERO;
    prey.owner = victim->index;
    {
        auto q = std::make_shared<Quest>();
        q->type = quest::SLAY_HERO;
        q->hero = h;
        q->targetKind = "army";
        q->army = &prey;
        side->quest = q;
    }
    ev = quest::Event();
    ev.killed = {&prey};
    Ok(quest::event(g, *side, "battle", ev) == nullptr,
       "someone else killing the quarry does not count");
    ev.stack = {h};
    r = quest::event(g, *side, "battle", ev);
    Ok(r && r->failed.empty(), "the hero killing the quarry completes it");
    Army ghost;
    ghost.type = armytype::HERO;
    {
        auto q = std::make_shared<Quest>();
        q->type = quest::PILLAGE_GOLD;
        q->hero = &ghost;
        q->targetKind = "none";
        q->required = 1;
        side->quest = q;
    }
    r = quest::event(g, *side, "turn");
    Ok(r && !r->failed.empty(), "a quest with a dead hero is abandoned");
    side->gold = 50;
    Quest with_hero;
    with_hero.hero = h;
    auto reward = quest::reward(g, *side, &with_hero);
    Eq(reward->kind, "gold", "a side with no gold is given gold");
    Ok(reward->gold >= 1002 && reward->gold <= 3000,
       "2d1000+1000: " + std::to_string(reward->gold));
    g.turn = 20;
    side->gold = 5000;
    reward = quest::reward(g, *side, &with_hero);
    Eq(reward->kind, "allies", "a small side past turn 15 is given allies");
    Ok(!reward->armies.empty() && reward->armies.size() <= 8, "1d3+5 allies arrive");
}

static void TestEndGame() {
    printf("end of the game\n");
    auto gp = NewGame("ERYTHEA", Seed(81));
    Game& g = *gp;
    for (Side* s : g.sides) s->computer = true;
    Side* winner = g.sides[0];
    for (auto& c : g.map->cities) c.ownerIndex = c.ownerIndex != NONE ? winner->index : NONE;
    for (size_t i = 1; i < g.sides.size(); i++) g.sides[i]->alive = false;
    auto r = game::checkEnd(g);
    Ok(r.triumph, "the last side standing has triumphed");
    Ok(!r.over, "and the game goes on, to be looked over");
    Eq(r.winner, winner, "the last side standing wins");
    Ok(!winner->computer, "and is switched to human control");
    Ok(g.won, "the game-won flag is set");
    Ok(!r.noHumans, "a game that never had a human does not say the last one fell");
    auto g2 = NewGame("ERYTHEA", Seed(82));
    for (auto& c : g2->map->cities) c.ownerIndex = NONE;
    Ok(!game::checkEnd(*g2).over, "sides still in the game are players, cities or not");
    auto r2 = game::endRound(*g2);
    Ok(r2.over, "with no cities owned the game is over at the round's end");
    Eq(r2.winner, static_cast<Side*>(nullptr), "and nobody won");
    Ok(contains(r2.message, "No more players"), "with the right message");
    Eq(r2.fallen.size(), g2->sides.size(), "every side fell");

    // the round's end puts a side with no city out (8065:18ab)
    auto g6p = NewGame("ERYTHEA", SeedWith(86, {{"diplomacy", 1}}));
    Game& g6 = *g6p;
    for (size_t i = 0; i < g6.sides.size(); i++) g6.sides[i]->computer = i > 0;
    game::begin(g6);
    Side* doomed = g6.sides[1];
    Side* other = g6.sides[2];
    for (City* c : game::sideCities(g6, *doomed)) c->ownerIndex = other->index;
    g6.diplomacy.state[doomed->index * 8 + other->index] = diplomacy::WAR;
    g6.diplomacy.state[other->index * 8 + doomed->index] = diplomacy::WAR;
    Ok(!game::sideArmies(g6, *doomed).empty(), "the doomed side still has armies");
    EndTurn(g6);
    Eq(g6.side, other, "a computer side with no city has no turn");
    Ok(doomed->alive, "but it is not out before the round's end");
    while (g6.turn == 1) EndTurn(g6);
    Ok(!doomed->alive, "the round's end puts it out");
    Eq(static_cast<int>(game::sideArmies(g6, *doomed).size()), 0, "with all its armies");
    Eq(doomed->gold, 0, "and no gold");
    Eq(diplomacy::state(g6, other->index, doomed->index), diplomacy::INTERMEDIATE,
       "others stand uneasy with it");
    Eq(diplomacy::proposal(g6, doomed->index, other->index), diplomacy::INTERMEDIATE,
       "both ways, proposals too");
    const Fallen* f = g6.ending.fallen.empty() ? nullptr : &g6.ending.fallen[0];
    Ok(f && f->side == doomed, "its fall is told");
    Ok(f && f->line >= 0 && f->line < game::FALLEN_LINES, "in a line of group 11");
    Ok(f && f->boxed, "in a box while a human plays");

    // a human left with no city still plays its turn until the round's end
    auto g7p = NewGame("ERYTHEA", Seed(87));
    Game& g7 = *g7p;
    for (size_t i = 0; i < g7.sides.size(); i++) g7.sides[i]->computer = i + 1 < g7.sides.size();
    game::begin(g7);
    Side* me7 = g7.sides.back();
    for (City* c : game::sideCities(g7, *me7)) c->ownerIndex = g7.sides[0]->index;
    for (size_t i = 1; i < g7.sides.size(); i++) EndTurn(g7);
    Eq(g7.side, me7, "the human's turn comes round without a city");
    Ok(me7->alive, "and it is still in the game");
    EndTurn(g7);
    Ok(!me7->alive, "the round's end puts it out");
    Ok(me7->computer, "a fallen human is the computer's from then on");
    Ok(g7.ending.noHumans, "the last human's fall is said");
    Ok(g7.noHumansSaid, "once");
    for (size_t i = 0; i < g7.sides.size(); i++) EndTurn(g7);
    Ok(!g7.ending.noHumans, "and only once");
    auto g3p = NewGame("ERYTHEA", Seed(83));
    Game& g3 = *g3p;
    for (Side* s : g3.sides) s->computer = false;
    Side* me = g3.sides[0];
    for (size_t i = 1; i < g3.sides.size(); i++) g3.sides[i]->alive = false;
    int standing = static_cast<int>(g3.map->cities.size()), given = 0;
    for (auto& c : g3.map->cities) c.ownerIndex = NONE;
    for (auto& c : g3.map->cities)
        if (given < standing / 2) {
            c.ownerIndex = me->index;
            given++;
        }
    Ok(!game::checkEnd(g3).over, "exactly half is not enough");
    for (auto& c : g3.map->cities)
        if (c.ownerIndex == NONE) {
            c.ownerIndex = me->index;
            break;
        }
    auto r3 = game::checkEnd(g3);
    Ok(r3.won, "more than half wins");
    Ok(!r3.over, "and the game goes on, to be looked over");
    Eq(r3.winner, me, "and names the winner");
    Ok(!game::checkEnd(g3).won, "which is said once");
    auto g4p = NewGame("ERYTHEA", Seed(84));
    Game& g4 = *g4p;
    Side* human = g4.sides[0];
    human->computer = false;
    for (size_t i = 1; i < g4.sides.size(); i++) g4.sides[i]->computer = true;
    for (auto& c : g4.map->cities) c.ownerIndex = NONE;
    Side* rival = g4.sides[1];
    int n = static_cast<int>(g4.map->cities.size());
    for (int i = 0; i < n; i++) {
        City& c = g4.map->cities[i];
        if (i + 1 <= n * 3 / 4)
            c.ownerIndex = human->index;
        else if (i + 1 == n)
            c.ownerIndex = rival->index;
    }
    auto r4 = game::checkEnd(g4);
    Ok(r4.surrender, "a dominant human is offered surrender");
    Ok(!r4.over, "but the game is not over");
    Ok(g4.surrenderOffered, "and the flag is set");
    Ok(!game::checkEnd(g4).surrender, "the offer is made once");
    g4.ending = Ending{};
    Ok(game::surrenderPending(g4),
       "the offer waits through the computer turns that clear the round's finding");
    g4.surrenderShown = true;
    Ok(!game::surrenderPending(g4), "and is put to the player once");
    g4.surrenderShown = false;
    auto g5p = NewGame("ERYTHEA", Seed(85));
    Game& g5 = *g5p;
    Side* side5 = g5.sides[0];
    for (size_t i = 1; i < g5.sides.size(); i++) g5.sides[i]->alive = false;
    for (Side* s : g5.sides) s->computer = false;
    for (auto& c : g5.map->cities) c.ownerIndex = NONE;
    g5.map->cities[0].ownerIndex = side5->index;
    for (size_t i = 1; i < g5.map->cities.size(); i++) g5.map->cities[i].razed = true;
    Ok(game::checkEnd(g5).won, "one city among ruins is still more than half");
    game::acceptSurrender(g4, *human);
    Ok(g4.won, "surrender accepted wins the game");
    Ok(!rival->alive, "and the computer sides are out");
}

// ------------------------------------------------------------ saving a game

static void TestSave() {
    printf("save and load\n");
    auto gp = NewGame("ERYTHEA", SeedWith(91, {{"quests", 1}, {"diplomacy", 1}}));
    Game& g = *gp;
    Side* side = game::begin(g);
    while (side && g.turn <= 8) {
        PlayAi(g, *side);
        side = EndTurn(g);
    }
    Army* h = nullptr;
    for (Army* a : g.armies)
        if (a->type == armytype::HERO) h = a;
    Ok(h != nullptr, "the game has a hero to save");
    if (!h) return;
    Side* owner = g.map->side(h->owner);
    owner->quest = CityQuest(quest::OCCUPY, h, &g.map->cities[4]);
    h->items = {&g.map->items[0]};
    g.map->items[0].status = 3;
    g.armies[0]->group = 4;
    g.armies[1]->group = 4;
    g.armies[0]->fortified = true;
    std::string text = save::encode(g);
    Ok(text.size() > 1000, "the save has content");
    auto h2p = save::decode(text, kData);
    Game& h2 = *h2p;
    Eq(h2.turn, g.turn, "the turn survives");
    Eq(h2.current, g.current, "whose turn it is survives");
    Eq(h2.armies.size(), g.armies.size(), "every army survives");
    Eq(h2.rng.state, g.rng.state, "the dice carry on where they left off");
    for (size_t i = 0; i < g.armies.size() && i < h2.armies.size(); i++) {
        Army* a = g.armies[i];
        Army* b = h2.armies[i];
        std::string n = std::to_string(i);
        Eq(b->x, a->x, "army " + n + " keeps its place");
        Eq(b->type, a->type, "army " + n + " keeps its type");
        Eq(b->strength, a->strength, "army " + n + " keeps its strength");
        Eq(b->moves, a->moves, "army " + n + " keeps its movement");
        Eq(b->owner, a->owner, "army " + n + " keeps its owner");
        Eq(b->group, a->group, "army " + n + " keeps the group it moves with");
        Eq(b->fortified, a->fortified, "army " + n + " stays dug in");
    }
    for (size_t i = 0; i < g.map->cities.size(); i++) {
        const City& c = g.map->cities[i];
        const City& d = h2.map->cities[i];
        std::string n = std::to_string(i);
        Eq(d.ownerIndex, c.ownerIndex, "city " + n + " keeps its owner");
        Eq(d.producing, c.producing, "city " + n + " keeps its production");
        Eq(d.slots.size(), c.slots.size(), "city " + n + " keeps its slots");
    }
    for (size_t i = 0; i < g.sides.size(); i++) {
        Side* s = g.sides[i];
        Eq(h2.sides[i]->gold, s->gold, s->name + " keeps its gold");
        Eq(h2.sides[i]->alive, s->alive, s->name + " keeps its standing");
        Eq(h2.sides[i]->diploScore, s->diploScore, s->name + " keeps its score");
    }
    for (size_t i = 0; i < g.map->sites.size(); i++) {
        const Site& s = g.map->sites[i];
        const Site& t = h2.map->sites[i];
        std::string n = std::to_string(i);
        Eq(t.content, s.content, "site " + n + " keeps its contents");
        Eq(t.searched, s.searched, "site " + n + " remembers being searched");
        Eq(t.rich, s.rich, "site " + n + " keeps its rich flag");
    }
    Side* owner2 = h2.map->side(owner->index);
    Ok(owner2->quest != nullptr, "the quest survives");
    if (owner2->quest) {
        Eq(owner2->quest->type, quest::OCCUPY, "with its type");
        Eq(owner2->quest->city ? owner2->quest->city->index : NONE, g.map->cities[4].index,
           "and its target city");
        Ok(owner2->quest->hero != nullptr, "and its hero");
        if (owner2->quest->hero) Eq(owner2->quest->hero->type, armytype::HERO, "which is a hero");
    }
    bool carried = false;
    for (Army* a : h2.armies)
        if (!a->items.empty()) carried = true;
    Ok(carried, "carried items survive");
    Eq(diplomacy::state(h2, 0, 1), diplomacy::state(g, 0, 1), "the diplomatic state survives");
    Side* side2 = h2.sides[h2.current];
    int before = h2.turn;
    for (size_t i = 0; i < h2.sides.size() * 2; i++) {
        if (!side2) break;
        PlayAi(h2, *side2);
        side2 = EndTurn(h2);
    }
    Ok(h2.turn > before, "the reloaded game plays on");
    // a file round trip
    std::string path = (std::filesystem::temp_directory_path() / "w2-test-save.json").string();
    writeFile(path, text);
    auto h3 = save::decode(mustRead(path), kData);
    Eq(h3->armies.size(), g.armies.size(), "a save written to a file reads back");
    removeFile(path);
}

// ------------------------------------------------------------- the hidden map

static void TestHiddenMap() {
    printf("hidden map\n");
    auto off = NewGame("ERYTHEA", Seed(101));
    Ok(game::seen(*off, 0, 5, 5), "with the option off every tile is seen");
    Eq(game::reveal(*off, 0, 5, 5, false), 0, "and nothing is revealed");
    auto gp = NewGame("ERYTHEA", SeedWith(101, {{"hiddenMap", 1}}));
    Game& g = *gp;
    Side* side = game::begin(g);
    Ok(game::seen(g, side->index, side->capital->x, side->capital->y), "its capital is seen");
    Ok(!game::seen(g, side->index, 0, 0), "the far corner is not");
    auto g2p = NewGame("ERYTHEA", SeedWith(102, {{"hiddenMap", 1}}));
    Game& g2 = *g2p;
    int ox = NONE, oy = NONE;
    for (int y = 20; y <= 60; y++)
        for (int x = 20; x <= 60; x++)
            if (ox == NONE && !game::cityAt(g2, x, y)) {
                ox = x;
                oy = y;
            }
    // side 20 is nobody, so its map starts completely dark
    Eq(game::reveal(g2, 20, ox, oy, false), 9, "in the open a stack sees 3x3");
    Eq(game::reveal(g2, 20, ox, oy, false), 0, "seeing it again reveals nothing new");
    Eq(game::reveal(g2, 20, ox + 20, oy, true), 25, "a flying stack sees 5x5");
    City& city = g2.map->cities[0];
    Eq(game::reveal(g2, 21, city.x, city.y, false), 25, "standing on a city sees 5x5");
    Army* army = game::sideArmies(g, *side)[0];
    army->moves = 99;
    City* unseen = nullptr;
    for (auto& c : g.map->cities)
        if (!unseen && !game::seen(g, side->index, c.x, c.y) && c.ownerIndex != side->index)
            unseen = &c;
    Ok(unseen != nullptr, "there is a city the side has not seen");
    if (unseen)
        Ok(!movement::findPath(g, {army}, army->x, army->y, unseen->x, unseen->y),
           "a side cannot path into the dark");
    auto count = [&]() {
        int n = 0;
        auto it = g.explored.find(side->index);
        if (it != g.explored.end())
            for (uint8_t v : it->second)
                if (v) n++;
        return n;
    };
    int before = count();
    bool found = false;
    int tx = 0, ty = 0;
    for (int dx = -3; dx <= 3; dx++) {
        for (int dy = -3; dy <= 3; dy++) {
            int x = army->x + dx, y = army->y + dy;
            if (!found && x >= 0 && y >= 0 && game::seen(g, side->index, x, y) &&
                (x != army->x || y != army->y)) {
                auto p = movement::findPath(g, {army}, army->x, army->y, x, y);
                if (p && !p->empty() && p->back().cost < movement::PAST_SHORE) {
                    found = true;
                    tx = x;
                    ty = y;
                }
            }
        }
    }
    if (found) {
        movement::moveTo(g, {army}, tx, ty);
        int after = count();
        Ok(after > before, fmt("walking uncovered %d more tiles", after - before));
    }
    Side* other = nullptr;
    for (Side* s : g.sides)
        if (s->index != side->index) other = s;
    Ok(!game::seen(g, other->index, side->capital->x, side->capital->y),
       "another side has not seen our capital");
    // a computer that opens the game plays to the next human's hidden map
    auto g3p = NewGame("ERYTHEA", SeedWith(103, {{"hiddenMap", 1}}));
    Game& g3 = *g3p;
    for (size_t i = 0; i < g3.sides.size(); i++) g3.sides[i]->computer = i != 2;
    Side* first = game::begin(g3);
    Ok(first == g3.sides[0], "a computer opens the game");
    Ok(game::nextHuman(g3) == g3.sides[2], "the screen is the human's who follows it");
    g3.sides[2]->computer = true;
    Ok(game::nextHuman(g3) == nullptr, "and nobody's with no human playing");
}

// --------------------------------------------------------------------- bugs

static void TestBugFlags() {
    printf("bug compatibility\n");
    Ok(rules::bugs.heroExperienceReadsAttackerTypes,
       "the original's bugs are reproduced by default");
    // every flag must actually control something: a flag nothing reads is a
    // promise the engine does not keep
    std::set<std::string> used;
    for (const char* module : {"hero", "combat", "game", "move", "ai", "site", "quest"}) {
        auto text = readFile(std::string("cpp/src/warlords/") + module + ".cpp");
        if (!text) continue;
        std::regex re("rules::bugs\\.(\\w+)");
        for (auto it = std::sregex_iterator(text->begin(), text->end(), re);
             it != std::sregex_iterator(); ++it)
            used.insert((*it)[1]);
    }
    for (const char* name : {"heroExperienceReadsAttackerTypes"})
        Ok(used.count(name), std::string("the ") + name + " flag is read by the engine");
}

// ------------------------------------------------------------ the computer AI

// The random map generator (warlords/randommap.cpp, docs/re/random_map.md):
// what it makes, and a game played on it and saved.
// The map evaluator (warlords/mapeval, docs/mapeval.md): the hand-made maps
// break no hard rule, and each rule fires when a map is broken on purpose.
static void TestMapEval() {
    printf("map evaluator\n");
    for (const char* s : {"ERYTHEA", "HADESHA", "ISLADIA", "DRAGON", "SORCERY"}) {
        auto map = scn::load(kData + "/" + s, s);
        auto e = mapeval::evaluate(*map, s);
        std::string first = "";
        for (auto& i : e.issues)
            if (mapeval::isHard(i.check)) {
                first = i.check + ": " + i.detail;
                break;
            }
        Ok(e.passed(), std::string(s) + " breaks no hard rule " + first);
        Ok(e.metric("clarkEvansR") > 1.3,
           std::string(s) + "'s cities are spread out (Clark-Evans R > 1.3)");
    }
    auto map = scn::load(kData + "/ERYTHEA", "ERYTHEA");
    Map& m = *map;
    auto at = [&](int x, int y) { return y * MAP_W + x; };
    auto tile_of = [&](int type) {
        for (int t = 0; t < 256; t++)
            if (m.terrainType[t] == type) return t;
        return -1;
    };
    // a lone road piece on open plain, away from everything
    int lx = -1, ly = -1;
    for (int y = 3; y < MAP_H - 3 && lx < 0; y++)
        for (int x = 3; x < MAP_W - 3 && lx < 0; x++) {
            bool clear = true;
            for (int dy = -2; dy <= 2 && clear; dy++)
                for (int dx = -2; dx <= 2 && clear; dx++)
                    clear = scn::terrainAt(m, x + dx, y + dy) == movement::PLAIN &&
                            scn::roadAt(m, x + dx, y + dy) == 0;
            if (clear) {
                lx = x;
                ly = y;
            }
        }
    Ok(lx >= 0, "found open plain for a stray road");
    m.roads[at(lx, ly)] = 1;  // piece 1: east-west
    // a city on top of another, a name used twice, a signpost that lies
    m.cities[1].x = m.cities[0].x + 1;
    m.cities[1].y = m.cities[0].y;
    m.cities[3].name = m.cities[2].name;
    m.signs[0].lines = {m.cities[5].name, "999 leagues north"};
    // a ruin walled in by mountains
    const Site& walled = m.sites[0];
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = walled.x + dx, y = walled.y + dy;
            if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) continue;
            m.tiles[at(x, y)] = tile_of(movement::MOUNTAINS);
            m.roads[at(x, y)] = 0;
        }
    auto e = mapeval::evaluate(m, "broken");
    Ok(!e.passed(), "a broken map fails");
    for (const char* c : {"road-to-nowhere", "dead-end", "city-gap", "city-terrain", "city-name",
                          "signpost", "unreachable"})
        Ok(e.count(c) > 0, std::string("the broken map trips ") + c);
    // a second side's capital next door to the first's
    {
        auto near = scn::load(kData + "/ERYTHEA", "ERYTHEA");
        City* home = near->sides[0].capital;
        City* next = nullptr;
        for (auto& c : near->cities)
            if (&c != home && (!next || std::hypot(c.x - home->x, c.y - home->y) <
                                                std::hypot(next->x - home->x, next->y - home->y)))
                next = &c;
        near->sides[1].capital = next;
        Ok(mapeval::evaluate(*near, "near").count("capital-gap") > 0,
           "two capitals side by side trip capital-gap");
    }
    // the fair capitals: the same world, its capitals chosen for a fair start
    {
        Rng r1(11), r2(11);
        randommap::Options o;
        o.dataDir = kData;
        o.rng = &r1;
        auto classic_files = randommap::generateNow(o);
        o.rng = &r2;
        o.fairCapitals = true;
        auto fair_files = randommap::generateNow(o);
        Ok(classic_files["SCN"] != fair_files["SCN"], "fair capitals make a different start");
        auto fair = scn::loadFromFiles(kData + "/RANDOM", "RANDOM", fair_files);
        std::set<City*> capitals;
        for (auto& sd : fair->sides) capitals.insert(sd.capital);
        Eq(static_cast<int>(capitals.size()), 8, "fair capitals: eight, one a side");
        auto fe = mapeval::evaluate(*fair, "fair#11");
        Eq(fe.count("capital-gap"), 0, "fair capitals are never side by side");
        Ok(fe.metric("remoteCityFrac") <= 0.05,
           "fair capitals leave hardly a city far from every capital");
        Ok(fe.metric("capitalGapMinTurns") >= 2.5,
           "fair capitals are as far apart as the designers' (2.5 turns)");
    }
    // the reference and the score: the designers' own maps score nothing
    std::vector<mapeval::Evaluation> refs;
    for (const char* s : {"ERYTHEA", "HADESHA"})
        refs.push_back(mapeval::evaluate(*scn::load(kData + "/" + s, s), s));
    auto ref = mapeval::reference(refs);
    for (auto& r : refs)
        Eq(mapeval::score(r, ref).penalty, 0.0, r.name + " sits inside its own reference");
    // a generated world is evaluated like any other, straight from memory
    Rng r(1);
    randommap::Options o;
    o.dataDir = kData;
    o.rng = &r;
    auto files = randommap::generateNow(o);
    auto made = scn::loadFromFiles(kData + "/RANDOM", "RANDOM", files);
    auto me = mapeval::evaluate(*made, "classic#1");
    Eq(static_cast<int>(me.metric("cities")), 80, "a generated world is evaluated");
    Ok(mapeval::toJson(me).dump().find("\"metrics\"") != std::string::npos,
       "the evaluation writes as JSON");
    // the scenario generator (warlords/scengen): every shape, a few seeds, and
    // every world it makes keeps the hard rules
    for (auto shape :
         {scengen::Shape::Continents, scengen::Shape::Pangaea, scengen::Shape::Islands}) {
        for (int seed = 1; seed <= 3; seed++) {
            Rng r(seed), again(seed);
            randommap::Options o;
            o.dataDir = kData;
            o.rng = &r;
            o.scenario = true;
            o.world.shape = shape;
            auto files = randommap::generateNow(o);
            o.rng = &again;
            Ok(files == randommap::generateNow(o),
               std::string("the scenario generator is deterministic: ") +
                       scengen::shapeName(shape));
            auto world = scn::loadFromFiles(kData + "/RANDOM", "RANDOM", files);
            auto e = mapeval::evaluate(
                    *world, std::string(scengen::shapeName(shape)) + "#" + std::to_string(seed));
            std::string first;
            for (auto& i : e.issues)
                if (mapeval::isHard(i.check)) {
                    first = ": " + i.check + " " + i.detail;
                    break;
                }
            Ok(e.passed(), e.name + " keeps every hard rule" + first);
            Eq(static_cast<int>(e.metric("cities")), 80, e.name + " has the 80 cities asked for");
            Ok(e.metric("cityRoadCoverage") >= 0.74,
               e.name + " puts its cities on roads, as the designers do");
            Eq(world->options.militaryAdvisor, 1, e.name + " reads as an ordinary scenario");
        }
    }
    // names: no wider than "The Frozen Steppe" in CHANCE17, the font New
    // Scenario's list draws them in, measured from the font's own metrics
    {
        std::string fin = *readFile(kData + "/CHANCE17.FIN");
        int count = static_cast<uint8_t>(fin[0]), first_char = static_cast<uint8_t>(fin[1]);
        auto width = [&](const std::string& t) {
            int n = 0;
            for (unsigned char c : t)
                if (c >= first_char && c < first_char + count)
                    n += static_cast<uint8_t>(fin[11 + count + (c - first_char)]);
            return n;
        };
        int widest = 0;
        bool fit = true;
        for (auto shape :
             {scengen::Shape::Continents, scengen::Shape::Pangaea, scengen::Shape::Islands})
            for (long seed = 1; seed <= 600; seed++) {
                auto t = scengen::title(shape, seed);
                widest = std::max(widest, width(t.name));
                if (t.name.empty() || t.name.size() > 20 || t.text.empty() || t.text.size() > 30)
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
        fs::path tmp = fs::temp_directory_path() / "w2test-scenarios";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "DATA");
        fs::create_directories(tmp / "GEN7");
        fs::create_directories(tmp / "NOINFO");
        fs::copy_file(kData + "/DATA/SCENARIO.DAT", tmp / "DATA" / "SCENARIO.DAT");
        scenarios::Entry made{"Continents 7", "GEN7", "A new world, seed 7", 80, 40, 8, true};
        writeFile((tmp / "GEN7" / "GEN7.INF").string(), scenarios::info(made));
        writeFile((tmp / "GEN7" / "GEN7.SCN").string(), "");
        writeFile((tmp / "NOINFO" / "NOINFO.SCN").string(), "");
        Ok(scenarios::remember(tmp.string(), "NOINFO") &&
                   scenarios::remember(tmp.string(), "GEN7") &&
                   scenarios::remember(tmp.string(), "GEN7"),
           "SCENGEN.LST remembers the generated folders");
        Eq(*readFile((tmp / "DATA" / "SCENGEN.LST").string()), std::string("GEN7\nNOINFO\n"),
           "once each, sorted");
        auto list = scenarios::list(tmp.string());
        Eq(static_cast<int>(list.size()), 7,
           "SCENARIO.DAT's six and the one listed world with a NAME.INF");
        Eq(list[3].dir, std::string("Erythea"), "SCENARIO.DAT's own order first");
        Eq(list.back().name, std::string("Continents 7"),
           "the generated world after them, by its NAME.INF");
        Eq(list.back().cities, 80, "with its cities");
        Ok(list.back().generated && !list[0].generated, "and marked as generated");
        auto back = scenarios::parseInfo(scenarios::info(made), "GEN7");
        Ok(back && back->text == made.text && back->players == 8, "NAME.INF reads back as written");
        fs::remove_all(tmp);
    }
    // the start menu's picture for a generated world: a .PCK the game reads
    {
        auto map = scn::load(kData + "/ERYTHEA", "ERYTHEA");
        auto pic = preview::menuPicture(*map, kData);
        Eq(pic.w, 264, "the menu picture fills the frame's width");
        Eq(pic.h, 225, "and its height");
        std::string path =
                (std::filesystem::temp_directory_path() / "w2test-SCENARIO.PCK").string();
        writeFile(path, pck::encode(pic));
        auto read = pck::decode(path);
        Ok(read->w == pic.w && read->h == pic.h && read->px == pic.px,
           "the .PCK it is written as decodes to the same picture");
        std::filesystem::remove(path);
        std::string png = preview::png(*map, kData);
        Ok(png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "the preview is a PNG");
    }
}

static void TestRandomMap() {
    printf("random map\n");
    auto make = [](int seed, std::array<int, 4> sliders, bool allies) {
        Rng r(seed);
        randommap::Options o;
        o.dataDir = kData;
        o.rng = &r;
        o.sliders = sliders;
        o.allies = allies;
        return randommap::generateNow(o);
    };
    auto a = make(7, {{3, 3, 2, 3}}, false), b = make(7, {{3, 3, 2, 3}}, false);
    for (const char* ext : randommap::FILES) {
        Ok(a.count(ext) && !a[ext].empty(), std::string("the generator writes RANDOM.") + ext);
        Ok(a[ext] == b[ext], std::string("the same dice make the same RANDOM.") + ext);
    }
    Eq(static_cast<int>(a["SCN"].size()), 12001,
       "RANDOM.SCN is Erythea's .SCN rewritten (4fef:1001)");
    Eq(static_cast<int>(a["MAP"].size()), 2 * 112 * 156, "RANDOM.MAP is a word a tile");
    randommap::install(kData, a);
    auto map = scn::load(kData + "/RANDOM", "RANDOM");
    Eq(static_cast<int>(map->cities.size()), 80,
       "Cities at its third place gives RANDOM.DAT's 80 (4bed:0000)");
    Eq(static_cast<int>(map->sites.size()), 40, "forty sites (513d:003a)");
    Ok(map->signs.size() >= 41 && map->signs.size() <= 70, "1d30+40 signposts (4fef:113d)");
    std::set<City*> capitals;
    for (auto& sd : map->sides) {
        Ok(sd.capital && sd.capital->owner == &sd, sd.name + " holds its capital (513d:0689)");
        capitals.insert(sd.capital);
    }
    Eq(static_cast<int>(capitals.size()), 8, "eight capitals, one a side");
    for (auto& c : map->cities) {
        Eq(map->terrainType[scn::tileAt(*map, c.x, c.y) % 256], 10,
           c.name + " stands on a castle tile");
        Ok(!c.produces.empty(), c.name + " makes something (513d:161d)");
        bool capital = c.owner != nullptr;
        Ok(c.income >= (capital ? 33 : 15) && c.income <= (capital ? 40 : 28),
           c.name + "'s income is value x 2 + 1d8 + 14 (513d:1171)");
        auto t = map->cityText.find(c.index);
        Ok(t != map->cityText.end() && startsWith(t->second[0], c.name + " is a"),
           c.name + " has its description");
    }
    Types types;
    armytype::load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    auto magical = [&](int t) { return types.byId(t) && types.byId(t)->bonus[48] != 0; };
    bool none = true;
    for (auto& c : map->cities)
        for (int t : c.produces)
            if (magical(t)) none = false;
    Ok(none, "without allies no city makes a magical army (513d:161d, 1c1d)");
    int allies = 0;
    for (int seed = 1; seed <= 10; seed++) {
        randommap::install(kData, make(seed, {{3, 3, 2, 3}}, true));
        auto m = scn::load(kData + "/RANDOM", "RANDOM");
        for (auto& c : m->cities)
            for (int t : c.produces)
                if (magical(t)) allies++;
    }
    Ok(allies > 0, "with allies on, 2d3 cities may make them (513d:1b3f)");
    randommap::install(kData, make(3, {{0, 0, 6, 0}}, false));
    Eq(static_cast<int>(scn::load(kData + "/RANDOM", "RANDOM")->cities.size()), 100,
       "Cities at its last place: 80 + 20");
    randommap::install(kData, make(3, {{6, 6, 0, 6}}, false));
    Eq(static_cast<int>(scn::load(kData + "/RANDOM", "RANDOM")->cities.size()), 70,
       "Cities at its first place: 80 - 10");
    Rng r5(5);
    auto rolled = randommap::settle({{7, 7, 7, 7}}, r5);
    bool in_range = true;
    for (int v : rolled)
        if (v < 0 || v > 6) in_range = false;
    Ok(in_range, "a slider left to chance is rolled 1d7-1 (7bab:10e8)");
    // the shipped RANDOM folder is the original's last world, left alone
    std::ifstream disk(kData + "/RANDOM/RANDOM.SCN", std::ios::binary);
    if (disk) {
        std::string on_disk((std::istreambuf_iterator<char>(disk)),
                            std::istreambuf_iterator<char>());
        Ok(on_disk != a["SCN"], "the made world is kept in memory, not written over RANDOM.SCN");
    }

    // a game on it, and a save that carries the world with it
    randommap::install(kData, a);
    game::NewGameOptions o;
    o.seed = 4;
    auto gp = game::newGame(kData, "RANDOM", o);
    Game& g = *gp;
    for (Side* sd : g.sides) sd->computer = true;
    Side* side = game::begin(g);
    while (side && g.turn <= 6) {
        PlayAi(g, *side);
        side = EndTurn(g);
    }
    Eq(g.turn, 7, "the computer players play a random world");
    std::string text = save::encode(g);
    randommap::install(kData, make(8, {{3, 3, 2, 3}}, false));
    auto g2 = save::decode(text, kData);
    Ok(g2->map->tiles == g.map->tiles, "a saved random world comes back with its own map");
    bool names = g2->map->cities.size() == g.map->cities.size();
    for (size_t i = 0; names && i < g.map->cities.size(); i++)
        names = g2->map->cities[i].name == g.map->cities[i].name;
    Ok(names, "... and its own cities");
    Eq(g2->map->signs.size(), g.map->signs.size(), "... and its own signposts");
}

static void TestComputerPlayers() {
    printf("computer players\n");
    namespace core = ai::core;
    namespace groups = ai::groups;
    for (int level = 0; level <= 2; level++)
        Eq(aicard::count(kData, level), 9, "nine cards for level " + std::to_string(level));
    auto w = *aicard::load(kData, 2, 0);
    Eq(w.groups, 4, "the Standard Warlord runs four assault groups");
    Eq(w.raze, 5, "and razes 5 in 1000");
    Eq(w.humanShare, 3, "and turns on a human with 3x10+1d10 percent");
    Eq(w.solidarity, 1, "and stands with the other computers");
    auto k = *aicard::load(kData, 0, 0);
    Eq(k.cautious, 1, "the Standard Knight is cautious");
    Eq(k.groups, 1, "and runs one group");
    Eq(aicard::describe(kData, 2, 1)->first, "Attila the Hun",
       "a card's name is the first line of its .DSC");
    auto lords = aicard::deck(kData, 1);
    Eq(static_cast<int>(lords.size()), 9, "the setup screen lists a Lord's nine characters");
    Eq(lords[0], "Standard Lord", "the Standard one first");
    Eq(lords[2], "Roland the Rabid", "each by its .DSC's first line");
    {
        // a character chosen on the setup screen is the card the side plays
        GameOpts ro = Seed(70);
        ro.sides[0] = SideSetup(false, 0);
        game::SideSetup rabid = SideSetup(true, 1);
        rabid.card = 2;
        rabid.name = "Rabids";
        ro.sides[1] = rabid;
        auto rp = NewGame("ERYTHEA", ro);
        Side& rs = rp->map->sides[1];
        Eq(rs.card, 2, "the side keeps its character");
        Eq(rs.name, "Rabids", "and the name it was given");
        Eq(rs.ai->maxGroups, 2, "Roland the Rabid runs two assault groups");
        Eq(rs.ai->raze, 14, "razes 14 in 1000");
        Eq(rs.ai->sack, 100, "sacks 100");
        Eq(rs.ai->early, 1, "and takes early vengeance on a human");
        Eq(rp->map->sides[2].ai->maxGroups, 4, "a side left alone plays its Standard card");
    }
    Eq(Rng(5).dice(1, 0, 3), 3, "dice(1, 0, 3) is 3");
    Eq(core::dist(0, 0, 3, 4), 5, "distance is Euclidean");
    Eq(core::dist(0, 0, 1, 1), 1, "and truncated");

    GameOpts o = Seed(71);
    for (int i = 0; i <= 7; i++) o.sides[i] = SideSetup(true, i % 3);
    o.sides[0] = SideSetup(false, 0);
    auto gp = NewGame("ERYTHEA", o);
    Game& g = *gp;
    Side& knight = g.map->sides[3];
    Side& lord = g.map->sides[1];
    Side& warlord = g.map->sides[2];
    Eq(knight.level, 0, "side 3 plays as a Knight");
    Eq(knight.ai->maxGroups, 1, "a Knight runs one group");
    Eq(knight.ai->cautious, 1, "and is cautious");
    Ok(!knight.ai->bold, "and never attacks a side it is not at war with");
    Eq(lord.ai->maxGroups, 3, "a Lord three");
    Ok(lord.ai->bold, "a Lord is bold -- the level sets it and a card cannot clear it");
    Eq(warlord.ai->maxGroups, 4, "a Warlord four");
    Ok(warlord.ai->humanShare >= 31 && warlord.ai->humanShare <= 40,
       "a Warlord turns on a human holding 31-40% of the world: " +
               std::to_string(warlord.ai->humanShare));
    Ok(knight.ai->humanShare >= 81 && knight.ai->humanShare <= 90, "a Knight at 81-90%");
    Eq(g.map->fightOrder[3][8], w.fightOrder[8], "the card sets the side's fight order");
    int unclaimed = 0;
    for (auto& c : g.map->cities)
        if (c.claim == NONE || c.claim == core::NEUTRAL) unclaimed++;
    Eq(unclaimed, 0, "the computers claim every city between them (623c:010b)");
    for (Side* s : g.sides) Eq(s->capital->claim, s->index, s->name + " claims its capital");
    for (Side* s : g.sides)
        Ok(s->diploScore >= 1 && s->diploScore <= 8, "a diplomatic score starts at 1d8");

    GameOpts og = o;
    og.seed = 72;
    og.greatest = true;
    og.options = {{"diplomacy", 1}};
    auto ggp = NewGame("ERYTHEA", og);
    Game& gg = *ggp;
    Ok(gg.map->sides[0].diploScore > 400, "I am the Greatest: a human's score starts above 400");
    Ok(gg.map->sides[1].diploScore <= 8, "a computer's does not");
    gg.diplomacy.state[1 * 8 + 0] = diplomacy::WAR;
    gg.diplomacy.state[0 * 8 + 1] = diplomacy::WAR;
    auto fights = groups::fightingHumans(gg);
    Ok(fights[1], "a computer at war with the human is marked");
    ai::diplo::phase(gg, gg.map->sides[1]);
    Eq(diplomacy::proposal(gg, 1, 2), diplomacy::PEACE, "and no computer proposes war on it");

    GameOpts oh = o;
    oh.seed = 73;
    oh.options = {{"hiddenMap", 1}};
    auto hp = NewGame("ERYTHEA", oh);
    Game& h = *hp;
    Side& human_s = h.map->sides[0];
    Side& comp = h.map->sides[1];
    auto hgrid = movement::grid(h, human_s.index);
    auto hpd = movement::prepare(h, hgrid, human_s.index, movement::LAND, 0, 0);
    auto cpd = movement::prepare(h, hgrid, comp.index, movement::LAND, 0, 0);
    int fogged = 0;
    for (int k2 = 0; k2 < h.map->width * h.map->height; k2++) {
        if (hpd[k2] == movement::SHUT && cpd[k2] != movement::SHUT) fogged++;
    }
    Ok(fogged > 1000, "unseen ground blocks the human's paths only: " + std::to_string(fogged));

    Army* army = nullptr;
    for (Army* a : g.armies)
        if (a->owner == warlord.index) army = a;
    auto sel = core::select(g, {army});
    Eq(core::odds(g, sel, 0, 0), 100, "nothing to fight is a sure thing");
    auto [nb, nd] = core::neighbours(g, g.map->cities[0]);
    Eq(static_cast<int>(nb.size()), 6, "a city has six neighbours");
    for (int v : nd) Ok(v < 100, "each within reach");

    Eq(groups::pickEnemy(g, warlord), NONE, "no enemy is picked in the first turns");
    warlord.ai->battles[4] = 3;
    warlord.ai->citiesLost[4] = 2;
    Ok(groups::pickEnemy(g, warlord) != NONE, "a side that has fought us can be picked");

    City* c = &g.map->cities[0];
    for (auto& cc : g.map->cities)
        if (&cc != knight.capital && &cc != warlord.capital) {
            c = &cc;
            break;
        }
    c->ownerIndex = warlord.index;
    int before = warlord.diploScore;
    Ok(groups::earlyVengeance(g, warlord.index, *c, 0),
       "a Warlord takes vengeance on a human's city");
    Ok(warlord.diploScore > before, "and it is an atrocity");
    Ok(!groups::earlyVengeance(g, knight.index, *c, 0), "a Knight does not");

    Side* side = game::begin(g);
    while (side && g.turn <= 12) {
        if (side->computer) PlayAi(g, *side);
        side = EndTurn(g);
    }
    auto grp = warlord.ai->groups[1];
    grp->active = 2;
    grp->target = 1;
    grp->rally = warlord.capital->index;
    grp->staged[2] = g.armies.back();
    auto g2 = save::decode(save::encode(g), kData);
    auto& w2 = *g2->map->sides[warlord.index].ai;
    Eq(w2.maxGroups, warlord.ai->maxGroups, "the AI data survives a save");
    Eq(w2.groups[1]->rally, warlord.capital->index, "and its groups");
    Eq(w2.groups[1]->staged[2], g2->armies.back(), "with their staged armies");
    Eq(g2->map->cities[4].claim, g.map->cities[4].claim, "and the cities' claims");
}

// --------------------------------------------------------------- encampments

static void TestEncampment() {
    printf("encampments\n");
    auto gp = NewGame("ERYTHEA", Seed(5));
    Game& g = *gp;
    Side* side = game::begin(g);
    City* cap = side->capital;
    Army* a = nullptr;
    for (Army* b : g.armies)
        if (!a && b->owner == side->index && !b->transit) a = b;
    int x = NONE, y = NONE;
    for (int r = 2; r <= 8 && x == NONE; r++) {
        for (int dx = -r; dx <= r && x == NONE; dx++) {
            for (int dy = -r; dy <= r && x == NONE; dy++) {
                int tx = cap->x + dx, ty = cap->y + dy;
                if (tx >= 0 && ty >= 0 && tx < g.map->width - 1 && ty < g.map->height &&
                    scn::terrainAt(*g.map, tx, ty) == movement::PLAIN &&
                    scn::terrainAt(*g.map, tx + 1, ty) == movement::PLAIN &&
                    scn::roadAt(*g.map, tx, ty) % 32 == 0 && game::armiesAt(g, tx, ty).empty() &&
                    game::armiesAt(g, tx + 1, ty).empty()) {
                    x = tx;
                    y = ty;
                }
            }
        }
    }
    Ok(a != nullptr && x != NONE, "an army and open ground to encamp on");
    if (!(a && x != NONE)) return;
    a->x = x;
    a->y = y;
    Ok(!game::towerAt(g, x, y), "a stack on open ground is no encampment");
    game::startTurn(g, *side);
    Ok(!game::towerAt(g, x, y), "nor after a turn in the army cycle");
    a->fortified = true;
    game::startTurn(g, *side);
    Ok(game::towerAt(g, x, y), "a defended stack encamps as its turn opens");
    Eq(combat::fortify(g, {}, x, y, combat::terrainClass(g, x, y)), 1,
       "an encampment fights as a fortification of 1");
    auto r = movement::moveTo(g, {a}, x + 1, y);
    Eq(r.steps, 1, "the encamped stack walks off");
    Ok(!game::towerAt(g, x, y), "and the empty tile is no encampment any more");
    a->x = cap->x;
    a->y = cap->y;
    game::startTurn(g, *side);
    Ok(!game::towerAt(g, cap->x, cap->y), "a defended stack in a city does not encamp");
}

// ------------------------------------------------------------- going to sea

static void TestSea() {
    printf("going to sea\n");
    auto gp = NewGame("ERYTHEA", Seed(81));
    Game* g = gp.get();
    int lx = NONE, ly = NONE, wx = NONE, wy = NONE;
    for (int y = 1; y <= g->map->height - 2; y++) {
        for (int x = 1; x <= g->map->width - 2; x++) {
            int t2 = scn::terrainAt(*g->map, x + 1, y);
            if (lx == NONE && scn::terrainAt(*g->map, x, y) == movement::PLAIN &&
                (t2 == movement::WATER || t2 == movement::SHORE) &&
                game::armiesAt(*g, x, y).empty() && game::armiesAt(*g, x + 1, y).empty()) {
                lx = x;
                ly = y;
                wx = x + 1;
                wy = y;
            }
        }
    }
    Ok(lx != NONE, "Erythea has a plain beside the sea");
    const ArmyType* dragon = nullptr;
    for (auto& t : g->types.all)
        if (t.flies && contains(t.name, "Dragon")) dragon = &t;
    auto army = [&](int type_id) { return AddArmy(*g, lx, ly, 0, type_id, 5, 20, 20); };
    Army* hero = army(armytype::HERO);
    Army* d = army(dragon->id);
    std::vector<Army*> stack = {hero, d};
    Eq(movement::modeOf(*g, stack), movement::FLYING, "a hero with a dragon flies");
    movement::walk(*g, stack, {movement::Step{wx, wy, 2}});
    Eq(hero->x, wx, "and flies out over the water");
    Ok(!d->atSea && !hero->atSea, "without either going to sea");
    Eq(movement::modeOf(*g, stack), movement::FLYING, "so it still flies");
    movement::walk(*g, stack, {movement::Step{lx, ly, 2}});
    Eq(hero->x, lx, "and can come back to land");
    Army* foot = army(1);
    movement::settleSea(*g, {foot}, wx, wy, movement::LAND, false);
    Ok(foot->atSea, "a land army ending on water is at sea");
    movement::settleSea(*g, {foot}, lx, ly, movement::LAND, true);
    Ok(!foot->atSea, "and ashore again on land");
    movement::settleSea(*g, {hero, d, foot}, wx, wy, movement::LAND, false);
    Ok(!d->atSea, "a flier never goes to sea");
    Ok(hero->atSea && foot->atSea, "the hero and the footman with it do");
    hero->atSea = false;
    foot->atSea = false;
    movement::settleSea(*g, {foot}, wx, wy, movement::BOAT, false);
    Ok(!foot->atSea, "a boat move leaves the flag as it was");
    hero->x = wx;
    hero->y = wy;
    d->x = wx;
    d->y = wy;
    hero->atSea = true;
    d->atSea = true;
    g->remove(foot);
    auto g2 = save::decode(save::encode(*g), kData);
    for (Army* a : g2->armies)
        if (a->x == wx && a->y == wy) Ok(!a->atSea, "a loaded flier and its hero are not at sea");

    auto gp2 = NewGame("ERYTHEA", Seed(81));
    g = gp2.get();
    City* mirea = nullptr;
    for (auto& c : g->map->cities)
        if (c.name == "Mirea") mirea = &c;
    Ok(mirea && mirea->ownerIndex == 0 && movement::isPort(*g, *mirea), "Mirea is side 0's port");
    if (!mirea) return;
    auto& grid = movement::grid(*g, 0);
    auto byte = [&](int x, int y) { return grid[y * g->map->width + x]; };
    Eq(scn::terrainAt(*g->map, 86, 7), movement::SHORE, "the shore runs past Mirea");
    Ok(movement::has(byte(86, 7), movement::WATER_F), "and a shore tile is water to a walker");
    Ok(!movement::stepCost(byte(83, 9), byte(82, 9), movement::LAND, false, false, 10).has_value(),
       "so a land stack cannot step onto it from a plain");
    Army* sailor = AddArmy(*g, mirea->x, mirea->y, 0, 1, 5, 20, 20);
    auto r = movement::moveTo(*g, {sailor}, 86, 4);
    Eq(sailor->y, 7, "leaving port it stops on the first sea tile");
    Eq(scn::terrainAt(*g->map, sailor->x, sailor->y), movement::SHORE, "the shore");
    Ok(sailor->atSea, "at sea");
    Eq(r.spent, movement::COST[movement::SHORE], "paying the tile's cost and no water charge");
    Eq(sailor->moves, 0, "and going to sea uses up the rest of its move");
    Eq(r.stopped, "out of moves", "which ends the walk");
    sailor->moves = 20;
    r = movement::moveTo(*g, {sailor}, 86, 4);
    Eq(sailor->y, 4, "next turn it sails out into open water");
    Ok(sailor->atSea, "still at sea");
    Eq(sailor->moves, 20 - (7 - 4) * movement::COST[movement::WATER], "paying 1 a tile");
    auto p = movement::findPath(*g, {sailor}, 86, 4, 86, 5);
    Eq(p ? (*p)[0].cost : NONE, movement::COST[movement::WATER],
       "sailing on costs no water charge either");
    r = movement::moveTo(*g, {sailor}, mirea->x, mirea->y);
    Eq(sailor->x * 1000 + sailor->y, mirea->x * 1000 + mirea->y, "it sails back into port");
    Ok(!sailor->atSea, "and comes ashore there");
    Eq(sailor->moves, 0, "which uses up its move too");
    City* kuuria = nullptr;
    for (auto& c : g->map->cities)
        if (c.name == "Kuuria") kuuria = &c;
    int sx = NONE, sy = NONE;
    for (int x = kuuria->x - 1; x <= kuuria->x + 2; x++) {
        for (int y = kuuria->y - 1; y <= kuuria->y + 2; y++) {
            int t = scn::terrainAt(*g->map, x, y);
            if (sx == NONE && (t == movement::WATER || t == movement::SHORE) &&
                !g->map->crossing[y * g->map->width + x] && game::armiesAt(*g, x, y).empty()) {
                sx = x;
                sy = y;
            }
        }
    }
    Ok(sx != NONE, "Kuuria has open water beside it");
    sailor->x = sx;
    sailor->y = sy;
    sailor->atSea = true;
    sailor->moves = 20;
    Ok(movement::findPath(*g, {sailor}, sx, sy, kuuria->x, kuuria->y).has_value(),
       "a stack at sea can path into the port it attacks");
}

// --------------------------------------------------------------------- sound

// Rules checked against WARLORD2.EXE in web-verification.md, one by one.
static void TestVerified() {
    printf("verified against the original\n");
    auto mk = [](Game& g, int x, int y, int owner, int type, int strength = NONE) {
        const ArmyType* t = g.types.byId(type);
        Army a;
        a.x = x;
        a.y = y;
        a.owner = owner;
        a.type = type;
        a.name = t->name;
        a.strength = strength != NONE ? strength : t->strength;
        a.maxMoves = t->move;
        a.moves = t->move;
        a.upkeep = 1;
        return g.add(a);
    };

    // the army table: the lowest free slot (1b62:0309), tiles read from the top
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        size_t n = g.armies.size();
        Army* third = g.armies[2];
        g.remove(third);
        Army* a = mk(g, 0, 0, 0, 11);
        Eq(a->slot, 2, "a new army takes the lowest free slot");
        Eq(static_cast<int>(std::find(g.armies.begin(), g.armies.end(), a) - g.armies.begin()), 2,
           "and stands in slot order");
        Eq(g.armies.size(), n, "the table is as long as before");
        Army* b = mk(g, 0, 0, 0, 11);
        Eq(game::armiesAt(g, 0, 0)[0], b, "a tile is read from the top of the table down");
    }

    // P1: the production nudges are dice(1,100) < p
    {
        Rng r(3);
        int hits = 0;
        for (int i = 0; i < 20000; i++)
            if (r.chance(10)) hits++;
        Ok(hits > 20000 * 0.08 && hits < 20000 * 0.10,
           "chance(10) is 9 in 100: " + std::to_string(hits));
        Rng t(1);
        bool never = true;
        for (int i = 0; i < 5000; i++)
            if (t.chance(1)) never = false;
        Ok(never, "chance(1) never comes up");
    }

    // P2, P3, P5: no spilling round a full city; a city that cannot build
    // goes idle; a human's city that builds starts the next
    {
        auto gp = NewGame("ERYTHEA", Seed(2));
        Game& g = *gp;
        Side* side = game::begin(g);
        City* city = side->capital;
        game::setProduction(g, *city, 0);
        city->countdown = 1;
        for (auto d : std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
            while (game::armiesAt(g, city->x + d.first, city->y + d.second).size() < 8) {
                mk(g, city->x + d.first, city->y + d.second, side->index, 11);
            }
        }
        size_t n = g.armies.size();
        side->productionDue = true;
        g.turn = 2;
        game::humanProduction(g, *side);
        Eq(g.armies.size(), n, "a full city builds nothing: no spilling round it");
        Eq(city->countdown, 0, "its countdown stays at 0");
        Eq(game::producing(*city), false, "and it is idle");
        for (Army* a : game::armiesAt(g, city->x + 1, city->y + 1)) g.remove(a);
        side->productionDue = true;
        game::humanProduction(g, *side);
        Eq(g.armies.size(), n - 8, "an idle city does not try again");
        game::setProduction(g, *city, 0);
        city->countdown = 1;
        side->productionDue = true;
        game::humanProduction(g, *side);
        Eq(g.armies.size(), n - 7, "set again, it builds");
        Eq(static_cast<int>(game::armiesAt(g, city->x + 1, city->y + 1).size()), 1,
           "on the tile with room");
        Eq(city->countdown, city->slots[0].time, "and starts the next (6f8c:06bd)");
        Army* built = game::armiesAt(g, city->x + 1, city->y + 1)[0];
        Eq(built->moves, built->maxMoves, "a new army has its full move (6f8c:0dd7)");
    }

    // P7: the city's tiles fill x first
    {
        auto gp = NewGame("ERYTHEA", Seed(2));
        Game& g = *gp;
        City& city = g.map->cities[0];
        for (Army* a : std::vector<Army*>(g.armies)) {
            if (a->x >= city.x && a->x <= city.x + 1 && a->y >= city.y && a->y <= city.y + 1)
                g.remove(a);
        }
        for (int i = 0; i < 8; i++) mk(g, city.x, city.y, 0, 11);
        auto [x, y] = game::freeTileIn(g, city, false);
        Eq(x, city.x + 1, "after (x,y) comes (x+1,y)");
        Eq(y, city.y, "on the same row");
    }

    // T1: Random Turns shuffles the order at each round's end
    {
        auto gp = NewGame("ERYTHEA", SeedWith(4, {{"randomTurns", 1}}));
        Game& g = *gp;
        for (Side* s : g.sides) s->computer = true;
        Side* side = game::begin(g);
        const std::array<int, 8> plain{0, 1, 2, 3, 4, 5, 6, 7};
        bool shuffled = false;
        while (side && g.turn <= 6) {
            if (g.turnOrder != plain) shuffled = true;
            Eq(side->index, g.turnOrder[g.orderAt], "the side to play is the order's");
            side = game::endTurn(g);
        }
        Ok(shuffled, "with Random Turns the order is shuffled (8065:1732)");
        auto sorted = g.turnOrder;
        std::sort(sorted.begin(), sorted.end());
        Ok(sorted == plain, "it is still every side once");
        auto g2p = NewGame("ERYTHEA", Seed(4));
        Game& g2 = *g2p;
        game::begin(g2);
        for (int i = 0; i < 30; i++) game::endTurn(g2);
        Ok(g2.turnOrder == plain, "without it the order stays");
    }

    // C7: a neutral city that beats off an attack builds, Neutral Cities >= 2
    {
        auto gp = NewGame("ERYTHEA", SeedWith(5, {{"neutralCities", 2}}));
        Game& g = *gp;
        Side* side = game::begin(g);
        City* city = nullptr;
        for (auto& c : g.map->cities)
            if (!city && c.ownerIndex == NONE && !c.slots.empty()) city = &c;
        for (Army* a : game::armiesAt(g, city->x, city->y)) a->strength = 9;
        Army* weak = mk(g, city->x - 1, city->y, side->index, 11, 1);
        Battle r = game::decideAttack(g, {weak}, city->x, city->y);
        Ok(city->attacked & (1 << side->index), "an attacked city is marked (67cc:02b1)");
        if (!r.won) {
            game::applyAttack(g, r);
            size_t before = game::armiesAt(g, city->x, city->y).size();
            for (size_t i = 0; i < 12 * g.sides.size(); i++) EndTurn(g);
            Ok(game::armiesAt(g, city->x, city->y).size() > before,
               "the neutral city builds (6bd8:025a)");
            Ok(game::armiesAt(g, city->x, city->y).size() <= 4 || before >= 4,
               "while its tile holds fewer than 4");
        }
    }

    // C10: a medal now and then for a hero's stack that wins
    {
        int medals = 0;
        for (int sd = 1; sd <= 60 && medals == 0; sd++) {
            auto gp = NewGame("ERYTHEA", Seed(sd));
            Game& g = *gp;
            Side* side = game::begin(g);
            City* city = nullptr;
            for (auto& c : g.map->cities)
                if (!city && c.ownerIndex == NONE) city = &c;
            for (Army* a : game::armiesAt(g, city->x, city->y)) a->strength = 1;
            while (game::armiesAt(g, city->x, city->y).size() < 2)
                mk(g, city->x, city->y, NONE, 11, 1);
            std::vector<Army*> stack = {mk(g, city->x - 1, city->y, side->index, armytype::HERO, 9),
                                        mk(g, city->x - 1, city->y, side->index, 11, 9),
                                        mk(g, city->x - 1, city->y, side->index, 11, 9)};
            Battle b = game::decideAttack(g, stack, city->x, city->y);
            Battle& r = game::applyAttack(g, b);
            if (r.medal) {
                medals++;
                Eq(r.medal->medals, 1, "a first medal");
                Ok(r.medal->type != armytype::HERO, "for an ordinary army, not the hero");
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
            Side* side = game::begin(g);
            Site* s = nullptr;
            for (auto& x : g.map->sites)
                if (!s && x.content == site::ALLIES) s = &x;
            if (!s) continue;
            tried = true;
            Army* h = mk(g, s->x, s->y, side->index, armytype::HERO, 1);
            auto r = site::search(g, {h}, s->x, s->y, false);
            Eq(r->kind, "allies", "a hero always comes out of an allies ruin with allies");
            Ok(g.alive(h), "alive");
        }
        Ok(tried, "found an allies ruin");
    }

    // Q1, Q4: a computer's quest types; Archons for a reward
    {
        auto gp = NewGame("ERYTHEA", SeedWith(8, {{"quests", 1}}));
        Game& g = *gp;
        Side* side = game::begin(g);
        Army* h = mk(g, side->capital->x, side->capital->y, side->index, armytype::HERO);
        std::set<int> seen;
        for (int i = 0; i < 40; i++) {
            side->quest.reset();
            seen.insert(quest::assign(g, *side, h, true)->type);
        }
        bool only = true;
        for (int t : seen) {
            if (t != quest::OCCUPY && t != quest::RAZE && t != quest::SLAUGHTER &&
                t != quest::PILLAGE_GOLD)
                only = false;
        }
        Ok(only, "a computer only gets occupy, raze, slaughter or gold (quest_assign(1))");
        bool archons = false;
        Quest q;
        q.hero = h;
        for (int i = 0; i < 60 && !archons; i++) {
            side->gold = 500;
            auto r = quest::reward(g, *side, &q);
            if (r->kind == "allies") {
                archons = true;
                Eq(r->type->id, quest::REWARD_ALLY, "a quest's allies are Archons");
            }
        }
        Ok(archons, "a quest can give allies");
    }

    // H1: no hero once the surrender has been offered
    {
        auto gp = NewGame("ERYTHEA", Seed(9));
        Game& g = *gp;
        Side* side = game::begin(g);
        g.surrenderOffered = true;
        side->gold = 100000;
        bool any = false;
        for (int i = 0; i < 50; i++)
            if (hero::offer(g, *side)) any = true;
        Ok(!any, "no hero offers itself after the surrender is offered");
    }

    // M1: a cached route is taken by a stack standing on it
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        Side* side = game::begin(g);
        Army* a = game::sideArmies(g, *side)[0];
        City* far = nullptr;
        for (auto& c : g.map->cities)
            if (!far && movement::distance(c.x, c.y, a->x, a->y) > 20) far = &c;
        auto p1 = movement::findPath(g, {a}, a->x, a->y, far->x, far->y);
        Ok(p1 && p1->size() > 3, "a long route");
        bool cached = false;
        for (auto& e : g.pathCache->slots)
            if (e && e->dx == far->x && e->dy == far->y) cached = true;
        Ok(cached, "is kept (1555:16f0)");
        auto mid = (*p1)[2];
        auto p2 = movement::findPath(g, {a}, mid.x, mid.y, far->x, far->y);
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
        auto f = ai::core::floodFrom(g, 0, c.x, c.y, 5, nullptr, NONE);
        bool beyond = false;
        for (int y = 0; y < g.map->height; y++) {
            for (int x = 0; x < g.map->width; x++) {
                if (std::max(std::abs(x - c.x), std::abs(y - c.y)) > 5 &&
                    ai::core::floodAt(f, x, y) != ai::core::UNREACHED) {
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
        Side* side = game::begin(g);
        int fx = NONE, fy = NONE;
        auto free_at = [&](int tx, int ty) {
            return scn::terrainAt(*g.map, tx, ty) == movement::PLAIN && !game::cityAt(g, tx, ty) &&
                   game::armiesAt(g, tx, ty).empty() && scn::roadAt(*g.map, tx, ty) % 32 == 0;
        };
        for (int y = 1; y < g.map->height - 1 && fx == NONE; y++)
            for (int x = 1; x < g.map->width - 1 && fx == NONE; x++)
                if (free_at(x, y) && free_at(x + 1, y)) {
                    fx = x;
                    fy = y;
                }
        Army* a = mk(g, fx, fy, side->index, 11);
        int cost = (*movement::findPath(g, {a}, a->x, a->y, fx + 1, fy))[0].cost;
        a->moves = cost + 1;
        Eq(movement::moveTo(g, {a}, fx + 1, fy).outOfMoves, true,
           "one move left after the walk: out of moves");
        a->moves = cost + 2;
        Eq(movement::moveTo(g, {a}, fx, fy).outOfMoves, false,
           "two left after arriving: not out of moves");
    }

    // a walk to sea is shown as it set off, on land, to its end (1a8b:04c8)
    {
        auto gp = NewGame("ERYTHEA", Seed(1));
        Game& g = *gp;
        Side* side = game::begin(g);
        Army* a = mk(g, 71, 11, side->index, 4);
        a->moves = 40;
        auto r = movement::moveTo(g, {a}, 70, 14);
        Ok(a->x == 70 && a->y == 14, "over the bridge to the shore");
        Eq(r.seaBefore, false, "it set off on land, and is shown so");
        Eq(a->atSea, true, "and is at sea once the walk is done");
        a->moves = 40;
        Eq(movement::moveTo(g, {a}, 71, 11).seaBefore, true, "coming ashore it is shown at sea");
    }

    // the item quest uncovers round its ruin two tiles out when the searching
    // stack flies (8611:1298, DS:1e9c), one otherwise
    for (bool flying : {false, true}) {
        auto gp = NewGame("ERYTHEA", SeedWith(2, {{"quests", 1}, {"hiddenMap", 1}}));
        Game& g = *gp;
        Side* side = game::begin(g);
        Site* s = nullptr;
        for (auto& x : g.map->sites) {
            if (s || x.content != site::ITEM) continue;
            for (auto& it : g.map->items)
                if (it.index == x.item && !site::itemReserved(it)) s = &x;
        }
        Army* h = mk(g, s->x, s->y, side->index, armytype::HERO);
        side->quest.reset();
        auto q = quest::assign(g, *side, h, false, flying);
        // a retrieve quest is not certain; take the first try that is one
        for (int i = 0; i < 200 && q && q->type != quest::RETRIEVE_ITEM; i++) {
            side->quest.reset();
            q = quest::assign(g, *side, h, false, flying);
        }
        if (!q || q->type != quest::RETRIEVE_ITEM) {
            Ok(false, "an item quest was given");
            continue;
        }
        Site* at = nullptr;
        for (auto& x : g.map->sites)
            if (x.content == site::ITEM && x.item == q->item->index) at = &x;
        int ring = 0, seen = 0;
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (std::max(std::abs(dx), std::abs(dy)) == 2) {
                    ring++;
                    if (game::seen(g, side->index, at->x + dx, at->y + dy)) seen++;
                }
        if (flying)
            Eq(seen, ring, "a flier's quest uncovers two tiles round the ruin");
        else
            Ok(game::seen(g, side->index, at->x + 1, at->y + 1), "a walker's uncovers one");
    }
}

// What plays when. The OPL synthesis is checked in the sound step.
static void TestSound() {
    printf("sound\n");
    auto files = uidata::strings(kData + "/DATA/FILE.DAT");
    auto first = [](int) { return 1; };
    auto [name, loop] = cues::song(files, cues::TITLE, {}, first);
    Eq(name, "STARTUP.XMI", "the start screens play STARTUP.XMI");
    Ok(loop, "and it starts again when it ends");
    Side human;
    human.computer = false;
    human.alive = true;
    std::tie(name, loop) = cues::song(files, cues::COMPUTER, {&human}, first);
    Eq(name, "INT2.XMI", "a computer's turn beside a human plays FILE.DAT group 11");
    Ok(!loop, "once");
    Eq(cues::song(files, cues::BEGIN, {}, first).first, "INT22.XMI", "the war begins to INT22");
    Game g;
    g.map = std::make_unique<Map>();
    g.turn = 1;
    Side side;
    side.index = 0;
    side.gold = 500;
    g.map->cities.resize(6);
    for (auto& c : g.map->cities) c.ownerIndex = 0;
    Eq(cues::advisor(g, side, first), NONE, "the advisor never speaks on turn 1");
    Eq(side.advisor.mark, 5, "but he has marked six cities down as five");
    g.turn = 2;
    Eq(cues::advisor(g, side, first), NONE, "nothing new, not a seventh turn: silence");
    for (int i = 0; i < 5; i++) g.map->cities[i].ownerIndex = 1;
    Eq(cues::advisor(g, side, first), 40, "down to one city: 'Thy sorry efforts...' (VLOSE05)");
    Eq(side.advisor.dir, 2, "and he remembers saying so");
    for (int i = 0; i < 5; i++) g.map->cities[i].ownerIndex = 0;
    Eq(cues::advisor(g, side, first), 47, "back to six: 'Thou art doing well... so far!'");
    g.turn = 7;
    side.gold = 50;
    Eq(cues::advisor(g, side, first), 54, "on a seventh turn, short of gold: VGOLD00");
}

static void TestModernAi() {
    printf("modern learnable AI & diplomacy\n");

    // 1. All 27 personality profiles exist and have bounded 10D traits
    for (int lv = 0; lv < 3; lv++) {
        for (int c = 0; c < 9; c++) {
            const auto& p = ai::personality::profileFor(lv, c);
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
        Side* s0 = g.map->side(0);
        s0->computer = true;
        s0->classicAi = false;
        int ux = NONE, uy = NONE;
        for (int y = 5; y < g.map->height - 5 && ux == NONE; y++) {
            for (int x = 5; x < g.map->width - 5 && ux == NONE; x++) {
                if (!game::seen(g, 0, x, y) && scn::terrainAt(*g.map, x, y) == movement::PLAIN) {
                    ux = x;
                    uy = y;
                }
            }
        }
        Ok(ux != NONE, "found unseen plain tile under hiddenMap");
        auto grid = movement::grid(g, 0);
        auto prep = movement::prepare(g, grid, 0, movement::LAND, 0, 0);
        int idx = uy * g.map->width + ux;
        Eq(prep[idx], movement::SHUT, "modern AI cannot pathfind through unseen fog tiles");
    }

    // 3. ALLIED shared fog-of-war vision, truce timers, tribute transfer, and joint war pact
    {
        auto gp = NewGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}}));
        Game& g = *gp;
        int tx = 50, ty = 50;
        Eq(game::seen(g, 0, tx, ty), false, "side 0 has not seen (50,50)");
        game::reveal(g, 1, tx, ty, false);
        Eq(game::seen(g, 1, tx, ty), true, "side 1 sees (50,50)");
        Eq(game::seen(g, 0, tx, ty), false, "unallied side 0 still cannot see side 1's tile");

        int gold0_before = g.map->sides[0].gold;
        int gold1_before = g.map->sides[1].gold;
        diplomacy::propose(g, 0, 1, diplomacy::PEACE);
        diplomacy::proposeAlliance(g, 0, 1, true);
        diplomacy::setGoldOffer(g, 0, 1, 50);
        diplomacy::setWarTarget(g, 0, 1, 2);

        diplomacy::propose(g, 1, 0, diplomacy::PEACE);
        diplomacy::proposeAlliance(g, 1, 0, true);
        diplomacy::apply(g, g.map->sides[1]);

        Eq(diplomacy::isAllied(g, 0, 1), true, "sides 0 and 1 are now ALLIED");
        Eq(diplomacy::truceTurns(g, 0, 1), 15, "15-turn alliance truce active");
        Eq(g.map->sides[0].gold, gold0_before - 50, "tribute deducted from proposer");
        Eq(g.map->sides[1].gold, gold1_before + 50, "tribute credited to recipient");
        Eq(diplomacy::state(g, 1, 2), diplomacy::WAR, "joint war target activated");
        Eq(game::seen(g, 0, tx, ty), true, "ALLIED side 0 now shares side 1's fog-of-war vision");

        auto bd = diplomacy::computeAttitude(g, 1, 0);
        Ok(!bd.factors.empty() && !bd.whatItTakes.empty(),
           "attitude ledger produces itemized factors and forecast");
    }

    // 4. SideBelief under viewEnemies == 0 and RNG preservation in estimateCombat
    {
        auto gp = NewGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Game& g = *gp;
        Side& s0 = g.map->sides[0];
        s0.computer = true;
        s0.classicAi = false;
        ai::belief::update(g, s0);
        const SideBelief& b = ai::belief::ensure(s0);
        for (City* c : b.visibleCities) {
            Ok(ai::belief::isCityVisible(g, 0, *c),
               "all visibleCities in SideBelief are genuinely seen by side 0");
        }
        uint32_t rng_before = g.rng.state;
        std::vector<Army*> my_armies = game::sideArmies(g, s0);
        if (!my_armies.empty() && !b.visibleCities.empty()) {
            std::vector<Army*> test_stack = {my_armies[0]};
            auto est = ai::belief::estimateCombat(g, s0, test_stack, b.visibleCities[0]->x,
                                                  b.visibleCities[0]->y);
            Ok(est.winProb >= 0.0F && est.winProb <= 1.0F,
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
        s0.classicAi = false;
        ai::belief::update(g, s0);
        auto ctx = ai::ml::buildContext(g, s0);
        float v = net.evaluateValue(ctx, ai::ml::buildCriticFeatures(g, s0));
        Ok(std::isfinite(v) && std::abs(v) < 5.0F,
           "linear value head gives a finite, small estimate at random init");

        auto flat1 = net.flatten();
        Eq(static_cast<int>(flat1.size()), ai::ml::PolicyNetwork::TOTAL_PARAMS,
           "flattened parameter count matches TOTAL_PARAMS");
        std::filesystem::path tmp_path =
                std::filesystem::temp_directory_path() / "w2_test_weights.json";
        Ok(net.saveJson(tmp_path), "saved PolicyNetwork JSON");
        ai::ml::PolicyNetwork net2;
        Ok(net2.loadJson(tmp_path), "loaded PolicyNetwork JSON");
        std::filesystem::remove(tmp_path);
        auto flat2 = net2.flatten();
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
            if (s.inUse) s.classicAi = false;
        }
        Side* cur = game::begin(g);
        int turns_played = 0;
        int worst_stack = 0;
        while (cur && !g.over && g.turn <= 8) {
            ai::playTurn(g, *cur);
            turns_played++;
            std::map<int, int> per_tile;
            for (Army* a : g.armies) {
                if (a->x != NONE && !a->transit)
                    worst_stack = std::max(worst_stack, ++per_tile[a->y * g.map->width + a->x]);
            }
            cur = game::endTurn(g);
        }
        Ok(turns_played >= 4,
           "modern learnable AI plays multiple turns cleanly under hiddenMap + viewEnemies=0");
        Ok(worst_stack <= rules::MAX_STACK, "modern AI never puts more than 8 armies on one tile");
    }

    // Tabula rasa initialization, honest partial-visibility belief, enemy distance, and island
    // frontiers
    {
        ai::ml::PolicyNetwork fresh_net;
        float max_skip = 0.0F;
        for (float w : fresh_net.missionNet.wSkip) max_skip = std::max(max_skip, std::abs(w));
        Ok(max_skip <= 0.25F, "PolicyNetwork starts tabula rasa with zero hand-coded skip priors");

        auto gp =
                NewGame("ERYTHEA",
                        SeedWith(7, {{"hiddenMap", 1}, {"viewEnemies", 0}, {"neutralCities", 2}}));
        Game& g = *gp;
        Side& s7 = g.map->sides[7];
        s7.computer = true;
        s7.classicAi = false;

        // Reveal a neutral city's top-left (x, y) tile while leaving (x + 1, y + 1) unseen
        City* target_neutral = nullptr;
        for (auto& c : g.map->cities) {
            if (c.ownerIndex == NONE && !c.razed && !game::seen(g, 7, c.x + 1, c.y + 1)) {
                target_neutral = &c;
                break;
            }
        }
        Ok(target_neutral != nullptr, "found unseen neutral city on ERYTHEA");
        if (target_neutral) {
            // Remove any garrison on (x, y) so (x, y) is visibly empty while (x+1, y+1) is in fog
            for (Army* a : game::armiesAt(g, target_neutral->x, target_neutral->y)) {
                a->x = NONE;
                a->y = NONE;
            }
            g.explored[7][target_neutral->y * g.map->width + target_neutral->x] = true;
            Eq(game::seen(g, 7, target_neutral->x, target_neutral->y), true,
               "top-left neutral city tile is seen");
            Eq(game::seen(g, 7, target_neutral->x + 1, target_neutral->y + 1), false,
               "bottom-right neutral city tile is unseen");
            ai::belief::update(g, s7);
            std::vector<Army*> my_armies = game::sideArmies(g, s7);
            Ok(!my_armies.empty(), "side 7 has starting army");
            auto est = ai::belief::estimateCombat(g, s7, {my_armies[0]}, target_neutral->x,
                                                  target_neutral->y);
            Eq(est.estimatedDefenderCount, 0,
               "partially-seen neutral city with visibly empty (x,y) does not fabricate 3 phantom "
               "defenders");
        }

        // With no rival cities or stacks visible to side 7, cityNearestEnemyDist should not treat
        // fog/neutrals as enemies
        const SideBelief& b7 = ai::belief::ensure(s7);
        if (s7.capital && s7.capital->index < static_cast<int>(b7.cityNearestEnemyDist.size())) {
            Ok(b7.cityNearestEnemyDist[s7.capital->index] > 50.0F,
               "cityNearestEnemyDist ignores neutral cities and fog frontiers when no rivals are "
               "visible");
        }

        // Reveal only side 7's home island land tiles (x < 27, y < 32) while surrounding sea
        // remains unseen
        g.explored[7].assign(static_cast<size_t>(g.map->width) * g.map->height, false);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 27; x++) {
                int terr = scn::terrainAt(*g.map, x, y);
                if (terr != movement::WATER && terr != movement::SHORE) {
                    game::reveal(g, 7, x, y, false);
                }
            }
        }
        ai::belief::update(g, s7);
        bool has_water_frontier = false;
        for (const auto& fc : ai::belief::ensure(s7).fogFrontiers) {
            if (fc.isWater) has_water_frontier = true;
        }
        Ok(has_water_frontier,
           "island faction generates coastal/water fog frontiers after exploring local island "
           "land");

        // Analytical backpropagation finite-difference gradient check on ResidualMLP
        ai::ml::MissionInVec test_in{};
        for (size_t i = 0; i < test_in.size(); i++) {
            test_in[i] = std::sin(static_cast<float>(i + 1) * 0.37F);
        }
        std::array<float, ai::ml::MISSION_OUT_DIM> d_out = {0.75F, -0.45F};
        ai::ml::PolicyNetwork::MissionNet grad_analytical{};
        grad_analytical.Zero();
        fresh_net.missionNet.Backward(test_in, d_out, grad_analytical);
        std::vector<float> flat_grad;
        grad_analytical.ToFlat(flat_grad);
        std::vector<float> base_params;
        fresh_net.missionNet.ToFlat(base_params);

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
            if (c.ownerIndex == NONE && !ai::belief::isCityVisible(g, 7, c)) {
                unseen_neutral = &c;
                break;
            }
        }
        Ok(unseen_neutral != nullptr, "found an unseen neutral castle on hidden-map ERYTHEA");
        if (unseen_neutral) {
            float phi_hidden = ai::ml::computeEmpireScore(g, 7);
            game::reveal(g, 7, unseen_neutral->x, unseen_neutral->y, false);
            float phi_revealed = ai::ml::computeEmpireScore(g, 7);
            int prev_owner = unseen_neutral->ownerIndex;
            unseen_neutral->ownerIndex = 7;
            float phi_captured = ai::ml::computeEmpireScore(g, 7);
            unseen_neutral->ownerIndex = prev_owner;

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
        for (Army* a : game::sideArmies(g, s7)) {
            a->moves = a->maxMoves;
            a->aiMoved = false;
        }
        ai::ml::RolloutSampler rec_sampler;
        ai::ml::TurnStepRecord rec_turn;
        rec_sampler.currentTurn = &rec_turn;
        ai::modern::playTurn(g, s7, nullptr, &fresh_net, &rec_sampler);
        Ok(!rec_turn.missionSteps.empty() && !rec_turn.missionSteps[0].candidates.empty(),
           "RolloutSampler records joint (target, coalition) MissionStepRecord candidates");

        // Multiplicity-balanced sampling gives equal family probability to 1 Hold vs 40 Fog options
        // at zero utility
        std::vector<float> zero_logits(41, 0.0F);
        std::vector<float> test_log_mult(41, std::log(40.0F));
        test_log_mult[0] = 0.0F;
        ai::ml::RolloutSampler mult_sampler;
        mult_sampler.rngState = 12345U;
        mult_sampler.temperature = 1.0F;
        int hold_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::selectFromLogits(zero_logits, &mult_sampler, nullptr, &test_log_mult) ==
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
        Eq(ai::ml::selectFromLogits(zero_logits, nullptr, nullptr, &test_log_mult), 0,
           "greedy selectFromLogits applies the multiplicity prior (argmax of u - logMult)");
        std::vector<float> fog_favoured = zero_logits;
        fog_favoured[7] =
                std::log(40.0F) + 0.1F;  // one fog option slightly above Hold in policy logits
        Eq(ai::ml::selectFromLogits(fog_favoured, nullptr, nullptr, &test_log_mult), 7,
           "greedy selectFromLogits picks a fog option once its u - logMult exceeds Hold's");
        mult_sampler.temperature = 0.35F;
        int fog_seven_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::selectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 7)
                fog_seven_picks++;
        }
        hold_picks = 0;
        for (int trial = 0; trial < 2000; trial++) {
            if (ai::ml::selectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 0)
                hold_picks++;
        }
        Ok(fog_seven_picks > hold_picks,
           "the sampled mode at low temperature agrees with the greedy choice");

        // Binary decisions: greedy returns the mode, sampling follows sigmoid(logit / T)
        Eq(ai::ml::selectBinary(0.3F, nullptr), true, "greedy selectBinary is logit > 0");
        Eq(ai::ml::selectBinary(-0.3F, nullptr), false,
           "greedy selectBinary is false for logit < 0");
        mult_sampler.temperature = 1.0F;
        int yes = 0;
        for (int trial = 0; trial < 2000; trial++)
            yes += ai::ml::selectBinary(std::log(3.0F), &mult_sampler) ? 1 : 0;
        Ok(yes >= 1400 && yes <= 1600,
           "sampled selectBinary with logit ln 3 says yes about 75% of the time");

        // Spectator visibility: AI-vs-AI games reveal the whole map to the viewer while keeping
        // per-AI fog intact
        {
            GameOpts human_opts = SeedWith(11, {{"hiddenMap", 1}});
            for (int i = 0; i < 8; i++) human_opts.sides[i].computer = (i > 0);
            auto gp_human = NewGame("ERYTHEA", human_opts);
            Game& gh = *gp_human;
            Eq(game::hasActiveHuman(gh), true, "game with human player reports active human");
            Eq(game::viewerSide(gh, gh.sides[0]), gh.sides[0]->index,
               "human viewerSide uses human side index");
            Eq(game::seen(gh, game::viewerSide(gh, gh.sides[0]), 0, 0), false,
               "human viewer still has fog-of-war on unseen corner");

            GameOpts ai_opts = SeedWith(11, {{"hiddenMap", 1}});
            for (int i = 0; i < 8; i++) ai_opts.sides[i].computer = true;
            auto gp_ai = NewGame("ERYTHEA", ai_opts);
            Game& ga = *gp_ai;
            Eq(game::hasActiveHuman(ga), false, "all-AI game reports no active human");
            Eq(game::viewerSide(ga, ga.sides[0]), NONE,
               "all-AI viewerSide returns NONE (spectator full map)");
            Eq(game::seen(ga, game::viewerSide(ga, ga.sides[0]), 0, 0), true,
               "spectator viewer sees unseen corner (0,0)");
            Eq(game::fogCell(ga, game::viewerSide(ga, ga.sides[0]), 0, 0), NONE,
               "spectator viewer draws no fog shroud");
            Eq(game::seen(ga, ga.sides[0]->index, 0, 0), false,
               "individual AI side still obeys its own Hidden Map fog");
        }

        // Classic AI vs Modern AI default toggle, mid-game switch, and save/load preservation
        {
            bool prev_default = ai::defaultClassicAi();
            ai::setDefaultClassicAi(true);
            auto gp_classic = NewGame("ERYTHEA", Seed(13));
            Eq(gp_classic->sides[0]->classicAi, true,
               "defaultClassicAi(true) initializes sides with Classic 1993 AI");
            std::string classic_json = save::encode(*gp_classic);

            ai::setDefaultClassicAi(false);
            auto gp_modern = NewGame("ERYTHEA", Seed(13));
            Eq(gp_modern->sides[0]->classicAi, false,
               "defaultClassicAi(false) initializes sides with Modern RL AI");
            std::string modern_json = save::encode(*gp_modern);

            // Decoding a Classic AI save while the app default is Modern AI must keep Classic AI
            auto decoded_classic = save::decode(classic_json, kData);
            Eq(decoded_classic->sides[0]->classicAi, true,
               "Classic AI save stays Classic AI even when app default is Modern AI");

            // Decoding a Modern AI save while the app default is Classic AI must keep Modern AI
            ai::setDefaultClassicAi(true);
            auto decoded_modern = save::decode(modern_json, kData);
            Eq(decoded_modern->sides[0]->classicAi, false,
               "Modern AI save stays Modern AI even when app default is Classic AI");

            // Mid-game live switch between Classic AI and Modern AI: alternate engines
            // turn by turn for several rounds and check the game stays legal
            for (Side* s : decoded_modern->sides) s->computer = true;
            Side* cur = game::begin(*decoded_modern);
            Ok(cur != nullptr, "mid-game switch test game begins");
            int switched_turns = 0, worst_tile = 0;
            bool positions_valid = true;
            while (cur && !decoded_modern->over && decoded_modern->turn <= 6) {
                bool classic = (decoded_modern->turn % 2 == 0);
                for (auto& s : decoded_modern->map->sides) s.classicAi = classic;
                ai::playTurn(*decoded_modern, *cur);
                switched_turns++;
                std::map<int, int> per_tile;
                for (Army* a : decoded_modern->armies) {
                    if (a->x == NONE || a->transit) continue;
                    if (a->x < 0 || a->y < 0 || a->x >= decoded_modern->map->width ||
                        a->y >= decoded_modern->map->height) {
                        positions_valid = false;
                    } else {
                        worst_tile = std::max(worst_tile,
                                              ++per_tile[a->y * decoded_modern->map->width + a->x]);
                    }
                }
                cur = game::endTurn(*decoded_modern);
            }
            Ok(switched_turns >= 20,
               "Classic and Modern AI alternate over several rounds without stalling");
            Ok(positions_valid, "every army stays on the map while engines alternate");
            Ok(worst_tile <= rules::MAX_STACK, "no tile exceeds 8 armies while engines alternate");

            ai::setDefaultClassicAi(prev_default);
        }

        // Small-map scaling (activeSides + cropMargin) and thread-local in-memory random maps
        {
            Rng map_rng(4242.0);
            randommap::Options ro;
            ro.dataDir = kData;
            ro.rng = &map_rng;
            ro.sliders = {3, 3, 2, 3};
            auto rfiles = randommap::generateNow(ro);

            game::NewGameOptions so;
            so.seed = 4242.0;
            so.memoryFiles = &rfiles;
            so.activeSides = 2;
            so.cropMargin = 14;
            so.options = {{"hiddenMap", 1}, {"diplomacy", 0}, {"neutralCities", 1}, {"quests", 1}};
            for (int i = 0; i < 8; i++) {
                so.sides[i].computer = true;
                so.sides[i].level = 2;
            }
            auto gp_small = game::newGame(kData, "RANDOM", so);
            Game& gs = *gp_small;
            Eq(static_cast<int>(gs.sides.size()), 2,
               "cropped 1v1 random map keeps exactly 2 active sides");
            Ok(gs.map->cities.size() >= 2 && gs.map->cities.size() < 45,
               "cropped 1v1 random map trims city count");
            bool cities_in_box = true;
            for (const auto& c : gs.map->cities) {
                if (c.x < gs.map->activeX0 || c.x + 1 > gs.map->activeX1 ||
                    c.y < gs.map->activeY0 || c.y + 1 > gs.map->activeY1) {
                    cities_in_box = false;
                }
            }
            Ok(cities_in_box, "all cropped cities sit inside the active island box");
            Eq(scn::terrainAt(*gs.map, 0, 0), movement::WATER,
               "outer cropped border tile (0,0) is converted to water");
            Eq(game::seen(gs, gs.sides[0]->index, 0, 0), true,
               "outer cropped water is pre-marked seen under Hidden Map");

            gs.sides[0]->classicAi = false;
            gs.sides[1]->classicAi = true;
            Side* cur = game::begin(gs);
            while (cur && !gs.over && !gs.won && gs.turn <= 10) {
                if (cur->alive) ai::playTurn(gs, *cur);
                cur = game::endTurn(gs);
            }
            Ok(gs.turn > 5 || gs.won || gs.over,
               "1v1 cropped random map runs Modern vs Classic cleanly");
        }

        // 5-head PolicyNetwork (v6: 30-wide context, privileged critics; 9700 params),
        // v5/v4/v3 compatibility, and temperature-multiplicity invariant
        {
            namespace ml = ai::ml;
            using PN = ml::PolicyNetwork;
            Eq(PN::TOTAL_PARAMS, 9700, "v6 PolicyNetwork has 9700 parameters");
            Eq(PN::LEGACY_V5_PARAMS, 8866, "legacy v5 parameter count remains 8866");
            Eq(PN::LEGACY_V4_PARAMS, 8066, "legacy v4 parameter count remains 8066");
            Eq(PN::LEGACY_V3_PARAMS, 6729, "legacy v3 parameter count remains 6729");
            ml::PolicyNetwork net;
            Ok(net.loadJson("cpp/ai_weights.json"), "installed cpp/ai_weights.json loads cleanly");
            std::vector<float> flat = net.flatten();
            Eq(static_cast<int>(flat.size()), 9700,
               "flattened parameter vector matches TOTAL_PARAMS");

            // Legacy flat vectors synthesised from a random v6 network by dropping
            // the input columns an older format did not have
            ml::PolicyNetwork src;
            src.initRandomWeights(7);
            auto added_ctx = [](int c) { return c >= ml::LEGACY_GLOBAL_DIM && c < ml::GLOBAL_DIM; };
            auto narrow = [](const auto& mlp, auto dropped, std::vector<float>& out) {
                using M = std::decay_t<decltype(mlp)>;
                for (int r = 0; r < M::OUT_DIM; r++)
                    for (int c = 0; c < M::IN_DIM; c++)
                        if (!dropped(c)) out.push_back(mlp.wSkip[r * M::IN_DIM + c]);
                for (int r = 0; r < M::H1_DIM; r++)
                    for (int c = 0; c < M::IN_DIM; c++)
                        if (!dropped(c)) out.push_back(mlp.w1[r * M::IN_DIM + c]);
                out.insert(out.end(), mlp.b1.begin(), mlp.b1.end());
                out.insert(out.end(), mlp.w2.begin(), mlp.w2.end());
                out.insert(out.end(), mlp.b2.begin(), mlp.b2.end());
                out.insert(out.end(), mlp.w3.begin(), mlp.w3.end());
                out.insert(out.end(), mlp.b3.begin(), mlp.b3.end());
            };
            auto critic_dropped = [&](int in) {
                return [&, in](int c) { return added_ctx(c) || c >= in - ml::CRITIC_DIM; };
            };
            std::vector<float> v5, v4;
            for (auto* out : {&v5, &v4}) {
                narrow(src.diploNet, added_ctx, *out);
                narrow(src.missionNet, added_ctx, *out);
                narrow(src.econNet, added_ctx, *out);
            }
            narrow(src.valueNet, added_ctx, v5);
            narrow(src.stackValueNet, added_ctx, v5);
            narrow(src.valueNet, critic_dropped(ml::VALUE_IN_DIM), v4);
            size_t v3_size = v4.size();
            narrow(src.stackValueNet, critic_dropped(ml::STACK_VALUE_IN_DIM), v4);
            Eq(static_cast<int>(v5.size()), 8866, "synthesised v5 vector has 8866 parameters");
            Eq(static_cast<int>(v4.size()), 8066, "synthesised v4 vector has 8066 parameters");
            Eq(static_cast<int>(v3_size), 6729, "synthesised v3 prefix has 6729 parameters");

            // Inputs with the columns a format lacked set to zero: the migrated
            // network must compute exactly what the source network computes
            ml::ContextVec cx{};
            for (int i = 0; i < ml::CONTEXT_DIM; i++)
                cx[i] = added_ctx(i) ? 0.0F : 0.1F * static_cast<float>(i % 7) - 0.3F;
            ml::CriticVec some_crit{}, zero_crit{};
            for (int i = 0; i < ml::CRITIC_DIM; i++)
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
            m5.initRandomWeights(99);
            Ok(!m5.flatten().empty(), "scratch network");
            m5.unflattenLegacy(v5);
            Ok(close(m5.scoreMission(cx, sv, tv).utility, src.scoreMission(cx, sv, tv).utility),
               "v5 MissionNet migrates function-preserving");
            Ok(close(m5.scoreEconomy(cx, ev), src.scoreEconomy(cx, ev)),
               "v5 EconNet migrates function-preserving");
            Ok(close(m5.scoreDiplomacy(cx, rv).stanceLogits[3],
                     src.scoreDiplomacy(cx, rv).stanceLogits[3]),
               "v5 DiploNet migrates function-preserving");
            Ok(close(m5.evaluateValue(cx, some_crit), src.evaluateValue(cx, some_crit)),
               "v5 ValueNet (with privileged inputs) migrates function-preserving");
            Ok(close(m5.evaluateStackValue(ml::packStackValueInput(cx, sv, some_crit)),
                     src.evaluateStackValue(ml::packStackValueInput(cx, sv, some_crit))),
               "v5 StackValueNet migrates function-preserving");
            ml::ContextVec cx_new = cx;
            cx_new[ml::LEGACY_GLOBAL_DIM] = 0.7F;
            Ok(close(m5.scoreMission(cx_new, sv, tv).utility, m5.scoreMission(cx, sv, tv).utility),
               "migrated network ignores the added context features until trained");

            ml::PolicyNetwork m4;
            m4.unflattenLegacy(v4);
            Ok(close(m4.scoreMission(cx, sv, tv).utility, src.scoreMission(cx, sv, tv).utility),
               "v4 MissionNet migrates function-preserving");
            Ok(close(m4.evaluateValue(cx, some_crit), src.evaluateValue(cx, zero_crit)),
               "v4 ValueNet ignores the privileged inputs until trained");
            Ok(close(m4.evaluateStackValue(ml::packStackValueInput(cx, sv, some_crit)),
                     src.evaluateStackValue(ml::packStackValueInput(cx, sv, zero_crit))),
               "v4 StackValueNet ignores the privileged inputs until trained");

            // v3: no StackValueNet in the file -> zero-initialised
            std::vector<float> v3(v4.begin(), v4.begin() + static_cast<long>(v3_size));
            ml::PolicyNetwork m3;
            m3.initRandomWeights(99);
            m3.unflattenLegacy(v3);
            ml::StackValueInVec zero_sv{};
            Eq(m3.evaluateStackValue(zero_sv), 0.0F,
               "legacy v3 weights zero-initialize StackValueNet");

            // At T = 0.5 with equal utilities (0.0), a singleton family and a 20-item family
            // with logMult = ln(20) receive equal 50/50 probability mass (z_i = u_i/T - logMult_i)
            std::vector<float> u(21, 0.0F);
            std::vector<float> lm(21, std::log(20.0F));
            lm[0] = 0.0F;
            ai::ml::RolloutSampler samp;
            samp.rngState = 1234567U;
            samp.temperature = 0.5F;
            int picks0 = 0;
            for (int i = 0; i < 2000; i++) {
                if (ai::ml::selectFromLogits(u, &samp, nullptr, &lm) == 0) picks0++;
            }
            Ok(picks0 > 880 && picks0 < 1120,
               "multiplicity prior stays balanced across families at T = 0.5");
        }
    }
}

// --------------------------------------------------------------------- main

int main() {
    if (!fileExists(kData + "/TERRAIN0/ARMYTYPE.DAT")) {
        printf("cannot find the game data in %s: run from the repository root\n", kData.c_str());
        return 1;
    }
    TestDice();
    TestArmyTypes();
    TestProduction();
    TestBugFlags();
    for (auto& s : kScenarios)
        if (fileExists(kData + "/" + s + "/" + s + ".SCN")) TestGame(s);
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
    if (fileExists(kData + "/TUTORIA/TUTORIA.SCN")) TestTutorialHero();
    TestVerified();
    TestSound();
    TestModernAi();
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
