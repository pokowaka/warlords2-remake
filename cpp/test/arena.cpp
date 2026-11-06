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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

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
// Development A/B comparisons (--compare / --dev-seeds) use their own set, so
// neither in-run selection nor the held-out report is spent on tuning.
constexpr uint32_t DEV_SEED_BASE = 750000;

enum class EvalMode { Mixed, Solo };

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
  float tempEnd = ml::TRAIN_TEMPERATURE_END;
  float evalTemp = ml::DEPLOY_TEMPERATURE;  // --eval-temp: temperature evaluation games sample at
  float clipEps = 0.2f;
  float entCoef = 0.01f;
  float valueCoef = 0.5f;
  float gamma = 0.97f;
  float gaeLambda = 0.95f;
  float localWeight = 1.0f;
  float targetKL = 0.03f;
  float maxGradNorm = 0.5f;
  float classicFrac = 0.0f;
  float randomMapFrac = 0.0f;
  int activeSides = 8;
  int cropMargin = 0;
  uint32_t seedOffset = 0;
  float winReward = 1.0f;
  float eliminationReward = -0.5f;
  bool soloLearner = false;
  bool fromScratch = false;
  bool benchmarkOnly = false;
  EvalMode evalMode = EvalMode::Mixed;
  std::string comparePath;   // --compare: B checkpoint, A is --out
  int devSeeds = 0;          // --dev-seeds: benchmark/compare on the development set
  bool breakdown = false;
  int criticWarmup = 0;      // --critic-warmup: iterations where only the critics learn
  float mixedFrac = 0.0f;    // --mixed-frac: solo-learner games played 4 vs 4 instead (all learner seats recorded)
  int saveEvery = 0;         // --save-every: also write <out>.iterN.json snapshots
  float raiderFrac = 0.0f;   // --raider-frac: share of Classic training seats played by the Raider
  opponents::Kind evalOpponent = opponents::Kind::Classic;  // --eval-opponent classic|raider
};

