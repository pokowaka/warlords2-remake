// Self-play reinforcement-learning trainer for the Modern AI (`w2arena`).
//
// Trains the 4-head PolicyNetwork with PPO + GAE in all-computer games.
//
// Objective. A side's reward each turn is the change in its empire score S
// (computeEmpireScore: 2 x city share + 0.5 x income share + 0.25 x army share,
// plus a small reconnaissance term under Hidden Map), measured from the start
// of its turn to the start of its next one, so losses suffered during rivals'
// turns count. Winning adds +1 and elimination -0.5 (and S drops to 0).
// Training games are cut off after --turns turns; that cut-off is a time limit,
// not an end of the game, so the critic's estimate is bootstrapped there.
// The objective is therefore the discounted growth of S with no fixed horizon,
// which matches open-ended real games. S is a hand-chosen definition of doing
// well; everything else (targets, coalitions, production, diplomacy) is learned.
//
// Opponents. Each game is either a mirror match (every side plays the current
// learner and is recorded), a league match (half the sides play a snapshot
// from the hall of fame), or -- only with --classic-frac > 0 -- a match against
// the 1993 Classic AI. By default the Classic AI is used solely for evaluation.
//
// Evaluation. Two disjoint seed sets, both disjoint from training seeds:
//   selection: decides which checkpoint is kept (--select-seeds);
//   report:    measured once at the end, never used for any decision
//              (--report-seeds).
// Each seed is played twice with the Modern/Classic side assignment swapped,
// half the sides each. The figure of merit is the Modern AI's share of the
// cities held by the two camps when the game is cut off, with a 95% interval.
//
// Usage (from the repository root):
//   cpp/build/w2arena [--iterations N] [--games-per-iter G] [--turns T]
//                     [--eval-every E] [--eval-turns T] [--select-seeds S]
//                     [--report-seeds R] [--classic-frac F] [--from-scratch]
//                     [--out PATH] [--log CSV] [--benchmark-only]

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
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"

using namespace w2;
namespace ml = w2::ai::ml;

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

const std::vector<std::string> SCENARIOS = {"ERYTHEA", "ISLADIA", "DRAGON", "TUTORIA", "SORCERY", "HADESHA"};

// Seed ranges: training uses iter * 1009 + game * 97 + 17 (< 700000 for any
// sane run); evaluation sets live well above it and never overlap.
constexpr uint32_t SELECT_SEED_BASE = 700000;
constexpr uint32_t REPORT_SEED_BASE = 800000;

struct Config {
  std::string dataDir = "original";
  std::string outPath = "cpp/ai_weights.json";
  std::string logPath;
  int iterations = 300;
  int gamesPerIter = 48;
  int maxTurns = 40;
  int evalEvery = 10;
  int evalTurns = 40;
  int selectSeeds = 12;
  int reportSeeds = 32;
  int epochs = 4;
  int minibatches = 8;
  float lrStart = 3.0e-3f;
  float lrEnd = 3.0e-4f;
  float tempStart = 1.0f;
  float tempEnd = ml::DEPLOY_TEMPERATURE;   // end where real games are played
  float clipEps = 0.2f;
  float entCoef = 0.01f;
  float valueCoef = 0.5f;
  float gamma = 0.97f;
  float gaeLambda = 0.95f;
  float localWeight = 1.0f;
  float targetKL = 0.03f;
  float maxGradNorm = 0.5f;
  float classicFrac = 0.0f;
  float winReward = 1.0f;
  float eliminationReward = -0.5f;
  bool fromScratch = false;
  bool benchmarkOnly = false;
};

game::NewGameOptions gameOptions(const TrainingPreset& preset, uint32_t seed) {
  game::NewGameOptions opts;
  opts.seed = (double)seed;
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
    opts.sides[i].card = (int)((seed + i * 5) % 9);
  }
  return opts;
}

// ---------------------------------------------------------------- rollouts

struct SideEpisode {
  int sideIndex = 0;
  std::vector<ml::TurnStepRecord> turns;
  float finalScore = 0.0f;       // S when the game ended or was cut off
  bool terminal = false;         // eliminated or game over: no bootstrap
  float terminalReward = 0.0f;   // win / elimination bonus
  float bootstrapValue = 0.0f;   // V(s) at the cut-off when not terminal
  int finalCities = 0;
};

struct RolloutSummary {
  std::vector<SideEpisode> episodes;
  int recordedCities = 0;
  int recordedSides = 0;
  int neutralsLeft = 0;
};

