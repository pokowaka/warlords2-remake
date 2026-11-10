#include <array>
#include <random>
#include <vector>

#include "benchmark/benchmark.h"
#include "warlords/ai/ml_policy.hpp"

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

}  // namespace
}  // namespace w2::ai::ml

BENCHMARK_MAIN();
