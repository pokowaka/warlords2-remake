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
    ASSERT_TRUE(armytype::Load(kData / "TERRAIN0/ARMYTYPE.DAT", types).ok());
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
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, true).Strength(), 9);
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, false).Strength(), 8);
    EXPECT_EQ(rules::ArmyFromSlot(sample_slot, false).Upkeep(), 4);
}

/** @brief Parameterized suite checking initial scenario invariants across all shipped scenarios. */
class GameScenarioTest : public ScenarioTest {};

TEST_P(GameScenarioTest, InitialStateGarrisonsAndSeedDeterminism) {
    const std::string& scenario = GetParam();
    auto game_ptr = SetupGame(scenario, Seed(3));
    Game& game = *game_ptr;
    EXPECT_GE(game.Sides().size(), 2U);
    EXPECT_EQ(game.Armies().size(), game.GetMap().Cities().size());

    for (Side* side : game.Sides()) {
        auto owned = game::SideCities(game, *side);
        ASSERT_EQ(owned.size(), 1U) << side->Name();
        EXPECT_EQ(owned[0]->Id(), side->CapitalCityId()) << side->Name();
    }
    for (const auto& city : game.GetMap().Cities()) {
        for (const auto& slot : city.Slots()) EXPECT_NE(slot.type, armytype::kNavy) << city.Name();
        EXPECT_EQ(city.Defence(), rules::CityDefence(city.SlotCount()));
        EXPECT_LE(city.Slots().size(), 4U);
    }
    std::map<int, int> per_tile;
    for (const Army* army : game.Armies()) {
        EXPECT_NE(game.ArmyTypes().ById(army->TypeIndex()), nullptr);
        EXPECT_EQ(army->Moves(), 0);
        EXPECT_LE(++per_tile[army->Y() * game.GetMap().Width() + army->X()], rules::kMaxStack);
    }

    auto rebuilt = SetupGame(scenario, Seed(3));
    ASSERT_EQ(rebuilt->Armies().size(), game.Armies().size());
    for (size_t i = 0; i < game.Armies().size(); ++i) {
        EXPECT_EQ(rebuilt->Armies()[i]->Type(), game.Armies()[i]->Type());
        EXPECT_EQ(rebuilt->Armies()[i]->Strength(), game.Armies()[i]->Strength());
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
    for (const Army* army : game::SideArmies(game, *side)) {
        EXPECT_EQ(army->Moves(), army->MaxMoves());
    }
    int initial_gold = side->Gold();
    EXPECT_EQ(side->Income(), side->CapitalCity(game.GetMap())->Income());
    EXPECT_EQ(side->Gold(), std::max(0, initial_gold));

    Army* first_army = game::SideArmies(game, *side)[0];
    first_army->SetDone(true);
    first_army->SetOffered(true);
    first_army->SetFortified(true);
    AdvanceRounds(game, 1);
    EXPECT_FALSE(first_army->IsDone());
    EXPECT_FALSE(first_army->IsOffered());
    EXPECT_TRUE(first_army->IsFortified());
    first_army->SetFortified(false);

    first_army->SetMoves(5);
    int base_moves = first_army->MaxMoves();
    AdvanceRounds(game, 1);
    EXPECT_EQ(first_army->Moves(), base_moves + rules::kMoveCarry);
    first_army->SetMoves(1);
    AdvanceRounds(game, 1);
    EXPECT_EQ(first_army->Moves(), base_moves + 1);

    City* city = side->CapitalCity(game.GetMap());
    game::SetProduction(game, *city, 0);
    size_t armies_before = game::SideArmies(game, *side).size();
    AdvanceRounds(game, static_cast<size_t>(city->SlotAt(0).time));
    EXPECT_EQ(game::SideArmies(game, *side).size(), armies_before + 1);
    Army* built = nullptr;
    for (Army* army : game::SideArmies(game, *side)) {
        if (army->HomeCity() == city->Id() && army != first_army) built = army;
    }
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(built->Upkeep(), city->SlotAt(0).cost / 2);
    EXPECT_EQ(built->X(), city->X());

    Side* victim = game.Sides().back();
    for (City* victim_city : game::SideCities(game, *victim)) victim_city->SetOwner(SideId::None());
    victim->SetGold(500);
    game.Diplomacy().SetState(victim->Id(), SideId(0), diplomacy::kWar);
    EXPECT_FALSE(game::SideArmies(game, *victim).empty());
    int round_of = game.Turn();
    while (game.Turn() == round_of) {
        EXPECT_TRUE(victim->IsAlive());
        EndTurn(game);
    }
    EXPECT_FALSE(victim->IsAlive());
    EXPECT_TRUE(game::SideArmies(game, *victim).empty());
    EXPECT_EQ(victim->Gold(), 0);
    EXPECT_EQ(diplomacy::State(game, victim->Index(), 0), diplomacy::kIntermediate);
    ASSERT_FALSE(game.EndingState().fallen.empty());
    EXPECT_EQ(game.EndingState().fallen[0].side, victim);

    auto second_game = SetupGame("ERYTHEA", Seed(6));
    game::Begin(*second_game);
    EXPECT_EQ(second_game->Turn(), 1);
    AdvanceRounds(*second_game, 1);
    EXPECT_EQ(second_game->Turn(), 2);
}

TEST(VectoringTest, TwoTurnTransitFourSenderCapAndSelfLift) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(9));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* from = side->CapitalCity(game.GetMap());
    City* dest = FindNeutralCity(game, 1);
    ASSERT_NE(dest, nullptr);
    dest->SetOwner(side->Id());
    for (Army* army : game::ArmiesAt(game, dest->X(), dest->Y())) army->SetOwner(side->Id());
    game::SetProduction(game, *from, 0);
    game::Vector(game, *from, dest);
    size_t at_dest = game::ArmiesAt(game, dest->X(), dest->Y()).size();
    AdvanceRounds(game, static_cast<size_t>(from->SlotAt(0).time + 2));
    EXPECT_GT(game::ArmiesAt(game, dest->X(), dest->Y()).size(), at_dest);

    Army* sent = nullptr;
    for (int i = 0; i < 10 && sent == nullptr; ++i) {
        AdvanceRounds(game, 1);
        bool any_sent = false;
        for (const auto& entry : game.TurnNews(side->Id()).produced) {
            if (entry.kind == ProducedKind::kSent) any_sent = true;
        }
        if (!any_sent) continue;
        for (Army* army : game.Armies()) {
            if (army->InTransit() && army->OwnedBy(side->Id()) &&
                army->TransitState()->dest == VectorTarget{dest->Id()} &&
                army->TransitState()->turns == 2) {
                sent = army;
            }
        }
    }
    ASSERT_NE(sent, nullptr);
    AdvanceRounds(game, 1);
    EXPECT_TRUE(sent->InTransit() && sent->TransitState()->turns == 1);
    AdvanceRounds(game, 1);
    EXPECT_TRUE(!sent->InTransit() && sent->X() != kNone);

    game::SetProduction(game, *from, kNone);
    EXPECT_FALSE(from->IsVectored());

    std::vector<City*> senders;
    for (auto& city : game.GetMap().Cities()) {
        if (&city != dest && senders.size() < 5) {
            city.SetOwner(side->Id());
            senders.push_back(&city);
        }
    }
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(game::Vector(game, *senders[i], dest));
    EXPECT_FALSE(game::Vector(game, *senders[4], dest));
    EXPECT_FALSE(senders[4]->IsVectored());
    EXPECT_EQ(game::VectoredTo(game, dest).size(), 4U);
    EXPECT_TRUE(game::Vector(game, *senders[0], dest));
    EXPECT_TRUE(game::Vector(game, *senders[0], senders[0]));
    EXPECT_FALSE(senders[0]->IsVectored());
    EXPECT_EQ(game::NearestCity(game, dest->X(), dest->Y(), side), dest);
    EXPECT_EQ(game::NearestCity(game, dest->X() + 1, dest->Y() - 1, side), dest);
}