enum class Opponent { Mirror, League, Classic };

RolloutSummary runRollout(const Config& cfg, const std::string& scenario, const TrainingPreset& preset,
                          uint32_t seed, float temperature, const ml::PolicyNetwork& learner,
                          const ml::PolicyNetwork& snapshot, Opponent mode) {
  auto gp = game::newGame(cfg.dataDir, scenario, gameOptions(preset, seed));
  Game& g = *gp;

  std::array<bool, 8> recorded{};
  std::array<bool, 8> classic{};
  std::array<SideEpisode, 8> eps{};
  std::array<ml::RolloutSampler, 8> samplers{};
  int idx = 0;
  for (int s = 0; s < 8; s++) {
    Side& side = g.map->sides[s];
    if (!side.inUse) continue;
    bool learnerSide = (mode == Opponent::Mirror) || ((idx + seed) % 2 == 0);
    idx++;
    recorded[s] = learnerSide;
    classic[s] = (mode == Opponent::Classic) && !learnerSide;
    side.classicAi = classic[s];
    eps[s].sideIndex = s;
    samplers[s].rngState = (seed * 2654435761u) ^ (uint32_t)((s + 1) * 2246822519u);
    samplers[s].temperature = temperature;
  }

  Side* cur = game::begin(g);
  int safety = 0;
  while (cur && !g.over && g.turn <= cfg.maxTurns && safety < cfg.maxTurns * 12) {
    safety++;
    int s = cur->index;
    if (cur->computer && cur->alive) {
      if (recorded[s]) {
        eps[s].turns.emplace_back();
        samplers[s].currentTurn = &eps[s].turns.back();
        ai::modern::playTurn(g, *cur, nullptr, &learner, &samplers[s]);
        samplers[s].currentTurn = nullptr;
      } else if (classic[s]) {
        ai::playTurn(g, *cur, nullptr);
      } else {
        ml::RolloutSampler opp;
        opp.rngState = samplers[s].rngState ^ (uint32_t)(g.turn * 374761393u);
        opp.temperature = temperature;
        ai::modern::playTurn(g, *cur, nullptr, &snapshot, &opp);
      }
    }
    cur = game::endTurn(g);
  }

  RolloutSummary out;
  std::array<int, 8> cities{};
  for (const auto& c : g.map->cities) {
    if (c.razed) continue;
    if (c.ownerIndex == NONE) out.neutralsLeft++;
    else if (c.ownerIndex >= 0 && c.ownerIndex < 8) cities[c.ownerIndex]++;
  }
  for (int s = 0; s < 8; s++) {
    if (!recorded[s] || eps[s].turns.empty()) continue;
    Side& side = g.map->sides[s];
    SideEpisode& ep = eps[s];
    ep.finalCities = cities[s];
    ep.finalScore = ml::computeEmpireScore(g, s);
    if (!side.alive) {
      ep.terminal = true;
      ep.terminalReward = cfg.eliminationReward;
    } else if (g.over) {
      ep.terminal = true;
      ep.terminalReward = (g.won == s) ? cfg.winReward : 0.0f;
    } else {
      ai::belief::update(g, side);
      ep.bootstrapValue = learner.evaluateValue(ml::buildContext(g, side));
    }
    out.recordedCities += cities[s];
    out.recordedSides++;
    out.episodes.push_back(std::move(ep));
  }
  return out;
}

// GAE over each side's own turns. A mission decision gets the turn's advantage
// plus `localWeight` times its own score change while it acted (a capture, a
// lost battle), centred on the turn's mean when `localWeight` <= 1 and taken
// as-is above that. The per-turn reward in an 8-side game is dominated by the
// rivals' turns; what a stack achieved itself is the cleanest signal there is.
void computeAdvantages(std::vector<SideEpisode>& episodes, float gamma, float lambda, float localWeight) {
  for (auto& ep : episodes) {
    const int T = (int)ep.turns.size();
    float gae = 0.0f;
    for (int t = T - 1; t >= 0; t--) {
      auto& cur = ep.turns[t];
      bool last = (t == T - 1);
      float nextScore = last ? ep.finalScore : ep.turns[t + 1].startScore;
      float nextValue = last ? (ep.terminal ? 0.0f : ep.bootstrapValue) : ep.turns[t + 1].valuePred;
      float reward = (nextScore - cur.startScore) + (last ? ep.terminalReward : 0.0f);
      float delta = reward + gamma * nextValue - cur.valuePred;
      gae = delta + gamma * lambda * gae;
      cur.advantage = gae;
      cur.returnTarget = gae + cur.valuePred;

      float meanLocal = 0.0f;
      for (const auto& ms : cur.missionSteps) meanLocal += ms.immediateDeltaScore;
      if (!cur.missionSteps.empty()) meanLocal /= (float)cur.missionSteps.size();
      if (localWeight > 1.0f) meanLocal = 0.0f;
      for (auto& ms : cur.missionSteps) {
        ms.advantage = cur.advantage + localWeight * (ms.immediateDeltaScore - meanLocal);
      }
    }
  }
}

