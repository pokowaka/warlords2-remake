/**
 * @file
 * @brief GoogleTest suite for the Warlords II domain model, encapsulated entities, and .SCN views.
 */
#include <algorithm>
#include <array>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/model/army_registry.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/fog_of_war.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/quest.hpp"
#include "warlords/model/turn_schedule.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/scn.hpp"
#include "warlords/scn_format.hpp"
#include "warlords/types.hpp"

namespace w2 {
namespace {

TEST(StrongIdsTest, ValueSemanticsOrderingHashingAndVectorTarget) {
    static_assert(!std::is_same_v<SideId, CityId>);
    static_assert(!std::is_same_v<CityId, ArmyId>);
    static_assert(!std::is_same_v<ArmyId, ArmyTypeId>);
    static_assert(!std::is_same_v<SiteId, ItemId>);

    constexpr SideId kNoSide;
    static_assert(!kNoSide.IsValid());
    static_assert(kNoSide.Value() == kNone);
    static_assert(kNoSide == SideId::None());

    constexpr SideId kSirians(0);
    constexpr SideId kStormGiants(1);
    static_assert(kSirians.IsValid());
    static_assert(kSirians.Value() == 0);
    static_assert(kSirians < kStormGiants);
    static_assert(kNoSide < kSirians);

    EXPECT_FALSE(CityId().IsValid());
    EXPECT_TRUE(CityId(5).IsValid());
    EXPECT_EQ(ArmyId(42).Value(), 42);
    EXPECT_NE(ArmyTypeId(3), ArmyTypeId(4));
    EXPECT_LT(SiteId(2), SiteId(9));
    EXPECT_GT(ItemId(7), ItemId(1));

    absl::flat_hash_map<CityId, std::string> city_names;
    city_names.emplace(CityId(0), "lloc");
    city_names.emplace(CityId(3), "Marthos");
    EXPECT_EQ(city_names.at(CityId(3)), "Marthos");
    EXPECT_FALSE(city_names.contains(CityId(1)));

    absl::flat_hash_set<ArmyId> seen_armies{ArmyId(10), ArmyId(20), ArmyId(10)};
    EXPECT_EQ(seen_armies.size(), 2U);

    constexpr VectorTarget kNoVector{};
    EXPECT_TRUE(std::holds_alternative<std::monostate>(kNoVector));

    constexpr VectorTarget kToCity = CityId(6);
    ASSERT_TRUE(std::holds_alternative<CityId>(kToCity));
    EXPECT_EQ(std::get<CityId>(kToCity), CityId(6));

    constexpr VectorTarget kToStandard = PlantedStandard{};
    EXPECT_TRUE(std::holds_alternative<PlantedStandard>(kToStandard));
    EXPECT_NE(kToCity, kToStandard);
}

TEST(RngEncapsulationTest, StateAccessorAndSetStateRoundTrip) {
    Rng first_rng(12345.0);
    uint32_t initial_state = first_rng.State();
    int first_draw = first_rng.Dice(2, 6, 1);
    EXPECT_NE(first_rng.State(), initial_state);

    Rng restored_rng;
    restored_rng.SetState(initial_state);
    EXPECT_EQ(restored_rng.State(), initial_state);
    EXPECT_EQ(restored_rng.Dice(2, 6, 1), first_draw);
    EXPECT_EQ(restored_rng.State(), first_rng.State());
}

TEST(TilePosTest, DefaultAndValidCoordinates) {
    constexpr TilePos kUnset;
    EXPECT_FALSE(kUnset.IsValid());
    EXPECT_EQ(kUnset.x, kNone);
    EXPECT_EQ(kUnset.y, kNone);

    constexpr TilePos kOrigin(0, 0);
    EXPECT_TRUE(kOrigin.IsValid());
    EXPECT_EQ(kOrigin.ToPair(), std::make_pair(0, 0));

    constexpr TilePos kTarget(10, 24);
    EXPECT_EQ(kOrigin.ChebyshevDistance(kTarget), 24);
    EXPECT_EQ(kOrigin.ManhattanDistance(kTarget), 34);
    EXPECT_LT(kOrigin, kTarget);
}

TEST(ArmyTypeTest, NamedBonusAccessorsMatchRawArray) {
    ArmyType dragon;
    dragon.id = 20;
    dragon.name = "Dragon";
    dragon.flies = true;
    dragon.bonus[32] = 2;
    dragon.bonus[42] = 1;
    dragon.bonus[48] = 1;
    dragon.bonus[50] = 1;
    dragon.bonus[52] = 3;

    EXPECT_EQ(dragon.SelfBonus(0), 2);
    EXPECT_EQ(dragon.StackBonus(1), 1);
    EXPECT_TRUE(dragon.IsMagical());
    EXPECT_EQ(dragon.Subtract(), 1);
    EXPECT_EQ(dragon.Ability(), 3);

    Types catalogue;
    catalogue.all.push_back(dragon);
    catalogue.ids[dragon.id] = &catalogue.all[0];
    ASSERT_NE(catalogue.ById(20), nullptr);
    EXPECT_EQ(catalogue.ById(20)->name, "Dragon");
    EXPECT_EQ(catalogue.ById(60), nullptr);
    EXPECT_EQ(catalogue.List().size(), 1U);
}

TEST(CityEncapsulationTest, VectorTargetsProductionSlotsAttacksAndRazeInvariants) {
    City castle;
    castle.SetId(CityId(3));
    castle.SetPosition(TilePos(40, 50));
    castle.Rename("Marthos");
    castle.SetIncome(28);
    castle.SetDefence(2);
    EXPECT_EQ(castle.Id(), CityId(3));
    EXPECT_EQ(castle.Index(), 3);
    EXPECT_TRUE(castle.IsNeutral());
    EXPECT_FALSE(castle.OwnedBy(SideId(0)));
    EXPECT_TRUE(castle.ContainsTile(40, 50));
    EXPECT_TRUE(castle.ContainsTile(41, 51));
    EXPECT_FALSE(castle.ContainsTile(42, 50));
    EXPECT_EQ(castle.Position(), TilePos(40, 50));
    EXPECT_FALSE(castle.IsProducing());
    EXPECT_EQ(castle.ActiveSlot(), nullptr);

    Slot expensive{4, "Cavalry", 6, 3, 10, 16, 400};
    Slot cheap{1, "Light Inf", 3, 1, 4, 10, 100};
    castle.ReplaceSlots({expensive, cheap});
    castle.SetOwner(SideId(1));
    castle.SelectProduction(0);
    EXPECT_TRUE(castle.OwnedBy(SideId(1)));
    EXPECT_TRUE(castle.OwnedBy(1));
    EXPECT_TRUE(castle.IsProducing());
    EXPECT_EQ(castle.Countdown(), 3);
    ASSERT_NE(castle.ActiveSlot(), nullptr);
    EXPECT_EQ(castle.ActiveSlot()->name, "Cavalry");

    // Sorting slots by price keeps producing_ pointing at Cavalry (now slot 1).
    castle.SortSlotsByPrice();
    EXPECT_EQ(castle.ProducingSlot(), 1);
    ASSERT_NE(castle.ActiveSlot(), nullptr);
    EXPECT_EQ(castle.ActiveSlot()->name, "Cavalry");

    // VectorTarget transitions: city -> standard -> clear.
    castle.VectorToCity(CityId(7));
    EXPECT_TRUE(castle.IsVectored());
    EXPECT_EQ(castle.VectoredCityId(), CityId(7));
    EXPECT_FALSE(castle.IsVectoredToStandard());

    castle.VectorToStandard();
    EXPECT_TRUE(castle.IsVectored());
    EXPECT_TRUE(castle.IsVectoredToStandard());
    EXPECT_EQ(castle.VectoredCityId(), CityId::None());

    castle.VectorToCity(CityId::None());
    EXPECT_FALSE(castle.IsVectored());

    // Countdown ticks down to completion.
    castle.VectorToCity(CityId(5));
    EXPECT_FALSE(castle.TickCountdown());
    EXPECT_FALSE(castle.TickCountdown());
    EXPECT_TRUE(castle.TickCountdown());
    EXPECT_EQ(castle.Countdown(), 0);
    EXPECT_FALSE(castle.IsProducing());

    // Stopping production clears vectoring as well.
    castle.SelectProduction(kNone);
    EXPECT_FALSE(castle.IsVectored());

    // Attack tracking and Raze invariants.
    castle.RecordAttackBy(SideId(2));
    castle.RecordAttackBy(SideId(5));
    EXPECT_TRUE(castle.HasBeenAttacked());
    EXPECT_TRUE(castle.HasBeenAttackedBy(SideId(2)));
    EXPECT_FALSE(castle.HasBeenAttackedBy(SideId(1)));

    castle.SetClaim(SideId(1));
    castle.SelectProduction(0);
    castle.VectorToStandard();
    castle.Raze(SideId(2));
    EXPECT_TRUE(castle.IsRazed());
    EXPECT_EQ(castle.RazedBy(), SideId(2));
    EXPECT_TRUE(castle.IsNeutral());
    EXPECT_EQ(castle.Claim(), SideId::None());
    EXPECT_TRUE(castle.Slots().empty());
    EXPECT_FALSE(castle.IsProducing());
    EXPECT_FALSE(castle.IsVectored());
    EXPECT_EQ(castle.Defence(), 0);
    EXPECT_EQ(castle.Income(), 0);

    // Razed cities reject new owners.
    castle.SetOwner(SideId(4));
    EXPECT_TRUE(castle.IsNeutral());
}

TEST(SideEncapsulationTest, CapitalResolutionWithoutPointersTreasuryAndMissives) {
    Map scenario_map;
    scenario_map.cities.resize(2);
    scenario_map.cities[0].SetId(CityId(0));
    scenario_map.cities[0].Rename("Stormheim");
    scenario_map.cities[0].SetPosition(TilePos(14, 22));
    scenario_map.cities[1].SetId(CityId(1));
    scenario_map.cities[1].Rename("Kor");
    scenario_map.cities[1].SetPosition(TilePos(40, 50));

    Side& faction = scenario_map.sides[1];
    faction.SetId(SideId(1));
    faction.SetName("Storm Giants");
    faction.SetInUse(true);
    faction.SetAlive(true);
    faction.SetComputerControlled(false);
    faction.SetIncome(85);
    faction.SetUpkeepTotal(30);
    faction.SetOriginCapitalPos(TilePos(40, 50));
    faction.SetCapitalCityId(CityId(1));

    EXPECT_TRUE(faction.IsActive());
    EXPECT_TRUE(faction.IsHuman());
    EXPECT_FALSE(faction.IsComputer());
    EXPECT_EQ(faction.NetIncome(), 55);
    EXPECT_EQ(faction.CapitalPosition(), TilePos(40, 50));

    // Reallocating Map::cities invalidates raw pointers, while CapitalCity(map) stays valid.
    scenario_map.cities.reserve(64);
    ASSERT_NE(faction.CapitalCity(scenario_map), nullptr);
    EXPECT_EQ(faction.CapitalCity(scenario_map)->Name(), "Kor");

    faction.SetCurrentCapitalPos(TilePos(14, 22));
    EXPECT_EQ(faction.CapitalPosition(), TilePos(14, 22));
    faction.ClearCurrentCapitalPos();
    EXPECT_EQ(faction.CapitalPosition(), TilePos(40, 50));

    // Treasury invariants.
    faction.SetGold(-50);
    EXPECT_EQ(faction.Gold(), 0);
    faction.AddGold(250);
    EXPECT_EQ(faction.Gold(), 250);
    EXPECT_FALSE(faction.SpendGold(300));
    EXPECT_EQ(faction.Gold(), 250);
    EXPECT_TRUE(faction.SpendGold(120));
    EXPECT_EQ(faction.Gold(), 130);

    // Missive FIFO queue and cooldown table.
    DiplomaticMissive first_missive;
    first_missive.from_side = 3;
    first_missive.headline = "Peace?";
    faction.AddMissive(first_missive);
    EXPECT_EQ(faction.Missives().size(), 1U);
    EXPECT_EQ(faction.PopFrontMissive().headline, "Peace?");
    EXPECT_TRUE(faction.Missives().empty());

    faction.SetLastMissiveTurn(SideId(3), 9);
    EXPECT_EQ(faction.LastMissiveTurn(SideId(3)), 9);
}

TEST(ArmyAndRegistryTest, SpatialIndexStaysCoherentAcrossPlaceTransitAndArrive) {
    ArmyRegistry registry;
    Army first_proto;
    first_proto.PlaceAt(TilePos(5, 6));
    first_proto.SetOwner(SideId(0));
    first_proto.SetType(ArmyTypeId(1));
    first_proto.SetMoves(12);

    Army hero_proto;
    hero_proto.PlaceAt(TilePos(5, 6));
    hero_proto.SetOwner(SideId(0));
    hero_proto.SetType(ArmyTypeId(armytype::kHero));
    hero_proto.SetMoves(16);

    Army* first = registry.Add(first_proto);
    Army* hero = registry.Add(hero_proto);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(hero, nullptr);
    EXPECT_NE(first->Id(), hero->Id());
    EXPECT_EQ(first->Slot(), 0);
    EXPECT_EQ(hero->Slot(), 1);
    EXPECT_TRUE(hero->Hero());
    EXPECT_TRUE(hero->CanMove());

    // Spatial index lists top-down slot order (slot 1 before slot 0).
    ASSERT_EQ(registry.IndexedOnTile(TilePos(5, 6)).size(), 2U);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(5, 6))[0], hero);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(5, 6))[1], first);

    // Direct Army::PlaceAt updates ArmyRegistry's spatial index automatically.
    hero->PlaceAt(TilePos(8, 9));
    EXPECT_EQ(registry.IndexedOnTile(TilePos(5, 6)).size(), 1U);
    ASSERT_EQ(registry.IndexedOnTile(TilePos(8, 9)).size(), 1U);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(8, 9))[0], hero);

    // Entering road transit removes the army from on-map spatial buckets immediately.
    first->SendIntoTransit(Transit{2, PlantedStandard{}}, /*returning=*/false);
    EXPECT_TRUE(first->InTransit());
    EXPECT_FALSE(first->OnMap());
    EXPECT_TRUE(registry.IndexedOnTile(TilePos(5, 6)).empty());
    EXPECT_FALSE(first->TickTransit());
    EXPECT_TRUE(first->TickTransit());

    // Arriving at (8, 9) restores on-map state and preserves descending slot order (hero slot 1,
    // first slot 0).
    first->ArriveAt(TilePos(8, 9));
    EXPECT_FALSE(first->InTransit());
    EXPECT_TRUE(first->OnMap());
    ASSERT_EQ(registry.IndexedOnTile(TilePos(8, 9)).size(), 2U);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(8, 9))[0], hero);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(8, 9))[1], first);

    // Items, blessings, targets, and slot reuse in pool.
    Item sword;
    sword.index = 8;
    sword.name = "Sword";
    hero->AddItem(&sword);
    EXPECT_TRUE(hero->HasItem(&sword));
    EXPECT_EQ(hero->TakeAllItems().size(), 1U);
    EXPECT_FALSE(hero->HasItem(&sword));

    hero->AddBlessing(2);
    EXPECT_TRUE(hero->HasBlessing(2));
    hero->SetTarget(TilePos(20, 21));
    ASSERT_TRUE(hero->HasTarget());
    EXPECT_EQ(*hero->Target(), TilePos(20, 21));
    hero->SetDone(true);
    EXPECT_FALSE(hero->CanMove());

    ArmyId removed_id = first->Id();
    registry.Remove(first);
    EXPECT_FALSE(registry.Alive(removed_id));
    EXPECT_EQ(registry.FindAlive(removed_id), nullptr);
    EXPECT_EQ(registry.FindInPool(removed_id), first);

    Army* replacement = registry.Add(first_proto);
    EXPECT_EQ(replacement->Slot(), 0);
    EXPECT_NE(replacement->Id(), removed_id);
}

