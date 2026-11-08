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

// Global context: 14 original features, then 6 added in weights v6 (gold,
// turn and army size on scales that do not saturate, empty and exposed own
// cities, budget balance). Personality channels follow.
constexpr int kLegacyGlobalDim = 14;
constexpr int kAddedGlobalDim = 6;
constexpr int kGlobalDim = kLegacyGlobalDim + kAddedGlobalDim;    // 20
constexpr int kContextDim = kGlobalDim + personality::kTraitDim;  // 30

constexpr int kRivalDim = 18;
constexpr int kDiploInDim = kContextDim + kRivalDim;  // 42
constexpr int kDiploOutDim = 6;

constexpr int kStackDim = 16;
constexpr int kTargetDim = 21;
constexpr int kMissionInDim = kContextDim + kStackDim + kTargetDim;  // 61
constexpr int kMissionOutDim = 2;

// EconVec layout: [0..6] action one-hot (HoldGold, ProduceSlot, StopProd, BuySlot,
// VectorCity, RetainArmy, CaptureSpoils); [7..19] unit/cost/city features;
// [20] "is the current choice" (the slot already building, the vector already
// set), so the policy can see what switching would throw away.
constexpr int kEconFeatDim = 21;
constexpr int kEconCurrent = 20;
constexpr int kEconInDim = kContextDim + kEconFeatDim;  // 44
constexpr int kEconOutDim = 1;

// Privileged critic features (asymmetric actor-critic). The two critics are
// used only to compute training advantages, so they may read the true game
// state (all cities, armies, opponents' types) to explain more of the return
// variance. The policy heads never see these features. Layout: see
// buildCriticFeatures().
constexpr int kCriticDim = 16;

constexpr int kValueInDim = kContextDim + kCriticDim;  // 46
constexpr int kValueOutDim = 1;

constexpr int kStackValueInDim = kContextDim + kStackDim + kCriticDim;  // 62

/**
 * Default sampling temperature the Modern AI plays real games at. Lower than
 * the training temperature: a sharper version of the trained policy plays
 * measurably stronger (w2arena temperature sweep, AI-DESIGN.md).
 */
constexpr float kDeployTemperature = 0.02F;

/** Temperature PPO training anneals to (exploration still needed while learning). */
constexpr float kTrainTemperatureEnd = 0.5F;

/** Temperature real (non-training) games sample at; DEPLOY_TEMPERATURE unless overridden. */
float DeployTemperature();

/** Overrides deployTemperature() process-wide (evaluation tools only; set before threads start). */
void SetDeployTemperature(float t);

using ContextVec = std::array<float, kContextDim>;
using RivalVec = std::array<float, kRivalDim>;
using StackVec = std::array<float, kStackDim>;
using TargetVec = std::array<float, kTargetDim>;
using EconVec = std::array<float, kEconFeatDim>;
using CriticVec = std::array<float, kCriticDim>;
using ValueInVec = std::array<float, kValueInDim>;

using DiploInVec = std::array<float, kDiploInDim>;
using MissionInVec = std::array<float, kMissionInDim>;
using EconInVec = std::array<float, kEconInDim>;
using StackValueInVec = std::array<float, kStackValueInDim>;

struct DiploOutput {
    std::array<float, 4> stance_logits{};  // [0]=Allied, [1]=Peace, [2]=Uneasy, [3]=War
    float tribute_logit = 0.0F;            // >0: offer gold to end a war (binary decision)
    float joint_war_logit = 0.0F;          // >0: ask the partner to join our war (binary decision)
};

struct MissionOutput {
    float utility = 0.0F;
    float stage_near_logit = 0.0F;  // >0 stage 1-2 tiles out for sync; <=0 attack now
};

/**
 * Differentiable 2-hidden-layer MLP with a direct linear skip connection:
 *   y = W_skip * x + W3 * tanh(W2 * tanh(W1 * x + b1) + b2) + b3
 */
template <int InDim, int H1Dim, int H2Dim, int OutDim>
struct ResidualMLP {
    static constexpr int kInDim = InDim;
    static constexpr int kH1Dim = H1Dim;
    static constexpr int kH2Dim = H2Dim;
    static constexpr int kOutDim = OutDim;
    static constexpr int kParamCount = (OutDim * InDim) + (H1Dim * InDim + H1Dim) +
                                       (H2Dim * H1Dim + H2Dim) + (OutDim * H2Dim + OutDim);

    std::array<float, static_cast<size_t>(OutDim) * InDim> w_skip{};
    std::array<float, static_cast<size_t>(H1Dim) * InDim> w1{};
    std::array<float, H1Dim> b1{};
    std::array<float, static_cast<size_t>(H2Dim) * H1Dim> w2{};
    std::array<float, H2Dim> b2{};
    std::array<float, static_cast<size_t>(OutDim) * H2Dim> w3{};
    std::array<float, OutDim> b3{};

    std::array<float, OutDim> Forward(const std::array<float, InDim>& x) const;

