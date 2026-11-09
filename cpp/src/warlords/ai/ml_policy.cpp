#include "warlords/ai/ml_policy.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "absl/base/no_destructor.h"
#include "absl/strings/str_format.h"

#include "nlohmann/json.hpp"
#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"

namespace w2::ai::ml {

template <int InDim, int H1Dim, int H2Dim, int OutDim>
std::array<float, OutDim> ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Forward(
        const std::array<float, InDim>& input) const {
    std::array<float, H1Dim> hidden1{};
    for (int i = 0; i < H1Dim; i++) {
        float sum = b1[i];
        const float* row = &w1[i * InDim];
        for (int j = 0; j < InDim; j++) sum += row[j] * input[j];
        hidden1[i] = std::tanh(sum);
    }

    std::array<float, H2Dim> hidden2{};
    for (int i = 0; i < H2Dim; i++) {
        float sum = b2[i];
        const float* row = &w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) sum += row[j] * hidden1[j];
        hidden2[i] = std::tanh(sum);
    }

    std::array<float, OutDim> out{};
    for (int i = 0; i < OutDim; i++) {
        float sum = b3[i];
        const float* skip_row = &w_skip[i * InDim];
        for (int j = 0; j < InDim; j++) sum += skip_row[j] * input[j];
        const float* row3 = &w3[i * H2Dim];
        for (int j = 0; j < H2Dim; j++) sum += row3[j] * hidden2[j];
        out[i] = sum;
    }
    return out;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Backward(const std::array<float, InDim>& input,
                                                        const std::array<float, OutDim>& d_out,
                                                        ResidualMLP& grad_accum) const {
    std::array<float, H1Dim> hidden1{};
    for (int i = 0; i < H1Dim; i++) {
        float sum = b1[i];
        const float* row = &w1[i * InDim];
        for (int j = 0; j < InDim; j++) sum += row[j] * input[j];
        hidden1[i] = std::tanh(sum);
    }

    std::array<float, H2Dim> hidden2{};
    for (int i = 0; i < H2Dim; i++) {
        float sum = b2[i];
        const float* row = &w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) sum += row[j] * hidden1[j];
        hidden2[i] = std::tanh(sum);
    }

    for (int out_idx = 0; out_idx < OutDim; out_idx++) {
        float grad_out = d_out[out_idx];
        if (grad_out == 0.0F) continue;
        grad_accum.b3[out_idx] += grad_out;
        float* grad_skip = &grad_accum.w_skip[out_idx * InDim];
        for (int j = 0; j < InDim; j++) grad_skip[j] += grad_out * input[j];
        float* grad_w3 = &grad_accum.w3[out_idx * H2Dim];
        for (int j = 0; j < H2Dim; j++) grad_w3[j] += grad_out * hidden2[j];
    }

    std::array<float, H2Dim> grad_z2{};
    for (int i = 0; i < H2Dim; i++) {
        float grad_hidden2 = 0.0F;
        for (int out_idx = 0; out_idx < OutDim; out_idx++)
            grad_hidden2 += d_out[out_idx] * w3[out_idx * H2Dim + i];
        float grad2 = grad_hidden2 * (1.0F - hidden2[i] * hidden2[i]);
        grad_z2[i] = grad2;
        grad_accum.b2[i] += grad2;
        float* grad_w2 = &grad_accum.w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) grad_w2[j] += grad2 * hidden1[j];
    }

    for (int j = 0; j < H1Dim; j++) {
        float grad_hidden1 = 0.0F;
        for (int i = 0; i < H2Dim; i++) grad_hidden1 += grad_z2[i] * w2[i * H1Dim + j];
        float grad1 = grad_hidden1 * (1.0F - hidden1[j] * hidden1[j]);
        grad_accum.b1[j] += grad1;
        float* grad_w1 = &grad_accum.w1[j * InDim];
        for (int in_idx = 0; in_idx < InDim; in_idx++) grad_w1[in_idx] += grad1 * input[in_idx];
    }
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
                if (new_col < InDim) arr[row * InDim + new_col] = weight;
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