TEST(ScnFormatTest, TypedRecordViewsReadErytheaScenario) {
    auto scn_bytes = ReadBytes("original/ERYTHEA/ERYTHEA.SCN");
    ASSERT_TRUE(scn_bytes.ok()) << scn_bytes.status();
    EXPECT_EQ(scn_bytes->size(), static_cast<size_t>(scn_format::kScnSize));

    scn_format::SideRecordView first_side(*scn_bytes, 0);
    EXPECT_EQ(first_side.Name(), "Sirians");
    EXPECT_GT(first_side.Gold(), 0);
    EXPECT_GE(first_side.CapX(), 0);
    EXPECT_GE(first_side.CapY(), 0);

    int city_count = U16(*scn_bytes, scn_format::kCitiesCount);
    EXPECT_GT(city_count, 20);
    scn_format::CityRecordView first_city(*scn_bytes, 0);
    EXPECT_FALSE(first_city.Name().empty());
    EXPECT_TRUE(first_city.X() >= 0 && first_city.X() < kMapW);
    EXPECT_TRUE(first_city.Y() >= 0 && first_city.Y() < kMapH);
}

TEST(GameModelTest, NewGameAndMapQueries) {
    game::NewGameOptions game_opts;
    game_opts.seed = 42;
    auto created_game = game::NewGame("original", "ERYTHEA", game_opts);
    ASSERT_TRUE(created_game.ok()) << created_game.status();
    auto& game_state = *created_game;
    ASSERT_NE(game_state, nullptr);
    ASSERT_NE(game_state->map, nullptr);
    EXPECT_EQ(game_state->LivingSideCount(), 8);
    EXPECT_TRUE(game_state->map->InBounds(0, 0));
    EXPECT_FALSE(game_state->map->InBounds(-1, 0));
    EXPECT_FALSE(game_state->map->InBounds(kMapW, kMapH));

    const City* first_city = game_state->map->City(0);
    ASSERT_NE(first_city, nullptr);
    EXPECT_EQ(game_state->map->CityAtTopLeft(first_city->X(), first_city->Y()), first_city);
    EXPECT_EQ(game_state->map->CityAtFootprint(first_city->X() + 1, first_city->Y() + 1),
              first_city);

    Army scout;
    scout.PlaceAt(first_city->Position());
    scout.SetOwner(SideId(0));
    scout.SetType(ArmyTypeId(0));
    scout.SetStrength(4);
    scout.SetMoves(10);
    Army* added = game_state->Add(scout);
    ASSERT_NE(added, nullptr);
    EXPECT_TRUE(game_state->Alive(added));
    auto stack_here = game_state->ArmiesOnTile(first_city->X(), first_city->Y());
    EXPECT_FALSE(stack_here.empty());
    game_state->Remove(added);
    EXPECT_FALSE(game_state->Alive(added));
}

