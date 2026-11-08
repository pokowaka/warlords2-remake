/**
 * @file
 * @brief The learnable AI's neural policy and value networks.
 *
 * Scores strategic, operational, diplomatic and economic macro-actions with
 * small multilayer perceptrons that have a linear skip connection, each
 * conditioned on the context vector (global state, then personality). The
 * networks start from small zero-mean random weights, with no hand-coded
 * strategy, and support exact backpropagation for Proximal Policy
 * Optimization (PPO) self-play training and saving to JSON
 * (ai_weights.json).
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"

#include "warlords/ai/personality.hpp"
#include "warlords/types.hpp"

namespace w2::ai::ml {

// Global context: 14 original features, then 6 added in weights v6 (gold,
// turn and army size on scales that do not saturate, empty and exposed own
// cities, budget balance). Personality channels follow.
constexpr int kLegacyGlobalDim = 14;  ///< Global features of weights v5 and older.
constexpr int kAddedGlobalDim = 6;    ///< Global features added in weights v6.
constexpr int kGlobalDim = kLegacyGlobalDim + kAddedGlobalDim;    ///< 20 global features.
constexpr int kContextDim = kGlobalDim + personality::kTraitDim;  ///< 30: global + personality.

constexpr int kRivalDim = 18;                         ///< Features describing one rival.
constexpr int kDiploInDim = kContextDim + kRivalDim;  ///< 48: diplomacy head input.
constexpr int kDiploOutDim = 6;                       ///< Diplomacy head outputs.

constexpr int kStackDim = 16;   ///< Features describing one of our stacks.
constexpr int kTargetDim = 21;  ///< Features describing one mission target.
constexpr int kMissionInDim = kContextDim + kStackDim + kTargetDim;  ///< 67: mission head input.
constexpr int kMissionOutDim = 2;                                    ///< Mission head outputs.

// EconVec layout: [0..6] action one-hot (HoldGold, ProduceSlot, StopProd, BuySlot,
// VectorCity, RetainArmy, CaptureSpoils); [7..19] unit/cost/city features;
// [20] "is the current choice" (the slot already building, the vector already
// set), so the policy can see what switching would throw away.
constexpr int kEconFeatDim = 21;                        ///< Features of one economic option.
constexpr int kEconCurrent = 20;                        ///< Index of "is the current choice".
constexpr int kEconInDim = kContextDim + kEconFeatDim;  ///< 51: economy head input.
constexpr int kEconOutDim = 1;                          ///< Economy head outputs.

// Privileged critic features (asymmetric actor-critic). The two critics are
// used only to compute training advantages, so they may read the true game
// state (all cities, armies, opponents' types) to explain more of the return
// variance. The policy heads never see these features. Layout: see
// BuildCriticFeatures().
constexpr int kCriticDim = 16;  ///< Privileged critic features.

constexpr int kValueInDim = kContextDim + kCriticDim;  ///< 46: value head input.
constexpr int kValueOutDim = 1;                        ///< Value head outputs.

/// 62: per-stack value head input.
constexpr int kStackValueInDim = kContextDim + kStackDim + kCriticDim;

/**
 * @brief Default sampling temperature the learnable AI plays real games at.
 *
 * Lower than the training temperature: a sharper version of the trained
 * policy plays measurably stronger (w2arena temperature sweep, AI-DESIGN.md).
 */
constexpr float kDeployTemperature = 0.02F;

/** @brief Temperature PPO training anneals to (exploration still needed while learning). */
constexpr float kTrainTemperatureEnd = 0.5F;

/** @brief Temperature real (non-training) games sample at; kDeployTemperature unless overridden. */
float DeployTemperature();

/**
 * @brief Overrides DeployTemperature() process-wide.
 *
 * For evaluation tools only; set it before threads start.
 */
void SetDeployTemperature(float temperature);

