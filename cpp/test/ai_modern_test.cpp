/**
 * @file
 * @brief GoogleTest suite for the modern learnable AI, SideBelief, PolicyNetwork, and fair-vision
 * rules.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "test/opponents.hpp"
#include "test/test_helpers.hpp"
#include "warlords/ai.hpp"
#include "warlords/armytype.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/rules.hpp"
#include "warlords/site.hpp"
#include "warlords/save.hpp"
#include "warlords/scn.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::GameOpts;
using test::kData;
using test::ScopedDefaultAi;
using test::ScopedTempPath;
using test::Seed;
using test::SeedWith;
using test::SetupGame;

TEST(ModernAiTest, PersonalityProfilesFogPathblockingAndAlliedDiplomacy) {
    for (int level = 0; level < 3; ++level) {
        for (int card = 0; card < 9; ++card) {
            const auto& profile = ai::personality::ProfileFor(level, card);
            EXPECT_TRUE(!profile.name.empty() && !profile.archetype.empty() &&
                        !profile.motto.empty());
            for (float trait : profile.traits) {
                EXPECT_TRUE(trait >= -1.0F && trait <= 1.0F);
            }
        }
    }

    {
        auto game = SetupGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Side* side0 = game->GetMap().SideAt(0);
        side0->SetComputerControlled(true);
        ai::SetSideClassicAi(*game, *side0, false);
        int unseen_x = kNone;
        int unseen_y = kNone;
        for (int y = 5; y < game->GetMap().Height() - 5 && unseen_x == kNone; ++y) {
            for (int x = 5; x < game->GetMap().Width() - 5 && unseen_x == kNone; ++x) {
                if (!game::Seen(*game, 0, x, y) &&
                    scn::TerrainAt(game->GetMap(), x, y) == movement::kPlain) {
                    unseen_x = x;
                    unseen_y = y;
                }
            }
        }
        ASSERT_NE(unseen_x, kNone);
        auto grid = movement::Grid(*game, 0);
        auto prep = movement::Prepare(*game, grid, 0, movement::kLand, 0, 0);
        EXPECT_EQ(prep[unseen_y * game->GetMap().Width() + unseen_x], movement::kShut);
    }

    {
        auto game = SetupGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}}));
        EXPECT_FALSE(game::Seen(*game, 0, 50, 50));
        game::Reveal(*game, 1, 50, 50, false);
        EXPECT_TRUE(game::Seen(*game, 1, 50, 50));
        EXPECT_FALSE(game::Seen(*game, 0, 50, 50));

        int gold0_before = game->GetMap().Sides()[0].Gold();
        int gold1_before = game->GetMap().Sides()[1].Gold();
        diplomacy::Propose(*game, 0, 1, diplomacy::kPeace);
        diplomacy::ProposeAlliance(*game, 0, 1, true);
        diplomacy::SetGoldOffer(*game, 0, 1, 50);
        diplomacy::SetWarTarget(*game, 0, 1, 2);
        diplomacy::Propose(*game, 1, 0, diplomacy::kPeace);
        diplomacy::ProposeAlliance(*game, 1, 0, true);
        diplomacy::Apply(*game, game->GetMap().Sides()[1]);

        EXPECT_TRUE(diplomacy::IsAllied(*game, 0, 1));
        EXPECT_EQ(diplomacy::TruceTurns(*game, 0, 1), 15);
        EXPECT_EQ(game->GetMap().Sides()[0].Gold(), gold0_before - 50);
        EXPECT_EQ(game->GetMap().Sides()[1].Gold(), gold1_before + 50);
        EXPECT_EQ(diplomacy::State(*game, 1, 2), diplomacy::kWar);
        EXPECT_TRUE(game::Seen(*game, 0, 50, 50));

        auto breakdown = diplomacy::ComputeAttitude(*game, 1, 0);
        EXPECT_TRUE(!breakdown.factors.empty() && !breakdown.what_it_takes.empty());
    }
}

TEST(ModernAiTest, SideBeliefPolicyRoundTripAndMultiTurnPlay) {
    {
        auto game = SetupGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Side& side0 = game->GetMap().Sides()[0];
        side0.SetComputerControlled(true);
        ai::SetSideClassicAi(*game, side0, false);
        ai::belief::Update(*game, side0);
        const SideBelief& belief = ai::belief::Ensure(side0);
        for (const City* city : belief.visible_cities) {
            EXPECT_TRUE(ai::belief::IsCityVisible(*game, 0, *city));
        }
        uint32_t rng_before = game->GetRng().State();
        auto my_armies = game::SideArmies(*game, side0);
        if (!my_armies.empty() && !belief.visible_cities.empty()) {
            auto est = ai::belief::EstimateCombat(*game, side0, {my_armies[0]},
                                                  belief.visible_cities[0]->X(),
                                                  belief.visible_cities[0]->Y());
            EXPECT_TRUE(est.win_prob >= 0.0F && est.win_prob <= 1.0F);
        }
        EXPECT_EQ(game->GetRng().State(), rng_before);
    }

    {
        ai::ml::PolicyNetwork net;
        auto game = SetupGame("TUTORIA", Seed(1));
        Side& side0 = game->GetMap().Sides()[0];
        ai::SetSideClassicAi(*game, side0, false);
        ai::belief::Update(*game, side0);
        auto ctx = ai::ml::BuildContext(*game, side0);
        float board_val = net.EvaluateValue(ctx, ai::ml::BuildCriticFeatures(*game, side0));
        EXPECT_TRUE(std::isfinite(board_val) && std::abs(board_val) < 5.0F);

        auto flat1 = net.Flatten();
        EXPECT_EQ(static_cast<int>(flat1.size()), ai::ml::PolicyNetwork::kTotalParams);
        ScopedTempPath weights_file("w2_test_weights.json");
        ASSERT_TRUE(net.SaveJson(weights_file.Path()).ok());
        ai::ml::PolicyNetwork net2;
        ASSERT_TRUE(net2.LoadJson(weights_file.Path()).ok());
        auto flat2 = net2.Flatten();
        for (size_t i = 0; i < flat1.size(); ++i) EXPECT_NEAR(flat1[i], flat2[i], 1e-4F);
    }

    {
        GameOpts opts = SeedWith(3, {{"hiddenMap", 1}, {"viewEnemies", 0}});
        for (int i = 0; i < 8; ++i) {
            opts.sides[i].computer = true;
            opts.sides[i].level = 2;
        }
        auto game = SetupGame("TUTORIA", opts);
        for (auto& side : game->GetMap().Sides()) {
            if (side.InUse()) ai::SetSideClassicAi(*game, side, false);
        }
        Side* cur = game::Begin(*game);
        int turns_played = 0;
        int worst_stack = 0;
        while (cur != nullptr && !game->IsOver() && game->Turn() <= 8) {
            ai::PlayTurn(*game, *cur);
            ++turns_played;
            std::map<int, int> per_tile;
            for (const Army* army : game->Armies()) {
                if (army->X() != kNone && !army->InTransit()) {
                    worst_stack =
                            std::max(worst_stack,
                                     ++per_tile[army->Y() * game->GetMap().Width() + army->X()]);
                }
            }
            cur = game::EndTurn(*game);
        }
        EXPECT_GE(turns_played, 4);
        EXPECT_LE(worst_stack, rules::kMaxStack);
    }
}

TEST(ModernAiTest, PartialVisibilityFrontiersGradientsPotentialAndSampling) {
    ai::ml::PolicyNetwork fresh_net;
    float max_skip = 0.0F;
    for (float weight : fresh_net.MissionHead().w_skip)
        max_skip = std::max(max_skip, std::abs(weight));
    EXPECT_LE(max_skip, 0.25F);

    auto game_ptr = SetupGame(
            "ERYTHEA", SeedWith(7, {{"hiddenMap", 1}, {"viewEnemies", 0}, {"neutralCities", 2}}));
    Game& game = *game_ptr;
    Side& side7 = game.GetMap().Sides()[7];
    side7.SetComputerControlled(true);
    ai::SetSideClassicAi(game, side7, false);

    City* target_neutral = nullptr;
    for (auto& city : game.GetMap().Cities()) {
        if (city.IsNeutral() && !city.IsRazed() &&
            !game::Seen(game, 7, city.X() + 1, city.Y() + 1)) {
            target_neutral = &city;
            break;
        }
    }
    ASSERT_NE(target_neutral, nullptr);
    for (Army* army : game::ArmiesAt(game, target_neutral->X(), target_neutral->Y())) {
        game.Armies().Relocate(army, TilePos());
    }
    game.Explored().MarkExplored(SideId(7), game.GetMap().Width(), game.GetMap().Height(),
                                 target_neutral->X(), target_neutral->Y());
    EXPECT_TRUE(game::Seen(game, 7, target_neutral->X(), target_neutral->Y()));
    EXPECT_FALSE(game::Seen(game, 7, target_neutral->X() + 1, target_neutral->Y() + 1));
    ai::belief::Update(game, side7);
    auto my_armies = game::SideArmies(game, side7);
    ASSERT_FALSE(my_armies.empty());
    auto est = ai::belief::EstimateCombat(game, side7, {my_armies[0]}, target_neutral->X(),
                                          target_neutral->Y());
    EXPECT_EQ(est.estimated_defender_count, 0);

    const SideBelief& belief7 = ai::belief::Ensure(side7);
    City* cap7 = side7.CapitalCity(game.GetMap());
    if (cap7 != nullptr &&
        cap7->Index() < static_cast<int>(belief7.city_nearest_enemy_dist.size())) {
        EXPECT_GT(belief7.city_nearest_enemy_dist[cap7->Index()], 50.0F);
    }

    game.Explored().ResetSide(SideId(7), game.GetMap().Width(), game.GetMap().Height(), false);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 27; ++x) {
            int terr = scn::TerrainAt(game.GetMap(), x, y);
            if (terr != movement::kWater && terr != movement::kShore) {
                game::Reveal(game, 7, x, y, false);
            }
        }
    }
    ai::belief::Update(game, side7);
    bool has_water_frontier = false;
    for (const auto& frontier : ai::belief::Ensure(side7).fog_frontiers) {
        if (frontier.is_water) has_water_frontier = true;
    }
    EXPECT_TRUE(has_water_frontier);

    ai::ml::MissionInVec test_in{};
    for (size_t i = 0; i < test_in.size(); ++i)
        test_in[i] = std::sin(static_cast<float>(i + 1) * 0.37F);
    std::array<float, ai::ml::kMissionOutDim> grad_out = {0.75F, -0.45F};
    ai::ml::PolicyNetwork::MissionNet grad_analytical{};
    grad_analytical.Zero();
    fresh_net.MissionHead().Backward(test_in, grad_out, grad_analytical);
    std::vector<float> flat_grad;
    grad_analytical.ToFlat(flat_grad);
    std::vector<float> base_params;
    fresh_net.MissionHead().ToFlat(base_params);

    constexpr float kEps = 1e-3F;
    for (size_t idx = 0; idx < base_params.size(); idx += 17) {
        std::vector<float> plus_p = base_params;
        std::vector<float> minus_p = base_params;
        plus_p[idx] += kEps;
        minus_p[idx] -= kEps;
        ai::ml::PolicyNetwork::MissionNet net_plus{};
        ai::ml::PolicyNetwork::MissionNet net_minus{};
        net_plus.FromFlat(plus_p, 0);
        net_minus.FromFlat(minus_p, 0);
        auto y_plus = net_plus.Forward(test_in);
        auto y_minus = net_minus.Forward(test_in);
        float loss_plus = grad_out[0] * y_plus[0] + grad_out[1] * y_plus[1];
        float loss_minus = grad_out[0] * y_minus[0] + grad_out[1] * y_minus[1];
        EXPECT_NEAR((loss_plus - loss_minus) / (2.0F * kEps), flat_grad[idx], 1e-3F);
    }

    City* unseen_neutral = nullptr;
    for (auto& city : game.GetMap().Cities()) {
        if (city.IsNeutral() && !ai::belief::IsCityVisible(game, 7, city)) {
            unseen_neutral = &city;
            break;
        }
    }
    ASSERT_NE(unseen_neutral, nullptr);
    float phi_hidden = ai::ml::ComputeEmpireScore(game, 7);
    game::Reveal(game, 7, unseen_neutral->X(), unseen_neutral->Y(), false);
    float phi_revealed = ai::ml::ComputeEmpireScore(game, 7);
    SideId prev_owner = unseen_neutral->Owner();
    unseen_neutral->SetOwner(SideId(7));
    float phi_captured = ai::ml::ComputeEmpireScore(game, 7);
    unseen_neutral->SetOwner(prev_owner);
    EXPECT_GT(phi_revealed - phi_hidden, 0.0F);
    EXPECT_GT(phi_captured - phi_revealed, 5.0F * (phi_revealed - phi_hidden));

    side7.SetGold(200);
    for (Army* army : game::SideArmies(game, side7)) {
        army->SetMoves(army->MaxMoves());
    }
    ai::ml::RolloutSampler rec_sampler;
    ai::ml::TurnStepRecord rec_turn;
    rec_sampler.current_turn = &rec_turn;
    ai::modern::PlayTurn(game, side7, nullptr, &fresh_net, &rec_sampler);
    EXPECT_TRUE(!rec_turn.mission_steps.empty() && !rec_turn.mission_steps[0].candidates.empty());

    std::vector<float> zero_logits(41, 0.0F);
    std::vector<float> test_log_mult(41, std::log(40.0F));
    test_log_mult[0] = 0.0F;
    ai::ml::RolloutSampler mult_sampler;
    mult_sampler.rng_state = 12345U;
    mult_sampler.temperature = 1.0F;
    int hold_picks = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        if (ai::ml::SelectFromLogits(zero_logits, &mult_sampler, nullptr, &test_log_mult) == 0) {
            ++hold_picks;
        }
    }
    EXPECT_TRUE(hold_picks >= 900 && hold_picks <= 1100);
    EXPECT_EQ(ai::ml::SelectFromLogits(zero_logits, nullptr, nullptr, &test_log_mult), 0);

    std::vector<float> fog_favoured = zero_logits;
    fog_favoured[7] = std::log(40.0F) + 0.1F;
    EXPECT_EQ(ai::ml::SelectFromLogits(fog_favoured, nullptr, nullptr, &test_log_mult), 7);
    mult_sampler.temperature = 0.35F;
    int fog_seven_picks = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        if (ai::ml::SelectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 7) {
            ++fog_seven_picks;
        }
    }
    hold_picks = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        if (ai::ml::SelectFromLogits(fog_favoured, &mult_sampler, nullptr, &test_log_mult) == 0) {
            ++hold_picks;
        }
    }
    EXPECT_GT(fog_seven_picks, hold_picks);

    EXPECT_TRUE(ai::ml::SelectBinary(0.3F, nullptr));
    EXPECT_FALSE(ai::ml::SelectBinary(-0.3F, nullptr));
    mult_sampler.temperature = 1.0F;
    int yes_count = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        if (ai::ml::SelectBinary(std::log(3.0F), &mult_sampler)) ++yes_count;
    }
    EXPECT_TRUE(yes_count >= 1400 && yes_count <= 1600);
}

TEST(ModernAiTest, SpectatorVisibilityEngineSwitchingAndCroppedSmallMaps) {
    {
        GameOpts human_opts = SeedWith(11, {{"hiddenMap", 1}});
        for (int i = 0; i < 8; ++i) human_opts.sides[i].computer = i > 0;
        auto human_game = SetupGame("ERYTHEA", human_opts);
        EXPECT_TRUE(game::HasActiveHuman(*human_game));
        EXPECT_EQ(game::ViewerSide(*human_game, human_game->Sides()[0]),
                  human_game->Sides()[0]->Index());
        EXPECT_FALSE(game::Seen(*human_game, game::ViewerSide(*human_game, human_game->Sides()[0]),
                                0, 0));

        GameOpts ai_opts = SeedWith(11, {{"hiddenMap", 1}});
        for (int i = 0; i < 8; ++i) ai_opts.sides[i].computer = true;
        auto ai_game = SetupGame("ERYTHEA", ai_opts);
        EXPECT_FALSE(game::HasActiveHuman(*ai_game));
        EXPECT_EQ(game::ViewerSide(*ai_game, ai_game->Sides()[0]), kNone);
        EXPECT_TRUE(game::Seen(*ai_game, game::ViewerSide(*ai_game, ai_game->Sides()[0]), 0, 0));
        EXPECT_EQ(game::FogCell(*ai_game, game::ViewerSide(*ai_game, ai_game->Sides()[0]), 0, 0),
                  kNone);
        EXPECT_FALSE(game::Seen(*ai_game, ai_game->Sides()[0]->Index(), 0, 0));
    }

    {
        ScopedDefaultAi default_ai_guard(true);
        auto classic_game = SetupGame("ERYTHEA", Seed(13));
        EXPECT_TRUE(classic_game->Sides()[0]->IsClassicAi());
        std::string classic_json = save::Encode(*classic_game);

        ai::SetDefaultClassicAi(false);
        auto modern_game = SetupGame("ERYTHEA", Seed(13));
        EXPECT_FALSE(modern_game->Sides()[0]->IsClassicAi());
        std::string modern_json = save::Encode(*modern_game);

        auto decoded_classic = *save::Decode(classic_json, kData);
        EXPECT_TRUE(decoded_classic->Sides()[0]->IsClassicAi());

        ai::SetDefaultClassicAi(true);
        auto decoded_modern = *save::Decode(modern_json, kData);
        EXPECT_FALSE(decoded_modern->Sides()[0]->IsClassicAi());

        for (Side* side : decoded_modern->Sides()) side->SetComputerControlled(true);
        Side* cur = game::Begin(*decoded_modern);
        ASSERT_NE(cur, nullptr);
        int switched_turns = 0;
        int worst_tile = 0;
        bool positions_valid = true;
        while (cur != nullptr && !decoded_modern->IsOver() && decoded_modern->Turn() <= 6) {
            bool use_classic = decoded_modern->Turn() % 2 == 0;
            for (auto& side : decoded_modern->GetMap().Sides()) {
                ai::SetSideClassicAi(*decoded_modern, side, use_classic);
            }
            ai::PlayTurn(*decoded_modern, *cur);
            ++switched_turns;
            std::map<int, int> per_tile;
            for (const Army* army : decoded_modern->Armies()) {
                if (army->X() == kNone || army->InTransit()) continue;
                if (!decoded_modern->GetMap().InBounds(army->X(), army->Y())) {
                    positions_valid = false;
                } else {
                    worst_tile = std::max(
                            worst_tile,
                            ++per_tile[army->Y() * decoded_modern->GetMap().Width() + army->X()]);
                }
            }
            cur = game::EndTurn(*decoded_modern);
        }
        EXPECT_GE(switched_turns, 20);
        EXPECT_TRUE(positions_valid);
        EXPECT_LE(worst_tile, rules::kMaxStack);
    }

    {
        Rng map_rng(4242.0);
        randommap::Options rand_opts;
        rand_opts.data_dir = kData;
        rand_opts.rng = &map_rng;
        rand_opts.sliders = {3, 3, 2, 3};
        auto rfiles = *randommap::GenerateNow(rand_opts);

        game::NewGameOptions small_opts;
        small_opts.seed = 4242.0;
        small_opts.memory_files = &rfiles;
        small_opts.active_sides = 2;
        small_opts.crop_margin = 14;
        small_opts.options = {
            {"hiddenMap", 1}, {"diplomacy", 0}, {"neutralCities", 1}, {"quests", 1}};
        for (int i = 0; i < 8; ++i) {
            small_opts.sides[i].computer = true;
            small_opts.sides[i].level = 2;
        }
        auto small_game = *game::NewGame(kData, "RANDOM", small_opts);
        EXPECT_EQ(small_game->Sides().size(), 2U);
        EXPECT_TRUE(small_game->GetMap().Cities().size() >= 2 &&
                    small_game->GetMap().Cities().size() < 45);
        for (const auto& city : small_game->GetMap().Cities()) {
            EXPECT_GE(city.X(), small_game->GetMap().ActiveX0());
            EXPECT_LE(city.X() + 1, small_game->GetMap().ActiveX1());
            EXPECT_GE(city.Y(), small_game->GetMap().ActiveY0());
            EXPECT_LE(city.Y() + 1, small_game->GetMap().ActiveY1());
        }
        EXPECT_EQ(scn::TerrainAt(small_game->GetMap(), 0, 0), movement::kWater);
        EXPECT_TRUE(game::Seen(*small_game, small_game->Sides()[0]->Index(), 0, 0));

        ai::SetSideClassicAi(*small_game, *small_game->Sides()[0], false);
        ai::SetSideClassicAi(*small_game, *small_game->Sides()[1], true);
        Side* cur = game::Begin(*small_game);
        while (cur != nullptr && !small_game->IsOver() && !small_game->IsWon() &&
               small_game->Turn() <= 10) {
            if (cur->IsAlive()) ai::PlayTurn(*small_game, *cur);
            cur = game::EndTurn(*small_game);
        }
        EXPECT_TRUE(small_game->Turn() > 5 || small_game->IsWon() || small_game->IsOver());
    }
}

TEST(ModernAiTest, PolicyNetworkWeightsLegacyMigrationAndTemperaturePrior) {
    namespace ml = ai::ml;
    using PolicyNet = ml::PolicyNetwork;
    EXPECT_EQ(PolicyNet::kTotalParams, 10097);
    EXPECT_EQ(PolicyNet::kLegacyV7Params, 9886);
    EXPECT_EQ(PolicyNet::kLegacyV6Params, 9700);
    EXPECT_EQ(PolicyNet::kLegacyV5Params, 8866);
    EXPECT_EQ(PolicyNet::kLegacyV4Params, 8066);
    EXPECT_EQ(PolicyNet::kLegacyV3Params, 6729);

    PolicyNet net;
    EXPECT_TRUE(net.LoadJson("cpp/ai_weights.json").ok());
    EXPECT_EQ(net.Flatten().size(), 10097U);

    PolicyNet src;
    src.InitRandomWeights(7);
    auto added_ctx = [](int col) { return col >= ml::kLegacyGlobalDim && col < ml::kGlobalDim; };
    auto v8_mission_tail = [](int col) { return col >= ml::kMissionInDim - ml::kAddedV8TargetDim; };
    auto v8_econ_tail = [](int col) { return col >= ml::kEconInDim - ml::kAddedV8EconDim; };
    auto v7_mission_tail = [](int col) { return col >= ml::kMissionInDim - ml::kAddedTargetDim; };
    auto v7_econ_tail = [](int col) { return col >= ml::kEconInDim - ml::kAddedEconDim; };
    auto narrow = [](const auto& mlp, auto dropped, std::vector<float>& out) {
        using MlpType = std::decay_t<decltype(mlp)>;
        for (int row = 0; row < MlpType::kOutDim; ++row) {
            for (int col = 0; col < MlpType::kInDim; ++col) {
                if (!dropped(col)) out.push_back(mlp.w_skip[row * MlpType::kInDim + col]);
            }
        }
        for (int row = 0; row < MlpType::kH1Dim; ++row) {
            for (int col = 0; col < MlpType::kInDim; ++col) {
                if (!dropped(col)) out.push_back(mlp.w1[row * MlpType::kInDim + col]);
            }
        }
        out.insert(out.end(), mlp.b1.begin(), mlp.b1.end());
        out.insert(out.end(), mlp.w2.begin(), mlp.w2.end());
        out.insert(out.end(), mlp.b2.begin(), mlp.b2.end());
        out.insert(out.end(), mlp.w3.begin(), mlp.w3.end());
        out.insert(out.end(), mlp.b3.begin(), mlp.b3.end());
    };
    auto no_drop = [](int) { return false; };
    auto critic_dropped = [&](int in_dim) {
        return [&, in_dim](int col) { return added_ctx(col) || col >= in_dim - ml::kCriticDim; };
    };

    std::vector<float> vec_v7;
    std::vector<float> vec_v6;
    std::vector<float> vec_v5;
    std::vector<float> vec_v4;
    narrow(src.DiploHead(), no_drop, vec_v7);
    narrow(src.MissionHead(), v8_mission_tail, vec_v7);
    narrow(src.EconHead(), v8_econ_tail, vec_v7);
    narrow(src.ValueHead(), no_drop, vec_v7);
    narrow(src.StackValueHead(), no_drop, vec_v7);
    EXPECT_EQ(vec_v7.size(), 9886U);

    narrow(src.DiploHead(), no_drop, vec_v6);
    narrow(src.MissionHead(), v7_mission_tail, vec_v6);
    narrow(src.EconHead(), v7_econ_tail, vec_v6);
    narrow(src.ValueHead(), no_drop, vec_v6);
    narrow(src.StackValueHead(), no_drop, vec_v6);
    EXPECT_EQ(vec_v6.size(), 9700U);

    for (auto* out : {&vec_v5, &vec_v4}) {
        narrow(src.DiploHead(), added_ctx, *out);
        narrow(
                src.MissionHead(), [&](int col) { return added_ctx(col) || v7_mission_tail(col); },
                *out);
        narrow(src.EconHead(), [&](int col) { return added_ctx(col) || v7_econ_tail(col); }, *out);
    }
    narrow(src.ValueHead(), added_ctx, vec_v5);
    narrow(src.StackValueHead(), added_ctx, vec_v5);
    narrow(src.ValueHead(), critic_dropped(ml::kValueInDim), vec_v4);
    size_t v3_size = vec_v4.size();
    narrow(src.StackValueHead(), critic_dropped(ml::kStackValueInDim), vec_v4);
    EXPECT_EQ(vec_v5.size(), 8866U);
    EXPECT_EQ(vec_v4.size(), 8066U);
    EXPECT_EQ(v3_size, 6729U);

    ml::ContextVec ctx_vec{};
    for (int i = 0; i < ml::kContextDim; ++i) {
        ctx_vec[i] = added_ctx(i) ? 0.0F : 0.1F * static_cast<float>(i % 7) - 0.3F;
    }
    ml::CriticVec some_crit{};
    ml::CriticVec zero_crit{};
    for (int i = 0; i < ml::kCriticDim; ++i) some_crit[i] = 0.05F * static_cast<float>(i + 1);
    ml::StackVec stack_vec{};
    stack_vec[3] = 0.4F;
    ml::TargetVec target_vec{};
    target_vec[2] = 1.0F;
    target_vec[10] = 0.6F;
    ml::EconVec econ_vec{};
    econ_vec[1] = 1.0F;
    econ_vec[7] = 0.5F;
    ml::RivalVec rival_vec{};
    rival_vec[0] = 0.3F;

    PolicyNet migrated_v7;
    migrated_v7.UnflattenLegacy(vec_v7);
    EXPECT_EQ(migrated_v7.Version(), 7);
    EXPECT_NEAR(migrated_v7.ScoreMission(ctx_vec, stack_vec, target_vec).utility,
                src.ScoreMission(ctx_vec, stack_vec, target_vec).utility, 1e-5F);
    EXPECT_NEAR(migrated_v7.ScoreEconomy(ctx_vec, econ_vec), src.ScoreEconomy(ctx_vec, econ_vec),
                1e-5F);

    PolicyNet muster_net;
    muster_net.InitRandomWeights(303);
    muster_net.SetUseMuster(true);
    muster_net.MusterHead().b3[0] = 0.25F;
    ScopedTempPath tmp_muster("w2_muster_test.json");
    EXPECT_TRUE(muster_net.SaveJson(tmp_muster.Path(), 1, 0.50F).ok());
    PolicyNet muster_loaded;
    EXPECT_TRUE(muster_loaded.LoadJson(tmp_muster.Path()).ok());
    EXPECT_TRUE(muster_loaded.UseMuster());
    EXPECT_NEAR(muster_loaded.MusterHead().b3[0], 0.25F, 1e-5F);

    PolicyNet migrated_v6;
    migrated_v6.UnflattenLegacy(vec_v6);
    EXPECT_EQ(migrated_v6.Version(), 6);
    EXPECT_NEAR(migrated_v6.ScoreMission(ctx_vec, stack_vec, target_vec).utility,
                src.ScoreMission(ctx_vec, stack_vec, target_vec).utility, 1e-5F);
    EXPECT_NEAR(migrated_v6.ScoreEconomy(ctx_vec, econ_vec), src.ScoreEconomy(ctx_vec, econ_vec),
                1e-5F);

    PolicyNet migrated_v5;
    migrated_v5.InitRandomWeights(99);
    EXPECT_FALSE(migrated_v5.Flatten().empty());
    migrated_v5.UnflattenLegacy(vec_v5);
    EXPECT_NEAR(migrated_v5.ScoreMission(ctx_vec, stack_vec, target_vec).utility,
                src.ScoreMission(ctx_vec, stack_vec, target_vec).utility, 1e-5F);
    EXPECT_NEAR(migrated_v5.ScoreEconomy(ctx_vec, econ_vec), src.ScoreEconomy(ctx_vec, econ_vec),
                1e-5F);
    EXPECT_NEAR(migrated_v5.ScoreDiplomacy(ctx_vec, rival_vec).stance_logits[3],
                src.ScoreDiplomacy(ctx_vec, rival_vec).stance_logits[3], 1e-5F);
    EXPECT_NEAR(migrated_v5.EvaluateValue(ctx_vec, some_crit),
                src.EvaluateValue(ctx_vec, some_crit), 1e-5F);
    EXPECT_NEAR(
            migrated_v5.EvaluateStackValue(ml::PackStackValueInput(ctx_vec, stack_vec, some_crit)),
            src.EvaluateStackValue(ml::PackStackValueInput(ctx_vec, stack_vec, some_crit)), 1e-5F);

    ml::ContextVec cx_new = ctx_vec;
    cx_new[ml::kLegacyGlobalDim] = 0.7F;
    EXPECT_NEAR(migrated_v5.ScoreMission(cx_new, stack_vec, target_vec).utility,
                migrated_v5.ScoreMission(ctx_vec, stack_vec, target_vec).utility, 1e-5F);

    PolicyNet migrated_v4;
    migrated_v4.UnflattenLegacy(vec_v4);
    EXPECT_NEAR(migrated_v4.ScoreMission(ctx_vec, stack_vec, target_vec).utility,
                src.ScoreMission(ctx_vec, stack_vec, target_vec).utility, 1e-5F);
    EXPECT_NEAR(migrated_v4.EvaluateValue(ctx_vec, some_crit),
                src.EvaluateValue(ctx_vec, zero_crit), 1e-5F);
    EXPECT_NEAR(
            migrated_v4.EvaluateStackValue(ml::PackStackValueInput(ctx_vec, stack_vec, some_crit)),
            src.EvaluateStackValue(ml::PackStackValueInput(ctx_vec, stack_vec, zero_crit)), 1e-5F);

    std::vector<float> vec_v3(vec_v4.begin(), vec_v4.begin() + static_cast<long>(v3_size));
    PolicyNet migrated_v3;
    migrated_v3.InitRandomWeights(99);
    migrated_v3.UnflattenLegacy(vec_v3);
    ml::StackValueInVec zero_sv{};
    EXPECT_FLOAT_EQ(migrated_v3.EvaluateStackValue(zero_sv), 0.0F);

    std::vector<float> utilities(21, 0.0F);
    std::vector<float> log_mults(21, std::log(20.0F));
    log_mults[0] = 0.0F;
    ml::RolloutSampler samp;
    samp.rng_state = 1234567U;
    samp.temperature = 0.5F;
    int picks0 = 0;
    for (int i = 0; i < 2000; ++i) {
        if (ml::SelectFromLogits(utilities, &samp, nullptr, &log_mults) == 0) ++picks0;
    }
    EXPECT_TRUE(picks0 > 880 && picks0 < 1120);

    // v7 Commander & Mixture-of-Experts JSON round-trip and exact uniform-gate blend
    PolicyNet exp_a;
    PolicyNet exp_b;
    exp_a.InitRandomWeights(101);
    exp_b.InitRandomWeights(202);
    exp_b.SetUseCommander(true);
    exp_b.CommanderB() = {0.3F, -0.2F, 0.1F, 0.0F, -0.4F};
    PolicyNet moe;
    moe.Zero();
    moe.GateHead().Zero();  // zero GateNet => exact 50/50 uniform logit blend
    for (const PolicyNet* exp_ptr : {&exp_a, &exp_b}) {
        PolicyNet::ExpertHeads exp_heads;
        exp_heads.version = exp_ptr->Version();
        exp_heads.use_commander = exp_ptr->UseCommander();
        exp_heads.commander_w = exp_ptr->CommanderW();
        exp_heads.commander_b = exp_ptr->CommanderB();
        exp_heads.diplo_net = exp_ptr->DiploHead();
        exp_heads.mission_net = exp_ptr->MissionHead();
        exp_heads.econ_net = exp_ptr->EconHead();
        moe.Experts().push_back(exp_heads);
    }
    ScopedTempPath tmp_moe("w2_moe_test.json");
    EXPECT_TRUE(moe.SaveJson(tmp_moe.Path(), 3, 0.52F).ok());
    PolicyNet moe_loaded;
    EXPECT_TRUE(moe_loaded.LoadJson(tmp_moe.Path()).ok());
    EXPECT_EQ(moe_loaded.Experts().size(), 2U);
    EXPECT_TRUE(moe_loaded.Experts()[1].use_commander);
    ml::ContextVec cx_a = ctx_vec;
    ml::ContextVec cx_b = ctx_vec;
    for (int i = 0; i < 5; ++i) {
        cx_a[25 + i] = 0.0F;
        cx_b[25 + i] = std::tanh(exp_b.CommanderB()[i]);
    }
    float expected_blend = 0.5F * (exp_a.ScoreMission(cx_a, stack_vec, target_vec).utility +
                                   exp_b.ScoreMission(cx_b, stack_vec, target_vec).utility);
    EXPECT_NEAR(moe_loaded.ScoreMission(ctx_vec, stack_vec, target_vec).utility, expected_blend,
                1e-5F);
}

TEST(ModernAiTest, RaiderAndFrozenComputerPlayerStrategiesExecutePolymorphically) {
    ai::ml::PolicyNetwork frozen_net;
    EXPECT_TRUE(frozen_net.LoadJson("cpp/ai_weights.json").ok());

    GameOpts opts = Seed(21);
    for (int i = 0; i < 8; ++i) opts.sides[i].computer = true;
    auto game = SetupGame("TUTORIA", opts);
    opponents::InstallOpponent(*game, *game->Sides()[0], opponents::Kind::kRaider);
    opponents::InstallOpponent(*game, *game->Sides()[1], opponents::Kind::kFrozen, &frozen_net);
    EXPECT_TRUE(game->Sides()[0]->IsClassicAi());
    EXPECT_FALSE(game->Sides()[1]->IsClassicAi());

    Side* cur = game::Begin(*game);
    while (cur != nullptr && !game->IsOver() && game->Turn() <= 3) {
        cur->ComputerPlayer()->PlayTurn(*game, *cur);
        cur = game::EndTurn(*game);
    }
    EXPECT_FALSE(game->Sides()[1]->ComputerPlayer()->Thoughts().empty());
}

TEST(ModernAiTest, FairRoadBlockerDetourAndStandardPlantingVectoring) {
    auto road_game = SetupGame("ERYTHEA", SeedWith(21, {{"hiddenMap", 1}, {"diplomacy", 0}}));
    Map& road_map = road_game->GetMap();
    Side& side0 = road_map.Sides()[0];
    Side& side1 = road_map.Sides()[1];
    side0.SetComputerControlled(true);
    ai::SetSideClassicAi(*road_game, side0, false);

    // Find a clear 3x5 plain corridor on ERYTHEA and place a blocker on the midpoint.
    int start_x = kNone;
    int start_y = kNone;
    int mid_x = kNone;
    int mid_y = kNone;
    int dest_x = kNone;
    int dest_y = kNone;
    for (int y = 20; y < road_map.Height() - 20 && start_x == kNone; ++y) {
        for (int x = 20; x < road_map.Width() - 20 && start_x == kNone; ++x) {
            bool all_plain = true;
            for (int off_y = -1; off_y <= 1 && all_plain; ++off_y) {
                for (int off_x = -1; off_x <= 3 && all_plain; ++off_x) {
                    int terr = scn::TerrainAt(road_map, x + off_x, y + off_y);
                    if ((terr != movement::kPlain && terr != movement::kRoad) ||
                        game::CityAt(*road_game, x + off_x, y + off_y) != nullptr) {
                        all_plain = false;
                    }
                }
            }
            if (all_plain) {
                start_x = x;
                start_y = y;
                mid_x = x + 1;
                mid_y = y;
                dest_x = x + 2;
                dest_y = y;
            }
        }
    }
    ASSERT_NE(start_x, kNone);
    for (int off_y = -2; off_y <= 2; ++off_y) {
        for (int off_x = -2; off_x <= 4; ++off_x) {
            game::Reveal(*road_game, side0.Index(), start_x + off_x, start_y + off_y, false);
        }
    }
    road_map.SetRoadByte(start_x, start_y, 1);
    road_map.SetRoadByte(mid_x, mid_y, 1);
    road_map.SetRoadByte(dest_x, dest_y, 1);
    movement::Invalidate(*road_game);

    Army* runner = test::AddArmy(*road_game, start_x, start_y, side0.Index(), 11, 3, 14, 14);
    Army* blocker = test::AddArmy(*road_game, mid_x, mid_y, side1.Index(), 4, 7, 12, 12);

    auto direct_path = movement::FindPath(*road_game, {runner}, start_x, start_y, dest_x, dest_y);
    ASSERT_TRUE(direct_path.has_value() && !direct_path->empty());
    bool direct_hits_blocker = false;
    for (const auto& step : *direct_path) {
        if (step.x == mid_x && step.y == mid_y) direct_hits_blocker = true;
    }
    EXPECT_TRUE(direct_hits_blocker);

    auto clear_path =
            movement::FindPathClear(*road_game, {runner}, start_x, start_y, dest_x, dest_y);
    ASSERT_TRUE(clear_path.has_value() && !clear_path->empty());
    bool clear_hits_blocker = false;
    for (const auto& step : *clear_path) {
        if (step.x == mid_x && step.y == mid_y) clear_hits_blocker = true;
    }
    EXPECT_FALSE(clear_hits_blocker);

    road_map.MutableOptions().diplomacy = 1;
    road_game->Diplomacy().SetSymmetricState(side0.Id(), side1.Id(), diplomacy::kPeace);
    auto peace_path =
            movement::FindPathClear(*road_game, {runner}, start_x, start_y, dest_x, dest_y);
    bool peace_uses_road = false;
    if (peace_path.has_value()) {
        for (const auto& step : *peace_path) {
            if (step.x == mid_x && step.y == mid_y) peace_uses_road = true;
        }
    }
    EXPECT_TRUE(peace_uses_road);
    road_game->Remove(runner);
    road_game->Remove(blocker);

    // Side Standard planting, vectoring to PlantedStandard, and enemy-occupied bounce safety.
    auto std_game = SetupGame("ERYTHEA", Seed(33));
    Side* std_side = game::Begin(*std_game);
    ASSERT_NE(std_side, nullptr);
    std_side->SetComputerControlled(true);
    ai::SetSideClassicAi(*std_game, *std_side, false);
    std_side->SetGold(500);
    Army* hero0 = nullptr;
    if (const auto& offer = std_game->TurnNews(std_side->Id()).hero_offer) {
        hero0 = hero::Recruit(*std_game, *std_side, *offer).hero;
    }
    ASSERT_NE(hero0, nullptr);

    City* second_city = nullptr;
    for (City& city : std_game->GetMap().Cities()) {
        if (city.IsNeutral() && !city.IsRazed() && !city.Slots().empty()) {
            game::SetCityOwner(*std_game, city, std_side->Index());
            for (Army* garrison : game::ArmiesAt(*std_game, city.X(), city.Y())) {
                garrison->SetOwner(std_side->Id());
            }
            second_city = &city;
            break;
        }
    }
    ASSERT_NE(second_city, nullptr);

    TilePos cap_pos = std_side->CapitalPosition();
    int plant_x = kNone;
    int plant_y = kNone;
    for (int off_y = -4; off_y <= 5 && plant_x == kNone; ++off_y) {
        for (int off_x = -4; off_x <= 5 && plant_x == kNone; ++off_x) {
            int tile_x = cap_pos.x + off_x;
            int tile_y = cap_pos.y + off_y;
            int terr = scn::TerrainAt(std_game->GetMap(), tile_x, tile_y);
            if ((terr == movement::kPlain || terr == movement::kRoad) &&
                game::CityAt(*std_game, tile_x, tile_y) == nullptr &&
                game::ArmiesAt(*std_game, tile_x, tile_y).empty()) {
                plant_x = tile_x;
                plant_y = tile_y;
            }
        }
    }
    ASSERT_NE(plant_x, kNone);
    hero0->PlaceAt(TilePos(plant_x, plant_y));
    hero0->SetMoves(0);  // Keep hero0 on (plant_x, plant_y) through military waves.

    ai::ml::PolicyNetwork std_net;
    std_net.Zero();
    std_net.SetVersion(8);
    std_net.SetUseStandard(true);
    std_net.MusterHead().w_skip[ai::ml::kContextDim + 0] = -8.0F;
    std_net.MusterHead().b3[0] = 4.0F;
    std_net.EconHead().w_skip[ai::ml::kContextDim + 1] = 6.0F;
    std_net.EconHead().w_skip[ai::ml::kContextDim + ai::ml::kEconMusterHub] = 8.0F;

    ScopedTempPath tmp_std("w2_std_test.json");
    EXPECT_TRUE(std_net.SaveJson(tmp_std.Path(), 1, 0.5F).ok());
    ai::ml::PolicyNetwork std_loaded;
    EXPECT_TRUE(std_loaded.LoadJson(tmp_std.Path()).ok());
    EXPECT_TRUE(std_loaded.UseStandard());
    EXPECT_FALSE(std_loaded.UseMuster());

    ai::modern::PlayTurn(*std_game, *std_side, nullptr, &std_loaded, nullptr);
    EXPECT_EQ(game::StandardAt(*std_game, *std_side).first, plant_x);
    bool any_vec_std = false;
    for (City* city : game::SideCities(*std_game, *std_side)) {
        if (city->IsVectoredToStandard()) any_vec_std = true;
    }
    EXPECT_TRUE(any_vec_std);

    // Verify transit army bounces back to home city if an enemy occupies (plant_x, plant_y).
    hero0->PlaceAt(cap_pos);
    Side* foe = std_game->Sides().back();
    Army* enemy_on_flag = test::AddArmy(*std_game, plant_x, plant_y, foe->Index(), 4, 6, 0, 12);
    Army arriving;
    arriving.SetOwner(std_side->Id());
    arriving.SetType(ArmyTypeId(11));
    arriving.SetStrength(3);
    arriving.SetMaxMoves(10);
    arriving.SetMoves(10);
    arriving.SetHomeCity(std_side->CapitalCityId());
    arriving.SendIntoTransit(Transit{1, PlantedStandard{}});
    Army* arr_ptr = std_game->Add(arriving);

    int guard = 0;
    do {
        test::EndTurn(*std_game);
        guard++;
    } while (std_game->CurrentSide() != std_side && guard < 16);

    EXPECT_TRUE(arr_ptr->IsReturning());
    EXPECT_TRUE(arr_ptr->X() == kNone && arr_ptr->InTransit());
    std_game->Remove(enemy_on_flag);
}

TEST(ModernAiTest, EvolvedPersonalitiesAffineEquivalenceAndJsonRoundTrip) {
    namespace pers = ai::personality;
    namespace ml = ai::ml;

    for (int level = 0; level < 3; ++level) {
        for (int card = 0; card < 9; ++card) {
            auto weights = pers::ArchetypeWeights(pers::ProfileFor(level, card));
            float weight_sum = 0.0F;
            for (float weight : weights) {
                EXPECT_GE(weight, 0.0F);
                EXPECT_LE(weight, 1.0F);
                weight_sum += weight;
            }
            EXPECT_NEAR(weight_sum, 1.0F, 1e-4F);
        }
    }

    auto top_arch = [](int level, int card) {
        auto weights = pers::ArchetypeWeights(pers::ProfileFor(level, card));
        int best = 0;
        for (int arch_idx = 1; arch_idx < pers::kNumArchetypes; ++arch_idx) {
            if (weights[arch_idx] > weights[best]) best = arch_idx;
        }
        return best;
    };
    EXPECT_EQ(top_arch(1, 0), static_cast<int>(pers::kArchImperial));
    EXPECT_EQ(top_arch(2, 0), static_cast<int>(pers::kArchImperial));
    EXPECT_EQ(top_arch(1, 2), static_cast<int>(pers::kArchBerserker));
    EXPECT_EQ(top_arch(2, 1), static_cast<int>(pers::kArchBerserker));
    EXPECT_EQ(top_arch(0, 2), static_cast<int>(pers::kArchGuardian));
    EXPECT_EQ(top_arch(0, 3), static_cast<int>(pers::kArchGuardian));
    EXPECT_EQ(top_arch(1, 4), static_cast<int>(pers::kArchSchemer));
    EXPECT_EQ(top_arch(2, 8), static_cast<int>(pers::kArchSchemer));
    EXPECT_EQ(top_arch(2, 7), static_cast<int>(pers::kArchQuestor));
    EXPECT_EQ(top_arch(2, 3), static_cast<int>(pers::kArchQuestor));

    float temp_knight = pers::DifficultyTemperature(0, 0.02F);
    float temp_lord = pers::DifficultyTemperature(1, 0.02F);
    float temp_warlord = pers::DifficultyTemperature(2, 0.02F);
    EXPECT_GT(temp_knight, temp_lord);
    EXPECT_GT(temp_lord, temp_warlord);
    EXPECT_NEAR(temp_warlord, 0.02F, 1e-6F);

    ml::PolicyNetwork trunk;
    trunk.InitRandomWeights(777);
    for (float& val : trunk.MusterHead().w1) val = 0.05F;
    for (float& val : trunk.MusterHead().w2) val = -0.04F;
    trunk.SetHasArchetypes(true);
    trunk.SetUsePersonalities(true);

    std::array<ml::PolicyNetwork, pers::kNumArchetypes> spec_nets;
    for (int arch_idx = 0; arch_idx < pers::kNumArchetypes; ++arch_idx) {
        ml::PolicyNetwork tmp;
        tmp.InitRandomWeights(static_cast<uint32_t>(1000 + arch_idx * 37));
        spec_nets[arch_idx] = trunk;
        spec_nets[arch_idx].DiploHead().w_skip = tmp.DiploHead().w_skip;
        spec_nets[arch_idx].DiploHead().w3 = tmp.DiploHead().w3;
        spec_nets[arch_idx].DiploHead().b3 = tmp.DiploHead().b3;
        spec_nets[arch_idx].MissionHead().w_skip = tmp.MissionHead().w_skip;
        spec_nets[arch_idx].MissionHead().w3 = tmp.MissionHead().w3;
        spec_nets[arch_idx].MissionHead().b3 = tmp.MissionHead().b3;
        spec_nets[arch_idx].EconHead().w_skip = tmp.EconHead().w_skip;
        spec_nets[arch_idx].EconHead().w3 = tmp.EconHead().w3;
        spec_nets[arch_idx].EconHead().b3 = tmp.EconHead().b3;
        for (size_t i = 0; i < spec_nets[arch_idx].MusterHead().w_skip.size(); ++i) {
            spec_nets[arch_idx].MusterHead().w_skip[i] =
                    0.03F * static_cast<float>(arch_idx - 2) * static_cast<float>((i % 5) + 1);
        }
        for (size_t i = 0; i < spec_nets[arch_idx].MusterHead().w3.size(); ++i) {
            spec_nets[arch_idx].MusterHead().w3[i] = 0.05F * static_cast<float>(arch_idx - 1);
        }
        spec_nets[arch_idx].MusterHead().b3[0] = 0.1F * static_cast<float>(arch_idx);
        spec_nets[arch_idx].SetUseMuster(arch_idx == pers::kArchGuardian ||
                                         arch_idx == pers::kArchQuestor);
        spec_nets[arch_idx].SetUseStandard(arch_idx == pers::kArchQuestor);
        spec_nets[arch_idx].SetAvoidBlockers(arch_idx == pers::kArchGuardian ||
                                             arch_idx == pers::kArchSchemer);
        spec_nets[arch_idx].SetDetourBias(-0.02F * static_cast<float>(arch_idx + 1));
        trunk.Archetypes()[arch_idx] = spec_nets[arch_idx].ExtractArchetypeHead();
    }

    auto weights_robin = pers::ArchetypeWeights(pers::ProfileFor(0, 2));
    ml::PolicyNetwork robin_net = trunk.MaterializeForWeights(weights_robin);
    EXPECT_TRUE(robin_net.AvoidBlockers() && robin_net.UseMuster() && !robin_net.UseStandard());

    auto weights_attila = pers::ArchetypeWeights(pers::ProfileFor(2, 1));
    ml::PolicyNetwork attila_net = trunk.MaterializeForWeights(weights_attila);
    EXPECT_FALSE(attila_net.AvoidBlockers());
    EXPECT_FALSE(attila_net.UseStandard());

    auto weights_roger = pers::ArchetypeWeights(pers::ProfileFor(2, 7));
    ml::PolicyNetwork roger_net = trunk.MaterializeForWeights(weights_roger);
    EXPECT_TRUE(roger_net.UseStandard() && roger_net.UseMuster() && !roger_net.AvoidBlockers());

    ml::ContextVec ctx_vec{};
    ml::RivalVec rival_vec{};
    ml::StackVec stack_vec{};
    ml::TargetVec target_vec{};
    ml::EconVec econ_vec{};
    ml::MusterVec muster_vec{};
    for (size_t i = 0; i < ctx_vec.size(); ++i) {
        ctx_vec[i] = 0.1F * std::sin(static_cast<float>(i + 1));
    }
    for (size_t i = 0; i < rival_vec.size(); ++i) {
        rival_vec[i] = 0.2F * std::cos(static_cast<float>(i + 2));
    }
    for (size_t i = 0; i < stack_vec.size(); ++i) {
        stack_vec[i] = -0.15F * std::sin(static_cast<float>(i + 3));
    }
    for (size_t i = 0; i < target_vec.size(); ++i) {
        target_vec[i] = 0.25F * std::cos(static_cast<float>(i + 4));
    }
    for (size_t i = 0; i < econ_vec.size(); ++i) {
        econ_vec[i] = 0.12F * std::sin(static_cast<float>(i + 5));
    }
    for (size_t i = 0; i < muster_vec.size(); ++i) {
        muster_vec[i] = 0.18F * std::cos(static_cast<float>(i + 6));
    }

    float exp_diplo = 0.0F;
    float exp_mission = 0.0F;
    float exp_econ = 0.0F;
    float exp_muster = 0.0F;
    for (int arch_idx = 0; arch_idx < pers::kNumArchetypes; ++arch_idx) {
        exp_diplo += weights_roger[arch_idx] *
                     spec_nets[arch_idx].ScoreDiplomacy(ctx_vec, rival_vec).stance_logits[3];
        exp_mission += weights_roger[arch_idx] *
                       spec_nets[arch_idx].ScoreMission(ctx_vec, stack_vec, target_vec).utility;
        exp_econ += weights_roger[arch_idx] * spec_nets[arch_idx].ScoreEconomy(ctx_vec, econ_vec);
        exp_muster +=
                weights_roger[arch_idx] *
                spec_nets[arch_idx].ForwardMuster(ml::PackMusterInput(ctx_vec, muster_vec))[0];
    }
    EXPECT_NEAR(roger_net.ScoreDiplomacy(ctx_vec, rival_vec).stance_logits[3], exp_diplo, 1e-5F);
    EXPECT_NEAR(roger_net.ScoreMission(ctx_vec, stack_vec, target_vec).utility, exp_mission, 1e-5F);
    EXPECT_NEAR(roger_net.ScoreEconomy(ctx_vec, econ_vec), exp_econ, 1e-5F);
    EXPECT_NEAR(roger_net.ForwardMuster(ml::PackMusterInput(ctx_vec, muster_vec))[0], exp_muster,
                1e-5F);

    ScopedTempPath tmp_pers("w2_personalities_test.json");
    EXPECT_TRUE(trunk.SaveJson(tmp_pers.Path(), 5, 0.49F).ok());
    ml::PolicyNetwork loaded_pers;
    EXPECT_TRUE(loaded_pers.LoadJson(tmp_pers.Path()).ok());
    EXPECT_TRUE(loaded_pers.HasArchetypes() && loaded_pers.UsePersonalities());
    ml::PolicyNetwork reloaded_roger = loaded_pers.MaterializeForWeights(weights_roger);
    EXPECT_NEAR(reloaded_roger.ScoreMission(ctx_vec, stack_vec, target_vec).utility, exp_mission,
                1e-4F);

    game::NewGameOptions pers_opts;
    pers_opts.seed = 4242;
    pers_opts.active_sides = 2;
    pers_opts.crop_margin = 16;
    for (int i = 0; i < 8; ++i) {
        pers_opts.sides[i].computer = true;
        pers_opts.sides[i].level = 2;
        pers_opts.sides[i].card = 8;  // William the Wily
    }
    auto pers_game_res = game::NewGame(kData, "ERYTHEA", pers_opts);
    ASSERT_TRUE(pers_game_res.ok());
    Game& pers_game = **pers_game_res;
    Side* pers_cur = game::Begin(pers_game);
    ASSERT_NE(pers_cur, nullptr);
    ai::SetSideClassicAi(pers_game, *pers_cur, false);
    ai::modern::PlayTurn(pers_game, *pers_cur, nullptr, &loaded_pers, nullptr);
    bool saw_persona = false;
    for (const AIThought& thought : pers_cur->ComputerPlayer()->Thoughts()) {
        if (thought.category == "Persona" && thought.actor_name == "William the Wily") {
            saw_persona = true;
        }
    }
    EXPECT_TRUE(saw_persona);
}

TEST(ModernAiTest, WardenPlayerGarrisonsCitiesAndExecutesPolymorphically) {
    GameOpts opts = Seed(21);
    for (int i = 0; i < 8; ++i) opts.sides[i].computer = true;
    auto game = SetupGame("TUTORIA", opts);
    Side& warden_side = *game->Sides()[0];
    opponents::InstallOpponent(*game, warden_side, opponents::Kind::kWarden);
    EXPECT_TRUE(warden_side.IsClassicAi());

    Side* cur = game::Begin(*game);
    while (cur != nullptr && !game->IsOver() && game->Turn() <= 3) {
        cur->ComputerPlayer()->PlayTurn(*game, *cur);
        cur = game::EndTurn(*game);
    }
    opponents::GarrisonCities(*game, warden_side);
    EXPECT_TRUE(warden_side.ComputerPlayer() != nullptr);
}

TEST(ModernAiTest, V8EndToEndGradientsSharpSamplingAndCausalOffTurnCredit) {
    // 1. V8 FlattenTrainable / UnflattenTrainable round-trip + Commander & MusterNet gradients
    {
        ai::ml::PolicyNetwork net;
        net.SetUseCommander(true);
        net.SetUseMuster(true);
        net.SetUseStandard(true);
        net.SetAvoidBlockers(true);
        net.SetDetourBias(-0.075F);
        for (size_t idx = 0; idx < net.CommanderW().size(); ++idx) {
            net.CommanderW()[idx] = 0.08F * std::sin(static_cast<float>(idx + 1) * 0.41F);
        }
        for (size_t idx = 0; idx < net.CommanderB().size(); ++idx) {
            net.CommanderB()[idx] = 0.03F * static_cast<float>(idx) - 0.06F;
        }
        for (size_t idx = 0; idx < net.MusterHead().w1.size(); ++idx) {
            net.MusterHead().w1[idx] = 0.09F * std::cos(static_cast<float>(idx + 2) * 0.29F);
        }
        for (size_t idx = 0; idx < net.MusterHead().w2.size(); ++idx) {
            net.MusterHead().w2[idx] = 0.07F * std::sin(static_cast<float>(idx + 3) * 0.31F);
        }
        for (size_t idx = 0; idx < net.MusterHead().w3.size(); ++idx) {
            net.MusterHead().w3[idx] = 0.11F * std::cos(static_cast<float>(idx + 1) * 0.53F);
        }
        for (size_t idx = 0; idx < net.MusterHead().w_skip.size(); ++idx) {
            net.MusterHead().w_skip[idx] = 0.04F * std::sin(static_cast<float>(idx + 5) * 0.19F);
        }

        auto flat_v8 = net.FlattenTrainable();
        ASSERT_EQ(flat_v8.size(), ai::ml::PolicyNetwork::kV8TrainableParams);
        ai::ml::PolicyNetwork copy_net;
        copy_net.Zero();
        copy_net.UnflattenTrainable(flat_v8);
        EXPECT_FLOAT_EQ(copy_net.DetourBias(), -0.075F);
        for (size_t idx = 0; idx < net.CommanderW().size(); ++idx) {
            EXPECT_FLOAT_EQ(copy_net.CommanderW()[idx], net.CommanderW()[idx]);
        }
        for (size_t idx = 0; idx < net.MusterHead().w1.size(); ++idx) {
            EXPECT_FLOAT_EQ(copy_net.MusterHead().w1[idx], net.MusterHead().w1[idx]);
        }

        // Finite-difference check of BackwardWithActivations d_posture + AccumulateCommanderGrad
        ai::ml::MissionInVec m_in{};
        for (size_t idx = 0; idx < m_in.size(); ++idx) {
            m_in[idx] = 0.35F * std::sin(static_cast<float>(idx + 1) * 0.23F);
        }
        ai::ml::RefreshCommanderPosture(m_in.data(), net);
        ai::ml::PolicyNetwork::MissionNet::Activations m_act{};
        auto base_out = net.MissionHead().ForwardWithActivations(m_in, m_act);
        (void)base_out;
        std::array<float, 2> d_out = {0.65F, -0.40F};
        std::array<float, 5> d_posture{};
        ai::ml::PolicyNetwork grad_net;
        grad_net.Zero();
        net.MissionHead().BackwardWithActivations(m_in, m_act, d_out, grad_net.MissionHead(),
                                                  &d_posture);
        ai::ml::AccumulateCommanderGrad(m_in.data(), d_posture, grad_net);

        constexpr float kEps = 1e-3F;
        for (size_t idx = 0; idx < net.CommanderW().size(); idx += 11) {
            ai::ml::PolicyNetwork plus_net = net;
            ai::ml::PolicyNetwork minus_net = net;
            plus_net.CommanderW()[idx] += kEps;
            minus_net.CommanderW()[idx] -= kEps;
            ai::ml::MissionInVec in_p = m_in;
            ai::ml::MissionInVec in_m = m_in;
            ai::ml::RefreshCommanderPosture(in_p.data(), plus_net);
            ai::ml::RefreshCommanderPosture(in_m.data(), minus_net);
            auto out_p = plus_net.MissionHead().Forward(in_p);
            auto out_m = minus_net.MissionHead().Forward(in_m);
            float loss_p = d_out[0] * out_p[0] + d_out[1] * out_p[1];
            float loss_m = d_out[0] * out_m[0] + d_out[1] * out_m[1];
            EXPECT_NEAR((loss_p - loss_m) / (2.0F * kEps), grad_net.CommanderW()[idx], 1e-3F);
        }
        for (size_t idx = 0; idx < net.CommanderB().size(); ++idx) {
            ai::ml::PolicyNetwork plus_net = net;
            ai::ml::PolicyNetwork minus_net = net;
            plus_net.CommanderB()[idx] += kEps;
            minus_net.CommanderB()[idx] -= kEps;
            ai::ml::MissionInVec in_p = m_in;
            ai::ml::MissionInVec in_m = m_in;
            ai::ml::RefreshCommanderPosture(in_p.data(), plus_net);
            ai::ml::RefreshCommanderPosture(in_m.data(), minus_net);
            auto out_p = plus_net.MissionHead().Forward(in_p);
            auto out_m = minus_net.MissionHead().Forward(in_m);
            float loss_p = d_out[0] * out_p[0] + d_out[1] * out_p[1];
            float loss_m = d_out[0] * out_m[0] + d_out[1] * out_m[1];
            EXPECT_NEAR((loss_p - loss_m) / (2.0F * kEps), grad_net.CommanderB()[idx], 1e-3F);
        }

        // Verify PlayTurn records muster_steps when >= 3 owned cities exist
        auto game_ptr = SetupGame("ERYTHEA", Seed(5));
        Game& game = *game_ptr;
        Side& side0 = game.GetMap().Sides()[0];
        side0.SetComputerControlled(true);
        ai::SetSideClassicAi(game, side0, false);
        int given = 0;
        for (auto& city : game.GetMap().Cities()) {
            if (city.IsNeutral() && given < 4) {
                city.SetOwner(SideId(0));
                given++;
            }
        }
        ai::ml::RolloutSampler sampler;
        sampler.temperature = 0.5F;
        ai::ml::TurnStepRecord turn_rec;
        sampler.current_turn = &turn_rec;
        ai::modern::PlayTurn(game, side0, nullptr, &net, &sampler);
        EXPECT_FALSE(turn_rec.muster_steps.empty());
    }

    // 2. Sharp-temperature sampling + sparse epsilon exploration
    {
        std::vector<float> logits = {0.5F, 0.0F};
        ai::ml::RolloutSampler sampler;
        sampler.rng_state = 98765U;
        sampler.temperature = 0.06F;
        sampler.explore_eps = 0.15F;
        sampler.explore_temp = 0.70F;

        int explore_draws = 0;
        int sharp_second_picks = 0;
        int explore_second_picks = 0;
        for (int trial = 0; trial < 2000; ++trial) {
            float log_prob = 0.0F;
            int pick = ai::ml::SelectFromLogits(logits, &sampler, &log_prob);
            float used_temp = sampler.last_step_temp;
            float logit0 = logits[0] / used_temp;
            float logit1 = logits[1] / used_temp;
            float prob0 = 1.0F / (1.0F + std::exp(logit1 - logit0));
            float expected_lp = std::log(pick == 0 ? prob0 : (1.0F - prob0));
            EXPECT_NEAR(log_prob, expected_lp, 1e-5F);
            if (std::abs(used_temp - 0.70F) < 1e-5F) {
                explore_draws++;
                if (pick == 1) explore_second_picks++;
            } else {
                EXPECT_NEAR(used_temp, 0.06F, 1e-5F);
                if (pick == 1) sharp_second_picks++;
            }
        }
        EXPECT_TRUE(explore_draws >= 230 && explore_draws <= 370);
        EXPECT_LE(sharp_second_picks, 5);
        EXPECT_GE(explore_second_picks, 55);
    }

    // 3. Causal off-turn per-stack defensive battle credit (repelled siege vs wiped lone garrison)
    {
        auto game_ptr = SetupGame("TUTORIA", Seed(9));
        Game& game = *game_ptr;
        Side& def_side = game.GetMap().Sides()[0];
        def_side.SetComputerControlled(true);
        ai::SetSideClassicAi(game, def_side, false);
        ModernMlPlayer* mod_player = ModernMlPlayer::FindMutableOn(def_side);
        ASSERT_NE(mod_player, nullptr);

        City* def_city = def_side.CapitalCity(game.GetMap());
        ASSERT_NE(def_city, nullptr);
        for (Army* existing : game::ArmiesAt(game, def_city->X(), def_city->Y())) {
            game.Armies().Remove(existing);
        }

        // Scenario A: Strong defender garrison repels a weak attacker -> positive off-turn credit
        Army def_proto;
        def_proto.SetOwner(SideId(0));
        def_proto.PlaceAt(def_city->Position());
        def_proto.SetType(ArmyTypeId(2));
        def_proto.SetStrength(8);
        def_proto.SetMaxMoves(12);
        def_proto.SetMoves(12);
        Army* strong_def = game.Armies().Add(def_proto);

        Army atk_proto;
        atk_proto.SetOwner(SideId(1));
        atk_proto.PlaceAt(TilePos(def_city->X() - 1, def_city->Y()));
        atk_proto.SetType(ArmyTypeId(0));
        atk_proto.SetStrength(1);
        atk_proto.SetMaxMoves(12);
        atk_proto.SetMoves(12);
        Army* weak_atk = game.Armies().Add(atk_proto);

        ai::ml::TurnStepRecord repel_turn;
        mod_player->SetPendingOffTurn(&repel_turn, 0.5F);
        Battle repel_battle = game::ResolveAttack(game, {weak_atk}, def_city->X(), def_city->Y());
        mod_player->SetPendingOffTurn(nullptr);
        ASSERT_FALSE(repel_battle.won);
        ASSERT_EQ(repel_turn.off_turn_unit_deltas.size(), 1U);
        EXPECT_EQ(repel_turn.off_turn_unit_deltas[0].unit_id,
                  reinterpret_cast<std::uintptr_t>(strong_def));
        EXPECT_GT(repel_turn.off_turn_unit_deltas[0].delta_score, 0.01F);

        // Scenario B: Lone weak scout garrison is wiped out and loses the city -> negative credit
        game.Armies().Remove(strong_def);
        def_proto.SetStrength(1);
        Army* lone_scout = game.Armies().Add(def_proto);

        std::vector<Army*> heavy_attackers;
        for (int idx = 0; idx < 4; ++idx) {
            atk_proto.SetStrength(9);
            heavy_attackers.push_back(game.Armies().Add(atk_proto));
        }

        ai::ml::TurnStepRecord wipe_turn;
        mod_player->SetPendingOffTurn(&wipe_turn, 0.5F);
        Battle wipe_battle =
                game::ResolveAttack(game, heavy_attackers, def_city->X(), def_city->Y());
        mod_player->SetPendingOffTurn(nullptr);
        ASSERT_TRUE(wipe_battle.won);
        ASSERT_EQ(wipe_turn.off_turn_unit_deltas.size(), 1U);
        EXPECT_EQ(wipe_turn.off_turn_unit_deltas[0].unit_id,
                  reinterpret_cast<std::uintptr_t>(lone_scout));
        EXPECT_LT(wipe_turn.off_turn_unit_deltas[0].delta_score, -0.02F);
    }
}

TEST(ModernAiTest, EconomyFamiliesUnitLineageAndStackConcentrationShaping) {
    // Stack-concentration shaping: concentrated blessed Hero stack outscores scattered 1-stacks
    {
        auto game_ptr = SetupGame("ERYTHEA", Seed(21));
        Game& game = *game_ptr;
        while (game.Turn() < 20) {
            game.AdvanceTurn();
        }
        for (int idx = static_cast<int>(game.Armies().size()) - 1; idx >= 0; --idx) {
            game.Armies().Remove(game.Armies()[idx]);
        }

        City* city0 = game.GetMap().Sides()[0].CapitalCity(game.GetMap());
        City* city1 = game.GetMap().Sides()[1].CapitalCity(game.GetMap());
        ASSERT_NE(city0, nullptr);
        ASSERT_NE(city1, nullptr);

        // Give both sides identical city count and income so only stack organization differs
        for (auto& city : game.GetMap().Cities()) {
            if (&city == city0) {
                city.SetOwner(SideId(0));
                city.SetIncome(20);
            } else if (&city == city1) {
                city.SetOwner(SideId(1));
                city.SetIncome(20);
            } else {
                city.SetOwner(SideId::None());
            }
        }

        // Side 0: 1 Hero + 3 units concentrated together on city0, blessed once
        for (int unit_idx = 0; unit_idx < 4; ++unit_idx) {
            Army proto;
            proto.SetOwner(SideId(0));
            proto.PlaceAt(city0->Position());
            proto.SetType(ArmyTypeId(unit_idx == 0 ? armytype::kHero : 2));
            proto.SetStrength(5);
            proto.SetMaxMoves(12);
            proto.SetMoves(12);
            proto.AddBlessing(0);
            game.Armies().Add(proto);
        }

        // Side 1: 4 identical Strength-5 non-hero unblessed units scattered on separate tiles
        for (int unit_idx = 0; unit_idx < 4; ++unit_idx) {
            Army proto;
            proto.SetOwner(SideId(1));
            proto.PlaceAt(TilePos(std::max(1, city1->X() - unit_idx), city1->Y()));
            proto.SetType(ArmyTypeId(2));
            proto.SetStrength(5);
            proto.SetMaxMoves(12);
            proto.SetMoves(12);
            game.Armies().Add(proto);
        }

        ai::ml::SetConcentrationScoreWeight(0.0F);
        float base_s0 = ai::ml::ComputeEmpireScore(game, 0);
        float base_s1 = ai::ml::ComputeEmpireScore(game, 1);
        EXPECT_NEAR(base_s0, base_s1, 1e-5F);

        ai::ml::SetConcentrationScoreWeight(0.15F);
        EXPECT_NEAR(ai::ml::ConcentrationScoreWeight(), 0.15F, 1e-6F);
        float conc_s0 = ai::ml::ComputeEmpireScore(game, 0);
        float conc_s1 = ai::ml::ComputeEmpireScore(game, 1);
        EXPECT_GT(conc_s0 - conc_s1, 0.025F);
        EXPECT_NEAR(conc_s1, base_s1, 1e-5F);
        ai::ml::SetConcentrationScoreWeight(0.0F);
    }

    // Economy family tagging, newly built unit capture, and backward causal lineage propagation
    {
        auto game_ptr = SetupGame("ERYTHEA", Seed(33));
        Game& game = *game_ptr;
        Side& our_side = game.GetMap().Sides()[0];
        for (int side_idx = 0; side_idx < 8; ++side_idx) {
            Side& side = game.GetMap().Sides()[side_idx];
            if (!side.InUse()) continue;
            side.SetComputerControlled(true);
            ai::SetSideClassicAi(game, side, side_idx != 0);
        }
        our_side.SetGold(2500);
        int extra_assigned = 0;
        for (auto& city : game.GetMap().Cities()) {
            if (city.IsNeutral() && extra_assigned < 2) {
                city.SetOwner(our_side.Id());
                extra_assigned++;
            }
        }

        ai::ml::PolicyNetwork net;
        net.InitRandomWeights(321);
        net.SetVersion(8);
        net.SetUseMuster(true);

        std::vector<ai::ml::TurnStepRecord> recorded_turns;
        ai::ml::RolloutSampler sampler;
        sampler.rng_state = 777U;
        sampler.temperature = 0.80F;

        Side* cur = game::Begin(game);
        int safety = 0;
        bool saw_produced_unit = false;
        while (cur && safety++ < 64 && recorded_turns.size() < 6) {
            if (cur->IsComputerControlled() && cur->IsAlive()) {
                if (cur->Index() == 0) {
                    recorded_turns.emplace_back();
                    sampler.current_turn = &recorded_turns.back();
                    ai::modern::PlayTurn(game, *cur, nullptr, &net, &sampler);
                    sampler.current_turn = nullptr;
                    if (!recorded_turns.back().produced_units.empty()) {
                        saw_produced_unit = true;
                    }
                    // Force at least one fast production slot in capital if stopped so build ticks
                    City* cap = our_side.CapitalCity(game.GetMap());
                    if (cap && !cap->Slots().empty() && !game::Producing(*cap)) {
                        game::SetProduction(game, *cap, 0);
                        cap->SetCountdown(1);
                    }
                } else {
                    ai::PlayTurn(game, *cur, nullptr);
                }
            }
            cur = game::EndTurn(game);
        }

        ASSERT_GE(recorded_turns.size(), 3U);
        bool saw_buy_family = false;
        bool saw_prod_family = false;
        bool saw_vec_family = false;
        for (const auto& turn_rec : recorded_turns) {
            for (const auto& econ_step : turn_rec.econ_steps) {
                if (econ_step.family == ai::ml::EconFamily::kBuySlot) saw_buy_family = true;
                if (econ_step.family == ai::ml::EconFamily::kProduce) saw_prod_family = true;
                if (econ_step.family == ai::ml::EconFamily::kVector) saw_vec_family = true;
            }
        }
        EXPECT_TRUE(saw_buy_family);
        EXPECT_TRUE(saw_prod_family);
        EXPECT_TRUE(saw_vec_family);
        ASSERT_TRUE(saw_produced_unit);

        // Verify deterministic causal lineage propagation across BuySlot + Produce + Vector -> Birth
        std::vector<ai::ml::TurnStepRecord> synthetic_turns(3);
        ai::ml::EconStepRecord buy_rec;
        buy_rec.family = ai::ml::EconFamily::kBuySlot;
        buy_rec.chosen_idx = 1;
        buy_rec.city_idx = 4;
        buy_rec.unit_type = 7;
        synthetic_turns[0].econ_steps.push_back(buy_rec);

        ai::ml::EconStepRecord prod_rec;
        prod_rec.family = ai::ml::EconFamily::kProduce;
        prod_rec.chosen_idx = 1;
        prod_rec.city_idx = 4;
        prod_rec.unit_type = 7;
        synthetic_turns[0].econ_steps.push_back(prod_rec);

        ai::ml::EconStepRecord vec_rec;
        vec_rec.family = ai::ml::EconFamily::kVector;
        vec_rec.chosen_idx = 1;
        vec_rec.city_idx = 4;
        vec_rec.target_idx = 9;
        synthetic_turns[1].econ_steps.push_back(vec_rec);

        constexpr std::uintptr_t kUnitA = 0x1001U;
        synthetic_turns[2].produced_units.push_back(
                ai::ml::ProducedUnitRecord{kUnitA, 4, 7, /*vectored=*/true});

        ai::ml::UnitReturnMap birth_returns;
        birth_returns[kUnitA] = 0.20F;
        ai::ml::PropagateEconomyLineage(absl::MakeSpan(synthetic_turns), birth_returns, 0.90F);

        // Turn 0 -> Turn 2 has delay 2 (0.90^2 = 0.81); Turn 1 -> Turn 2 has delay 1 (0.90^1 = 0.90)
        EXPECT_NEAR(synthetic_turns[0].econ_steps[0].lineage_return, 0.20F * 0.81F, 1e-5F);
        EXPECT_NEAR(synthetic_turns[0].econ_steps[1].lineage_return, 0.20F * 0.81F, 1e-5F);
        EXPECT_NEAR(synthetic_turns[1].econ_steps[0].lineage_return, 0.20F * 0.90F, 1e-5F);
    }
}

