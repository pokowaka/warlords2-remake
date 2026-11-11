#include "warlords/ai/ml_policy.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <sstream>

#include "absl/base/no_destructor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "nlohmann/json.hpp"
#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"

namespace w2::ai::ml {

namespace {

inline float DotProduct(const float* lhs, const float* rhs, int n) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t acc0 = vdupq_n_f32(0.0F);
    float32x4_t acc1 = vdupq_n_f32(0.0F);
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        acc0 = vmlaq_f32(acc0, vld1q_f32(lhs + idx), vld1q_f32(rhs + idx));
        acc1 = vmlaq_f32(acc1, vld1q_f32(lhs + idx + 4), vld1q_f32(rhs + idx + 4));
    }
    acc0 = vaddq_f32(acc0, acc1);
    float sum = vaddvq_f32(acc0);
    for (; idx < n; ++idx) {
        sum += lhs[idx] * rhs[idx];
    }
    return sum;
#elif defined(__AVX2__)
    __m256 acc = _mm256_setzero_ps();
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(lhs + idx), _mm256_loadu_ps(rhs + idx), acc);
    }
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum128 = _mm_add_ps(lo, hi);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    float sum = _mm_cvtss_f32(sum128);
    for (; idx < n; ++idx) {
        sum += lhs[idx] * rhs[idx];
    }
    return sum;
#elif defined(__SSE2__)
    __m128 acc0 = _mm_setzero_ps();
    __m128 acc1 = _mm_setzero_ps();
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(lhs + idx), _mm_loadu_ps(rhs + idx)));
        acc1 = _mm_add_ps(acc1,
                          _mm_mul_ps(_mm_loadu_ps(lhs + idx + 4), _mm_loadu_ps(rhs + idx + 4)));
    }
    acc0 = _mm_add_ps(acc0, acc1);
    alignas(16) float buf[4];
    _mm_store_ps(buf, acc0);
    float sum = buf[0] + buf[1] + buf[2] + buf[3];
    for (; idx < n; ++idx) {
        sum += lhs[idx] * rhs[idx];
    }
    return sum;
#else
    float sum0 = 0.0F;
    float sum1 = 0.0F;
    float sum2 = 0.0F;
    float sum3 = 0.0F;
    int idx = 0;
    for (; idx + 3 < n; idx += 4) {
        sum0 += lhs[idx] * rhs[idx];
        sum1 += lhs[idx + 1] * rhs[idx + 1];
        sum2 += lhs[idx + 2] * rhs[idx + 2];
        sum3 += lhs[idx + 3] * rhs[idx + 3];
    }
    float sum = (sum0 + sum1) + (sum2 + sum3);
    for (; idx < n; ++idx) {
        sum += lhs[idx] * rhs[idx];
    }
    return sum;
#endif
}

inline void Axpy(float* y, float alpha, const float* x, int n) {
    if (alpha == 0.0F) return;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t alpha_vec = vdupq_n_f32(alpha);
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        float32x4_t dst0 = vld1q_f32(y + idx);
        float32x4_t src0 = vld1q_f32(x + idx);
        vst1q_f32(y + idx, vmlaq_f32(dst0, alpha_vec, src0));

        float32x4_t dst1 = vld1q_f32(y + idx + 4);
        float32x4_t src1 = vld1q_f32(x + idx + 4);
        vst1q_f32(y + idx + 4, vmlaq_f32(dst1, alpha_vec, src1));
    }
    for (; idx < n; ++idx) {
        y[idx] += alpha * x[idx];
    }
#elif defined(__AVX2__)
    __m256 alpha_vec = _mm256_set1_ps(alpha);
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        __m256 y_vec = _mm256_loadu_ps(y + idx);
        __m256 x_vec = _mm256_loadu_ps(x + idx);
        _mm256_storeu_ps(y + idx, _mm256_fmadd_ps(alpha_vec, x_vec, y_vec));
    }
    for (; idx < n; ++idx) {
        y[idx] += alpha * x[idx];
    }
#elif defined(__SSE2__)
    __m128 alpha_vec = _mm_set1_ps(alpha);
    int idx = 0;
    for (; idx + 7 < n; idx += 8) {
        __m128 dst0 = _mm_loadu_ps(y + idx);
        __m128 src0 = _mm_loadu_ps(x + idx);
        _mm_storeu_ps(y + idx, _mm_add_ps(dst0, _mm_mul_ps(alpha_vec, src0)));

        __m128 dst1 = _mm_loadu_ps(y + idx + 4);
        __m128 src1 = _mm_loadu_ps(x + idx + 4);
        _mm_storeu_ps(y + idx + 4, _mm_add_ps(dst1, _mm_mul_ps(alpha_vec, src1)));
    }
    for (; idx < n; ++idx) {
        y[idx] += alpha * x[idx];
    }
#else
    for (int j = 0; j < n; ++j) {
        y[j] += alpha * x[j];
    }
#endif
}

}  // namespace