PolicyNetwork::PolicyNetwork() {
    InitRandomWeights(42);
}

void PolicyNetwork::InitRandomWeights(uint32_t seed) {
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
            // Sum of 2 uniforms in [-1, 1] gives smooth zero-mean triangular distribution
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

    init_mlp(diplo_net);
    init_mlp(mission_net);
    init_mlp(econ_net);
    init_mlp(value_net);
    init_mlp(stack_value_net);
}

void PolicyNetwork::Zero() {
    diplo_net.Zero();
    mission_net.Zero();
    econ_net.Zero();
    value_net.Zero();
    stack_value_net.Zero();
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

DiploOutput PolicyNetwork::ScoreDiplomacy(const ContextVec& context, const RivalVec& rival) const {
    auto raw = diplo_net.Forward(PackDiploInput(context, rival));
    DiploOutput out;
    out.stance_logits = {raw[0], raw[1], raw[2], raw[3]};
    out.tribute_logit = raw[4];
    out.joint_war_logit = raw[5];
    return out;
}

MissionOutput PolicyNetwork::ScoreMission(const ContextVec& context, const StackVec& stack,
                                          const TargetVec& target) const {
    auto raw = mission_net.Forward(PackMissionInput(context, stack, target));
    return MissionOutput{raw[0], raw[1]};
}

float PolicyNetwork::ScoreEconomy(const ContextVec& context, const EconVec& econ) const {
    return econ_net.Forward(PackEconInput(context, econ))[0];
}

float PolicyNetwork::EvaluateValue(const ContextVec& context, const CriticVec& critic) const {
    // Linear: the critic predicts discounted return in empire-score units, which
    // is not bounded to [-1, 1]
    return value_net.Forward(PackValueInput(context, critic))[0];
}

float PolicyNetwork::EvaluateStackValue(const StackValueInVec& stack_state) const {
    return stack_value_net.Forward(stack_state)[0];
}

std::vector<float> PolicyNetwork::Flatten() const {
    std::vector<float> out;
    out.reserve(kTotalParams);
    diplo_net.ToFlat(out);
    mission_net.ToFlat(out);
    econ_net.ToFlat(out);
    value_net.ToFlat(out);
    stack_value_net.ToFlat(out);
    return out;
}

void PolicyNetwork::Unflatten(const std::vector<float>& params) {
    size_t offset = 0;
    offset = diplo_net.FromFlat(params, offset);
    offset = mission_net.FromFlat(params, offset);
    offset = econ_net.FromFlat(params, offset);
    offset = value_net.FromFlat(params, offset);
    stack_value_net.FromFlat(params, offset);
}

void PolicyNetwork::UnflattenLegacy(const std::vector<float>& params) {
    // Legacy files lack the 6 added context features (inserted after the 14
    // original ones); v3/v4 also lack the critics' privileged inputs (at the
    // end); v3 has no StackValueNet. Missing columns start at zero.
    const bool is_v5 = params.size() == static_cast<size_t>(kLegacyV5Params);
    const bool is_v3 = params.size() == static_cast<size_t>(kLegacyV3Params);
    const int insert_at = kLegacyGlobalDim, insert_count = kAddedGlobalDim;
    const int critic_drop = is_v5 ? kV5Dropped : kV4CriticDropped;
    size_t offset = 0;
    offset = diplo_net.FromFlatWidenInput(params, offset, DiploNet::kInDim - kV5Dropped, insert_at,
                                          insert_count);
    offset = mission_net.FromFlatWidenInput(params, offset, MissionNet::kInDim - kV5Dropped,
                                            insert_at, insert_count);
    offset = econ_net.FromFlatWidenInput(params, offset, EconNet::kInDim - kV5Dropped, insert_at,
                                         insert_count);
    offset = value_net.FromFlatWidenInput(params, offset, ValueNet::kInDim - critic_drop, insert_at,
                                          insert_count);
    if (is_v3) {
        stack_value_net.Zero();
    } else {
        stack_value_net.FromFlatWidenInput(params, offset, StackValueNet::kInDim - critic_drop,
                                           insert_at, insert_count);
    }
}

absl::Status PolicyNetwork::SaveJson(const std::filesystem::path& path, int generation,
                                     float selection_city_share) const {
    std::ostringstream out;
    auto weights = Flatten();
    out << "{\n";
    out << "  \"version\": 6,\n";
    out << "  \"generation\": " << generation << ",\n";
    out << "  \"selectionCityShare\": " << selection_city_share << ",\n";
    out << "  \"paramCount\": " << weights.size() << ",\n";
    out << "  \"weights\": [";
    for (size_t i = 0; i < weights.size(); i++) {
        if (i % 16 == 0) out << "\n    ";
        out << weights[i];
        if (i + 1 < weights.size()) out << ", ";
    }
    out << "\n  ]\n}\n";
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
                "Provide a valid v3..v6 ai_weights.json file.",
                path.string()));
    }
    const auto& arr = *weights_it;
    int param_count = static_cast<int>(arr.size());
    if (param_count != kTotalParams && param_count != kLegacyV5Params &&
        param_count != kLegacyV4Params && param_count != kLegacyV3Params) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load PolicyNetwork weights '%s': weight count %d does not match v6 (%d) "
                "or supported legacy counts (%d, %d, %d). Re-train or restore matching weights.",
                path.string(), param_count, kTotalParams, kLegacyV5Params, kLegacyV4Params,
                kLegacyV3Params));
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
    } else {
        UnflattenLegacy(weights);
    }
    return absl::OkStatus();
}

