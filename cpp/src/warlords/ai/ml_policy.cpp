#include "warlords/ai/ml_policy.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "util/json.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"

namespace w2::ai::ml {

template <int InDim, int H1Dim, int H2Dim, int OutDim>
std::array<float, OutDim> ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Forward(
        const std::array<float, InDim>& x) const {
    std::array<float, H1Dim> h1{};
    for (int i = 0; i < H1Dim; i++) {
        float s = b1[i];
        const float* row = &w1[i * InDim];
        for (int j = 0; j < InDim; j++) s += row[j] * x[j];
        h1[i] = std::tanh(s);
    }

    std::array<float, H2Dim> h2{};
    for (int i = 0; i < H2Dim; i++) {
        float s = b2[i];
        const float* row = &w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) s += row[j] * h1[j];
        h2[i] = std::tanh(s);
    }

    std::array<float, OutDim> out{};
    for (int i = 0; i < OutDim; i++) {
        float s = b3[i];
        const float* skip_row = &wSkip[i * InDim];
        for (int j = 0; j < InDim; j++) s += skip_row[j] * x[j];
        const float* row3 = &w3[i * H2Dim];
        for (int j = 0; j < H2Dim; j++) s += row3[j] * h2[j];
        out[i] = s;
    }
    return out;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Backward(const std::array<float, InDim>& x,
                                                        const std::array<float, OutDim>& d_out,
                                                        ResidualMLP& grad_accum) const {
    std::array<float, H1Dim> h1{};
    for (int i = 0; i < H1Dim; i++) {
        float s = b1[i];
        const float* row = &w1[i * InDim];
        for (int j = 0; j < InDim; j++) s += row[j] * x[j];
        h1[i] = std::tanh(s);
    }

    std::array<float, H2Dim> h2{};
    for (int i = 0; i < H2Dim; i++) {
        float s = b2[i];
        const float* row = &w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) s += row[j] * h1[j];
        h2[i] = std::tanh(s);
    }

    for (int k = 0; k < OutDim; k++) {
        float g_out = d_out[k];
        if (g_out == 0.0F) continue;
        grad_accum.b3[k] += g_out;
        float* g_skip = &grad_accum.wSkip[k * InDim];
        for (int j = 0; j < InDim; j++) g_skip[j] += g_out * x[j];
        float* g_w3 = &grad_accum.w3[k * H2Dim];
        for (int j = 0; j < H2Dim; j++) g_w3[j] += g_out * h2[j];
    }

    std::array<float, H2Dim> dz2{};
    for (int i = 0; i < H2Dim; i++) {
        float dh2 = 0.0F;
        for (int k = 0; k < OutDim; k++) dh2 += d_out[k] * w3[k * H2Dim + i];
        float g2 = dh2 * (1.0F - h2[i] * h2[i]);
        dz2[i] = g2;
        grad_accum.b2[i] += g2;
        float* g_w2 = &grad_accum.w2[i * H1Dim];
        for (int j = 0; j < H1Dim; j++) g_w2[j] += g2 * h1[j];
    }

