// Tabula-Rasa Self-Play Reinforcement Learning Arena (`w2arena`).
//
// Architectural Role: Trains the 4-Head Differentiable Policy & Value Network
// (`PolicyNetwork`) from pure zero-mean random initialization using multi-threaded
// Proximal Policy Optimization (PPO), Generalized Advantage Estimation (GAE),
// counterfactual multi-stack credit assignment, and a Hall-of-Fame self-play
// archive across diverse game presets (Open Map, Diplomacy, Hidden Map) and
// scenarios (ERYTHEA, ISLADIA, DRAGON, TUTORIA, SORCERY, HADESHA).
// The 1993 Classic AI is never used during training—only as an external
// evaluation benchmark to measure emergent strategic competence.
//
// Usage (from repository root):
//   cpp/build/w2arena [--iterations N] [--games-per-iter G] [--turns T]
//                     [--from-scratch] [--eval-every E] [--out PATH] [--benchmark-only]

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

using namespace w2;

namespace {

struct TrainingPreset {
  const char* label;
  int hiddenMap;
  int diplomacy;
  int neutralCities;
};

constexpr TrainingPreset PRESETS[4] = {
    {"Beginner-Open", 0, 0, 0},
    {"Interm-Diplo", 0, 1, 1},
    {"Adv-Hidden", 1, 1, 2},
    {"Fog-TotalWar", 1, 0, 1},
};

struct SideEpisode {
  int sideIndex = 0;
  std::vector<ai::ml::TurnStepRecord> turns;
  float finalPhi = 0.0f;
  float finalBootstrapValue = 0.0f;
  float terminalBonus = 0.0f;
  bool truncated = false;
  int finalCities = 0;
};

struct RolloutGameSummary {
  std::vector<SideEpisode> episodes;
  int totalLearnerCities = 0;
  int learnerSides = 0;
  int unownedNeutralCities = 0;
};

// Runs one multi-faction Pure Self-Play game (Learner vs Hall-of-Fame / Self),
// collecting PPO trajectories for the learner sides under strict fair-information rules.
RolloutGameSummary runSelfPlayRollout(const std::string& dataDir, const std::string& scenario,
                                      const TrainingPreset& preset, uint32_t gameSeed,
                                      int maxTurns, float temperature,
                                      const ai::ml::PolicyNetwork& learnerNet,
                                      const ai::ml::PolicyNetwork& hofOpponentNet,
                                      bool pureMirrorSelfPlay) {
  game::NewGameOptions opts;
  opts.seed = (double)gameSeed;
  opts.options = {
      {"hiddenMap", preset.hiddenMap},
      {"viewEnemies", 0},
      {"diplomacy", preset.diplomacy},
      {"neutralCities", preset.neutralCities},
      {"quests", 1},
  };
  for (int i = 0; i < 8; i++) {
    opts.sides[i].computer = true;
    opts.sides[i].level = 2;
    opts.sides[i].card = (int)((gameSeed + i * 5) % 9);
  }

  auto gp = game::newGame(dataDir, scenario, opts);
  Game& g = *gp;

  std::vector<int> activeSides;
  for (int i = 0; i < 8; i++) {
    if (g.map->sides[i].inUse) {
      g.map->sides[i].classicAi = false;
      activeSides.push_back(i);
    }
  }

  std::array<bool, 8> isLearner{};
  std::array<SideEpisode, 8> sideEps{};
  std::array<ai::ml::RolloutSampler, 8> samplers{};
  for (size_t idx = 0; idx < activeSides.size(); idx++) {
    int s = activeSides[idx];
    bool recordThisSide = pureMirrorSelfPlay || ((idx + gameSeed) % 2 == 0);
    isLearner[s] = recordThisSide;
    sideEps[s].sideIndex = s;
    samplers[s].rngState = (gameSeed * 2654435761u) ^ (uint32_t)((s + 1) * 2246822519u);
    samplers[s].temperature = temperature;
  }

  Side* cur = game::begin(g);
  int safetySteps = 0;
  while (cur && !g.over && g.turn <= maxTurns && safetySteps < maxTurns * 12) {
    safetySteps++;
    int s = cur->index;
    if (cur->computer && cur->alive) {
      if (isLearner[s]) {
        sideEps[s].turns.emplace_back();
        samplers[s].currentTurn = &sideEps[s].turns.back();
        ai::modern::playTurn(g, *cur, nullptr, &learnerNet, &samplers[s]);
        samplers[s].currentTurn = nullptr;
      } else {
        // Hall-of-Fame self-play opponent (also stochastic with mild temperature for variety)
        ai::ml::RolloutSampler oppSampler;
        oppSampler.rngState = samplers[s].rngState ^ (uint32_t)(g.turn * 374761393u);
        oppSampler.temperature = 0.85f;
        ai::modern::playTurn(g, *cur, nullptr, &hofOpponentNet, &oppSampler);
      }
    }
    cur = game::endTurn(g);
  }

  std::array<int, 8> sideCities{};
  int maxSideCities = 0;
  int neutralsLeft = 0;
  for (const auto& c : g.map->cities) {
    if (c.razed) continue;
    if (c.ownerIndex == NONE) {
      neutralsLeft++;
    } else if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
      sideCities[c.ownerIndex]++;
      maxSideCities = std::max(maxSideCities, sideCities[c.ownerIndex]);
    }
  }

