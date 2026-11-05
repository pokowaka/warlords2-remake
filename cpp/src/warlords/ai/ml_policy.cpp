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

template struct ResidualMLP<DIPLO_IN_DIM, 24, 12, DIPLO_OUT_DIM>;
template struct ResidualMLP<MISSION_IN_DIM, 32, 16, MISSION_OUT_DIM>;
template struct ResidualMLP<ECON_IN_DIM, 24, 12, ECON_OUT_DIM>;
template struct ResidualMLP<VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;

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

  auto initMlp = [&](auto& mlp, float inScale, float h1Scale) {
    initArr(mlp.wSkip, 0.04f);
    initArr(mlp.w1, inScale);
    mlp.b1.fill(0.0f);
    initArr(mlp.w2, h1Scale);
    mlp.b2.fill(0.0f);
    initArr(mlp.w3, 0.05f);
    mlp.b3.fill(0.0f);
  };

  initMlp(diploNet, 1.0f / std::sqrt((float)DIPLO_IN_DIM), 1.0f / std::sqrt(24.0f));
  initMlp(missionNet, 1.0f / std::sqrt((float)MISSION_IN_DIM), 1.0f / std::sqrt(32.0f));
  initMlp(econNet, 1.0f / std::sqrt((float)ECON_IN_DIM), 1.0f / std::sqrt(24.0f));
  initMlp(valueNet, 1.0f / std::sqrt((float)VALUE_IN_DIM), 1.0f / std::sqrt(24.0f));
}

void PolicyNetwork::zero() {
  diploNet.zero();
  missionNet.zero();
  econNet.zero();
  valueNet.zero();
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

DiploOutput PolicyNetwork::scoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const {
  auto raw = diploNet.forward(packDiploInput(ctx, rival));
  DiploOutput out;
  out.stanceLogits = {raw[0], raw[1], raw[2], raw[3]};
  out.tributeSignal = std::tanh(raw[4]);
  out.jointWarSignal = std::tanh(raw[5]);
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

float PolicyNetwork::evaluateValue(const ContextVec& ctx) const {
  return std::tanh(valueNet.forward(ctx)[0]);
}

std::vector<float> PolicyNetwork::flatten() const {
  std::vector<float> out;
  out.reserve(TOTAL_PARAMS);
  diploNet.toFlat(out);
  missionNet.toFlat(out);
  econNet.toFlat(out);
  valueNet.toFlat(out);
  return out;
}

void PolicyNetwork::unflatten(const std::vector<float>& params) {
  size_t off = 0;
  off = diploNet.fromFlat(params, off);
  off = missionNet.fromFlat(params, off);
  off = econNet.fromFlat(params, off);
  valueNet.fromFlat(params, off);
}

bool PolicyNetwork::saveJson(const std::filesystem::path& path, int generation, float elo) const {
  std::ofstream out(path);
  if (!out.is_open()) return false;
  auto weights = flatten();
  out << "{\n";
  out << "  \"version\": 2,\n";
  out << "  \"generation\": " << generation << ",\n";
  out << "  \"elo\": " << elo << ",\n";
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
    if ((int)arr.size() != TOTAL_PARAMS) return false;
    std::vector<float> weights(TOTAL_PARAMS);
    for (int i = 0; i < TOTAL_PARAMS; i++) {
      weights[i] = (float)arr[i].number();
    }
    unflatten(weights);
    return true;
  } catch (...) {
    return false;
  }
}

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
  if (!sampler || logits.size() == 1) {
    int best = 0;
    for (size_t i = 1; i < logits.size(); i++) {
      if (logits[i] > logits[best]) best = (int)i;
    }
    if (outLogProb) *outLogProb = 0.0f;
    return best;
  }

  float invT = 1.0f / std::max(0.05f, sampler->temperature);
  auto effZ = [&](size_t i) {
    float lm = (logMult && i < logMult->size()) ? (*logMult)[i] : 0.0f;
    return logits[i] * invT - lm;
  };
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

float computeEmpirePotential(const Game& g, int sideIndex) {
  if (sideIndex < 0 || sideIndex >= 8 || !g.map->sides[sideIndex].alive) {
    return -0.5f;
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

  float reconTerm = 0.20f;
  if (g.map->options.hiddenMap != 0) {
    int sampled = 0, seenSampled = 0;
    const int W = g.map->width, H = g.map->height;
    for (int y = 0; y < H; y += 2) {
      for (int x = 0; x < W; x += 2) {
        sampled++;
        if (game::seen(g, sideIndex, x, y)) seenSampled++;
      }
    }
    float seenTileFrac = (float)seenSampled / (float)std::max(1, sampled);
    float revCityFrac = (float)revealedCities / (float)totalStanding;
    reconTerm = 0.14f * revCityFrac + 0.06f * seenTileFrac;
  }

  float myArmyStr = 0.0f;
  for (const Army* a : g.armies) {
    if (a->owner == sideIndex && a->x != NONE) {
      myArmyStr += (float)a->strength;
    }
  }

  float cityShare = (float)myCities / (float)totalStanding;
  float incShare = (float)myIncome / (float)totalIncome;
  float citySublinear = std::sqrt((float)myCities) * 0.14f;
  float armyTerm = std::min(0.6f, myArmyStr * 0.0035f);
  return citySublinear + 1.5f * cityShare + 0.8f * incShare + armyTerm + reconTerm;
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
    if (a->owner == me && a->x != NONE) {
      myArmies++;
      if (a->hero()) myHeroes++;
    }
  }

  int wars = 0, allies = 0;
  for (int s = 0; s < 8; s++) {
    if (s != me && g.map->sides[s].alive) {
      if (diplomacy::state(g, me, s) == diplomacy::WAR) wars++;
      if (diplomacy::isAllied(g, me, s)) allies++;
    }
  }

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
  c[3] = std::tanh((float)side.gold / 300.0f);
  c[4] = std::tanh((float)(inc - upk) / 40.0f);
  c[5] = std::tanh((float)visNeutralCities / 6.0f);
  c[6] = std::tanh((float)visEnemyCities / 6.0f);
  c[7] = (float)wars / 7.0f;
  c[8] = (float)allies / 7.0f;
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
