/**
 * @file
 * @brief GoogleTest suite for game setup, production, vectoring, reports, history, disbanding, and
 * turn/end-game rules.
 */
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/quest.hpp"
#include "warlords/report.hpp"
#include "warlords/rng.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"

namespace w2 {
namespace {

using namespace w2::test;

TEST(ProductionTest, CitySlotsNudgeBoundsBestSlotAndCityDefence) {
    Types types;
    armytype::Load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    Rng rng(42);
    auto slots = rules::CitySlots({4, 11, 2}, types, rng);
    ASSERT_EQ(slots.size(), 3U);
    for (size_t i = 1; i < slots.size(); ++i) EXPECT_GE(slots[i].price, slots[i - 1].price);

    bool ok_bounds = true;
    for (int seed = 1; seed <= 2000; ++seed) {
        Rng seeded_rng(seed);
        for (const auto& slot : rules::CitySlots({1, 2, 3, 4}, types, seeded_rng)) {
            const ArmyType* base = types.ById(slot.type);
            if (slot.strength < 1 || slot.strength > 9 || slot.strength < base->strength - 1 ||
                slot.strength > base->strength + 1 || slot.move < 6 || slot.move > base->move + 4 ||
                slot.time < 1 || slot.time > base->time + 1) {
                ok_bounds = false;
            }
        }
    }
    EXPECT_TRUE(ok_bounds);

    bool any_flier = false;
    for (const auto& slot : slots) {
        if (types.ById(slot.type)->flies) any_flier = true;
    }
    EXPECT_EQ(rules::BestSlot(slots, 4, types, false) != kNone, any_flier);
    EXPECT_NE(rules::BestSlot(slots, 6, types, false), kNone);
    EXPECT_NE(rules::BestSlot(slots, 3, types, false), kNone);

    EXPECT_EQ(rules::CityDefence(2), 1);
    EXPECT_EQ(rules::CityDefence(3), 2);
    EXPECT_EQ(rules::CityDefence(4), 2);

    Slot sample_slot{4, "x", 8, 2, 8, 12, 0};
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, true).strength, 9);
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, false).strength, 8);
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, false).upkeep, 4);
}

/** @brief Parameterized suite checking initial scenario invariants across all shipped scenarios. */
class GameScenarioTest : public ScenarioTest {};

TEST_P(GameScenarioTest, InitialStateGarrisonsAndSeedDeterminism) {
    const std::string& scenario = GetParam();
    auto game_ptr = SetupGame(scenario, Seed(3));
    Game& game = *game_ptr;
    EXPECT_GE(game.sides.size(), 2U);
    EXPECT_EQ(game.armies.size(), game.map->cities.size());

    for (Side* side : game.sides) {
        auto owned = game::SideCities(game, *side);
        ASSERT_EQ(owned.size(), 1U) << side->name;
        EXPECT_EQ(owned[0]->index, side->capital->index) << side->name;
    }
    for (const auto& city : game.map->cities) {
        for (const auto& slot : city.slots) EXPECT_NE(slot.type, armytype::kNavy) << city.name;
        EXPECT_EQ(city.defence, rules::CityDefence(static_cast<int>(city.slots.size())));
        EXPECT_LE(city.slots.size(), 4U);
    }
    std::map<int, int> per_tile;
    for (const Army* army : game.armies) {
        EXPECT_NE(game.types.ById(army->type), nullptr);
        EXPECT_EQ(army->moves, 0);
        EXPECT_LE(++per_tile[army->y * game.map->width + army->x], rules::kMaxStack);
    }

    auto rebuilt = SetupGame(scenario, Seed(3));
    ASSERT_EQ(rebuilt->armies.size(), game.armies.size());
    for (size_t i = 0; i < game.armies.size(); ++i) {
        EXPECT_EQ(rebuilt->armies[i]->type, game.armies[i]->type);
        EXPECT_EQ(rebuilt->armies[i]->strength, game.armies[i]->strength);
    }
}

// NOLINTNEXTLINE(readability-identifier-length)
INSTANTIATE_TEST_SUITE_P(AllScenarios, GameScenarioTest, ::testing::ValuesIn(kScenarios),
                         ScenarioParamName);