  RolloutGameSummary summary;
  summary.unownedNeutralCities = neutralsLeft;

  for (int s : activeSides) {
    if (!isLearner[s] || sideEps[s].turns.empty()) continue;
    Side& side = g.map->sides[s];
    SideEpisode& ep = sideEps[s];
    ep.finalCities = sideCities[s];
    ep.finalPhi = ai::ml::computeEmpirePotential(g, s);
    ep.truncated = (!g.over && side.alive);
    if (ep.truncated) {
      ai::belief::update(g, side);
      ep.finalBootstrapValue = learnerNet.evaluateValue(ai::ml::buildContext(g, side));
    } else {
      ep.finalBootstrapValue = 0.0f;
    }

    if (!side.alive) {
      ep.terminalBonus = -0.60f;
    } else if (g.over && g.won == s) {
      ep.terminalBonus = 1.00f;
    } else if (sideCities[s] == maxSideCities && maxSideCities > 1) {
      ep.terminalBonus = 0.35f;
    }

    summary.totalLearnerCities += sideCities[s];
    summary.learnerSides++;
    summary.episodes.push_back(std::move(ep));
  }
  return summary;
}

void computeGAE(std::vector<SideEpisode>& episodes, float gamma, float gaeLambda) {
  for (auto& ep : episodes) {
    const int T = (int)ep.turns.size();
    if (T == 0) continue;
    float gae = 0.0f;
    for (int t = T - 1; t >= 0; t--) {
      auto& cur = ep.turns[t];
      float nextPhi = (t + 1 < T) ? ep.turns[t + 1].startPhi : ep.finalPhi;
      float nextVal = (t + 1 < T) ? ep.turns[t + 1].valuePred : ep.finalBootstrapValue;
      float term = (t == T - 1) ? ep.terminalBonus : 0.0f;

      float stepReward = (gamma * nextPhi - cur.startPhi) + term;
      float delta = stepReward + gamma * nextVal - cur.valuePred;
      gae = delta + gamma * gaeLambda * gae;
      cur.advantage = gae;
      cur.returnTarget = std::clamp(gae + cur.valuePred, -1.0f, 1.0f);

      float sumMissionDelta = 0.0f;
      for (const auto& ms : cur.missionSteps) {
        sumMissionDelta += ms.immediateDeltaPhi;
      }
      // Counterfactual multi-stack baseline: exclude unrelated stacks' deltas in this turn
      cur.restAdvantage = cur.advantage - sumMissionDelta;

      // Causal intra-turn return-to-go so Wave 1 reconnaissance receives discounted credit
      // when subsequent waves capture newly uncovered castles on the same turn
      float futureIntra = 0.0f;
      for (int m = (int)cur.missionSteps.size() - 1; m >= 0; m--) {
        cur.missionSteps[m].causalAdvantage =
            cur.missionSteps[m].immediateDeltaPhi + futureIntra + cur.restAdvantage;
        futureIntra = 0.5f * (cur.missionSteps[m].immediateDeltaPhi + futureIntra);
      }
    }
  }
}

struct AdvantageStats {
  float mean = 0.0f;
  float invStd = 1.0f;
  float normalize(float x) const {
    return std::clamp((x - mean) * invStd, -4.0f, 4.0f);
  }
};

