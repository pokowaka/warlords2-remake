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
#include "warlords/ai/classic_ai_player.hpp"
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
    for (Side* player : game.Sides()) {
        owned_at_start[player->Index()] = static_cast<int>(game::SideCities(game, *player).size());
    }
    while (side != nullptr && game.Turn() <= param.turns) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    EXPECT_TRUE(game.Turn() > param.turns || side == nullptr);

    std::map<int, int> per_tile;
    for (const Army* army : game.Armies()) {
        EXPECT_NE(game.ArmyTypes().ById(army->TypeIndex()), nullptr);
        if (!army->InTransit()) {
            EXPECT_TRUE(army->X() != kNone && army->Y() != kNone);
            EXPECT_TRUE(army->X() >= 0 && army->X() < game.GetMap().Width() && army->Y() >= 0 &&
                        army->Y() < game.GetMap().Height());
            ++per_tile[army->Y() * game.GetMap().Width() + army->X()];
        }
        EXPECT_GE(army->Moves(), 0);
        EXPECT_LE(army->Moves(), rules::kMaxMove);
    }
    int worst_stack = 0;
    for (const auto& [tile_idx, tile_count] : per_tile) {
        (void)tile_idx;
        worst_stack = std::max(worst_stack, tile_count);
    }
    EXPECT_LE(worst_stack, rules::kMaxStack);

    for (Side* player : game.Sides()) {
        EXPECT_GE(player->Gold(), 0) << player->Name();
        if (!player->IsAlive()) {
            EXPECT_TRUE(game::SideCities(game, *player).empty());
        }
    }
    for (const auto& city : game.GetMap().Cities()) {
        if (!city.IsNeutral()) {
            EXPECT_NE(game.GetMap().SideById(city.Owner()), nullptr);
        }
        EXPECT_LE(city.Slots().size(), 4U);
    }

    bool cities_changed = false;
    int captures = 0;
    for (Side* player : game.Sides()) {
        int current_count = static_cast<int>(game::SideCities(game, *player).size());
        if (current_count != owned_at_start[player->Index()]) {
            cities_changed = true;
        }
        captures += std::max(0, current_count - owned_at_start[player->Index()]);
    }
    EXPECT_TRUE(cities_changed);
    EXPECT_GT(captures, 0);
    EXPECT_GT(game.Armies().size(), game.GetMap().Cities().size());
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
    Side& rabid_side = rabid_game->GetMap().Sides()[1];
    const AIData& rabid_ai = ClassicAiPlayer::EnsureOn(*rabid_game, rabid_side).Data();
    EXPECT_EQ(rabid_side.Card(), 2);
    EXPECT_EQ(rabid_side.Name(), "Rabids");
    EXPECT_EQ(rabid_ai.max_groups, 2);
    EXPECT_EQ(rabid_ai.raze, 14);
    EXPECT_EQ(rabid_ai.sack, 100);
    EXPECT_EQ(rabid_ai.early, 1);
    EXPECT_EQ(ClassicAiPlayer::EnsureOn(*rabid_game, rabid_game->GetMap().Sides()[2])
                      .Data()
                      .max_groups,
              4);

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
    Side& knight = game.GetMap().Sides()[3];
    Side& lord = game.GetMap().Sides()[1];
    Side& warlord = game.GetMap().Sides()[2];
    const AIData& knight_ai = ClassicAiPlayer::EnsureOn(game, knight).Data();
    const AIData& lord_ai = ClassicAiPlayer::EnsureOn(game, lord).Data();
    AIData& warlord_ai = ClassicAiPlayer::EnsureOn(game, warlord).MutableData();
    EXPECT_EQ(knight.Level(), 0);
    EXPECT_EQ(knight_ai.max_groups, 1);
    EXPECT_EQ(knight_ai.cautious, 1);
    EXPECT_FALSE(knight_ai.bold);
    EXPECT_EQ(lord_ai.max_groups, 3);
    EXPECT_TRUE(lord_ai.bold);
    EXPECT_EQ(warlord_ai.max_groups, 4);
    EXPECT_GE(warlord_ai.human_share, 31);
    EXPECT_LE(warlord_ai.human_share, 40);
    EXPECT_GE(knight_ai.human_share, 81);
    EXPECT_LE(knight_ai.human_share, 90);
    EXPECT_EQ(game.GetMap().FightOrder()[3][8], warlord_card->fight_order[8]);

    int unclaimed = 0;
    for (const auto& city : game.GetMap().Cities()) {
        if (!city.Claim().IsValid() || city.Claim() == SideId(ai::core::kNeutral)) {
            ++unclaimed;
        }
    }
    EXPECT_EQ(unclaimed, 0);
    for (Side* player : game.Sides()) {
        EXPECT_EQ(player->CapitalCity(game.GetMap())->Claim(), player->Id()) << player->Name();
        EXPECT_GE(player->DiploScore(), 1);
        EXPECT_LE(player->DiploScore(), 8);
    }

    GameOpts greatest_opts = opts;
    greatest_opts.seed = 72;
    greatest_opts.greatest = true;
    greatest_opts.options = {{"diplomacy", 1}};
    auto greatest = SetupGame("ERYTHEA", greatest_opts);
    EXPECT_GT(greatest->GetMap().Sides()[0].DiploScore(), 400);
    EXPECT_LE(greatest->GetMap().Sides()[1].DiploScore(), 8);
    greatest->Diplomacy().SetSymmetricState(SideId(1), SideId(0), diplomacy::kWar);
    auto fights = ai::groups::FightingHumans(*greatest);
    EXPECT_TRUE(fights[1]);
    ai::diplo::Phase(*greatest, greatest->GetMap().Sides()[1]);
    EXPECT_EQ(diplomacy::Proposal(*greatest, 1, 2), diplomacy::kPeace);

    GameOpts hidden_opts = opts;
    hidden_opts.seed = 73;
    hidden_opts.options = {{"hiddenMap", 1}};
    auto hidden = SetupGame("ERYTHEA", hidden_opts);
    Side& human_side = hidden->GetMap().Sides()[0];
    Side& comp_side = hidden->GetMap().Sides()[1];
    auto human_grid = movement::Grid(*hidden, human_side.Index());
    auto human_prep =
            movement::Prepare(*hidden, human_grid, human_side.Index(), movement::kLand, 0, 0);
    auto comp_prep =
            movement::Prepare(*hidden, human_grid, comp_side.Index(), movement::kLand, 0, 0);
    int fogged = 0;
    int map_cells = hidden->GetMap().Width() * hidden->GetMap().Height();
    for (int cell = 0; cell < map_cells; ++cell) {
        if (human_prep[cell] == movement::kShut && comp_prep[cell] != movement::kShut) {
            ++fogged;
        }
    }
    EXPECT_GT(fogged, 1000);

    Army* warlord_army = nullptr;
    for (Army* candidate : game.Armies()) {
        if (candidate->OwnedBy(warlord.Id())) {
            warlord_army = candidate;
        }
    }
    ASSERT_NE(warlord_army, nullptr);
    auto selected = ai::core::Select(game, {warlord_army});
    EXPECT_EQ(ai::core::Odds(game, selected, 0, 0), 100);
    auto [neighbours, distances] = ai::core::FindNeighbours(game, game.GetMap().Cities()[0]);
    EXPECT_EQ(neighbours.size(), 6U);
    for (int distance : distances) {
        EXPECT_LT(distance, 100);
    }

    EXPECT_EQ(ai::groups::PickEnemy(game, warlord), kNone);
    warlord_ai.battles[4] = 3;
    warlord_ai.cities_lost[4] = 2;
    EXPECT_NE(ai::groups::PickEnemy(game, warlord), kNone);

    City* target_city = game.GetMap().CityAt(0);
    for (auto& candidate : game.GetMap().Cities()) {
        if (candidate.Id() != knight.CapitalCityId() && candidate.Id() != warlord.CapitalCityId()) {
            target_city = &candidate;
            break;
        }
    }
    target_city->SetOwner(warlord.Id());
    int score_before = warlord.DiploScore();
    EXPECT_TRUE(ai::groups::EarlyVengeance(game, warlord.Index(), *target_city, 0));
    EXPECT_GT(warlord.DiploScore(), score_before);
    EXPECT_FALSE(ai::groups::EarlyVengeance(game, knight.Index(), *target_city, 0));

    Side* current_side = game::Begin(game);
    while (current_side != nullptr && game.Turn() <= 12) {
        if (current_side->IsComputerControlled()) {
            PlayAi(game, *current_side);
        }
        current_side = EndTurn(game);
    }
    auto grp = warlord_ai.groups[1];
    grp->active = 2;
    grp->target = 1;
    grp->rally = warlord.CapitalCityId().Value();
    grp->staged[2] = game.Armies()[game.Armies().size() - 1];

    auto reloaded = *save::Decode(save::Encode(game), kData);
    ASSERT_NE(reloaded, nullptr);
    const AIData& reloaded_ai =
            ClassicAiPlayer::EnsureOn(*reloaded, reloaded->GetMap().Sides()[warlord.Index()])
                    .Data();
    EXPECT_EQ(reloaded_ai.max_groups, warlord_ai.max_groups);
    EXPECT_EQ(reloaded_ai.groups[1]->rally, warlord.CapitalCityId().Value());
    EXPECT_EQ(reloaded_ai.groups[1]->staged[2], reloaded->Armies()[reloaded->Armies().size() - 1]);
    EXPECT_EQ(reloaded->GetMap().Cities()[4].Claim(), game.GetMap().Cities()[4].Claim());
}

}  // namespace
}  // namespace w2
