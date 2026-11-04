// 4-Head Neural Policy Network for the Fair-Information Warlords II AI.
//
// Architectural Role: Evaluates strategic, operational, diplomatic, and economic
// macro-actions using four compact multilayer perceptrons with linear residual
// skip connections, conditioned on the 24D (Global State + Personality) context.
// Supports flat parameter vector access for CMA-ES / Population-Based Evolution
// and JSON serialization (ai_weights.json).
#pragma once

#include <array>
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

constexpr int STACK_DIM = 14;
constexpr int TARGET_DIM = 18;
constexpr int MISSION_IN_DIM = CONTEXT_DIM + STACK_DIM + TARGET_DIM;  // 56
constexpr int MISSION_OUT_DIM = 2;

constexpr int ECON_FEAT_DIM = 20;
constexpr int ECON_IN_DIM = CONTEXT_DIM + ECON_FEAT_DIM;          // 44
constexpr int ECON_OUT_DIM = 1;

constexpr int VALUE_IN_DIM = CONTEXT_DIM;                         // 24
constexpr int VALUE_OUT_DIM = 1;

using ContextVec = std::array<float, CONTEXT_DIM>;
using RivalVec = std::array<float, RIVAL_DIM>;
using StackVec = std::array<float, STACK_DIM>;
using TargetVec = std::array<float, TARGET_DIM>;
using EconVec = std::array<float, ECON_FEAT_DIM>;

struct DiploOutput {
  std::array<float, 4> stanceLogits{};  // [0]=Allied, [1]=Peace, [2]=Uneasy, [3]=War
  float tributeSignal = 0.0f;           // >0 demand gold, <0 offer gold
  float jointWarSignal = 0.0f;          // >0 request joint war
};

struct MissionOutput {
  float utility = 0.0f;
  float stageNearLogit = 0.0f;          // >0 stage 2 tiles out for next-turn sync; <=0 attack now
};

/**
 * A 2-hidden-layer MLP with a direct linear skip connection from input to output:
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

  /** Resets parameters to structured prior weights + small deterministic symmetry-breaking weights. */
  void initPriorWeights(uint32_t seed = 42);

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

/** Returns the global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& activePolicy();

/** Builds the 24D (Global State + Personality) context vector for `side`. */
ContextVec buildContext(const Game& g, const Side& side);

}  // namespace w2::ai::ml