using ContextVec = std::array<float, kContextDim>;  ///< Global state and personality.
using RivalVec = std::array<float, kRivalDim>;      ///< One rival, for the diplomacy head.
using StackVec = std::array<float, kStackDim>;      ///< One of our stacks.
using TargetVec = std::array<float, kTargetDim>;    ///< One mission target.
using EconVec = std::array<float, kEconFeatDim>;    ///< One economic option.
using CriticVec = std::array<float, kCriticDim>;    ///< Privileged critic features.
using ValueInVec = std::array<float, kValueInDim>;  ///< Value head input.

using DiploInVec = std::array<float, kDiploInDim>;            ///< Diplomacy head input.
using MissionInVec = std::array<float, kMissionInDim>;        ///< Mission head input.
using EconInVec = std::array<float, kEconInDim>;              ///< Economy head input.
using StackValueInVec = std::array<float, kStackValueInDim>;  ///< Per-stack value head input.

/** @brief What the diplomacy head says about one rival. */
struct DiploOutput {
    std::array<float, 4> stance_logits{};  ///< Allied, Peace, Uneasy, War.
    float tribute_logit = 0.0F;            ///< > 0: offer gold to end a war.
    float joint_war_logit = 0.0F;          ///< > 0: ask the partner to join our war.
};

/** @brief What the mission head says about sending a stack to a target. */
struct MissionOutput {
    float utility = 0.0F;           ///< How good the mission is.
    float stage_near_logit = 0.0F;  ///< > 0: stage 1-2 tiles out to synchronize; else attack now.
};

/**
 * @brief A differentiable MLP with two hidden layers and a direct linear skip
 * connection: y = W_skip * x + W3 * tanh(W2 * tanh(W1 * x + b1) + b2) + b3.
 *
 * Weights are stored row by row: row i of a matrix holds the weights into
 * output i.
 */
template <int InDim, int H1Dim, int H2Dim, int OutDim>
struct ResidualMLP {
    static constexpr int kInDim = InDim;    ///< Input width.
    static constexpr int kH1Dim = H1Dim;    ///< First hidden layer width.
    static constexpr int kH2Dim = H2Dim;    ///< Second hidden layer width.
    static constexpr int kOutDim = OutDim;  ///< Output width.
    /// Number of weights and biases, in the order ToFlat() writes them.
    static constexpr int kParamCount = (OutDim * InDim) + (H1Dim * InDim + H1Dim) +
                                       (H2Dim * H1Dim + H2Dim) + (OutDim * H2Dim + OutDim);

    std::array<float, static_cast<size_t>(OutDim) * InDim> w_skip{};  ///< Skip weights.
    std::array<float, static_cast<size_t>(H1Dim) * InDim> w1{};       ///< Input to hidden 1.
    std::array<float, H1Dim> b1{};                                    ///< Hidden 1 biases.
    std::array<float, static_cast<size_t>(H2Dim) * H1Dim> w2{};       ///< Hidden 1 to hidden 2.
    std::array<float, H2Dim> b2{};                                    ///< Hidden 2 biases.
    std::array<float, static_cast<size_t>(OutDim) * H2Dim> w3{};      ///< Hidden 2 to output.
    std::array<float, OutDim> b3{};                                   ///< Output biases.

    /** @brief Evaluates the network on an input. */
    std::array<float, OutDim> Forward(const std::array<float, InDim>& input) const;

    /**
     * @brief Accumulates the exact parameter gradients dL/dW and dL/db into
     * `grad_accum`, given the input and the output gradient `d_out` = dL/dy.
     */
    void Backward(const std::array<float, InDim>& input, const std::array<float, OutDim>& d_out,
                  ResidualMLP& grad_accum) const;

    /** @brief Sets every weight and bias to zero. */
    void Zero();
    /** @brief Appends every weight and bias to `out`. */
    void ToFlat(std::vector<float>& out) const;
    /**
     * @brief Reads every weight and bias from `flat`, starting at `offset`.
     * @return The offset just past what was read.
     */
    size_t FromFlat(const std::vector<float>& flat, size_t offset);

