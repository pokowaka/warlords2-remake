#include <array>
#include <random>
#include <vector>

#include "benchmark/benchmark.h"
#include "test/test_helpers.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"

namespace w2::ai::ml {
namespace {

std::mt19937 MakeRng() {
    return std::mt19937(1337);
}

template <size_t N>
std::array<float, N> RandomVec(std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::array<float, N> v{};
    for (size_t i = 0; i < N; ++i) {
        v[i] = dist(rng);
    }
    return v;
}

// ============================================================================
// 1. AI Evaluator Performance During Play
// ============================================================================

static void BM_Evaluator_MissionNet_Forward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    auto input = RandomVec<kMissionInDim>(rng);
    for (auto _ : state) {
        auto out = net.Forward(input);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Evaluator_MissionNet_Forward);

static void BM_Evaluator_EconNet_Forward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::EconNet net;
    auto input = RandomVec<kEconInDim>(rng);
    for (auto _ : state) {
        auto out = net.Forward(input);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Evaluator_EconNet_Forward);

static void BM_Evaluator_DiploNet_Forward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::DiploNet net;
    auto input = RandomVec<kDiploInDim>(rng);
    for (auto _ : state) {
        auto out = net.Forward(input);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Evaluator_DiploNet_Forward);

static void BM_Evaluator_ValueNet_Forward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::ValueNet net;
    auto input = RandomVec<kValueInDim>(rng);
    for (auto _ : state) {
        auto out = net.Forward(input);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Evaluator_ValueNet_Forward);

static void BM_Evaluator_StackValueNet_Forward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::StackValueNet net;
    auto input = RandomVec<kStackValueInDim>(rng);
    for (auto _ : state) {
        auto out = net.Forward(input);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Evaluator_StackValueNet_Forward);

static void BM_Evaluator_Batch64_MissionDecisions(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    std::vector<MissionInVec> inputs(64);
    for (auto& in : inputs) {
        in = RandomVec<kMissionInDim>(rng);
    }
    for (auto _ : state) {
        for (const auto& in : inputs) {
            auto out = net.Forward(in);
            benchmark::DoNotOptimize(out);
        }
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_Evaluator_Batch64_MissionDecisions);

static void BM_Evaluator_SelectFromLogits_64Candidates(benchmark::State& state) {
    auto rng = MakeRng();
    std::vector<float> logits(64);
    std::vector<float> log_mult(64);
    std::uniform_real_distribution<float> dist(-3.0F, 3.0F);
    for (size_t i = 0; i < 64; ++i) {
        logits[i] = dist(rng);
        log_mult[i] = std::log(static_cast<float>(i + 1));
    }
    for (auto _ : state) {
        int best = SelectFromLogits(logits, /*sampler=*/nullptr, nullptr, &log_mult);
        benchmark::DoNotOptimize(best);
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_Evaluator_SelectFromLogits_64Candidates);

// ============================================================================
// 2. Learning / Training Performance
// ============================================================================

static void BM_Learning_MissionNet_Backward_Baseline(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    PolicyNetwork::MissionNet grad;
    grad.Zero();
    auto input = RandomVec<kMissionInDim>(rng);
    std::array<float, kMissionOutDim> d_out = {0.5F, -0.2F};
    for (auto _ : state) {
        net.Backward(input, d_out, grad);
        benchmark::DoNotOptimize(grad);
    }
}
BENCHMARK(BM_Learning_MissionNet_Backward_Baseline);

static void BM_Learning_MissionNet_Backward_CachedActivations(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    PolicyNetwork::MissionNet grad;
    grad.Zero();
    auto input = RandomVec<kMissionInDim>(rng);
    PolicyNetwork::MissionNet::Activations act;
    (void)net.ForwardWithActivations(input, act);
    std::array<float, kMissionOutDim> d_out = {0.5F, -0.2F};
    for (auto _ : state) {
        net.BackwardWithActivations(input, act, d_out, grad);
        benchmark::DoNotOptimize(grad);
    }
}
BENCHMARK(BM_Learning_MissionNet_Backward_CachedActivations);

static void BM_Learning_EconNet_Backward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::EconNet net;
    PolicyNetwork::EconNet grad;
    grad.Zero();
    auto input = RandomVec<kEconInDim>(rng);
    std::array<float, kEconOutDim> d_out = {0.5F};
    for (auto _ : state) {
        net.Backward(input, d_out, grad);
        benchmark::DoNotOptimize(grad);
    }
}
BENCHMARK(BM_Learning_EconNet_Backward);

static void BM_Learning_ValueNet_Backward(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::ValueNet net;
    PolicyNetwork::ValueNet grad;
    grad.Zero();
    auto input = RandomVec<kValueInDim>(rng);
    std::array<float, kValueOutDim> d_out = {0.75F};
    for (auto _ : state) {
        net.Backward(input, d_out, grad);
        benchmark::DoNotOptimize(grad);
    }
}
BENCHMARK(BM_Learning_ValueNet_Backward);

static void BM_Learning_TurnStep_64Candidates_Baseline(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    PolicyNetwork::MissionNet grad;
    grad.Zero();
    std::vector<MissionInVec> candidates(64);
    for (auto& c : candidates) {
        c = RandomVec<kMissionInDim>(rng);
    }
    std::vector<float> d_scores(64, 0.05F);

    for (auto _ : state) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto out = net.Forward(candidates[i]);
            benchmark::DoNotOptimize(out);
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            net.Backward(candidates[i], {d_scores[i], 0.0F}, grad);
            benchmark::DoNotOptimize(grad);
        }
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_Learning_TurnStep_64Candidates_Baseline);

static void BM_Learning_TurnStep_64Candidates_CachedActivations(benchmark::State& state) {
    auto rng = MakeRng();
    PolicyNetwork::MissionNet net;
    PolicyNetwork::MissionNet grad;
    grad.Zero();
    std::vector<MissionInVec> candidates(64);
    for (auto& c : candidates) {
        c = RandomVec<kMissionInDim>(rng);
    }
    std::vector<float> d_scores(64, 0.05F);
    std::vector<PolicyNetwork::MissionNet::Activations> acts(64);

    for (auto _ : state) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto out = net.ForwardWithActivations(candidates[i], acts[i]);
            benchmark::DoNotOptimize(out);
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            net.BackwardWithActivations(candidates[i], acts[i], {d_scores[i], 0.0F}, grad);
            benchmark::DoNotOptimize(grad);
        }
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_Learning_TurnStep_64Candidates_CachedActivations);

// ============================================================================
// 3. Game Engine & Tactical Combat Optimization
// ============================================================================

static void BM_Engine_FrontierSort_UncachedDistance(benchmark::State& state) {
    std::mt19937 rng(42);
    std::vector<std::pair<int, int>> anchors;
    for (int i = 0; i < 30; ++i) {
        anchors.emplace_back(rng() % 112, rng() % 156);
    }
    std::vector<FrontierCluster> initial_frontiers;
    for (int i = 0; i < 300; ++i) {
        initial_frontiers.push_back(FrontierCluster{
                static_cast<int>(rng() % 112), static_cast<int>(rng() % 156),
                static_cast<int>(1 + rng() % 8), static_cast<int>(1 + rng() % 8),
                (rng() % 4 == 0), (rng() % 2 == 0)});
    }

    for (auto _ : state) {
        auto raw_frontiers = initial_frontiers;
        std::sort(raw_frontiers.begin(), raw_frontiers.end(),
                  [&](const FrontierCluster& lhs, const FrontierCluster& rhs) {
                      int lhs_distance = 999, rhs_distance = 999;
                      for (const auto& [anchor_x, anchor_y] : anchors) {
                          lhs_distance = std::min(lhs_distance,
                                                  move::Distance(lhs.x, lhs.y, anchor_x, anchor_y));
                          rhs_distance = std::min(rhs_distance,
                                                  move::Distance(rhs.x, rhs.y, anchor_x, anchor_y));
                      }
                      int lhs_score = lhs_distance - (lhs.on_road ? 4 : 0);
                      int rhs_score = rhs_distance - (rhs.on_road ? 4 : 0);
                      if (lhs_score != rhs_score) return lhs_score < rhs_score;
                      return lhs.on_road > rhs.on_road;
                  });
        benchmark::DoNotOptimize(raw_frontiers);
    }
    state.SetItemsProcessed(state.iterations() * 300);
}
BENCHMARK(BM_Engine_FrontierSort_UncachedDistance);

static void BM_Engine_FrontierSort_PrecomputedDistance(benchmark::State& state) {
    std::mt19937 rng(42);
    std::vector<std::pair<int, int>> anchors;
    for (int i = 0; i < 30; ++i) {
        anchors.emplace_back(rng() % 112, rng() % 156);
    }
    std::vector<FrontierCluster> initial_frontiers;
    for (int i = 0; i < 300; ++i) {
        initial_frontiers.push_back(FrontierCluster{
                static_cast<int>(rng() % 112), static_cast<int>(rng() % 156),
                static_cast<int>(1 + rng() % 8), static_cast<int>(1 + rng() % 8),
                (rng() % 4 == 0), (rng() % 2 == 0)});
    }

    struct ScoredFrontier {
        FrontierCluster cluster;
        int score = 0;
    };

    for (auto _ : state) {
        std::vector<ScoredFrontier> scored;
        scored.reserve(initial_frontiers.size());
        for (const auto& frontier : initial_frontiers) {
            int min_distance = 999;
            for (const auto& [anchor_x, anchor_y] : anchors) {
                min_distance = std::min(min_distance,
                                        move::Distance(frontier.x, frontier.y, anchor_x, anchor_y));
            }
            int score = min_distance - (frontier.on_road ? 4 : 0);
            scored.push_back({frontier, score});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const ScoredFrontier& lhs, const ScoredFrontier& rhs) {
                      if (lhs.score != rhs.score) return lhs.score < rhs.score;
                      return lhs.cluster.on_road > rhs.cluster.on_road;
                  });
        benchmark::DoNotOptimize(scored);
    }
    state.SetItemsProcessed(state.iterations() * 300);
}
BENCHMARK(BM_Engine_FrontierSort_PrecomputedDistance);