TEST(TurnLoopTest, MoveRefreshCarryoverProductionAndCitylessElimination) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(5));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    ASSERT_NE(side, nullptr);
    for (const Army* army : game::SideArmies(game, *side)) EXPECT_EQ(army->moves, army->max_moves);
    int initial_gold = side->gold;
    EXPECT_EQ(side->income, side->capital->income);
    EXPECT_EQ(side->gold, std::max(0, initial_gold));

    Army* first_army = game::SideArmies(game, *side)[0];
    first_army->done = true;
    first_army->offered = true;
    first_army->fortified = true;
    AdvanceRounds(game, 1);
    EXPECT_FALSE(first_army->done);
    EXPECT_FALSE(first_army->offered);
    EXPECT_TRUE(first_army->fortified);
    first_army->fortified = false;

    first_army->moves = 5;
    int base_moves = first_army->max_moves;
    AdvanceRounds(game, 1);
    EXPECT_EQ(first_army->moves, base_moves + rules::kMoveCarry);
    first_army->moves = 1;
    AdvanceRounds(game, 1);
    EXPECT_EQ(first_army->moves, base_moves + 1);

    City* city = side->capital;
    game::SetProduction(game, *city, 0);
    size_t armies_before = game::SideArmies(game, *side).size();
    AdvanceRounds(game, static_cast<size_t>(city->slots[0].time));
    EXPECT_EQ(game::SideArmies(game, *side).size(), armies_before + 1);
    Army* built = nullptr;
    for (Army* army : game::SideArmies(game, *side)) {
        if (army->home_city == city->index && army != first_army) built = army;
    }
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(built->upkeep, city->slots[0].cost / 2);
    EXPECT_EQ(built->x, city->x);

    Side* victim = game.sides.back();
    for (City* victim_city : game::SideCities(game, *victim)) victim_city->owner_index = kNone;
    victim->gold = 500;
    game.diplomacy.state[static_cast<size_t>(victim->index) * 8] = diplomacy::kWar;
    EXPECT_FALSE(game::SideArmies(game, *victim).empty());
    int round_of = game.turn;
    while (game.turn == round_of) {
        EXPECT_TRUE(victim->alive);
        EndTurn(game);
    }
    EXPECT_FALSE(victim->alive);
    EXPECT_TRUE(game::SideArmies(game, *victim).empty());
    EXPECT_EQ(victim->gold, 0);
    EXPECT_EQ(diplomacy::State(game, victim->index, 0), diplomacy::kIntermediate);
    ASSERT_FALSE(game.ending.fallen.empty());
    EXPECT_EQ(game.ending.fallen[0].side, victim);

    auto second_game = SetupGame("ERYTHEA", Seed(6));
    game::Begin(*second_game);
    EXPECT_EQ(second_game->turn, 1);
    AdvanceRounds(*second_game, 1);
    EXPECT_EQ(second_game->turn, 2);
}

TEST(VectoringTest, TwoTurnTransitFourSenderCapAndSelfLift) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(9));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* from = side->capital;
    City* dest = FindNeutralCity(game, 1);
    ASSERT_NE(dest, nullptr);
    dest->owner_index = side->index;
    for (Army* army : game::ArmiesAt(game, dest->x, dest->y)) army->owner = side->index;
    game::SetProduction(game, *from, 0);
    game::Vector(game, *from, dest);
    size_t at_dest = game::ArmiesAt(game, dest->x, dest->y).size();
    AdvanceRounds(game, static_cast<size_t>(from->slots[0].time + 2));
    EXPECT_GT(game::ArmiesAt(game, dest->x, dest->y).size(), at_dest);

    Army* sent = nullptr;
    for (int i = 0; i < 10 && sent == nullptr; ++i) {
        AdvanceRounds(game, 1);
        bool any_sent = false;
        for (const auto& entry : side->produced) {
            if (entry.kind == "sent") any_sent = true;
        }
        if (!any_sent) continue;
        for (Army* army : game.armies) {
            if (army->transit && army->owner == side->index && army->transit->dest == dest->index &&
                army->transit->turns == 2) {
                sent = army;
            }
        }
    }
    ASSERT_NE(sent, nullptr);
    AdvanceRounds(game, 1);
    EXPECT_TRUE(sent->transit && sent->transit->turns == 1);
    AdvanceRounds(game, 1);
    EXPECT_TRUE(!sent->transit && sent->x != kNone);

    game::SetProduction(game, *from, kNone);
    EXPECT_EQ(from->vector_to, kNone);

    std::vector<City*> senders;
    for (auto& city : game.map->cities) {
        if (&city != dest && senders.size() < 5) {
            city.owner_index = side->index;
            senders.push_back(&city);
        }
    }
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(game::Vector(game, *senders[i], dest));
    EXPECT_FALSE(game::Vector(game, *senders[4], dest));
    EXPECT_EQ(senders[4]->vector_to, kNone);
    EXPECT_EQ(game::VectoredTo(game, dest).size(), 4U);
    EXPECT_TRUE(game::Vector(game, *senders[0], dest));
    EXPECT_TRUE(game::Vector(game, *senders[0], senders[0]));
    EXPECT_EQ(senders[0]->vector_to, kNone);
    EXPECT_EQ(game::NearestCity(game, dest->x, dest->y, side), dest);
    EXPECT_EQ(game::NearestCity(game, dest->x + 1, dest->y - 1, side), dest);
}