TEST(BuyProductionTest, PurchasingSortingOverwritingAndRenamingPersistence) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(17));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* city = side->CapitalCity(game.GetMap());
    side->SetGold(5000);
    auto list = game::BuyableTypes(game);
    ASSERT_TRUE(!list.empty() && list.size() <= 20);
    for (const auto* army_type : list) EXPECT_GE(army_type->price, 0) << army_type->name;
    EXPECT_EQ(game::CannotBuy(game, *side, *city, *game.ArmyTypes().ById(city->SlotAt(0).type)),
              "has it");
    int slot_count = city->SlotCount();
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
        city->PopLastSlot();
        slot_count = 3;
    }
    game::SetProduction(game, *city, 0);
    Slot building = city->SlotAt(0);
    int gold_before = side->Gold();
    game::BuyProduction(game, *side, *city, slot_count, pick->id);
    EXPECT_EQ(side->Gold(), gold_before - pick->price);
    const Slot& bought = city->SlotAt(slot_count);
    EXPECT_EQ(bought.type, pick->id);
    EXPECT_EQ(bought.strength, pick->strength);
    EXPECT_EQ(bought.time, pick->time);
    EXPECT_EQ(bought.move, pick->move);
    EXPECT_EQ(city->Defence(), rules::CityDefence(city->SlotCount()));
    EXPECT_EQ(city->ProducingSlot(), 0);

    game::SortProduction(*city);
    for (size_t i = 1; i < city->Slots().size(); ++i) {
        EXPECT_LE(city->SlotAt(i - 1).price, city->SlotAt(i).price);
    }
    EXPECT_EQ(city->SlotAt(city->ProducingSlot()), building);

    game::Vector(
            game, *city,
            game.GetMap().CityAt(0) != city ? game.GetMap().CityAt(0) : game.GetMap().CityAt(1));
    const ArmyType* replacement = nullptr;
    for (const auto* army_type : list) {
        if (!replacement && game::CannotBuy(game, *side, *city, *army_type).empty()) {
            replacement = army_type;
        }
    }
    ASSERT_NE(replacement, nullptr);
    game::BuyProduction(game, *side, *city, city->ProducingSlot(), replacement->id);
    EXPECT_EQ(city->ProducingSlot(), kNone);
    EXPECT_FALSE(city->IsVectored());

    game::RenameCity(game, *city, "Newtown");
    auto loaded = *save::Decode(save::Encode(game), kData);
    EXPECT_EQ(loaded->GetMap().Cities()[city->Index()].Name(), "Newtown");
    EXPECT_EQ(loaded->GetMap().Cities()[city->Index()].Slots().back().type,
              city->Slots().back().type);
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
    EXPECT_EQ(unused, 8 - static_cast<int>(report_game.Sides().size()));

    int owned_armies = 0;
    for (const Army* army : report_game.Armies()) {
        if (army->OwnedBy(side->Id())) ++owned_armies;
    }
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kArmy).result, owned_armies);
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kGold).result, side->Gold());

    auto winning = report::ComputeFigures(report_game, *side, report::kWinning);
    EXPECT_EQ(winning.max, 100);
    EXPECT_TRUE(winning.result >= 0 && winning.result <= 7);
    side->SetGold(100000);
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kWinning).result, 0);
    EXPECT_EQ(report::Score(report_game, *side), 500);

    City* capital = side->CapitalCity(report_game.GetMap());
    game::SetProduction(report_game, *capital, 0);
    capital->SetCountdown(1);
    for (int turns = 0; turns <= 20; ++turns) {
        EndTurn(report_game);
        if (report_game.CurrentSide() == side) break;
    }
    const auto& produced = report_game.TurnNews(side->Id()).produced;
    ASSERT_FALSE(produced.empty());
    EXPECT_EQ(produced[0].city, capital->Id());
    EXPECT_EQ(produced[0].kind, ProducedKind::kBuilt);
    EXPECT_EQ(report::ComputeFigures(report_game, *side, report::kProduction).result,
              static_cast<int>(produced.size()));

    auto disband_game_ptr = SetupGame("ERYTHEA", Seed(29));
    Game& disband_game = *disband_game_ptr;
    Side* d_side = game::Begin(disband_game);
    Army* hero =
            hero::Recruit(disband_game, *d_side, *disband_game.TurnNews(d_side->Id()).hero_offer)
                    .hero;
    auto stack = game::ArmiesAt(disband_game, hero->X(), hero->Y());
    hero->SetStrength(5);
    auto fight_strengths = combat::StackStrengths(disband_game, {hero}, hero->X(), hero->Y());
    EXPECT_EQ(fight_strengths[hero], 5 + combat::kHeroTable[5] + 1);

    size_t army_count = disband_game.Armies().size();
    d_side->SetQuest(MakeCityQuest(quest::kOccupy, hero, nullptr));
    Item* standard = hero->Items()[0];
    game::Disband(disband_game, *d_side, stack);
    EXPECT_EQ(disband_game.Armies().size(), army_count - stack.size());
    EXPECT_TRUE(game::ArmiesAt(disband_game, hero->X(), hero->Y()).empty());
    EXPECT_EQ(standard->status, 1);
    EXPECT_EQ(d_side->ActiveQuest(), nullptr);

    Sign* sign = game::SignAt(disband_game, 18, 12);
    ASSERT_NE(sign, nullptr);
    EXPECT_EQ(sign->lines[0], "Traveller! Pray excuse the ");
    EXPECT_EQ(sign->lines[1], "sulphur fumes. Keep On!");
    sign->lines[0] = "Hello there";
    auto reloaded = *save::Decode(save::Encode(disband_game), kData);
    EXPECT_EQ(game::SignAt(*reloaded, 18, 12)->lines[0], "Hello there");

    disband_game.GetMap().MutableFightOrder()[d_side->Index()][3] = 26;
    reloaded = *save::Decode(save::Encode(disband_game), kData);
    EXPECT_EQ(reloaded->GetMap().FightOrder()[d_side->Index()][3], 26);

    int diplo_before = d_side->DiploScore();
    game::Resign(disband_game, *d_side);
    EXPECT_TRUE(game::SideCities(disband_game, *d_side).empty());
    EXPECT_TRUE(game::SideArmies(disband_game, *d_side).empty());
    EXPECT_TRUE(d_side->CapitalCity(disband_game.GetMap())->IsRazed());
    EXPECT_EQ(d_side->DiploScore(), diplo_before);
}

