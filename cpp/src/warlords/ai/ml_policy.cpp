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
std::array<float, OutDim> ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::forward(
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
    const float* skipRow = &wSkip[i * InDim];
    for (int j = 0; j < InDim; j++) s += skipRow[j] * x[j];
    const float* row3 = &w3[i * H2Dim];
    for (int j = 0; j < H2Dim; j++) s += row3[j] * h2[j];
    out[i] = s;
  }
  return out;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::backward(
    const std::array<float, InDim>& x, const std::array<float, OutDim>& dOut,
    ResidualMLP& gradAccum) const {
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
    float gOut = dOut[k];
    if (gOut == 0.0f) continue;
    gradAccum.b3[k] += gOut;
    float* gSkip = &gradAccum.wSkip[k * InDim];
    for (int j = 0; j < InDim; j++) gSkip[j] += gOut * x[j];
    float* gW3 = &gradAccum.w3[k * H2Dim];
    for (int j = 0; j < H2Dim; j++) gW3[j] += gOut * h2[j];
  }

  std::array<float, H2Dim> dz2{};
  for (int i = 0; i < H2Dim; i++) {
    float dh2 = 0.0f;
    for (int k = 0; k < OutDim; k++) dh2 += dOut[k] * w3[k * H2Dim + i];
    float g2 = dh2 * (1.0f - h2[i] * h2[i]);
    dz2[i] = g2;
    gradAccum.b2[i] += g2;
    float* gW2 = &gradAccum.w2[i * H1Dim];
    for (int j = 0; j < H1Dim; j++) gW2[j] += g2 * h1[j];
  }

  for (int j = 0; j < H1Dim; j++) {
    float dh1 = 0.0f;
    for (int i = 0; i < H2Dim; i++) dh1 += dz2[i] * w2[i * H1Dim + j];
    float g1 = dh1 * (1.0f - h1[j] * h1[j]);
    gradAccum.b1[j] += g1;
    float* gW1 = &gradAccum.w1[j * InDim];
    for (int m = 0; m < InDim; m++) gW1[m] += g1 * x[m];
  }
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::zero() {
  wSkip.fill(0.0f);
  w1.fill(0.0f);
  b1.fill(0.0f);
  w2.fill(0.0f);
  b2.fill(0.0f);
  w3.fill(0.0f);
  b3.fill(0.0f);
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
void ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::toFlat(std::vector<float>& out) const {
  out.insert(out.end(), wSkip.begin(), wSkip.end());
  out.insert(out.end(), w1.begin(), w1.end());
  out.insert(out.end(), b1.begin(), b1.end());
  out.insert(out.end(), w2.begin(), w2.end());
  out.insert(out.end(), b2.begin(), b2.end());
  out.insert(out.end(), w3.begin(), w3.end());
  out.insert(out.end(), b3.begin(), b3.end());
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::fromFlat(const std::vector<float>& in, size_t offset) {
  auto copySlice = [&](auto& arr) {
    for (size_t i = 0; i < arr.size() && offset < in.size(); i++) {
      arr[i] = in[offset++];
    }
  };
  copySlice(wSkip);
  copySlice(w1);
  copySlice(b1);
  copySlice(w2);
  copySlice(b2);
  copySlice(w3);
  copySlice(b3);
  return offset;
}

template <int InDim, int H1Dim, int H2Dim, int OutDim>
size_t ResidualMLP<InDim, H1Dim, H2Dim, OutDim>::fromFlatNarrowInput(const std::vector<float>& in, size_t offset,
                                                                      int oldInDim) {
  auto next = [&]() { return offset < in.size() ? in[offset++] : 0.0f; };
  auto copyMatrix = [&](auto& arr, int rows) {
    arr.fill(0.0f);
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < oldInDim; c++) arr[r * InDim + c] = next();
    }
  };
  auto copySlice = [&](auto& arr) {
    for (float& v : arr) v = next();
  };
  copyMatrix(wSkip, OutDim);
  copyMatrix(w1, H1Dim);
  copySlice(b1);
  copySlice(w2);
  copySlice(b2);
  copySlice(w3);
  copySlice(b3);
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
  uint32_t state = seed ? seed : 1u;
  auto nextSym = [&]() -> float {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    float u = (float)(state & 0x00ffffffu) / (float)0x00800000u - 1.0f;
    return u;
  };
  auto initArr = [&](auto& arr, float scale) {
    for (float& v : arr) {
      // Sum of 2 uniforms in [-1, 1] gives smooth zero-mean triangular distribution
      v = 0.5f * (nextSym() + nextSym()) * scale;
    }
  };

  auto initMlp = [&](auto& mlp) {
    using MlpT = std::decay_t<decltype(mlp)>;
    initArr(mlp.wSkip, 0.04f);
    initArr(mlp.w1, 1.0f / std::sqrt((float)MlpT::IN_DIM));
    mlp.b1.fill(0.0f);
    initArr(mlp.w2, 1.0f / std::sqrt((float)MlpT::H1_DIM));
    mlp.b2.fill(0.0f);
    initArr(mlp.w3, 0.2f / std::sqrt((float)MlpT::H2_DIM));
    mlp.b3.fill(0.0f);
  };

  initMlp(diploNet);
  initMlp(missionNet);
  initMlp(econNet);
  initMlp(valueNet);
  initMlp(stackValueNet);
}

