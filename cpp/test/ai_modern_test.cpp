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
        Side* side0 = game->map->Side(0);
        side0->SetComputerControlled(true);
        ai::SetSideClassicAi(*game, *side0, false);
        int unseen_x = kNone;
        int unseen_y = kNone;
        for (int y = 5; y < game->map->height - 5 && unseen_x == kNone; ++y) {
            for (int x = 5; x < game->map->width - 5 && unseen_x == kNone; ++x) {
                if (!game::Seen(*game, 0, x, y) &&
                    scn::TerrainAt(*game->map, x, y) == movement::kPlain) {
                    unseen_x = x;
                    unseen_y = y;
                }
            }
        }
        ASSERT_NE(unseen_x, kNone);
        auto grid = movement::Grid(*game, 0);
        auto prep = movement::Prepare(*game, grid, 0, movement::kLand, 0, 0);
        EXPECT_EQ(prep[unseen_y * game->map->width + unseen_x], movement::kShut);
    }

    {
        auto game = SetupGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}}));
        EXPECT_FALSE(game::Seen(*game, 0, 50, 50));
        game::Reveal(*game, 1, 50, 50, false);
        EXPECT_TRUE(game::Seen(*game, 1, 50, 50));
        EXPECT_FALSE(game::Seen(*game, 0, 50, 50));

        int gold0_before = game->map->sides[0].Gold();
        int gold1_before = game->map->sides[1].Gold();
        diplomacy::Propose(*game, 0, 1, diplomacy::kPeace);
        diplomacy::ProposeAlliance(*game, 0, 1, true);
        diplomacy::SetGoldOffer(*game, 0, 1, 50);
        diplomacy::SetWarTarget(*game, 0, 1, 2);
        diplomacy::Propose(*game, 1, 0, diplomacy::kPeace);
        diplomacy::ProposeAlliance(*game, 1, 0, true);
        diplomacy::Apply(*game, game->map->sides[1]);

        EXPECT_TRUE(diplomacy::IsAllied(*game, 0, 1));
        EXPECT_EQ(diplomacy::TruceTurns(*game, 0, 1), 15);
        EXPECT_EQ(game->map->sides[0].Gold(), gold0_before - 50);
        EXPECT_EQ(game->map->sides[1].Gold(), gold1_before + 50);
        EXPECT_EQ(diplomacy::State(*game, 1, 2), diplomacy::kWar);
        EXPECT_TRUE(game::Seen(*game, 0, 50, 50));

        auto breakdown = diplomacy::ComputeAttitude(*game, 1, 0);
        EXPECT_TRUE(!breakdown.factors.empty() && !breakdown.what_it_takes.empty());
    }
}