TEST(BuyProductionTest, PurchasingSortingOverwritingAndRenamingPersistence) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(17));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* city = side->capital;
    side->gold = 5000;
    auto list = game::BuyableTypes(game);
    ASSERT_TRUE(!list.empty() && list.size() <= 20);
    for (const auto* army_type : list) EXPECT_GE(army_type->price, 0) << army_type->name;
    EXPECT_EQ(game::CannotBuy(game, *side, *city, *game.types.ById(city->slots[0].type)), "has it");
    int slot_count = static_cast<int>(city->slots.size());
    EXPECT_EQ(game::BuySlot(*city), slot_count < 4 ? slot_count : 0);

    const ArmyType* pick = nullptr;
    for (const auto* army_type : list) {
        if (game::CannotBuy(game, *side, *city, *army_type).empty() &&
            (!pick || army_type->price > pick->price)) {
            pick = army_type;
        }
    }
    ASSERT_NE(pick, nullptr);
    if (slot_count == 4) {
        city->slots.pop_back();
        slot_count = 3;
    }
    game::SetProduction(game, *city, 0);
    Slot building = city->slots[0];
    int gold_before = side->gold;
    game::BuyProduction(game, *side, *city, slot_count, pick->id);
    EXPECT_EQ(side->gold, gold_before - pick->price);
    const Slot& bought = city->slots[slot_count];
    EXPECT_EQ(bought.type, pick->id);
    EXPECT_EQ(bought.strength, pick->strength);
    EXPECT_EQ(bought.time, pick->time);
    EXPECT_EQ(bought.move, pick->move);
    EXPECT_EQ(city->defence, rules::CityDefence(static_cast<int>(city->slots.size())));
    EXPECT_EQ(city->producing, 0);

    game::SortProduction(*city);
    for (size_t i = 1; i < city->slots.size(); ++i) {
        EXPECT_LE(city->slots[i - 1].price, city->slots[i].price);
    }
    EXPECT_EQ(city->slots[city->producing], building);

    game::Vector(game, *city,
                 &game.map->cities[0] != city ? &game.map->cities[0] : &game.map->cities[1]);
    const ArmyType* replacement = nullptr;
    for (const auto* army_type : list) {
        if (!replacement && game::CannotBuy(game, *side, *city, *army_type).empty()) {
            replacement = army_type;
        }
    }
    ASSERT_NE(replacement, nullptr);
    game::BuyProduction(game, *side, *city, city->producing, replacement->id);
    EXPECT_EQ(city->producing, kNone);
    EXPECT_EQ(city->vector_to, kNone);

    game::RenameCity(game, *city, "Newtown");
    auto loaded = save::Decode(save::Encode(game), kData);
    EXPECT_EQ(loaded->map->cities[city->index].name, "Newtown");
    EXPECT_EQ(loaded->map->cities[city->index].slots.back().type, city->slots.back().type);
}