template <int InDim, int H1Dim, int H2Dim, int OutDim>
std::array<float, OutDim> ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::ForwardWithActivations(
        const std::array<float, InDim>& input, Activations& act) const {
    for (int i = 0; i < H1Dim; i++) {
        const float* row = &w1[static_cast<size_t>(i) * InDim];
        float sum = b1[i] + DotProduct(row, input.data(), InDim);
        act.hidden1[i] = std::tanh(sum);
    }

    for (int i = 0; i < H2Dim; i++) {
        const float* row = &w2[static_cast<size_t>(i) * H1Dim];
        float sum = b2[i] + DotProduct(row, act.hidden1.data(), H1Dim);
        act.hidden2[i] = std::tanh(sum);
    }

    std::array<float, OutDim> out{};
    for (int i = 0; i < OutDim; i++) {
        const float* skip_row = &w_skip[static_cast<size_t>(i) * InDim];
        const float* row3 = &w3[static_cast<size_t>(i) * H2Dim];
        out[i] = b3[i] + DotProduct(skip_row, input.data(), InDim) +
                 DotProduct(row3, act.hidden2.data(), H2Dim);
    }
    return out;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
std::array<float, OutDim> ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Forward(
        const std::array<float, InDim>& input) const {
    Activations act;
    return ForwardWithActivations(input, act);
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::BackwardWithActivations(
        const std::array<float, InDim>& input, const Activations& act,
        const std::array<float, OutDim>& d_out, ResidualMLP& grad_accum,
        std::array<float, 5>* d_posture) const {
    const auto& hidden1 = act.hidden1;
    const auto& hidden2 = act.hidden2;

    for (int out_idx = 0; out_idx < OutDim; out_idx++) {
        float grad_out = d_out[out_idx];
        if (grad_out == 0.0F) continue;
        grad_accum.b3[out_idx] += grad_out;
        float* grad_skip = &grad_accum.w_skip[static_cast<size_t>(out_idx) * InDim];
        Axpy(grad_skip, grad_out, input.data(), InDim);
        float* grad_w3 = &grad_accum.w3[static_cast<size_t>(out_idx) * H2Dim];
        Axpy(grad_w3, grad_out, hidden2.data(), H2Dim);
        if constexpr (InDim >= kContextDim) {
            if (d_posture != nullptr) {
                const float* skip_row = &w_skip[static_cast<size_t>(out_idx) * InDim + 25];
                for (int post_idx = 0; post_idx < 5; post_idx++) {
                    (*d_posture)[post_idx] += grad_out * skip_row[post_idx];
                }
            }
        }
    }

    std::array<float, H2Dim> grad_z2{};
    for (int i = 0; i < H2Dim; i++) {
        float grad_hidden2 = 0.0F;
        for (int out_idx = 0; out_idx < OutDim; out_idx++) {
            grad_hidden2 += d_out[out_idx] * w3[static_cast<size_t>(out_idx) * H2Dim + i];
        }
        float grad2 = grad_hidden2 * (1.0F - hidden2[i] * hidden2[i]);
        grad_z2[i] = grad2;
        grad_accum.b2[i] += grad2;
        float* grad_w2 = &grad_accum.w2[static_cast<size_t>(i) * H1Dim];
        Axpy(grad_w2, grad2, hidden1.data(), H1Dim);
    }

    std::array<float, H1Dim> grad_hidden1{};
    for (int i = 0; i < H2Dim; i++) {
        const float* row_w2 = &w2[static_cast<size_t>(i) * H1Dim];
        Axpy(grad_hidden1.data(), grad_z2[i], row_w2, H1Dim);
    }

    for (int j = 0; j < H1Dim; j++) {
        float grad1 = grad_hidden1[j] * (1.0F - hidden1[j] * hidden1[j]);
        grad_accum.b1[j] += grad1;
        float* grad_w1 = &grad_accum.w1[static_cast<size_t>(j) * InDim];
        Axpy(grad_w1, grad1, input.data(), InDim);
        if constexpr (InDim >= kContextDim) {
            if (d_posture != nullptr) {
                const float* row_w1 = &w1[static_cast<size_t>(j) * InDim + 25];
                for (int post_idx = 0; post_idx < 5; post_idx++) {
                    (*d_posture)[post_idx] += grad1 * row_w1[post_idx];
                }
            }
        }
    }
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Backward(const std::array<float, InDim>& input,
                                                        const std::array<float, OutDim>& d_out,
                                                        ResidualMLP& grad_accum) const {
    Activations act;
    for (int i = 0; i < H1Dim; i++) {
        const float* row = &w1[static_cast<size_t>(i) * InDim];
        float sum = b1[i] + DotProduct(row, input.data(), InDim);
        act.hidden1[i] = std::tanh(sum);
    }
    for (int i = 0; i < H2Dim; i++) {
        const float* row = &w2[static_cast<size_t>(i) * H1Dim];
        float sum = b2[i] + DotProduct(row, act.hidden1.data(), H1Dim);
        act.hidden2[i] = std::tanh(sum);
    }
    BackwardWithActivations(input, act, d_out, grad_accum);
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Zero() {
    w_skip.fill(0.0F);
    w1.fill(0.0F);
    b1.fill(0.0F);
    w2.fill(0.0F);
    b2.fill(0.0F);
    w3.fill(0.0F);
    b3.fill(0.0F);
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::ToFlat(std::vector<float>& out) const {
    out.insert(out.end(), w_skip.begin(), w_skip.end());
    out.insert(out.end(), w1.begin(), w1.end());
    out.insert(out.end(), b1.begin(), b1.end());
    out.insert(out.end(), w2.begin(), w2.end());
    out.insert(out.end(), b2.begin(), b2.end());
    out.insert(out.end(), w3.begin(), w3.end());
    out.insert(out.end(), b3.begin(), b3.end());
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::FromFlat(const std::vector<float>& flat,
                                                          size_t offset) {
    auto copy_slice = [&](auto& arr) {
        for (size_t i = 0; i < arr.size() && offset < flat.size(); i++) {
            arr[i] = flat[offset++];
        }
    };
    copy_slice(w_skip);
    copy_slice(w1);
    copy_slice(b1);
    copy_slice(w2);
    copy_slice(b2);
    copy_slice(w3);
    copy_slice(b3);
    return offset;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::FromFlatWidenInput(const std::vector<float>& flat,
                                                                    size_t offset, int old_in_dim,
                                                                    int insert_at,
                                                                    int insert_count) {
    auto next = [&]() { return offset < flat.size() ? flat[offset++] : 0.0F; };
    auto copy_matrix = [&](auto& arr, int rows) {
        arr.fill(0.0F);
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < old_in_dim; col++) {
                float weight = next();
                int new_col = col < insert_at ? col : col + insert_count;
                if (new_col < InDim) {
                    arr[static_cast<size_t>(row) * InDim + new_col] = weight;
                }
            }
        }
    };
    auto copy_slice = [&](auto& arr) {
        for (float& weight : arr) weight = next();
    };
    copy_matrix(w_skip, OutDim);
    copy_matrix(w1, H1Dim);
    copy_slice(b1);
    copy_slice(w2);
    copy_slice(b2);
    copy_slice(w3);
    copy_slice(b3);
    return offset;
}

template struct ResidualMLP<kDiploInDim, 24, 12, kDiploOutDim>;
template struct ResidualMLP<kMissionInDim, 32, 16, kMissionOutDim>;
template struct ResidualMLP<kEconInDim, 24, 12, kEconOutDim>;
template struct ResidualMLP<kValueInDim, 24, 12, kValueOutDim>;
template struct ResidualMLP<kStackValueInDim, 24, 12, kValueOutDim>;
template struct ResidualMLP<PolicyNetwork::kGateInDim, 10, 6, PolicyNetwork::kMaxExperts>;
template struct ResidualMLP<kMusterInDim, 12, 6, kMusterOutDim>;

PolicyNetwork::PolicyNetwork() {
    InitRandomWeights(42);
}

void PolicyNetwork::InitRandomWeights(uint32_t seed) {
    version_ = 8;
    uint32_t state = seed ? seed : 1U;
    auto next_sym = [&]() -> float {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        float uniform =
                static_cast<float>(state & 0x00ffffffU) / static_cast<float>(0x00800000U) - 1.0F;
        return uniform;
    };
    auto init_arr = [&](auto& arr, float scale) {
        for (float& weight : arr) {
            weight = 0.5F * (next_sym() + next_sym()) * scale;
        }
    };

    auto init_mlp = [&](auto& mlp) {
        using MlpT = std::decay_t<decltype(mlp)>;
        init_arr(mlp.w_skip, 0.04F);
        init_arr(mlp.w1, 1.0F / std::sqrt(static_cast<float>(MlpT::kInDim)));
        mlp.b1.fill(0.0F);
        init_arr(mlp.w2, 1.0F / std::sqrt(static_cast<float>(MlpT::kH1Dim)));
        mlp.b2.fill(0.0F);
        init_arr(mlp.w3, 0.2F / std::sqrt(static_cast<float>(MlpT::kH2Dim)));
        mlp.b3.fill(0.0F);
    };

    init_mlp(diplo_net_);
    init_mlp(mission_net_);
    init_mlp(econ_net_);
    init_mlp(value_net_);
    init_mlp(stack_value_net_);
    muster_net_.Zero();
}

void PolicyNetwork::Zero() {
    diplo_net_.Zero();
    mission_net_.Zero();
    econ_net_.Zero();
    value_net_.Zero();
    stack_value_net_.Zero();
    muster_net_.Zero();
    commander_w_.fill(0.0F);
    commander_b_.fill(0.0F);
    detour_bias_ = 0.0F;
}

DiploInVec PackDiploInput(const ContextVec& context, const RivalVec& rival) {
    DiploInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(rival.begin(), rival.end(), packed.begin() + kContextDim);
    return packed;
}

MissionInVec PackMissionInput(const ContextVec& context, const StackVec& stack,
                              const TargetVec& target) {
    MissionInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(stack.begin(), stack.end(), packed.begin() + kContextDim);
    std::copy(target.begin(), target.end(), packed.begin() + kContextDim + kStackDim);
    return packed;
}

EconInVec PackEconInput(const ContextVec& context, const EconVec& econ) {
    EconInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(econ.begin(), econ.end(), packed.begin() + kContextDim);
    return packed;
}

MusterInVec PackMusterInput(const ContextVec& context, const MusterVec& muster) {
    MusterInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(muster.begin(), muster.end(), packed.begin() + kContextDim);
    return packed;
}

ValueInVec PackValueInput(const ContextVec& context, const CriticVec& critic) {
    ValueInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(critic.begin(), critic.end(), packed.begin() + kContextDim);
    return packed;
}

StackValueInVec PackStackValueInput(const ContextVec& context, const StackVec& stack,
                                    const CriticVec& critic) {
    StackValueInVec packed{};
    std::copy(context.begin(), context.end(), packed.begin());
    std::copy(stack.begin(), stack.end(), packed.begin() + kContextDim);
    std::copy(critic.begin(), critic.end(), packed.begin() + kContextDim + kStackDim);
    return packed;
}

namespace {

template <size_t DimN>
std::array<float, PolicyNetwork::kMaxExperts> ComputeExpertGates(
        const PolicyNetwork& net, const std::array<float, DimN>& input) {
    std::array<float, PolicyNetwork::kGateInDim> gate_in{};
    for (int i = 0; i < PolicyNetwork::kGateInDim; i++) gate_in[i] = input[i];
    auto raw = net.GateHead().Forward(gate_in);
    int num_exp = std::min(static_cast<int>(net.Experts().size()), PolicyNetwork::kMaxExperts);
    float max_logit = -1e30F;
    for (int i = 0; i < num_exp; i++) max_logit = std::max(max_logit, raw[i]);
    std::array<float, PolicyNetwork::kMaxExperts> gates{};
    float sum = 0.0F;
    for (int i = 0; i < num_exp; i++) {
        gates[i] = std::exp(std::clamp(raw[i] - max_logit, -25.0F, 0.0F));
        sum += gates[i];
    }
    if (sum > 0.0F) {
        for (int i = 0; i < num_exp; i++) gates[i] /= sum;
    }
    return gates;
}

template <size_t DimN>
void ApplyExpertContext(const PolicyNetwork::ExpertHeads& expert,
                        std::array<float, DimN>& local_in) {
    if (expert.version < 7) {
        for (int col = kGlobalDim; col < kContextDim; col++) local_in[col] = 0.0F;
        return;
    }
    if (expert.use_commander) {
        for (int i = 0; i < 5; i++) {
            float acc = expert.commander_b[i];
            for (int j = 0; j < PolicyNetwork::kGateInDim; j++) {
                acc += expert.commander_w[static_cast<size_t>(i) * PolicyNetwork::kGateInDim + j] *
                       local_in[j];
            }
            local_in[25 + i] = std::tanh(acc);
        }
    } else {
        for (int col = 25; col < kContextDim; col++) local_in[col] = 0.0F;
    }
}

}  // namespace

std::array<float, kDiploOutDim> PolicyNetwork::ForwardDiplo(const DiploInVec& input) const {
    if (experts_.empty()) return diplo_net_.Forward(input);
    auto gates = ComputeExpertGates(*this, input);
    int num_exp = std::min(static_cast<int>(experts_.size()), kMaxExperts);
    std::array<float, kDiploOutDim> acc{};
    for (int exp_idx = 0; exp_idx < num_exp; exp_idx++) {
        DiploInVec local_in = input;
        ApplyExpertContext(experts_[exp_idx], local_in);
        auto raw = experts_[exp_idx].diplo_net.Forward(local_in);
        for (int out_idx = 0; out_idx < kDiploOutDim; out_idx++)
            acc[out_idx] += gates[exp_idx] * raw[out_idx];
    }
    return acc;
}

std::array<float, kMissionOutDim> PolicyNetwork::ForwardMission(const MissionInVec& input) const {
    if (experts_.empty()) return mission_net_.Forward(input);
    auto gates = ComputeExpertGates(*this, input);
    int num_exp = std::min(static_cast<int>(experts_.size()), kMaxExperts);
    std::array<float, kMissionOutDim> acc{};
    for (int exp_idx = 0; exp_idx < num_exp; exp_idx++) {
        MissionInVec local_in = input;
        ApplyExpertContext(experts_[exp_idx], local_in);
        auto raw = experts_[exp_idx].mission_net.Forward(local_in);
        for (int out_idx = 0; out_idx < kMissionOutDim; out_idx++)
            acc[out_idx] += gates[exp_idx] * raw[out_idx];
    }
    return acc;
}

std::array<float, kEconOutDim> PolicyNetwork::ForwardEcon(const EconInVec& input) const {
    if (experts_.empty()) return econ_net_.Forward(input);
    auto gates = ComputeExpertGates(*this, input);
    int num_exp = std::min(static_cast<int>(experts_.size()), kMaxExperts);
    std::array<float, kEconOutDim> acc{};
    for (int exp_idx = 0; exp_idx < num_exp; exp_idx++) {
        EconInVec local_in = input;
        ApplyExpertContext(experts_[exp_idx], local_in);
        auto raw = experts_[exp_idx].econ_net.Forward(local_in);
        acc[0] += gates[exp_idx] * raw[0];
    }
    return acc;
}

std::array<float, kMusterOutDim> PolicyNetwork::ForwardMuster(const MusterInVec& input) const {
    return muster_net_.Forward(input);
}

DiploOutput PolicyNetwork::ScoreDiplomacy(const ContextVec& context, const RivalVec& rival) const {
    auto raw = ForwardDiplo(PackDiploInput(context, rival));
    DiploOutput out;
    out.stance_logits = {raw[0], raw[1], raw[2], raw[3]};
    out.tribute_logit = raw[4];
    out.joint_war_logit = raw[5];
    return out;
}

MissionOutput PolicyNetwork::ScoreMission(const ContextVec& context, const StackVec& stack,
                                          const TargetVec& target) const {
    auto raw = ForwardMission(PackMissionInput(context, stack, target));
    return MissionOutput{raw[0], raw[1]};
}

float PolicyNetwork::ScoreEconomy(const ContextVec& context, const EconVec& econ) const {
    return ForwardEcon(PackEconInput(context, econ))[0];
}

float PolicyNetwork::EvaluateValue(const ContextVec& context, const CriticVec& critic) const {
    return value_net_.Forward(PackValueInput(context, critic))[0];
}

float PolicyNetwork::EvaluateStackValue(const StackValueInVec& stack_state) const {
    return stack_value_net_.Forward(stack_state)[0];
}

std::vector<float> PolicyNetwork::Flatten() const {
    std::vector<float> out;
    out.reserve(kTotalParams);
    diplo_net_.ToFlat(out);
    mission_net_.ToFlat(out);
    econ_net_.ToFlat(out);
    value_net_.ToFlat(out);
    stack_value_net_.ToFlat(out);
    return out;
}

void PolicyNetwork::Unflatten(const std::vector<float>& params) {
    size_t offset = 0;
    offset = diplo_net_.FromFlat(params, offset);
    offset = mission_net_.FromFlat(params, offset);
    offset = econ_net_.FromFlat(params, offset);
    offset = value_net_.FromFlat(params, offset);
    stack_value_net_.FromFlat(params, offset);
}

std::vector<float> PolicyNetwork::FlattenTrainable() const {
    std::vector<float> out;
    out.reserve(kV8TrainableParams);
    diplo_net_.ToFlat(out);
    mission_net_.ToFlat(out);
    econ_net_.ToFlat(out);
    value_net_.ToFlat(out);
    stack_value_net_.ToFlat(out);
    muster_net_.ToFlat(out);
    out.insert(out.end(), commander_w_.begin(), commander_w_.end());
    out.insert(out.end(), commander_b_.begin(), commander_b_.end());
    out.push_back(detour_bias_);
    return out;
}

void PolicyNetwork::UnflattenTrainable(const std::vector<float>& params) {
    size_t offset = 0;
    offset = diplo_net_.FromFlat(params, offset);
    offset = mission_net_.FromFlat(params, offset);
    offset = econ_net_.FromFlat(params, offset);
    offset = value_net_.FromFlat(params, offset);
    offset = stack_value_net_.FromFlat(params, offset);
    offset = muster_net_.FromFlat(params, offset);
    for (float& weight : commander_w_) {
        weight = offset < params.size() ? params[offset++] : 0.0F;
    }
    for (float& bias : commander_b_) {
        bias = offset < params.size() ? params[offset++] : 0.0F;
    }
    if (offset < params.size()) detour_bias_ = params[offset];
}

namespace {

size_t WidenFlatMlp(const std::vector<float>& flat, size_t offset, int out_dim, int h1_dim,
                    int h2_dim, int old_in, int new_in, int insert_at, int insert_count,
                    std::vector<float>& out) {
    auto next = [&]() { return offset < flat.size() ? flat[offset++] : 0.0F; };
    auto copy_mat = [&](int rows) {
        size_t base = out.size();
        out.insert(out.end(), static_cast<size_t>(rows) * new_in, 0.0F);
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < old_in; col++) {
                float weight = next();
                int new_col = col < insert_at ? col : col + insert_count;
                if (new_col < new_in) {
                    out[base + static_cast<size_t>(row) * new_in + new_col] = weight;
                }
            }
        }
    };
    auto copy_vec = [&](int count) {
        for (int i = 0; i < count; i++) out.push_back(next());
    };
    copy_mat(out_dim);
    copy_mat(h1_dim);
    copy_vec(h1_dim);
    copy_vec(h2_dim * h1_dim);
    copy_vec(h2_dim);
    copy_vec(out_dim * h2_dim);
    copy_vec(out_dim);
    return offset;
}

template <typename Mlp>
void ZeroContextColumns(Mlp& mlp, int col_begin, int col_end) {
    for (int row = 0; row < Mlp::kOutDim; row++) {
        for (int col = col_begin; col < col_end && col < Mlp::kInDim; col++) {
            mlp.w_skip[static_cast<size_t>(row) * Mlp::kInDim + col] = 0.0F;
        }
    }
    for (int row = 0; row < Mlp::kH1Dim; row++) {
        for (int col = col_begin; col < col_end && col < Mlp::kInDim; col++) {
            mlp.w1[static_cast<size_t>(row) * Mlp::kInDim + col] = 0.0F;
        }
    }
}

}  // namespace

void PolicyNetwork::UnflattenLegacy(const std::vector<float>& params) {
    if (params.size() == static_cast<size_t>(kLegacyV7Params)) {
        const int old_mission_in = MissionNet::kInDim - kAddedV8TargetDim;
        const int old_econ_in = EconNet::kInDim - kAddedV8EconDim;
        size_t offset = 0;
        offset = diplo_net_.FromFlat(params, offset);
        offset = mission_net_.FromFlatWidenInput(params, offset, old_mission_in, old_mission_in,
                                                 kAddedV8TargetDim);
        offset = econ_net_.FromFlatWidenInput(params, offset, old_econ_in, old_econ_in,
                                              kAddedV8EconDim);
        offset = value_net_.FromFlat(params, offset);
        stack_value_net_.FromFlat(params, offset);
        version_ = 7;
        return;
    }
    if (params.size() == static_cast<size_t>(kLegacyV6Params)) {
        const int old_mission_in = MissionNet::kInDim - kAddedTargetDim;
        const int old_econ_in = EconNet::kInDim - kAddedEconDim;
        size_t offset = 0;
        offset = diplo_net_.FromFlat(params, offset);
        offset = mission_net_.FromFlatWidenInput(params, offset, old_mission_in, old_mission_in,
                                                 kAddedTargetDim);
        offset = econ_net_.FromFlatWidenInput(params, offset, old_econ_in, old_econ_in,
                                              kAddedEconDim);
        offset = value_net_.FromFlat(params, offset);
        stack_value_net_.FromFlat(params, offset);
        version_ = 6;
        return;
    }
    // Older files (v5/v4/v3) first widen to v6, then v6 -> v8.
    const bool is_v5 = params.size() == static_cast<size_t>(kLegacyV5Params);
    const bool is_v3 = params.size() == static_cast<size_t>(kLegacyV3Params);
    const int insert_at = kLegacyGlobalDim, insert_count = kAddedGlobalDim;
    const int critic_drop = is_v5 ? kV5Dropped : kV4CriticDropped;
    const int v6_mission_in = MissionNet::kInDim - kAddedTargetDim;
    const int v6_econ_in = EconNet::kInDim - kAddedEconDim;
    std::vector<float> v6_flat;
    v6_flat.reserve(kLegacyV6Params);
    size_t offset = 0;
    offset = WidenFlatMlp(params, offset, DiploNet::kOutDim, DiploNet::kH1Dim, DiploNet::kH2Dim,
                          DiploNet::kInDim - kV5Dropped, DiploNet::kInDim, insert_at, insert_count,
                          v6_flat);
    offset = WidenFlatMlp(params, offset, MissionNet::kOutDim, MissionNet::kH1Dim,
                          MissionNet::kH2Dim, v6_mission_in - kV5Dropped, v6_mission_in, insert_at,
                          insert_count, v6_flat);
    offset = WidenFlatMlp(params, offset, EconNet::kOutDim, EconNet::kH1Dim, EconNet::kH2Dim,
                          v6_econ_in - kV5Dropped, v6_econ_in, insert_at, insert_count, v6_flat);
    offset = WidenFlatMlp(params, offset, ValueNet::kOutDim, ValueNet::kH1Dim, ValueNet::kH2Dim,
                          ValueNet::kInDim - critic_drop, ValueNet::kInDim, insert_at, insert_count,
                          v6_flat);
    if (is_v3) {
        v6_flat.insert(v6_flat.end(), StackValueNet::kParamCount, 0.0F);
    } else {
        WidenFlatMlp(params, offset, StackValueNet::kOutDim, StackValueNet::kH1Dim,
                     StackValueNet::kH2Dim, StackValueNet::kInDim - critic_drop,
                     StackValueNet::kInDim, insert_at, insert_count, v6_flat);
    }
    UnflattenLegacy(v6_flat);
    version_ = is_v5 ? 5 : (is_v3 ? 3 : 4);
}

void PolicyNetwork::UpgradeToV7() {
    if (version_ >= 7) return;
    ZeroContextColumns(diplo_net_, kGlobalDim, kContextDim);
    ZeroContextColumns(mission_net_, kGlobalDim, kContextDim);
    ZeroContextColumns(econ_net_, kGlobalDim, kContextDim);
    ZeroContextColumns(value_net_, kGlobalDim, kContextDim);
    ZeroContextColumns(stack_value_net_, kGlobalDim, kContextDim);
    econ_net_.w_skip[kContextDim + kEconGuard1] = -0.15F;
    econ_net_.w_skip[kContextDim + kEconGuard2] = -0.15F;
    version_ = 7;
}

void PolicyNetwork::UpgradeToV8() {
    if (version_ < 7) UpgradeToV7();
    if (version_ >= 8) return;
    version_ = 8;
}

PolicyNetwork::ArchetypeHead PolicyNetwork::ExtractArchetypeHead() const {
    ArchetypeHead head;
    head.use_muster = use_muster_;
    head.use_standard = use_standard_;
    head.avoid_blockers = avoid_blockers_;
    head.detour_bias = detour_bias_;
    size_t offset = 0;
    auto pack = [&](const auto& mlp) {
        for (float val : mlp.w_skip) head.params[offset++] = val;
        for (float val : mlp.w3) head.params[offset++] = val;
        for (float val : mlp.b3) head.params[offset++] = val;
    };
    pack(diplo_net_);
    pack(mission_net_);
    pack(econ_net_);
    pack(muster_net_);
    return head;
}

void PolicyNetwork::ApplyArchetypeHead(const ArchetypeHead& head) {
    use_muster_ = head.use_muster;
    use_standard_ = head.use_standard;
    avoid_blockers_ = head.avoid_blockers;
    detour_bias_ = head.detour_bias;
    size_t offset = 0;
    auto unpack = [&](auto& mlp) {
        for (float& val : mlp.w_skip) val = head.params[offset++];
        for (float& val : mlp.w3) val = head.params[offset++];
        for (float& val : mlp.b3) val = head.params[offset++];
    };
    unpack(diplo_net_);
    unpack(mission_net_);
    unpack(econ_net_);
    unpack(muster_net_);
}

PolicyNetwork PolicyNetwork::MaterializeForWeights(
        const personality::ArchetypeWeightArray& weights) const {
    if (!has_archetypes_) return *this;
    ArchetypeHead blended{};
    float muster_vote = 0.0F;
    float std_vote = 0.0F;
    float avoid_vote = 0.0F;
    float bias_acc = 0.0F;
    for (int arch_idx = 0; arch_idx < personality::kNumArchetypes; arch_idx++) {
        float weight = weights[arch_idx];
        if (weight <= 0.0F) continue;
        const ArchetypeHead& head = archetypes_[arch_idx];
        for (size_t i = 0; i < ArchetypeHead::kParamCount; i++) {
            blended.params[i] += weight * head.params[i];
        }
        if (head.use_muster) muster_vote += weight;
        if (head.use_standard) std_vote += weight;
        if (head.avoid_blockers) avoid_vote += weight;
        bias_acc += weight * head.detour_bias;
    }
    blended.use_muster = muster_vote >= 0.35F;
    blended.use_standard = std_vote >= 0.35F;
    blended.avoid_blockers = avoid_vote >= 0.25F;
    blended.detour_bias = bias_acc;

    PolicyNetwork out = *this;
    out.ApplyArchetypeHead(blended);
    out.use_personalities_ = false;
    out.persona_active_ = persona_active_;
    return out;
}

PolicyNetwork PolicyNetwork::MaterializeForSide(const Side& side) const {
    if (!use_personalities_ || !has_archetypes_) return *this;
    return MaterializeForWeights(personality::ArchetypeWeights(personality::ForSide(side)));
}

absl::Status PolicyNetwork::SaveJson(const std::filesystem::path& path, int generation,
                                     float selection_city_share) const {
    std::ostringstream out;
    auto weights = Flatten();
    auto write_vec = [&](const std::vector<float>& vec, const char* indent) {
        out << "[";
        for (size_t i = 0; i < vec.size(); i++) {
            if (i % 16 == 0) out << "\n" << indent;
            out << vec[i];
            if (i + 1 < vec.size()) out << ", ";
        }
        out << "\n  ]";
    };
    out << "{\n";
    out << "  \"version\": " << version_ << ",\n";
    out << "  \"enableCityGuard\": " << (enable_city_guard_ ? 1 : 0) << ",\n";
    out << "  \"generation\": " << generation << ",\n";
    out << "  \"selectionCityShare\": " << selection_city_share << ",\n";
    out << "  \"paramCount\": " << weights.size() << ",\n";
    out << "  \"weights\": ";
    write_vec(weights, "    ");
    if (use_commander_) {
        std::vector<float> cmd;
        cmd.insert(cmd.end(), commander_w_.begin(), commander_w_.end());
        cmd.insert(cmd.end(), commander_b_.begin(), commander_b_.end());
        out << ",\n  \"commander\": ";
        write_vec(cmd, "    ");
    }
    if (use_muster_ || use_standard_ || has_archetypes_) {
        out << ",\n  \"useMuster\": " << (use_muster_ ? 1 : 0);
        out << ",\n  \"useStandard\": " << (use_standard_ ? 1 : 0);
        std::vector<float> mst;
        muster_net_.ToFlat(mst);
        out << ",\n  \"muster\": ";
        write_vec(mst, "    ");
    }
    if (avoid_blockers_) {
        out << ",\n  \"avoidBlockers\": 1";
        out << ",\n  \"detourBias\": " << detour_bias_;
    }
    if (has_archetypes_) {
        out << ",\n  \"usePersonalities\": " << (use_personalities_ ? 1 : 0);
        out << ",\n  \"archetypes\": [\n";
        for (int arch_idx = 0; arch_idx < personality::kNumArchetypes; arch_idx++) {
            const ArchetypeHead& head = archetypes_[arch_idx];
            std::vector<float> p_vec(head.params.begin(), head.params.end());
            out << "    {\"name\": \"" << personality::ArchetypeName(arch_idx) << "\""
                << ", \"useMuster\": " << (head.use_muster ? 1 : 0)
                << ", \"useStandard\": " << (head.use_standard ? 1 : 0)
                << ", \"avoidBlockers\": " << (head.avoid_blockers ? 1 : 0)
                << ", \"detourBias\": " << head.detour_bias << ", \"params\": ";
            write_vec(p_vec, "      ");
            out << "}" << (arch_idx + 1 < personality::kNumArchetypes ? "," : "") << "\n";
        }
        out << "  ]";
    }
    if (!experts_.empty()) {
        std::vector<float> gate_flat;
        gate_net_.ToFlat(gate_flat);
        out << ",\n  \"gate\": ";
        write_vec(gate_flat, "    ");
        out << ",\n  \"experts\": [\n";
        for (size_t exp_idx = 0; exp_idx < experts_.size(); exp_idx++) {
            std::vector<float> exp_flat;
            experts_[exp_idx].diplo_net.ToFlat(exp_flat);
            experts_[exp_idx].mission_net.ToFlat(exp_flat);
            experts_[exp_idx].econ_net.ToFlat(exp_flat);
            out << "    {\"version\": " << experts_[exp_idx].version;
            if (experts_[exp_idx].use_commander) {
                std::vector<float> exp_cmd;
                exp_cmd.insert(exp_cmd.end(), experts_[exp_idx].commander_w.begin(),
                               experts_[exp_idx].commander_w.end());
                exp_cmd.insert(exp_cmd.end(), experts_[exp_idx].commander_b.begin(),
                               experts_[exp_idx].commander_b.end());
                out << ", \"commander\": ";
                write_vec(exp_cmd, "      ");
            }
            out << ", \"weights\": ";
            write_vec(exp_flat, "      ");
            out << "}" << (exp_idx + 1 < experts_.size() ? "," : "") << "\n";
        }
        out << "  ]";
    }
    out << "\n}\n";
    return WriteFile(path, out.str());
}

absl::Status PolicyNetwork::LoadJson(const std::filesystem::path& path) {
    auto text = ReadFile(path);
    if (!text.ok()) return text.status();
    nlohmann::ordered_json root =
            nlohmann::ordered_json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to parse PolicyNetwork weights '%s': file is not valid JSON. Re-export or "
                "restore cpp/ai_weights.json.",
                path.string()));
    }
    auto weights_it = root.find("weights");
    if (weights_it == root.end() || !weights_it->is_array()) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load PolicyNetwork weights '%s': missing JSON array field \"weights\". "
                "Provide a valid v3..v8 ai_weights.json file.",
                path.string()));
    }
    const auto& arr = *weights_it;
    int param_count = static_cast<int>(arr.size());
    if (param_count != kTotalParams && param_count != kLegacyV7Params &&
        param_count != kLegacyV6Params && param_count != kLegacyV5Params &&
        param_count != kLegacyV4Params && param_count != kLegacyV3Params) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load PolicyNetwork weights '%s': weight count %d does not match v8 (%d) "
                "or supported legacy counts (%d, %d, %d, %d, %d). Re-train or restore matching "
                "weights.",
                path.string(), param_count, kTotalParams, kLegacyV7Params, kLegacyV6Params,
                kLegacyV5Params, kLegacyV4Params, kLegacyV3Params));
    }
    std::vector<float> weights(static_cast<size_t>(param_count));
    for (int i = 0; i < param_count; i++) {
        if (!arr[static_cast<size_t>(i)].is_number()) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to load PolicyNetwork weights '%s': weight element %d is not numeric. "
                    "Re-export or restore cpp/ai_weights.json.",
                    path.string(), i));
        }
        weights[static_cast<size_t>(i)] = arr[static_cast<size_t>(i)].get<float>();
    }
    if (param_count == kTotalParams) {
        Unflatten(weights);
        auto ver_it = root.find("version");
        version_ = (ver_it != root.end() && ver_it->is_number_integer()) ? ver_it->get<int>() : 8;
    } else {
        UnflattenLegacy(weights);
        if (param_count == kLegacyV7Params) {
            auto ver_it = root.find("version");
            if (ver_it != root.end() && ver_it->is_number_integer()) {
                version_ = ver_it->get<int>();
            }
        }
    }

    auto int_field = [&](const nlohmann::ordered_json& obj, const char* key, int def_val) -> int {
        auto found = obj.find(key);
        return (found != obj.end() && found->is_number()) ? found->get<int>() : def_val;
    };
    auto float_field = [&](const nlohmann::ordered_json& obj, const char* key,
                           float def_val) -> float {
        auto found = obj.find(key);
        return (found != obj.end() && found->is_number()) ? found->get<float>() : def_val;
    };

    enable_city_guard_ = int_field(root, "enableCityGuard", 0) != 0;
    use_commander_ = false;
    commander_w_.fill(0.0F);
    commander_b_.fill(0.0F);
    auto cmd_it = root.find("commander");
    if (cmd_it != root.end() && cmd_it->is_array() &&
        cmd_it->size() == commander_w_.size() + commander_b_.size()) {
        use_commander_ = true;
        for (size_t i = 0; i < commander_w_.size(); i++) {
            commander_w_[i] = (*cmd_it)[i].get<float>();
        }
        for (size_t i = 0; i < commander_b_.size(); i++) {
            commander_b_[i] = (*cmd_it)[commander_w_.size() + i].get<float>();
        }
    }

    use_muster_ = false;
    use_standard_ = false;
    muster_net_.Zero();
    auto mst_it = root.find("muster");
    if (mst_it != root.end() && mst_it->is_array() &&
        static_cast<int>(mst_it->size()) == MusterNet::kParamCount) {
        use_muster_ = int_field(root, "useMuster", 1) != 0;
        use_standard_ = int_field(root, "useStandard", 0) != 0;
        std::vector<float> mst_flat(mst_it->size());
        for (size_t i = 0; i < mst_it->size(); i++) mst_flat[i] = (*mst_it)[i].get<float>();
        muster_net_.FromFlat(mst_flat, 0);
    }

    avoid_blockers_ = int_field(root, "avoidBlockers", 0) != 0;
    detour_bias_ = float_field(root, "detourBias", -0.05F);
    use_personalities_ = false;
    persona_active_ = false;
    has_archetypes_ = false;
    archetypes_ = {};
    auto arch_it = root.find("archetypes");
    if (arch_it != root.end() && arch_it->is_array() &&
        static_cast<int>(arch_it->size()) == personality::kNumArchetypes) {
        bool ok_arch = true;
        for (int arch_idx = 0; arch_idx < personality::kNumArchetypes; arch_idx++) {
            const auto& arch_json = (*arch_it)[static_cast<size_t>(arch_idx)];
            auto params_it = arch_json.find("params");
            if (params_it == arch_json.end() || !params_it->is_array() ||
                static_cast<int>(params_it->size()) != ArchetypeHead::kParamCount) {
                ok_arch = false;
                break;
            }
            ArchetypeHead head;
            head.use_muster = int_field(arch_json, "useMuster", 0) != 0;
            head.use_standard = int_field(arch_json, "useStandard", 0) != 0;
            head.avoid_blockers = int_field(arch_json, "avoidBlockers", 0) != 0;
            head.detour_bias = float_field(arch_json, "detourBias", -0.05F);
            for (int i = 0; i < ArchetypeHead::kParamCount; i++) {
                head.params[static_cast<size_t>(i)] =
                        (*params_it)[static_cast<size_t>(i)].get<float>();
            }
            archetypes_[arch_idx] = head;
        }
        if (ok_arch) {
            has_archetypes_ = true;
            use_personalities_ = int_field(root, "usePersonalities", 1) != 0;
            persona_active_ = use_personalities_;
        }
    }

    experts_.clear();
    gate_net_.Zero();
    auto gate_it = root.find("gate");
    auto exp_it = root.find("experts");
    if (gate_it != root.end() && gate_it->is_array() && exp_it != root.end() &&
        exp_it->is_array()) {
        std::vector<float> gate_flat(gate_it->size());
        for (size_t i = 0; i < gate_it->size(); i++) gate_flat[i] = (*gate_it)[i].get<float>();
        gate_net_.FromFlat(gate_flat, 0);
        const size_t exp_actor_params =
                DiploNet::kParamCount + MissionNet::kParamCount + EconNet::kParamCount;
        const size_t exp_v7_params =
                DiploNet::kParamCount +
                MlpParamsWithInput<MissionNet>(MissionNet::kInDim - kAddedV8TargetDim) +
                MlpParamsWithInput<EconNet>(EconNet::kInDim - kAddedV8EconDim);
        for (size_t exp_idx = 0;
             exp_idx < exp_it->size() && exp_idx < static_cast<size_t>(kMaxExperts); exp_idx++) {
            const auto& exp_json = (*exp_it)[exp_idx];
            auto exp_w_it = exp_json.find("weights");
            if (exp_w_it == exp_json.end() || !exp_w_it->is_array()) continue;
            if (exp_w_it->size() != exp_actor_params && exp_w_it->size() != exp_v7_params) continue;
            std::vector<float> exp_flat(exp_w_it->size());
            for (size_t i = 0; i < exp_w_it->size(); i++) exp_flat[i] = (*exp_w_it)[i].get<float>();
            ExpertHeads exp_head;
            exp_head.version = int_field(exp_json, "version", 6);
            auto exp_cmd_it = exp_json.find("commander");
            if (exp_cmd_it != exp_json.end() && exp_cmd_it->is_array() &&
                exp_cmd_it->size() == exp_head.commander_w.size() + exp_head.commander_b.size()) {
                exp_head.use_commander = true;
                for (size_t i = 0; i < exp_head.commander_w.size(); i++) {
                    exp_head.commander_w[i] = (*exp_cmd_it)[i].get<float>();
                }
                for (size_t i = 0; i < exp_head.commander_b.size(); i++) {
                    exp_head.commander_b[i] =
                            (*exp_cmd_it)[exp_head.commander_w.size() + i].get<float>();
                }
            }
            size_t offset = 0;
            offset = exp_head.diplo_net.FromFlat(exp_flat, offset);
            if (exp_w_it->size() == exp_v7_params) {
                const int old_m = MissionNet::kInDim - kAddedV8TargetDim;
                const int old_e = EconNet::kInDim - kAddedV8EconDim;
                offset = exp_head.mission_net.FromFlatWidenInput(exp_flat, offset, old_m, old_m,
                                                                 kAddedV8TargetDim);
                exp_head.econ_net.FromFlatWidenInput(exp_flat, offset, old_e, old_e,
                                                     kAddedV8EconDim);
            } else {
                offset = exp_head.mission_net.FromFlat(exp_flat, offset);
                exp_head.econ_net.FromFlat(exp_flat, offset);
            }
            experts_.push_back(std::move(exp_head));
        }
    }
    return absl::OkStatus();
}