TEST(DiplomacyMatrixTest, PairStatesAlliancesTrucesTrustAndTargets) {
    DiplomacyMatrix matrix;
    matrix.Reset(diplomacy::kWar);
    EXPECT_EQ(matrix.State(SideId(0), SideId(1)), diplomacy::kWar);
    EXPECT_EQ(matrix.Proposal(SideId(0), SideId(1)), diplomacy::kWar);

    matrix.Propose(SideId(0), SideId(1), diplomacy::kPeace);
    EXPECT_EQ(matrix.Proposal(SideId(0), SideId(1)), diplomacy::kPeace);
    matrix.ProposeAlliance(SideId(0), SideId(1), true);
    EXPECT_TRUE(matrix.ProposalAllied(SideId(0), SideId(1)));
    matrix.Propose(SideId(0), SideId(1), diplomacy::kWar);
    EXPECT_FALSE(matrix.ProposalAllied(SideId(0), SideId(1)));

    matrix.SetAllied(SideId(2), SideId(3), true);
    EXPECT_TRUE(matrix.IsAllied(SideId(2), SideId(3)));
    EXPECT_TRUE(matrix.IsAllied(SideId(3), SideId(2)));
    EXPECT_EQ(matrix.State(SideId(2), SideId(3)), diplomacy::kPeace);

    matrix.SetTruce(SideId(2), SideId(3), 10);
    EXPECT_EQ(matrix.TruceTurns(SideId(3), SideId(2)), 10);
    matrix.AddTrust(SideId(2), SideId(3), 140);
    EXPECT_EQ(matrix.Trust(SideId(2), SideId(3)), 100);
    matrix.AddTrust(SideId(2), SideId(3), -250);
    EXPECT_EQ(matrix.Trust(SideId(2), SideId(3)), -100);

    matrix.SetGoldOffer(SideId(0), SideId(2), 150);
    EXPECT_EQ(matrix.GoldOffer(SideId(0), SideId(2)), 150);
    matrix.SetWarTarget(SideId(0), SideId(2), SideId(5));
    EXPECT_EQ(matrix.WarTarget(SideId(0), SideId(2)), SideId(5));
    matrix.SetAlliedTargetCity(SideId(2), CityId(12));
    EXPECT_EQ(matrix.AlliedTargetCity(SideId(2)), CityId(12));

    matrix.ResetFallenSide(SideId(2), diplomacy::kIntermediate);
    EXPECT_EQ(matrix.State(SideId(2), SideId(3)), diplomacy::kIntermediate);
    EXPECT_EQ(matrix.State(SideId(3), SideId(2)), diplomacy::kIntermediate);
    EXPECT_EQ(matrix.State(SideId(2), SideId(2)), diplomacy::kIntermediate);
}

