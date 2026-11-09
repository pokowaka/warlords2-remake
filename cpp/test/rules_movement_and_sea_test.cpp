/**
 * @file
 * @brief GoogleTest suite for land/sea/air movement, stack limits, ships, fog of war, and
 * encampments.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"
#include "warlords/scn.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::AddArmy;
using test::AdvanceRounds;
using test::FindCityByName;
using test::FindEnemyCity;
using test::FindTerrainTile;
using test::kData;
using test::ScenarioParamName;
using test::ScenarioTest;
using test::Seed;
using test::SeedWith;
using test::SetupGame;

/** @brief Parameterized movement suite exercised on land-heavy and island scenarios. */
class MovementScenarioTest : public ScenarioTest {};

TEST_P(MovementScenarioTest, StepCostsPathfindingAttacksAndRoutePreview) {
    auto game_ptr = SetupGame(GetParam(), Seed(11));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    std::vector<Army*> stack = {army};

    EXPECT_EQ(movement::kCost[movement::kMountains], 0);
    EXPECT_EQ(movement::kCost[movement::kRoad], 1);
    EXPECT_EQ(movement::kCost[movement::kHills], 6);

    const auto& grid = movement::Grid(game, side->Index());
    int roaded = 0;
    bool road_cost_ok = true;
    for (int y = 0; y < game.GetMap().Height(); ++y) {
        for (int x = 0; x < game.GetMap().Width(); ++x) {
            if (scn::RoadAt(game.GetMap(), x, y) % 0x20 != 0) {
                ++roaded;
                if (grid[y * game.GetMap().Width() + x] % 8 != 1) road_cost_ok = false;
            }
        }
    }
    EXPECT_GT(roaded, 0);
    EXPECT_TRUE(road_cost_ok);

    constexpr int kLand = 2;
    constexpr int kWater = 2 | movement::kWaterF;
    constexpr int kCross = 1 | movement::kWaterF | movement::kCrossF;
    auto unwrap = [](std::optional<int> cost) { return cost ? *cost : kNone; };
    EXPECT_EQ(unwrap(movement::StepCost(kLand, kWater, movement::kLand, false, false, 10)), kNone);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 0, movement::kLand, false, false, 10)), kNone);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 6 | movement::kHillsF, movement::kLand, false, false,
                                        10)),
              6);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 6 | movement::kHillsF, movement::kLand, false, true,
                                        10)),
              2);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 4 | movement::kForestF, movement::kLand, true, false,
                                        10)),
              2);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, kCross, movement::kLand, false, false, 10)), 1);
    EXPECT_EQ(unwrap(movement::StepCost(kCross, kWater, movement::kLand, false, false, 10)), 12);
    EXPECT_EQ(unwrap(movement::StepCost(kCross, kWater, movement::kLand, false, false, 20)), 22);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 0, movement::kFlying, false, false, 10)), 2);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, kWater, movement::kFlying, false, false, 10)), 2);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 1, movement::kFlying, false, false, 10)), 1);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, 6 | movement::kHillsF, movement::kFlying, false,
                                        false, 10)),
              2);
    EXPECT_EQ(unwrap(movement::StepCost(kLand, movement::kCityF, movement::kFlying, false, false,
                                        10)),
              kNone);
    EXPECT_EQ(movement::Distance(5, 5, 6, 6), 1);
    EXPECT_EQ(movement::Distance(5, 5, 7, 6), 2);
    EXPECT_EQ(movement::Direction(5, 5, 6, 4), 1);
    EXPECT_EQ(movement::Direction(5, 5, 9, 4), 1);
    EXPECT_EQ(movement::Direction(5, 5, 5, 9), 4);
    EXPECT_EQ(unwrap(movement::StepCost(kWater, kLand, movement::kBoat, false, false, 10)), kNone);
    EXPECT_EQ(unwrap(movement::StepCost(kWater, kWater, movement::kBoat, false, false, 10)), 2);

    army->SetMoves(7);
    Army slow_army;
    Army fast_army;
    slow_army.SetMoves(3);
    fast_army.SetMoves(9);
    EXPECT_EQ(movement::StackMoves({army, &slow_army, &fast_army}), 3);

    army->SetMoves(army->MaxMoves());
    int from_x = army->X();
    int from_y = army->Y();
    bool found = false;
    int target_x = 0;
    int target_y = 0;
    for (int dx = -6; dx <= 6 && !found; ++dx) {
        for (int dy = -6; dy <= 6 && !found; ++dy) {
            int x = from_x + dx;
            int y = from_y + dy;
            if (!game.GetMap().InBounds(x, y) || game::CityAt(game, x, y)) continue;
            auto found_path = movement::FindPath(game, stack, from_x, from_y, x, y);
            if (found_path && found_path->size() >= 2 &&
                found_path->back().cost < movement::kPastShore &&
                !movement::Has(movement::Grid(game, side->Index())[y * game.GetMap().Width() + x],
                               movement::kWaterF)) {
                found = true;
                target_x = x;
                target_y = y;
            }
        }
    }
    EXPECT_TRUE(found);
    if (found) {
        int before = army->Moves();
        auto result = movement::MoveTo(game, stack, target_x, target_y);
        EXPECT_GT(result.steps, 0);
        EXPECT_EQ(army->Moves(), std::max(0, before - result.spent));
        EXPECT_TRUE(army->X() != from_x || army->Y() != from_y);
    }

    army->SetMoves(0);
    EXPECT_EQ(movement::MoveTo(game, stack, from_x, from_y + 1).steps, 0);

    City* enemy = FindEnemyCity(game, side->Index());
    ASSERT_NE(enemy, nullptr);
    army->SetMoves(99);
    army->PlaceAt(TilePos(enemy->X(), enemy->Y() - 1));
    auto attack_path =
            movement::FindPath(game, stack, army->X(), army->Y(), enemy->X(), enemy->Y());
    if (attack_path && !attack_path->empty()) {
        auto walk = movement::Walk(game, stack, *attack_path);
        EXPECT_EQ(walk.stopped, movement::WalkStop::kAttack);
        EXPECT_TRUE(walk.attack && walk.attack->city == enemy);
        EXPECT_EQ(army->X(), enemy->X());
        EXPECT_EQ(army->Y(), enemy->Y() - 1);
    }
    EXPECT_EQ(movement::Grid(game, side->Index())[enemy->Y() * game.GetMap().Width() + enemy->X()] %
                      8,
              0);

    Army* scout = AddArmy(game, army->X(), army->Y(), side->Index(), 11, 9, 20, 20);
    scout->SetUpkeep(1);
    int far_x = 0;
    int far_y = 0;
    std::optional<movement::Preview> route;
    for (int distance = 12; distance <= 30; distance += 2) {
        far_x = std::min(game.GetMap().Width() - 2, scout->X() + distance);
        far_y = scout->Y();
        route = movement::PreviewRoute(game, {scout}, far_x, far_y);
        if (route && route->reach < static_cast<int>(route->path.size())) break;
    }
    if (route) {
        EXPECT_FALSE(route->path.empty());
        EXPECT_LE(route->reach, static_cast<int>(route->path.size()));
        EXPECT_EQ(route->path.back().x, far_x);
        EXPECT_EQ(route->path.back().y, far_y);
        int prev_x = scout->X();
        int prev_y = scout->Y();
        bool single_steps = true;
        for (const auto& step : route->path) {
            if (std::max(std::abs(step.x - prev_x), std::abs(step.y - prev_y)) != 1) {
                single_steps = false;
            }
            prev_x = step.x;
            prev_y = step.y;
        }
        EXPECT_TRUE(single_steps);
        auto scout_walk = movement::MoveTo(game, {scout}, far_x, far_y);
        EXPECT_EQ(static_cast<int>(scout_walk.walked.size()), scout_walk.steps);
        if (scout_walk.steps > 0) {
            EXPECT_EQ(scout_walk.walked.back().first, scout->X());
            EXPECT_EQ(scout_walk.walked.back().second, scout->Y());
        }
        auto remaining = movement::PreviewRoute(game, {scout}, far_x, far_y);
        if (remaining) EXPECT_LT(remaining->reach, static_cast<int>(remaining->path.size()));
    }
    game.Remove(scout);
}