namespace {

class MlRuntimeConfig {
  public:
    float DeployTemp() const { return deploy_temperature_.load(std::memory_order_relaxed); }

    void SetDeployTemp(float temperature) {
        deploy_temperature_.store(std::max(0.01F, temperature), std::memory_order_relaxed);
    }

    float ArmyWeight() const { return army_score_weight_.load(std::memory_order_relaxed); }

    void SetArmyWeight(float weight) { army_score_weight_.store(weight, std::memory_order_relaxed); }

    float ConcentrationWeight() const {
        return concentration_score_weight_.load(std::memory_order_relaxed);
    }

    void SetConcentrationWeight(float weight) {
        concentration_score_weight_.store(std::max(0.0F, weight), std::memory_order_relaxed);
    }

    bool RecordThoughts() const { return record_thoughts_.load(std::memory_order_relaxed); }

    void SetRecordThoughts(bool enabled) {
        record_thoughts_.store(enabled, std::memory_order_relaxed);
    }

  private:
    std::atomic<float> deploy_temperature_{kDeployTemperature};
    std::atomic<float> army_score_weight_{0.25F};
    std::atomic<float> concentration_score_weight_{0.0F};
    std::atomic<bool> record_thoughts_{true};
};

MlRuntimeConfig& GlobalMlRuntimeConfig() {
    static absl::NoDestructor<MlRuntimeConfig> config;
    return *config;
}

}  // namespace