namespace {
/// Process-wide deployment sampling temperature, changeable via SetDeployTemperature().
float g_deploy_temperature = kDeployTemperature;
}  // namespace

float DeployTemperature() {
    return g_deploy_temperature;
}

void SetDeployTemperature(float temperature) {
    g_deploy_temperature = std::max(0.01F, temperature);
}

float RolloutSampler::NextUniform01() {
    if (rng_state == 0) rng_state = 0x9e3779b9U;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return static_cast<float>(rng_state & 0x00ffffffU) / static_cast<float>(0x01000000U);
}

int SelectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler, float* out_log_prob,
                     const std::vector<float>* log_mult) {
    if (logits.empty()) {
        if (out_log_prob) *out_log_prob = 0.0F;
        return 0;
    }
    // Option i has policy probability p_i ~ exp(-logMult_i) * exp(u_i / T), i.e.
    // effective logit z_i = u_i / T - logMult_i, so the multiplicity prior q_i ~ 1/M_i
    // keeps family probabilities invariant to temperature T rather than raising (1/M_i)
    // to the 1/T power at deployment temperature.
    auto log_mult_of = [&](size_t index) {
        return (log_mult && index < log_mult->size()) ? (*log_mult)[index] : 0.0F;
    };
    if (!sampler || logits.size() == 1) {
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

    float inv_t = 1.0F / std::max(0.05F, sampler->temperature);
    auto effective_logit = [&](size_t index) { return logits[index] * inv_t - log_mult_of(index); };
    float max_logit = effective_logit(0);
    for (size_t i = 1; i < logits.size(); i++) {
        max_logit = std::max(max_logit, effective_logit(i));
    }
    std::vector<float> probs(logits.size());
    float sum_exp = 0.0F;
    for (size_t i = 0; i < logits.size(); i++) {
        float exp_logit = std::exp(std::clamp(effective_logit(i) - max_logit, -30.0F, 0.0F));
        probs[i] = exp_logit;
        sum_exp += exp_logit;
    }
    for (float& prob : probs) prob /= std::max(1e-12F, sum_exp);

    float sample = sampler->NextUniform01();
    float acc = 0.0F;
    int chosen = static_cast<int>(logits.size()) - 1;
    for (size_t i = 0; i < probs.size(); i++) {
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
    float inv_t = 1.0F / std::max(0.05F, sampler->temperature);
    float scaled_logit = std::clamp(logit * inv_t, -15.0F, 15.0F);
    float prob_yes = 1.0F / (1.0F + std::exp(-scaled_logit));
    bool yes = sampler->NextUniform01() < prob_yes;
    if (out_log_prob) *out_log_prob = std::log(std::max(1e-12F, yes ? prob_yes : 1.0F - prob_yes));
    return yes;
}

float ComputeEmpireScore(const Game& game, int side_index) {
    if (side_index < 0 || side_index >= 8 || !game.map->sides[side_index].alive) {
        return 0.0F;
    }
    int total_standing = 0, total_income = 0;
    int my_cities = 0, my_income = 0;
    int revealed_cities = 0;
    for (const auto& city : game.map->cities) {
        if (city.razed) continue;
        total_standing++;
        total_income += std::max(1, city.income);
        if (ai::belief::IsCityVisible(game, side_index, city)) {
            revealed_cities++;
        }
        if (city.owner_index == side_index) {
            my_cities++;
            my_income += std::max(1, city.income);
        }
    }
    total_standing = std::max(1, total_standing);
    total_income = std::max(1, total_income);

    // Reconnaissance only counts under Hidden Map, and stays small next to cities
    float recon_term = 0.0F;
    if (game.map->options.hidden_map != 0) {
        int sampled = 0, seen_sampled = 0;
        const int min_x = std::max(0, game.map->active_x0),
                  max_x = std::min(game.map->width - 1, game.map->active_x1);
        const int min_y = std::max(0, game.map->active_y0),
                  max_y = std::min(game.map->height - 1, game.map->active_y1);
        for (int y = min_y; y <= max_y; y += 2) {
            for (int x = min_x; x <= max_x; x += 2) {
                sampled++;
                if (game::Seen(game, side_index, x, y)) seen_sampled++;
            }
        }
        float seen_tile_frac =
                static_cast<float>(seen_sampled) / static_cast<float>(std::max(1, sampled));
        float rev_city_frac =
                static_cast<float>(revealed_cities) / static_cast<float>(total_standing);
        recon_term = 0.08F * rev_city_frac + 0.02F * seen_tile_frac;
    }

    // Army combat power (Lanchester square law in sequential 1v1 duels, including
    // vectored units in transit) as a zero-sum share of all sides' forces.
    float my_army_str = 0.0F, all_army_str = 0.0F;
    for (const Army* army : game.armies) {
        if (army->disbanded || (army->x == kNone && !army->transit) || army->owner < 0 ||
            army->owner >= 8)
            continue;
        float power = static_cast<float>(army->strength * army->strength);
        all_army_str += power;
        if (army->owner == side_index) my_army_str += power;
    }
    float army_share = all_army_str > 0.0F ? my_army_str / all_army_str : 0.0F;

    float city_share = static_cast<float>(my_cities) / static_cast<float>(total_standing);
    float inc_share = static_cast<float>(my_income) / static_cast<float>(total_income);
    return 2.0F * city_share + 0.5F * inc_share + 0.25F * army_share + recon_term;
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

ContextVec BuildContext(const Game& game, const Side& side) {
    ContextVec context{};
    int our_side = side.index;
    const SideBelief& belief = ai::belief::Ensure(const_cast<Side&>(side));

    int my_cities = 0, total_vis_cities = 0, max_cities = 0;
    int vis_neutral_cities = 0, vis_enemy_cities = 0;
    std::array<int, 8> side_cities{};
    for (City* city : belief.visible_cities) {
        if (city->razed) continue;
        total_vis_cities++;
        if (city->owner_index == kNone) {
            vis_neutral_cities++;
        } else if (city->owner_index >= 0 && city->owner_index < 8) {
            side_cities[city->owner_index]++;
            max_cities = std::max(max_cities, side_cities[city->owner_index]);
            if (city->owner_index == our_side) {
                my_cities++;
            } else if (!diplomacy::IsAllied(game, our_side, city->owner_index)) {
                vis_enemy_cities++;
            }
        }
    }

    int my_armies = 0, my_heroes = 0, my_strength = 0;
    for (Army* army : game.armies) {
        if (army->owner == our_side && !army->disbanded && (army->x != kNone || army->transit)) {
            my_armies++;
            my_strength += army->strength;
            if (army->Hero()) my_heroes++;
        }
    }

    // Own cities without a single own army inside, and those of them a visible
    // hostile stack could reach within a turn (belief threat map: fair information)
    std::vector<char> garrisoned(game.map->cities.size(), 0);
    for (Army* army : game.armies) {
        if (army->owner != our_side || army->disbanded || army->transit || army->x == kNone)
            continue;
        if (City* city = game::CityAt(game, army->x, army->y)) {
            if (city->index >= 0 && city->index < static_cast<int>(garrisoned.size()))
                garrisoned[city->index] = 1;
        }
    }
    int own_cities = 0, empty_cities = 0, exposed_cities = 0;
    for (const auto& city : game.map->cities) {
        if (city.razed || city.owner_index != our_side) continue;
        own_cities++;
        if (city.index >= 0 && city.index < static_cast<int>(garrisoned.size()) &&
            garrisoned[city.index])
            continue;
        empty_cities++;
        if (city.index < static_cast<int>(belief.city_threat1_turn.size()) &&
            belief.city_threat1_turn[city.index] > 0.0F)
            exposed_cities++;
    }

    int wars = 0, allies = 0, alive_rivals = 0;
    for (int rival = 0; rival < 8; rival++) {
        if (rival != our_side && game.map->sides[rival].alive) {
            alive_rivals++;
            if (diplomacy::State(game, our_side, rival) == diplomacy::kWar) wars++;
            if (diplomacy::IsAllied(game, our_side, rival)) allies++;
        }
    }
    float rival_denom = static_cast<float>(std::max(1, alive_rivals));

    float cap_threat = 0.0F;
    if (side.capital && side.capital->owner_index == our_side &&
        side.capital->index < static_cast<int>(belief.city_threat1_turn.size())) {
        cap_threat = std::tanh(belief.city_threat1_turn[side.capital->index] / 20.0F);
    }

    int income = game::Income(game, side);
    int upkeep = game::Upkeep(game, side);

    context[0] = std::tanh(static_cast<float>(game.turn) / 30.0F) * 2.0F - 1.0F;
    context[1] = static_cast<float>(my_cities) / static_cast<float>(std::max(1, total_vis_cities));
    context[2] = std::tanh(static_cast<float>(my_armies) / 24.0F);
    context[3] = std::tanh(static_cast<float>(side.gold) / 600.0F);
    context[4] = std::tanh(static_cast<float>(income - upkeep) / 40.0F);
    context[5] = std::tanh(static_cast<float>(vis_neutral_cities) / 6.0F);
    context[6] = std::tanh(static_cast<float>(vis_enemy_cities) / 6.0F);
    context[7] = static_cast<float>(wars) / rival_denom;
    context[8] = static_cast<float>(allies) / rival_denom;
    context[9] = static_cast<float>(max_cities) / static_cast<float>(std::max(1, total_vis_cities));
    context[10] = belief.frontier_ratio;
    context[11] = cap_threat;
    context[12] = (side.quest && side.quest->HasTarget()) ? 1.0F : 0.0F;
    context[13] = std::min(1.0F, static_cast<float>(my_heroes) / 3.0F);
    // v6 additions: non-saturating scales and empty-city exposure
    context[14] = 0.5F * std::log1p(static_cast<float>(std::max(0, side.gold)) / 1000.0F);
    context[15] = std::min(2.0F, static_cast<float>(game.turn) / 50.0F);
    context[16] = 0.5F * std::log1p(static_cast<float>(my_strength) / 50.0F);
    context[17] = own_cities > 0 ? static_cast<float>(empty_cities) / static_cast<float>(own_cities)
                                 : 0.0F;
    context[18] = own_cities > 0
                          ? static_cast<float>(exposed_cities) / static_cast<float>(own_cities)
                          : 0.0F;
    context[19] = std::clamp(
            static_cast<float>(income - upkeep) / static_cast<float>(std::max(1, income)), -1.0F,
            1.0F);

    // Keep the 10D personality conditioning channels neutral (0.0f) for the base tabula-rasa
    // policy so no hand-authored trait biases fragment hidden-layer activations across sides.
    for (int i = 0; i < personality::kTraitDim; i++) {
        context[kGlobalDim + i] = 0.0F;
    }
    return context;
}

CriticVec BuildCriticFeatures(const Game& game, const Side& side) {
    CriticVec features{};
    const int our_side = side.index;
    std::array<int, 8> cities{}, income{};
    std::array<float, 8> power{};
    int standing = 0, neutral = 0, total_income = 0;
    for (const auto& city : game.map->cities) {
        if (city.razed) continue;
        standing++;
        total_income += std::max(1, city.income);
        if (city.owner_index == kNone)
            neutral++;
        else if (city.owner_index >= 0 && city.owner_index < 8) {
            cities[city.owner_index]++;
            income[city.owner_index] += std::max(1, city.income);
        }
    }
    float all_power = 0.0F;
    for (const Army* army : game.armies) {
        if (army->disbanded || (army->x == kNone && !army->transit) || army->owner < 0 ||
            army->owner >= 8)
            continue;
        float army_power = static_cast<float>(army->strength * army->strength);
        power[army->owner] += army_power;
        all_power += army_power;
    }
    int alive = 0, in_use = 0, rivals = 0, classic_rivals = 0, richer_rivals = 0;
    int max_rival_cities = 0;
    float max_rival_power = 0.0F;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        const Side& other = game.map->sides[side_idx];
        if (other.in_use) in_use++;
        if (!other.alive) continue;
        alive++;
        if (side_idx == our_side) continue;
        rivals++;
        if (other.IsClassicAi()) classic_rivals++;
        if (cities[side_idx] > cities[our_side]) richer_rivals++;
        max_rival_cities = std::max(max_rival_cities, cities[side_idx]);
        max_rival_power = std::max(max_rival_power, power[side_idx]);
    }

    // Cities where hostile strength within 6 tiles outweighs our own within 2
    constexpr int kThreatRadius = 6, kGuardRadius = 2;
    int my_cities = 0, threatened = 0;
    for (const auto& city : game.map->cities) {
        if (city.razed || city.owner_index != our_side) continue;
        my_cities++;
        float hostile = 0.0F, guard = 0.0F;
        for (const Army* army : game.armies) {
            if (army->disbanded || army->x == kNone || army->owner < 0 || army->owner >= 8)
                continue;
            int dist = std::max(std::abs(army->x - city.x), std::abs(army->y - city.y));
            if (army->owner == our_side) {
                if (dist <= kGuardRadius) guard += static_cast<float>(army->strength);
            } else if (dist <= kThreatRadius && !diplomacy::IsAllied(game, our_side, army->owner)) {
                hostile += static_cast<float>(army->strength);
            }
        }
        if (hostile > guard) threatened++;
    }

    const float standing_denom = static_cast<float>(std::max(1, standing));
    features[0] = std::min(2.0F, static_cast<float>(game.turn) / 50.0F);
    features[1] = static_cast<float>(cities[our_side]) / standing_denom;
    features[2] =
            static_cast<float>(income[our_side]) / static_cast<float>(std::max(1, total_income));
    features[3] = all_power > 0.0F ? power[our_side] / all_power : 0.0F;
    features[4] = static_cast<float>(neutral) / standing_denom;
    features[5] = static_cast<float>(alive) / 8.0F;
    features[6] = static_cast<float>(in_use) / 8.0F;
    features[7] = static_cast<float>(standing) / 80.0F;
    features[8] = static_cast<float>(max_rival_cities) / standing_denom;
    features[9] = game.map->options.hidden_map != 0 ? 1.0F : 0.0F;
    features[10] = game.map->options.diplomacy != 0 ? 1.0F : 0.0F;
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