// NOLINTNEXTLINE(readability-identifier-length)
INSTANTIATE_TEST_SUITE_P(LandAndIsland, MovementScenarioTest,
                         ::testing::Values("ERYTHEA", "ISLADIA"), ScenarioParamName);

TEST(MovementTest, StackLimitBlocksEnteringFullTile) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(13));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    int target_x = army->X();
    int target_y = army->Y() - 1 >= 0 ? army->Y() - 1 : army->Y() + 1;
    for (int i = 0; i < rules::kMaxStack; ++i) {
        Army* filler =
                AddArmy(game, target_x, target_y, side->Index(), army->TypeIndex(), 1, 10, 10);
        filler->SetHomeCity(army->HomeCity());
    }
    EXPECT_EQ(static_cast<int>(game::ArmiesAt(game, target_x, target_y).size()), rules::kMaxStack);
    army->SetMoves(99);
    EXPECT_EQ(movement::MoveTo(game, {army}, target_x, target_y).steps, 0);
    EXPECT_TRUE(army->X() != target_x || army->Y() != target_y);
}

TEST(MovementTest, ShipsDisembarkAtTurnStartAndSinkStrandedLandUnits) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(3));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* city = game::SideCities(game, *side)[0];
    Army* sailor = AddArmy(game, city->X(), city->Y(), side->Index(), 2, 3, 0, 14);
    sailor->SetAtSea(true);
    AdvanceRounds(game, 1);
    EXPECT_FALSE(sailor->IsAtSea());
    EXPECT_EQ(sailor->Moves(), sailor->MaxMoves());

    TilePos water = FindTerrainTile(game, movement::kWater, true);
    ASSERT_TRUE(water.IsValid());
    Army* walker = AddArmy(game, water.x, water.y, side->Index(), 2, 3, 14, 14);
    Army* boat = AddArmy(game, water.x, water.y, side->Index(), 2, 3, 14, 14);
    boat->SetAtSea(true);
    Army* hero = AddArmy(game, water.x, water.y, side->Index(), armytype::kHero, 5, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 1);
    EXPECT_FALSE(game.Alive(walker));
    EXPECT_TRUE(game.Alive(boat) && game.Alive(hero));

    int flyer_type = kNone;
    for (const auto& army_type : game.ArmyTypes().all) {
        if (flyer_type == kNone && army_type.flies) flyer_type = army_type.id;
    }
    Army* bird = AddArmy(game, water.x, water.y, side->Index(), flyer_type, 3, 14, 14);
    Army* walker2 = AddArmy(game, water.x, water.y, side->Index(), 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side), 1);
    EXPECT_TRUE(game.Alive(bird) && !game.Alive(walker2));

    Item wings;
    wings.type = rules::kItemFlight;
    hero->AddItem(&wings);
    Army* carried = AddArmy(game, water.x, water.y, side->Index(), 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 0);
    hero->ClearItems();
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 1);
    EXPECT_FALSE(game.Alive(carried));

    Army* ashore = AddArmy(game, city->X(), city->Y(), side->Index(), 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, city->X(), city->Y()), 0);
    EXPECT_TRUE(game.Alive(ashore));
}