float DeployTemperature() {
    return GlobalMlRuntimeConfig().DeployTemp();
}

void SetDeployTemperature(float temperature) {
    GlobalMlRuntimeConfig().SetDeployTemp(temperature);
}

float ArmyScoreWeight() {
    return GlobalMlRuntimeConfig().ArmyWeight();
}

void SetArmyScoreWeight(float weight) {
    GlobalMlRuntimeConfig().SetArmyWeight(weight);
}

float ConcentrationScoreWeight() {
    return GlobalMlRuntimeConfig().ConcentrationWeight();
}

void SetConcentrationScoreWeight(float weight) {
    GlobalMlRuntimeConfig().SetConcentrationWeight(weight);
}

void PropagateEconomyLineage(absl::Span<TurnStepRecord> turns,
                             const UnitReturnMap& unit_birth_returns, float stack_gamma) {
    struct StepRef {
        int turn_idx = kNone;
        size_t step_idx = 0;
    };
    absl::flat_hash_map<int, StepRef> active_produce;
    absl::flat_hash_map<int, StepRef> active_vector;
    absl::flat_hash_map<uint64_t, StepRef> bought_slots;

    auto make_slot_key = [](int city_idx, int unit_type) -> uint64_t {
        return (static_cast<uint64_t>(static_cast<uint32_t>(city_idx)) << 32U) |
               static_cast<uint64_t>(static_cast<uint32_t>(unit_type));
    };

    for (auto& turn : turns) {
        for (auto& econ_step : turn.econ_steps) {
            econ_step.lineage_return = 0.0F;
        }
    }

    for (int turn_idx = 0; turn_idx < static_cast<int>(turns.size()); ++turn_idx) {
        for (const auto& prod : turns[turn_idx].produced_units) {
            auto ret_it = unit_birth_returns.find(prod.unit_id);
            if (ret_it == unit_birth_returns.end() || ret_it->second == 0.0F) continue;
            const float birth_ret = ret_it->second;

            if (auto prod_it = active_produce.find(prod.city_idx);
                prod_it != active_produce.end() && prod_it->second.turn_idx >= 0 &&
                prod_it->second.turn_idx < turn_idx) {
                float disc = std::pow(stack_gamma,
                                      static_cast<float>(turn_idx - prod_it->second.turn_idx));
                turns[prod_it->second.turn_idx].econ_steps[prod_it->second.step_idx].lineage_return +=
                        disc * birth_ret;
            }
            if (auto vec_it = active_vector.find(prod.city_idx);
                vec_it != active_vector.end() && vec_it->second.turn_idx >= 0 &&
                vec_it->second.turn_idx < turn_idx) {
                float disc = std::pow(stack_gamma,
                                      static_cast<float>(turn_idx - vec_it->second.turn_idx));
                turns[vec_it->second.turn_idx].econ_steps[vec_it->second.step_idx].lineage_return +=
                        disc * birth_ret;
            }
            if (prod.city_idx >= 0 && prod.unit_type >= 0) {
                uint64_t slot_key = make_slot_key(prod.city_idx, prod.unit_type);
                if (auto buy_it = bought_slots.find(slot_key);
                    buy_it != bought_slots.end() && buy_it->second.turn_idx >= 0 &&
                    buy_it->second.turn_idx < turn_idx) {
                    float disc = std::pow(stack_gamma,
                                          static_cast<float>(turn_idx - buy_it->second.turn_idx));
                    turns[buy_it->second.turn_idx]
                            .econ_steps[buy_it->second.step_idx]
                            .lineage_return += disc * birth_ret;
                }
            }
        }

        const auto& econ_steps = turns[turn_idx].econ_steps;
        for (size_t step_idx = 0; step_idx < econ_steps.size(); ++step_idx) {
            const auto& step = econ_steps[step_idx];
            switch (step.family) {
                case EconFamily::kBuySlot:
                    if (step.chosen_idx > 0 && step.city_idx >= 0 && step.unit_type >= 0) {
                        bought_slots[make_slot_key(step.city_idx, step.unit_type)] =
                                StepRef{turn_idx, step_idx};
                    }
                    break;
                case EconFamily::kProduce:
                    if (step.city_idx >= 0) {
                        if (step.chosen_idx > 0) {
                            active_produce[step.city_idx] = StepRef{turn_idx, step_idx};
                        } else {
                            active_produce.erase(step.city_idx);
                        }
                    }
                    break;
                case EconFamily::kVector:
                    if (step.city_idx >= 0) {
                        active_vector[step.city_idx] = StepRef{turn_idx, step_idx};
                    }
                    break;
                case EconFamily::kRetain:
                case EconFamily::kSpoils:
                case EconFamily::kGuardCity:
                case EconFamily::kCount:
                    break;
            }
        }
    }
}

