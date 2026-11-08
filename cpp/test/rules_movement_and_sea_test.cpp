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

    const auto& grid = movement::Grid(game, side->index);
    int roaded = 0;
    bool road_cost_ok = true;
    for (int y = 0; y < game.map->height; ++y) {
        for (int x = 0; x < game.map->width; ++x) {
            if (scn::RoadAt(*game.map, x, y) % 0x20 != 0) {
                ++roaded;
                if (grid[y * game.map->width + x] % 8 != 1) road_cost_ok = false;
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

    army->moves = 7;
    Army slow_army;
    Army fast_army;
    slow_army.moves = 3;
    fast_army.moves = 9;
    EXPECT_EQ(movement::StackMoves({army, &slow_army, &fast_army}), 3);

    army->moves = army->max_moves;
    int from_x = army->x;
    int from_y = army->y;
    bool found = false;
    int target_x = 0;
    int target_y = 0;
    for (int dx = -6; dx <= 6 && !found; ++dx) {
        for (int dy = -6; dy <= 6 && !found; ++dy) {
            int x = from_x + dx;
            int y = from_y + dy;
            if (!game.map->InBounds(x, y) || game::CityAt(game, x, y)) continue;
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
    EXPECT_TRUE(found);
    if (found) {
        int before = army->moves;
        auto result = movement::MoveTo(game, stack, target_x, target_y);
        EXPECT_GT(result.steps, 0);
        EXPECT_EQ(army->moves, std::max(0, before - result.spent));
        EXPECT_TRUE(army->x != from_x || army->y != from_y);
    }

    army->moves = 0;
    EXPECT_EQ(movement::MoveTo(game, stack, from_x, from_y + 1).steps, 0);

    City* enemy = FindEnemyCity(game, side->index);
    ASSERT_NE(enemy, nullptr);
    army->moves = 99;
    army->x = enemy->x;
    army->y = enemy->y - 1;
    auto attack_path = movement::FindPath(game, stack, army->x, army->y, enemy->x, enemy->y);
    if (attack_path && !attack_path->empty()) {
        auto walk = movement::Walk(game, stack, *attack_path);
        EXPECT_EQ(walk.stopped, "attack");
        EXPECT_TRUE(walk.attack && walk.attack->city == enemy);
        EXPECT_EQ(army->x, enemy->x);
        EXPECT_EQ(army->y, enemy->y - 1);
    }
    EXPECT_EQ(movement::Grid(game, side->index)[enemy->y * game.map->width + enemy->x] % 8, 0);

    Army* scout = AddArmy(game, army->x, army->y, side->index, 11, 9, 20, 20);
    scout->upkeep = 1;
    int far_x = 0;
    int far_y = 0;
    std::optional<movement::Preview> route;
    for (int distance = 12; distance <= 30; distance += 2) {
        far_x = std::min(game.map->width - 2, scout->x + distance);
        far_y = scout->y;
        route = movement::PreviewRoute(game, {scout}, far_x, far_y);
        if (route && route->reach < static_cast<int>(route->path.size())) break;
    }
    if (route) {
        EXPECT_FALSE(route->path.empty());
        EXPECT_LE(route->reach, static_cast<int>(route->path.size()));
        EXPECT_EQ(route->path.back().x, far_x);
        EXPECT_EQ(route->path.back().y, far_y);
        int prev_x = scout->x;
        int prev_y = scout->y;
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
            EXPECT_EQ(scout_walk.walked.back().first, scout->x);
            EXPECT_EQ(scout_walk.walked.back().second, scout->y);
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
    int target_x = army->x;
    int target_y = army->y - 1 >= 0 ? army->y - 1 : army->y + 1;
    for (int i = 0; i < rules::kMaxStack; ++i) {
        Army* filler = AddArmy(game, target_x, target_y, side->index, army->type, 1, 10, 10);
        filler->home_city = army->home_city;
    }
    EXPECT_EQ(static_cast<int>(game::ArmiesAt(game, target_x, target_y).size()), rules::kMaxStack);
    army->moves = 99;
    EXPECT_EQ(movement::MoveTo(game, {army}, target_x, target_y).steps, 0);
    EXPECT_TRUE(army->x != target_x || army->y != target_y);
}

TEST(MovementTest, ShipsDisembarkAtTurnStartAndSinkStrandedLandUnits) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(3));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* city = game::SideCities(game, *side)[0];
    Army* sailor = AddArmy(game, city->x, city->y, side->index, 2, 3, 0, 14);
    sailor->at_sea = true;
    AdvanceRounds(game, 1);
    EXPECT_FALSE(sailor->at_sea);
    EXPECT_EQ(sailor->moves, sailor->max_moves);

    TilePos water = FindTerrainTile(game, movement::kWater, true);
    ASSERT_TRUE(water.IsValid());
    Army* walker = AddArmy(game, water.x, water.y, side->index, 2, 3, 14, 14);
    Army* boat = AddArmy(game, water.x, water.y, side->index, 2, 3, 14, 14);
    boat->at_sea = true;
    Army* hero = AddArmy(game, water.x, water.y, side->index, armytype::kHero, 5, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 1);
    EXPECT_FALSE(game.Alive(walker));
    EXPECT_TRUE(game.Alive(boat) && game.Alive(hero));

    int flyer_type = kNone;
    for (const auto& army_type : game.types.all) {
        if (flyer_type == kNone && army_type.flies) flyer_type = army_type.id;
    }
    Army* bird = AddArmy(game, water.x, water.y, side->index, flyer_type, 3, 14, 14);
    Army* walker2 = AddArmy(game, water.x, water.y, side->index, 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side), 1);
    EXPECT_TRUE(game.Alive(bird) && !game.Alive(walker2));

    Item wings;
    wings.type = rules::kItemFlight;
    hero->items.push_back(&wings);
    Army* carried = AddArmy(game, water.x, water.y, side->index, 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 0);
    hero->items.clear();
    EXPECT_EQ(game::SinkStranded(game, *side, water.x, water.y), 1);
    EXPECT_FALSE(game.Alive(carried));

    Army* ashore = AddArmy(game, city->x, city->y, side->index, 2, 3, 14, 14);
    EXPECT_EQ(game::SinkStranded(game, *side, city->x, city->y), 0);
    EXPECT_TRUE(game.Alive(ashore));
}

TEST(MovementTest, LastMovePointBuysRoadStepAndChargesAttackMinimumTwo) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(11));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* army = game::SideArmies(game, *side)[0];
    int road_x = kNone;
    int road_y = kNone;
    for (int y = 1; y < game.map->height - 1 && road_x == kNone; ++y) {
        for (int x = 1; x < game.map->width - 1 && road_x == kNone; ++x) {
            if (scn::RoadAt(*game.map, x, y) % 0x20 == 0 ||
                scn::RoadAt(*game.map, x + 1, y) % 0x20 == 0) {
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
    army->x = road_x;
    army->y = road_y;
    army->at_sea = false;
    army->moves = 1;
    movement::Invalidate(game);
    auto walk_result = movement::MoveTo(game, {army}, road_x + 1, road_y);
    EXPECT_EQ(walk_result.steps, 1);
    EXPECT_EQ(army->x, road_x + 1);
    EXPECT_EQ(army->moves, 0);

    City* enemy = FindEnemyCity(game, side->index);
    ASSERT_NE(enemy, nullptr);
    Army* attacker = AddArmy(game, enemy->x, enemy->y - 1, side->index, 11, 3, 1, 20);
    game::ChargeAttack(game, {attacker}, enemy->x, enemy->y);
    EXPECT_EQ(attacker->moves, 0);
    attacker->moves = 10;
    game::ChargeAttack(game, {attacker}, enemy->x, enemy->y);
    EXPECT_EQ(attacker->moves, 8);
}

TEST(MovementTest, HiddenMapSightRadiiPathBlockingAndNextHumanScreen) {
    auto off = SetupGame("ERYTHEA", Seed(101));
    EXPECT_TRUE(game::Seen(*off, 0, 5, 5));
    EXPECT_EQ(game::Reveal(*off, 0, 5, 5, false), 0);

    auto game_ptr = SetupGame("ERYTHEA", SeedWith(101, {{"hiddenMap", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    EXPECT_TRUE(game::Seen(game, side->index, side->capital->x, side->capital->y));
    EXPECT_FALSE(game::Seen(game, side->index, 0, 0));

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
    City& first_city = dark_game->map->cities[0];
    EXPECT_EQ(game::Reveal(*dark_game, 21, first_city.x, first_city.y, false), 25);

    Army* army = game::SideArmies(game, *side)[0];
    army->moves = 99;
    City* unseen = nullptr;
    for (auto& candidate : game.map->cities) {
        if (!unseen && !game::Seen(game, side->index, candidate.x, candidate.y) &&
            candidate.owner_index != side->index) {
            unseen = &candidate;
        }
    }
    ASSERT_NE(unseen, nullptr);
    EXPECT_FALSE(movement::FindPath(game, {army}, army->x, army->y, unseen->x, unseen->y));

    auto count_seen = [&]() {
        int count = 0;
        auto explored = game.explored.find(side->index);
        if (explored != game.explored.end()) {
            for (uint8_t tile_seen : explored->second) {
                if (tile_seen != 0) ++count;
            }
        }
        return count;
    };
    int seen_before = count_seen();
    for (int dx = -3; dx <= 3; ++dx) {
        bool walked = false;
        for (int dy = -3; dy <= 3 && !walked; ++dy) {
            int x = army->x + dx;
            int y = army->y + dy;
            if (x >= 0 && y >= 0 && game::Seen(game, side->index, x, y) &&
                (x != army->x || y != army->y)) {
                auto path = movement::FindPath(game, {army}, army->x, army->y, x, y);
                if (path && !path->empty() && path->back().cost < movement::kPastShore) {
                    movement::MoveTo(game, {army}, x, y);
                    EXPECT_GT(count_seen(), seen_before);
                    walked = true;
                }
            }
        }
        if (walked) break;
    }

    Side* other = game.sides[0] != side ? game.sides[0] : game.sides[1];
    EXPECT_FALSE(game::Seen(game, other->index, side->capital->x, side->capital->y));

    auto computer_first = SetupGame("ERYTHEA", SeedWith(103, {{"hiddenMap", 1}}));
    for (size_t i = 0; i < computer_first->sides.size(); ++i) {
        computer_first->sides[i]->computer = i != 2;
    }
    EXPECT_EQ(game::Begin(*computer_first), computer_first->sides[0]);
    EXPECT_EQ(game::NextHuman(*computer_first), computer_first->sides[2]);
    computer_first->sides[2]->computer = true;
    EXPECT_EQ(game::NextHuman(*computer_first), nullptr);
}

TEST(MovementTest, EncampmentFormsOnOpenGroundWhenFortifiedAndAddsFortifyOne) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(5));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* cap = side->capital;
    Army* army = game::SideArmies(game, *side)[0];
    int x = kNone;
    int y = kNone;
    for (int radius = 2; radius <= 8 && x == kNone; ++radius) {
        for (int dx = -radius; dx <= radius && x == kNone; ++dx) {
            for (int dy = -radius; dy <= radius && x == kNone; ++dy) {
                int tile_x = cap->x + dx;
                int tile_y = cap->y + dy;
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
    ASSERT_TRUE(army != nullptr && x != kNone);
    army->x = x;
    army->y = y;
    EXPECT_FALSE(game::TowerAt(game, x, y));
    game::StartTurn(game, *side);
    EXPECT_FALSE(game::TowerAt(game, x, y));

    army->fortified = true;
    game::StartTurn(game, *side);
    EXPECT_TRUE(game::TowerAt(game, x, y));
    EXPECT_EQ(combat::Fortify(game, {}, x, y, combat::TerrainClass(game, x, y)), 1);

    EXPECT_EQ(movement::MoveTo(game, {army}, x + 1, y).steps, 1);
    EXPECT_FALSE(game::TowerAt(game, x, y));

    army->x = cap->x;
    army->y = cap->y;
    game::StartTurn(game, *side);
    EXPECT_FALSE(game::TowerAt(game, cap->x, cap->y));
}

TEST(MovementTest, EmbarkingDisembarkingFlyingOverWaterAndPortSailing) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(81));
    Game* game = game_ptr.get();
    int land_x = kNone;
    int land_y = kNone;
    int water_x = kNone;
    int water_y = kNone;
    for (int y = 1; y <= game->map->height - 2 && land_x == kNone; ++y) {
        for (int x = 1; x <= game->map->width - 2 && land_x == kNone; ++x) {
            int next_terrain = scn::TerrainAt(*game->map, x + 1, y);
            if (scn::TerrainAt(*game->map, x, y) == movement::kPlain &&
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
    for (const auto& army_type : game->types.all) {
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
    EXPECT_EQ(hero->x, water_x);
    EXPECT_TRUE(!dragon_army->at_sea && !hero->at_sea);
    EXPECT_EQ(movement::ModeOf(*game, stack), movement::kFlying);
    movement::Walk(*game, stack, {movement::Step{land_x, land_y, 2}});
    EXPECT_EQ(hero->x, land_x);

    Army* foot = AddArmy(*game, land_x, land_y, 0, 1, 5, 20, 20);
    movement::SettleSea(*game, {foot}, water_x, water_y, movement::kLand, false);
    EXPECT_TRUE(foot->at_sea);
    movement::SettleSea(*game, {foot}, land_x, land_y, movement::kLand, true);
    EXPECT_FALSE(foot->at_sea);
    movement::SettleSea(*game, {hero, dragon_army, foot}, water_x, water_y, movement::kLand, false);
    EXPECT_FALSE(dragon_army->at_sea);
    EXPECT_TRUE(hero->at_sea && foot->at_sea);
    hero->at_sea = false;
    foot->at_sea = false;
    movement::SettleSea(*game, {foot}, water_x, water_y, movement::kBoat, false);
    EXPECT_FALSE(foot->at_sea);

    hero->x = water_x;
    hero->y = water_y;
    dragon_army->x = water_x;
    dragon_army->y = water_y;
    hero->at_sea = true;
    dragon_army->at_sea = true;
    game->Remove(foot);
    auto reloaded = *save::Decode(save::Encode(*game), kData);
    for (const Army* loaded_army : reloaded->armies) {
        if (loaded_army->x == water_x && loaded_army->y == water_y) {
            EXPECT_FALSE(loaded_army->at_sea);
        }
    }

    auto game2_ptr = SetupGame("ERYTHEA", Seed(81));
    game = game2_ptr.get();
    City* mirea = FindCityByName(*game, "Mirea");
    City* kuuria = FindCityByName(*game, "Kuuria");
    ASSERT_NE(mirea, nullptr);
    EXPECT_TRUE(mirea->owner_index == 0 && movement::IsPort(*game, *mirea));
    const auto& grid = movement::Grid(*game, 0);
    auto grid_byte = [&](int x, int y) { return grid[y * game->map->width + x]; };
    EXPECT_EQ(scn::TerrainAt(*game->map, 86, 7), movement::kShore);
    EXPECT_TRUE(movement::Has(grid_byte(86, 7), movement::kWaterF));
    EXPECT_FALSE(movement::StepCost(grid_byte(83, 9), grid_byte(82, 9), movement::kLand, false,
                                    false, 10)
                         .has_value());

    Army* sailor = AddArmy(*game, mirea->x, mirea->y, 0, 1, 5, 20, 20);
    auto walk_result = movement::MoveTo(*game, {sailor}, 86, 4);
    EXPECT_EQ(sailor->y, 7);
    EXPECT_EQ(scn::TerrainAt(*game->map, sailor->x, sailor->y), movement::kShore);
    EXPECT_TRUE(sailor->at_sea);
    EXPECT_EQ(walk_result.spent, movement::kCost[movement::kShore]);
    EXPECT_EQ(sailor->moves, 0);
    EXPECT_EQ(walk_result.stopped, "out of moves");

    sailor->moves = 20;
    walk_result = movement::MoveTo(*game, {sailor}, 86, 4);
    EXPECT_EQ(sailor->y, 4);
    EXPECT_TRUE(sailor->at_sea);
    EXPECT_EQ(sailor->moves, 20 - (7 - 4) * movement::kCost[movement::kWater]);
    auto open_path = movement::FindPath(*game, {sailor}, 86, 4, 86, 5);
    EXPECT_EQ(open_path ? (*open_path)[0].cost : kNone, movement::kCost[movement::kWater]);

    walk_result = movement::MoveTo(*game, {sailor}, mirea->x, mirea->y);
    EXPECT_EQ(sailor->x * 1000 + sailor->y, mirea->x * 1000 + mirea->y);
    EXPECT_FALSE(sailor->at_sea);
    EXPECT_EQ(sailor->moves, 0);

    ASSERT_NE(kuuria, nullptr);
    int sea_x = kNone;
    int sea_y = kNone;
    for (int x = kuuria->x - 1; x <= kuuria->x + 2 && sea_x == kNone; ++x) {
        for (int y = kuuria->y - 1; y <= kuuria->y + 2 && sea_x == kNone; ++y) {
            int terrain = scn::TerrainAt(*game->map, x, y);
            if ((terrain == movement::kWater || terrain == movement::kShore) &&
                !game->map->crossing[y * game->map->width + x] &&
                game::ArmiesAt(*game, x, y).empty()) {
                sea_x = x;
                sea_y = y;
            }
        }
    }
    ASSERT_NE(sea_x, kNone);
    sailor->x = sea_x;
    sailor->y = sea_y;
    sailor->at_sea = true;
    sailor->moves = 20;
    EXPECT_TRUE(
            movement::FindPath(*game, {sailor}, sea_x, sea_y, kuuria->x, kuuria->y).has_value());
}

}  // namespace
}  // namespace w2