    for (int j = 0; j < H1Dim; j++) {
        float dh1 = 0.0F;
        for (int i = 0; i < H2Dim; i++) dh1 += dz2[i] * w2[i * H1Dim + j];
        float g1 = dh1 * (1.0F - h1[j] * h1[j]);
        grad_accum.b1[j] += g1;
        float* g_w1 = &grad_accum.w1[j * InDim];
        for (int m = 0; m < InDim; m++) g_w1[m] += g1 * x[m];
    }
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::Zero() {
    wSkip.fill(0.0F);
    w1.fill(0.0F);
    b1.fill(0.0F);
    w2.fill(0.0F);
    b2.fill(0.0F);
    w3.fill(0.0F);
    b3.fill(0.0F);
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::ToFlat(std::vector<float>& out) const {
    out.insert(out.end(), wSkip.begin(), wSkip.end());
    out.insert(out.end(), w1.begin(), w1.end());
    out.insert(out.end(), b1.begin(), b1.end());
    out.insert(out.end(), w2.begin(), w2.end());
    out.insert(out.end(), b2.begin(), b2.end());
    out.insert(out.end(), w3.begin(), w3.end());
    out.insert(out.end(), b3.begin(), b3.end());
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::FromFlat(const std::vector<float>& in,
                                                          size_t offset) {
    auto copy_slice = [&](auto& arr) {
        for (size_t i = 0; i < arr.size() && offset < in.size(); i++) {
            arr[i] = in[offset++];
        }
    };
    copy_slice(wSkip);
    copy_slice(w1);
    copy_slice(b1);
    copy_slice(w2);
    copy_slice(b2);
    copy_slice(w3);
    copy_slice(b3);
    return offset;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::FromFlatWidenInput(const std::vector<float>& in,
                                                                    size_t offset, int old_in_dim,
                                                                    int insert_at,
                                                                    int insert_count) {
    auto next = [&]() { return offset < in.size() ? in[offset++] : 0.0F; };
    auto copy_matrix = [&](auto& arr, int rows) {
        arr.fill(0.0F);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < old_in_dim; c++) {
                float v = next();
                int nc = c < insert_at ? c : c + insert_count;
                if (nc < InDim) arr[r * InDim + nc] = v;
            }
        }
    };
    auto copy_slice = [&](auto& arr) {
        for (float& v : arr) v = next();
    };
    copy_matrix(wSkip, OutDim);
    copy_matrix(w1, H1Dim);
    copy_slice(b1);
    copy_slice(w2);
    copy_slice(b2);
    copy_slice(w3);
    copy_slice(b3);
    return offset;
}

template struct ResidualMLP<DIPLO_IN_DIM, 24, 12, DIPLO_OUT_DIM>;
template struct ResidualMLP<MISSION_IN_DIM, 32, 16, MISSION_OUT_DIM>;
template struct ResidualMLP<ECON_IN_DIM, 24, 12, ECON_OUT_DIM>;
template struct ResidualMLP<VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;
template struct ResidualMLP<STACK_VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;

PolicyNetwork::PolicyNetwork() {
    initRandomWeights(42);
}

void PolicyNetwork::initRandomWeights(uint32_t seed) {
    uint32_t state = seed ? seed : 1U;
    auto next_sym = [&]() -> float {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        float u = static_cast<float>(state & 0x00ffffffU) / static_cast<float>(0x00800000U) - 1.0F;
        return u;
    };
    auto init_arr = [&](auto& arr, float scale) {
        for (float& v : arr) {
            // Sum of 2 uniforms in [-1, 1] gives smooth zero-mean triangular distribution
            v = 0.5F * (next_sym() + next_sym()) * scale;
        }
    };

    auto init_mlp = [&](auto& mlp) {
        using MlpT = std::decay_t<decltype(mlp)>;
        init_arr(mlp.wSkip, 0.04F);
        init_arr(mlp.w1, 1.0F / std::sqrt(static_cast<float>(MlpT::IN_DIM)));
        mlp.b1.fill(0.0F);
        init_arr(mlp.w2, 1.0F / std::sqrt(static_cast<float>(MlpT::H1_DIM)));
        mlp.b2.fill(0.0F);
        init_arr(mlp.w3, 0.2F / std::sqrt(static_cast<float>(MlpT::H2_DIM)));
        mlp.b3.fill(0.0F);
    };

    init_mlp(diploNet);
    init_mlp(missionNet);
    init_mlp(econNet);
    init_mlp(valueNet);
    init_mlp(stackValueNet);
}

void PolicyNetwork::zero() {
    diploNet.Zero();
    missionNet.Zero();
    econNet.Zero();
    valueNet.Zero();
    stackValueNet.Zero();
}

DiploInVec packDiploInput(const ContextVec& ctx, const RivalVec& rival) {
    DiploInVec x{};
    std::copy(ctx.begin(), ctx.end(), x.begin());
    std::copy(rival.begin(), rival.end(), x.begin() + CONTEXT_DIM);
    return x;
}

MissionInVec packMissionInput(const ContextVec& ctx, const StackVec& stack,
                              const TargetVec& target) {
    MissionInVec x{};
    std::copy(ctx.begin(), ctx.end(), x.begin());
    std::copy(stack.begin(), stack.end(), x.begin() + CONTEXT_DIM);
    std::copy(target.begin(), target.end(), x.begin() + CONTEXT_DIM + STACK_DIM);
    return x;
}

EconInVec packEconInput(const ContextVec& ctx, const EconVec& econ) {
    EconInVec x{};
    std::copy(ctx.begin(), ctx.end(), x.begin());
    std::copy(econ.begin(), econ.end(), x.begin() + CONTEXT_DIM);
    return x;
}

ValueInVec packValueInput(const ContextVec& ctx, const CriticVec& critic) {
    ValueInVec x{};
    std::copy(ctx.begin(), ctx.end(), x.begin());
    std::copy(critic.begin(), critic.end(), x.begin() + CONTEXT_DIM);
    return x;
}

StackValueInVec packStackValueInput(const ContextVec& ctx, const StackVec& stack,
                                    const CriticVec& critic) {
    StackValueInVec x{};
    std::copy(ctx.begin(), ctx.end(), x.begin());
    std::copy(stack.begin(), stack.end(), x.begin() + CONTEXT_DIM);
    std::copy(critic.begin(), critic.end(), x.begin() + CONTEXT_DIM + STACK_DIM);
    return x;
}

DiploOutput PolicyNetwork::scoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const {
    auto raw = diploNet.Forward(packDiploInput(ctx, rival));
    DiploOutput out;
    out.stanceLogits = {raw[0], raw[1], raw[2], raw[3]};
    out.tributeLogit = raw[4];
    out.jointWarLogit = raw[5];
    return out;
}

MissionOutput PolicyNetwork::scoreMission(const ContextVec& ctx, const StackVec& stack,
                                          const TargetVec& target) const {
    auto raw = missionNet.Forward(packMissionInput(ctx, stack, target));
    return MissionOutput{raw[0], raw[1]};
}

float PolicyNetwork::scoreEconomy(const ContextVec& ctx, const EconVec& econ) const {
    return econNet.Forward(packEconInput(ctx, econ))[0];
}

float PolicyNetwork::evaluateValue(const ContextVec& ctx, const CriticVec& critic) const {
    // Linear: the critic predicts discounted return in empire-score units, which
    // is not bounded to [-1, 1]
    return valueNet.Forward(packValueInput(ctx, critic))[0];
}

float PolicyNetwork::evaluateStackValue(const StackValueInVec& stack_state) const {
    return stackValueNet.Forward(stack_state)[0];
}

std::vector<float> PolicyNetwork::flatten() const {
    std::vector<float> out;
    out.reserve(TOTAL_PARAMS);
    diploNet.ToFlat(out);
    missionNet.ToFlat(out);
    econNet.ToFlat(out);
    valueNet.ToFlat(out);
    stackValueNet.ToFlat(out);
    return out;
}

void PolicyNetwork::unflatten(const std::vector<float>& params) {
    size_t off = 0;
    off = diploNet.FromFlat(params, off);
    off = missionNet.FromFlat(params, off);
    off = econNet.FromFlat(params, off);
    off = valueNet.FromFlat(params, off);
    stackValueNet.FromFlat(params, off);
}

void PolicyNetwork::unflattenLegacy(const std::vector<float>& params) {
    // Legacy files lack the 6 added context features (inserted after the 14
    // original ones); v3/v4 also lack the critics' privileged inputs (at the
    // end); v3 has no StackValueNet. Missing columns start at zero.
    const bool v5 = params.size() == static_cast<size_t>(LEGACY_V5_PARAMS);
    const bool v3 = params.size() == static_cast<size_t>(LEGACY_V3_PARAMS);
    const int at = LEGACY_GLOBAL_DIM, add = ADDED_GLOBAL_DIM;
    const int critic_drop = v5 ? V5_DROPPED : V4_CRITIC_DROPPED;
    size_t off = 0;
    off = diploNet.FromFlatWidenInput(params, off, DiploNet::IN_DIM - V5_DROPPED, at, add);
    off = missionNet.FromFlatWidenInput(params, off, MissionNet::IN_DIM - V5_DROPPED, at, add);
    off = econNet.FromFlatWidenInput(params, off, EconNet::IN_DIM - V5_DROPPED, at, add);
    off = valueNet.FromFlatWidenInput(params, off, ValueNet::IN_DIM - critic_drop, at, add);
    if (v3) {
        stackValueNet.Zero();
    } else {
        stackValueNet.FromFlatWidenInput(params, off, StackValueNet::IN_DIM - critic_drop, at, add);
    }
}

bool PolicyNetwork::saveJson(const std::filesystem::path& path, int generation,
                             float selection_city_share) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    auto weights = flatten();
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
    return true;
}

bool PolicyNetwork::loadJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::stringstream buf;
    buf << in.rdbuf();
    try {
        Json root = Json::parse(buf.str());
        const Json& arr = root["weights"];
        int n = static_cast<int>(arr.size());
        if (n != TOTAL_PARAMS && n != LEGACY_V5_PARAMS && n != LEGACY_V4_PARAMS &&
            n != LEGACY_V3_PARAMS)
            return false;
        std::vector<float> weights(n);
        for (int i = 0; i < n; i++) {
            weights[i] = static_cast<float>(arr[i].number());
        }
        if (n == TOTAL_PARAMS)
            unflatten(weights);
        else
            unflattenLegacy(weights);
        return true;
    } catch (...) {
        return false;
    }
}