TEST(ModernAiTest, FairClassicFogMaskingAndWarbandPlayerConcentratedPlay) {
    // Fair-fog masking prevents raiding unexplored empty enemy cities under Hidden Map
    {
        auto game_ptr = SetupGame("ERYTHEA", SeedWith(45, {{"hiddenMap", 1}, {"diplomacy", 0}}));
        Game& game = *game_ptr;
        Side& side0 = game.GetMap().Sides()[0];
        Side& side1 = game.GetMap().Sides()[1];
        side0.SetComputerControlled(true);
        side1.SetComputerControlled(true);
        City* cap0 = side0.CapitalCity(game.GetMap());
        ASSERT_NE(cap0, nullptr);

        // Pick a nearby non-owned city, make it undefended Side 1, and ensure it is unrevealed
        City* target_city = nullptr;
        int best_dist = 9999;
        for (auto& city : game.GetMap().Cities()) {
            if (&city == cap0) continue;
            int dist = move::Distance(cap0->X(), cap0->Y(), city.X(), city.Y());
            if (dist >= 4 && dist < best_dist && !game::Seen(game, 0, city.X(), city.Y())) {
                best_dist = dist;
                target_city = &city;
            }
        }
        ASSERT_NE(target_city, nullptr);
        target_city->SetOwner(SideId(1));
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (Army* occ : game::ArmiesAt(game, target_city->X() + dx, target_city->Y() + dy)) {
                    game.Armies().Remove(occ);
                }
            }
        }

        // Place a fast flyer for Side 0 at cap0 (+ extra defender so it is not LastDefender)
        Army guard_proto;
        guard_proto.SetOwner(SideId(0));
        guard_proto.PlaceAt(cap0->Position());
        guard_proto.SetType(ArmyTypeId(0));
        guard_proto.SetStrength(3);
        guard_proto.SetMaxMoves(10);
        guard_proto.SetMoves(10);
        game.Armies().Add(guard_proto);

        int fly_type = 0;
        for (int tid = 0; tid < 28; ++tid) {
            const ArmyType* atype = game.ArmyTypes().ById(tid);
            if (atype && atype->flies) {
                fly_type = tid;
                break;
            }
        }
        Army flyer_proto;
        flyer_proto.SetOwner(SideId(0));
        flyer_proto.PlaceAt(cap0->Position());
        flyer_proto.SetType(ArmyTypeId(fly_type));
        flyer_proto.SetStrength(5);
        flyer_proto.SetMaxMoves(40);
        flyer_proto.SetMoves(40);
        game.Armies().Add(flyer_proto);

        // Fair-fog RaidEmptyCities must ignore the unexplored target_city
        EXPECT_EQ(opponents::RaidEmptyCities(game, side0, /*fair_fog=*/true), 0);
        EXPECT_EQ(target_city->OwnerIndex(), 1);

        // Omniscient RaidEmptyCities captures it immediately
        EXPECT_GE(opponents::RaidEmptyCities(game, side0, /*fair_fog=*/false), 1);

        opponents::InstallOpponent(game, side1, opponents::Kind::kFairClassic);
        const ClassicAiPlayer* fair_classic = ClassicAiPlayer::FindOn(side1);
        ASSERT_NE(fair_classic, nullptr);
        EXPECT_TRUE(fair_classic->IsFairFog());
        opponents::PlayTurn(game, side1, opponents::Kind::kFairClassic);
    }

    // WarbandPlayer buys an elite citadel slot, rallies/blesses its Hero stack, and garrisons cities
    {
        auto game_ptr = SetupGame("ERYTHEA", SeedWith(52, {{"hiddenMap", 1}, {"diplomacy", 0}}));
        Game& game = *game_ptr;
        Side& wb_side = game.GetMap().Sides()[0];
        wb_side.SetComputerControlled(true);
        wb_side.SetGold(1800);

        City* cap = wb_side.CapitalCity(game.GetMap());
        ASSERT_NE(cap, nullptr);
        int extra_cities = 0;
        for (auto& city : game.GetMap().Cities()) {
            if (city.IsNeutral() &&
                move::Distance(cap->X(), cap->Y(), city.X(), city.Y()) <= 18 && extra_cities < 2) {
                city.SetOwner(SideId(0));
                extra_cities++;
            }
        }
        game::RevealStart(game, wb_side);

        // Place a Hero at cap and 4 spare units 1-2 tiles away so Warband rallies & garrisons
        Army hero_proto;
        hero_proto.SetOwner(SideId(0));
        hero_proto.PlaceAt(cap->Position());
        hero_proto.SetType(ArmyTypeId(armytype::kHero));
        hero_proto.SetStrength(6);
        hero_proto.SetMaxMoves(16);
        hero_proto.SetMoves(16);
        hero_proto.SetLevel(1);
        game.Armies().Add(hero_proto);

        for (int idx = 0; idx < 5; ++idx) {
            Army unit_proto;
            unit_proto.SetOwner(SideId(0));
            unit_proto.PlaceAt(TilePos(cap->X() + (idx % 2), cap->Y() + ((idx / 2) % 2)));
            unit_proto.SetType(ArmyTypeId(2));
            unit_proto.SetStrength(5);
            unit_proto.SetMaxMoves(36);
            unit_proto.SetMoves(36);
            game.Armies().Add(unit_proto);
        }

        opponents::PlayTurn(game, wb_side, opponents::Kind::kWarband);
        EXPECT_NE(dynamic_cast<const WarbandPlayer*>(wb_side.ComputerPlayer()), nullptr);
        ASSERT_NE(ClassicAiPlayer::FindOn(wb_side), nullptr);
        EXPECT_TRUE(ClassicAiPlayer::FindOn(wb_side)->IsFairFog());
        EXPECT_LT(wb_side.Gold(), 1800);

        int undefended_owned = 0;
        for (const auto& city : game.GetMap().Cities()) {
            if (city.IsRazed() || !city.OwnedBy(0)) continue;
            int def_cnt = 0;
            for (int dy = 0; dy <= 1; ++dy) {
                for (int dx = 0; dx <= 1; ++dx) {
                    for (Army* occ : game::ArmiesAt(game, city.X() + dx, city.Y() + dy)) {
                        if (occ->OwnedBy(0)) def_cnt++;
                    }
                }
            }
            if (def_cnt == 0) undefended_owned++;
        }
        EXPECT_EQ(undefended_owned, 0);
    }
}