void RefreshCommanderPosture(float* input_ctx, const PolicyNetwork& net) {
    if (!net.UseCommander()) return;
    for (int post_idx = 0; post_idx < 5; post_idx++) {
        const float* row =
                &net.CommanderW()[static_cast<size_t>(post_idx) * PolicyNetwork::kGateInDim];
        float acc = net.CommanderB()[post_idx] + DotProduct(row, input_ctx, PolicyNetwork::kGateInDim);
        input_ctx[25 + post_idx] = std::tanh(acc);
    }
}

void AccumulateCommanderGrad(const float* input_ctx, const std::array<float, 5>& d_posture,
                             PolicyNetwork& grad) {
    for (int post_idx = 0; post_idx < 5; post_idx++) {
        float act = input_ctx[25 + post_idx];
        float d_pre = d_posture[post_idx] * (1.0F - act * act);
        if (d_pre == 0.0F) continue;
        grad.CommanderB()[post_idx] += d_pre;
        float* grad_row =
                &grad.CommanderW()[static_cast<size_t>(post_idx) * PolicyNetwork::kGateInDim];
        Axpy(grad_row, d_pre, input_ctx, PolicyNetwork::kGateInDim);
    }
}

bool RecordThoughts() {
    return GlobalMlRuntimeConfig().RecordThoughts();
}

