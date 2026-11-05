// 4-Head Differentiable Neural Policy & Value Network for the Fair-Information Warlords II AI.
//
// Architectural Role: Evaluates strategic, operational, diplomatic, and economic
// macro-actions using four compact multilayer perceptrons with linear residual
// skip connections, conditioned on the 24D (Global State + Personality) context.
// Initialized tabula rasa from small zero-mean random weights (zero hand-coded
// strategic priors) and supports analytical backpropagation for Proximal Policy
// Optimization (PPO) self-play training and JSON serialization (ai_weights.json).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "warlords/ai/personality.hpp"
#include "warlords/types.hpp"

namespace w2::ai::ml {

constexpr int GLOBAL_DIM = 14;
constexpr int CONTEXT_DIM = GLOBAL_DIM + personality::TRAIT_DIM;  // 24

constexpr int RIVAL_DIM = 18;
constexpr int DIPLO_IN_DIM = CONTEXT_DIM + RIVAL_DIM;             // 42
constexpr int DIPLO_OUT_DIM = 6;

constexpr int STACK_DIM = 16;
constexpr int TARGET_DIM = 21;
constexpr int MISSION_IN_DIM = CONTEXT_DIM + STACK_DIM + TARGET_DIM;  // 61
constexpr int MISSION_OUT_DIM = 2;

// EconVec layout: [0..6] action one-hot (HoldGold, ProduceSlot, StopProd, BuySlot,
// VectorCity, RetainArmy, CaptureSpoils); [7..19] unit/cost/city features;
// [20] "is the current choice" (the slot already building, the vector already
// set), so the policy can see what switching would throw away.
constexpr int ECON_FEAT_DIM = 21;
constexpr int ECON_CURRENT = 20;
constexpr int ECON_IN_DIM = CONTEXT_DIM + ECON_FEAT_DIM;          // 44
constexpr int ECON_OUT_DIM = 1;

constexpr int VALUE_IN_DIM = CONTEXT_DIM;                         // 24
constexpr int VALUE_OUT_DIM = 1;

using ContextVec = std::array<float, CONTEXT_DIM>;
using RivalVec = std::array<float, RIVAL_DIM>;
using StackVec = std::array<float, STACK_DIM>;
using TargetVec = std::array<float, TARGET_DIM>;
using EconVec = std::array<float, ECON_FEAT_DIM>;

using DiploInVec = std::array<float, DIPLO_IN_DIM>;
using MissionInVec = std::array<float, MISSION_IN_DIM>;
using EconInVec = std::array<float, ECON_IN_DIM>;

struct DiploOutput {
  std::array<float, 4> stanceLogits{};  // [0]=Allied, [1]=Peace, [2]=Uneasy, [3]=War
  float tributeLogit = 0.0f;            // >0: offer gold to end a war (binary decision)
  float jointWarLogit = 0.0f;           // >0: ask the partner to join our war (binary decision)
};

struct MissionOutput {
  float utility = 0.0f;
  float stageNearLogit = 0.0f;          // >0 stage 1-2 tiles out for sync; <=0 attack now
};

/**
 * Differentiable 2-hidden-layer MLP with a direct linear skip connection:
 *   y = W_skip * x + W3 * tanh(W2 * tanh(W1 * x + b1) + b2) + b3
 */
template <int InDim, int H1Dim, int H2Dim, int OutDim>
struct ResidualMLP {
  static constexpr int PARAM_COUNT =
      (OutDim * InDim) + (H1Dim * InDim + H1Dim) + (H2Dim * H1Dim + H2Dim) + (OutDim * H2Dim + OutDim);

  std::array<float, OutDim * InDim> wSkip{};
  std::array<float, H1Dim * InDim> w1{};
  std::array<float, H1Dim> b1{};
  std::array<float, H2Dim * H1Dim> w2{};
  std::array<float, H2Dim> b2{};
  std::array<float, OutDim * H2Dim> w3{};
  std::array<float, OutDim> b3{};

  std::array<float, OutDim> forward(const std::array<float, InDim>& x) const;

  /**
   * Accumulates exact parameter gradients dL/dW and dL/db into `gradAccum`
   * given input `x` and output gradient `dOut` = dL/dy.
   */
  void backward(const std::array<float, InDim>& x, const std::array<float, OutDim>& dOut,
                ResidualMLP& gradAccum) const;

  void zero();
  void toFlat(std::vector<float>& out) const;
  size_t fromFlat(const std::vector<float>& in, size_t offset);
};

class PolicyNetwork {
 public:
  using DiploNet = ResidualMLP<DIPLO_IN_DIM, 24, 12, DIPLO_OUT_DIM>;
  using MissionNet = ResidualMLP<MISSION_IN_DIM, 32, 16, MISSION_OUT_DIM>;
  using EconNet = ResidualMLP<ECON_IN_DIM, 24, 12, ECON_OUT_DIM>;
  using ValueNet = ResidualMLP<VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;

  static constexpr int TOTAL_PARAMS =
      DiploNet::PARAM_COUNT + MissionNet::PARAM_COUNT + EconNet::PARAM_COUNT + ValueNet::PARAM_COUNT;

  PolicyNetwork();

