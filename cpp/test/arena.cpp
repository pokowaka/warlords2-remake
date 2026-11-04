// Multi-Threaded Self-Play Training & Elo Benchmark Arena (`w2arena`).
//
// Architectural Role: Trains the 4-Head Residual MLP (`PolicyNetwork`) via
// parallel Separable CMA-ES / Population-Based Evolution across multi-faction
// self-play matches under strict fair-information rules (`hiddenMap=1`,
// `viewEnemies=0`), optimizes multi-objective personality-aligned rewards,
// benchmarks against the 1993 Classic Warlord AI, and exports `cpp/ai_weights.json`.
//
// Usage (from repository root):
//   cpp/build/w2arena [--generations N] [--pop M] [--turns T] [--scenario NAME] [--out PATH]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"

using namespace w2;

namespace {

struct MatchResult {
  float candidateFitness = 0.0f;
  bool candidateWon = false;
  int candidateCities = 0;
  int classicCities = 0;
  int turnsPlayed = 0;
};

// Runs a headless match where a subset of sides use `candidateNet` (100% Learnable
// Fair-Information AI) and the remaining sides use either `baselineNet` or the
// 1993 Classic Warlord AI (`vsClassic = true`).
MatchResult runMatch(const std::string& dataDir, const std::string& scenario, uint32_t seedVal,
                     int maxTurns, const ai::ml::PolicyNetwork& candidateNet,
                     const ai::ml::PolicyNetwork& opponentNet, bool vsClassic) {
  game::NewGameOptions opts;
  opts.seed = (double)seedVal;
  opts.options = {{"hiddenMap", 1}, {"viewEnemies", 0}, {"diplomacy", 1}, {"quests", 1}};
  for (int i = 0; i < 8; i++) {
    opts.sides[i].computer = true;
    opts.sides[i].level = 2;  // Warlord tier
    opts.sides[i].card = (int)((seedVal + i * 3) % 9);
  }

  auto gp = game::newGame(dataDir, scenario, opts);
  Game& g = *gp;

  // Assign alternating sides to candidate vs opponent
  std::vector<int> activeSides;
  for (int i = 0; i < 8; i++) {
    if (g.map->sides[i].inUse) activeSides.push_back(i);
  }
  std::array<bool, 8> isCandidate{};
  for (size_t idx = 0; idx < activeSides.size(); idx++) {
    int s = activeSides[idx];
    bool cand = ((idx + seedVal) % 2 == 0);
    isCandidate[s] = cand;
    g.map->sides[s].classicAi = (!cand && vsClassic);
  }

  Side* cur = game::begin(g);
  int safetySteps = 0;
  while (cur && !g.over && g.turn <= maxTurns && safetySteps < maxTurns * 12) {
    safetySteps++;
    if (cur->computer && cur->alive) {
      if (isCandidate[cur->index]) {
        ai::modern::playTurn(g, *cur, nullptr, &candidateNet);
      } else if (vsClassic) {
        ai::playTurn(g, *cur, nullptr);
      } else {
        ai::modern::playTurn(g, *cur, nullptr, &opponentNet);
      }
    }
    cur = game::endTurn(g);
  }

  // Evaluate final board state and multi-objective personality alignment
  int totalStandingCities = 0;
  std::array<int, 8> sideCities{};
  int razedCount = 0;
  for (const auto& c : g.map->cities) {
    if (c.razed) {
      razedCount++;
      continue;
    }
    totalStandingCities++;
    if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
      sideCities[c.ownerIndex]++;
    }
  }
  totalStandingCities = std::max(1, totalStandingCities);

  MatchResult res;
  res.turnsPlayed = g.turn;

  float candScoreSum = 0.0f;
  int candCount = 0;
  int bestSide = NONE, bestCityCnt = -1;
  for (int s : activeSides) {
    if (sideCities[s] > bestCityCnt) {
      bestCityCnt = sideCities[s];
      bestSide = s;
    }
    if (isCandidate[s]) {
      res.candidateCities += sideCities[s];
    } else {
      res.classicCities += sideCities[s];
    }
  }

  if (bestSide != NONE && isCandidate[bestSide]) {
    res.candidateWon = true;
  }

