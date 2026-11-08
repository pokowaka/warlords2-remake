/**
 * @file
 * @brief GoogleTest suite for the Warlords II domain model, value types, and .SCN record views.
 */
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"
#include "warlords/scn_format.hpp"
#include "warlords/types.hpp"

namespace w2 {
namespace {

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

}  // namespace
}  // namespace w2