    /**
     * @brief Loads weights saved with a narrower input of `old_in_dim` columns.
     *
     * Old column c goes to c (c < insert_at) or c + insert_count
     * (c >= insert_at); every new column starts at zero, so the function
     * computed is unchanged until training uses the new inputs.
     * @return The offset just past what was read.
     */
    size_t FromFlatWidenInput(const std::vector<float>& flat, size_t offset, int old_in_dim,
                              int insert_at, int insert_count);
};

/** @brief Parameters of a residual MLP like `Mlp` but with an input `in_dim` columns wide. */
template <typename Mlp>
constexpr int MlpParamsWithInput(int in_dim) {
    return Mlp::kOutDim * in_dim + Mlp::kH1Dim * in_dim + Mlp::kH1Dim + Mlp::kH2Dim * Mlp::kH1Dim +
           Mlp::kH2Dim + Mlp::kOutDim * Mlp::kH2Dim + Mlp::kOutDim;
}

/**
 * @brief The learnable AI's networks: three policy heads and two critics.
 *
 * Diplomacy, mission and economy heads choose what to do; the value and
 * per-stack value critics are used only in training.
 */
class PolicyNetwork {
  public:
    using DiploNet = ResidualMLP<kDiploInDim, 24, 12, kDiploOutDim>;            ///< Head 1.
    using MissionNet = ResidualMLP<kMissionInDim, 32, 16, kMissionOutDim>;      ///< Head 2.
    using EconNet = ResidualMLP<kEconInDim, 24, 12, kEconOutDim>;               ///< Head 3.
    using ValueNet = ResidualMLP<kValueInDim, 24, 12, kValueOutDim>;            ///< Head 4.
    using StackValueNet = ResidualMLP<kStackValueInDim, 24, 12, kValueOutDim>;  ///< Head 5.

    /// Parameters of all five heads, the current (v6) file format.
    static constexpr int kTotalParams = DiploNet::kParamCount + MissionNet::kParamCount +
                                        EconNet::kParamCount + ValueNet::kParamCount +
                                        StackValueNet::kParamCount;

    // Legacy files: v5 lacks the 6 added context features; v4/v3 also lack the
    // privileged critic inputs; v3 also lacks StackValueNet.
    static constexpr int kV5Dropped = kAddedGlobalDim;  ///< Inputs v5 lacks.
    /// Critic inputs v4 and v3 lack.
    static constexpr int kV4CriticDropped = kAddedGlobalDim + kCriticDim;
    /// Parameters in a v5 file.
    static constexpr int kLegacyV5Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV5Dropped);
    /// Parameters in a v3 file.
    static constexpr int kLegacyV3Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV4CriticDropped);
    /// Parameters in a v4 file.
    static constexpr int kLegacyV4Params =
            kLegacyV3Params +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV4CriticDropped);

    /** @brief Initializes all five heads with default random weights (seed 42). */
    PolicyNetwork();

    /** @brief Initializes all parameters from zero-mean random noise (no human priors). */
    void InitRandomWeights(uint32_t seed = 42);

    /** @brief Zeros all weights and biases (used when this instance acts as a gradient buffer). */
    void Zero();

    /** @brief Evaluates head 1: diplomatic stances, tribute and joint-war urgency. */
    DiploOutput ScoreDiplomacy(const ContextVec& context, const RivalVec& rival) const;

    /** @brief Evaluates head 2: a stack mission's utility, and attacking now vs staging near. */
    MissionOutput ScoreMission(const ContextVec& context, const StackVec& stack,
                               const TargetVec& target) const;

    /**
     * @brief Evaluates head 3: the unified economy (unit retention, production,
     * vectoring and capture spoils).
     */
    float ScoreEconomy(const ContextVec& context, const EconVec& econ) const;

    /** @brief Evaluates head 4, the critic: expected discounted return in empire-score units. */
    float EvaluateValue(const ContextVec& context, const CriticVec& critic) const;

    /**
     * @brief Evaluates head 5, the per-stack critic: expected multi-turn local
     * score change from a group's state.
     */
    float EvaluateStackValue(const StackValueInVec& stack_state) const;

    /** @brief Flattens all five heads into a single vector of kTotalParams values. */
    std::vector<float> Flatten() const;

    /** @brief Loads parameters from a flat vector of kTotalParams values. */
    void Unflatten(const std::vector<float>& params);

    /** @brief Loads a legacy (v5/v4/v3) flat vector, widening the inputs without changing the
     * function. */
    void UnflattenLegacy(const std::vector<float>& params);

    /** @brief Saves the weights with the training iteration and the held-out selection score. */
    absl::Status SaveJson(const std::filesystem::path& path, int generation = 0,
                          float selection_city_share = 0.0F) const;

    /** @brief Loads the network weights from a JSON file. */
    absl::Status LoadJson(const std::filesystem::path& path);

    DiploNet diplo_net;             ///< Head 1: diplomacy.
    MissionNet mission_net;         ///< Head 2: stack missions.
    EconNet econ_net;               ///< Head 3: economy.
    ValueNet value_net;             ///< Head 4: the turn critic.
    StackValueNet stack_value_net;  ///< Head 5: the per-stack critic.
};