TEST(HistoryTest, DeedPrioritySlotsRoundRecordsAndHeroStandardTallies) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(41));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    history::RecordDeed(game, side, DeedType::kFound, history::kFoundSage, 0, "Hero");
    history::RecordDeed(game, side, DeedType::kCityWon, side->Index(), 3, side->Name());
    history::RecordDeed(game, side, DeedType::kHeroEmerges, 1, 0, "Hero");
    const auto& side_deeds = game.DeedsForSide(side->Index());
    ASSERT_EQ(side_deeds.size(), 2U);
    std::vector<history::DeedType> deed_types = {side_deeds[0].type, side_deeds[1].type};
    std::sort(deed_types.begin(), deed_types.end());
    EXPECT_EQ(deed_types[0], DeedType::kHeroEmerges);
    EXPECT_EQ(deed_types[1], DeedType::kCityWon);

    history::RecordDeed(game, side, DeedType::kAtPeace, 0, 1, "");
    EXPECT_EQ(game.DeedsForSide(side->Index()).size(), 2U);

    for (int guard = 0; game.Turn() < 3 && guard < 50; ++guard) EndTurn(game);
    ASSERT_FALSE(game.History().empty());
    EXPECT_EQ(game.History()[0].owners.size(), game.GetMap().Cities().size());
    EXPECT_GE(game.History()[0].events.size(), 2U);
    EXPECT_TRUE(game.DeedsForSide(side->Index()).empty());

    auto reloaded = *save::Decode(save::Encode(game), kData);
    EXPECT_EQ(reloaded->History().size(), game.History().size());

    Side* foe = game.Sides()[1];
    Item standard;
    standard.index = foe->Index();
    Army dead_hero;
    dead_hero.SetOwner(foe->Id());
    dead_hero.SetType(ArmyTypeId(armytype::kHero));
    dead_hero.SetItems({&standard});
    history::Tally(game, &dead_hero, side->Index());
    EXPECT_EQ(history::Triumph(game, side->Index(), foe->Index(), history::Loss::kHeroes), 1);
    EXPECT_EQ(history::Triumph(game, side->Index(), foe->Index(), history::Loss::kStandards), 1);
    EXPECT_EQ(history::Triumph(game, foe->Index(), foe->Index(), history::Loss::kHeroes), 1);
}