struct Normalizer {
  float mean = 0.0f, invStd = 1.0f;
  float operator()(float x) const { return std::clamp((x - mean) * invStd, -5.0f, 5.0f); }
  static Normalizer fit(const std::vector<float>& v) {
    Normalizer n;
    if (v.size() < 2) return n;
    double s = 0.0, sq = 0.0;
    for (float x : v) s += x;
    n.mean = (float)(s / v.size());
    for (float x : v) sq += ((double)x - n.mean) * ((double)x - n.mean);
    n.invStd = 1.0f / std::max(1e-4f, (float)std::sqrt(sq / v.size()));
    return n;
  }
};

// ---------------------------------------------------------------- PPO maths

struct HeadStats {
  int mission = 0, econ = 0, diplo = 0, turns = 0;
  double policyLoss = 0.0, valueLoss = 0.0, entropy = 0.0, kl = 0.0;
  int klSamples = 0, entSamples = 0;
  void add(const HeadStats& o) {
    mission += o.mission; econ += o.econ; diplo += o.diplo; turns += o.turns;
    policyLoss += o.policyLoss; valueLoss += o.valueLoss; entropy += o.entropy; kl += o.kl;
    klSamples += o.klSamples; entSamples += o.entSamples;
  }
};

// Clipped-surrogate gradient for a categorical choice over options with policy
// logits z_i = (u_i - logMult_i) / T, plus a KL pull toward the multiplicity
// prior q_i ~ exp(-logMult_i) (entropy regularisation over action families).
// Writes dLoss/du_i into dU.
void categoricalPPO(const std::vector<float>& u, const std::vector<float>* logMult, int chosen, float oldLogProb,
                    float adv, float invT, const Config& cfg, std::vector<float>& dU, HeadStats& st) {
  const size_t K = u.size();
  dU.assign(K, 0.0f);
  if (K <= 1) return;
  auto lmOf = [&](size_t i) { return (logMult && i < logMult->size()) ? (*logMult)[i] : 0.0f; };
  float maxZ = -1e30f;
  std::vector<float> z(K);
  for (size_t i = 0; i < K; i++) {
    z[i] = (u[i] - lmOf(i)) * invT;
    maxZ = std::max(maxZ, z[i]);
  }
  std::vector<float> p(K);
  float sum = 0.0f;
  for (size_t i = 0; i < K; i++) {
    p[i] = std::exp(std::max(-30.0f, z[i] - maxZ));
    sum += p[i];
  }
  for (float& v : p) v /= sum;

  float newLP = std::log(std::max(1e-12f, p[chosen]));
  float logRatio = std::clamp(newLP - oldLogProb, -10.0f, 10.0f);
  float ratio = std::exp(logRatio);
  st.policyLoss += -std::min(ratio * adv, std::clamp(ratio, 1.0f - cfg.clipEps, 1.0f + cfg.clipEps) * adv);
  st.kl += (ratio - 1.0f) - logRatio;
  st.klSamples++;
  bool clipped = (adv > 0.0f && ratio > 1.0f + cfg.clipEps) || (adv < 0.0f && ratio < 1.0f - cfg.clipEps);
  float dLossDLogP = clipped ? 0.0f : -ratio * adv;

  float meanAdj = 0.0f, ent = 0.0f;
  for (size_t j = 0; j < K; j++) {
    float lp = std::log(std::max(1e-12f, p[j]));
    meanAdj += p[j] * (lp + lmOf(j));
    ent -= p[j] * lp;
  }
  float normLogK = std::max(1.0f, std::log((float)K));
  st.entropy += ent / normLogK;
  st.entSamples++;
  float klCoef = cfg.entCoef / normLogK;
  for (size_t j = 0; j < K; j++) {
    float ind = ((int)j == chosen) ? 1.0f : 0.0f;
    float lp = std::log(std::max(1e-12f, p[j]));
    dU[j] = dLossDLogP * (ind - p[j]) * invT + klCoef * p[j] * ((lp + lmOf(j)) - meanAdj) * invT;
  }
}