  for (int s : activeSides) {
    if (!isCandidate[s]) continue;
    candCount++;
    const Side& side = g.map->sides[s];
    const auto& prof = ai::personality::forSide(side);

    float cityShare = (float)sideCities[s] / (float)totalStandingCities;
    float winBonus = (bestSide == s) ? 1.0f : 0.0f;
    if (g.over && g.won == s) winBonus += 0.6f;
    float speedBonus = (bestSide == s) ? (float)(maxTurns - std::min(maxTurns, g.turn)) / (float)maxTurns * 0.4f : 0.0f;
    float aliveBonus = side.alive ? 0.25f : -0.5f;

    // Count unit classes & heroes for personality behavioral alignment Phi(i)
    int heroes = 0, cav = 0, fliers = 0, siege = 0, totalUnits = 0;
    for (const Army* a : g.armies) {
      if (a->owner != s || a->x == NONE) continue;
      totalUnits++;
      if (a->hero()) heroes++;
      const ArmyType* at = g.types.byId(a->type);
      if (at && at->flies) fliers++;
      else if (a->maxMoves >= 14) cav++;
      if (at && (at->siege || at->bonus[48] != 0)) siege++;
    }
    float unitDenom = (float)std::max(1, totalUnits);

    int warPartners = 0, allies = 0;
    float avgTrust = 0.0f;
    int otherCnt = 0;
    for (int o : activeSides) {
      if (o == s) continue;
      otherCnt++;
      if (diplomacy::state(g, s, o) == diplomacy::WAR) warPartners++;
      if (diplomacy::isAllied(g, s, o)) allies++;
      avgTrust += (float)diplomacy::trust(g, s, o) / 100.0f;
    }
    if (otherCnt > 0) avgTrust /= (float)otherCnt;

    // Normalized behavioral statistics Phi_behavior in [-1, 1]^10
    ai::personality::PersonalityVector phi{};
    phi[ai::personality::AGGRESSION] = std::tanh(cityShare * 2.0f + (float)warPartners * 0.3f - 0.4f);
    phi[ai::personality::HONOUR] = std::clamp(avgTrust, -1.0f, 1.0f);
    phi[ai::personality::AVARICE] = std::tanh((float)side.gold / 600.0f - 0.3f);
    phi[ai::personality::SCORCHED_EARTH] = std::tanh((float)razedCount / 4.0f - 0.2f);
    phi[ai::personality::HERO_FOCUS] = std::tanh((float)heroes / 2.0f - 0.3f);
    phi[ai::personality::COALITION_DRIVE] = std::tanh((float)allies * 0.8f);
    phi[ai::personality::CAVALRY_AFFINITY] = std::tanh((float)cav / unitDenom * 3.0f - 0.4f);
    phi[ai::personality::FLIER_AFFINITY] = std::tanh((float)fliers / unitDenom * 3.0f - 0.3f);
    phi[ai::personality::SIEGE_AFFINITY] = std::tanh((float)siege / unitDenom * 3.0f - 0.3f);
    phi[ai::personality::CAUTION] = std::tanh((float)totalUnits / (float)std::max(1, sideCities[s] * 3) - 0.5f);

    float dotAlign = 0.0f;
    for (int k = 0; k < ai::personality::TRAIT_DIM; k++) {
      dotAlign += prof.traits[k] * phi[k];
    }
    dotAlign /= (float)ai::personality::TRAIT_DIM;

    float rVictory = winBonus + 2.2f * cityShare + speedBonus + aliveBonus;
    candScoreSum += rVictory + 0.25f * dotAlign;
  }