TEST(SetupTest, NewGameOptionsInactiveSidesQuickStartAndSaveFidelity) {
    GameOpts setup = SeedWith(5, {{"quests", 0}});
    setup.sides[0] = MakeSideSetup(false);
    setup.sides[1] = MakeSideSetup(true, 2);
    setup.sides[2] = MakeSideSetup(false, kNone, true);
    auto game_ptr = SetupGame("ERYTHEA", setup);
    Game& game = *game_ptr;
    EXPECT_EQ(game.GetMap().GameOptions().quests, 0);
    EXPECT_FALSE(game.GetMap().Sides()[0].IsComputerControlled());
    EXPECT_TRUE(game.GetMap().Sides()[1].IsComputerControlled() &&
                game.GetMap().Sides()[1].Level() == 2);
    EXPECT_FALSE(game.GetMap().Sides()[2].InUse());
    City* capital = game.GetMap().Sides()[2].CapitalCity(game.GetMap());
    EXPECT_EQ(capital != nullptr ? capital->OwnerIndex() : kNone, kNone);
    for (const Side* side : game.Sides()) EXPECT_NE(side->Index(), 2);

    game.GetMap().Sides()[0].SetName("Ozymandias");
    auto reloaded = *save::Decode(save::Encode(game), kData);
    EXPECT_TRUE(!reloaded->GetMap().Sides()[2].InUse() && !reloaded->GetMap().Sides()[2].IsAlive());
    EXPECT_EQ(reloaded->Sides().size(), game.Sides().size());
    for (const Side* side : reloaded->Sides()) EXPECT_NE(side->Index(), 2);
    EXPECT_EQ(reloaded->GetMap().Sides()[0].Name(), "Ozymandias");
    EXPECT_EQ(reloaded->GetMap().Sides()[1].Level(), 2);

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
    full->SetSurrenderOffered(true);
    auto loaded_full = *save::Decode(save::Encode(*full), kData);
    EXPECT_TRUE(game::SurrenderPending(*loaded_full));
    for (const auto& [opt_name, opt_val] : options) {
        EXPECT_EQ(*loaded_full->GetMap().MutableOptions().Field(opt_name), opt_val) << opt_name;
    }
    EXPECT_TRUE(loaded_full->IsGreatestCampaign());
    EXPECT_EQ(loaded_full->GetMap().Sides()[1].Card(), 3);
    EXPECT_EQ(loaded_full->GetMap().Sides()[1].Level(), 1);
    EXPECT_TRUE(loaded_full->GetMap().Sides()[1].IsComputerControlled());
    EXPECT_TRUE(game::Seen(*loaded_full, 0, 50, 80));
    EXPECT_FALSE(game::Seen(*loaded_full, 0, 90, 20));

    GameOpts quick_setup = SeedWith(5, {{"quickStart", 1}});
    quick_setup.sides[2] = MakeSideSetup(false, kNone, true);
    auto quick = SetupGame("ERYTHEA", quick_setup);
    std::map<int, int> counts;
    int neutral = 0;
    for (const auto& city : quick->GetMap().Cities()) {
        city.IsNeutral() ? ++neutral : ++counts[city.OwnerIndex()];
    }
    EXPECT_TRUE(neutral == 0 && counts.count(2) == 0U);
    int least = 1 << 30;
    int most = 0;
    for (const Side* side : quick->Sides()) {
        least = std::min(least, counts[side->Index()]);
        most = std::max(most, counts[side->Index()]);
        EXPECT_EQ(side->CapitalCity(quick->GetMap())->Owner(), side->Id());
    }
    EXPECT_LE(most - least, 1);
    for (const Army* army : quick->Armies()) {
        if (City* city = game::CityAt(*quick, army->X(), army->Y())) {
            EXPECT_EQ(army->Owner(), city->Owner());
        }
    }

    GameOpts slow_setup = Seed(5);
    slow_setup.sides[2] = MakeSideSetup(false, kNone, true);
    EXPECT_NE(FindNeutralCity(*SetupGame("ERYTHEA", slow_setup)), nullptr);
}