void SetRecordThoughts(bool enabled) {
    GlobalMlRuntimeConfig().SetRecordThoughts(enabled);
}

float RolloutSampler::NextUniform01() {
    if (rng_state == 0) rng_state = 0x9e3779b9U;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return static_cast<float>(rng_state & 0x00ffffffU) / static_cast<float>(0x01000000U);
}

float RolloutSampler::DrawStepTemperature() {
    const float base_temp = std::max(0.02F, temperature);
    if (explore_eps <= 0.0F) {
        last_step_temp = base_temp;
        return last_step_temp;
    }
    const float roll = NextUniform01();
    last_step_temp = (roll < explore_eps) ? std::max(0.05F, explore_temp) : base_temp;
    return last_step_temp;
}

int SelectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler, float* out_log_prob,
                     const std::vector<float>* log_mult) {
    if (logits.empty()) {
        if (out_log_prob) *out_log_prob = 0.0F;
        return 0;
    }
    auto log_mult_of = [&](size_t index) {
        return (log_mult && index < log_mult->size()) ? (*log_mult)[index] : 0.0F;
    };
    if (!sampler || logits.size() == 1) {
        if (sampler) sampler->last_step_temp = std::max(0.02F, sampler->temperature);
        float deploy_inv_t = 1.0F / DeployTemperature();
        int best = 0;
        for (size_t i = 1; i < logits.size(); i++) {
            if (logits[i] * deploy_inv_t - log_mult_of(i) >
                logits[best] * deploy_inv_t - log_mult_of(best))
                best = static_cast<int>(i);
        }
        if (out_log_prob) *out_log_prob = 0.0F;
        return best;
    }

    const float step_temp = sampler->DrawStepTemperature();
    float inv_t = 1.0F / step_temp;
    auto effective_logit = [&](size_t index) { return logits[index] * inv_t - log_mult_of(index); };
    float max_logit = effective_logit(0);
    for (size_t i = 1; i < logits.size(); i++) {
        max_logit = std::max(max_logit, effective_logit(i));
    }
    constexpr size_t kInlineProbsCap = 256;
    std::array<float, kInlineProbsCap> inline_probs{};
    std::vector<float> heap_probs;
    float* probs = nullptr;
    if (logits.size() <= kInlineProbsCap) {
        probs = inline_probs.data();
    } else {
        heap_probs.resize(logits.size());
        probs = heap_probs.data();
    }
    float sum_exp = 0.0F;
    for (size_t i = 0; i < logits.size(); i++) {
        float exp_logit = std::exp(std::clamp(effective_logit(i) - max_logit, -30.0F, 0.0F));
        probs[i] = exp_logit;
        sum_exp += exp_logit;
    }
    const float denom = std::max(1e-12F, sum_exp);
    for (size_t i = 0; i < logits.size(); i++) probs[i] /= denom;

    float sample = sampler->NextUniform01();
    float acc = 0.0F;
    int chosen = static_cast<int>(logits.size()) - 1;
    for (size_t i = 0; i < logits.size(); i++) {
        acc += probs[i];
        if (sample <= acc) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    if (out_log_prob) {
        *out_log_prob = std::log(std::max(1e-12F, probs[chosen]));
    }
    return chosen;
}

bool SelectBinary(float logit, RolloutSampler* sampler, float* out_log_prob) {
    if (!sampler) {
        if (out_log_prob) *out_log_prob = 0.0F;
        return logit > 0.0F;
    }
    const float step_temp = sampler->DrawStepTemperature();
    float inv_t = 1.0F / step_temp;
    float scaled_logit = std::clamp(logit * inv_t, -15.0F, 15.0F);
    float prob_yes = 1.0F / (1.0F + std::exp(-scaled_logit));
    bool yes = sampler->NextUniform01() < prob_yes;
    if (out_log_prob) *out_log_prob = std::log(std::max(1e-12F, yes ? prob_yes : 1.0F - prob_yes));
    return yes;
}

float ComputeEmpireScore(const Game& game, int side_index) {
    const Map& map = game.GetMap();
    if (side_index < 0 || side_index >= 8 || !map.Sides()[side_index].IsAlive()) {
        return 0.0F;
    }
    const bool hidden_map = map.GameOptions().hidden_map != 0;
    const game::SeenMasks seen =
            hidden_map ? game::BuildSeenMasks(game, side_index) : game::SeenMasks{};
    const Side* viewer = map.SideAt(side_index);
    const CityId quest_city_id = (viewer && viewer->ActiveQuest())
                                         ? viewer->ActiveQuest()->TargetCityId()
                                         : CityId::None();

    int total_standing = 0, total_income = 0;
    int my_cities = 0, my_income = 0;
    int revealed_cities = 0;
    for (const auto& city : map.Cities()) {
        if (city.IsRazed()) continue;
        total_standing++;
        total_income += std::max(1, city.Income());
        bool city_visible = !hidden_map || seen.all_visible || (city.Id() == quest_city_id);
        if (!city_visible) {
            for (int dy = 0; dy <= 1 && !city_visible; dy++) {
                for (int dx = 0; dx <= 1 && !city_visible; dx++) {
                    if (game::IsSeenAt(seen, city.X() + dx, city.Y() + dy)) city_visible = true;
                }
            }
        }
        if (city_visible) {
            revealed_cities++;
        }
        if (city.OwnerIndex() == side_index) {
            my_cities++;
            my_income += std::max(1, city.Income());
        }
    }
    total_standing = std::max(1, total_standing);
    total_income = std::max(1, total_income);

    float recon_term = 0.0F;
    if (hidden_map) {
        int sampled = 0, seen_sampled = 0;
        const int min_x = std::max(0, map.ActiveX0()),
                  max_x = std::min(map.Width() - 1, map.ActiveX1());
        const int min_y = std::max(0, map.ActiveY0()),
                  max_y = std::min(map.Height() - 1, map.ActiveY1());
        for (int y = min_y; y <= max_y; y += 2) {
            size_t row = static_cast<size_t>(y) * static_cast<size_t>(seen.width);
            for (int x = min_x; x <= max_x; x += 2) {
                sampled++;
                if (game::IsSeenIndex(seen, row + static_cast<size_t>(x))) seen_sampled++;
            }
        }
        float seen_tile_frac =
                static_cast<float>(seen_sampled) / static_cast<float>(std::max(1, sampled));
        float rev_city_frac =
                static_cast<float>(revealed_cities) / static_cast<float>(total_standing);
        recon_term = 0.08F * rev_city_frac + 0.02F * seen_tile_frac;
    }

    const float conc_weight = GlobalMlRuntimeConfig().ConcentrationWeight();
    float my_army_str = 0.0F, all_army_str = 0.0F;
    float my_synergy = 0.0F, all_synergy = 0.0F;
    for (const Army* army : game.Armies()) {
        if (army->IsDisbanded() || (army->X() == kNone && !army->InTransit()) ||
            army->OwnerIndex() < 0 || army->OwnerIndex() >= 8)
            continue;
        float power = static_cast<float>(army->Strength() * army->Strength());
        all_army_str += power;
        if (army->OwnerIndex() == side_index) my_army_str += power;

        if (conc_weight > 0.0F && army->OnMap()) {
            absl::Span<Army* const> tile_stack =
                    game::ArmiesOnTileSpan(game, army->X(), army->Y());
            int same_owner_cnt = 0;
            bool stack_has_hero = false;
            for (const Army* mate : tile_stack) {
                if (!mate->IsDisbanded() && mate->OwnerIndex() == army->OwnerIndex()) {
                    same_owner_cnt++;
                    if (mate->Hero()) stack_has_hero = true;
                }
            }
            int bless_bonus = std::min(2, static_cast<int>(army->Blessings().size()));
            int hero_bonus = (stack_has_hero && same_owner_cnt >= 2) ? 1 : 0;
            float eff_str = static_cast<float>(
                    std::min(9, army->Strength() + bless_bonus + hero_bonus));
            float eff_power = eff_str * eff_str;
            float size_frac = static_cast<float>(std::min(8, std::max(1, same_owner_cnt)) - 1) /
                              7.0F;
            float unit_synergy = eff_power * size_frac + (eff_power - power);
            all_synergy += unit_synergy;
            if (army->OwnerIndex() == side_index) my_synergy += unit_synergy;
        }
    }
    float army_share = all_army_str > 0.0F ? my_army_str / all_army_str : 0.0F;
    float conc_term = 0.0F;
    if (conc_weight > 0.0F && my_synergy > 0.0F) {
        float turn_ramp =
                std::clamp(static_cast<float>(game.Turn() - 4) / 16.0F, 0.25F, 1.0F);
        float conc_share = my_synergy / std::max(1.0F, all_army_str + all_synergy);
        conc_term = conc_weight * turn_ramp * conc_share;
    }

    float city_share = static_cast<float>(my_cities) / static_cast<float>(total_standing);
    float inc_share = static_cast<float>(my_income) / static_cast<float>(total_income);
    return 2.0F * city_share + 0.5F * inc_share +
           GlobalMlRuntimeConfig().ArmyWeight() * army_share + conc_term + recon_term;
}

PolicyNetwork& ActivePolicy() {
    static absl::NoDestructor<PolicyNetwork> instance([]() {
        PolicyNetwork net;
        if (!net.LoadJson("cpp/ai_weights.json").ok()) {
            net.LoadJson("ai_weights.json").IgnoreError();
        }
        return net;
    }());
    return *instance;
}

ContextVec BuildContext(const Game& game, const Side& side, const PolicyNetwork* policy) {
    const Map& map = game.GetMap();
    ContextVec context{};
    int our_side = side.Index();
    const SideBelief& belief = ai::belief::Ensure(const_cast<Side&>(side));

    int my_cities = 0, total_vis_cities = 0, max_cities = 0;
    int vis_neutral_cities = 0, vis_enemy_cities = 0;
    std::array<int, 8> side_cities{};
    for (City* city : belief.visible_cities) {
        if (city->IsRazed()) continue;
        total_vis_cities++;
        if (city->IsNeutral()) {
            vis_neutral_cities++;
        } else if (city->OwnerIndex() >= 0 && city->OwnerIndex() < 8) {
            side_cities[city->OwnerIndex()]++;
            max_cities = std::max(max_cities, side_cities[city->OwnerIndex()]);
            if (city->OwnerIndex() == our_side) {
                my_cities++;
            } else if (!diplomacy::IsAllied(game, our_side, city->OwnerIndex())) {
                vis_enemy_cities++;
            }
        }
    }

    int my_armies = 0, my_heroes = 0, my_strength = 0;
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() == our_side && !army->IsDisbanded() &&
            (army->X() != kNone || army->InTransit())) {
            my_armies++;
            my_strength += army->Strength();
            if (army->Hero()) my_heroes++;
        }
    }

    std::vector<char> garrisoned(map.Cities().size(), 0);
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() != our_side || army->IsDisbanded() || army->InTransit() ||
            army->X() == kNone)
            continue;
        if (City* city = game::CityAt(game, army->X(), army->Y())) {
            if (city->Index() >= 0 && city->Index() < static_cast<int>(garrisoned.size()))
                garrisoned[city->Index()] = 1;
        }
    }
    int own_cities = 0, empty_cities = 0, exposed_cities = 0;
    for (const auto& city : map.Cities()) {
        if (city.IsRazed() || city.OwnerIndex() != our_side) continue;
        own_cities++;
        if (city.Index() >= 0 && city.Index() < static_cast<int>(garrisoned.size()) &&
            garrisoned[city.Index()])
            continue;
        empty_cities++;
        if (city.Index() < static_cast<int>(belief.city_threat1_turn.size()) &&
            belief.city_threat1_turn[city.Index()] > 0.0F)
            exposed_cities++;
    }

    int wars = 0, allies = 0, alive_rivals = 0;
    for (int rival = 0; rival < 8; rival++) {
        if (rival != our_side && map.Sides()[rival].IsAlive()) {
            alive_rivals++;
            if (diplomacy::State(game, our_side, rival) == diplomacy::kWar) wars++;
            if (diplomacy::IsAllied(game, our_side, rival)) allies++;
        }
    }
    float rival_denom = static_cast<float>(std::max(1, alive_rivals));

    float cap_threat = 0.0F;
    const City* capital = side.CapitalCity(map);
    if (capital && capital->OwnerIndex() == our_side &&
        capital->Index() < static_cast<int>(belief.city_threat1_turn.size())) {
        cap_threat = std::tanh(belief.city_threat1_turn[capital->Index()] / 20.0F);
    }

    int income = game::Income(game, side);
    int upkeep = game::Upkeep(game, side);

    context[0] = std::tanh(static_cast<float>(game.Turn()) / 30.0F) * 2.0F - 1.0F;
    context[1] = static_cast<float>(my_cities) / static_cast<float>(std::max(1, total_vis_cities));
    context[2] = std::tanh(static_cast<float>(my_armies) / 24.0F);
    context[3] = std::tanh(static_cast<float>(side.Gold()) / 600.0F);
    context[4] = std::tanh(static_cast<float>(income - upkeep) / 40.0F);
    context[5] = std::tanh(static_cast<float>(vis_neutral_cities) / 6.0F);
    context[6] = std::tanh(static_cast<float>(vis_enemy_cities) / 6.0F);
    context[7] = static_cast<float>(wars) / rival_denom;
    context[8] = static_cast<float>(allies) / rival_denom;
    context[9] = static_cast<float>(max_cities) / static_cast<float>(std::max(1, total_vis_cities));
    context[10] = belief.frontier_ratio;
    context[11] = cap_threat;
    context[12] = (side.ActiveQuest() && side.ActiveQuest()->HasTarget()) ? 1.0F : 0.0F;
    context[13] = std::min(1.0F, static_cast<float>(my_heroes) / 3.0F);
    // v6 additions: non-saturating scales and empty-city exposure
    context[14] = 0.5F * std::log1p(static_cast<float>(std::max(0, side.Gold())) / 1000.0F);
    context[15] = std::min(2.0F, static_cast<float>(game.Turn()) / 50.0F);
    context[16] = 0.5F * std::log1p(static_cast<float>(my_strength) / 50.0F);
    context[17] = own_cities > 0 ? static_cast<float>(empty_cities) / static_cast<float>(own_cities)
                                 : 0.0F;
    context[18] = own_cities > 0
                          ? static_cast<float>(exposed_cities) / static_cast<float>(own_cities)
                          : 0.0F;
    context[19] = std::clamp(
            static_cast<float>(income - upkeep) / static_cast<float>(std::max(1, income)), -1.0F,
            1.0F);

    for (int i = 0; i < personality::kTraitDim; i++) {
        context[kGlobalDim + i] = 0.0F;
    }
    if (policy && policy->Version() >= 7) {
        context[20] = map.GameOptions().diplomacy == 0 ? 1.0F : 0.0F;
        context[21] = map.GameOptions().hidden_map != 0 ? 1.0F : 0.0F;
        context[22] = map.GameOptions().neutral_cities <= 0 ? 1.0F : 0.0F;
        context[23] = map.GameOptions().neutral_cities >= 2 ? 1.0F : 0.0F;
        int act_w = std::max(1, map.ActiveX1() - map.ActiveX0() + 1);
        int act_h = std::max(1, map.ActiveY1() - map.ActiveY0() + 1);
        context[24] = std::clamp(
                static_cast<float>(act_w * act_h) / static_cast<float>(kMapW * kMapH), 0.0F, 1.0F);
        for (int i = 0; i < 5; i++) {
            if (policy->UseCommander()) {
                float acc = policy->CommanderB()[i];
                for (int j = 0; j < PolicyNetwork::kGateInDim; j++) {
                    acc += policy->CommanderW()[static_cast<size_t>(i) * PolicyNetwork::kGateInDim +
                                                j] *
                           context[j];
                }
                context[25 + i] = std::tanh(acc);
            } else {
                context[25 + i] = policy->Posture()[i];
            }
        }
    }
    return context;
}

