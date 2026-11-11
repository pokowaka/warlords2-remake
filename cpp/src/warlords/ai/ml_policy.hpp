/**
 * @file
 * @brief The learnable AI's neural policy and value networks.
 *
 * Scores strategic, operational, diplomatic and economic macro-actions with
 * small multilayer perceptrons that have a linear skip connection, each
 * conditioned on the context vector (global state, then personality/posture).
 * The networks start from small zero-mean random weights, with no hand-coded
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
// cities, budget balance). Personality / v7 setup+posture channels follow.
constexpr int kLegacyGlobalDim = 14;  ///< Global features of weights v5 and older.
constexpr int kAddedGlobalDim = 6;    ///< Global features added in weights v6.
constexpr int kGlobalDim = kLegacyGlobalDim + kAddedGlobalDim;    ///< 20 global features.
constexpr int kContextDim = kGlobalDim + personality::kTraitDim;  ///< 30: global + personality.

constexpr int kRivalDim = 18;                         ///< Features describing one rival.
constexpr int kDiploInDim = kContextDim + kRivalDim;  ///< 48: diplomacy head input.
constexpr int kDiploOutDim = 6;                       ///< Diplomacy head outputs.

constexpr int kStackDim = 16;         ///< Features describing one of our stacks.
constexpr int kV6TargetDim = 21;      ///< Target features in weights v6.
constexpr int kAddedV7TargetDim = 4;  ///< v7 target features (order memory & staging).
constexpr int kV7TargetDim = kV6TargetDim + kAddedV7TargetDim;  ///< 25: v7 target features.
constexpr int kAddedV8TargetDim = 4;                            ///< v8 muster target features.
constexpr int kAddedTargetDim =
        kAddedV7TargetDim + kAddedV8TargetDim;              ///< 8 added target features.
constexpr int kTargetDim = kV6TargetDim + kAddedTargetDim;  ///< 29: mission target features.
constexpr int kMissionInDim = kContextDim + kStackDim + kTargetDim;  ///< 75: mission head input.
constexpr int kMissionOutDim = 2;                                    ///< Mission head outputs.

// EconVec layout: [0..6] action one-hot (HoldGold, ProduceSlot, StopProd, BuySlot,
// VectorCity, RetainArmy, CaptureSpoils); [7..19] unit/cost/city features;
// [20] "is the current choice" (the slot already building, the vector already
// set), so the policy can see what switching would throw away;
// v7 additions: [21] GuardCity >=1 defender indicator, [22] GuardCity >=2 indicator;
// v8 additions: [23] dstIsMusterHub, [24] dstMusterNeed, [25] dstHeroAndStreams.
constexpr int kV6EconFeatDim = 21;                                ///< Economy features in v6.
constexpr int kAddedV7EconDim = 2;                                ///< GuardCity features in v7.
constexpr int kV7EconFeatDim = kV6EconFeatDim + kAddedV7EconDim;  ///< 23: v7 economy features.
constexpr int kAddedV8EconDim = 3;                                ///< Muster vector features in v8.
constexpr int kAddedEconDim = kAddedV7EconDim + kAddedV8EconDim;  ///< 5 added economy features.
constexpr int kEconFeatDim =
        kV6EconFeatDim + kAddedEconDim;                 ///< 26: features of one economic option.
constexpr int kEconCurrent = 20;                        ///< Index of "is the current choice".
constexpr int kEconGuard1 = 21;                         ///< GuardCity >= 1 defender indicator.
constexpr int kEconGuard2 = 22;                         ///< GuardCity >= 2 defender indicator.
constexpr int kEconMusterHub = 23;                      ///< Vector destination is Muster Hub.
constexpr int kEconMusterNeed = 24;                     ///< Vector destination Muster Hub need.
constexpr int kEconMusterFlow = 25;                     ///< Vector destination Muster Hub flow.
constexpr int kEconInDim = kContextDim + kEconFeatDim;  ///< 56: economy head input.
constexpr int kEconOutDim = 1;                          ///< Economy head outputs.

constexpr int kMusterFeatDim = 14;  ///< Features describing one city for MusterNet.
constexpr int kMusterInDim = kContextDim + kMusterFeatDim;  ///< 44: muster head input.
constexpr int kMusterOutDim = 1;                            ///< Muster head outputs.

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
using MusterVec = std::array<float, kMusterFeatDim>;          ///< One city for the muster head.
using MusterInVec = std::array<float, kMusterInDim>;          ///< Muster head input.
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

    /** @brief Hidden layer activations preserved for fast backpropagation. */
    struct Activations {
        std::array<float, H1Dim> hidden1{};  ///< First hidden layer tanh activations.
        std::array<float, H2Dim> hidden2{};  ///< Second hidden layer tanh activations.
    };

    /** @brief Evaluates the network on an input. */
    std::array<float, OutDim> Forward(const std::array<float, InDim>& input) const;

    /** @brief Evaluates the network on an input and preserves hidden activations for backprop. */
    std::array<float, OutDim> ForwardWithActivations(const std::array<float, InDim>& input,
                                                     Activations& act) const;

    /**
     * @brief Accumulates the exact parameter gradients dL/dW and dL/db into
     * `grad_accum`, given the input and the output gradient `d_out` = dL/dy.
     */
    void Backward(const std::array<float, InDim>& input, const std::array<float, OutDim>& d_out,
                  ResidualMLP& grad_accum) const;

    /**
     * @brief Accumulates parameter gradients using cached hidden activations, avoiding
     * recomputing forward matrix multiplications during training passes.
     */
    void BackwardWithActivations(const std::array<float, InDim>& input, const Activations& act,
                                 const std::array<float, OutDim>& d_out,
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
 * @brief The learnable AI's networks: policy heads, muster head, archetype specialists, and
 * critics.
 *
 * Diplomacy, mission, economy and muster heads choose what to do; the value and
 * per-stack value critics are used only in training.
 */
class PolicyNetwork {
  public:
    static constexpr int kGateInDim = 25;
    static constexpr size_t kCommanderWeights = 5U * static_cast<size_t>(kGateInDim);
    static constexpr int kMaxExperts = 4;
    using DiploNet = ResidualMLP<kDiploInDim, 24, 12, kDiploOutDim>;            ///< Head 1.
    using MissionNet = ResidualMLP<kMissionInDim, 32, 16, kMissionOutDim>;      ///< Head 2.
    using EconNet = ResidualMLP<kEconInDim, 24, 12, kEconOutDim>;               ///< Head 3.
    using ValueNet = ResidualMLP<kValueInDim, 24, 12, kValueOutDim>;            ///< Head 4.
    using StackValueNet = ResidualMLP<kStackValueInDim, 24, 12, kValueOutDim>;  ///< Head 5.
    using GateNet = ResidualMLP<kGateInDim, 10, 6, kMaxExperts>;                ///< MoE gate.
    using MusterNet = ResidualMLP<kMusterInDim, 12, 6, kMusterOutDim>;  ///< Muster hub head.

    /** @brief One expert's actor heads for optional Mixture-of-Experts routing. */
    struct ExpertHeads {
        int version = 6;
        bool use_commander = false;
        std::array<float, kCommanderWeights> commander_w{};
        std::array<float, 5> commander_b{};
        DiploNet diplo_net{};
        MissionNet mission_net{};
        EconNet econ_net{};
    };

    /** @brief Specialist readout & skip parameters (`w_skip`, `w3`, `b3`) for one evolved
     * archetype. */
    struct ArchetypeHead {
        static constexpr int kParamCount =
                (DiploNet::kOutDim * DiploNet::kInDim + DiploNet::kOutDim * DiploNet::kH2Dim +
                 DiploNet::kOutDim) +
                (MissionNet::kOutDim * MissionNet::kInDim +
                 MissionNet::kOutDim * MissionNet::kH2Dim + MissionNet::kOutDim) +
                (EconNet::kOutDim * EconNet::kInDim + EconNet::kOutDim * EconNet::kH2Dim +
                 EconNet::kOutDim) +
                (MusterNet::kOutDim * MusterNet::kInDim + MusterNet::kOutDim * MusterNet::kH2Dim +
                 MusterNet::kOutDim);

        bool use_muster = false;
        bool use_standard = false;
        bool avoid_blockers = false;
        float detour_bias = -0.05F;
        std::array<float, kParamCount> params{};
    };

    /// Parameters of all five core heads, the current (v8) file format.
    static constexpr int kTotalParams = DiploNet::kParamCount + MissionNet::kParamCount +
                                        EconNet::kParamCount + ValueNet::kParamCount +
                                        StackValueNet::kParamCount;

    // Legacy files: v7 lacks the 4 muster target inputs and 3 muster econ inputs;
    // v6 also lacks the 4 order-memory target inputs and 2 GuardCity econ inputs;
    // v5 also lacks the 6 added context features; v4/v3 also lack privileged critics.
    static constexpr int kV5Dropped = kAddedGlobalDim;  ///< Context inputs v5 lacks.
    /// Critic inputs v4 and v3 lack.
    static constexpr int kV4CriticDropped = kAddedGlobalDim + kCriticDim;
    /// Parameters in a v7 file.
    static constexpr int kLegacyV7Params =
            DiploNet::kParamCount +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kAddedV8TargetDim) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kAddedV8EconDim) + ValueNet::kParamCount +
            StackValueNet::kParamCount;
    /// Parameters in a v6 file.
    static constexpr int kLegacyV6Params =
            DiploNet::kParamCount +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kAddedTargetDim) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kAddedEconDim) + ValueNet::kParamCount +
            StackValueNet::kParamCount;
    /// Parameters in a v5 file.
    static constexpr int kLegacyV5Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kAddedTargetDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kAddedEconDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV5Dropped);
    /// Parameters in a v3 file.
    static constexpr int kLegacyV3Params =
            MlpParamsWithInput<DiploNet>(DiploNet::kInDim - kV5Dropped) +
            MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kAddedTargetDim - kV5Dropped) +
            MlpParamsWithInput<EconNet>(EconNet::kInDim - kAddedEconDim - kV5Dropped) +
            MlpParamsWithInput<ValueNet>(ValueNet::kInDim - kV4CriticDropped);
    /// Parameters in a v4 file.
    static constexpr int kLegacyV4Params =
            kLegacyV3Params +
            MlpParamsWithInput<StackValueNet>(StackValueNet::kInDim - kV4CriticDropped);

    /** @brief Initializes all heads with default random weights (seed 42). */
    PolicyNetwork();

    /** @brief Initializes all parameters from zero-mean random noise (no human priors). */
    void InitRandomWeights(uint32_t seed = 42);

    /** @brief Zeros all weights and biases (used when this instance acts as a gradient buffer). */
    void Zero();

    /** @brief Raw forward passes (dispatching through MoE GateNet when `experts_` is non-empty). */
    std::array<float, kDiploOutDim> ForwardDiplo(const DiploInVec& input) const;
    std::array<float, kMissionOutDim> ForwardMission(const MissionInVec& input) const;
    std::array<float, kEconOutDim> ForwardEcon(const EconInVec& input) const;
    std::array<float, kMusterOutDim> ForwardMuster(const MusterInVec& input) const;

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

    /** @brief Flattens all five core heads into a single vector of kTotalParams values. */
    std::vector<float> Flatten() const;

    /** @brief Loads parameters from a flat vector of kTotalParams values. */
    void Unflatten(const std::vector<float>& params);

    /**
     * @brief Loads a legacy (v7/v6/v5/v4/v3) flat vector, widening the inputs without changing the
     * function.
     */
    void UnflattenLegacy(const std::vector<float>& params);

    /**
     * @brief Upgrades a loaded legacy (< v7) checkpoint to v7 in-place, zeroing the previously
     * unused `ctx[20..29]` columns and initializing GuardCity skip weights mildly negative so
     * initial greedy behaviour matches v6.
     */
    void UpgradeToV7();

    /** @brief Upgrades a loaded checkpoint to v8 (Muster Hub support) in-place. */
    void UpgradeToV8();

    /** @brief Saves the weights with the training iteration and the held-out selection score. */
    absl::Status SaveJson(const std::filesystem::path& path, int generation = 0,
                          float selection_city_share = 0.0F) const;

    /** @brief Loads the network weights from a JSON file. */
    absl::Status LoadJson(const std::filesystem::path& path);

    /** @brief Extracts this network's affine readout & skip parameters as an `ArchetypeHead`. */
    ArchetypeHead ExtractArchetypeHead() const;

    /** @brief Applies an `ArchetypeHead` onto this network's shared hidden trunk in-place. */
    void ApplyArchetypeHead(const ArchetypeHead& head);

    /** @brief Materializes an exact product-of-experts specialist blend over `archetypes_` for
     * `weights`. */
    PolicyNetwork MaterializeForWeights(const personality::ArchetypeWeightArray& weights) const;

    /** @brief Materializes the personality-blended policy for `side` (returns `*this` when
     * `use_personalities_` is false). */
    PolicyNetwork MaterializeForSide(const Side& side) const;

    /** @brief Returns Head 1 (diplomacy MLP). */
    DiploNet& DiploHead() { return diplo_net_; }
    /** @brief Returns Head 1 (diplomacy MLP, const). */
    const DiploNet& DiploHead() const { return diplo_net_; }

    /** @brief Returns Head 2 (stack mission MLP). */
    MissionNet& MissionHead() { return mission_net_; }
    /** @brief Returns Head 2 (stack mission MLP, const). */
    const MissionNet& MissionHead() const { return mission_net_; }

    /** @brief Returns Head 3 (economy MLP). */
    EconNet& EconHead() { return econ_net_; }
    /** @brief Returns Head 3 (economy MLP, const). */
    const EconNet& EconHead() const { return econ_net_; }

    /** @brief Returns Head 4 (turn value critic MLP). */
    ValueNet& ValueHead() { return value_net_; }
    /** @brief Returns Head 4 (turn value critic MLP, const). */
    const ValueNet& ValueHead() const { return value_net_; }

    /** @brief Returns Head 5 (per-stack value critic MLP). */
    StackValueNet& StackValueHead() { return stack_value_net_; }
    /** @brief Returns Head 5 (per-stack value critic MLP, const). */
    const StackValueNet& StackValueHead() const { return stack_value_net_; }

    /** @brief Returns the Muster Hub selection MLP. */
    MusterNet& MusterHead() { return muster_net_; }
    /** @brief Returns the Muster Hub selection MLP (const). */
    const MusterNet& MusterHead() const { return muster_net_; }

    /** @brief Returns the Mixture-of-Experts gating MLP. */
    GateNet& GateHead() { return gate_net_; }
    /** @brief Returns the Mixture-of-Experts gating MLP (const). */
    const GateNet& GateHead() const { return gate_net_; }

    /** @brief Returns the Mixture-of-Experts specialist actor list. */
    std::vector<ExpertHeads>& Experts() { return experts_; }
    /** @brief Returns the Mixture-of-Experts specialist actor list (const). */
    const std::vector<ExpertHeads>& Experts() const { return experts_; }

    /** @brief Returns the checkpoint format version (`3..8`). */
    int Version() const { return version_; }
    /** @brief Sets the checkpoint format version. */
    void SetVersion(int version) { version_ = version; }

    /** @brief True when the pre-wave GuardCity economy pass is enabled. */
    bool EnableCityGuard() const { return enable_city_guard_; }
    /** @brief Sets whether the pre-wave GuardCity economy pass is enabled. */
    void SetEnableCityGuard(bool enabled) { enable_city_guard_ = enabled; }

    /** @brief True when the learned Commander maps `ctx[0..24]` into `ctx[25..29]`. */
    bool UseCommander() const { return use_commander_; }
    /** @brief Sets whether the learned Commander is active. */
    void SetUseCommander(bool enabled) { use_commander_ = enabled; }

    /** @brief Returns the learned Commander weight matrix (`5 x 25`). */
    std::array<float, kCommanderWeights>& CommanderW() { return commander_w_; }
    /** @brief Returns the learned Commander weight matrix (`5 x 25`, const). */
    const std::array<float, kCommanderWeights>& CommanderW() const { return commander_w_; }

    /** @brief Returns the learned Commander bias vector (`5`). */
    std::array<float, 5>& CommanderB() { return commander_b_; }
    /** @brief Returns the learned Commander bias vector (`5`, const). */
    const std::array<float, 5>& CommanderB() const { return commander_b_; }

    /** @brief Returns the fixed/override posture channels (`ctx[25..29]`). */
    std::array<float, 5>& Posture() { return posture_; }
    /** @brief Returns the fixed/override posture channels (`ctx[25..29]`, const). */
    const std::array<float, 5>& Posture() const { return posture_; }

    /** @brief True when learned Muster Hub selection (`MusterNet`) is enabled. */
    bool UseMuster() const { return use_muster_; }
    /** @brief Sets whether learned Muster Hub selection is enabled. */
    void SetUseMuster(bool enabled) { use_muster_ = enabled; }

    /** @brief True when learned Side Standard planting/vectoring is enabled. */
    bool UseStandard() const { return use_standard_; }
    /** @brief Sets whether learned Side Standard planting/vectoring is enabled. */
    void SetUseStandard(bool enabled) { use_standard_ = enabled; }

    /** @brief True when blocked stacks stop and re-evaluate instead of auto-attacking blockers. */
    bool YieldToBlockers() const { return yield_to_blockers_; }
    /** @brief Sets whether blocked stacks yield to non-target blockers. */
    void SetYieldToBlockers(bool enabled) { yield_to_blockers_ = enabled; }

    /** @brief True when obstacle-aware detour scoring around visible hostile stacks is enabled. */
    bool AvoidBlockers() const { return avoid_blockers_; }
    /** @brief Sets whether obstacle-aware detour scoring is enabled. */
    void SetAvoidBlockers(bool enabled) { avoid_blockers_ = enabled; }

    /** @brief Returns the utility bias added to clear detour routes (`u_detour + bias >
     * u_through`). */
    float DetourBias() const { return detour_bias_; }
    /** @brief Sets the utility bias added to clear detour routes. */
    void SetDetourBias(float bias) { detour_bias_ = bias; }

    /** @brief True when the 27-card evolved personality mixer is enabled. */
    bool UsePersonalities() const { return use_personalities_; }
    /** @brief Sets whether the 27-card evolved personality mixer is enabled. */
    void SetUsePersonalities(bool enabled) { use_personalities_ = enabled; }

    /** @brief True when 5 evolved specialist `ArchetypeHead`s are loaded. */
    bool HasArchetypes() const { return has_archetypes_; }
    /** @brief Sets whether 5 evolved specialist `ArchetypeHead`s are loaded. */
    void SetHasArchetypes(bool enabled) { has_archetypes_ = enabled; }

    /** @brief True when 1-ply Warlord tactical lookahead is enabled. */
    bool WarlordLookahead() const { return warlord_lookahead_; }
    /** @brief Sets whether 1-ply Warlord tactical lookahead is enabled. */
    void SetWarlordLookahead(bool enabled) { warlord_lookahead_ = enabled; }

    /** @brief Returns the 5 evolved specialist `ArchetypeHead`s. */
    std::array<ArchetypeHead, personality::kNumArchetypes>& Archetypes() { return archetypes_; }
    /** @brief Returns the 5 evolved specialist `ArchetypeHead`s (const). */
    const std::array<ArchetypeHead, personality::kNumArchetypes>& Archetypes() const {
        return archetypes_;
    }

  private:
    DiploNet diplo_net_;             ///< Head 1: diplomacy.
    MissionNet mission_net_;         ///< Head 2: stack missions.
    EconNet econ_net_;               ///< Head 3: economy.
    ValueNet value_net_;             ///< Head 4: the turn critic.
    StackValueNet stack_value_net_;  ///< Head 5: the per-stack critic.

    int version_ = 8;
    bool enable_city_guard_ = false;
    bool use_commander_ = false;
    std::array<float, kCommanderWeights> commander_w_{};
    std::array<float, 5> commander_b_{};
    std::array<float, 5> posture_{};

    bool use_muster_ = false;
    bool use_standard_ = false;
    MusterNet muster_net_{};

    GateNet gate_net_{};
    std::vector<ExpertHeads> experts_;

    bool yield_to_blockers_ = false;
    bool avoid_blockers_ = false;
    float detour_bias_ = -0.05F;

    bool use_personalities_ = false;
    bool has_archetypes_ = false;
    bool warlord_lookahead_ = false;
    std::array<ArchetypeHead, personality::kNumArchetypes> archetypes_{};
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
    std::vector<std::uintptr_t> unit_ids;  ///< Non-recycled Army IDs in the stack.
    /// Non-recycled Army IDs matching stack_value_in.
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
    int guard_city = kNone;             ///< v7: owned city whose garrison GuardCity decided.
    float guard_loss = 0.0F;            ///< Filled by trainer if guard_city fell by next turn.
    float advantage = 0.0F;             ///< Filled by trainer.
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

/** @brief Packs (ContextVec, MusterVec) into a single MusterInVec. */
MusterInVec PackMusterInput(const ContextVec& context, const MusterVec& muster);

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

/**
 * @brief Sets the weight of the army-power share in `ComputeEmpireScore` (default `0.25`).
 *
 * Set once before training starts.
 */
void SetArmyScoreWeight(float weight);

/** @brief Whether human-readable `AIThought` strings are constructed during AI turns (default
 * `true`). */
bool RecordThoughts();

/** @brief Enables or disables `AIThought` string construction (disabled in headless trainers). */
void SetRecordThoughts(bool enabled);

/** @brief The global active PolicyNetwork used by computer players in the game. */
PolicyNetwork& ActivePolicy();

/** @brief Builds the fair-information context vector (global state + personality/posture) for
 * `side`. */
ContextVec BuildContext(const Game& game, const Side& side, const PolicyNetwork* policy = nullptr);

/**
 * @brief Builds the privileged critic features for `side` from the true game state.
 *
 * Training-only information: fed to ValueNet / StackValueNet, never to a
 * policy head.
 */
CriticVec BuildCriticFeatures(const Game& game, const Side& side);

}  // namespace w2::ai::ml