TEST(EndGameTest, TriumphEliminationMajorityVictoryAndSurrenderOffer) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(81));
    Game& game = *game_ptr;
    for (Side* side : game.Sides()) side->SetComputerControlled(true);
    Side* winner = game.Sides()[0];
    for (auto& city : game.GetMap().Cities()) {
        city.SetOwner(!city.IsNeutral() ? winner->Id() : SideId::None());
    }
    for (size_t i = 1; i < game.Sides().size(); ++i) game.Sides()[i]->SetAlive(false);
    auto triumph = game::CheckEnd(game);
    EXPECT_TRUE(triumph.triumph && !triumph.over && !triumph.no_humans && triumph.winner == winner);
    EXPECT_TRUE(!winner->IsComputerControlled() && game.IsWon());

    auto cityless = SetupGame("ERYTHEA", Seed(82));
    for (auto& city : cityless->GetMap().Cities()) city.SetOwner(SideId::None());
    EXPECT_FALSE(game::CheckEnd(*cityless).over);
    auto round_end = game::EndRound(*cityless);
    EXPECT_TRUE(round_end.over && round_end.winner == nullptr);
    EXPECT_NE(round_end.message.find("No more players"), std::string::npos);
    EXPECT_EQ(round_end.fallen.size(), cityless->Sides().size());

    auto fall_game = SetupGame("ERYTHEA", SeedWith(86, {{"diplomacy", 1}}));
    for (size_t i = 0; i < fall_game->Sides().size(); ++i) {
        fall_game->Sides()[i]->SetComputerControlled(i > 0);
    }
    game::Begin(*fall_game);
    Side* doomed = fall_game->Sides()[1];
    Side* other = fall_game->Sides()[2];
    for (City* city : game::SideCities(*fall_game, *doomed)) city->SetOwner(other->Id());
    fall_game->Diplomacy().SetSymmetricState(doomed->Id(), other->Id(), diplomacy::kWar);
    EXPECT_FALSE(game::SideArmies(*fall_game, *doomed).empty());
    EndTurn(*fall_game);
    EXPECT_TRUE(fall_game->CurrentSide() == other && doomed->IsAlive());
    while (fall_game->Turn() == 1) EndTurn(*fall_game);
    EXPECT_TRUE(!doomed->IsAlive() && game::SideArmies(*fall_game, *doomed).empty() &&
                doomed->Gold() == 0);
    EXPECT_EQ(diplomacy::State(*fall_game, other->Index(), doomed->Index()),
              diplomacy::kIntermediate);
    EXPECT_EQ(diplomacy::Proposal(*fall_game, doomed->Index(), other->Index()),
              diplomacy::kIntermediate);
    ASSERT_FALSE(fall_game->EndingState().fallen.empty());
    const Fallen& fallen = fall_game->EndingState().fallen[0];
    EXPECT_TRUE(fallen.side == doomed && fallen.line >= 0 && fallen.line < game::kFallenLines &&
                fallen.boxed);

    auto human_fall = SetupGame("ERYTHEA", Seed(87));
    for (size_t i = 0; i < human_fall->Sides().size(); ++i) {
        human_fall->Sides()[i]->SetComputerControlled(i + 1 < human_fall->Sides().size());
    }
    game::Begin(*human_fall);
    Side* last_human = human_fall->Sides().back();
    for (City* city : game::SideCities(*human_fall, *last_human)) {
        city->SetOwner(human_fall->Sides()[0]->Id());
    }
    for (size_t i = 1; i < human_fall->Sides().size(); ++i) EndTurn(*human_fall);
    EXPECT_TRUE(human_fall->CurrentSide() == last_human && last_human->IsAlive());
    EndTurn(*human_fall);
    EXPECT_TRUE(!last_human->IsAlive() && last_human->IsComputerControlled());
    EXPECT_TRUE(human_fall->EndingState().no_humans && human_fall->NoHumansSaid());
    AdvanceRounds(*human_fall, 1);
    EXPECT_FALSE(human_fall->EndingState().no_humans);

    auto half_game = SetupGame("ERYTHEA", Seed(83));
    for (Side* side : half_game->Sides()) side->SetComputerControlled(false);
    Side* holder = half_game->Sides()[0];
    for (size_t i = 1; i < half_game->Sides().size(); ++i) half_game->Sides()[i]->SetAlive(false);
    int standing = static_cast<int>(half_game->GetMap().Cities().size());
    int given = 0;
    for (auto& city : half_game->GetMap().Cities()) {
        city.SetOwner(given++ < standing / 2 ? holder->Id() : SideId::None());
    }
    EXPECT_FALSE(game::CheckEnd(*half_game).over);
    FindNeutralCity(*half_game)->SetOwner(holder->Id());
    auto majority = game::CheckEnd(*half_game);
    EXPECT_TRUE(majority.won && !majority.over && majority.winner == holder);
    EXPECT_FALSE(game::CheckEnd(*half_game).won);

    auto surrender_game = SetupGame("ERYTHEA", Seed(84));
    Side* human = surrender_game->Sides()[0];
    human->SetComputerControlled(false);
    for (size_t i = 1; i < surrender_game->Sides().size(); ++i) {
        surrender_game->Sides()[i]->SetComputerControlled(true);
    }
    Side* rival = surrender_game->Sides()[1];
    int city_count = static_cast<int>(surrender_game->GetMap().Cities().size());
    for (int i = 0; i < city_count; ++i) {
        surrender_game->GetMap().Cities()[i].SetOwner(
                (i + 1 <= city_count * 3 / 4)
                        ? human->Id()
                        : (i + 1 == city_count ? rival->Id() : SideId::None()));
    }
    auto offer = game::CheckEnd(*surrender_game);
    EXPECT_TRUE(offer.surrender && !offer.over && surrender_game->SurrenderOffered());
    EXPECT_FALSE(game::CheckEnd(*surrender_game).surrender);
    surrender_game->SetEndingState(Ending{});
    EXPECT_TRUE(game::SurrenderPending(*surrender_game));
    surrender_game->SetSurrenderShown(true);
    EXPECT_FALSE(game::SurrenderPending(*surrender_game));
    surrender_game->SetSurrenderShown(false);

    auto ruins_game = SetupGame("ERYTHEA", Seed(85));
    for (size_t i = 1; i < ruins_game->Sides().size(); ++i) ruins_game->Sides()[i]->SetAlive(false);
    for (Side* side : ruins_game->Sides()) side->SetComputerControlled(false);
    for (size_t i = 0; i < ruins_game->GetMap().Cities().size(); ++i) {
        ruins_game->GetMap().Cities()[i].SetRazed(i > 0);
        ruins_game->GetMap().Cities()[i].SetOwner(i == 0 ? ruins_game->Sides()[0]->Id()
                                                         : SideId::None());
    }
    EXPECT_TRUE(game::CheckEnd(*ruins_game).won);

    game::AcceptSurrender(*surrender_game, *human);
    EXPECT_TRUE(surrender_game->IsWon() && !rival->IsAlive());
}

}  // namespace
}  // namespace w2