void PolicyNetwork::zero() {
  diploNet.zero();
  missionNet.zero();
  econNet.zero();
  valueNet.zero();
  stackValueNet.zero();
}

DiploInVec packDiploInput(const ContextVec& ctx, const RivalVec& rival) {
  DiploInVec x{};
  std::copy(ctx.begin(), ctx.end(), x.begin());
  std::copy(rival.begin(), rival.end(), x.begin() + CONTEXT_DIM);
  return x;
}

MissionInVec packMissionInput(const ContextVec& ctx, const StackVec& stack, const TargetVec& target) {
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

StackValueInVec packStackValueInput(const ContextVec& ctx, const StackVec& stack, const CriticVec& critic) {
  StackValueInVec x{};
  std::copy(ctx.begin(), ctx.end(), x.begin());
  std::copy(stack.begin(), stack.end(), x.begin() + CONTEXT_DIM);
  std::copy(critic.begin(), critic.end(), x.begin() + CONTEXT_DIM + STACK_DIM);
  return x;
}

DiploOutput PolicyNetwork::scoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const {
  auto raw = diploNet.forward(packDiploInput(ctx, rival));
  DiploOutput out;
  out.stanceLogits = {raw[0], raw[1], raw[2], raw[3]};
  out.tributeLogit = raw[4];
  out.jointWarLogit = raw[5];
  return out;
}

MissionOutput PolicyNetwork::scoreMission(const ContextVec& ctx, const StackVec& stack,
                                          const TargetVec& target) const {
  auto raw = missionNet.forward(packMissionInput(ctx, stack, target));
  return MissionOutput{raw[0], raw[1]};
}

float PolicyNetwork::scoreEconomy(const ContextVec& ctx, const EconVec& econ) const {
  return econNet.forward(packEconInput(ctx, econ))[0];
}

float PolicyNetwork::evaluateValue(const ContextVec& ctx, const CriticVec& critic) const {
  // Linear: the critic predicts discounted return in empire-score units, which
  // is not bounded to [-1, 1]
  return valueNet.forward(packValueInput(ctx, critic))[0];
}

float PolicyNetwork::evaluateStackValue(const StackValueInVec& stackState) const {
  return stackValueNet.forward(stackState)[0];
}

std::vector<float> PolicyNetwork::flatten() const {
  std::vector<float> out;
  out.reserve(TOTAL_PARAMS);
  diploNet.toFlat(out);
  missionNet.toFlat(out);
  econNet.toFlat(out);
  valueNet.toFlat(out);
  stackValueNet.toFlat(out);
  return out;
}

void PolicyNetwork::unflatten(const std::vector<float>& params) {
  size_t off = 0;
  off = diploNet.fromFlat(params, off);
  off = missionNet.fromFlat(params, off);
  off = econNet.fromFlat(params, off);
  if (params.size() == (size_t)TOTAL_PARAMS) {
    off = valueNet.fromFlat(params, off);
    stackValueNet.fromFlat(params, off);
    return;
  }
  // v3/v4 files: critics without the privileged inputs (zero-initialised columns)
  off = valueNet.fromFlatNarrowInput(params, off, LEGACY_VALUE_IN_DIM);
  if (params.size() == (size_t)LEGACY_V4_PARAMS) {
    stackValueNet.fromFlatNarrowInput(params, off, LEGACY_STACK_VALUE_IN_DIM);
  } else {
    stackValueNet.zero();
  }
}

bool PolicyNetwork::saveJson(const std::filesystem::path& path, int generation, float selectionCityShare) const {
  std::ofstream out(path);
  if (!out.is_open()) return false;
  auto weights = flatten();
  out << "{\n";
  out << "  \"version\": 5,\n";
  out << "  \"generation\": " << generation << ",\n";
  out << "  \"selectionCityShare\": " << selectionCityShare << ",\n";
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
    int n = (int)arr.size();
    if (n != TOTAL_PARAMS && n != LEGACY_V4_PARAMS && n != LEGACY_V3_PARAMS) return false;
    std::vector<float> weights(n);
    for (int i = 0; i < n; i++) {
      weights[i] = (float)arr[i].number();
    }
    unflatten(weights);
    return true;
  } catch (...) {
    return false;
  }
}