  /** Initializes all parameters from zero-mean random noise (zero human priors). */
  void initRandomWeights(uint32_t seed = 42);

  /** Zeros all weights and biases (used when this instance acts as a gradient buffer). */
  void zero();

  /** Evaluates Head 1 (Diplomacy stances, tribute, and joint-war urgency). */
  DiploOutput scoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const;

  /** Evaluates Head 2 (Stack mission utility and AttackNow vs StageNear synchronization). */
  MissionOutput scoreMission(const ContextVec& ctx, const StackVec& stack, const TargetVec& target) const;

  /** Evaluates Head 3 (Unified Economy, Unit Retention, Production, Vectoring, and Capture Spoils). */
  float scoreEconomy(const ContextVec& ctx, const EconVec& econ) const;

  /** Evaluates Head 4 (Expected game outcome value in [-1, 1]). */
  float evaluateValue(const ContextVec& ctx) const;

  /** Flattens all 4 heads into a single vector of length TOTAL_PARAMS. */
  std::vector<float> flatten() const;

  /** Loads parameters from a flat vector of length TOTAL_PARAMS. */
  void unflatten(const std::vector<float>& params);

  /** Saves the network weights and training metadata to a JSON file. */
  bool saveJson(const std::filesystem::path& path, int generation = 0, float elo = 1500.0f) const;

  /** Loads the network weights from a JSON file; returns false if missing or invalid. */
  bool loadJson(const std::filesystem::path& path);

  DiploNet diploNet;
  MissionNet missionNet;
  EconNet econNet;
  ValueNet valueNet;
};

// --- Trajectory Recording & Stochastic Exploration Primitives for PPO Self-Play ---

struct MissionStepRecord {
  std::vector<MissionInVec> candidates;
  std::vector<float> logMult;
  int chosenIdx = 0;
  float oldLogProb = 0.0f;
  bool hasStageChoice = false;
  bool stageChosen = false;
  float oldStageLogProb = 0.0f;
  float immediateDeltaPhi = 0.0f;
  float causalAdvantage = 0.0f;
};

struct EconStepRecord {
  std::vector<EconInVec> candidates;
  std::vector<float> logMult;
  int chosenIdx = 0;
  float oldLogProb = 0.0f;
};

struct DiploStepRecord {
  DiploInVec input{};
  int chosenStance = 1;
  float oldStanceLogProb = 0.0f;
  // Optional binary sub-decisions taken alongside a Peace/Ally stance
  bool hasTribute = false;      // offer gold to end a war (output 4)
  bool tributeChosen = false;
  float oldTributeLogProb = 0.0f;
  bool hasJointWar = false;     // ask the partner to join our war (output 5)
  bool jointWarChosen = false;
  float oldJointWarLogProb = 0.0f;
};

struct TurnStepRecord {
  ContextVec startCtx{};
  float valuePred = 0.0f;
  float startPhi = 0.0f;
  float postTurnPhi = 0.0f;
  float advantage = 0.0f;
  float restAdvantage = 0.0f;
  float returnTarget = 0.0f;
  std::vector<MissionStepRecord> missionSteps;
  std::vector<EconStepRecord> econSteps;
  std::vector<DiploStepRecord> diploSteps;
};

struct RolloutSampler {
  uint32_t rngState = 0x9e3779b9u;
  float temperature = 1.0f;
  TurnStepRecord* currentTurn = nullptr;

  float nextUniform01();
};

/** Packs (ContextVec, RivalVec) into a single DiploInVec. */
DiploInVec packDiploInput(const ContextVec& ctx, const RivalVec& rival);

/** Packs (ContextVec, StackVec, TargetVec) into a single MissionInVec. */
MissionInVec packMissionInput(const ContextVec& ctx, const StackVec& stack, const TargetVec& target);

/** Packs (ContextVec, EconVec) into a single EconInVec. */
EconInVec packEconInput(const ContextVec& ctx, const EconVec& econ);

/**
 * Samples an index from softmax(logits / temperature) when `sampler != nullptr`,
 * or returns deterministic argmax when `sampler == nullptr`. Writes log(p[chosen])
 * to `outLogProb` if non-null.
 */
int selectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler,
                     float* outLogProb = nullptr, const std::vector<float>* logMult = nullptr);

/**
 * A binary (yes/no) policy decision with logit `logit`: samples
 * Bernoulli(sigmoid(logit / temperature)) when `sampler != nullptr`, or
 * returns `logit > 0` (the mode) when `sampler == nullptr`. Writes the log
 * probability of the outcome to `outLogProb` if non-null.
 */
bool selectBinary(float logit, RolloutSampler* sampler, float* outLogProb = nullptr);

/**
 * Computes pure outcome-based empire state potential Phi(g, sideIndex) from
 * owned city share, income share, and surviving army strength (used strictly for
 * potential-based reward shaping during offline RL training, never as a policy input).
 */
float computeEmpirePotential(const Game& g, int sideIndex);

/** Returns the global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& activePolicy();

/** Builds the 24D fair-information (Global State + Personality) context vector for `side`. */
ContextVec buildContext(const Game& g, const Side& side);

}  // namespace w2::ai::ml