static void BM_Combat_8Battles_ResolveAllocating(benchmark::State& state) {
    auto game_ptr = test::SetupGame("ERYTHEA", test::Seed(42));
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    Side* side1 = game.GetMap().SideAt(1);

    std::vector<Army> atk_storage(8);
    std::vector<Army*> attackers;
    for (int i = 0; i < 8; ++i) {
        atk_storage[i].SetType(ArmyTypeId(1));
        atk_storage[i].SetStrength(5);
        atk_storage[i].SetOwner(side0->Id());
        attackers.push_back(&atk_storage[i]);
    }

    std::vector<Army> def_storage(8);
    std::vector<Army*> defenders;
    for (int i = 0; i < 8; ++i) {
        def_storage[i].SetType(ArmyTypeId(2));
        def_storage[i].SetStrength(4);
        def_storage[i].SetOwner(side1->Id());
        defenders.push_back(&def_storage[i]);
    }

    for (auto _ : state) {
        int wins = 0;
        int survivors = 0;
        int dead = 0;
        for (int i = 0; i < 8; ++i) {
            Battle b = combat::Resolve(game, attackers, defenders, 10, 10);
            if (b.won) wins++;
            survivors += static_cast<int>(b.attackers.size());
            dead += static_cast<int>(b.dead_defenders.size());
        }
        benchmark::DoNotOptimize(wins);
        benchmark::DoNotOptimize(survivors);
        benchmark::DoNotOptimize(dead);
    }
    state.SetItemsProcessed(state.iterations() * 8);
}
BENCHMARK(BM_Combat_8Battles_ResolveAllocating);

