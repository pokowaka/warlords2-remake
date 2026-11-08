/**
 * @file
 * @brief GoogleTest suite for the classic 1993 computer AI, character cards, and full-game
 * invariants.
 */
#include <algorithm>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/aicard.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/rng.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::EndTurn;
using test::GameOpts;
using test::kData;
using test::MakeSideSetup;
using test::PlayAi;
using test::Seed;
using test::SetupGame;

/** @brief Parameter bundle for running a headless multi-turn Classic AI game. */
struct AiGameCase {
    const char* scenario;  ///< Scenario directory name.
    int turns;             ///< Turn limit to simulate.
};

/** @brief Parameterized fixture running multi-turn all-AI games across scenarios. */
class ClassicAiGameTest : public ::testing::TestWithParam<AiGameCase> {};

TEST_P(ClassicAiGameTest, PlaysToTurnLimitMaintainingBoardInvariants) {
    const AiGameCase param = GetParam();
    auto game_ptr = SetupGame(param.scenario, Seed(101));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    std::map<int, int> owned_at_start;
    for (Side* player : game.sides) {
        owned_at_start[player->index] = static_cast<int>(game::SideCities(game, *player).size());
    }
    while (side != nullptr && game.turn <= param.turns) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    EXPECT_TRUE(game.turn > param.turns || side == nullptr);

    std::map<int, int> per_tile;
    for (const Army* army : game.armies) {
        EXPECT_NE(game.types.ById(army->type), nullptr);
        if (!army->transit) {
            EXPECT_TRUE(army->x != kNone && army->y != kNone);
            EXPECT_TRUE(army->x >= 0 && army->x < game.map->width && army->y >= 0 &&
                        army->y < game.map->height);
            ++per_tile[army->y * game.map->width + army->x];
        }
        EXPECT_GE(army->moves, 0);
        EXPECT_LE(army->moves, rules::kMaxMove);
    }
    int worst_stack = 0;
    for (const auto& [tile_idx, tile_count] : per_tile) {
        (void)tile_idx;
        worst_stack = std::max(worst_stack, tile_count);
    }
    EXPECT_LE(worst_stack, rules::kMaxStack);

    for (Side* player : game.sides) {
        EXPECT_GE(player->gold, 0) << player->name;
        if (!player->alive) {
            EXPECT_TRUE(game::SideCities(game, *player).empty());
        }
    }
    for (const auto& city : game.map->cities) {
        if (city.owner_index != kNone) {
            EXPECT_NE(game.map->Side(city.owner_index), nullptr);
        }
        EXPECT_LE(city.slots.size(), 4U);
    }

    bool cities_changed = false;
    int captures = 0;
    for (Side* player : game.sides) {
        int current_count = static_cast<int>(game::SideCities(game, *player).size());
        if (current_count != owned_at_start[player->index]) {
            cities_changed = true;
        }
        captures += std::max(0, current_count - owned_at_start[player->index]);
    }
    EXPECT_TRUE(cities_changed);
    EXPECT_GT(captures, 0);
    EXPECT_GT(game.armies.size(), game.map->cities.size());
}

// NOLINTNEXTLINE(readability-identifier-length)
INSTANTIATE_TEST_SUITE_P(Scenarios, ClassicAiGameTest,
                         ::testing::Values(AiGameCase{"TUTORIA", 30}, AiGameCase{"ERYTHEA", 25}),
                         [](const ::testing::TestParamInfo<AiGameCase>& info) {
                             return std::string(info.param.scenario);
                         });