namespace {
float g_deploy_temperature = DEPLOY_TEMPERATURE;
}  // namespace

float deployTemperature() {
    return g_deploy_temperature;
}

void setDeployTemperature(float t) {
    g_deploy_temperature = std::max(0.01F, t);
}

float RolloutSampler::nextUniform01() {
    if (rngState == 0) rngState = 0x9e3779b9U;
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return static_cast<float>(rngState & 0x00ffffffU) / static_cast<float>(0x01000000U);
}

int selectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler, float* out_log_prob,
                     const std::vector<float>* log_mult) {
    if (logits.empty()) {
        if (out_log_prob) *out_log_prob = 0.0F;
        return 0;
    }
    // Option i has policy probability p_i ~ exp(-logMult_i) * exp(u_i / T), i.e.
    // effective logit z_i = u_i / T - logMult_i, so the multiplicity prior q_i ~ 1/M_i
    // keeps family probabilities invariant to temperature T rather than raising (1/M_i)
    // to the 1/T power at deployment temperature.
    auto lm_of = [&](size_t i) {
        return (log_mult && i < log_mult->size()) ? (*log_mult)[i] : 0.0F;
    };
    if (!sampler || logits.size() == 1) {
        float deploy_inv_t = 1.0F / deployTemperature();
        int best = 0;
        for (size_t i = 1; i < logits.size(); i++) {
            if (logits[i] * deploy_inv_t - lm_of(i) > logits[best] * deploy_inv_t - lm_of(best))
                best = static_cast<int>(i);
        }
        if (out_log_prob) *out_log_prob = 0.0F;
        return best;
    }

    float inv_t = 1.0F / std::max(0.05F, sampler->temperature);
    auto eff_z = [&](size_t i) { return logits[i] * inv_t - lm_of(i); };
    float max_l = eff_z(0);
    for (size_t i = 1; i < logits.size(); i++) {
        max_l = std::max(max_l, eff_z(i));
    }
    std::vector<float> probs(logits.size());
    float sum_exp = 0.0F;
    for (size_t i = 0; i < logits.size(); i++) {
        float e = std::exp(std::clamp(eff_z(i) - max_l, -30.0F, 0.0F));
        probs[i] = e;
        sum_exp += e;
    }
    for (float& p : probs) p /= std::max(1e-12F, sum_exp);

    float u = sampler->nextUniform01();
    float acc = 0.0F;
    int chosen = static_cast<int>(logits.size()) - 1;
    for (size_t i = 0; i < probs.size(); i++) {
        acc += probs[i];
        if (u <= acc) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    if (out_log_prob) {
        *out_log_prob = std::log(std::max(1e-12F, probs[chosen]));
    }
    return chosen;
}

bool selectBinary(float logit, RolloutSampler* sampler, float* out_log_prob) {
    if (!sampler) {
        if (out_log_prob) *out_log_prob = 0.0F;
        return logit > 0.0F;
    }
    float inv_t = 1.0F / std::max(0.05F, sampler->temperature);
    float z = std::clamp(logit * inv_t, -15.0F, 15.0F);
    float p = 1.0F / (1.0F + std::exp(-z));
    bool yes = sampler->nextUniform01() < p;
    if (out_log_prob) *out_log_prob = std::log(std::max(1e-12F, yes ? p : 1.0F - p));
    return yes;
}

float computeEmpireScore(const Game& g, int side_index) {
    if (side_index < 0 || side_index >= 8 || !g.map->sides[side_index].alive) {
        return 0.0F;
    }
    int total_standing = 0, total_income = 0;
    int my_cities = 0, my_income = 0;
    int revealed_cities = 0;
    for (const auto& c : g.map->cities) {
        if (c.razed) continue;
        total_standing++;
        total_income += std::max(1, c.income);
        if (ai::belief::isCityVisible(g, side_index, c)) {
            revealed_cities++;
        }
        if (c.ownerIndex == side_index) {
            my_cities++;
            my_income += std::max(1, c.income);
        }
    }
    total_standing = std::max(1, total_standing);
    total_income = std::max(1, total_income);

    // Reconnaissance only counts under Hidden Map, and stays small next to cities
    float recon_term = 0.0F;
    if (g.map->options.hiddenMap != 0) {
        int sampled = 0, seen_sampled = 0;
        const int x0 = std::max(0, g.map->activeX0),
                  x1 = std::min(g.map->width - 1, g.map->activeX1);
        const int y0 = std::max(0, g.map->activeY0),
                  y1 = std::min(g.map->height - 1, g.map->activeY1);
        for (int y = y0; y <= y1; y += 2) {
            for (int x = x0; x <= x1; x += 2) {
                sampled++;
                if (game::seen(g, side_index, x, y)) seen_sampled++;
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
    for (const Army* a : g.armies) {
        if (a->disbanded || (a->x == NONE && !a->transit) || a->owner < 0 || a->owner >= 8)
            continue;
        float power = static_cast<float>(a->strength * a->strength);
        all_army_str += power;
        if (a->owner == side_index) my_army_str += power;
    }
    float army_share = all_army_str > 0.0F ? my_army_str / all_army_str : 0.0F;

    float city_share = static_cast<float>(my_cities) / static_cast<float>(total_standing);
    float inc_share = static_cast<float>(my_income) / static_cast<float>(total_income);
    return 2.0F * city_share + 0.5F * inc_share + 0.25F * army_share + recon_term;
}

PolicyNetwork& activePolicy() {
    static PolicyNetwork instance = []() {
        PolicyNetwork net;
        if (!net.loadJson("cpp/ai_weights.json")) {
            net.loadJson("ai_weights.json");
        }
        return net;
    }();
    return instance;
}

ContextVec buildContext(const Game& g, const Side& side) {
    ContextVec c{};
    int me = side.index;
    const SideBelief& b = ai::belief::ensure(const_cast<Side&>(side));

    int my_cities = 0, total_vis_cities = 0, max_cities = 0;
    int vis_neutral_cities = 0, vis_enemy_cities = 0;
    std::array<int, 8> side_cities{};
    for (City* city : b.visibleCities) {
        if (city->razed) continue;
        total_vis_cities++;
        if (city->ownerIndex == NONE) {
            vis_neutral_cities++;
        } else if (city->ownerIndex >= 0 && city->ownerIndex < 8) {
            side_cities[city->ownerIndex]++;
            max_cities = std::max(max_cities, side_cities[city->ownerIndex]);
            if (city->ownerIndex == me) {
                my_cities++;
            } else if (!diplomacy::isAllied(g, me, city->ownerIndex)) {
                vis_enemy_cities++;
            }
        }
    }

    int my_armies = 0, my_heroes = 0, my_strength = 0;
    for (Army* a : g.armies) {
        if (a->owner == me && !a->disbanded && (a->x != NONE || a->transit)) {
            my_armies++;
            my_strength += a->strength;
            if (a->hero()) my_heroes++;
        }
    }

    // Own cities without a single own army inside, and those of them a visible
    // hostile stack could reach within a turn (belief threat map: fair information)
    std::vector<char> garrisoned(g.map->cities.size(), 0);
    for (Army* a : g.armies) {
        if (a->owner != me || a->disbanded || a->transit || a->x == NONE) continue;
        if (City* in = game::cityAt(g, a->x, a->y)) {
            if (in->index >= 0 && in->index < static_cast<int>(garrisoned.size()))
                garrisoned[in->index] = 1;
        }
    }
    int own_cities = 0, empty_cities = 0, exposed_cities = 0;
    for (const auto& city : g.map->cities) {
        if (city.razed || city.ownerIndex != me) continue;
        own_cities++;
        if (city.index >= 0 && city.index < static_cast<int>(garrisoned.size()) &&
            garrisoned[city.index])
            continue;
        empty_cities++;
        if (city.index < static_cast<int>(b.cityThreat1Turn.size()) &&
            b.cityThreat1Turn[city.index] > 0.0F)
            exposed_cities++;
    }

    int wars = 0, allies = 0, alive_rivals = 0;
    for (int s = 0; s < 8; s++) {
        if (s != me && g.map->sides[s].alive) {
            alive_rivals++;
            if (diplomacy::state(g, me, s) == diplomacy::WAR) wars++;
            if (diplomacy::isAllied(g, me, s)) allies++;
        }
    }
    float rival_denom = static_cast<float>(std::max(1, alive_rivals));

    float cap_threat = 0.0F;
    if (side.capital && side.capital->ownerIndex == me &&
        side.capital->index < static_cast<int>(b.cityThreat1Turn.size())) {
        cap_threat = std::tanh(b.cityThreat1Turn[side.capital->index] / 20.0F);
    }

    int inc = game::income(g, side);
    int upk = game::upkeep(g, side);

    c[0] = std::tanh(static_cast<float>(g.turn) / 30.0F) * 2.0F - 1.0F;
    c[1] = static_cast<float>(my_cities) / static_cast<float>(std::max(1, total_vis_cities));
    c[2] = std::tanh(static_cast<float>(my_armies) / 24.0F);
    c[3] = std::tanh(static_cast<float>(side.gold) / 600.0F);
    c[4] = std::tanh(static_cast<float>(inc - upk) / 40.0F);
    c[5] = std::tanh(static_cast<float>(vis_neutral_cities) / 6.0F);
    c[6] = std::tanh(static_cast<float>(vis_enemy_cities) / 6.0F);
    c[7] = static_cast<float>(wars) / rival_denom;
    c[8] = static_cast<float>(allies) / rival_denom;
    c[9] = static_cast<float>(max_cities) / static_cast<float>(std::max(1, total_vis_cities));
    c[10] = b.frontierRatio;
    c[11] = cap_threat;
    c[12] = (side.quest && side.quest->hasTarget()) ? 1.0F : 0.0F;
    c[13] = std::min(1.0F, static_cast<float>(my_heroes) / 3.0F);
    // v6 additions: non-saturating scales and empty-city exposure
    c[14] = 0.5F * std::log1p(static_cast<float>(std::max(0, side.gold)) / 1000.0F);
    c[15] = std::min(2.0F, static_cast<float>(g.turn) / 50.0F);
    c[16] = 0.5F * std::log1p(static_cast<float>(my_strength) / 50.0F);
    c[17] = own_cities > 0 ? static_cast<float>(empty_cities) / static_cast<float>(own_cities)
                           : 0.0F;
    c[18] = own_cities > 0 ? static_cast<float>(exposed_cities) / static_cast<float>(own_cities)
                           : 0.0F;
    c[19] = std::clamp(static_cast<float>(inc - upk) / static_cast<float>(std::max(1, inc)), -1.0F,
                       1.0F);

    // Keep the 10D personality conditioning channels neutral (0.0f) for the base tabula-rasa
    // policy so no hand-authored trait biases fragment hidden-layer activations across sides.
    for (int i = 0; i < personality::TRAIT_DIM; i++) {
        c[GLOBAL_DIM + i] = 0.0F;
    }
    return c;
}

}  // namespace w2::ai::ml

namespace w2::ai::ml {

CriticVec buildCriticFeatures(const Game& g, const Side& side) {
    CriticVec f{};
    const int me = side.index;
    std::array<int, 8> cities{}, income{};
    std::array<float, 8> power{};
    int standing = 0, neutral = 0, total_income = 0;
    for (const auto& c : g.map->cities) {
        if (c.razed) continue;
        standing++;
        total_income += std::max(1, c.income);
        if (c.ownerIndex == NONE)
            neutral++;
        else if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
            cities[c.ownerIndex]++;
            income[c.ownerIndex] += std::max(1, c.income);
        }
    }
    float all_power = 0.0F;
    for (const Army* a : g.armies) {
        if (a->disbanded || (a->x == NONE && !a->transit) || a->owner < 0 || a->owner >= 8)
            continue;
        float p = static_cast<float>(a->strength * a->strength);
        power[a->owner] += p;
        all_power += p;
    }
    int alive = 0, in_use = 0, rivals = 0, classic_rivals = 0, richer_rivals = 0;
    int max_rival_cities = 0;
    float max_rival_power = 0.0F;
    for (int s = 0; s < 8; s++) {
        const Side& o = g.map->sides[s];
        if (o.inUse) in_use++;
        if (!o.alive) continue;
        alive++;
        if (s == me) continue;
        rivals++;
        if (o.classicAi) classic_rivals++;
        if (cities[s] > cities[me]) richer_rivals++;
        max_rival_cities = std::max(max_rival_cities, cities[s]);
        max_rival_power = std::max(max_rival_power, power[s]);
    }

    // Cities where hostile strength within 6 tiles outweighs our own within 2
    constexpr int kThreatRadius = 6, kGuardRadius = 2;
    int my_cities = 0, threatened = 0;
    for (const auto& c : g.map->cities) {
        if (c.razed || c.ownerIndex != me) continue;
        my_cities++;
        float hostile = 0.0F, guard = 0.0F;
        for (const Army* a : g.armies) {
            if (a->disbanded || a->x == NONE || a->owner < 0 || a->owner >= 8) continue;
            int d = std::max(std::abs(a->x - c.x), std::abs(a->y - c.y));
            if (a->owner == me) {
                if (d <= kGuardRadius) guard += static_cast<float>(a->strength);
            } else if (d <= kThreatRadius && !diplomacy::isAllied(g, me, a->owner)) {
                hostile += static_cast<float>(a->strength);
            }
        }
        if (hostile > guard) threatened++;
    }

    const float st = static_cast<float>(std::max(1, standing));
    f[0] = std::min(2.0F, static_cast<float>(g.turn) / 50.0F);
    f[1] = static_cast<float>(cities[me]) / st;
    f[2] = static_cast<float>(income[me]) / static_cast<float>(std::max(1, total_income));
    f[3] = all_power > 0.0F ? power[me] / all_power : 0.0F;
    f[4] = static_cast<float>(neutral) / st;
    f[5] = static_cast<float>(alive) / 8.0F;
    f[6] = static_cast<float>(in_use) / 8.0F;
    f[7] = static_cast<float>(standing) / 80.0F;
    f[8] = static_cast<float>(max_rival_cities) / st;
    f[9] = g.map->options.hiddenMap != 0 ? 1.0F : 0.0F;
    f[10] = g.map->options.diplomacy != 0 ? 1.0F : 0.0F;
    f[11] = rivals > 0 ? static_cast<float>(classic_rivals) / static_cast<float>(rivals) : 0.0F;
    f[12] = my_cities > 0 ? static_cast<float>(threatened) / static_cast<float>(my_cities) : 0.0F;
    f[13] = 0.5F * computeEmpireScore(g, me);
    f[14] = rivals > 0 ? static_cast<float>(richer_rivals) / static_cast<float>(rivals) : 0.0F;
    f[15] = all_power > 0.0F ? max_rival_power / all_power : 0.0F;
    return f;
}

}  // namespace w2::ai::ml