    /**
     * Accumulates exact parameter gradients dL/dW and dL/db into `gradAccum`
     * given input `x` and output gradient `dOut` = dL/dy.
     */
    void Backward(const std::array<float, InDim>& x, const std::array<float, OutDim>& d_out,
                  ResidualMLP& grad_accum) const;

    void Zero();
    void ToFlat(std::vector<float>& out) const;
    size_t FromFlat(const std::vector<float>& in, size_t offset);

    /**
     * Loads weights saved with a narrower input of `oldInDim` columns: old column
     * c goes to c (c < insertAt) or c + insertCount (c >= insertAt); every new
     * column starts at zero, so the function computed is unchanged until
     * training uses the new inputs.
     */
    size_t FromFlatWidenInput(const std::vector<float>& in, size_t offset, int old_in_dim,
                              int insert_at, int insert_count);
};

/** Parameters of a residual MLP like `Mlp` but with an input `in` columns wide. */
template <typename Mlp>
constexpr int MlpParamsWithInput(int in) {
    return Mlp::kOutDim * in + Mlp::kH1Dim * in + Mlp::kH1Dim + Mlp::kH2Dim * Mlp::kH1Dim +
           Mlp::kH2Dim + Mlp::kOutDim * Mlp::kH2Dim + Mlp::kOutDim;
}

class PolicyNetwork {
  public:
    using DiploNet = ResidualMLP<kDiploInDim, 24, 12, kDiploOutDim>;
    using MissionNet = ResidualMLP<kMissionInDim, 32, 16, kMissionOutDim>;
    using EconNet = ResidualMLP<kEconInDim, 24, 12, kEconOutDim>;
    using ValueNet = ResidualMLP<kValueInDim, 24, 12, kValueOutDim>;
    using StackValueNet = ResidualMLP<kStackValueInDim, 24, 12, kValueOutDim>;

    static constexpr int kTotalParams = DiploNet::kParamCount + MissionNet::kParamCount +
                                        EconNet::kParamCount + ValueNet::kParamCount +
                                        StackValueNet::kParamCount;

    // Legacy files: v5 lacks the 6 added context features; v4/v3 also lack the
    // privileged critic inputs; v3 also lacks StackValueNet.
    static constexpr int kV5Dropped = kAddedGlobalDim;
    static constexpr int kV4CriticDropped = kAddedGlobalDim + kCriticDim;
    static constexpr int kLegacyV5Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV5Dropped);
    static constexpr int kLegacyV3Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV4CriticDropped);
    static constexpr int kLegacyV4Params =
            kLegacyV3Params +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV4CriticDropped);

    PolicyNetwork();

    /** Initializes all parameters from zero-mean random noise (zero human priors). */
    void InitRandomWeights(uint32_t seed = 42);

    /** Zeros all weights and biases (used when this instance acts as a gradient buffer). */
    void Zero();

    /** Evaluates Head 1 (Diplomacy stances, tribute, and joint-war urgency). */
    DiploOutput ScoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const;

    /** Evaluates Head 2 (Stack mission utility and AttackNow vs StageNear synchronization). */
    MissionOutput ScoreMission(const ContextVec& ctx, const StackVec& stack,
                               const TargetVec& target) const;

    /** Evaluates Head 3 (Unified Economy, Unit Retention, Production, Vectoring, and Capture
     * Spoils). */
    float ScoreEconomy(const ContextVec& ctx, const EconVec& econ) const;

    /** Evaluates Head 4 (critic: expected discounted return in empire-score units, unbounded). */
    float EvaluateValue(const ContextVec& ctx, const CriticVec& critic) const;

    /** Evaluates Head 5 (per-stack local critic: expected multi-turn local score change from group
     * state). */
    float EvaluateStackValue(const StackValueInVec& stack_state) const;

    /** Flattens all 5 heads into a single vector of length TOTAL_PARAMS. */
    std::vector<float> Flatten() const;

    /** Loads parameters from a flat vector of length TOTAL_PARAMS (or LEGACY_V5/V4/V3_PARAMS). */
    void Unflatten(const std::vector<float>& params);

    /** Loads a legacy (v5/v4/v3) flat vector, widening the inputs function-preservingly. */
    void UnflattenLegacy(const std::vector<float>& params);

    /** Saves the weights with the training iteration and the held-out selection score. */
    bool SaveJson(const std::filesystem::path& path, int generation = 0,
                  float selection_city_share = 0.0F) const;

    /** Loads the network weights from a JSON file; returns false if missing or invalid. */
    bool LoadJson(const std::filesystem::path& path);

    DiploNet diplo_net;
    MissionNet mission_net;
    EconNet econ_net;
    ValueNet value_net;
    StackValueNet stack_value_net;
};

// --- Trajectory Recording & Stochastic Exploration Primitives for PPO Self-Play ---