static void BM_Combat_8Battles_FastSimulateRepeated(benchmark::State& state) {
    auto game_ptr = test::SetupGame("ERYTHEA", test::Seed(42));
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    Side* side1 = game.GetMap().SideAt(1);

    std::vector<Army> atk_storage(8);
    std::vector<Army*> attackers;
    for (int i = 0; i < 8; ++i) {
        atk_storage[i].SetType(ArmyTypeId(1));
        atk_storage[i].SetStrength(5);
        atk_storage[i].SetOwner(side0->Id());
        attackers.push_back(&atk_storage[i]);
    }

    std::vector<Army> def_storage(8);
    std::vector<Army*> defenders;
    for (int i = 0; i < 8; ++i) {
        def_storage[i].SetType(ArmyTypeId(2));
        def_storage[i].SetStrength(4);
        def_storage[i].SetOwner(side1->Id());
        defenders.push_back(&def_storage[i]);
    }

    for (auto _ : state) {
        int wins = 0;
        int survivors = 0;
        int dead = 0;
        combat::FastSimulateRepeated(game, attackers, defenders, 10, 10, 8, wins, survivors, dead);
        benchmark::DoNotOptimize(wins);
        benchmark::DoNotOptimize(survivors);
        benchmark::DoNotOptimize(dead);
    }
    state.SetItemsProcessed(state.iterations() * 8);
}
BENCHMARK(BM_Combat_8Battles_FastSimulateRepeated);