// --- Trajectory recording and stochastic exploration for PPO self-play ---

/** @brief One mission decision made during a recorded turn. */
struct MissionStepRecord {
    std::vector<MissionInVec> candidates;  ///< The inputs of every option scored.
    std::vector<float> log_mult;           ///< Log multiplicity prior of each option.
    int chosen_idx = 0;                    ///< The option taken.
    float old_log_prob = 0.0F;             ///< Its log probability when it was taken.
    bool has_stage_choice = false;         ///< Whether attack-now vs stage-near was decided.
    bool stage_chosen = false;             ///< Staging near was chosen.
    float old_stage_log_prob = 0.0F;       ///< Log probability of that choice.
    float immediate_delta_score = 0.0F;    ///< Change in empire score while this stack acted.
    StackValueInVec stack_value_in{};      ///< Group state before the decision, for the baseline.
    float stack_value_pred = 0.0F;         ///< V_stack(s) at decision time.
    float stack_return_target = 0.0F;      ///< Multi-turn local return the group achieved.
    std::vector<std::uintptr_t> unit_ids;  ///< Non-recycled Game::pool Army* IDs in the stack.
    /// Non-recycled Game::pool Army* IDs matching stack_value_in.
    std::vector<std::uintptr_t> pool_unit_ids;
    /// Own city this move left without defenders (still empty at turn end).
    int vacated_city = kNone;
    /// Filled by the trainer: that city's score weight if lost by our next turn.
    float vacated_loss = 0.0F;
    float advantage = 0.0F;  ///< Filled by the trainer.
};

/** @brief One economic decision made during a recorded turn. */
struct EconStepRecord {
    std::vector<EconInVec> candidates;  ///< The inputs of every option scored.
    std::vector<float> log_mult;        ///< Log multiplicity prior of each option.
    int chosen_idx = 0;                 ///< The option taken.
    float old_log_prob = 0.0F;          ///< Its log probability when it was taken.
};

/** @brief One diplomatic decision made during a recorded turn. */
struct DiploStepRecord {
    DiploInVec input{};                ///< The diplomacy head's input.
    int chosen_stance = 1;             ///< The stance taken (index into stance_logits).
    float old_stance_log_prob = 0.0F;  ///< Its log probability when it was taken.
    // Optional binary sub-decisions taken alongside a Peace/Ally stance
    bool has_tribute = false;             ///< Whether a tribute offer was decided (output 4).
    bool tribute_chosen = false;          ///< Gold was offered to end a war.
    float old_tribute_log_prob = 0.0F;    ///< Log probability of that choice.
    bool has_joint_war = false;           ///< Whether a joint war was decided (output 5).
    bool joint_war_chosen = false;        ///< The partner was asked to join our war.
    float old_joint_war_log_prob = 0.0F;  ///< Log probability of that choice.
};