// Clipped-surrogate gradient for a yes/no choice with logit `logit`
// (P(yes) = sigmoid(logit / T)), plus an entropy bonus. Returns dLoss/dlogit.
float binaryPPO(float logit, bool chosen, float oldLogProb, float adv, float invT, const Config& cfg, HeadStats& st) {
  float z = std::clamp(logit * invT, -15.0f, 15.0f);
  float p = 1.0f / (1.0f + std::exp(-z));
  float newLP = std::log(std::max(1e-12f, chosen ? p : 1.0f - p));
  float logRatio = std::clamp(newLP - oldLogProb, -10.0f, 10.0f);
  float ratio = std::exp(logRatio);
  st.policyLoss += -std::min(ratio * adv, std::clamp(ratio, 1.0f - cfg.clipEps, 1.0f + cfg.clipEps) * adv);
  st.kl += (ratio - 1.0f) - logRatio;
  st.klSamples++;
  bool clipped = (adv > 0.0f && ratio > 1.0f + cfg.clipEps) || (adv < 0.0f && ratio < 1.0f - cfg.clipEps);
  float dLossDLogP = clipped ? 0.0f : -ratio * adv;
  float y = chosen ? 1.0f : 0.0f;
  // d(-H)/dz = p (1 - p) z for the Bernoulli entropy H
  return dLossDLogP * (y - p) * invT + cfg.entCoef * p * (1.0f - p) * z * invT;
}

struct Normalizers {
  Normalizer mission, econ, diplo;
};

// Accumulate the gradients of one turn's decisions and its critic target.
void accumulateTurn(const ml::PolicyNetwork& net, const ml::TurnStepRecord& tr, const Normalizers& nz, float invT,
                    const Config& cfg, ml::PolicyNetwork& grad, HeadStats& st, std::vector<float>& u,
                    std::vector<float>& dU) {
  // Critic: 0.5 (V - G)^2 on a linear head
  float v = net.evaluateValue(tr.startCtx);
  float dv = v - tr.returnTarget;
  st.valueLoss += 0.5 * dv * dv;
  st.turns++;
  net.valueNet.backward(tr.startCtx, {dv}, grad.valueNet);

  for (const auto& ms : tr.missionSteps) {
    float adv = nz.mission(ms.advantage);
    const size_t K = ms.candidates.size();
    if (K > 1) {
      u.resize(K);
      for (size_t k = 0; k < K; k++) u[k] = net.missionNet.forward(ms.candidates[k])[0];
      categoricalPPO(u, &ms.logMult, ms.chosenIdx, ms.oldLogProb, adv, invT, cfg, dU, st);
      for (size_t k = 0; k < K; k++) {
        if (dU[k] != 0.0f) net.missionNet.backward(ms.candidates[k], {dU[k], 0.0f}, grad.missionNet);
      }
      st.mission++;
    }
    if (ms.hasStageChoice && ms.chosenIdx < (int)K) {
      float s = net.missionNet.forward(ms.candidates[ms.chosenIdx])[1];
      float d = binaryPPO(s, ms.stageChosen, ms.oldStageLogProb, adv, invT, cfg, st);
      net.missionNet.backward(ms.candidates[ms.chosenIdx], {0.0f, d}, grad.missionNet);
      st.mission++;
    }
  }

  float econAdv = nz.econ(tr.advantage);
  for (const auto& es : tr.econSteps) {
    const size_t K = es.candidates.size();
    if (K <= 1) continue;
    u.resize(K);
    for (size_t k = 0; k < K; k++) u[k] = net.econNet.forward(es.candidates[k])[0];
    categoricalPPO(u, &es.logMult, es.chosenIdx, es.oldLogProb, econAdv, invT, cfg, dU, st);
    for (size_t k = 0; k < K; k++) {
      if (dU[k] != 0.0f) net.econNet.backward(es.candidates[k], {dU[k]}, grad.econNet);
    }
    st.econ++;
  }

  float diploAdv = nz.diplo(tr.advantage);
  for (const auto& ds : tr.diploSteps) {
    auto raw = net.diploNet.forward(ds.input);
    std::array<float, ml::DIPLO_OUT_DIM> dOut{};
    u = {raw[0], raw[1], raw[2], raw[3]};
    categoricalPPO(u, nullptr, ds.chosenStance, ds.oldStanceLogProb, diploAdv, invT, cfg, dU, st);
    for (int k = 0; k < 4; k++) dOut[k] = dU[k];
    st.diplo++;
    if (ds.hasTribute) {
      dOut[4] = binaryPPO(raw[4], ds.tributeChosen, ds.oldTributeLogProb, diploAdv, invT, cfg, st);
      st.diplo++;
    }
    if (ds.hasJointWar) {
      dOut[5] = binaryPPO(raw[5], ds.jointWarChosen, ds.oldJointWarLogProb, diploAdv, invT, cfg, st);
      st.diplo++;
    }
    net.diploNet.backward(ds.input, dOut, grad.diploNet);
  }
}