AdvantageStats fitStats(const std::vector<float>& vals) {
  AdvantageStats st;
  if (vals.size() < 2) return st;
  double sum = 0.0;
  for (float v : vals) sum += v;
  st.mean = (float)(sum / (double)vals.size());
  double sq = 0.0;
  for (float v : vals) {
    double d = (double)v - (double)st.mean;
    sq += d * d;
  }
  float stdVal = (float)std::sqrt(sq / (double)vals.size());
  st.invStd = 1.0f / std::max(1e-4f, stdVal);
  return st;
}

struct PPOGradResult {
  float policyLossSum = 0.0f;
  float valueLossSum = 0.0f;
  float entropySum = 0.0f;
  int sampleCount = 0;
};

void accumulateCategoricalPPO(
    const std::vector<float>& rawLogits, const std::vector<float>* logMult,
    int chosenIdx, float oldLogProb,
    float normAdv, float invTemp, float clipEps, float entCoef,
    std::vector<float>& outDLogits, PPOGradResult& stats) {
  const size_t K = rawLogits.size();
  outDLogits.assign(K, 0.0f);
  if (K <= 1) return;

  auto effZ = [&](size_t i) {
    float lm = (logMult && i < logMult->size()) ? (*logMult)[i] : 0.0f;
    return (rawLogits[i] - lm) * invTemp;
  };

  float maxZ = effZ(0);
  for (size_t i = 1; i < K; i++) maxZ = std::max(maxZ, effZ(i));

  std::vector<float> probs(K);
  float sumExp = 0.0f;
  for (size_t i = 0; i < K; i++) {
    float e = std::exp(std::clamp(effZ(i) - maxZ, -30.0f, 0.0f));
    probs[i] = e;
    sumExp += e;
  }
  for (float& p : probs) p /= std::max(1e-12f, sumExp);

  float newLogProb = std::log(std::max(1e-12f, probs[chosenIdx]));
  float ratio = std::exp(std::clamp(newLogProb - oldLogProb, -10.0f, 10.0f));
  float clippedRatio = std::clamp(ratio, 1.0f - clipEps, 1.0f + clipEps);
  float surr1 = ratio * normAdv;
  float surr2 = clippedRatio * normAdv;
  stats.policyLossSum += -std::min(surr1, surr2);

  bool isClipped = (normAdv > 0.0f && ratio > 1.0f + clipEps) ||
                   (normAdv < 0.0f && ratio < 1.0f - clipEps);
  float dPolDLogP = isClipped ? 0.0f : (-ratio * normAdv);

  // Regularize via KL to the multiplicity-balanced family-uniform prior q_j proportional to exp(-logMult_j)
  float entropy = 0.0f;
  float meanAdj = 0.0f;
  for (size_t j = 0; j < K; j++) {
    float p = probs[j];
    if (p > 1e-12f) {
      float lp = std::log(p);
      float lm = (logMult && j < logMult->size()) ? (*logMult)[j] : 0.0f;
      entropy -= p * lp;
      meanAdj += p * (lp + lm);
    }
  }
  float normLogK = std::max(1.0f, std::log((float)K));
  stats.entropySum += entropy / normLogK;
  stats.sampleCount++;

  float effEntCoef = entCoef / normLogK;
  for (size_t j = 0; j < K; j++) {
    float ind = ((int)j == chosenIdx) ? 1.0f : 0.0f;
    float dLogPDu = (ind - probs[j]) * invTemp;
    float logPj = std::log(std::max(1e-12f, probs[j]));
    float lm = (logMult && j < logMult->size()) ? (*logMult)[j] : 0.0f;
    float dKLdDu = effEntCoef * probs[j] * ((logPj + lm) - meanAdj) * invTemp;
    outDLogits[j] = dPolDLogP * dLogPDu + dKLdDu;
  }
}

struct EvalBenchmark {
  int learnerCitiesT10 = 0;
  int classicCitiesT10 = 0;
  int learnerCitiesEnd = 0;
  int classicCitiesEnd = 0;
  int learnerWins = 0;
  int totalGames = 0;
};