TEST(ModernAiTest, CharacterRelationalMemoryVendettasHonourAndCoalitions) {
    namespace pers = ai::personality;

    // Relational memory hooks & SaveState/LoadState persistence
    {
        auto game_ptr = SetupGame("ERYTHEA", SeedWith(701, {{"hiddenMap", 0}, {"diplomacy", 1}}));
        Game& game = *game_ptr;
        Side& victim = game.GetMap().Sides()[0];
        Side& razer = game.GetMap().Sides()[1];
        Side& payer = game.GetMap().Sides()[2];
        victim.SetComputerControlled(true);
        razer.SetComputerControlled(true);
        payer.SetComputerControlled(true);
        ai::SetSideClassicAi(game, victim, false);
        ai::SetSideClassicAi(game, razer, false);
        ai::SetSideClassicAi(game, payer, false);

        ModernMlPlayer& victim_ai = ModernMlPlayer::EnsureOn(victim);
        EXPECT_FLOAT_EQ(ai::ComputeVendettaScore(victim_ai.Grudge(), 1, game.Turn()), 0.0F);

        // Undefended city stolen + razed by Side 1 records both cities_lost and cities_razed_by
        City* victim_city = nullptr;
        for (auto& city : game.GetMap().Cities()) {
            if (city.IsNeutral() && !city.IsRazed()) {
                city.SetOwner(SideId(0));
                victim_city = &city;
                break;
            }
        }
        ASSERT_NE(victim_city, nullptr);
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (Army* occ : game::ArmiesAt(game, victim_city->X() + dx, victim_city->Y() + dy)) {
                    game.Remove(occ);
                }
            }
        }
        Army* raider = test::AddArmy(game, victim_city->X() - 1, victim_city->Y(), 1, 2, 6, 12, 12);
        diplomacy::Propose(game, 1, 0, diplomacy::kWar);
        diplomacy::AttackAnyway(game, 1, 0);
        Battle cap_res = game::ResolveAttack(game, {raider}, victim_city->X(), victim_city->Y());
        EXPECT_TRUE(cap_res.won);
        EXPECT_GE(victim_ai.Grudge().cities_lost[1], 1);
        EXPECT_GE(victim_ai.Grudge().truces_broken_by[1], 1);

        game::Raze(game, razer, *victim_city, cap_res.attackers);
        EXPECT_GE(victim_ai.Grudge().cities_razed_by[1], 1);
        EXPECT_GT(ai::ComputeVendettaScore(victim_ai.Grudge(), 1, game.Turn()), 0.65F);

        // Tribute paid by Side 2 to Side 0 is recorded in tribute_received
        game.Diplomacy().SetSymmetricState(SideId(0), SideId(2), diplomacy::kWar);
        payer.SetGold(400);
        diplomacy::Propose(game, 2, 0, diplomacy::kPeace);
        diplomacy::SetGoldOffer(game, 2, 0, 150);
        diplomacy::Propose(game, 0, 2, diplomacy::kPeace);
        (void)diplomacy::Apply(game, victim);
        EXPECT_EQ(victim_ai.Grudge().tribute_received[2], 150);

        // SaveState / LoadState round-trips relational memory cleanly
        nlohmann::ordered_json saved = victim_ai.SaveState();
        ModernMlPlayer loaded_ai;
        ASSERT_TRUE(loaded_ai.LoadState(saved, game).ok());
        EXPECT_EQ(loaded_ai.Grudge().cities_lost[1], victim_ai.Grudge().cities_lost[1]);
        EXPECT_EQ(loaded_ai.Grudge().cities_razed_by[1], victim_ai.Grudge().cities_razed_by[1]);
        EXPECT_EQ(loaded_ai.Grudge().truces_broken_by[1], victim_ai.Grudge().truces_broken_by[1]);
        EXPECT_EQ(loaded_ai.Grudge().tribute_received[2], 150);
    }

    // Raymond Ratsbane vendetta target selection & Hand the Hammer capital strike prior
    {
        const pers::Profile& raymond = pers::ProfileFor(1, 3);  // Raymond Ratsbane
        const pers::Profile& hand = pers::ProfileFor(2, 6);     // Hand the Hammer
        const pers::Profile& robin = pers::ProfileFor(0, 2);    // Brave Brave Sir Robin

        pers::MissionDoctrineContext neutral_enemy{};
        neutral_enemy.is_enemy_target = true;
        neutral_enemy.win_converged = 0.72F;

        pers::MissionDoctrineContext feud_enemy = neutral_enemy;
        feud_enemy.vendetta_score = 0.90F;

        EXPECT_GT(pers::ScoreMissionCharacterPrior(raymond, feud_enemy),
                  pers::ScoreMissionCharacterPrior(raymond, neutral_enemy) + 0.20F);

        pers::MissionDoctrineContext cap_target = neutral_enemy;
        cap_target.is_enemy_capital = true;
        EXPECT_GT(pers::ScoreMissionCharacterPrior(hand, cap_target),
                  pers::ScoreMissionCharacterPrior(hand, neutral_enemy) + 0.08F);

        pers::MissionDoctrineContext risky_fight{};
        risky_fight.is_enemy_target = true;
        risky_fight.win_converged = 0.35F;
        EXPECT_LT(pers::ScoreMissionCharacterPrior(robin, risky_fight), -0.15F);
    }

    // Winston truce honour vs Nikolas opportunistic backstab vs William anti-leader coalition
    {
        const pers::Profile& winston = pers::ProfileFor(0, 1);  // Winston (+0.8 honour)
        const pers::Profile& nikolas = pers::ProfileFor(2, 4);  // Nikolas (-0.9 honour)
        const pers::Profile& william = pers::ProfileFor(2, 8);  // William (+0.95 coalition)

        pers::DiplomacyDoctrineContext truce_ctx{};
        truce_ctx.cur_at_peace = true;
        truce_ctx.truce_turns = 8;
        truce_ctx.border_advantage = 0.65F;

        std::array<float, 4> win_stances{0.0F, 0.0F, 0.0F, 0.0F};
        float win_trib = 0.0F;
        float win_jwar = 0.0F;
        pers::ApplyDiplomacyCharacterPrior(winston, truce_ctx, win_stances, win_trib, win_jwar);
        EXPECT_GT(win_stances[1], win_stances[3] + 0.80F);

        pers::DiplomacyDoctrineContext backstab_ctx{};
        backstab_ctx.cur_at_peace = true;
        backstab_ctx.truce_turns = 0;
        backstab_ctx.border_advantage = 0.75F;
        backstab_ctx.active_wars = 0;
        std::array<float, 4> nik_stances{0.0F, 0.0F, 0.0F, 0.0F};
        float nik_trib = 0.0F;
        float nik_jwar = 0.0F;
        pers::ApplyDiplomacyCharacterPrior(nikolas, backstab_ctx, nik_stances, nik_trib, nik_jwar);
        EXPECT_GT(nik_stances[3], nik_stances[1] + 0.35F);

        pers::DiplomacyDoctrineContext coal_partner_ctx{};
        coal_partner_ctx.cur_at_peace = true;
        coal_partner_ctx.third_party_runaway = true;
        coal_partner_ctx.runaway_share = 0.40F;
        std::array<float, 4> wil_stances{0.0F, 0.0F, 0.0F, 0.0F};
        float wil_trib = 0.0F;
        float wil_jwar = 0.0F;
        pers::ApplyDiplomacyCharacterPrior(william, coal_partner_ctx, wil_stances, wil_trib,
                                           wil_jwar);
        EXPECT_GT(wil_stances[0], 0.30F);
        EXPECT_GT(wil_jwar, 0.40F);
    }
}

