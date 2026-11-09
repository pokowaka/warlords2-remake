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

}  // namespace
}  // namespace w2