// External read-only benchmark against the 1993 Classic AI across Open Map,
// Intermediate Diplomacy, Hidden Map, and Island scenarios.
EvalBenchmark evaluateVsClassicSuite(const std::string& dataDir,
                                     const ai::ml::PolicyNetwork& policy,
                                     int maxTurns) {
  struct SuiteCase {
    const char* scenario;
    TrainingPreset preset;
    uint32_t seed;
  };
  const SuiteCase cases[8] = {
      {"ERYTHEA", PRESETS[0], 101},
      {"ERYTHEA", PRESETS[0], 102},
      {"ERYTHEA", PRESETS[1], 201},
      {"ERYTHEA", PRESETS[2], 301},
      {"ERYTHEA", PRESETS[2], 302},
      {"ISLADIA", PRESETS[0], 401},
      {"ISLADIA", PRESETS[2], 402},
      {"DRAGON", PRESETS[2], 501},
  };

  EvalBenchmark bench;
  bench.totalGames = 8;
  std::mutex mu;
  std::vector<std::thread> workers;
  for (int ci = 0; ci < 8; ci++) {
    workers.emplace_back([&, ci]() {
      const auto& sc = cases[ci];
      game::NewGameOptions opts;
      opts.seed = (double)sc.seed;
      opts.options = {
          {"hiddenMap", sc.preset.hiddenMap},
          {"viewEnemies", 0},
          {"diplomacy", sc.preset.diplomacy},
          {"neutralCities", sc.preset.neutralCities},
          {"quests", 1},
      };
      for (int i = 0; i < 8; i++) {
        opts.sides[i].computer = true;
        opts.sides[i].level = 2;
        opts.sides[i].card = (i + ci) % 9;
      }
      auto gp = game::newGame(dataDir, sc.scenario, opts);
      Game& g = *gp;

      std::array<bool, 8> isModern{};
      int idx = 0;
      for (int i = 0; i < 8; i++) {
        if (!g.map->sides[i].inUse) continue;
        bool mod = ((idx + ci) % 2 == 0);
        isModern[i] = mod;
        g.map->sides[i].classicAi = !mod;
        idx++;
      }

      int modT10 = 0, clsT10 = 0;
      bool recordedT10 = false;
      Side* cur = game::begin(g);
      int safety = 0;
      while (cur && !g.over && g.turn <= maxTurns && safety < maxTurns * 12) {
        safety++;
        if (g.turn > 10 && !recordedT10) {
          recordedT10 = true;
          for (const auto& c : g.map->cities) {
            if (!c.razed && c.ownerIndex >= 0 && c.ownerIndex < 8) {
              if (isModern[c.ownerIndex]) modT10++;
              else clsT10++;
            }
          }
        }
        if (cur->computer && cur->alive) {
          if (isModern[cur->index]) {
            ai::modern::playTurn(g, *cur, nullptr, &policy, nullptr);
          } else {
            ai::playTurn(g, *cur, nullptr);
          }
        }
        cur = game::endTurn(g);
      }

      int modEnd = 0, clsEnd = 0;
      std::array<int, 8> sideCnt{};
      int bestSide = NONE, bestCnt = -1;
      for (const auto& c : g.map->cities) {
        if (!c.razed && c.ownerIndex >= 0 && c.ownerIndex < 8) {
          sideCnt[c.ownerIndex]++;
          if (isModern[c.ownerIndex]) modEnd++;
          else clsEnd++;
          if (sideCnt[c.ownerIndex] > bestCnt) {
            bestCnt = sideCnt[c.ownerIndex];
            bestSide = c.ownerIndex;
          }
        }
      }
      if (!recordedT10) {
        modT10 = modEnd;
        clsT10 = clsEnd;
      }

      std::lock_guard<std::mutex> lk(mu);
      bench.learnerCitiesT10 += modT10;
      bench.classicCitiesT10 += clsT10;
      bench.learnerCitiesEnd += modEnd;
      bench.classicCitiesEnd += clsEnd;
      if (bestSide != NONE && isModern[bestSide]) bench.learnerWins++;
    });
  }
  for (auto& th : workers) th.join();
  return bench;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dataDir = "original";
  std::string outPath = "cpp/ai_weights.json";
  int iterations = 80;
  int gamesPerIter = 16;
  int maxTurns = 22;
  int evalEvery = 10;
  bool fromScratch = false;
  bool benchmarkOnly = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if ((arg == "--iterations" || arg == "--generations") && i + 1 < argc) {
      iterations = std::max(0, std::stoi(argv[++i]));
    } else if ((arg == "--games-per-iter" || arg == "--pop") && i + 1 < argc) {
      gamesPerIter = std::max(2, std::stoi(argv[++i]));
    } else if (arg == "--turns" && i + 1 < argc) {
      maxTurns = std::max(6, std::stoi(argv[++i]));
    } else if (arg == "--eval-every" && i + 1 < argc) {
      evalEvery = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--out" && i + 1 < argc) {
      outPath = argv[++i];
    } else if (arg == "--from-scratch") {
      fromScratch = true;
    } else if (arg == "--benchmark-only") {
      benchmarkOnly = true;
    }
  }

  if (!fileExists(dataDir + "/TERRAIN0/ARMYTYPE.DAT")) {
    std::fprintf(stderr, "Cannot find game data in '%s': run from repository root.\n",
                 dataDir.c_str());
    return 1;
  }

  const std::vector<std::string> scenarios = {
      "ERYTHEA", "ISLADIA", "DRAGON", "TUTORIA", "SORCERY", "HADESHA"};

  unsigned int hwThreads = std::max(2u, std::thread::hardware_concurrency());
  std::printf("=== Warlords II Tabula-Rasa PPO Self-Play Arena (w2arena) ===\n");
  std::printf("Params: %d | Threads: %u | Games/Iter: %d | Iters: %d | MaxTurns: %d\n",
              ai::ml::PolicyNetwork::TOTAL_PARAMS, hwThreads, gamesPerIter, iterations, maxTurns);

  ai::ml::PolicyNetwork learner;
  if (fromScratch) {
    learner.initRandomWeights(2026);
    std::printf("[Init] Initialized 100%% Tabula Rasa random weights (zero human priors).\n");
  } else if (std::filesystem::exists(outPath) && learner.loadJson(outPath)) {
    std::printf("[Init] Loaded existing checkpoint from %s\n", outPath.c_str());
  } else {
    learner.initRandomWeights(2026);
    std::printf("[Init] No compatible checkpoint found; initialized Tabula Rasa random weights.\n");
  }

  EvalBenchmark initBench = evaluateVsClassicSuite(dataDir, learner, maxTurns);
  std::printf("[Eval Iter 0] Cities@T10: Learner %d vs Classic %d | Cities@End: %d vs %d | Wins: %d/%d\n",
              initBench.learnerCitiesT10, initBench.classicCitiesT10,
              initBench.learnerCitiesEnd, initBench.classicCitiesEnd,
              initBench.learnerWins, initBench.totalGames);
  std::fflush(stdout);

  if (benchmarkOnly || iterations <= 0) {
    return 0;
  }

  // Hall-of-Fame archive of past self-play policies
  std::vector<ai::ml::PolicyNetwork> hallOfFame = {learner};
  ai::ml::PolicyNetwork bestCheckpoint = learner;
  float bestEvalRatio = (float)initBench.learnerCitiesEnd /
                        (float)std::max(1, initBench.learnerCitiesEnd + initBench.classicCitiesEnd);

  // Adam optimizer state
  const size_t D = ai::ml::PolicyNetwork::TOTAL_PARAMS;
  std::vector<float> params = learner.flatten();
  std::vector<float> adamM(D, 0.0f);
  std::vector<float> adamV(D, 0.0f);
  int adamStep = 0;

  constexpr float GAMMA = 0.96f;
  constexpr float GAE_LAMBDA = 0.92f;
  constexpr float CLIP_EPS = 0.20f;
  constexpr int PPO_EPOCHS = 4;

  for (int iter = 1; iter <= iterations; iter++) {
    auto t0 = std::chrono::steady_clock::now();
    float progress = (float)(iter - 1) / (float)std::max(1, iterations - 1);
    float temperature = 1.00f - 0.65f * progress;
    float lr = 0.0032f * (1.0f - 0.70f * progress);
    float entCoef = 0.014f * (1.0f - 0.85f * progress);
    float invTemp = 1.0f / temperature;

    std::vector<RolloutGameSummary> gameSummaries(gamesPerIter);
    std::atomic<int> nextGame{0};
    std::vector<std::thread> rolloutThreads;
    unsigned int nWorkers = std::min<unsigned int>(hwThreads, (unsigned int)gamesPerIter);

    for (unsigned int w = 0; w < nWorkers; w++) {
      rolloutThreads.emplace_back([&]() {
        while (true) {
          int gi = nextGame.fetch_add(1);
          if (gi >= gamesPerIter) break;
          const std::string& scen = scenarios[(iter * 7 + gi) % scenarios.size()];
          const TrainingPreset& preset = PRESETS[(iter + gi) % 4];
          uint32_t seed = (uint32_t)(iter * 1009 + gi * 97 + 17);
          bool mirrorSelf = (gi % 3 != 0) || hallOfFame.empty();
          const auto& oppNet = hallOfFame[(iter + gi) % hallOfFame.size()];
          gameSummaries[gi] = runSelfPlayRollout(
              dataDir, scen, preset, seed, maxTurns, temperature, learner, oppNet, mirrorSelf);
        }
      });
    }
    for (auto& th : rolloutThreads) th.join();

    std::vector<SideEpisode> allEpisodes;
    int totalCities = 0, totalLearnerSides = 0, totalNeutralsLeft = 0;
    for (auto& gs : gameSummaries) {
      totalCities += gs.totalLearnerCities;
      totalLearnerSides += gs.learnerSides;
      totalNeutralsLeft += gs.unownedNeutralCities;
      for (auto& ep : gs.episodes) {
        allEpisodes.push_back(std::move(ep));
      }
    }

    computeGAE(allEpisodes, GAMMA, GAE_LAMBDA);

    // Fit per-head advantage normalization across the batch
    std::vector<float> missionAdvs, econAdvs, diploAdvs;
    for (const auto& ep : allEpisodes) {
      for (const auto& tr : ep.turns) {
        for (const auto& ms : tr.missionSteps) {
          missionAdvs.push_back(ms.causalAdvantage);
        }
        for (size_t i = 0; i < tr.econSteps.size(); i++) {
          econAdvs.push_back(tr.advantage);
        }
        for (size_t i = 0; i < tr.diploSteps.size(); i++) {
          diploAdvs.push_back(tr.advantage);
        }
      }
    }
    AdvantageStats mStats = fitStats(missionAdvs);
    AdvantageStats eStats = fitStats(econAdvs);
    AdvantageStats dStats = fitStats(diploAdvs);

    PPOGradResult lastEpochStats;

    // Run PPO epochs over the collected trajectories
    for (int epIdx = 0; epIdx < PPO_EPOCHS; epIdx++) {
      std::atomic<size_t> nextEp{0};
      unsigned int gWorkers = std::min<unsigned int>(hwThreads, (unsigned int)std::max<size_t>(1, allEpisodes.size()));
      std::vector<ai::ml::PolicyNetwork> threadGrads(gWorkers);
      std::vector<PPOGradResult> threadStats(gWorkers);
      for (auto& tg : threadGrads) tg.zero();

      std::vector<std::thread> gradThreads;
      for (unsigned int w = 0; w < gWorkers; w++) {
        gradThreads.emplace_back([&, w]() {
          auto& gAcc = threadGrads[w];
          auto& st = threadStats[w];
          std::vector<float> rawLogits;
          std::vector<float> dLogits;

          while (true) {
            size_t idx = nextEp.fetch_add(1);
            if (idx >= allEpisodes.size()) break;
            const SideEpisode& ep = allEpisodes[idx];

            for (const auto& tr : ep.turns) {
              // Head 4: ValueNet MSE gradient
              float vPred = learner.evaluateValue(tr.startCtx);
              float vDiff = vPred - tr.returnTarget;
              st.valueLossSum += 0.5f * vDiff * vDiff;
              float dValRaw = 0.5f * vDiff * (1.0f - vPred * vPred);
              learner.valueNet.backward(tr.startCtx, {dValRaw}, gAcc.valueNet);

              // Head 2: MissionNet listwise categorical + StageNear binary PPO gradients
              for (const auto& ms : tr.missionSteps) {
                float normAdv = mStats.normalize(ms.causalAdvantage);
                const size_t K = ms.candidates.size();
                if (K > 1) {
                  rawLogits.resize(K);
                  for (size_t k = 0; k < K; k++) {
                    rawLogits[k] = learner.missionNet.forward(ms.candidates[k])[0];
                  }
                  accumulateCategoricalPPO(rawLogits, &ms.logMult, ms.chosenIdx, ms.oldLogProb, normAdv,
                                           invTemp, CLIP_EPS, entCoef, dLogits, st);
                  for (size_t k = 0; k < K; k++) {
                    if (dLogits[k] != 0.0f) {
                      learner.missionNet.backward(ms.candidates[k], {dLogits[k], 0.0f}, gAcc.missionNet);
                    }
                  }
                }
                if (ms.hasStageChoice && ms.chosenIdx < (int)ms.candidates.size()) {
                  float sRaw = learner.missionNet.forward(ms.candidates[ms.chosenIdx])[1];
                  float z = std::clamp(sRaw * invTemp, -15.0f, 15.0f);
                  float pStage = 1.0f / (1.0f + std::exp(-z));
                  float newLP = std::log(std::max(1e-12f, ms.stageChosen ? pStage : (1.0f - pStage)));
                  float ratio = std::exp(std::clamp(newLP - ms.oldStageLogProb, -10.0f, 10.0f));
                  bool clipped = (normAdv > 0.0f && ratio > 1.0f + CLIP_EPS) ||
                                 (normAdv < 0.0f && ratio < 1.0f - CLIP_EPS);
                  float dPol = clipped ? 0.0f : (-ratio * normAdv);
                  float y = ms.stageChosen ? 1.0f : 0.0f;
                  float ent = -(pStage * std::log(std::max(1e-12f, pStage)) +
                                (1.0f - pStage) * std::log(std::max(1e-12f, 1.0f - pStage)));
                  float dNegEnt = entCoef * pStage * (std::log(std::max(1e-12f, pStage)) + ent) * invTemp +
                                  entCoef * (1.0f - pStage) * (std::log(std::max(1e-12f, 1.0f - pStage)) + ent) * (-invTemp);
                  float dStage = dPol * (y - pStage) * invTemp + dNegEnt;
                  learner.missionNet.backward(ms.candidates[ms.chosenIdx], {0.0f, dStage}, gAcc.missionNet);
                }
              }

              // Head 3: EconNet listwise categorical PPO gradients
              if (!tr.econSteps.empty()) {
                float normAdv = eStats.normalize(tr.advantage);
                for (const auto& es : tr.econSteps) {
                  const size_t K = es.candidates.size();
                  if (K <= 1) continue;
                  rawLogits.resize(K);
                  for (size_t k = 0; k < K; k++) {
                    rawLogits[k] = learner.econNet.forward(es.candidates[k])[0];
                  }
                  accumulateCategoricalPPO(rawLogits, &es.logMult, es.chosenIdx, es.oldLogProb, normAdv,
                                           invTemp, CLIP_EPS, entCoef, dLogits, st);
                  for (size_t k = 0; k < K; k++) {
                    if (dLogits[k] != 0.0f) {
                      learner.econNet.backward(es.candidates[k], {dLogits[k]}, gAcc.econNet);
                    }
                  }
                }
              }

              // Head 1: DiploNet 4-stance categorical PPO gradients
              if (!tr.diploSteps.empty()) {
                float normAdv = dStats.normalize(tr.advantage);
                for (const auto& ds : tr.diploSteps) {
                  auto dRaw = learner.diploNet.forward(ds.input);
                  rawLogits = {dRaw[0], dRaw[1], dRaw[2], dRaw[3]};
                  accumulateCategoricalPPO(rawLogits, nullptr, ds.chosenStance, ds.oldStanceLogProb,
                                           normAdv, invTemp, CLIP_EPS, entCoef, dLogits, st);
                  learner.diploNet.backward(
                      ds.input, {dLogits[0], dLogits[1], dLogits[2], dLogits[3], 0.0f, 0.0f},
                      gAcc.diploNet);
                }
              }
            }
          }
        });
      }
      for (auto& th : gradThreads) th.join();

      std::vector<float> totalGrad(D, 0.0f);
      PPOGradResult epochSt;
      for (unsigned int w = 0; w < gWorkers; w++) {
        std::vector<float> fg = threadGrads[w].flatten();
        for (size_t d = 0; d < D; d++) totalGrad[d] += fg[d];
        epochSt.policyLossSum += threadStats[w].policyLossSum;
        epochSt.valueLossSum += threadStats[w].valueLossSum;
        epochSt.entropySum += threadStats[w].entropySum;
        epochSt.sampleCount += threadStats[w].sampleCount;
      }
      lastEpochStats = epochSt;

      float denom = (float)std::max(1, epochSt.sampleCount);
      double gradNormSq = 0.0;
      for (size_t d = 0; d < D; d++) {
        totalGrad[d] = totalGrad[d] / denom + 1e-4f * params[d];
        gradNormSq += (double)totalGrad[d] * (double)totalGrad[d];
      }
      float gradNorm = (float)std::sqrt(gradNormSq);
      float clipScale = (gradNorm > 1.0f) ? (1.0f / gradNorm) : 1.0f;

      adamStep++;
      float biasCorr1 = 1.0f - std::pow(0.9f, (float)adamStep);
      float biasCorr2 = 1.0f - std::pow(0.999f, (float)adamStep);
      for (size_t d = 0; d < D; d++) {
        float g = totalGrad[d] * clipScale;
        adamM[d] = 0.9f * adamM[d] + 0.1f * g;
        adamV[d] = 0.999f * adamV[d] + 0.001f * g * g;
        float mHat = adamM[d] / biasCorr1;
        float vHat = adamV[d] / biasCorr2;
        params[d] -= lr * mHat / (std::sqrt(vHat) + 1e-8f);
      }
      learner.unflatten(params);
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    float avgCitiesPerSide = (float)totalCities / (float)std::max(1, totalLearnerSides);
    float avgNeutLeft = (float)totalNeutralsLeft / (float)gamesPerIter;
    float avgEnt = lastEpochStats.entropySum / (float)std::max(1, lastEpochStats.sampleCount);
    float avgVLoss = lastEpochStats.valueLossSum / (float)std::max(1, lastEpochStats.sampleCount);

    if (iter == 1 || iter % 5 == 0 || iter == iterations) {
      std::printf("[Iter %3d/%d] SelfPlay AvgCities/Side: %.2f | NeutralsLeft: %.1f | Ent: %.3f | VLoss: %.4f | Decisions: %zu (%.2fs)\n",
                  iter, iterations, avgCitiesPerSide, avgNeutLeft, avgEnt, avgVLoss,
                  missionAdvs.size() + econAdvs.size() + diploAdvs.size(), sec);
      std::fflush(stdout);
    }

    if (iter % 8 == 0) {
      if (hallOfFame.size() < 8) {
        hallOfFame.push_back(learner);
      } else {
        hallOfFame[(iter / 8) % hallOfFame.size()] = learner;
      }
    }

    if (iter % evalEvery == 0 || iter == iterations) {
      EvalBenchmark eb = evaluateVsClassicSuite(dataDir, learner, maxTurns);
      float ratio = (float)eb.learnerCitiesEnd /
                    (float)std::max(1, eb.learnerCitiesEnd + eb.classicCitiesEnd);
      float elo = 1500.0f + 400.0f * std::log10(std::clamp(ratio, 0.05f, 0.95f) /
                                                (1.0f - std::clamp(ratio, 0.05f, 0.95f)));
      std::printf("  --> [Eval @ Iter %d] Cities@T10: %d vs %d | Cities@End: %d vs %d (%.1f%%) | Wins: %d/%d | EstElo: %.0f\n",
                  iter, eb.learnerCitiesT10, eb.classicCitiesT10,
                  eb.learnerCitiesEnd, eb.classicCitiesEnd, ratio * 100.0f,
                  eb.learnerWins, eb.totalGames, elo);
      std::fflush(stdout);
      if (ratio >= bestEvalRatio) {
        bestEvalRatio = ratio;
        bestCheckpoint = learner;
        bestCheckpoint.saveJson(outPath, iter, elo);
      }
    }
  }

  float finalElo = 1500.0f + 400.0f * std::log10(std::clamp(bestEvalRatio, 0.05f, 0.95f) /
                                                 (1.0f - std::clamp(bestEvalRatio, 0.05f, 0.95f)));
  bestCheckpoint.saveJson(outPath, iterations, finalElo);
  std::printf("Saved best tabula-rasa learned policy to %s (CityShare vs Classic: %.1f%%, EstElo: %.0f)\n",
              outPath.c_str(), bestEvalRatio * 100.0f, finalElo);
  return 0;
}