CriticVec BuildCriticFeatures(const Game& game, const Side& side) {
    const Map& map = game.GetMap();
    CriticVec features{};
    const int our_side = side.Index();
    std::array<int, 8> cities{}, income{};
    std::array<float, 8> power{};
    int standing = 0, neutral = 0, total_income = 0;
    for (const auto& city : map.Cities()) {
        if (city.IsRazed()) continue;
        standing++;
        total_income += std::max(1, city.Income());
        if (city.IsNeutral())
            neutral++;
        else if (city.OwnerIndex() >= 0 && city.OwnerIndex() < 8) {
            cities[city.OwnerIndex()]++;
            income[city.OwnerIndex()] += std::max(1, city.Income());
        }
    }
    float all_power = 0.0F;
    for (const Army* army : game.Armies()) {
        if (army->IsDisbanded() || (army->X() == kNone && !army->InTransit()) ||
            army->OwnerIndex() < 0 || army->OwnerIndex() >= 8)
            continue;
        float army_power = static_cast<float>(army->Strength() * army->Strength());
        power[army->OwnerIndex()] += army_power;
        all_power += army_power;
    }
    int alive = 0, in_use = 0, rivals = 0, classic_rivals = 0, richer_rivals = 0;
    int max_rival_cities = 0;
    float max_rival_power = 0.0F;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        const Side& other = map.Sides()[side_idx];
        if (other.InUse()) in_use++;
        if (!other.IsAlive()) continue;
        alive++;
        if (side_idx == our_side) continue;
        rivals++;
        if (other.IsClassicAi()) classic_rivals++;
        if (cities[side_idx] > cities[our_side]) richer_rivals++;
        max_rival_cities = std::max(max_rival_cities, cities[side_idx]);
        max_rival_power = std::max(max_rival_power, power[side_idx]);
    }

    constexpr int kThreatRadius = 6, kGuardRadius = 2;
    int my_cities = 0, threatened = 0;
    for (const auto& city : map.Cities()) {
        if (city.IsRazed() || city.OwnerIndex() != our_side) continue;
        my_cities++;
        float hostile = 0.0F, guard = 0.0F;
        for (const Army* army : game.Armies()) {
            if (army->IsDisbanded() || army->X() == kNone || army->OwnerIndex() < 0 ||
                army->OwnerIndex() >= 8)
                continue;
            int dist = std::max(std::abs(army->X() - city.X()), std::abs(army->Y() - city.Y()));
            if (army->OwnerIndex() == our_side) {
                if (dist <= kGuardRadius) guard += static_cast<float>(army->Strength());
            } else if (dist <= kThreatRadius &&
                       !diplomacy::IsAllied(game, our_side, army->OwnerIndex())) {
                hostile += static_cast<float>(army->Strength());
            }
        }
        if (hostile > guard) threatened++;
    }

    const float standing_denom = static_cast<float>(std::max(1, standing));
    features[0] = std::min(2.0F, static_cast<float>(game.Turn()) / 50.0F);
    features[1] = static_cast<float>(cities[our_side]) / standing_denom;
    features[2] =
            static_cast<float>(income[our_side]) / static_cast<float>(std::max(1, total_income));
    features[3] = all_power > 0.0F ? power[our_side] / all_power : 0.0F;
    features[4] = static_cast<float>(neutral) / standing_denom;
    features[5] = static_cast<float>(alive) / 8.0F;
    features[6] = static_cast<float>(in_use) / 8.0F;
    features[7] = static_cast<float>(standing) / 80.0F;
    features[8] = static_cast<float>(max_rival_cities) / standing_denom;
    features[9] = map.GameOptions().hidden_map != 0 ? 1.0F : 0.0F;
    features[10] = map.GameOptions().diplomacy != 0 ? 1.0F : 0.0F;
    features[11] =
            rivals > 0 ? static_cast<float>(classic_rivals) / static_cast<float>(rivals) : 0.0F;
    features[12] =
            my_cities > 0 ? static_cast<float>(threatened) / static_cast<float>(my_cities) : 0.0F;
    features[13] = 0.5F * ComputeEmpireScore(game, our_side);
    features[14] =
            rivals > 0 ? static_cast<float>(richer_rivals) / static_cast<float>(rivals) : 0.0F;
    features[15] = all_power > 0.0F ? max_rival_power / all_power : 0.0F;
    return features;
}

}  // namespace w2::ai::ml