TEST(ReportsAndDisbandTest, ReportFiguresStackDisbandSignpostsAndResignation) {
    auto report_game_ptr = SetupGame("ERYTHEA", Seed(23));
    Game& report_game = *report_game_ptr;
    Side* side = game::Begin(report_game);
    auto city_figures = report::ComputeFigures(report_game, *side, report::kCity);
    EXPECT_EQ(city_figures.result, static_cast<int>(game::SideCities(report_game, *side).size()));
    EXPECT_EQ(city_figures.max % 2, 0);
    int unused = 0;
    for (int i = 0; i < 8; ++i) {
        if (city_figures.out[i]) ++unused;
    }
    EXPECT_EQ(unused, 8 - static_cast<int>(report_game.sides.size()));

    int owned_armies = 0;
    for (const Army* army : report_game.armies) {
        if (army->owner == side->index) ++owned_armies;
    }
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kArmy).result, owned_armies);
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kGold).result, side->gold);

    auto winning = report::ComputeFigures(report_game, *side, report::kWinning);
    EXPECT_EQ(winning.max, 100);
    EXPECT_TRUE(winning.result >= 0 && winning.result <= 7);
    side->gold = 100000;
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kWinning).result, 0);
    EXPECT_EQ(report::Score(report_game, *side), 500);

    City* capital = side->capital;
    game::SetProduction(report_game, *capital, 0);
    capital->countdown = 1;
    for (int turns = 0; turns <= 20; ++turns) {
        EndTurn(report_game);
        if (report_game.side == side) break;
    }
    ASSERT_FALSE(side->produced.empty());
    EXPECT_EQ(side->produced[0].city, capital->index);
    EXPECT_EQ(side->produced[0].kind, "built");
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kProduction).result,
              static_cast<int>(side->produced.size()));

    auto disband_game_ptr = SetupGame("ERYTHEA", Seed(29));
    Game& disband_game = *disband_game_ptr;
    Side* d_side = game::Begin(disband_game);
    Army* hero = hero::Recruit(disband_game, *d_side, *d_side->hero_offer).hero;
    auto stack = game::ArmiesAt(disband_game, hero->x, hero->y);
    hero->strength = 5;
    auto fight_strengths = combat::StackStrengths(disband_game, {hero}, hero->x, hero->y);
    EXPECT_EQ(fight_strengths[hero], 5 + combat::kHeroTable[5] + 1);

    size_t army_count = disband_game.armies.size();
    d_side->quest = MakeCityQuest(quest::kOccupy, hero, nullptr);
    Item* standard = hero->items[0];
    game::Disband(disband_game, *d_side, stack);
    EXPECT_EQ(disband_game.armies.size(), army_count - stack.size());
    EXPECT_TRUE(game::ArmiesAt(disband_game, hero->x, hero->y).empty());
    EXPECT_EQ(standard->status, 1);
    EXPECT_EQ(d_side->quest, nullptr);

    Sign* sign = game::SignAt(disband_game, 18, 12);
    ASSERT_NE(sign, nullptr);
    EXPECT_EQ(sign->lines[0], "Traveller! Pray excuse the ");
    EXPECT_EQ(sign->lines[1], "sulphur fumes. Keep On!");
    sign->lines[0] = "Hello there";
    auto reloaded = save::Decode(save::Encode(disband_game), kData);
    EXPECT_EQ(game::SignAt(*reloaded, 18, 12)->lines[0], "Hello there");

    disband_game.map->fight_order[d_side->index][3] = 26;
    reloaded = save::Decode(save::Encode(disband_game), kData);
    EXPECT_EQ(reloaded->map->fight_order[d_side->index][3], 26);

    int diplo_before = d_side->diplo_score;
    game::Resign(disband_game, *d_side);
    EXPECT_TRUE(game::SideCities(disband_game, *d_side).empty());
    EXPECT_TRUE(game::SideArmies(disband_game, *d_side).empty());
    EXPECT_TRUE(d_side->capital->razed);
    EXPECT_EQ(d_side->diplo_score, diplo_before);
}