TEST(ModernAiTest, SideBeliefPolicyRoundTripAndMultiTurnPlay) {
    {
        auto game = SetupGame("ERYTHEA", SeedWith(1, {{"hiddenMap", 1}, {"viewEnemies", 0}}));
        Side& side0 = game->map->sides[0];
        side0.SetComputerControlled(true);
        ai::SetSideClassicAi(*game, side0, false);
        ai::belief::Update(*game, side0);
        const SideBelief& belief = ai::belief::Ensure(side0);
        for (const City* city : belief.visible_cities) {
            EXPECT_TRUE(ai::belief::IsCityVisible(*game, 0, *city));
        }
        uint32_t rng_before = game->rng.State();
        auto my_armies = game::SideArmies(*game, side0);
        if (!my_armies.empty() && !belief.visible_cities.empty()) {
            auto est = ai::belief::EstimateCombat(*game, side0, {my_armies[0]},
                                                  belief.visible_cities[0]->X(),
                                                  belief.visible_cities[0]->Y());
            EXPECT_TRUE(est.win_prob >= 0.0F && est.win_prob <= 1.0F);
        }
        EXPECT_EQ(game->rng.State(), rng_before);
    }

    {
        ai::ml::PolicyNetwork net;
        auto game = SetupGame("TUTORIA", Seed(1));
        Side& side0 = game->map->sides[0];
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
        for (auto& side : game->map->sides) {
            if (side.InUse()) ai::SetSideClassicAi(*game, side, false);
        }
        Side* cur = game::Begin(*game);
        int turns_played = 0;
        int worst_stack = 0;
        while (cur != nullptr && !game->over && game->turn <= 8) {
            ai::PlayTurn(*game, *cur);
            ++turns_played;
            std::map<int, int> per_tile;
            for (const Army* army : game->armies) {
                if (army->X() != kNone && !army->InTransit()) {
                    worst_stack = std::max(worst_stack,
                                           ++per_tile[army->Y() * game->map->width + army->X()]);
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
    for (float weight : fresh_net.mission_net.w_skip)
        max_skip = std::max(max_skip, std::abs(weight));
    EXPECT_LE(max_skip, 0.25F);

    auto game_ptr = SetupGame(
            "ERYTHEA", SeedWith(7, {{"hiddenMap", 1}, {"viewEnemies", 0}, {"neutralCities", 2}}));
    Game& game = *game_ptr;
    Side& side7 = game.map->sides[7];
    side7.SetComputerControlled(true);
    ai::SetSideClassicAi(game, side7, false);

    City* target_neutral = nullptr;
    for (auto& city : game.map->cities) {
        if (city.IsNeutral() && !city.IsRazed() &&
            !game::Seen(game, 7, city.X() + 1, city.Y() + 1)) {
            target_neutral = &city;
            break;
        }
    }
    ASSERT_NE(target_neutral, nullptr);
    for (Army* army : game::ArmiesAt(game, target_neutral->X(), target_neutral->Y())) {
        game.armies.Relocate(army, TilePos());
    }
    game.explored.MarkExplored(SideId(7), game.map->width, game.map->height, target_neutral->X(),
                               target_neutral->Y());
    EXPECT_TRUE(game::Seen(game, 7, target_neutral->X(), target_neutral->Y()));
    EXPECT_FALSE(game::Seen(game, 7, target_neutral->X() + 1, target_neutral->Y() + 1));
    ai::belief::Update(game, side7);
    auto my_armies = game::SideArmies(game, side7);
    ASSERT_FALSE(my_armies.empty());
    auto est = ai::belief::EstimateCombat(game, side7, {my_armies[0]}, target_neutral->X(),
                                          target_neutral->Y());
    EXPECT_EQ(est.estimated_defender_count, 0);

    const SideBelief& belief7 = ai::belief::Ensure(side7);
    City* cap7 = side7.CapitalCity(*game.map);
    if (cap7 != nullptr &&
        cap7->Index() < static_cast<int>(belief7.city_nearest_enemy_dist.size())) {
        EXPECT_GT(belief7.city_nearest_enemy_dist[cap7->Index()], 50.0F);
    }

    game.explored.ResetSide(SideId(7), game.map->width, game.map->height, false);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 27; ++x) {
            int terr = scn::TerrainAt(*game.map, x, y);
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
    fresh_net.mission_net.Backward(test_in, grad_out, grad_analytical);
    std::vector<float> flat_grad;
    grad_analytical.ToFlat(flat_grad);
    std::vector<float> base_params;
    fresh_net.mission_net.ToFlat(base_params);

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
    for (auto& city : game.map->cities) {
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
        EXPECT_EQ(game::ViewerSide(*human_game, human_game->sides[0]),
                  human_game->sides[0]->Index());
        EXPECT_FALSE(
                game::Seen(*human_game, game::ViewerSide(*human_game, human_game->sides[0]), 0, 0));

        GameOpts ai_opts = SeedWith(11, {{"hiddenMap", 1}});
        for (int i = 0; i < 8; ++i) ai_opts.sides[i].computer = true;
        auto ai_game = SetupGame("ERYTHEA", ai_opts);
        EXPECT_FALSE(game::HasActiveHuman(*ai_game));
        EXPECT_EQ(game::ViewerSide(*ai_game, ai_game->sides[0]), kNone);
        EXPECT_TRUE(game::Seen(*ai_game, game::ViewerSide(*ai_game, ai_game->sides[0]), 0, 0));
        EXPECT_EQ(game::FogCell(*ai_game, game::ViewerSide(*ai_game, ai_game->sides[0]), 0, 0),
                  kNone);
        EXPECT_FALSE(game::Seen(*ai_game, ai_game->sides[0]->Index(), 0, 0));
    }

    {
        ScopedDefaultAi default_ai_guard(true);
        auto classic_game = SetupGame("ERYTHEA", Seed(13));
        EXPECT_TRUE(classic_game->sides[0]->IsClassicAi());
        std::string classic_json = save::Encode(*classic_game);

        ai::SetDefaultClassicAi(false);
        auto modern_game = SetupGame("ERYTHEA", Seed(13));
        EXPECT_FALSE(modern_game->sides[0]->IsClassicAi());
        std::string modern_json = save::Encode(*modern_game);

        auto decoded_classic = *save::Decode(classic_json, kData);
        EXPECT_TRUE(decoded_classic->sides[0]->IsClassicAi());

        ai::SetDefaultClassicAi(true);
        auto decoded_modern = *save::Decode(modern_json, kData);
        EXPECT_FALSE(decoded_modern->sides[0]->IsClassicAi());

        for (Side* side : decoded_modern->sides) side->SetComputerControlled(true);
        Side* cur = game::Begin(*decoded_modern);
        ASSERT_NE(cur, nullptr);
        int switched_turns = 0;
        int worst_tile = 0;
        bool positions_valid = true;
        while (cur != nullptr && !decoded_modern->over && decoded_modern->turn <= 6) {
            bool use_classic = decoded_modern->turn % 2 == 0;
            for (auto& side : decoded_modern->map->sides) {
                ai::SetSideClassicAi(*decoded_modern, side, use_classic);
            }
            ai::PlayTurn(*decoded_modern, *cur);
            ++switched_turns;
            std::map<int, int> per_tile;
            for (const Army* army : decoded_modern->armies) {
                if (army->X() == kNone || army->InTransit()) continue;
                if (!decoded_modern->map->InBounds(army->X(), army->Y())) {
                    positions_valid = false;
                } else {
                    worst_tile = std::max(
                            worst_tile,
                            ++per_tile[army->Y() * decoded_modern->map->width + army->X()]);
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
        EXPECT_EQ(small_game->sides.size(), 2U);
        EXPECT_TRUE(small_game->map->cities.size() >= 2 && small_game->map->cities.size() < 45);
        for (const auto& city : small_game->map->cities) {
            EXPECT_GE(city.X(), small_game->map->active_x0);
            EXPECT_LE(city.X() + 1, small_game->map->active_x1);
            EXPECT_GE(city.Y(), small_game->map->active_y0);
            EXPECT_LE(city.Y() + 1, small_game->map->active_y1);
        }
        EXPECT_EQ(scn::TerrainAt(*small_game->map, 0, 0), movement::kWater);
        EXPECT_TRUE(game::Seen(*small_game, small_game->sides[0]->Index(), 0, 0));

        ai::SetSideClassicAi(*small_game, *small_game->sides[0], false);
        ai::SetSideClassicAi(*small_game, *small_game->sides[1], true);
        Side* cur = game::Begin(*small_game);
        while (cur != nullptr && !small_game->over && !small_game->won && small_game->turn <= 10) {
            if (cur->IsAlive()) ai::PlayTurn(*small_game, *cur);
            cur = game::EndTurn(*small_game);
        }
        EXPECT_TRUE(small_game->turn > 5 || small_game->won || small_game->over);
    }
}

TEST(ModernAiTest, PolicyNetworkWeightsLegacyMigrationAndTemperaturePrior) {
    namespace ml = ai::ml;
    using PolicyNet = ml::PolicyNetwork;
    EXPECT_EQ(PolicyNet::kTotalParams, 9700);
    EXPECT_EQ(PolicyNet::kLegacyV5Params, 8866);
    EXPECT_EQ(PolicyNet::kLegacyV4Params, 8066);
    EXPECT_EQ(PolicyNet::kLegacyV3Params, 6729);

    PolicyNet net;
    EXPECT_TRUE(net.LoadJson("cpp/ai_weights.json").ok());
    EXPECT_EQ(net.Flatten().size(), 9700U);

    PolicyNet src;
    src.InitRandomWeights(7);
    auto added_ctx = [](int col) { return col >= ml::kLegacyGlobalDim && col < ml::kGlobalDim; };
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
    auto critic_dropped = [&](int in_dim) {
        return [&, in_dim](int col) { return added_ctx(col) || col >= in_dim - ml::kCriticDim; };
    };

    std::vector<float> vec_v5;
    std::vector<float> vec_v4;
    for (auto* out : {&vec_v5, &vec_v4}) {
        narrow(src.diplo_net, added_ctx, *out);
        narrow(src.mission_net, added_ctx, *out);
        narrow(src.econ_net, added_ctx, *out);
    }
    narrow(src.value_net, added_ctx, vec_v5);
    narrow(src.stack_value_net, added_ctx, vec_v5);
    narrow(src.value_net, critic_dropped(ml::kValueInDim), vec_v4);
    size_t v3_size = vec_v4.size();
    narrow(src.stack_value_net, critic_dropped(ml::kStackValueInDim), vec_v4);
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
}

TEST(ModernAiTest, RaiderAndFrozenComputerPlayerStrategiesExecutePolymorphically) {
    ai::ml::PolicyNetwork frozen_net;
    EXPECT_TRUE(frozen_net.LoadJson("cpp/ai_weights.json").ok());

    GameOpts opts = Seed(21);
    for (int i = 0; i < 8; ++i) opts.sides[i].computer = true;
    auto game = SetupGame("TUTORIA", opts);
    opponents::InstallOpponent(*game, *game->sides[0], opponents::Kind::kRaider);
    opponents::InstallOpponent(*game, *game->sides[1], opponents::Kind::kFrozen, &frozen_net);
    EXPECT_TRUE(game->sides[0]->IsClassicAi());
    EXPECT_FALSE(game->sides[1]->IsClassicAi());

    Side* cur = game::Begin(*game);
    while (cur != nullptr && !game->over && game->turn <= 3) {
        cur->ComputerPlayer()->PlayTurn(*game, *cur);
        cur = game::EndTurn(*game);
    }
    EXPECT_FALSE(game->sides[1]->ComputerPlayer()->Thoughts().empty());
}

}  // namespace
}  // namespace w2