TEST(MovementTest, LastMovePointBuysRoadStepAndChargesAttackMinimumTwo) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(11));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    int road_x = kNone;
    int road_y = kNone;
    for (int y = 1; y < game.GetMap().Height() - 1 && road_x == kNone; ++y) {
        for (int x = 1; x < game.GetMap().Width() - 1 && road_x == kNone; ++x) {
            if (scn::RoadAt(game.GetMap(), x, y) % 0x20 == 0 ||
                scn::RoadAt(game.GetMap(), x + 1, y) % 0x20 == 0) {
                continue;
            }
            if (game::CityAt(game, x, y) || game::CityAt(game, x + 1, y)) continue;
            if (!game::ArmiesAt(game, x, y).empty() || !game::ArmiesAt(game, x + 1, y).empty()) {
                continue;
            }
            road_x = x;
            road_y = y;
        }
    }
    ASSERT_NE(road_x, kNone);
    army->PlaceAt(TilePos(road_x, road_y));
    army->SetAtSea(false);
    army->SetMoves(1);
    movement::Invalidate(game);
    auto walk_result = movement::MoveTo(game, {army}, road_x + 1, road_y);
    EXPECT_EQ(walk_result.steps, 1);
    EXPECT_EQ(army->X(), road_x + 1);
    EXPECT_EQ(army->Moves(), 0);

    City* enemy = FindEnemyCity(game, side->Index());
    ASSERT_NE(enemy, nullptr);
    Army* attacker = AddArmy(game, enemy->X(), enemy->Y() - 1, side->Index(), 11, 3, 1, 20);
    game::ChargeAttack(game, {attacker}, enemy->X(), enemy->Y());
    EXPECT_EQ(attacker->Moves(), 0);
    attacker->SetMoves(10);
    game::ChargeAttack(game, {attacker}, enemy->X(), enemy->Y());
    EXPECT_EQ(attacker->Moves(), 8);
}

