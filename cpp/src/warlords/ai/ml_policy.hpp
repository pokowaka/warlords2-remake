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

// Privileged critic features (asymmetric actor-critic). The two critics are
// used only to compute training advantages, so they may read the true game
// state (all cities, armies, opponents' types) to explain more of the return
// variance. The policy heads never see these features. Layout: see
// buildCriticFeatures().
constexpr int CRITIC_DIM = 16;

constexpr int VALUE_IN_DIM = CONTEXT_DIM + CRITIC_DIM;            // 40
constexpr int VALUE_OUT_DIM = 1;

constexpr int STACK_VALUE_IN_DIM = CONTEXT_DIM + STACK_DIM + CRITIC_DIM;  // 56
// Input widths of the critics before the privileged features (weights v3/v4)
constexpr int LEGACY_VALUE_IN_DIM = CONTEXT_DIM;                  // 24
constexpr int LEGACY_STACK_VALUE_IN_DIM = CONTEXT_DIM + STACK_DIM; // 40

/** Sampling temperature the Modern AI plays real games at (training anneals to it). */
constexpr float DEPLOY_TEMPERATURE = 0.5f;

using ContextVec = std::array<float, CONTEXT_DIM>;
using RivalVec = std::array<float, RIVAL_DIM>;
using StackVec = std::array<float, STACK_DIM>;
using TargetVec = std::array<float, TARGET_DIM>;
using EconVec = std::array<float, ECON_FEAT_DIM>;
using CriticVec = std::array<float, CRITIC_DIM>;
using ValueInVec = std::array<float, VALUE_IN_DIM>;

using DiploInVec = std::array<float, DIPLO_IN_DIM>;
using MissionInVec = std::array<float, MISSION_IN_DIM>;
using EconInVec = std::array<float, ECON_IN_DIM>;
using StackValueInVec = std::array<float, STACK_VALUE_IN_DIM>;

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
  static constexpr int IN_DIM = InDim;
  static constexpr int H1_DIM = H1Dim;
  static constexpr int H2_DIM = H2Dim;
  static constexpr int OUT_DIM = OutDim;
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

  /**
   * Loads weights saved with a narrower input (`oldInDim` <= InDim): the first
   * `oldInDim` input columns are copied, the extra ones start at zero, so the
   * function computed is unchanged until training uses the new inputs.
   */
  size_t fromFlatNarrowInput(const std::vector<float>& in, size_t offset, int oldInDim);
};

class PolicyNetwork {
 public:
  using DiploNet = ResidualMLP<DIPLO_IN_DIM, 24, 12, DIPLO_OUT_DIM>;
  using MissionNet = ResidualMLP<MISSION_IN_DIM, 32, 16, MISSION_OUT_DIM>;
  using EconNet = ResidualMLP<ECON_IN_DIM, 24, 12, ECON_OUT_DIM>;
  using ValueNet = ResidualMLP<VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;
  using StackValueNet = ResidualMLP<STACK_VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;

  static constexpr int TOTAL_PARAMS = DiploNet::PARAM_COUNT + MissionNet::PARAM_COUNT + EconNet::PARAM_COUNT +
                                      ValueNet::PARAM_COUNT + StackValueNet::PARAM_COUNT;
  /** Parameters of the critics before the privileged inputs were added (v3/v4 files). */
  static constexpr int LEGACY_VALUE_PARAMS = ValueNet::PARAM_COUNT - CRITIC_DIM * (1 + 24);
  static constexpr int LEGACY_STACK_VALUE_PARAMS = StackValueNet::PARAM_COUNT - CRITIC_DIM * (1 + 24);
  static constexpr int LEGACY_V3_PARAMS =
      DiploNet::PARAM_COUNT + MissionNet::PARAM_COUNT + EconNet::PARAM_COUNT + LEGACY_VALUE_PARAMS;
  static constexpr int LEGACY_V4_PARAMS = LEGACY_V3_PARAMS + LEGACY_STACK_VALUE_PARAMS;

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

  /** Evaluates Head 4 (critic: expected discounted return in empire-score units, unbounded). */
  float evaluateValue(const ContextVec& ctx, const CriticVec& critic) const;

  /** Evaluates Head 5 (per-stack local critic: expected multi-turn local score change from group state). */
  float evaluateStackValue(const StackValueInVec& stackState) const;

  /** Flattens all 5 heads into a single vector of length TOTAL_PARAMS. */
  std::vector<float> flatten() const;

  /** Loads parameters from a flat vector of length TOTAL_PARAMS (or LEGACY_V4/V3_PARAMS). */
  void unflatten(const std::vector<float>& params);

  /** Saves the weights with the training iteration and the held-out selection score. */
  bool saveJson(const std::filesystem::path& path, int generation = 0, float selectionCityShare = 0.0f) const;

  /** Loads the network weights from a JSON file; returns false if missing or invalid. */
  bool loadJson(const std::filesystem::path& path);

  DiploNet diploNet;
  MissionNet missionNet;
  EconNet econNet;
  ValueNet valueNet;
  StackValueNet stackValueNet;
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
  float immediateDeltaScore = 0.0f;   // change in empire score while this stack acted
  StackValueInVec stackValueIn{};     // pre-decision group state for per-stack baseline
  float stackValuePred = 0.0f;        // V_stack(s) at decision time
  float stackReturnTarget = 0.0f;     // multi-turn local return achieved by pre-decision group
  std::vector<std::uintptr_t> unitIds;      // non-recycled Game::pool Army* IDs in chosen stack
  std::vector<std::uintptr_t> poolUnitIds;  // non-recycled Game::pool Army* IDs matching stackValueIn
  float advantage = 0.0f;             // filled by the trainer
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
  CriticVec startCritic{};            // privileged critic features as the turn begins
  float valuePred = 0.0f;
  float startScore = 0.0f;            // empire score as the turn begins
  float advantage = 0.0f;             // GAE, filled by the trainer
  float returnTarget = 0.0f;          // critic target, filled by the trainer
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

/** Packs (ContextVec, CriticVec) into a single ValueInVec. */
ValueInVec packValueInput(const ContextVec& ctx, const CriticVec& critic);

/** Packs (ContextVec, StackVec, CriticVec) into a single StackValueInVec. */
StackValueInVec packStackValueInput(const ContextVec& ctx, const StackVec& stack, const CriticVec& critic);

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
 * The empire score S(g, side) whose per-turn change is the training reward:
 *   S = 2 * cityShare + 0.5 * incomeShare + 0.25 * armyShare (+ recon <= 0.15
 *       under Hidden Map), and 0 once the side is eliminated.
 * Shares are of all standing cities / income / armies, so S is a hand-chosen
 * definition of "doing well" -- the one deliberate prior of the trainer. It
 * reads the true game state and is used only for offline training rewards,
 * never as a policy input.
 */
float computeEmpireScore(const Game& g, int sideIndex);

/** Returns the global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& activePolicy();

/** Builds the 24D fair-information (Global State + Personality) context vector for `side`. */
ContextVec buildContext(const Game& g, const Side& side);

/**
 * Builds the privileged critic features for `side` from the true game state.
 * Training-only information: fed to ValueNet / StackValueNet, never to a
 * policy head.
 */
CriticVec buildCriticFeatures(const Game& g, const Side& side);

}  // namespace w2::ai::ml