static void BM_Combat_Advise_19Battles(benchmark::State& state) {
    auto game_ptr = test::SetupGame("ERYTHEA", test::Seed(42));
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    Side* side1 = game.GetMap().SideAt(1);

    std::vector<Army> atk_storage(8);
    std::vector<Army*> attackers;
    for (int i = 0; i < 8; ++i) {
        atk_storage[i].SetType(ArmyTypeId(1));
        atk_storage[i].SetStrength(5);
        atk_storage[i].SetOwner(side0->Id());
        attackers.push_back(&atk_storage[i]);
    }

    std::vector<Army> def_storage(8);
    std::vector<Army*> defenders;
    for (int i = 0; i < 8; ++i) {
        def_storage[i].SetType(ArmyTypeId(2));
        def_storage[i].SetStrength(4);
        def_storage[i].SetOwner(side1->Id());
        defenders.push_back(&def_storage[i]);
    }

    for (auto _ : state) {
        auto verdict = combat::Advise(game, attackers, defenders, 10, 10);
        benchmark::DoNotOptimize(verdict);
    }
    state.SetItemsProcessed(state.iterations() * 19);
}
BENCHMARK(BM_Combat_Advise_19Battles);

void BM_Engine_Pathfinder_Flood72_HiddenMap(benchmark::State& state) {  // NOLINT(readability-identifier-naming)
    auto opts = test::SeedWith(42, {{"hiddenMap", 1}, {"diplomacy", 1}});
    opts.sides[0] = test::MakeSideSetup(true, 2);
    auto game_ptr = test::SetupGame("ERYTHEA", opts);
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    ai::SetSideClassicAi(game, *side0, false);
    City* cap = side0->CapitalCity(game.GetMap());
    const int cap_x = cap ? cap->X() : 20;
    const int cap_y = cap ? cap->Y() : 20;

    for ([[maybe_unused]] auto step : state) {
        auto dist =
                game.GetPathfinder().Flood(game, 0, cap_x, cap_y, 72, move::kLand, false, false, 0);
        benchmark::DoNotOptimize(dist);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Engine_Pathfinder_Flood72_HiddenMap);

void BM_Engine_Belief_Update_HiddenMap(benchmark::State& state) {  // NOLINT(readability-identifier-naming)
    auto opts = test::SeedWith(42, {{"hiddenMap", 1}, {"diplomacy", 1}});
    opts.sides[0] = test::MakeSideSetup(true, 2);
    auto game_ptr = test::SetupGame("ERYTHEA", opts);
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    ai::SetSideClassicAi(game, *side0, false);

    for ([[maybe_unused]] auto step : state) {
        belief::Update(game, *side0);
        benchmark::DoNotOptimize(side0->ComputerPlayer());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Engine_Belief_Update_HiddenMap);

void BM_Evaluator_ComputeEmpireScore_HiddenMap(benchmark::State& state) {  // NOLINT(readability-identifier-naming)
    auto opts = test::SeedWith(42, {{"hiddenMap", 1}, {"diplomacy", 1}});
    opts.sides[0] = test::MakeSideSetup(true, 2);
    auto game_ptr = test::SetupGame("ERYTHEA", opts);
    Game& game = *game_ptr;
    Side* side0 = game.GetMap().SideAt(0);
    ai::SetSideClassicAi(game, *side0, false);

    for ([[maybe_unused]] auto step : state) {
        float score = ComputeEmpireScore(game, 0);
        benchmark::DoNotOptimize(score);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Evaluator_ComputeEmpireScore_HiddenMap);

}  // namespace
}  // namespace w2::ai::ml

BENCHMARK_MAIN();