TEST(FogOfWarTest, PerSideVisibilityMasksAndCounts) {
    FogOfWar fog;
    EXPECT_FALSE(fog.HasMask(SideId(0)));
    EXPECT_FALSE(fog.IsExplored(SideId(0), 10, 10, 3, 4));

    EXPECT_TRUE(fog.MarkExplored(SideId(0), 10, 10, 3, 4));
    EXPECT_FALSE(fog.MarkExplored(SideId(0), 10, 10, 3, 4));
    EXPECT_TRUE(fog.IsExplored(SideId(0), 10, 10, 3, 4));
    EXPECT_EQ(fog.ExploredCount(SideId(0)), 1);

    fog.ResetSide(SideId(1), 4, 4, true);
    EXPECT_EQ(fog.ExploredCount(SideId(1)), 16);
    EXPECT_EQ(fog.SidesWithMasks(), (std::vector<SideId>{SideId(0), SideId(1)}));

    fog.Clear();
    EXPECT_FALSE(fog.HasMask(SideId(0)));
}

TEST(TurnScheduleTest, SequentialAndRandomizedRoundOrder) {
    TurnSchedule schedule;
    EXPECT_EQ(schedule.Cursor(), -1);
    EXPECT_FALSE(schedule.AtRoundEnd());
    EXPECT_EQ(schedule.Advance(), SideId(0));
    EXPECT_EQ(schedule.CurrentSideId(), SideId(0));

    for (int step = 1; step < 8; ++step) {
        EXPECT_EQ(schedule.Advance(), SideId(step));
    }
    EXPECT_TRUE(schedule.AtRoundEnd());

    Rng rng(77.0);
    schedule.StartNewRound(/*random_turns=*/true, rng);
    EXPECT_EQ(schedule.Cursor(), -1);
    auto sorted_order = schedule.Order();
    std::sort(sorted_order.begin(), sorted_order.end());
    EXPECT_EQ(sorted_order, (std::array<int, 8>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(QuestAndProducedModelTest, VariantTargetsResolutionAndPerGameBugs) {
    game::NewGameOptions game_opts;
    game_opts.seed = 13;
    auto created_game = game::NewGame("original", "ERYTHEA", game_opts);
    ASSERT_TRUE(created_game.ok()) << created_game.status();
    Game& game = **created_game;

    Army hero_proto;
    hero_proto.PlaceAt(game.map->cities[0].Position());
    hero_proto.SetOwner(SideId(0));
    hero_proto.SetType(ArmyTypeId(armytype::kHero));
    Army* hero = game.Add(hero_proto);

    Quest city_quest(quest::kOccupy, hero->Id(), CityId(2));
    EXPECT_TRUE(city_quest.HasTarget());
    EXPECT_EQ(city_quest.TargetCityId(), CityId(2));
    EXPECT_EQ(city_quest.ResolveHero(game), hero);
    EXPECT_EQ(city_quest.ResolveCity(game), &game.map->cities[2]);

    Quest pillage_quest(quest::kPillageGold, hero->Id(), std::monostate{}, 800, 0);
    EXPECT_TRUE(pillage_quest.HasTarget());
    pillage_quest.AddProgress(350);
    EXPECT_EQ(pillage_quest.Done(), 350);

    Quest broken_quest(quest::kOccupy, hero->Id(), CityId::None());
    EXPECT_FALSE(broken_quest.HasTarget());

    Produced built_entry{ProducedKind::kBuilt, 4, CityId(2), false};
    EXPECT_EQ(built_entry.kind, ProducedKind::kBuilt);
    EXPECT_EQ(built_entry.city, CityId(2));

    Game other_game;
    EXPECT_TRUE(game.RulesBugs().hero_experience_reads_attacker_types);
    game.RulesBugs().hero_experience_reads_attacker_types = false;
    EXPECT_FALSE(game.RulesBugs().hero_experience_reads_attacker_types);
    EXPECT_TRUE(other_game.RulesBugs().hero_experience_reads_attacker_types);
}

TEST(Phase4ArchitectureTest, TurnReportPathfinderAndComputerPlayerStrategy) {
    game::NewGameOptions game_opts;
    game_opts.seed = 17;
    auto created_game = game::NewGame("original", "ERYTHEA", game_opts);
    ASSERT_TRUE(created_game.ok()) << created_game.status();
    Game& game = **created_game;

    for (const Side* side : game.sides) {
        ASSERT_NE(side->ComputerPlayer(), nullptr);
        EXPECT_TRUE(side->IsClassicAi());
    }

    AttitudeBreakdown attitude =
            game.sides[1]->ComputerPlayer()->EvaluateProposal(game, SideId(1), SideId(0));
    EXPECT_EQ(attitude.from_side, 1);
    EXPECT_EQ(attitude.to_side, 0);

    Side* current = game::Begin(game);
    ASSERT_NE(current, nullptr);
    TurnReport& news = game.TurnNews(current->Id());
    EXPECT_NE(news.hero_offer, nullptr);

    Pathfinder& pathfinder = game.GetPathfinder();
    auto& grid0 = pathfinder.GridFor(game, current->Index());
    EXPECT_FALSE(grid0.empty());
    pathfinder.InvalidateSide(current->Id());
    pathfinder.Invalidate();

    auto saved_ai = game.sides[1]->ComputerPlayer()->SaveState();
    EXPECT_TRUE(saved_ai.is_object());
    EXPECT_TRUE(game.sides[1]->ComputerPlayer()->LoadState(saved_ai, game).ok());
}

}  // namespace
}  // namespace w2