struct MissionStepRecord {
    std::vector<MissionInVec> candidates;
    std::vector<float> log_mult;
    int chosen_idx = 0;
    float old_log_prob = 0.0F;
    bool has_stage_choice = false;
    bool stage_chosen = false;
    float old_stage_log_prob = 0.0F;
    float immediate_delta_score = 0.0F;    // change in empire score while this stack acted
    StackValueInVec stack_value_in{};      // pre-decision group state for per-stack baseline
    float stack_value_pred = 0.0F;         // V_stack(s) at decision time
    float stack_return_target = 0.0F;      // multi-turn local return achieved by pre-decision group
    std::vector<std::uintptr_t> unit_ids;  // non-recycled Game::pool Army* IDs in chosen stack
    std::vector<std::uintptr_t>
            pool_unit_ids;  // non-recycled Game::pool Army* IDs matching stackValueIn
    int vacated_city =
            kNone;  // own city this move left without defenders (still empty at turn end)
    float vacated_loss =
            0.0F;  // filled by the trainer: that city's score weight if lost by our next turn
    float advantage = 0.0F;  // filled by the trainer
};

struct EconStepRecord {
    std::vector<EconInVec> candidates;
    std::vector<float> log_mult;
    int chosen_idx = 0;
    float old_log_prob = 0.0F;
};

struct DiploStepRecord {
    DiploInVec input{};
    int chosen_stance = 1;
    float old_stance_log_prob = 0.0F;
    // Optional binary sub-decisions taken alongside a Peace/Ally stance
    bool has_tribute = false;  // offer gold to end a war (output 4)
    bool tribute_chosen = false;
    float old_tribute_log_prob = 0.0F;
    bool has_joint_war = false;  // ask the partner to join our war (output 5)
    bool joint_war_chosen = false;
    float old_joint_war_log_prob = 0.0F;
};

struct TurnStepRecord {
    ContextVec start_ctx{};
    CriticVec start_critic{};  // privileged critic features as the turn begins
    float value_pred = 0.0F;
    float start_score = 0.0F;    // empire score as the turn begins
    float advantage = 0.0F;      // GAE, filled by the trainer
    float return_target = 0.0F;  // critic target, filled by the trainer
    std::vector<MissionStepRecord> mission_steps;
    std::vector<EconStepRecord> econ_steps;
    std::vector<DiploStepRecord> diplo_steps;
};

struct RolloutSampler {
    uint32_t rng_state = 0x9e3779b9U;
    float temperature = 1.0F;
    TurnStepRecord* current_turn = nullptr;

    float NextUniform01();
};

/** Packs (ContextVec, RivalVec) into a single DiploInVec. */
DiploInVec PackDiploInput(const ContextVec& ctx, const RivalVec& rival);

/** Packs (ContextVec, StackVec, TargetVec) into a single MissionInVec. */
MissionInVec PackMissionInput(const ContextVec& ctx, const StackVec& stack,
                              const TargetVec& target);

/** Packs (ContextVec, EconVec) into a single EconInVec. */
EconInVec PackEconInput(const ContextVec& ctx, const EconVec& econ);

/** Packs (ContextVec, CriticVec) into a single ValueInVec. */
ValueInVec PackValueInput(const ContextVec& ctx, const CriticVec& critic);

/** Packs (ContextVec, StackVec, CriticVec) into a single StackValueInVec. */
StackValueInVec PackStackValueInput(const ContextVec& ctx, const StackVec& stack,
                                    const CriticVec& critic);

/**
 * Samples an index from softmax(logits / temperature) when `sampler != nullptr`,
 * or returns deterministic argmax when `sampler == nullptr`. Writes log(p[chosen])
 * to `outLogProb` if non-null.
 */
int SelectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler,
                     float* out_log_prob = nullptr, const std::vector<float>* log_mult = nullptr);

/**
 * A binary (yes/no) policy decision with logit `logit`: samples
 * Bernoulli(sigmoid(logit / temperature)) when `sampler != nullptr`, or
 * returns `logit > 0` (the mode) when `sampler == nullptr`. Writes the log
 * probability of the outcome to `outLogProb` if non-null.
 */
bool SelectBinary(float logit, RolloutSampler* sampler, float* out_log_prob = nullptr);

/**
 * The empire score S(g, side) whose per-turn change is the training reward:
 *   S = 2 * cityShare + 0.5 * incomeShare + 0.25 * armyShare (+ recon <= 0.15
 *       under Hidden Map), and 0 once the side is eliminated.
 * Shares are of all standing cities / income / armies, so S is a hand-chosen
 * definition of "doing well" -- the one deliberate prior of the trainer. It
 * reads the true game state and is used only for offline training rewards,
 * never as a policy input.
 */
float ComputeEmpireScore(const Game& g, int side_index);

/** Returns the global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& ActivePolicy();

/** Builds the 24D fair-information (Global State + Personality) context vector for `side`. */
ContextVec BuildContext(const Game& g, const Side& side);

/**
 * Builds the privileged critic features for `side` from the true game state.
 * Training-only information: fed to ValueNet / StackValueNet, never to a
 * policy head.
 */
CriticVec BuildCriticFeatures(const Game& g, const Side& side);

}  // namespace w2::ai::ml
