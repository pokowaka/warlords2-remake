/**
 * @file
 * @brief GoogleTest suite for the Warlords II domain model, value types, and .SCN record views.
 */
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
#include "warlords/model/quest.hpp"
#include "warlords/model/turn_schedule.hpp"
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

TEST(DomainEntitiesTest, CitySideAndArmyQueries) {
    City castle;
    castle.index = 3;
    castle.x = 40;
    castle.y = 50;
    castle.name = "Marthos";
    castle.owner_index = kNone;
    EXPECT_TRUE(castle.IsNeutral());
    EXPECT_FALSE(castle.OwnedBy(0));
    EXPECT_TRUE(castle.ContainsTile(40, 50));
    EXPECT_TRUE(castle.ContainsTile(41, 51));
    EXPECT_FALSE(castle.ContainsTile(42, 50));
    EXPECT_EQ(castle.Position(), TilePos(40, 50));
    EXPECT_FALSE(castle.IsProducing());
    EXPECT_EQ(castle.ActiveSlot(), nullptr);

    Slot heavy_inf;
    heavy_inf.type = 2;
    heavy_inf.name = "Heavy Inf";
    heavy_inf.strength = 5;
    heavy_inf.time = 2;
    castle.slots.push_back(heavy_inf);
    castle.producing = 0;
    castle.countdown = 2;
    castle.owner_index = 1;
    castle.vector_to = 4;
    EXPECT_FALSE(castle.IsNeutral());
    EXPECT_TRUE(castle.OwnedBy(1));
    EXPECT_TRUE(castle.IsProducing());
    ASSERT_NE(castle.ActiveSlot(), nullptr);
    EXPECT_EQ(castle.ActiveSlot()->name, "Heavy Inf");
    EXPECT_TRUE(castle.IsVectored());

    Side faction;
    faction.index = 1;
    faction.in_use = true;
    faction.alive = true;
    faction.computer = false;
    faction.income = 85;
    faction.upkeep_total = 30;
    faction.cap_x = 40;
    faction.cap_y = 50;
    EXPECT_TRUE(faction.IsActive());
    EXPECT_TRUE(faction.IsHuman());
    EXPECT_FALSE(faction.IsComputer());
    EXPECT_EQ(faction.NetIncome(), 55);
    EXPECT_EQ(faction.CapitalPosition(), TilePos(40, 50));
    faction.cap_now_x = 12;
    faction.cap_now_y = 18;
    EXPECT_EQ(faction.CapitalPosition(), TilePos(12, 18));

    Army unit;
    unit.x = 40;
    unit.y = 50;
    unit.owner = 1;
    unit.type = armytype::kHero;
    unit.moves = 12;
    EXPECT_TRUE(unit.Hero());
    EXPECT_TRUE(unit.OnMap());
    EXPECT_TRUE(unit.OwnedBy(1));
    EXPECT_TRUE(unit.CanMove());
    EXPECT_EQ(unit.Position(), TilePos(40, 50));
    unit.done = true;
    EXPECT_FALSE(unit.CanMove());
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
    EXPECT_EQ(game_state->map->CityAtTopLeft(first_city->x, first_city->y), first_city);
    EXPECT_EQ(game_state->map->CityAtFootprint(first_city->x + 1, first_city->y + 1), first_city);

    Army scout;
    scout.x = first_city->x;
    scout.y = first_city->y;
    scout.owner = 0;
    scout.type = 0;
    scout.strength = 4;
    scout.moves = 10;
    Army* added = game_state->Add(scout);
    ASSERT_NE(added, nullptr);
    EXPECT_TRUE(game_state->Alive(added));
    auto stack_here = game_state->ArmiesOnTile(first_city->x, first_city->y);
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

TEST(ArmyRegistryTest, StableIdsSlotReuseAndSpatialRelocate) {
    ArmyRegistry registry;
    Army first_proto;
    first_proto.x = 5;
    first_proto.y = 6;
    first_proto.owner = 0;
    first_proto.type = 1;

    Army second_proto = first_proto;
    Army* first = registry.Add(first_proto);
    Army* second = registry.Add(second_proto);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->id, second->id);
    EXPECT_EQ(first->slot, 0);
    EXPECT_EQ(second->slot, 1);

    // ArmiesOnTile and IndexedOnTile list top-down slot order (slot 1 before slot 0).
    auto stack = registry.ArmiesOnTile(5, 6);
    ASSERT_EQ(stack.size(), 2U);
    EXPECT_EQ(stack[0], second);
    EXPECT_EQ(stack[1], first);
    ASSERT_EQ(registry.IndexedOnTile(TilePos(5, 6)).size(), 2U);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(5, 6))[0], second);

    ArmyId removed_id = first->id;
    registry.Remove(first);
    EXPECT_FALSE(registry.Alive(first));
    EXPECT_FALSE(registry.Alive(removed_id));
    EXPECT_EQ(registry.FindAlive(removed_id), nullptr);
    EXPECT_EQ(registry.FindInPool(removed_id), first);

    Army* replacement = registry.Add(first_proto);
    EXPECT_EQ(replacement->slot, 0);
    EXPECT_NE(replacement->id, removed_id);
    EXPECT_EQ(registry[0], replacement);
    EXPECT_EQ(registry[1], second);

    registry.Relocate(second, TilePos(8, 9));
    EXPECT_EQ(second->Position(), TilePos(8, 9));
    EXPECT_EQ(registry.IndexedOnTile(TilePos(5, 6)).size(), 1U);
    ASSERT_EQ(registry.IndexedOnTile(TilePos(8, 9)).size(), 1U);
    EXPECT_EQ(registry.IndexedOnTile(TilePos(8, 9))[0], second);
}

TEST(QuestAndProducedModelTest, VariantTargetsResolutionAndPerGameBugs) {
    game::NewGameOptions game_opts;
    game_opts.seed = 13;
    auto created_game = game::NewGame("original", "ERYTHEA", game_opts);
    ASSERT_TRUE(created_game.ok()) << created_game.status();
    Game& game = **created_game;

    Army hero_proto;
    hero_proto.x = game.map->cities[0].x;
    hero_proto.y = game.map->cities[0].y;
    hero_proto.owner = 0;
    hero_proto.type = armytype::kHero;
    Army* hero = game.Add(hero_proto);

    Quest city_quest(quest::kOccupy, hero->id, CityId(2));
    EXPECT_TRUE(city_quest.HasTarget());
    EXPECT_EQ(city_quest.TargetCityId(), CityId(2));
    EXPECT_EQ(city_quest.ResolveHero(game), hero);
    EXPECT_EQ(city_quest.ResolveCity(game), &game.map->cities[2]);

    Quest pillage_quest(quest::kPillageGold, hero->id, std::monostate{}, 800, 0);
    EXPECT_TRUE(pillage_quest.HasTarget());
    pillage_quest.AddProgress(350);
    EXPECT_EQ(pillage_quest.Done(), 350);

    Quest broken_quest(quest::kOccupy, hero->id, CityId::None());
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

}  // namespace
}  // namespace w2