namespace {
float g_deployTemperature = DEPLOY_TEMPERATURE;
}  // namespace

float deployTemperature() { return g_deployTemperature; }

void setDeployTemperature(float t) { g_deployTemperature = std::max(0.01f, t); }

float RolloutSampler::nextUniform01() {
  if (rngState == 0) rngState = 0x9e3779b9u;
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return (float)(rngState & 0x00ffffffu) / (float)0x01000000u;
}

int selectFromLogits(const std::vector<float>& logits, RolloutSampler* sampler,
                     float* outLogProb, const std::vector<float>* logMult) {
  if (logits.empty()) {
    if (outLogProb) *outLogProb = 0.0f;
    return 0;
  }
  // Option i has policy probability p_i ~ exp(-logMult_i) * exp(u_i / T), i.e.
  // effective logit z_i = u_i / T - logMult_i, so the multiplicity prior q_i ~ 1/M_i
  // keeps family probabilities invariant to temperature T rather than raising (1/M_i)
  // to the 1/T power at deployment temperature.
  auto lmOf = [&](size_t i) {
    return (logMult && i < logMult->size()) ? (*logMult)[i] : 0.0f;
  };
  if (!sampler || logits.size() == 1) {
    float deployInvT = 1.0f / deployTemperature();
    int best = 0;
    for (size_t i = 1; i < logits.size(); i++) {
      if (logits[i] * deployInvT - lmOf(i) > logits[best] * deployInvT - lmOf(best)) best = (int)i;
    }
    if (outLogProb) *outLogProb = 0.0f;
    return best;
  }

  float invT = 1.0f / std::max(0.05f, sampler->temperature);
  auto effZ = [&](size_t i) { return logits[i] * invT - lmOf(i); };
  float maxL = effZ(0);
  for (size_t i = 1; i < logits.size(); i++) {
    maxL = std::max(maxL, effZ(i));
  }
  std::vector<float> probs(logits.size());
  float sumExp = 0.0f;
  for (size_t i = 0; i < logits.size(); i++) {
    float e = std::exp(std::clamp(effZ(i) - maxL, -30.0f, 0.0f));
    probs[i] = e;
    sumExp += e;
  }
  for (float& p : probs) p /= std::max(1e-12f, sumExp);

  float u = sampler->nextUniform01();
  float acc = 0.0f;
  int chosen = (int)logits.size() - 1;
  for (size_t i = 0; i < probs.size(); i++) {
    acc += probs[i];
    if (u <= acc) {
      chosen = (int)i;
      break;
    }
  }
  if (outLogProb) {
    *outLogProb = std::log(std::max(1e-12f, probs[chosen]));
  }
  return chosen;
}

bool selectBinary(float logit, RolloutSampler* sampler, float* outLogProb) {
  if (!sampler) {
    if (outLogProb) *outLogProb = 0.0f;
    return logit > 0.0f;
  }
  float invT = 1.0f / std::max(0.05f, sampler->temperature);
  float z = std::clamp(logit * invT, -15.0f, 15.0f);
  float p = 1.0f / (1.0f + std::exp(-z));
  bool yes = sampler->nextUniform01() < p;
  if (outLogProb) *outLogProb = std::log(std::max(1e-12f, yes ? p : 1.0f - p));
  return yes;
}