TEST(HistoryTest, DeedPrioritySlotsRoundRecordsAndHeroStandardTallies) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(41));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    history::RecordDeed(game, side, DeedType::kFound, history::kFoundSage, 0, "Hero");
    history::RecordDeed(game, side, DeedType::kCityWon, side->index, 3, side->name);
    history::RecordDeed(game, side, DeedType::kHeroEmerges, 1, 0, "Hero");
    ASSERT_EQ(game.deeds[side->index].size(), 2U);
    std::vector<history::DeedType> deed_types = {game.deeds[side->index][0].type,
                                                 game.deeds[side->index][1].type};
    std::sort(deed_types.begin(), deed_types.end());
    EXPECT_EQ(deed_types[0], DeedType::kHeroEmerges);
    EXPECT_EQ(deed_types[1], DeedType::kCityWon);

    history::RecordDeed(game, side, DeedType::kAtPeace, 0, 1, "");
    EXPECT_EQ(game.deeds[side->index].size(), 2U);

    for (int guard = 0; game.turn < 3 && guard < 50; ++guard) EndTurn(game);
    ASSERT_FALSE(game.history.empty());
    EXPECT_EQ(game.history[0].owners.size(), game.map->cities.size());
    EXPECT_GE(game.history[0].events.size(), 2U);
    EXPECT_TRUE(!game.deeds.count(side->index) || game.deeds[side->index].empty());

    auto reloaded = save::Decode(save::Encode(game), kData);
    EXPECT_EQ(reloaded->history.size(), game.history.size());

    Side* foe = game.sides[1];
    Item standard;
    standard.index = foe->index;
    Army dead_hero;
    dead_hero.owner = foe->index;
    dead_hero.type = armytype::kHero;
    dead_hero.items = {&standard};
    history::Tally(game, &dead_hero, side->index);
    EXPECT_EQ(history::Triumph(game, side->index, foe->index, history::Loss::kHeroes), 1);
    EXPECT_EQ(history::Triumph(game, side->index, foe->index, history::Loss::kStandards), 1);
    EXPECT_EQ(history::Triumph(game, foe->index, foe->index, history::Loss::kHeroes), 1);
}

TEST(SetupTest, NewGameOptionsInactiveSidesQuickStartAndSaveFidelity) {
    GameOpts setup = SeedWith(5, {{"quests", 0}});
    setup.sides[0] = MakeSideSetup(false);
    setup.sides[1] = MakeSideSetup(true, 2);
    setup.sides[2] = MakeSideSetup(false, kNone, true);
    auto game_ptr = SetupGame("ERYTHEA", setup);
    Game& game = *game_ptr;
    EXPECT_EQ(game.map->options.quests, 0);
    EXPECT_FALSE(game.map->sides[0].computer);
    EXPECT_TRUE(game.map->sides[1].computer && game.map->sides[1].level == 2);
    EXPECT_FALSE(game.map->sides[2].in_use);
    City* capital = game.map->sides[2].capital;
    EXPECT_EQ(capital != nullptr ? capital->owner_index : kNone, kNone);
    for (const Side* side : game.sides) EXPECT_NE(side->index, 2);

    game.map->sides[0].name = "Ozymandias";
    auto reloaded = save::Decode(save::Encode(game), kData);
    EXPECT_TRUE(!reloaded->map->sides[2].in_use && !reloaded->map->sides[2].alive);
    EXPECT_EQ(reloaded->sides.size(), game.sides.size());
    for (const Side* side : reloaded->sides) EXPECT_NE(side->index, 2);
    EXPECT_EQ(reloaded->map->sides[0].name, "Ozymandias");
    EXPECT_EQ(reloaded->map->sides[1].level, 2);

    const std::vector<std::pair<std::string, int>> options = {
        {"neutralCities", 2},   {"diplomacy", 0},      {"quests", 0},        {"hiddenMap", 1},
        {"viewEnemies", 0},     {"viewProduction", 1}, {"intenseCombat", 1}, {"quickStart", 1},
        {"militaryAdvisor", 0}, {"randomTurns", 1}};
    GameOpts full_setup = SeedWith(5, options);
    full_setup.greatest = true;
    full_setup.sides[1] = MakeSideSetup(true, 1);
    full_setup.sides[1].card = 3;
    auto full = SetupGame("ERYTHEA", full_setup);
    game::Reveal(*full, 0, 50, 80, true);
    full->surrender_offered = true;
    auto loaded_full = save::Decode(save::Encode(*full), kData);
    EXPECT_TRUE(game::SurrenderPending(*loaded_full));
    for (const auto& [opt_name, opt_val] : options) {
        EXPECT_EQ(*loaded_full->map->options.Field(opt_name), opt_val) << opt_name;
    }
    EXPECT_TRUE(loaded_full->greatest);
    EXPECT_EQ(loaded_full->map->sides[1].card, 3);
    EXPECT_EQ(loaded_full->map->sides[1].level, 1);
    EXPECT_TRUE(loaded_full->map->sides[1].computer);
    EXPECT_TRUE(game::Seen(*loaded_full, 0, 50, 80));
    EXPECT_FALSE(game::Seen(*loaded_full, 0, 90, 20));

    GameOpts quick_setup = SeedWith(5, {{"quickStart", 1}});
    quick_setup.sides[2] = MakeSideSetup(false, kNone, true);
    auto quick = SetupGame("ERYTHEA", quick_setup);
    std::map<int, int> counts;
    int neutral = 0;
    for (const auto& city : quick->map->cities) {
        city.owner_index == kNone ? ++neutral : ++counts[city.owner_index];
    }
    EXPECT_TRUE(neutral == 0 && counts.count(2) == 0U);
    int least = 1 << 30;
    int most = 0;
    for (const Side* side : quick->sides) {
        least = std::min(least, counts[side->index]);
        most = std::max(most, counts[side->index]);
        EXPECT_EQ(side->capital->owner_index, side->index);
    }
    EXPECT_LE(most - least, 1);
    for (const Army* army : quick->armies) {
        if (City* city = game::CityAt(*quick, army->x, army->y))
            EXPECT_EQ(army->owner, city->owner_index);
    }

    GameOpts slow_setup = Seed(5);
    slow_setup.sides[2] = MakeSideSetup(false, kNone, true);
    EXPECT_NE(FindNeutralCity(*SetupGame("ERYTHEA", slow_setup)), nullptr);
}