  res.candidateFitness = candCount > 0 ? candScoreSum / (float)candCount : 0.0f;
  return res;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dataDir = "original";
  std::string scenario = "ERYTHEA";
  std::string outPath = "cpp/ai_weights.json";
  int generations = 6;
  int popSize = 12;
  int maxTurns = 22;
  int benchmarkGames = 12;
  bool benchmarkOnly = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--generations" && i + 1 < argc) generations = std::stoi(argv[++i]);
    else if (arg == "--pop" && i + 1 < argc) popSize = std::max(4, std::stoi(argv[++i]));
    else if (arg == "--turns" && i + 1 < argc) maxTurns = std::max(5, std::stoi(argv[++i]));
    else if (arg == "--scenario" && i + 1 < argc) scenario = argv[++i];
    else if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
    else if (arg == "--benchmark-games" && i + 1 < argc) benchmarkGames = std::max(2, std::stoi(argv[++i]));
    else if (arg == "--benchmark-only") benchmarkOnly = true;
  }

  if (!fileExists(dataDir + "/TERRAIN0/ARMYTYPE.DAT")) {
    std::fprintf(stderr, "Cannot find game data in '%s': run from repository root.\n", dataDir.c_str());
    return 1;
  }

  unsigned int hwThreads = std::max(2u, std::thread::hardware_concurrency());
  std::printf("=== Warlords II Neural Self-Play Arena (w2arena) ===\n");
  std::printf("Scenario: %s | Params: %d | Threads: %u | Pop: %d | Gens: %d | MaxTurns: %d\n",
              scenario.c_str(), ai::ml::PolicyNetwork::TOTAL_PARAMS, hwThreads, popSize, generations, maxTurns);

  ai::ml::PolicyNetwork bestNet;
  if (std::filesystem::exists(outPath)) {
    if (bestNet.loadJson(outPath)) {
      std::printf("Loaded existing checkpoint from %s\n", outPath.c_str());
    }
  }

  std::vector<float> mean = bestNet.flatten();
  const size_t D = mean.size();
  std::vector<float> diagStd(D, 0.045f);
  float stepSigma = 1.0f;
  float bestFitness = -1e9f;

  // Evaluate initial baseline fitness
  {
    MatchResult m0 = runMatch(dataDir, scenario, 101, maxTurns, bestNet, bestNet, true);
    MatchResult m1 = runMatch(dataDir, scenario, 202, maxTurns, bestNet, bestNet, false);
    bestFitness = 0.5f * (m0.candidateFitness + m1.candidateFitness);
    std::printf("[Init] Baseline fitness: %.3f (cities vs classic: %d vs %d)\n",
                bestFitness, m0.candidateCities, m0.classicCities);
  }

  std::mt19937 rng(42);
  std::normal_distribution<float> norm(0.0f, 1.0f);

  if (!benchmarkOnly && generations > 0) {
    int eliteCount = std::max(2, popSize / 3);
    std::vector<float> weightsLog(eliteCount);
    float wSum = 0.0f;
    for (int i = 0; i < eliteCount; i++) {
      weightsLog[i] = std::log((float)eliteCount + 0.5f) - std::log((float)(i + 1));
      wSum += weightsLog[i];
    }
    for (float& w : weightsLog) w /= wSum;

    for (int gen = 1; gen <= generations; gen++) {
      auto t0 = std::chrono::steady_clock::now();

      std::vector<std::vector<float>> candidates(popSize, std::vector<float>(D));
      std::vector<float> fitnesses(popSize, 0.0f);

      // Antithetic sampling around current mean for low-variance evolutionary updates
      for (int p = 0; p < popSize; p += 2) {
        for (size_t d = 0; d < D; d++) {
          float z = norm(rng) * stepSigma * diagStd[d];
          candidates[p][d] = mean[d] + z;
          if (p + 1 < popSize) {
            candidates[p + 1][d] = mean[d] - z;
          }
        }
      }

      std::atomic<int> nextIdx{0};
      std::vector<std::thread> workers;
      int numWorkers = std::min((int)hwThreads, popSize);
      for (int w = 0; w < numWorkers; w++) {
        workers.emplace_back([&]() {
          while (true) {
            int idx = nextIdx.fetch_add(1);
            if (idx >= popSize) break;
            ai::ml::PolicyNetwork candNet;
            candNet.unflatten(candidates[idx]);
            uint32_t seedA = (uint32_t)(1000 + gen * 37 + (idx / 2) * 13);
            uint32_t seedB = (uint32_t)(2000 + gen * 53 + (idx / 2) * 17);
            // Play 1 match vs current best self-play policy and 1 match vs 1993 Classic AI
            MatchResult rSelf = runMatch(dataDir, scenario, seedA, maxTurns, candNet, bestNet, false);
            MatchResult rClas = runMatch(dataDir, scenario, seedB, maxTurns, candNet, bestNet, true);
            fitnesses[idx] = 0.5f * (rSelf.candidateFitness + rClas.candidateFitness);
          }
        });
      }
      for (auto& th : workers) th.join();

      // Rank candidates by fitness descending
      std::vector<int> order(popSize);
      std::iota(order.begin(), order.end(), 0);
      std::sort(order.begin(), order.end(), [&](int a, int b) {
        return fitnesses[a] > fitnesses[b];
      });

      int topIdx = order[0];
      if (fitnesses[topIdx] > bestFitness) {
        bestFitness = fitnesses[topIdx];
        bestNet.unflatten(candidates[topIdx]);
      }

      // Separable CMA-ES mean & diagonal covariance update
      std::vector<float> newMean(D, 0.0f);
      for (int rank = 0; rank < eliteCount; rank++) {
        int cIdx = order[rank];
        float w = weightsLog[rank];
        for (size_t d = 0; d < D; d++) {
          newMean[d] += w * candidates[cIdx][d];
        }
      }
      float covLr = 0.15f;
      for (size_t d = 0; d < D; d++) {
        float varEst = 0.0f;
        for (int rank = 0; rank < eliteCount; rank++) {
          int cIdx = order[rank];
          float diff = (candidates[cIdx][d] - mean[d]) / std::max(1e-4f, stepSigma);
          varEst += weightsLog[rank] * (diff * diff);
        }
        diagStd[d] = std::clamp((1.0f - covLr) * diagStd[d] + covLr * std::sqrt(varEst), 0.008f, 0.12f);
      }
      mean = std::move(newMean);
      stepSigma *= 0.96f;

      auto t1 = std::chrono::steady_clock::now();
      double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
      std::printf("[Gen %2d/%2d] BestFit: %.3f | GenTop: %.3f | GenAvg: %.3f | Time: %.2fs\n",
                  gen, generations, bestFitness, fitnesses[topIdx],
                  std::accumulate(fitnesses.begin(), fitnesses.end(), 0.0f) / (float)popSize,
                  elapsedSec);
    }

    if (bestNet.saveJson(outPath)) {
      std::printf("Saved trained PolicyNetwork (%d params) to %s\n",
                  ai::ml::PolicyNetwork::TOTAL_PARAMS, outPath.c_str());
    }
  }

  // Head-to-Head Elo Benchmark vs 1993 Classic Warlord AI
  std::printf("\n=== Head-to-Head Benchmark: Learnable Fair-Info AI vs 1993 Classic AI ===\n");
  std::atomic<int> nextGame{0};
  std::atomic<int> modernWins{0};
  std::atomic<int> modernCitiesTotal{0};
  std::atomic<int> classicCitiesTotal{0};

  std::vector<std::thread> benchThreads;
  int numBenchWorkers = std::min((int)hwThreads, benchmarkGames);
  for (int w = 0; w < numBenchWorkers; w++) {
    benchThreads.emplace_back([&]() {
      while (true) {
        int gIdx = nextGame.fetch_add(1);
        if (gIdx >= benchmarkGames) break;
        std::string scn = (gIdx % 2 == 0) ? scenario : "ERYTHEA";
        MatchResult res = runMatch(dataDir, scn, (uint32_t)(5000 + gIdx * 29), maxTurns, bestNet, bestNet, true);
        if (res.candidateWon) modernWins.fetch_add(1);
        modernCitiesTotal.fetch_add(res.candidateCities);
        classicCitiesTotal.fetch_add(res.classicCities);
      }
    });
  }
  for (auto& th : benchThreads) th.join();

  int wins = modernWins.load();
  int mCities = modernCitiesTotal.load();
  int cCities = classicCitiesTotal.load();
  float winRate = (float)wins / (float)std::max(1, benchmarkGames);
  float clampedP = std::clamp(winRate, 0.05f, 0.95f);
  float eloDiff = -400.0f * std::log10(1.0f / clampedP - 1.0f);

  std::printf("Benchmark Games: %d (Fair-Info HiddenMap=1, ViewEnemies=0 vs Classic Warlord AI)\n", benchmarkGames);
  std::printf("Learnable AI Wins: %d/%d (%.1f%%) | Cities Held: Learnable %d vs Classic %d | Est. Elo Delta: %+.0f\n",
              wins, benchmarkGames, winRate * 100.0f, mCities, cCities, eloDiff);

  return 0;
}