// ---------------------------------------------------------------- evaluation

struct EvalResult {
  int games = 0;
  double meanShare = 0.0;     // Modern AI's share of the cities held by both camps
  double ci95 = 0.0;          // half-width of the 95% interval on meanShare
  int modernTop = 0;          // games whose largest side is a Modern AI side
  int modernCities = 0, classicCities = 0;
};

EvalResult evaluateVsClassic(const Config& cfg, const ml::PolicyNetwork& policy, uint32_t seedBase, int seeds,
                             int turns) {
  struct Case {
    std::string scenario;
    TrainingPreset preset;
    uint32_t seed;
    int swap;
  };
  std::vector<Case> cases;
  for (int i = 0; i < seeds; i++) {
    for (int swap = 0; swap < 2; swap++) {
      cases.push_back({SCENARIOS[(i + i / 4) % SCENARIOS.size()], PRESETS[i % 4], seedBase + (uint32_t)i, swap});
    }
  }
  std::vector<double> shares(cases.size(), 0.5);
  std::vector<int> top(cases.size(), 0), modC(cases.size(), 0), clsC(cases.size(), 0);
  std::atomic<size_t> next{0};
  unsigned int workers = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned int w = 0; w < workers; w++) {
    pool.emplace_back([&]() {
      for (size_t ci; (ci = next.fetch_add(1)) < cases.size();) {
        const Case& c = cases[ci];
        auto gp = game::newGame(cfg.dataDir, c.scenario, gameOptions(c.preset, c.seed));
        Game& g = *gp;
        std::array<bool, 8> modern{};
        int idx = 0;
        for (int s = 0; s < 8; s++) {
          if (!g.map->sides[s].inUse) continue;
          modern[s] = ((idx + c.swap) % 2 == 0);
          g.map->sides[s].classicAi = !modern[s];
          idx++;
        }
        Side* cur = game::begin(g);
        int safety = 0;
        while (cur && !g.over && g.turn <= turns && safety < turns * 12) {
          safety++;
          if (cur->computer && cur->alive) {
            if (modern[cur->index]) ai::modern::playTurn(g, *cur, nullptr, &policy, nullptr);
            else ai::playTurn(g, *cur, nullptr);
          }
          cur = game::endTurn(g);
        }
        std::array<int, 8> cnt{};
        int m = 0, k = 0, best = NONE, bestCnt = -1;
        for (const auto& city : g.map->cities) {
          if (city.razed || city.ownerIndex < 0 || city.ownerIndex >= 8) continue;
          cnt[city.ownerIndex]++;
          (modern[city.ownerIndex] ? m : k)++;
        }
        for (int s = 0; s < 8; s++) {
          if (cnt[s] > bestCnt) { bestCnt = cnt[s]; best = s; }
        }
        shares[ci] = (m + k) > 0 ? (double)m / (double)(m + k) : 0.5;
        top[ci] = (best != NONE && modern[best]) ? 1 : 0;
        modC[ci] = m;
        clsC[ci] = k;
      }
    });
  }
  for (auto& t : pool) t.join();

  EvalResult r;
  r.games = (int)cases.size();
  for (size_t i = 0; i < cases.size(); i++) {
    r.meanShare += shares[i];
    r.modernTop += top[i];
    r.modernCities += modC[i];
    r.classicCities += clsC[i];
  }
  r.meanShare /= std::max(1, r.games);
  double var = 0.0;
  for (double s : shares) var += (s - r.meanShare) * (s - r.meanShare);
  var /= std::max(1, r.games - 1);
  r.ci95 = 1.96 * std::sqrt(var / std::max(1, r.games));
  return r;
}