TEST(ComputerPlayersTest, CharacterCardsLevelsClaimsFogAndSavePersistence) {
    for (int level = 0; level <= 2; ++level) {
        EXPECT_EQ(aicard::Count(kData, level), 9) << "level " << level;
    }
    auto warlord_card = aicard::Load(kData, 2, 0);
    ASSERT_TRUE(warlord_card.has_value());
    EXPECT_EQ(warlord_card->groups, 4);
    EXPECT_EQ(warlord_card->raze, 5);
    EXPECT_EQ(warlord_card->human_share, 3);
    EXPECT_EQ(warlord_card->solidarity, 1);

    auto knight_card = aicard::Load(kData, 0, 0);
    ASSERT_TRUE(knight_card.has_value());
    EXPECT_EQ(knight_card->cautious, 1);
    EXPECT_EQ(knight_card->groups, 1);

    auto attila = aicard::Describe(kData, 2, 1);
    ASSERT_TRUE(attila.has_value());
    EXPECT_EQ(attila->first, "Attila the Hun");

    auto lords = aicard::Deck(kData, 1);
    ASSERT_EQ(lords.size(), 9U);
    EXPECT_EQ(lords[0], "Standard Lord");
    EXPECT_EQ(lords[2], "Roland the Rabid");

    GameOpts rabid_opts = Seed(70);
    rabid_opts.sides[0] = MakeSideSetup(false, 0);
    game::SideSetup rabid = MakeSideSetup(true, 1);
    rabid.card = 2;
    rabid.name = "Rabids";
    rabid_opts.sides[1] = rabid;
    auto rabid_game = SetupGame("ERYTHEA", rabid_opts);
    Side& rabid_side = rabid_game->map->sides[1];
    EXPECT_EQ(rabid_side.card, 2);
    EXPECT_EQ(rabid_side.name, "Rabids");
    EXPECT_EQ(rabid_side.ai->max_groups, 2);
    EXPECT_EQ(rabid_side.ai->raze, 14);
    EXPECT_EQ(rabid_side.ai->sack, 100);
    EXPECT_EQ(rabid_side.ai->early, 1);
    EXPECT_EQ(rabid_game->map->sides[2].ai->max_groups, 4);

    EXPECT_EQ(Rng(5).Dice(1, 0, 3), 3);
    EXPECT_EQ(ai::core::Dist(0, 0, 3, 4), 5);
    EXPECT_EQ(ai::core::Dist(0, 0, 1, 1), 1);

    GameOpts opts = Seed(71);
    for (int i = 0; i <= 7; ++i) {
        opts.sides[i] = MakeSideSetup(true, i % 3);
    }
    opts.sides[0] = MakeSideSetup(false, 0);
    auto game_ptr = SetupGame("ERYTHEA", opts);
    Game& game = *game_ptr;
    Side& knight = game.map->sides[3];
    Side& lord = game.map->sides[1];
    Side& warlord = game.map->sides[2];
    EXPECT_EQ(knight.level, 0);
    EXPECT_EQ(knight.ai->max_groups, 1);
    EXPECT_EQ(knight.ai->cautious, 1);
    EXPECT_FALSE(knight.ai->bold);
    EXPECT_EQ(lord.ai->max_groups, 3);
    EXPECT_TRUE(lord.ai->bold);
    EXPECT_EQ(warlord.ai->max_groups, 4);
    EXPECT_GE(warlord.ai->human_share, 31);
    EXPECT_LE(warlord.ai->human_share, 40);
    EXPECT_GE(knight.ai->human_share, 81);
    EXPECT_LE(knight.ai->human_share, 90);
    EXPECT_EQ(game.map->fight_order[3][8], warlord_card->fight_order[8]);

    int unclaimed = 0;
    for (const auto& city : game.map->cities) {
        if (city.claim == kNone || city.claim == ai::core::kNeutral) {
            ++unclaimed;
        }
    }
    EXPECT_EQ(unclaimed, 0);
    for (Side* player : game.sides) {
        EXPECT_EQ(player->capital->claim, player->index) << player->name;
        EXPECT_GE(player->diplo_score, 1);
        EXPECT_LE(player->diplo_score, 8);
    }

    GameOpts greatest_opts = opts;
    greatest_opts.seed = 72;
    greatest_opts.greatest = true;
    greatest_opts.options = {{"diplomacy", 1}};
    auto greatest = SetupGame("ERYTHEA", greatest_opts);
    EXPECT_GT(greatest->map->sides[0].diplo_score, 400);
    EXPECT_LE(greatest->map->sides[1].diplo_score, 8);
    greatest->diplomacy.state[1 * 8 + 0] = diplomacy::kWar;
    greatest->diplomacy.state[0 * 8 + 1] = diplomacy::kWar;
    auto fights = ai::groups::FightingHumans(*greatest);
    EXPECT_TRUE(fights[1]);
    ai::diplo::Phase(*greatest, greatest->map->sides[1]);
    EXPECT_EQ(diplomacy::Proposal(*greatest, 1, 2), diplomacy::kPeace);

    GameOpts hidden_opts = opts;
    hidden_opts.seed = 73;
    hidden_opts.options = {{"hiddenMap", 1}};
    auto hidden = SetupGame("ERYTHEA", hidden_opts);
    Side& human_side = hidden->map->sides[0];
    Side& comp_side = hidden->map->sides[1];
    auto human_grid = movement::Grid(*hidden, human_side.index);
    auto human_prep =
            movement::Prepare(*hidden, human_grid, human_side.index, movement::kLand, 0, 0);
    auto comp_prep = movement::Prepare(*hidden, human_grid, comp_side.index, movement::kLand, 0, 0);
    int fogged = 0;
    int map_cells = hidden->map->width * hidden->map->height;
    for (int cell = 0; cell < map_cells; ++cell) {
        if (human_prep[cell] == movement::kShut && comp_prep[cell] != movement::kShut) {
            ++fogged;
        }
    }
    EXPECT_GT(fogged, 1000);

    Army* warlord_army = nullptr;
    for (Army* candidate : game.armies) {
        if (candidate->owner == warlord.index) {
            warlord_army = candidate;
        }
    }
    ASSERT_NE(warlord_army, nullptr);
    auto selected = ai::core::Select(game, {warlord_army});
    EXPECT_EQ(ai::core::Odds(game, selected, 0, 0), 100);
    auto [neighbours, distances] = ai::core::FindNeighbours(game, game.map->cities[0]);
    EXPECT_EQ(neighbours.size(), 6U);
    for (int distance : distances) {
        EXPECT_LT(distance, 100);
    }

    EXPECT_EQ(ai::groups::PickEnemy(game, warlord), kNone);
    warlord.ai->battles[4] = 3;
    warlord.ai->cities_lost[4] = 2;
    EXPECT_NE(ai::groups::PickEnemy(game, warlord), kNone);

    City* target_city = &game.map->cities[0];
    for (auto& candidate : game.map->cities) {
        if (&candidate != knight.capital && &candidate != warlord.capital) {
            target_city = &candidate;
            break;
        }
    }
    target_city->owner_index = warlord.index;
    int score_before = warlord.diplo_score;
    EXPECT_TRUE(ai::groups::EarlyVengeance(game, warlord.index, *target_city, 0));
    EXPECT_GT(warlord.diplo_score, score_before);
    EXPECT_FALSE(ai::groups::EarlyVengeance(game, knight.index, *target_city, 0));

    Side* current_side = game::Begin(game);
    while (current_side != nullptr && game.turn <= 12) {
        if (current_side->computer) {
            PlayAi(game, *current_side);
        }
        current_side = EndTurn(game);
    }
    auto grp = warlord.ai->groups[1];
    grp->active = 2;
    grp->target = 1;
    grp->rally = warlord.capital->index;
    grp->staged[2] = game.armies.back();

    auto reloaded = save::Decode(save::Encode(game), kData);
    ASSERT_NE(reloaded, nullptr);
    const auto& reloaded_ai = *reloaded->map->sides[warlord.index].ai;
    EXPECT_EQ(reloaded_ai.max_groups, warlord.ai->max_groups);
    EXPECT_EQ(reloaded_ai.groups[1]->rally, warlord.capital->index);
    EXPECT_EQ(reloaded_ai.groups[1]->staged[2], reloaded->armies.back());
    EXPECT_EQ(reloaded->map->cities[4].claim, game.map->cities[4].claim);
}

}  // namespace
}  // namespace w2