TEST(EndGameTest, TriumphEliminationMajorityVictoryAndSurrenderOffer) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(81));
    Game& game = *game_ptr;
    for (Side* side : game.sides) side->computer = true;
    Side* winner = game.sides[0];
    for (auto& city : game.map->cities)
        city.owner_index = city.owner_index != kNone ? winner->index : kNone;
    for (size_t i = 1; i < game.sides.size(); ++i) game.sides[i]->alive = false;
    auto triumph = game::CheckEnd(game);
    EXPECT_TRUE(triumph.triumph && !triumph.over && !triumph.no_humans && triumph.winner == winner);
    EXPECT_TRUE(!winner->computer && game.won);

    auto cityless = SetupGame("ERYTHEA", Seed(82));
    for (auto& city : cityless->map->cities) city.owner_index = kNone;
    EXPECT_FALSE(game::CheckEnd(*cityless).over);
    auto round_end = game::EndRound(*cityless);
    EXPECT_TRUE(round_end.over && round_end.winner == nullptr);
    EXPECT_NE(round_end.message.find("No more players"), std::string::npos);
    EXPECT_EQ(round_end.fallen.size(), cityless->sides.size());

    auto fall_game = SetupGame("ERYTHEA", SeedWith(86, {{"diplomacy", 1}}));
    for (size_t i = 0; i < fall_game->sides.size(); ++i) fall_game->sides[i]->computer = i > 0;
    game::Begin(*fall_game);
    Side* doomed = fall_game->sides[1];
    Side* other = fall_game->sides[2];
    for (City* city : game::SideCities(*fall_game, *doomed)) city->owner_index = other->index;
    fall_game->diplomacy.state[doomed->index * 8 + other->index] = diplomacy::kWar;
    fall_game->diplomacy.state[other->index * 8 + doomed->index] = diplomacy::kWar;
    EXPECT_FALSE(game::SideArmies(*fall_game, *doomed).empty());
    EndTurn(*fall_game);
    EXPECT_TRUE(fall_game->side == other && doomed->alive);
    while (fall_game->turn == 1) EndTurn(*fall_game);
    EXPECT_TRUE(!doomed->alive && game::SideArmies(*fall_game, *doomed).empty() &&
                doomed->gold == 0);
    EXPECT_EQ(diplomacy::State(*fall_game, other->index, doomed->index), diplomacy::kIntermediate);
    EXPECT_EQ(diplomacy::Proposal(*fall_game, doomed->index, other->index),
              diplomacy::kIntermediate);
    ASSERT_FALSE(fall_game->ending.fallen.empty());
    const Fallen& fallen = fall_game->ending.fallen[0];
    EXPECT_TRUE(fallen.side == doomed && fallen.line >= 0 && fallen.line < game::kFallenLines &&
                fallen.boxed);

    auto human_fall = SetupGame("ERYTHEA", Seed(87));
    for (size_t i = 0; i < human_fall->sides.size(); ++i)
        human_fall->sides[i]->computer = i + 1 < human_fall->sides.size();
    game::Begin(*human_fall);
    Side* last_human = human_fall->sides.back();
    for (City* city : game::SideCities(*human_fall, *last_human))
        city->owner_index = human_fall->sides[0]->index;
    for (size_t i = 1; i < human_fall->sides.size(); ++i) EndTurn(*human_fall);
    EXPECT_TRUE(human_fall->side == last_human && last_human->alive);
    EndTurn(*human_fall);
    EXPECT_TRUE(!last_human->alive && last_human->computer);
    EXPECT_TRUE(human_fall->ending.no_humans && human_fall->no_humans_said);
    AdvanceRounds(*human_fall, 1);
    EXPECT_FALSE(human_fall->ending.no_humans);

    auto half_game = SetupGame("ERYTHEA", Seed(83));
    for (Side* side : half_game->sides) side->computer = false;
    Side* holder = half_game->sides[0];
    for (size_t i = 1; i < half_game->sides.size(); ++i) half_game->sides[i]->alive = false;
    int standing = static_cast<int>(half_game->map->cities.size());
    int given = 0;
    for (auto& city : half_game->map->cities)
        city.owner_index = given++ < standing / 2 ? holder->index : kNone;
    EXPECT_FALSE(game::CheckEnd(*half_game).over);
    FindNeutralCity(*half_game)->owner_index = holder->index;
    auto majority = game::CheckEnd(*half_game);
    EXPECT_TRUE(majority.won && !majority.over && majority.winner == holder);
    EXPECT_FALSE(game::CheckEnd(*half_game).won);

    auto surrender_game = SetupGame("ERYTHEA", Seed(84));
    Side* human = surrender_game->sides[0];
    human->computer = false;
    for (size_t i = 1; i < surrender_game->sides.size(); ++i)
        surrender_game->sides[i]->computer = true;
    Side* rival = surrender_game->sides[1];
    int city_count = static_cast<int>(surrender_game->map->cities.size());
    for (int i = 0; i < city_count; ++i)
        surrender_game->map->cities[i].owner_index =
                (i + 1 <= city_count * 3 / 4) ? human->index
                                              : (i + 1 == city_count ? rival->index : kNone);
    auto offer = game::CheckEnd(*surrender_game);
    EXPECT_TRUE(offer.surrender && !offer.over && surrender_game->surrender_offered);
    EXPECT_FALSE(game::CheckEnd(*surrender_game).surrender);
    surrender_game->ending = Ending{};
    EXPECT_TRUE(game::SurrenderPending(*surrender_game));
    surrender_game->surrender_shown = true;
    EXPECT_FALSE(game::SurrenderPending(*surrender_game));
    surrender_game->surrender_shown = false;

    auto ruins_game = SetupGame("ERYTHEA", Seed(85));
    for (size_t i = 1; i < ruins_game->sides.size(); ++i) ruins_game->sides[i]->alive = false;
    for (Side* side : ruins_game->sides) side->computer = false;
    for (size_t i = 0; i < ruins_game->map->cities.size(); ++i) {
        ruins_game->map->cities[i].owner_index = i == 0 ? ruins_game->sides[0]->index : kNone;
        ruins_game->map->cities[i].razed = i > 0;
    }
    EXPECT_TRUE(game::CheckEnd(*ruins_game).won);

    game::AcceptSurrender(*surrender_game, *human);
    EXPECT_TRUE(surrender_game->won && !rival->alive);
}

}  // namespace
}  // namespace w2