TEST(MovementTest, HiddenMapSightRadiiPathBlockingAndNextHumanScreen) {
    auto off = SetupGame("ERYTHEA", Seed(101));
    EXPECT_TRUE(game::Seen(*off, 0, 5, 5));
    EXPECT_EQ(game::Reveal(*off, 0, 5, 5, false), 0);

    auto game_ptr = SetupGame("ERYTHEA", SeedWith(101, {{"hiddenMap", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* side_cap = side->CapitalCity(game.GetMap());
    EXPECT_TRUE(game::Seen(game, side->Index(), side_cap->X(), side_cap->Y()));
    EXPECT_FALSE(game::Seen(game, side->Index(), 0, 0));

    auto dark_game = SetupGame("ERYTHEA", SeedWith(102, {{"hiddenMap", 1}}));
    int open_x = kNone;
    int open_y = kNone;
    for (int y = 20; y <= 60 && open_x == kNone; ++y) {
        for (int x = 20; x <= 60 && open_x == kNone; ++x) {
            if (!game::CityAt(*dark_game, x, y)) {
                open_x = x;
                open_y = y;
            }
        }
    }
    EXPECT_EQ(game::Reveal(*dark_game, 20, open_x, open_y, false), 9);
    EXPECT_EQ(game::Reveal(*dark_game, 20, open_x, open_y, false), 0);
    EXPECT_EQ(game::Reveal(*dark_game, 20, open_x + 20, open_y, true), 25);
    City& first_city = dark_game->GetMap().Cities()[0];
    EXPECT_EQ(game::Reveal(*dark_game, 21, first_city.X(), first_city.Y(), false), 25);

    Army* army = game::SideArmies(game, *side)[0];
    army->SetMoves(99);
    City* unseen = nullptr;
    for (auto& candidate : game.GetMap().Cities()) {
        if (!unseen && !game::Seen(game, side->Index(), candidate.X(), candidate.Y()) &&
            !candidate.OwnedBy(side->Id())) {
            unseen = &candidate;
        }
    }
    ASSERT_NE(unseen, nullptr);
    EXPECT_FALSE(movement::FindPath(game, {army}, army->X(), army->Y(), unseen->X(), unseen->Y()));

    auto count_seen = [&]() { return game.Explored().ExploredCount(side->Id()); };
    int seen_before = count_seen();
    for (int dx = -3; dx <= 3; ++dx) {
        bool walked = false;
        for (int dy = -3; dy <= 3 && !walked; ++dy) {
            int x = army->X() + dx;
            int y = army->Y() + dy;
            if (x >= 0 && y >= 0 && game::Seen(game, side->Index(), x, y) &&
                (x != army->X() || y != army->Y())) {
                auto path = movement::FindPath(game, {army}, army->X(), army->Y(), x, y);
                if (path && !path->empty() && path->back().cost < movement::kPastShore) {
                    movement::MoveTo(game, {army}, x, y);
                    EXPECT_GT(count_seen(), seen_before);
                    walked = true;
                }
            }
        }
        if (walked) break;
    }

    Side* other = game.Sides()[0] != side ? game.Sides()[0] : game.Sides()[1];
    EXPECT_FALSE(game::Seen(game, other->Index(), side_cap->X(), side_cap->Y()));

    auto computer_first = SetupGame("ERYTHEA", SeedWith(103, {{"hiddenMap", 1}}));
    for (size_t i = 0; i < computer_first->Sides().size(); ++i) {
        computer_first->Sides()[i]->SetComputerControlled(i != 2);
    }
    EXPECT_EQ(game::Begin(*computer_first), computer_first->Sides()[0]);
    EXPECT_EQ(game::NextHuman(*computer_first), computer_first->Sides()[2]);
    computer_first->Sides()[2]->SetComputerControlled(true);
    EXPECT_EQ(game::NextHuman(*computer_first), nullptr);
}

TEST(MovementTest, EncampmentFormsOnOpenGroundWhenFortifiedAndAddsFortifyOne) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(5));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* cap = side->CapitalCity(game.GetMap());
    Army* army = game::SideArmies(game, *side)[0];
    int x = kNone;
    int y = kNone;
    for (int radius = 2; radius <= 8 && x == kNone; ++radius) {
        for (int dx = -radius; dx <= radius && x == kNone; ++dx) {
            for (int dy = -radius; dy <= radius && x == kNone; ++dy) {
                int tile_x = cap->X() + dx;
                int tile_y = cap->Y() + dy;
                if (tile_x >= 0 && tile_y >= 0 && tile_x < game.GetMap().Width() - 1 &&
                    tile_y < game.GetMap().Height() &&
                    scn::TerrainAt(game.GetMap(), tile_x, tile_y) == movement::kPlain &&
                    scn::TerrainAt(game.GetMap(), tile_x + 1, tile_y) == movement::kPlain &&
                    scn::RoadAt(game.GetMap(), tile_x, tile_y) % 32 == 0 &&
                    game::ArmiesAt(game, tile_x, tile_y).empty() &&
                    game::ArmiesAt(game, tile_x + 1, tile_y).empty()) {
                    x = tile_x;
                    y = tile_y;
                }
            }
        }
    }
    ASSERT_TRUE(army != nullptr && x != kNone);
    army->PlaceAt(TilePos(x, y));
    EXPECT_FALSE(game::TowerAt(game, x, y));
    game::StartTurn(game, *side);
    EXPECT_FALSE(game::TowerAt(game, x, y));

    army->SetFortified(true);
    game::StartTurn(game, *side);
    EXPECT_TRUE(game::TowerAt(game, x, y));
    EXPECT_EQ(combat::Fortify(game, {}, x, y, combat::TerrainClass(game, x, y)), 1);

    EXPECT_EQ(movement::MoveTo(game, {army}, x + 1, y).steps, 1);
    EXPECT_FALSE(game::TowerAt(game, x, y));

    army->PlaceAt(cap->Position());
    game::StartTurn(game, *side);
    EXPECT_FALSE(game::TowerAt(game, cap->X(), cap->Y()));
}

TEST(MovementTest, EmbarkingDisembarkingFlyingOverWaterAndPortSailing) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(81));
    Game* game = game_ptr.get();
    int land_x = kNone;
    int land_y = kNone;
    int water_x = kNone;
    int water_y = kNone;
    for (int y = 1; y <= game->GetMap().Height() - 2 && land_x == kNone; ++y) {
        for (int x = 1; x <= game->GetMap().Width() - 2 && land_x == kNone; ++x) {
            int next_terrain = scn::TerrainAt(game->GetMap(), x + 1, y);
            if (scn::TerrainAt(game->GetMap(), x, y) == movement::kPlain &&
                (next_terrain == movement::kWater || next_terrain == movement::kShore) &&
                game::ArmiesAt(*game, x, y).empty() && game::ArmiesAt(*game, x + 1, y).empty()) {
                land_x = x;
                land_y = y;
                water_x = x + 1;
                water_y = y;
            }
        }
    }
    ASSERT_NE(land_x, kNone);

    const ArmyType* dragon = nullptr;
    for (const auto& army_type : game->ArmyTypes().all) {
        if (army_type.flies && army_type.name.find("Dragon") != std::string::npos) {
            dragon = &army_type;
        }
    }
    ASSERT_NE(dragon, nullptr);
    Army* hero = AddArmy(*game, land_x, land_y, 0, armytype::kHero, 5, 20, 20);
    Army* dragon_army = AddArmy(*game, land_x, land_y, 0, dragon->id, 5, 20, 20);
    std::vector<Army*> stack = {hero, dragon_army};
    EXPECT_EQ(movement::ModeOf(*game, stack), movement::kFlying);
    movement::Walk(*game, stack, {movement::Step{water_x, water_y, 2}});
    EXPECT_EQ(hero->X(), water_x);
    EXPECT_TRUE(!dragon_army->IsAtSea() && !hero->IsAtSea());
    EXPECT_EQ(movement::ModeOf(*game, stack), movement::kFlying);
    movement::Walk(*game, stack, {movement::Step{land_x, land_y, 2}});
    EXPECT_EQ(hero->X(), land_x);

    Army* foot = AddArmy(*game, land_x, land_y, 0, 1, 5, 20, 20);
    movement::SettleSea(*game, {foot}, water_x, water_y, movement::kLand, false);
    EXPECT_TRUE(foot->IsAtSea());
    movement::SettleSea(*game, {foot}, land_x, land_y, movement::kLand, true);
    EXPECT_FALSE(foot->IsAtSea());
    movement::SettleSea(*game, {hero, dragon_army, foot}, water_x, water_y, movement::kLand, false);
    EXPECT_FALSE(dragon_army->IsAtSea());
    EXPECT_TRUE(hero->IsAtSea() && foot->IsAtSea());
    hero->SetAtSea(false);
    foot->SetAtSea(false);
    movement::SettleSea(*game, {foot}, water_x, water_y, movement::kBoat, false);
    EXPECT_FALSE(foot->IsAtSea());

    hero->PlaceAt(TilePos(water_x, water_y));
    dragon_army->PlaceAt(TilePos(water_x, water_y));
    hero->SetAtSea(true);
    dragon_army->SetAtSea(true);
    game->Remove(foot);
    auto reloaded = *save::Decode(save::Encode(*game), kData);
    for (const Army* loaded_army : reloaded->Armies()) {
        if (loaded_army->X() == water_x && loaded_army->Y() == water_y) {
            EXPECT_FALSE(loaded_army->IsAtSea());
        }
    }

    auto game2_ptr = SetupGame("ERYTHEA", Seed(81));
    game = game2_ptr.get();
    City* mirea = FindCityByName(*game, "Mirea");
    City* kuuria = FindCityByName(*game, "Kuuria");
    ASSERT_NE(mirea, nullptr);
    EXPECT_TRUE(mirea->OwnedBy(0) && movement::IsPort(*game, *mirea));
    const auto& grid = movement::Grid(*game, 0);
    auto grid_byte = [&](int x, int y) { return grid[y * game->GetMap().Width() + x]; };
    EXPECT_EQ(scn::TerrainAt(game->GetMap(), 86, 7), movement::kShore);
    EXPECT_TRUE(movement::Has(grid_byte(86, 7), movement::kWaterF));
    EXPECT_FALSE(movement::StepCost(grid_byte(83, 9), grid_byte(82, 9), movement::kLand, false,
                                    false, 10)
                         .has_value());

    Army* sailor = AddArmy(*game, mirea->X(), mirea->Y(), 0, 1, 5, 20, 20);
    auto walk_result = movement::MoveTo(*game, {sailor}, 86, 4);
    EXPECT_EQ(sailor->Y(), 7);
    EXPECT_EQ(scn::TerrainAt(game->GetMap(), sailor->X(), sailor->Y()), movement::kShore);
    EXPECT_TRUE(sailor->IsAtSea());
    EXPECT_EQ(walk_result.spent, movement::kCost[movement::kShore]);
    EXPECT_EQ(sailor->Moves(), 0);
    EXPECT_EQ(walk_result.stopped, movement::WalkStop::kOutOfMoves);

    sailor->SetMoves(20);
    walk_result = movement::MoveTo(*game, {sailor}, 86, 4);
    EXPECT_EQ(sailor->Y(), 4);
    EXPECT_TRUE(sailor->IsAtSea());
    EXPECT_EQ(sailor->Moves(), 20 - (7 - 4) * movement::kCost[movement::kWater]);
    auto open_path = movement::FindPath(*game, {sailor}, 86, 4, 86, 5);
    EXPECT_EQ(open_path ? (*open_path)[0].cost : kNone, movement::kCost[movement::kWater]);

    walk_result = movement::MoveTo(*game, {sailor}, mirea->X(), mirea->Y());
    EXPECT_EQ(sailor->X() * 1000 + sailor->Y(), mirea->X() * 1000 + mirea->Y());
    EXPECT_FALSE(sailor->IsAtSea());
    EXPECT_EQ(sailor->Moves(), 0);

    ASSERT_NE(kuuria, nullptr);
    int sea_x = kNone;
    int sea_y = kNone;
    for (int x = kuuria->X() - 1; x <= kuuria->X() + 2 && sea_x == kNone; ++x) {
        for (int y = kuuria->Y() - 1; y <= kuuria->Y() + 2 && sea_x == kNone; ++y) {
            int terrain = scn::TerrainAt(game->GetMap(), x, y);
            if ((terrain == movement::kWater || terrain == movement::kShore) &&
                !game->GetMap().IsCrossing(x, y) && game::ArmiesAt(*game, x, y).empty()) {
                sea_x = x;
                sea_y = y;
            }
        }
    }
    ASSERT_NE(sea_x, kNone);
    sailor->PlaceAt(TilePos(sea_x, sea_y));
    sailor->SetAtSea(true);
    sailor->SetMoves(20);
    EXPECT_TRUE(movement::FindPath(*game, {sailor}, sea_x, sea_y, kuuria->X(), kuuria->Y())
                        .has_value());
}

}  // namespace
}  // namespace w2