game::NewGameOptions gameOptions(const Config& cfg, const TrainingPreset& preset, uint32_t seed) {
  game::NewGameOptions opts;
  opts.seed = (double)seed;
  opts.activeSides = cfg.activeSides;
  opts.cropMargin = cfg.cropMargin;
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

std::unique_ptr<Game> makeArenaGame(const Config& cfg, const std::string& scenario, const TrainingPreset& preset,
                                    uint32_t seed, bool allowRandomMap) {
  auto opts = gameOptions(cfg, preset, seed);
  float mapRoll = (float)(((seed * 1597334677u) ^ 0x9e3779b9u) >> 8) / (float)(1u << 24);
  if ((allowRandomMap && mapRoll < cfg.randomMapFrac) || scenario == "RANDOM") {
    Rng mapRng((double)(seed ^ 0x5bf03635u));
    randommap::Options ropts;
    ropts.dataDir = cfg.dataDir;
    ropts.rng = &mapRng;
    ropts.sliders = {3, 3, 2, 3};
    ropts.allies = preset.diplomacy != 0;
    ropts.terrainSet = 0;
    auto files = randommap::generateNow(ropts);
    opts.memoryFiles = &files;
    return game::newGame(cfg.dataDir, "RANDOM", opts);
  }
  return game::newGame(cfg.dataDir, scenario, opts);
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
  auto gp = makeArenaGame(cfg, scenario, preset, seed, true);
  Game& g = *gp;

  std::array<bool, 8> recorded{};
  std::array<bool, 8> classic{};
  std::array<opponents::Kind, 8> classicKind{};
  std::array<SideEpisode, 8> eps{};
  std::array<ml::RolloutSampler, 8> samplers{};
  int activeCount = 0;
  for (int s = 0; s < 8; s++) {
    if (g.map->sides[s].inUse) activeCount++;
  }
  int soloSeat = activeCount > 0 ? (int)(seed % (uint32_t)activeCount) : 0;
  // --mixed-frac: some solo-learner games are played like the benchmark instead,
  // alternating learner and Classic seats, with every learner seat recorded.
  float mixRoll = (float)(((seed * 2246822519u) ^ 0x85ebca6bu) >> 8) / (float)(1u << 24);
  const bool solo = cfg.soloLearner && !(mixRoll < cfg.mixedFrac);
  const bool mixedVsClassic = cfg.soloLearner && !solo;
  int idx = 0;
  for (int s = 0; s < 8; s++) {
    Side& side = g.map->sides[s];
    if (!side.inUse) continue;
    bool learnerSide = solo             ? (idx == soloSeat)
                       : mixedVsClassic ? ((idx + seed) % 2 == 0)
                                        : ((mode == Opponent::Mirror) || ((idx + seed) % 2 == 0));
    idx++;
    recorded[s] = learnerSide;
    classic[s] = (solo || mixedVsClassic) ? !learnerSide : ((mode == Opponent::Classic) && !learnerSide);
    side.classicAi = classic[s];
    float raidRoll = (float)(((seed * 747796405u) ^ (uint32_t)((s + 1) * 2891336453u)) >> 8) / (float)(1u << 24);
    classicKind[s] = raidRoll < cfg.raiderFrac ? opponents::Kind::Raider : opponents::Kind::Classic;
    eps[s].sideIndex = s;
    samplers[s].rngState = (seed * 2654435761u) ^ (uint32_t)((s + 1) * 2246822519u);
    samplers[s].temperature = temperature;
  }

  Side* cur = game::begin(g);
  int safety = 0;
  while (cur && !g.over && !g.won && g.turn <= cfg.maxTurns && safety < cfg.maxTurns * 12) {
    safety++;
    int s = cur->index;
    if (cur->computer && cur->alive) {
      if (recorded[s]) {
        eps[s].turns.emplace_back();
        samplers[s].currentTurn = &eps[s].turns.back();
        ai::modern::playTurn(g, *cur, nullptr, &learner, &samplers[s]);
        samplers[s].currentTurn = nullptr;
      } else if (classic[s]) {
        opponents::playTurn(g, *cur, classicKind[s]);
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
    } else if (g.over || g.won) {
      ep.terminal = true;
      bool wonGame = (g.ending.winner && g.ending.winner->index == s);
      ep.terminalReward = wonGame ? cfg.winReward : 0.0f;
    } else {
      ai::belief::update(g, side);
      ep.bootstrapValue = learner.evaluateValue(ml::buildContext(g, side), ml::buildCriticFeatures(g, side));
    }
    out.recordedCities += cities[s];
    out.recordedSides++;
    out.episodes.push_back(std::move(ep));
  }
  return out;
}

// GAE over each side's own turns, plus optional per-stack multi-turn local
// credit tracked over non-recycled Game::pool Army* identities.
void computeAdvantages(std::vector<SideEpisode>& episodes, float gamma, float lambda, float localWeight) {
  constexpr float stackGamma = 0.90f;
  for (auto& ep : episodes) {
    const int T = (int)ep.turns.size();
    float gae = 0.0f;
    std::unordered_map<std::uintptr_t, float> unitFutureLocal;
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

      for (auto& [uid, val] : unitFutureLocal) val *= stackGamma;
      for (int mi = (int)cur.missionSteps.size() - 1; mi >= 0; mi--) {
        auto& ms = cur.missionSteps[mi];
        if (!ms.unitIds.empty()) {
          float perUnit = ms.immediateDeltaScore / (float)ms.unitIds.size();
          for (std::uintptr_t uid : ms.unitIds) {
            unitFutureLocal[uid] += perUnit;
          }
        }
        const auto& evalIds = ms.poolUnitIds.empty() ? ms.unitIds : ms.poolUnitIds;
        float groupReturn = 0.0f;
        for (std::uintptr_t uid : evalIds) {
          auto it = unitFutureLocal.find(uid);
          if (it != unitFutureLocal.end()) groupReturn += it->second;
        }
        ms.stackReturnTarget = groupReturn;
        float localAdv = ms.stackReturnTarget - ms.stackValuePred;
        ms.advantage = cur.advantage + localWeight * localAdv;
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
  int mission = 0, econ = 0, diplo = 0, turns = 0, stackValues = 0;
  double policyLoss = 0.0, valueLoss = 0.0, entropy = 0.0, kl = 0.0;
  int klSamples = 0, entSamples = 0;
  void add(const HeadStats& o) {
    mission += o.mission; econ += o.econ; diplo += o.diplo; turns += o.turns; stackValues += o.stackValues;
    policyLoss += o.policyLoss; valueLoss += o.valueLoss; entropy += o.entropy; kl += o.kl;
    klSamples += o.klSamples; entSamples += o.entSamples;
  }
};

// Clipped-surrogate gradient for a categorical choice over options with policy
// logits z_i = u_i / T - logMult_i, plus a KL pull toward the multiplicity
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
    z[i] = u[i] * invT - lmOf(i);
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
  const ml::ValueInVec valueIn = ml::packValueInput(tr.startCtx, tr.startCritic);
  float v = net.valueNet.forward(valueIn)[0];
  float dv = v - tr.returnTarget;
  st.valueLoss += 0.5 * dv * dv;
  st.turns++;
  net.valueNet.backward(valueIn, {dv}, grad.valueNet);

  for (const auto& ms : tr.missionSteps) {
    float sv = net.evaluateStackValue(ms.stackValueIn);
    float dsv = sv - ms.stackReturnTarget;
    st.valueLoss += 0.25 * dsv * dsv;
    st.stackValues++;
    net.stackValueNet.backward(ms.stackValueIn, {dsv}, grad.stackValueNet);

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

// One evaluation game: the scenario/preset/seed/seat assignment and its outcome.
struct EvalCase {
  std::string scenario;
  int preset = 0;
  uint32_t seed = 0;
  int swap = 0;
  double share = 0.5;   // Modern share of the cities held by both camps
  int top = 0;          // 1 when the largest side is a Modern side
  int modern = 0, classic = 0;
  int modernSides = 0, sides = 0;
};

struct EvalResult {
  int games = 0;
  double meanShare = 0.0;     // Modern AI's share of the cities held by both camps
  double ci95 = 0.0;          // half-width of the 95% interval on meanShare
  int modernTop = 0;          // games whose largest side is a Modern AI side
  int modernCities = 0, classicCities = 0;
  double parity = 0.5;        // share an equally strong Modern side would get
  std::vector<EvalCase> cases;
};

// Solo evaluation (1 Modern vs K-1 Classic) is used when asked for, and during
// cropped-map curriculum selection where the training setup is solo.
bool useSolo(const Config& cfg, bool isSelection) {
  if (cfg.evalMode == EvalMode::Solo) return true;
  return cfg.soloLearner && isSelection && (cfg.activeSides < 8 || cfg.cropMargin > 0);
}

std::vector<EvalCase> evalCases(uint32_t seedBase, int seeds) {
  std::vector<EvalCase> cases;
  for (int i = 0; i < seeds; i++) {
    for (int swap = 0; swap < 2; swap++) {
      EvalCase c;
      c.scenario = SCENARIOS[(i + i / 4) % SCENARIOS.size()];
      c.preset = i % 4;
      c.seed = seedBase + (uint32_t)i;
      c.swap = swap;
      cases.push_back(c);
    }
  }
  return cases;
}

void playEvalCase(const Config& cfg, const ml::PolicyNetwork& policy, EvalCase& c, bool solo, bool allowRandom,
                  int turns) {
  auto gp = makeArenaGame(cfg, c.scenario, PRESETS[c.preset], c.seed, allowRandom);
  Game& g = *gp;
  int activeCount = 0;
  for (int s = 0; s < 8; s++) {
    if (g.map->sides[s].inUse) activeCount++;
  }
  // Solo: the Modern seat rotates with the seed; the swapped game takes the seat
  // half-way round the table so both games of a seed differ in position.
  int soloSeat = activeCount > 0 ? (int)((c.seed + (uint32_t)(c.swap * std::max(1, activeCount / 2))) %
                                         (uint32_t)activeCount)
                                 : 0;
  std::array<bool, 8> modern{};
  int idx = 0;
  for (int s = 0; s < 8; s++) {
    if (!g.map->sides[s].inUse) continue;
    modern[s] = solo ? (idx == soloSeat) : ((idx + c.swap) % 2 == 0);
    g.map->sides[s].classicAi = !modern[s];
    if (modern[s]) c.modernSides++;
    c.sides++;
    idx++;
  }
  Side* cur = game::begin(g);
  int safety = 0;
  while (cur && !g.over && !g.won && g.turn <= turns && safety < turns * 12) {
    safety++;
    if (cur->computer && cur->alive) {
      if (modern[cur->index]) ai::modern::playTurn(g, *cur, nullptr, &policy, nullptr);
      else opponents::playTurn(g, *cur, cfg.evalOpponent);
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
  c.share = (m + k) > 0 ? (double)m / (double)(m + k) : 0.5;
  c.top = (best != NONE && modern[best]) ? 1 : 0;
  c.modern = m;
  c.classic = k;
}

// Plays `policies.size()` checkpoints on the same cases (same maps, seats and
// seeds), in parallel over all hardware threads.
std::vector<std::vector<EvalCase>> runEval(const Config& cfg, const std::vector<const ml::PolicyNetwork*>& policies,
                                           uint32_t seedBase, int seeds, int turns) {
  bool isSelection = (seedBase == SELECT_SEED_BASE);
  bool solo = useSolo(cfg, isSelection);
  std::vector<std::vector<EvalCase>> out(policies.size(), evalCases(seedBase, seeds));
  const size_t perPolicy = out[0].size();
  const size_t total = perPolicy * policies.size();
  std::atomic<size_t> next{0};
  unsigned int workers = std::max(2u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned int w = 0; w < workers; w++) {
    pool.emplace_back([&]() {
      for (size_t i; (i = next.fetch_add(1)) < total;) {
        size_t p = i % policies.size(), ci = i / policies.size();
        playEvalCase(cfg, *policies[p], out[p][ci], solo, isSelection, turns);
      }
    });
  }
  for (auto& t : pool) t.join();
  return out;
}

EvalResult summarize(std::vector<EvalCase> cases) {
  EvalResult r;
  r.games = (int)cases.size();
  double parity = 0.0;
  for (const auto& c : cases) {
    r.meanShare += c.share;
    r.modernTop += c.top;
    r.modernCities += c.modern;
    r.classicCities += c.classic;
    parity += c.sides > 0 ? (double)c.modernSides / (double)c.sides : 0.5;
  }
  r.meanShare /= std::max(1, r.games);
  r.parity = parity / std::max(1, r.games);
  double var = 0.0;
  for (const auto& c : cases) var += (c.share - r.meanShare) * (c.share - r.meanShare);
  var /= std::max(1, r.games - 1);
  r.ci95 = 1.96 * std::sqrt(var / std::max(1, r.games));
  r.cases = std::move(cases);
  return r;
}

EvalResult evaluateVsClassic(const Config& cfg, const ml::PolicyNetwork& policy, uint32_t seedBase, int seeds,
                             int turns) {
  return summarize(std::move(runEval(cfg, {&policy}, seedBase, seeds, turns)[0]));
}

void printEval(const char* label, const EvalResult& r) {
  std::printf("  [%s] %d games: Modern city share %.1f%% +/- %.1f%% (95%%, parity %.1f%%) | cities %d vs %d | "
              "Modern largest side in %d/%d\n",
              label, r.games, r.meanShare * 100.0, r.ci95 * 100.0, r.parity * 100.0, r.modernCities,
              r.classicCities, r.modernTop, r.games);
  std::fflush(stdout);
}

// Mean share per preset and per scenario.
void printBreakdown(const EvalResult& r) {
  auto line = [&](const char* kind, const std::string& key, auto match) {
    double s = 0.0;
    int n = 0, m = 0, k = 0;
    for (const auto& c : r.cases) {
      if (!match(c)) continue;
      s += c.share;
      n++;
      m += c.modern;
      k += c.classic;
    }
    if (n > 0) std::printf("    %-8s %-14s %4d games  share %5.1f%%  cities %5d vs %5d\n", kind, key.c_str(), n,
                           100.0 * s / n, m, k);
  };
  for (int p = 0; p < 4; p++) line("preset", PRESETS[p].label, [&](const EvalCase& c) { return c.preset == p; });
  for (const auto& sc : SCENARIOS) line("scenario", sc, [&](const EvalCase& c) { return c.scenario == sc; });
  std::fflush(stdout);
}

// Paired comparison: the same cases played by A and B; the mean per-game share
// difference has a much tighter interval than two independent means.
void printPaired(const char* nameA, const EvalResult& a, const char* nameB, const EvalResult& b) {
  const size_t n = std::min(a.cases.size(), b.cases.size());
  std::vector<double> d(n);
  double mean = 0.0;
  int wins = 0, losses = 0;
  for (size_t i = 0; i < n; i++) {
    d[i] = b.cases[i].share - a.cases[i].share;
    mean += d[i];
    wins += d[i] > 1e-9;
    losses += d[i] < -1e-9;
  }
  mean /= std::max<size_t>(1, n);
  double var = 0.0;
  for (double x : d) var += (x - mean) * (x - mean);
  var /= std::max<size_t>(1, n - 1);
  double ci = 1.96 * std::sqrt(var / std::max<size_t>(1, n));
  std::printf("  [paired] B - A = %+.2f%% +/- %.2f%% (95%%) over %zu games | B better in %d, worse in %d | %s\n",
              mean * 100.0, ci * 100.0, n, wins, losses,
              (mean - ci > 0) ? "B significantly better" : (mean + ci < 0) ? "B significantly worse" : "no significant difference");
  std::printf("    A = %s\n    B = %s\n", nameA, nameB);
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
    else if (a == "--report-seeds") cfg.reportSeeds = std::max(0, std::stoi(next()));
    else if (a == "--epochs") cfg.epochs = std::max(1, std::stoi(next()));
    else if (a == "--minibatches") cfg.minibatches = std::max(1, std::stoi(next()));
    else if (a == "--lr") cfg.lrStart = std::stof(next());
    else if (a == "--temp-start") cfg.tempStart = std::max(0.1f, std::stof(next()));
    else if (a == "--temp-end") cfg.tempEnd = std::max(0.1f, std::stof(next()));
    else if (a == "--eval-temp") cfg.evalTemp = std::max(0.01f, std::stof(next()));
    else if (a == "--seed-offset") cfg.seedOffset = std::min<uint32_t>(500000u, (uint32_t)std::stoul(next()));
    else if (a == "--classic-frac") cfg.classicFrac = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--random-map-frac") cfg.randomMapFrac = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--active-sides") cfg.activeSides = std::clamp(std::stoi(next()), 2, 8);
    else if (a == "--crop-margin") cfg.cropMargin = std::max(0, std::stoi(next()));
    else if (a == "--lambda") cfg.gaeLambda = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--local-weight") cfg.localWeight = std::max(0.0f, std::stof(next()));
    else if (a == "--out") cfg.outPath = next();
    else if (a == "--log") cfg.logPath = next();
    else if (a == "--data") cfg.dataDir = next();
    else if (a == "--solo-learner") cfg.soloLearner = true;
    else if (a == "--from-scratch") cfg.fromScratch = true;
    else if (a == "--benchmark-only") cfg.benchmarkOnly = true;
    else if (a == "--eval-mode") {
      std::string m = next();
      if (m == "solo") cfg.evalMode = EvalMode::Solo;
      else if (m == "mixed") cfg.evalMode = EvalMode::Mixed;
      else {
        std::fprintf(stderr, "Unknown --eval-mode '%s'; expected 'mixed' (4 vs 4) or 'solo' (1 vs K-1).\n", m.c_str());
        return false;
      }
    }
    else if (a == "--compare") cfg.comparePath = next();
    else if (a == "--dev-seeds") cfg.devSeeds = std::max(0, std::stoi(next()));
    else if (a == "--breakdown") cfg.breakdown = true;
    else if (a == "--critic-warmup") cfg.criticWarmup = std::max(0, std::stoi(next()));
    else if (a == "--mixed-frac") cfg.mixedFrac = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--save-every") cfg.saveEvery = std::max(0, std::stoi(next()));
    else if (a == "--raider-frac") cfg.raiderFrac = std::clamp(std::stof(next()), 0.0f, 1.0f);
    else if (a == "--eval-opponent") {
      std::string o = next();
      if (o == "raider") cfg.evalOpponent = opponents::Kind::Raider;
      else if (o == "classic") cfg.evalOpponent = opponents::Kind::Classic;
      else {
        std::fprintf(stderr, "Unknown --eval-opponent '%s'; expected 'classic' or 'raider'.\n", o.c_str());
        return false;
      }
    }

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
  ml::setDeployTemperature(cfg.evalTemp);

  ml::PolicyNetwork learner;
  bool loaded = false;
  if (!cfg.fromScratch && std::filesystem::exists(cfg.outPath)) loaded = learner.loadJson(cfg.outPath);
  if (!loaded) learner.initRandomWeights(2026);
  std::printf("=== w2arena: PPO self-play | params %d | threads %u | %s ===\n", ml::PolicyNetwork::TOTAL_PARAMS,
              threads, loaded ? ("loaded " + cfg.outPath).c_str() : "random init");

  if (cfg.benchmarkOnly || cfg.iterations == 0 || !cfg.comparePath.empty()) {
    // Seed set: development (--dev-seeds) > held-out report (--report-seeds) > selection.
    uint32_t base = cfg.devSeeds > 0 ? DEV_SEED_BASE : cfg.reportSeeds > 0 ? REPORT_SEED_BASE : SELECT_SEED_BASE;
    int seeds = cfg.devSeeds > 0 ? cfg.devSeeds : cfg.reportSeeds > 0 ? cfg.reportSeeds : cfg.selectSeeds;
    const char* label = cfg.devSeeds > 0 ? "dev" : cfg.reportSeeds > 0 ? "report" : "select";
    if (!loaded) {
      std::fprintf(stderr, "Cannot load checkpoint '%s' for benchmarking; pass an existing --out file.\n",
                   cfg.outPath.c_str());
      return 1;
    }
    if (cfg.comparePath.empty()) {
      EvalResult r = summarize(std::move(runEval(cfg, {&learner}, base, seeds, cfg.evalTurns)[0]));
      printEval(label, r);
      if (cfg.breakdown) printBreakdown(r);
      return 0;
    }
    ml::PolicyNetwork other;
    if (!other.loadJson(cfg.comparePath)) {
      std::fprintf(stderr, "Cannot load --compare checkpoint '%s'; check the path and JSON format.\n",
                   cfg.comparePath.c_str());
      return 1;
    }
    auto both = runEval(cfg, {&learner, &other}, base, seeds, cfg.evalTurns);
    EvalResult ra = summarize(std::move(both[0])), rb = summarize(std::move(both[1]));
    printEval((std::string(label) + " A").c_str(), ra);
    if (cfg.breakdown) printBreakdown(ra);
    printEval((std::string(label) + " B").c_str(), rb);
    if (cfg.breakdown) printBreakdown(rb);
    printPaired(cfg.outPath.c_str(), ra, cfg.comparePath.c_str(), rb);
    return 0;
  }
  std::string modeLabel =
      cfg.soloLearner ? "solo-learner (1 vs K-1 Classic)" : fmt("classic-frac %.2f", cfg.classicFrac);
  std::printf("iters %d | games/iter %d | turns %d | sides %d (crop %d, rand %.2f) | epochs %d x %d minibatches | "
              "%s\n",
              cfg.iterations, cfg.gamesPerIter, cfg.maxTurns, cfg.activeSides, cfg.cropMargin, cfg.randomMapFrac,
              cfg.epochs, cfg.minibatches, modeLabel.c_str());

  FILE* logf = cfg.logPath.empty() ? nullptr : std::fopen(cfg.logPath.c_str(), "w");
  if (logf) {
    std::fprintf(logf, "iter,seconds,temperature,lr,avg_cities,neutrals_left,entropy,value_loss,kl,epochs_run,"
                       "decisions,select_share,select_ci,ev_value,ev_stack\n");
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
  const size_t offStackValue = offValue + ml::PolicyNetwork::ValueNet::PARAM_COUNT;
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
          uint32_t seed = cfg.seedOffset + (uint32_t)(iter * 1009 + gi * 97 + 17);
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

    // Explained variance of the critics' rollout-time predictions:
    // 1 - Var(target - prediction) / Var(target); 1 is perfect, <= 0 is useless.
    auto explainedVariance = [](const std::vector<float>& target, const std::vector<float>& pred) {
      if (target.size() < 2) return 0.0;
      double mt = 0.0, mr = 0.0;
      for (size_t i = 0; i < target.size(); i++) {
        mt += target[i];
        mr += target[i] - pred[i];
      }
      mt /= target.size();
      mr /= target.size();
      double vt = 0.0, vr = 0.0;
      for (size_t i = 0; i < target.size(); i++) {
        vt += (target[i] - mt) * (target[i] - mt);
        double r = target[i] - pred[i] - mr;
        vr += r * r;
      }
      return vt > 1e-12 ? 1.0 - vr / vt : 0.0;
    };
    std::vector<float> vTarget, vPred, svTarget, svPred;
    for (const auto* tr : turns) {
      vTarget.push_back(tr->returnTarget);
      vPred.push_back(tr->valuePred);
      for (const auto& ms : tr->missionSteps) {
        svTarget.push_back(ms.stackReturnTarget);
        svPred.push_back(ms.stackValuePred);
      }
    }
    const double evValue = explainedVariance(vTarget, vPred), evStack = explainedVariance(svTarget, svPred);
    // Critic warm-up: only the critics learn for the first --critic-warmup iterations
    const bool warmup = iter <= cfg.criticWarmup;

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
        scale(offDiplo, offMission, warmup ? 0.0f : 1.0f / std::max(1, mbStats.diplo));
        scale(offMission, offEcon, warmup ? 0.0f : 1.0f / std::max(1, mbStats.mission));
        scale(offEcon, offValue, warmup ? 0.0f : 1.0f / std::max(1, mbStats.econ));
        scale(offValue, offStackValue, cfg.valueCoef / std::max(1, mbStats.turns));
        scale(offStackValue, D, cfg.valueCoef / std::max(1, mbStats.stackValues));
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
                "ev %.2f/%.2f | kl %.4f (%d ep) | %zu turns%s\n",
                iter, sec, temperature, lr, avgCities, avgNeutrals, ent, vloss, evValue, evStack, kl, epochsRun,
                turns.size(), warmup ? " | critic warm-up" : "");
    std::fflush(stdout);

    if (cfg.saveEvery > 0 && iter % cfg.saveEvery == 0) {
      std::filesystem::path snap = cfg.outPath;
      snap.replace_extension(fmt(".iter%d.json", iter));
      learner.saveJson(snap, iter, 0.0f);
    }
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
      std::fprintf(logf, "%d,%.2f,%.3f,%.2e,%.3f,%.2f,%.4f,%.5f,%.5f,%d,%zu,%.4f,%.4f,%.4f,%.4f\n", iter, sec,
                   temperature, lr, avgCities, avgNeutrals, ent, vloss, kl, epochsRun, decisions, selShare, selCi,
                   evValue, evStack);
      std::fflush(logf);
    }
  }
  if (logf) std::fclose(logf);

  best.saveJson(cfg.outPath, bestIter, bestShare);
  {
    std::filesystem::path fin = cfg.outPath;
    fin.replace_extension(".final.json");
    learner.saveJson(fin, cfg.iterations, 0.0f);
  }
  if (cfg.reportSeeds > 0) {
    std::printf("Kept iteration %d (selection share %.1f%%) in %s. Held-out report:\n", bestIter, bestShare * 100.0f,
                cfg.outPath.c_str());
    printEval("report", evaluateVsClassic(cfg, best, REPORT_SEED_BASE, cfg.reportSeeds, cfg.evalTurns));
  } else {
    std::printf("Kept iteration %d (selection share %.1f%%) in %s (report skipped: --report-seeds 0).\n", bestIter,
                bestShare * 100.0f, cfg.outPath.c_str());
  }
  return 0;
}