TEST(ModernAiTest, CharacterSignatureUnitRostersSpoilsAndHeroQuestPriors) {
    namespace pers = ai::personality;

    const pers::Profile& attila = pers::ProfileFor(2, 1);   // Attila (+0.95 cavalry)
    const pers::Profile& ian = pers::ProfileFor(2, 3);      // Ian (+0.95 siege/heavy)
    const pers::Profile& roger = pers::ProfileFor(2, 7);    // Roger (+0.95 hero, +0.80 flier)
    const pers::Profile& charles = pers::ProfileFor(0, 7);  // Charles (+0.80 avarice, +0.70 caut)
    const pers::Profile& roland = pers::ProfileFor(1, 2);   // Roland (+0.95 scorched earth)
    const pers::Profile& rebecca = pers::ProfileFor(1, 4);  // Rebecca (+0.95 avarice)
    const pers::Profile& winston = pers::ProfileFor(0, 1);  // Winston (-0.90 scorched earth)

    // Signature unit roster affinities: Cavalry vs Heavy/Siege vs Flier
    pers::UnitDoctrineContext cav_unit{};
    cav_unit.is_cavalry = true;
    cav_unit.strength = 5;
    cav_unit.build_time = 2;

    pers::UnitDoctrineContext siege_unit{};
    siege_unit.is_heavy_or_siege = true;
    siege_unit.strength = 8;
    siege_unit.build_time = 4;

    pers::UnitDoctrineContext flier_unit{};
    flier_unit.is_flier = true;
    flier_unit.strength = 6;
    flier_unit.build_time = 3;

    EXPECT_GT(pers::ScoreUnitCharacterPrior(attila, cav_unit),
              pers::ScoreUnitCharacterPrior(attila, siege_unit) + 0.08F);
    EXPECT_GT(pers::ScoreUnitCharacterPrior(ian, siege_unit),
              pers::ScoreUnitCharacterPrior(ian, cav_unit) + 0.08F);
    EXPECT_GT(pers::ScoreUnitCharacterPrior(roger, flier_unit),
              pers::ScoreUnitCharacterPrior(roger, cav_unit) + 0.04F);

    // Treasury reserve hoarding for Charles the Craven when below war-chest reserve
    EXPECT_GT(pers::ScoreHoldGoldCharacterPrior(charles, 320), 0.20F);
    EXPECT_LE(pers::ScoreHoldGoldCharacterPrior(charles, 1400), 0.05F);

    // Capture spoils doctrines: Roland razes vulnerable border town, Rebecca pillages/sacks,
    // Winston strictly occupies
    const std::vector<int> spoil_modes = {0, 1, 2, 3};
    std::vector<float> roland_scores = {0.0F, 0.0F, 0.0F, 0.0F};
    pers::ApplySpoilsCharacterPrior(roland, 12.0F, 12, 150, 300, spoil_modes, roland_scores);
    EXPECT_GT(roland_scores[3], roland_scores[0] + 0.25F);

    std::vector<float> rebecca_scores = {0.0F, 0.0F, 0.0F, 0.0F};
    pers::ApplySpoilsCharacterPrior(rebecca, 8.0F, 18, 250, 500, spoil_modes, rebecca_scores);
    EXPECT_GT(std::max(rebecca_scores[1], rebecca_scores[2]), rebecca_scores[0] + 0.20F);

    std::vector<float> winston_scores = {0.0F, 0.0F, 0.0F, 0.0F};
    pers::ApplySpoilsCharacterPrior(winston, 12.0F, 12, 150, 300, spoil_modes, winston_scores);
    EXPECT_GT(winston_scores[0], winston_scores[3] + 1.00F);

    // Hero / Ruin / Temple questing prior for Roger the Unrepentant
    pers::MissionDoctrineContext ruin_ctx{};
    ruin_ctx.is_ruin_or_temple = true;
    ruin_ctx.stack_has_hero = true;
    EXPECT_GT(pers::ScoreMissionCharacterPrior(roger, ruin_ctx), 0.10F);
}

