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

// Explicit template instantiations
template struct ResidualMLP<DIPLO_IN_DIM, 24, 12, DIPLO_OUT_DIM>;
template struct ResidualMLP<MISSION_IN_DIM, 32, 16, MISSION_OUT_DIM>;
template struct ResidualMLP<ECON_IN_DIM, 24, 12, ECON_OUT_DIM>;
template struct ResidualMLP<VALUE_IN_DIM, 24, 12, VALUE_OUT_DIM>;

PolicyNetwork::PolicyNetwork() {
  initPriorWeights(42);
}

void PolicyNetwork::initPriorWeights(uint32_t seed) {
  Rng rng(seed);
  auto initSmall = [&](auto& arr, float scale) {
    for (float& v : arr) {
      v = ((float)rng.dice(1, 2001, -1001) / 1000.0f) * scale;
    }
  };

  // Initialize hidden layers with small symmetry-breaking weights so gradients/mutations
  // activate non-linear hidden units immediately
  initSmall(diploNet.wSkip, 0.02f);
  initSmall(diploNet.w1, 0.12f);
  diploNet.b1.fill(0.0f);
  initSmall(diploNet.w2, 0.12f);
  diploNet.b2.fill(0.0f);
  initSmall(diploNet.w3, 0.08f);
  diploNet.b3.fill(0.0f);

  initSmall(missionNet.wSkip, 0.02f);
  initSmall(missionNet.w1, 0.12f);
  missionNet.b1.fill(0.0f);
  initSmall(missionNet.w2, 0.12f);
  missionNet.b2.fill(0.0f);
  initSmall(missionNet.w3, 0.08f);
  missionNet.b3.fill(0.0f);

  initSmall(econNet.wSkip, 0.02f);
  initSmall(econNet.w1, 0.12f);
  econNet.b1.fill(0.0f);
  initSmall(econNet.w2, 0.12f);
  econNet.b2.fill(0.0f);
  initSmall(econNet.w3, 0.08f);
  econNet.b3.fill(0.0f);

  initSmall(valueNet.wSkip, 0.02f);
  initSmall(valueNet.w1, 0.12f);
  valueNet.b1.fill(0.0f);
  initSmall(valueNet.w2, 0.12f);
  valueNet.b2.fill(0.0f);
  initSmall(valueNet.w3, 0.08f);
  valueNet.b3.fill(0.0f);

  // Set interpretable skip-connection priors so Generation 0 already plays coherently
  // before CMA-ES refines all weights in self-play:
  auto setDiploSkip = [&](int outIdx, int inIdx, float w) {
    diploNet.wSkip[outIdx * DIPLO_IN_DIM + inIdx] = w;
  };
  // Context indices: 6=activeWars, 14=AGGRESSION, 15=HONOUR, 16=AVARICE, 19=COALITION_DRIVE, 23=CAUTION
  // Rival indices (offset 24):
  // 0=isAllied, 1=isPeace, 2=isUneasy, 3=isWar, 4=truceLeft, 5=trust, 6=attitudeScore,
  // 7=myBorderStr, 8=theirBorderStr, 9=borderAdv, 10=theirCityShare, 11=isRunaway,
  // 12=sharedWars, 13=grudge, 14=holdMyCap, 15=goldOffered, 16=dist, 17=otherWars
  const int R0 = CONTEXT_DIM;
  // Stance 0: ALLIED
  setDiploSkip(0, 15, 0.9f);        // HONOUR
  setDiploSkip(0, 19, 1.2f);        // COALITION_DRIVE
  setDiploSkip(0, R0 + 0, 1.5f);    // hysteresis: stay Allied
  setDiploSkip(0, R0 + 5, 1.4f);    // trust
  setDiploSkip(0, R0 + 6, 1.8f);    // attitudeScore
  setDiploSkip(0, R0 + 12, 1.6f);   // sharedWarEnemies
  setDiploSkip(0, R0 + 14, -4.0f);  // never ally if they hold our capital
  diploNet.b3[0] = -0.8f;

  // Stance 1: PEACE
  setDiploSkip(1, 15, 0.8f);        // HONOUR
  setDiploSkip(1, 23, 0.8f);        // CAUTION
  setDiploSkip(1, R0 + 1, 0.35f);   // mild hysteresis: stay Peace
  setDiploSkip(1, R0 + 4, 2.2f);    // active truce strongly preserves peace
  setDiploSkip(1, R0 + 6, 1.4f);    // attitudeScore
  setDiploSkip(1, R0 + 11, -1.8f);  // don't stay passive with primary expansion target / leader
  setDiploSkip(1, R0 + 15, 1.5f);   // gold offered to us
  setDiploSkip(1, R0 + 16, 1.4f);   // far away -> stay at peace
  setDiploSkip(1, R0 + 17, 1.6f);   // already fighting other wars -> keep peace here
  diploNet.b3[1] = 0.0f;

  // Stance 2: UNEASY (staging / border friction)
  setDiploSkip(2, 14, 0.8f);        // AGGRESSION
  setDiploSkip(2, R0 + 2, 0.6f);    // hysteresis
  setDiploSkip(2, R0 + 7, 1.2f);    // our border stacks near them
  setDiploSkip(2, R0 + 8, 1.2f);    // their border stacks near us
  setDiploSkip(2, R0 + 10, 1.0f);   // visible cities to contest
  setDiploSkip(2, R0 + 11, 1.6f);   // primary expansion rival / leader
  setDiploSkip(2, R0 + 13, 1.0f);   // grudge
  diploNet.b3[2] = 0.1f;

  // Stance 3: WAR
  setDiploSkip(3, 6, -1.2f);        // open a war when activeWars == 0; restrain when already at war
  setDiploSkip(3, 14, 1.5f);        // AGGRESSION
  setDiploSkip(3, 15, -0.7f);       // HONOUR restrains unprovoked war
  setDiploSkip(3, R0 + 3, 1.4f);    // war commitment hysteresis
  setDiploSkip(3, R0 + 4, -2.5f);   // truce discourages war
  setDiploSkip(3, R0 + 6, -1.6f);   // negative attitude drives war
  setDiploSkip(3, R0 + 7, 1.8f);    // we have border strength ready
  setDiploSkip(3, R0 + 9, 1.8f);    // border advantage
  setDiploSkip(3, R0 + 10, 1.3f);   // visible cities of theirs
  setDiploSkip(3, R0 + 11, 2.2f);   // primary expansion target / runaway leader
  setDiploSkip(3, R0 + 13, 1.5f);   // grudge
  setDiploSkip(3, R0 + 14, 3.5f);   // they hold our capital!
  setDiploSkip(3, R0 + 16, -1.4f);  // don't declare war on unreachable far sides
  setDiploSkip(3, R0 + 17, -1.6f);  // avoid multi-front war overload
  diploNet.b3[3] = 0.25f;

  // Tribute & Joint War signals
  setDiploSkip(4, 16, 1.5f);        // AVARICE drives demanding tribute
  setDiploSkip(4, R0 + 9, 1.2f);    // demand tribute when we have border advantage
  setDiploSkip(5, 19, 1.5f);        // COALITION_DRIVE drives joint war requests
  setDiploSkip(5, R0 + 11, 1.4f);

  // Head 2: Mission & Coalition Head priors
  auto setMissionSkip = [&](int outIdx, int inIdx, float w) {
    missionNet.wSkip[outIdx * MISSION_IN_DIM + inIdx] = w;
  };
  const int S0 = CONTEXT_DIM;
  const int T0 = CONTEXT_DIM + STACK_DIM;
  // Stack features S0 +:
  // 0=size, 1=totalStr, 2=avgStr, 3=minMove, 4=hasHero, 5=hasFlier, 6=allFly,
  // 7=hasSiege, 8=hasSpecial, 9=bless, 10=upkeep, 11=heroCmd, 12=isSingleScout, 13=marginalSpeedDrop
  // Target features T0 +:
  // 0=isHoldGarrison, 1=isNeutralCity, 2=isEnemyCity, 3=isEnemyStack, 4=isRuinOrSage,
  // 5=isTempleOrItem, 6=isFogFrontier, 7=isReinforceFront, 8=turnsToReach, 9=winProbNow,
  // 10=winProbNextTurn, 11=expectedSurvRatio, 12=value, 13=isCapOrQuest, 14=threatAtTarget,
  // 15=marginalHoldGain, 16=alreadyAssigned, 17=stagingRisk
  setMissionSkip(0, S0 + 0, -0.55f);  // slight opportunity cost of tying up extra units once winProb saturates
  setMissionSkip(0, S0 + 1, 0.20f);   // stack strength
  setMissionSkip(0, S0 + 3, 0.7f);    // faster stack
  setMissionSkip(0, S0 + 7, 0.6f);    // siege bonus
  setMissionSkip(0, S0 + 11, 0.7f);   // hero command bonus
  setMissionSkip(0, S0 + 12, 0.9f);   // single-unit scout efficiency on exploration targets
  setMissionSkip(0, S0 + 13, -0.8f);  // penalize slowing down a fast stack unnecessarily

  setMissionSkip(0, T0 + 0, 1.8f);    // HoldGarrison base (augmented by marginalHoldGain when needed)
  setMissionSkip(0, T0 + 1, 3.5f);    // Neutral city expansion
  setMissionSkip(0, T0 + 2, 3.8f);    // Enemy city capture
  setMissionSkip(0, T0 + 3, 2.2f);    // Enemy field stack
  setMissionSkip(0, T0 + 4, 6.8f);    // Ruin / Sage (for heroes)
  setMissionSkip(0, T0 + 5, 5.6f);    // Temple / Item
  setMissionSkip(0, T0 + 6, 4.4f);    // Fog Frontier exploration
  setMissionSkip(0, T0 + 7, 3.4f);    // Reinforce frontline city (augmented by marginalHoldGain)
  setMissionSkip(0, T0 + 8, -2.4f);   // Strongly prefer closer targets (fewer turns to reach)
  setMissionSkip(0, T0 + 9, 4.8f);    // High immediate win probability (combat targets)
  setMissionSkip(0, T0 + 10, 2.6f);   // High next-turn coordinated win probability (multi-stack convergence)
  setMissionSkip(0, T0 + 11, 2.5f);   // High expected survivor ratio
  setMissionSkip(0, T0 + 12, 1.6f);   // Target city income / value
  setMissionSkip(0, T0 + 13, 2.0f);   // Capital or Quest objective
  setMissionSkip(0, T0 + 15, 4.8f);   // Marginal city hold gain (locks needed garrison, releases surplus!)
  setMissionSkip(0, T0 + 16, -2.5f);  // Avoid crowding targets that already have enough stacks

  // Second output of Mission Head: StageNear (>0) vs AttackNow (<=0)
  setMissionSkip(1, 23, 0.8f);        // CAUTION personality prefers staging when needed
  setMissionSkip(1, 14, -0.6f);       // AGGRESSION prefers attacking now
  setMissionSkip(1, T0 + 9, -3.5f);   // If winProbNow is high, AttackNow (negative logit)
  setMissionSkip(1, T0 + 10, 2.8f);   // If winProbNextTurn is much higher than winProbNow, StageNear!
  setMissionSkip(1, T0 + 17, -1.5f);  // High risk on staging tile discourages staging
  missionNet.b3[1] = -0.3f;

  // Head 3: Unified Economy, Retention & Spoils priors
  auto setEconSkip = [&](int inIdx, float w) {
    econNet.wSkip[inIdx] = w;
  };
  const int E0 = CONTEXT_DIM;
  // Econ features E0 +:
  // 0=isHoldTreasury, 1=isProduceSlot, 2=isStopProd, 3=isBuySlot, 4=isVectorRoute,
  // 5=isRetainArmy, 6=isSpoilsChoice, 7=unitStr, 8=unitMove, 9=buildTurns,
  // 10=costOrUpkeep, 11=isFlier, 12=isSiegeOrSpec, 13=isCavalry, 14=frontlineThreat,
  // 15=distToEnemy, 16=garrisonFill, 17=vectorThreatGain, 18=spoilsGold, 19=spoilsDiploPen
  setEconSkip(3, 0.8f);               // high treasury favors buying new slots
  setEconSkip(4, 0.8f);               // positive net income favors spending/retaining
  setEconSkip(E0 + 0, 0.4f);          // HoldTreasury baseline
  setEconSkip(E0 + 1, 1.2f);          // Produce unit
  setEconSkip(E0 + 2, -1.2f);         // Stop production (only wins when upkeep is crushing & garrison full)
  setEconSkip(E0 + 3, -1.1f);         // BuyProductionSlot baseline (only buy when treasury is high & unit is elite/flier)
  setEconSkip(E0 + 5, 2.2f);          // Retain existing army baseline (positive so healthy armies aren't disbanded)
  setEconSkip(E0 + 7, 2.2f);          // Unit strength
  setEconSkip(E0 + 8, 1.2f);          // Unit movement speed
  setEconSkip(E0 + 9, -1.8f);         // Fewer remaining build turns is strongly preferred (preserves in-progress builds!)
  setEconSkip(E0 + 10, -1.4f);        // High upkeep / purchase cost penalty
  setEconSkip(E0 + 11, 1.0f);         // Fliers bonus
  setEconSkip(E0 + 12, 1.1f);         // Siege / Special bonus
  setEconSkip(E0 + 13, 0.7f);         // Cavalry bonus
  setEconSkip(E0 + 14, 1.2f);         // Frontline threat boosts production & retention
  setEconSkip(E0 + 15, -0.9f);        // Far from enemy reduces retention/local garrison value
  setEconSkip(E0 + 17, 2.0f);         // Vector from safe interior to threatened frontline
  setEconSkip(E0 + 18, 1.4f);         // Spoils immediate gold / city value
  setEconSkip(E0 + 19, -1.0f);        // Spoils diplomatic atrocity penalty

  // Head 4: Board Value Head priors
  valueNet.wSkip[1] = 2.2f;           // myCityShare
  valueNet.wSkip[2] = 1.4f;           // myArmyRatio
  valueNet.wSkip[3] = 0.5f;           // myGold
  valueNet.wSkip[4] = 0.8f;           // netIncome
  valueNet.wSkip[5] = 0.6f;           // myHeroes
  valueNet.wSkip[6] = -0.5f;          // activeWars
  valueNet.wSkip[7] = 0.4f;           // activeAllies
  valueNet.wSkip[9] = 0.8f;           // amILeader
  valueNet.wSkip[11] = -0.9f;         // capitalThreat
}