/** @brief Everything recorded about one side's turn, for training. */
struct TurnStepRecord {
    ContextVec start_ctx{};      ///< The context as the turn begins.
    CriticVec start_critic{};    ///< Privileged critic features as the turn begins.
    float value_pred = 0.0F;     ///< The critic's prediction as the turn begins.
    float start_score = 0.0F;    ///< Empire score as the turn begins.
    float advantage = 0.0F;      ///< GAE, filled by the trainer.
    float return_target = 0.0F;  ///< Critic target, filled by the trainer.
    std::vector<MissionStepRecord> mission_steps;  ///< The turn's mission decisions.
    std::vector<EconStepRecord> econ_steps;        ///< The turn's economic decisions.
    std::vector<DiploStepRecord> diplo_steps;      ///< The turn's diplomatic decisions.
};

/** @brief Samples decisions during self-play and records them. */
struct RolloutSampler {
    uint32_t rng_state = 0x9e3779b9U;        ///< Xorshift state, separate from the game's.
    float temperature = 1.0F;                ///< Softmax temperature.
    TurnStepRecord* current_turn = nullptr;  ///< Where decisions are recorded, if anywhere.

    /** @brief The next uniform random number in [0, 1). */
    float NextUniform01();
};

/** @brief Packs (ContextVec, RivalVec) into a single DiploInVec. */
DiploInVec PackDiploInput(const ContextVec& context, const RivalVec& rival);

/** @brief Packs (ContextVec, StackVec, TargetVec) into a single MissionInVec. */
MissionInVec PackMissionInput(const ContextVec& context, const StackVec& stack,
                              const TargetVec& target);

/** @brief Packs (ContextVec, EconVec) into a single EconInVec. */
EconInVec PackEconInput(const ContextVec& context, const EconVec& econ);

/** @brief Packs (ContextVec, CriticVec) into a single ValueInVec. */
ValueInVec PackValueInput(const ContextVec& context, const CriticVec& critic);

/** @brief Packs (ContextVec, StackVec, CriticVec) into a single StackValueInVec. */
StackValueInVec PackStackValueInput(const ContextVec& context, const StackVec& stack,
                                    const CriticVec& critic);

/**
 * @brief Picks one of several options by their logits.
 *
 * Samples an index from softmax(logits / temperature) when `sampler` is
 * given, or returns the deterministic argmax when it is null. Writes
 * log(p[chosen]) to `out_log_prob` if non-null.
 */
int SelectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler,
                     float* out_log_prob = nullptr, const std::vector<float>* log_mult = nullptr);

/**
 * @brief A binary (yes/no) policy decision with logit `logit`.
 *
 * Samples Bernoulli(sigmoid(logit / temperature)) when `sampler` is given,
 * or returns `logit > 0` (the mode) when it is null. Writes the log
 * probability of the outcome to `out_log_prob` if non-null.
 */
bool SelectBinary(float logit, RolloutSampler* sampler, float* out_log_prob = nullptr);

/**
 * @brief The empire score S(game, side) whose per-turn change is the training reward.
 *
 *   S = 2 * city share + 0.5 * income share + 0.25 * army share (+ recon <= 0.1
 *       under Hidden Map), and 0 once the side is eliminated.
 *
 * Shares are of all standing cities / income / armies, so S is a hand-chosen
 * definition of "doing well" -- the one deliberate prior of the trainer. It
 * reads the true game state and is used only for offline training rewards,
 * never as a policy input.
 */
float ComputeEmpireScore(const Game& game, int side_index);

/** @brief The global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& ActivePolicy();

/** @brief Builds the fair-information context vector (global state + personality) for `side`. */
ContextVec BuildContext(const Game& game, const Side& side);

/**
 * @brief Builds the privileged critic features for `side` from the true game state.
 *
 * Training-only information: fed to ValueNet / StackValueNet, never to a
 * policy head.
 */
CriticVec BuildCriticFeatures(const Game& game, const Side& side);

}  // namespace w2::ai::ml
