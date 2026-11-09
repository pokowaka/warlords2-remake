/**
 * @file
 * @brief GoogleTest suite for diplomacy declarations, peace treaties, ratings, and peaceful
 * movement.
 */
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::AddArmy;
using test::Seed;
using test::SeedWith;
using test::SetupGame;

TEST(DiplomacyTest, MustDeclareAndAttackAnywayTransitionsToWar) {
    auto ask_game = SetupGame("ERYTHEA", SeedWith(65, {{"diplomacy", 1}}));
    City* city = ask_game->GetMap().CityAt(1);
    EXPECT_TRUE(diplomacy::MustDeclare(*ask_game, 0, 1, nullptr));
    EXPECT_FALSE(diplomacy::MustDeclare(*ask_game, 0, kNone, city));

    ask_game->Diplomacy().SetState(SideId(0), SideId(1), diplomacy::kIntermediate);
    EXPECT_FALSE(diplomacy::MustDeclare(*ask_game, 0, 1, nullptr))
            << "armies of an uneasy side do not require war declaration";
    EXPECT_TRUE(diplomacy::MustDeclare(*ask_game, 0, 1, city));
    EXPECT_FALSE(diplomacy::MustDeclare(*ask_game, 0, 1, ask_game->GetMap().CityAt(0)))
            << "city 0 reproduces the original's > 0 check";

    ask_game->Diplomacy().SetState(SideId(0), SideId(1), diplomacy::kPeace);
    int score_before = ask_game->Sides()[0]->DiploScore();
    diplomacy::AttackAnyway(*ask_game, 0, 1);
    int cost = ask_game->Sides()[0]->DiploScore() - score_before;
    EXPECT_GE(cost, 101);
    EXPECT_LE(cost, 200);
    EXPECT_EQ(diplomacy::State(*ask_game, 0, 1), diplomacy::kWar);
    EXPECT_EQ(diplomacy::State(*ask_game, 1, 0), diplomacy::kWar);
    EXPECT_EQ(diplomacy::Proposal(*ask_game, 1, 0), diplomacy::kWar);
    EXPECT_FALSE(diplomacy::MustDeclare(*ask_game, 0, 1, city));

    auto off_opts = SetupGame("ERYTHEA", Seed(65));
    EXPECT_FALSE(diplomacy::MustDeclare(*off_opts, 0, 1, city));
}

TEST(DiplomacyTest, WarDeclarationsAndMutualPeaceTreaties) {
    auto off = SetupGame("ERYTHEA", Seed(61));
    EXPECT_EQ(off->GetMap().GameOptions().diplomacy, 0);
    EXPECT_EQ(diplomacy::State(*off, 0, 1), diplomacy::kWar);
    EXPECT_TRUE(diplomacy::MayAttack(*off, 0, 1));

    auto game_ptr = SetupGame("ERYTHEA", SeedWith(61, {{"diplomacy", 1}}));
    Game& game = *game_ptr;
    EXPECT_EQ(diplomacy::State(game, 0, 1), diplomacy::kPeace);
    EXPECT_FALSE(diplomacy::MayAttack(game, 0, 1));
    EXPECT_TRUE(diplomacy::MayAttack(game, 0, kNone));
    EXPECT_EQ(diplomacy::State(game, 0, 0), diplomacy::kPeace);

    diplomacy::Propose(game, 0, 1, diplomacy::kWar);
    auto war_msgs = diplomacy::Apply(game, *game.Sides()[0]);
    EXPECT_EQ(diplomacy::State(game, 0, 1), diplomacy::kWar);
    EXPECT_EQ(diplomacy::State(game, 1, 0), diplomacy::kWar);
    ASSERT_FALSE(war_msgs.empty());
    EXPECT_NE(war_msgs[0].find("War declared"), std::string::npos);
    EXPECT_EQ(diplomacy::Proposal(game, 1, 0), diplomacy::kWar);

    auto peace_game_ptr = SetupGame("ERYTHEA", SeedWith(62, {{"diplomacy", 1}}));
    Game& peace_game = *peace_game_ptr;
    diplomacy::Propose(peace_game, 0, 1, diplomacy::kWar);
    diplomacy::Apply(peace_game, *peace_game.Sides()[0]);
    peace_game.Diplomacy().SetProposal(SideId(1), SideId(0), kNone);
    diplomacy::Propose(peace_game, 0, 1, diplomacy::kPeace);
    diplomacy::Apply(peace_game, *peace_game.Sides()[0]);
    EXPECT_EQ(diplomacy::State(peace_game, 0, 1), diplomacy::kWar)
            << "one-sided peace proposal does not land";

    diplomacy::Propose(peace_game, 0, 1, diplomacy::kPeace);
    diplomacy::Propose(peace_game, 1, 0, diplomacy::kPeace);
    auto peace_msgs = diplomacy::Apply(peace_game, *peace_game.Sides()[0]);
    EXPECT_EQ(diplomacy::State(peace_game, 0, 1), diplomacy::kPeace);
    ASSERT_FALSE(peace_msgs.empty());
    EXPECT_NE(peace_msgs[0].find("Peace negotiated"), std::string::npos);
}