float computeEmpireScore(const Game& g, int sideIndex) {
  if (sideIndex < 0 || sideIndex >= 8 || !g.map->sides[sideIndex].alive) {
    return 0.0f;
  }
  int totalStanding = 0, totalIncome = 0;
  int myCities = 0, myIncome = 0;
  int revealedCities = 0;
  for (const auto& c : g.map->cities) {
    if (c.razed) continue;
    totalStanding++;
    totalIncome += std::max(1, c.income);
    if (ai::belief::isCityVisible(g, sideIndex, c)) {
      revealedCities++;
    }
    if (c.ownerIndex == sideIndex) {
      myCities++;
      myIncome += std::max(1, c.income);
    }
  }
  totalStanding = std::max(1, totalStanding);
  totalIncome = std::max(1, totalIncome);

  // Reconnaissance only counts under Hidden Map, and stays small next to cities
  float reconTerm = 0.0f;
  if (g.map->options.hiddenMap != 0) {
    int sampled = 0, seenSampled = 0;
    const int x0 = std::max(0, g.map->activeX0), x1 = std::min(g.map->width - 1, g.map->activeX1);
    const int y0 = std::max(0, g.map->activeY0), y1 = std::min(g.map->height - 1, g.map->activeY1);
    for (int y = y0; y <= y1; y += 2) {
      for (int x = x0; x <= x1; x += 2) {
        sampled++;
        if (game::seen(g, sideIndex, x, y)) seenSampled++;
      }
    }
    float seenTileFrac = (float)seenSampled / (float)std::max(1, sampled);
    float revCityFrac = (float)revealedCities / (float)totalStanding;
    reconTerm = 0.08f * revCityFrac + 0.02f * seenTileFrac;
  }

  // Army combat power (Lanchester square law in sequential 1v1 duels, including
  // vectored units in transit) as a zero-sum share of all sides' forces.
  float myArmyStr = 0.0f, allArmyStr = 0.0f;
  for (const Army* a : g.armies) {
    if (a->disbanded || (a->x == NONE && !a->transit) || a->owner < 0 || a->owner >= 8) continue;
    float power = (float)(a->strength * a->strength);
    allArmyStr += power;
    if (a->owner == sideIndex) myArmyStr += power;
  }
  float armyShare = allArmyStr > 0.0f ? myArmyStr / allArmyStr : 0.0f;

  float cityShare = (float)myCities / (float)totalStanding;
  float incShare = (float)myIncome / (float)totalIncome;
  return 2.0f * cityShare + 0.5f * incShare + 0.25f * armyShare + reconTerm;
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

  int myCities = 0, totalVisCities = 0, maxCities = 0;
  int visNeutralCities = 0, visEnemyCities = 0;
  std::array<int, 8> sideCities{};
  for (City* city : b.visibleCities) {
    if (city->razed) continue;
    totalVisCities++;
    if (city->ownerIndex == NONE) {
      visNeutralCities++;
    } else if (city->ownerIndex >= 0 && city->ownerIndex < 8) {
      sideCities[city->ownerIndex]++;
      maxCities = std::max(maxCities, sideCities[city->ownerIndex]);
      if (city->ownerIndex == me) {
        myCities++;
      } else if (!diplomacy::isAllied(g, me, city->ownerIndex)) {
        visEnemyCities++;
      }
    }
  }

  int myArmies = 0, myHeroes = 0;
  for (Army* a : g.armies) {
    if (a->owner == me && !a->disbanded && (a->x != NONE || a->transit)) {
      myArmies++;
      if (a->hero()) myHeroes++;
    }
  }

  int wars = 0, allies = 0, aliveRivals = 0;
  for (int s = 0; s < 8; s++) {
    if (s != me && g.map->sides[s].alive) {
      aliveRivals++;
      if (diplomacy::state(g, me, s) == diplomacy::WAR) wars++;
      if (diplomacy::isAllied(g, me, s)) allies++;
    }
  }
  float rivalDenom = (float)std::max(1, aliveRivals);

  float capThreat = 0.0f;
  if (side.capital && side.capital->ownerIndex == me &&
      side.capital->index < (int)b.cityThreat1Turn.size()) {
    capThreat = std::tanh(b.cityThreat1Turn[side.capital->index] / 20.0f);
  }

  int inc = game::income(g, side);
  int upk = game::upkeep(g, side);

  c[0] = std::tanh((float)g.turn / 30.0f) * 2.0f - 1.0f;
  c[1] = (float)myCities / (float)std::max(1, totalVisCities);
  c[2] = std::tanh((float)myArmies / 24.0f);
  c[3] = std::tanh((float)side.gold / 600.0f);
  c[4] = std::tanh((float)(inc - upk) / 40.0f);
  c[5] = std::tanh((float)visNeutralCities / 6.0f);
  c[6] = std::tanh((float)visEnemyCities / 6.0f);
  c[7] = (float)wars / rivalDenom;
  c[8] = (float)allies / rivalDenom;
  c[9] = (float)maxCities / (float)std::max(1, totalVisCities);
  c[10] = b.frontierRatio;
  c[11] = capThreat;
  c[12] = (side.quest && side.quest->hasTarget()) ? 1.0f : 0.0f;
  c[13] = std::min(1.0f, (float)myHeroes / 3.0f);

  // Keep the 10D personality conditioning channels neutral (0.0f) for the base tabula-rasa
  // policy so no hand-authored trait biases fragment hidden-layer activations across sides.
  for (int i = 0; i < personality::TRAIT_DIM; i++) {
    c[GLOBAL_DIM + i] = 0.0f;
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
  int standing = 0, neutral = 0, totalIncome = 0;
  for (const auto& c : g.map->cities) {
    if (c.razed) continue;
    standing++;
    totalIncome += std::max(1, c.income);
    if (c.ownerIndex == NONE) neutral++;
    else if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
      cities[c.ownerIndex]++;
      income[c.ownerIndex] += std::max(1, c.income);
    }
  }
  float allPower = 0.0f;
  for (const Army* a : g.armies) {
    if (a->disbanded || (a->x == NONE && !a->transit) || a->owner < 0 || a->owner >= 8) continue;
    float p = (float)(a->strength * a->strength);
    power[a->owner] += p;
    allPower += p;
  }
  int alive = 0, inUse = 0, rivals = 0, classicRivals = 0, richerRivals = 0;
  int maxRivalCities = 0;
  float maxRivalPower = 0.0f;
  for (int s = 0; s < 8; s++) {
    const Side& o = g.map->sides[s];
    if (o.inUse) inUse++;
    if (!o.alive) continue;
    alive++;
    if (s == me) continue;
    rivals++;
    if (o.classicAi) classicRivals++;
    if (cities[s] > cities[me]) richerRivals++;
    maxRivalCities = std::max(maxRivalCities, cities[s]);
    maxRivalPower = std::max(maxRivalPower, power[s]);
  }

  // Cities where hostile strength within 6 tiles outweighs our own within 2
  constexpr int kThreatRadius = 6, kGuardRadius = 2;
  int myCities = 0, threatened = 0;
  for (const auto& c : g.map->cities) {
    if (c.razed || c.ownerIndex != me) continue;
    myCities++;
    float hostile = 0.0f, guard = 0.0f;
    for (const Army* a : g.armies) {
      if (a->disbanded || a->x == NONE || a->owner < 0 || a->owner >= 8) continue;
      int d = std::max(std::abs(a->x - c.x), std::abs(a->y - c.y));
      if (a->owner == me) {
        if (d <= kGuardRadius) guard += (float)a->strength;
      } else if (d <= kThreatRadius && !diplomacy::isAllied(g, me, a->owner)) {
        hostile += (float)a->strength;
      }
    }
    if (hostile > guard) threatened++;
  }

  const float st = (float)std::max(1, standing);
  f[0] = std::min(2.0f, (float)g.turn / 50.0f);
  f[1] = (float)cities[me] / st;
  f[2] = (float)income[me] / (float)std::max(1, totalIncome);
  f[3] = allPower > 0.0f ? power[me] / allPower : 0.0f;
  f[4] = (float)neutral / st;
  f[5] = (float)alive / 8.0f;
  f[6] = (float)inUse / 8.0f;
  f[7] = (float)standing / 80.0f;
  f[8] = (float)maxRivalCities / st;
  f[9] = g.map->options.hiddenMap != 0 ? 1.0f : 0.0f;
  f[10] = g.map->options.diplomacy != 0 ? 1.0f : 0.0f;
  f[11] = rivals > 0 ? (float)classicRivals / (float)rivals : 0.0f;
  f[12] = myCities > 0 ? (float)threatened / (float)myCities : 0.0f;
  f[13] = 0.5f * computeEmpireScore(g, me);
  f[14] = rivals > 0 ? (float)richerRivals / (float)rivals : 0.0f;
  f[15] = allPower > 0.0f ? maxRivalPower / allPower : 0.0f;
  return f;
}

}  // namespace w2::ai::ml