void printEval(const char* label, const EvalResult& r) {
  std::printf("  [%s] %d games: Modern city share %.1f%% +/- %.1f%% (95%%) | cities %d vs %d | Modern largest side in %d/%d\n",
              label, r.games, r.meanShare * 100.0, r.ci95 * 100.0, r.modernCities, r.classicCities, r.modernTop,
              r.games);
  std::fflush(stdout);
}

// ---------------------------------------------------------------- main

bool parseArgs(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--iterations") cfg.iterations = std::max(0, std::stoi(next()));
    else if (a == "--games-per-iter") cfg.gamesPerIter = std::max(2, std::stoi(next()));
    else if (a == "--turns") cfg.maxTurns = std::max(6, std::stoi(next()));
    else if (a == "--eval-every") cfg.evalEvery = std::max(1, std::stoi(next()));
    else if (a == "--eval-turns") cfg.evalTurns = std::max(6, std::stoi(next()));
    else if (a == "--select-seeds") cfg.selectSeeds = std::max(1, std::stoi(next()));
    else if (a == "--report-seeds") cfg.reportSeeds = std::max(1, std::stoi(next()));
    else if (a == "--epochs") cfg.epochs = std::max(1, std::stoi(next()));
    else if (a == "--minibatches") cfg.minibatches = std::max(1, std::stoi(next()));
    else if (a == "--lr") cfg.lrStart = std::stof(next());
    else if (a == "--classic-frac") cfg.classicFrac = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--lambda") cfg.gaeLambda = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--local-weight") cfg.localWeight = std::max(0.0f, std::stof(next()));
    else if (a == "--out") cfg.outPath = next();
    else if (a == "--log") cfg.logPath = next();
    else if (a == "--data") cfg.dataDir = next();
    else if (a == "--from-scratch") cfg.fromScratch = true;
    else if (a == "--benchmark-only") cfg.benchmarkOnly = true;
    else {
      std::fprintf(stderr, "Unknown argument '%s'; see the usage at the top of cpp/test/arena.cpp.\n", a.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parseArgs(argc, argv, cfg)) return 2;
  if (!fileExists(cfg.dataDir + "/TERRAIN0/ARMYTYPE.DAT")) {
    std::fprintf(stderr, "Cannot find the game data in '%s': run from the repository root or pass --data DIR.\n",
                 cfg.dataDir.c_str());
    return 1;
  }
  unsigned int threads = std::max(2u, std::thread::hardware_concurrency());

  ml::PolicyNetwork learner;
  bool loaded = false;
  if (!cfg.fromScratch && std::filesystem::exists(cfg.outPath)) loaded = learner.loadJson(cfg.outPath);
  if (!loaded) learner.initRandomWeights(2026);
  std::printf("=== w2arena: PPO self-play | params %d | threads %u | %s ===\n", ml::PolicyNetwork::TOTAL_PARAMS,
              threads, loaded ? ("loaded " + cfg.outPath).c_str() : "random init");

  if (cfg.benchmarkOnly || cfg.iterations == 0) {
    printEval("report", evaluateVsClassic(cfg, learner, REPORT_SEED_BASE, cfg.reportSeeds, cfg.evalTurns));
    return 0;
  }
  std::printf("iters %d | games/iter %d | turns %d | epochs %d x %d minibatches | classic-frac %.2f\n",
              cfg.iterations, cfg.gamesPerIter, cfg.maxTurns, cfg.epochs, cfg.minibatches, cfg.classicFrac);

  FILE* logf = cfg.logPath.empty() ? nullptr : std::fopen(cfg.logPath.c_str(), "w");
  if (logf) {
    std::fprintf(logf, "iter,seconds,temperature,lr,avg_cities,neutrals_left,entropy,value_loss,kl,epochs_run,"
                       "decisions,select_share,select_ci\n");
  }

  std::vector<ml::PolicyNetwork> hallOfFame = {learner};
  ml::PolicyNetwork best = learner;
  int bestIter = 0;
  float bestShare = -1.0f;

  const size_t D = ml::PolicyNetwork::TOTAL_PARAMS;
  const size_t offDiplo = 0;
  const size_t offMission = offDiplo + ml::PolicyNetwork::DiploNet::PARAM_COUNT;
  const size_t offEcon = offMission + ml::PolicyNetwork::MissionNet::PARAM_COUNT;
  const size_t offValue = offEcon + ml::PolicyNetwork::EconNet::PARAM_COUNT;
  std::vector<float> params = learner.flatten(), adamM(D, 0.0f), adamV(D, 0.0f);
  int adamStep = 0;
  std::mt19937 shuffleRng(12345);

  for (int iter = 1; iter <= cfg.iterations; iter++) {
    auto t0 = std::chrono::steady_clock::now();
    float progress = (float)(iter - 1) / (float)std::max(1, cfg.iterations - 1);
    float temperature = cfg.tempStart + (cfg.tempEnd - cfg.tempStart) * progress;
    float lr = cfg.lrStart + (cfg.lrEnd - cfg.lrStart) * progress;
    float invT = 1.0f / temperature;

    // ---- rollouts
    std::vector<RolloutSummary> games(cfg.gamesPerIter);
    std::atomic<int> nextGame{0};
    std::vector<std::thread> pool;
    for (unsigned int w = 0; w < std::min<unsigned int>(threads, cfg.gamesPerIter); w++) {
      pool.emplace_back([&]() {
        for (int gi; (gi = nextGame.fetch_add(1)) < cfg.gamesPerIter;) {
          uint32_t seed = (uint32_t)(iter * 1009 + gi * 97 + 17);
          const std::string& scen = SCENARIOS[(iter * 7 + gi) % SCENARIOS.size()];
          const TrainingPreset& preset = PRESETS[(iter + gi) % 4];
          float roll = (float)((seed * 2654435761u) >> 8) / (float)(1u << 24);
          Opponent mode = roll < cfg.classicFrac ? Opponent::Classic
                          : (gi % 2 == 0)        ? Opponent::Mirror
                                                 : Opponent::League;
          const auto& snap = hallOfFame[(iter + gi) % hallOfFame.size()];
          games[gi] = runRollout(cfg, scen, preset, seed, temperature, learner, snap, mode);
        }
      });
    }
    for (auto& t : pool) t.join();

    std::vector<SideEpisode> episodes;
    int cities = 0, sides = 0, neutrals = 0;
    for (auto& gs : games) {
      cities += gs.recordedCities;
      sides += gs.recordedSides;
      neutrals += gs.neutralsLeft;
      for (auto& ep : gs.episodes) episodes.push_back(std::move(ep));
    }
    computeAdvantages(episodes, cfg.gamma, cfg.gaeLambda, cfg.localWeight);

    std::vector<const ml::TurnStepRecord*> turns;
    std::vector<float> mAdv, eAdv, dAdv;
    for (const auto& ep : episodes) {
      for (const auto& tr : ep.turns) {
        turns.push_back(&tr);
        for (const auto& ms : tr.missionSteps) mAdv.push_back(ms.advantage);
        if (!tr.econSteps.empty()) eAdv.push_back(tr.advantage);
        if (!tr.diploSteps.empty()) dAdv.push_back(tr.advantage);
      }
    }
    Normalizers nz{Normalizer::fit(mAdv), Normalizer::fit(eAdv), Normalizer::fit(dAdv)};

    // ---- PPO: epochs x minibatches of turns, per-head mean losses
    HeadStats lastEpoch;
    int epochsRun = 0;
    for (int epoch = 0; epoch < cfg.epochs; epoch++) {
      std::shuffle(turns.begin(), turns.end(), shuffleRng);
      HeadStats epochStats;
      size_t mbSize = (turns.size() + cfg.minibatches - 1) / cfg.minibatches;
      for (size_t mb0 = 0; mb0 < turns.size(); mb0 += mbSize) {
        size_t mb1 = std::min(turns.size(), mb0 + mbSize);
        std::atomic<size_t> nextTurn{mb0};
        unsigned int gw = std::min<unsigned int>(threads, (unsigned int)std::max<size_t>(1, mb1 - mb0));
        std::vector<ml::PolicyNetwork> grads(gw);
        std::vector<HeadStats> stats(gw);
        std::vector<std::thread> gpool;
        for (unsigned int w = 0; w < gw; w++) {
          grads[w].zero();
          gpool.emplace_back([&, w]() {
            std::vector<float> u, dU;
            for (size_t ti; (ti = nextTurn.fetch_add(1)) < mb1;) {
              accumulateTurn(learner, *turns[ti], nz, invT, cfg, grads[w], stats[w], u, dU);
            }
          });
        }
        for (auto& t : gpool) t.join();

        HeadStats mbStats;
        std::vector<float> g(D, 0.0f);
        for (unsigned int w = 0; w < gw; w++) {
          auto fg = grads[w].flatten();
          for (size_t d = 0; d < D; d++) g[d] += fg[d];
          mbStats.add(stats[w]);
        }
        // Each head's loss is a mean over its own decisions; the critic's over turns
        auto scale = [&](size_t a, size_t b, float s) { for (size_t d = a; d < b; d++) g[d] *= s; };
        scale(offDiplo, offMission, 1.0f / std::max(1, mbStats.diplo));
        scale(offMission, offEcon, 1.0f / std::max(1, mbStats.mission));
        scale(offEcon, offValue, 1.0f / std::max(1, mbStats.econ));
        scale(offValue, D, cfg.valueCoef / std::max(1, mbStats.turns));
        double norm = 0.0;
        for (float x : g) norm += (double)x * x;
        float clip = std::sqrt(norm) > cfg.maxGradNorm ? cfg.maxGradNorm / (float)std::sqrt(norm) : 1.0f;
        adamStep++;
        float bc1 = 1.0f - std::pow(0.9f, (float)adamStep), bc2 = 1.0f - std::pow(0.999f, (float)adamStep);
        for (size_t d = 0; d < D; d++) {
          float gd = g[d] * clip;
          adamM[d] = 0.9f * adamM[d] + 0.1f * gd;
          adamV[d] = 0.999f * adamV[d] + 0.001f * gd * gd;
          params[d] -= lr * (adamM[d] / bc1) / (std::sqrt(adamV[d] / bc2) + 1e-8f);
        }
        learner.unflatten(params);
        epochStats.add(mbStats);
      }
      lastEpoch = epochStats;
      epochsRun++;
      double kl = epochStats.kl / std::max(1, epochStats.klSamples);
      if (kl > cfg.targetKL) break;   // the policy has moved far enough from the one that played
    }

    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double ent = lastEpoch.entropy / std::max(1, lastEpoch.entSamples);
    double vloss = lastEpoch.valueLoss / std::max(1, lastEpoch.turns);
    double kl = lastEpoch.kl / std::max(1, lastEpoch.klSamples);
    float avgCities = (float)cities / (float)std::max(1, sides);
    float avgNeutrals = (float)neutrals / (float)cfg.gamesPerIter;
    size_t decisions = mAdv.size() + eAdv.size() + dAdv.size();
    std::printf("[iter %3d] %.1fs T=%.2f lr=%.1e | cities/side %.2f | neutrals %.1f | ent %.3f | vloss %.4f | "
                "kl %.4f (%d ep) | %zu turns\n",
                iter, sec, temperature, lr, avgCities, avgNeutrals, ent, vloss, kl, epochsRun, turns.size());
    std::fflush(stdout);

    if (iter % 10 == 0) {
      if (hallOfFame.size() < 12) hallOfFame.push_back(learner);
      else hallOfFame[(iter / 10) % hallOfFame.size()] = learner;
    }

    float selShare = NAN, selCi = NAN;
    if (iter % cfg.evalEvery == 0 || iter == cfg.iterations) {
      EvalResult sel = evaluateVsClassic(cfg, learner, SELECT_SEED_BASE, cfg.selectSeeds, cfg.evalTurns);
      printEval(("select @" + std::to_string(iter)).c_str(), sel);
      selShare = (float)sel.meanShare;
      selCi = (float)sel.ci95;
      if (selShare > bestShare) {
        bestShare = selShare;
        best = learner;
        bestIter = iter;
        best.saveJson(cfg.outPath, bestIter, bestShare);
      }
    }
    if (logf) {
      std::fprintf(logf, "%d,%.2f,%.3f,%.2e,%.3f,%.2f,%.4f,%.5f,%.5f,%d,%zu,%.4f,%.4f\n", iter, sec, temperature, lr,
                   avgCities, avgNeutrals, ent, vloss, kl, epochsRun, decisions, selShare, selCi);
      std::fflush(logf);
    }
  }
  if (logf) std::fclose(logf);

  best.saveJson(cfg.outPath, bestIter, bestShare);
  std::printf("Kept iteration %d (selection share %.1f%%) in %s. Held-out report:\n", bestIter, bestShare * 100.0f,
              cfg.outPath.c_str());
  printEval("report", evaluateVsClassic(cfg, best, REPORT_SEED_BASE, cfg.reportSeeds, cfg.evalTurns));
  return 0;
}