TEST(DiplomacyTest, ScoreUpdatesAndRatingTitles) {
    auto score_game_ptr = SetupGame("ERYTHEA", SeedWith(63, {{"diplomacy", 1}}));
    Game& score_game = *score_game_ptr;
    diplomacy::Propose(score_game, 0, 1, diplomacy::kWar);
    diplomacy::Apply(score_game, *score_game.Sides()[0]);
    diplomacy::Propose(score_game, 0, 1, diplomacy::kPeace);
    int before = score_game.Sides()[0]->DiploScore();
    diplomacy::ScoreUpdate(score_game, *score_game.Sides()[0]);
    int gain = score_game.Sides()[0]->DiploScore() - before;
    EXPECT_GE(gain, 11);
    EXPECT_LE(gain, 20);

    diplomacy::Propose(score_game, 1, 0, diplomacy::kPeace);
    before = score_game.Sides()[0]->DiploScore();
    diplomacy::ScoreUpdate(score_game, *score_game.Sides()[0]);
    EXPECT_EQ(score_game.Sides()[0]->DiploScore(), before);

    auto rating_game_ptr = SetupGame("ERYTHEA", Seed(63));
    Game& rating_game = *rating_game_ptr;
    for (size_t i = 0; i < rating_game.Sides().size(); ++i) {
        rating_game.Sides()[i]->SetDiploScore(static_cast<int>(i + 1) * 10);
    }
    auto ratings = diplomacy::Ratings(rating_game);
    EXPECT_EQ(ratings[rating_game.Sides()[0]->Index()], "Statesman");
    EXPECT_EQ(ratings[rating_game.Sides().back()->Index()], "Running Dog");

    const auto& ranks = diplomacy::RatingRanks(2);
    ASSERT_EQ(ranks.size(), 2U);
    EXPECT_EQ(std::string(diplomacy::kTitles[ranks[0] - 1]), "Statesman");
    EXPECT_EQ(std::string(diplomacy::kTitles[ranks[1] - 1]), "Running Dog");
}