TEST(ModernAiTest, CharacterHeraldVoiceDispatchesMissivesAndLogsEvents) {
    namespace pers = ai::personality;

    const pers::Profile& raymond = pers::ProfileFor(1, 3);  // Raymond Ratsbane
    const pers::Profile& nikolas = pers::ProfileFor(2, 4);  // Nikolas the Nefarious
    const pers::Profile& william = pers::ProfileFor(2, 8);  // William the Wily
    const pers::Profile& roland = pers::ProfileFor(1, 2);   // Roland the Rabid

    pers::CharacterDispatchContext feud_ctx{};
    feud_ctx.actor_faction = "Lord Bane";
    feud_ctx.target_faction = "Sirians";
    feud_ctx.cities_lost = 2;
    feud_ctx.heroes_slain = 1;
    std::string feud_msg =
            pers::FormatCharacterDispatch(raymond, pers::CharacterEvent::kVendettaWar, feud_ctx);
    EXPECT_NE(feud_msg.find("Sirians"), std::string::npos);
    EXPECT_NE(feud_msg.find("hunt your throne"), std::string::npos);

    pers::CharacterDispatchContext stab_ctx{};
    stab_ctx.actor_faction = "Darkness";
    stab_ctx.target_faction = "Elvallie";
    std::string stab_msg =
            pers::FormatCharacterDispatch(nikolas, pers::CharacterEvent::kBetrayalWar, stab_ctx);
    EXPECT_NE(stab_msg.find("Elvallie"), std::string::npos);
    EXPECT_NE(stab_msg.find("masks"), std::string::npos);

    pers::CharacterDispatchContext coal_ctx{};
    coal_ctx.actor_faction = "Grey";
    coal_ctx.target_faction = "Sirians";
    coal_ctx.third_faction = "Storm Giants";
    std::string coal_msg = pers::FormatCharacterDispatch(
            william, pers::CharacterEvent::kCoalitionAlliance, coal_ctx);
    EXPECT_NE(coal_msg.find("Storm Giants"), std::string::npos);

    pers::CharacterDispatchContext raze_ctx{};
    raze_ctx.actor_faction = "Orcs of Kor";
    raze_ctx.city_name = "Marthos";
    std::string raze_msg =
            pers::FormatCharacterDispatch(roland, pers::CharacterEvent::kCityRazed, raze_ctx);
    EXPECT_NE(raze_msg.find("Marthos"), std::string::npos);
    EXPECT_NE(raze_msg.find("Ashes"), std::string::npos);

    // Live human-player game: Raymond Ratsbane sends a Vendetta War missive & herald log entry
    auto game_ptr = SetupGame("ERYTHEA", SeedWith(777, {{"hiddenMap", 0}, {"diplomacy", 1}}));
    Game& game = *game_ptr;
    game.SetTurn(6);
    Side& ai_side = game.GetMap().Sides()[0];
    Side& human_side = game.GetMap().Sides()[1];
    ai_side.SetComputerControlled(true);
    ai_side.SetLevel(1);
    ai_side.SetCard(3);  // Raymond Ratsbane
    ai::SetSideClassicAi(game, ai_side, false);
    human_side.SetComputerControlled(false);

    ModernMlPlayer& ai_player = ModernMlPlayer::EnsureOn(ai_side);
    ai_player.MutableGrudge().heroes_killed[1] = 1;
    ai_player.MutableGrudge().cities_razed_by[1] = 1;
    ai_player.MutableGrudge().last_grudge_turn[1] = 5;

    ai::ml::PolicyNetwork net;
    net.Zero();
    net.SetUsePersonalities(true);
    human_side.ClearMissives();
    game.ClearLog();
    ai::modern::PlayTurn(game, ai_side, nullptr, &net, nullptr);

    ASSERT_FALSE(human_side.Missives().empty());
    const DiplomaticMissive& missive = human_side.Missives().front();
    EXPECT_EQ(missive.proposed_state, diplomacy::kWar);
    EXPECT_NE(missive.headline.find("Raymond Ratsbane"), std::string::npos);
    EXPECT_FALSE(game.Log().empty());
}

}  // namespace
}  // namespace w2