DiploOutput PolicyNetwork::scoreDiplomacy(const ContextVec& ctx, const RivalVec& rival) const {
  std::array<float, DIPLO_IN_DIM> x{};
  std::copy(ctx.begin(), ctx.end(), x.begin());
  std::copy(rival.begin(), rival.end(), x.begin() + CONTEXT_DIM);
  auto raw = diploNet.forward(x);
  DiploOutput out;
  out.stanceLogits = {raw[0], raw[1], raw[2], raw[3]};
  out.tributeSignal = std::tanh(raw[4]);
  out.jointWarSignal = std::tanh(raw[5]);
  return out;
}

MissionOutput PolicyNetwork::scoreMission(const ContextVec& ctx, const StackVec& stack,
                                          const TargetVec& target) const {
  std::array<float, MISSION_IN_DIM> x{};
  std::copy(ctx.begin(), ctx.end(), x.begin());
  std::copy(stack.begin(), stack.end(), x.begin() + CONTEXT_DIM);
  std::copy(target.begin(), target.end(), x.begin() + CONTEXT_DIM + STACK_DIM);

  // Inject personality-specific unit-class affinities into the non-linear score
  auto raw = missionNet.forward(x);
  return MissionOutput{raw[0], raw[1]};
}

float PolicyNetwork::scoreEconomy(const ContextVec& ctx, const EconVec& econ) const {
  std::array<float, ECON_IN_DIM> x{};
  std::copy(ctx.begin(), ctx.end(), x.begin());
  std::copy(econ.begin(), econ.end(), x.begin() + CONTEXT_DIM);
  return econNet.forward(x)[0];
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
  out << "  \"version\": 1,\n";
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

PolicyNetwork& activePolicy() {
  static PolicyNetwork instance = []() {
    PolicyNetwork net;
    // Automatically load trained weights if present in working directory or cpp/
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
  std::array<int, 8> sideCities{};
  for (City* city : b.visibleCities) {
    if (city->razed) continue;
    totalVisCities++;
    if (city->ownerIndex >= 0 && city->ownerIndex < 8) {
      sideCities[city->ownerIndex]++;
      maxCities = std::max(maxCities, sideCities[city->ownerIndex]);
    }
    if (city->ownerIndex == me) myCities++;
  }

  std::array<int, 8> sideArmies{};
  int maxArmies = 1, myHeroes = 0;
  for (Army* a : g.armies) {
    if (a->owner >= 0 && a->owner < 8) {
      sideArmies[a->owner]++;
      maxArmies = std::max(maxArmies, sideArmies[a->owner]);
    }
    if (a->owner == me && a->hero()) myHeroes++;
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

  c[0] = std::clamp((float)g.turn / 60.0f, 0.0f, 1.5f) - 0.5f;
  c[1] = (float)myCities / (float)std::max(1, totalVisCities);
  c[2] = (float)sideArmies[me] / (float)std::max(1, maxArmies);
  c[3] = std::tanh((float)side.gold / 400.0f);
  c[4] = std::tanh((float)(inc - upk) / 50.0f);
  c[5] = std::min(1.0f, (float)myHeroes / 4.0f);
  c[6] = (float)wars / 7.0f;
  c[7] = (float)allies / 7.0f;
  c[8] = (float)maxCities / (float)std::max(1, totalVisCities);
  c[9] = (myCities >= maxCities && myCities > 0) ? 1.0f : -1.0f;
  c[10] = b.frontierRatio;
  c[11] = capThreat;
  c[12] = (side.quest && side.quest->hasTarget()) ? 1.0f : -1.0f;
  c[13] = (float)(side.level - 1);

  const auto& prof = personality::forSide(side);
  for (int i = 0; i < personality::TRAIT_DIM; i++) {
    c[GLOBAL_DIM + i] = prof.traits[i];
  }
  return c;
}

}  // namespace w2::ai::ml