TEST(DiplomacyTest, PeacefulMovementOverStacksAndCombatOnRazedRuins) {
    auto walk_game_ptr = SetupGame("ERYTHEA", SeedWith(64, {{"diplomacy", 1}}));
    Game& walk_game = *walk_game_ptr;
    Side* side = game::Begin(walk_game);
    Side* other = nullptr;
    for (Side* candidate : walk_game.Sides()) {
        if (candidate->Id() != side->Id()) {
            other = candidate;
        }
    }
    ASSERT_NE(other, nullptr);
    City* city = other->CapitalCity(walk_game.GetMap());
    Army* army = AddArmy(walk_game, city->X(), city->Y() - 1, side->Index(), 11, 3, 20, 20);
    army->SetUpkeep(1);
    auto path = movement::FindPath(walk_game, {army}, army->X(), army->Y(), city->X(), city->Y());
    if (path && !path->empty()) {
        auto into_city = movement::Walk(walk_game, {army}, *path);
        EXPECT_EQ(into_city.stopped, movement::WalkStop::kAtPeace);
        EXPECT_EQ(army->X(), city->X());
    }

    int map_width = walk_game.GetMap().Width();
    auto is_open = [&](int x, int y) {
        int terrain = scn::TerrainAt(walk_game.GetMap(), x, y);
        return terrain != movement::kWater && terrain != movement::kShore &&
               terrain != movement::kCity && terrain != movement::kBridge &&
               !walk_game.GetMap().CityAtFootprint(x, y) && game::ArmiesAt(walk_game, x, y).empty();
    };
    int start_x = kNone;
    int start_y = kNone;
    for (int y = 10; y <= walk_game.GetMap().Height() - 10 && start_x == kNone; ++y) {
        for (int x = 10; x <= map_width - 10 && start_x == kNone; ++x) {
            if (is_open(x, y) && is_open(x + 1, y) && is_open(x + 2, y)) {
                start_x = x;
                start_y = y;
            }
        }
    }
    ASSERT_NE(start_x, kNone);
    Army* walker = AddArmy(walk_game, start_x, start_y, side->Index(), 11, 3, 20, 20);
    Army* blocker = AddArmy(walk_game, start_x + 1, start_y, other->Index(), 11, 3, 20, 20);
    walker->SetUpkeep(1);
    blocker->SetUpkeep(1);
    movement::Path steps = {{start_x + 1, start_y, 2}, {start_x + 2, start_y, 2}};
    auto past_walk = movement::Walk(walk_game, {walker}, steps);
    EXPECT_EQ(walker->X(), start_x + 2);
    EXPECT_EQ(walker->Moves(), 16);
    EXPECT_EQ(past_walk.stopped, movement::WalkStop::kArrived);

    walker->PlaceAt(TilePos(start_x, start_y));
    walker->SetMoves(20);
    walk_game.Diplomacy().SetState(side->Id(), other->Id(), diplomacy::kWar);
    auto war_walk = movement::Walk(walk_game, {walker}, steps);
    EXPECT_EQ(war_walk.stopped, movement::WalkStop::kAttack);
    EXPECT_EQ(walker->X(), start_x);

    City* ruin = nullptr;
    for (auto& candidate : walk_game.GetMap().Cities()) {
        if (!ruin && !candidate.IsNeutral() && !candidate.OwnedBy(side->Id())) {
            ruin = &candidate;
        }
    }
    ASSERT_NE(ruin, nullptr);
    game::Raze(walk_game, walk_game.GetMap().Sides()[ruin->OwnerIndex()], *ruin, {}, true);
    std::vector<Army*> on_ruin;
    for (Army* occupant : walk_game.Armies()) {
        if (occupant->X() != kNone && ruin->ContainsTile(occupant->X(), occupant->Y())) {
            on_ruin.push_back(occupant);
        }
    }
    for (Army* occupant : on_ruin) {
        walk_game.Remove(occupant);
    }
    walk_game.Diplomacy().SetState(side->Id(), other->Id(), diplomacy::kWar);
    AddArmy(walk_game, ruin->X(), ruin->Y(), other->Index(), 11, 1, 20, 20);
    AddArmy(walk_game, ruin->X() + 1, ruin->Y() + 1, other->Index(), 11, 1, 20, 20);
    std::vector<Army*> strike;
    for (int i = 0; i < 8; ++i) {
        strike.push_back(
                AddArmy(walk_game, ruin->X() - 1, ruin->Y(), side->Index(), 11, 9, 20, 20));
    }
    auto fight = game::ResolveAttack(walk_game, strike, ruin->X(), ruin->Y());
    EXPECT_EQ(fight.lines.city, nullptr);
    EXPECT_EQ(fight.lines.defenders.size(), 1U);
    EXPECT_EQ(fight.captured, nullptr);
    EXPECT_TRUE(ruin->IsNeutral());
}

}  // namespace
}  // namespace w2
