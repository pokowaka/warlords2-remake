#include "warlords/ai/modern_ai.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"

namespace w2::ai::modern {

namespace {

enum class TargetKind : int {
  HOLD_GARRISON = 0,
  NEUTRAL_CITY = 1,
  ENEMY_CITY = 2,
  ENEMY_STACK = 3,
  RUIN_OR_SAGE = 4,
  TEMPLE_OR_ITEM = 5,
  FOG_FRONTIER = 6,
  REINFORCE_CITY = 7,
};

// May `me` choose an attack on `owner` (at `city`, or a field stack when null)?
// Same rule the actuator enforces: no declaration needed, or DiploNet is no
// longer proposing peace to that side (so the attack declares war).
bool attackable(const Game& g, int me, int owner, const City* city) {
  if (owner == NONE) return true;
  return !diplomacy::mustDeclare(g, me, owner, city) ||
         diplomacy::proposal(g, me, owner) != diplomacy::PEACE;
}

struct CandidateTarget {
  TargetKind kind = TargetKind::HOLD_GARRISON;
  int x = 0, y = 0;
  City* city = nullptr;
  Site* site = nullptr;
  Item* item = nullptr;
  int owner = NONE;
  float value = 0.0f;
  bool isQuestOrOwnCap = false;
  bool isEnemyCapital = false;
  bool isWater = false;
  float enemyThreat = 0.0f;
  float nearbyFriendlyStr = 0.0f;
  int assignedStacks = 0;
  float assignedStrength = 0.0f;
  std::string label;
};

ml::StackVec buildStackFeatures(const Game& g, const std::vector<Army*>& stack, int prevMinMove,
                                float originRemGuardStr = 10.0f, float originThreat = 0.0f) {
  ml::StackVec s{};
  if (stack.empty()) return s;
  int totalStr = 0, minMove = 99, totalUpkeep = 0, blessed = 0;
  bool hasHero = false, hasFlier = false, allFly = true, hasSiege = false, hasSpecial = false;
  for (Army* a : stack) {
    totalStr += a->strength;
    minMove = std::min(minMove, a->moves);
    totalUpkeep += a->upkeep;
    if (!a->blessings.empty()) blessed++;
    if (a->hero()) hasHero = true;
    const ArmyType* at = g.types.byId(a->type);
    if (at) {
      if (at->flies) hasFlier = true;
      else allFly = false;
      if (at->siege) hasSiege = true;
      if (at->bonus[48] != 0 || at->bonus[52] != 0) hasSpecial = true;
    } else {
      allFly = false;
    }
  }
  if (minMove == 99) minMove = 0;
  int heroCmd = combat::heroBonus(g, stack);
  float speedDrop = (prevMinMove > 0 && minMove < prevMinMove)
                        ? (float)(prevMinMove - minMove) / 20.0f
                        : 0.0f;

  s[0] = (float)stack.size() / 8.0f;
  s[1] = std::tanh((float)totalStr / 40.0f);
  s[2] = (float)totalStr / (float)(stack.size() * 9);
  s[3] = std::tanh((float)minMove / 16.0f);
  s[4] = hasHero ? 1.0f : 0.0f;
  s[5] = hasFlier ? 1.0f : 0.0f;
  s[6] = allFly ? 1.0f : 0.0f;
  s[7] = hasSiege ? 1.0f : 0.0f;
  s[8] = hasSpecial ? 1.0f : 0.0f;
  s[9] = (float)blessed / (float)stack.size();
  s[10] = std::tanh((float)totalUpkeep / 20.0f);
  s[11] = std::tanh((float)heroCmd / 3.0f);
  s[12] = (stack.size() == 1) ? 1.0f : 0.0f;
  s[13] = speedDrop;
  s[14] = std::tanh(originRemGuardStr / 10.0f);
  s[15] = std::tanh(originThreat / 20.0f);
  return s;
}

int targetPathCost(const Game& g, const core::Flood& flood, const CandidateTarget& tgt) {
  if (tgt.kind == TargetKind::HOLD_GARRISON) return 0;
  if (tgt.city) {
    int best = core::UNREACHED;
    for (int dy = -1; dy <= 2; dy++) {
      for (int dx = -1; dx <= 2; dx++) {
        int tx = tgt.city->x + dx, ty = tgt.city->y + dy;
        if (tx >= 0 && ty >= 0 && tx < g.map->width && ty < g.map->height) {
          int d = core::floodAt(flood, tx, ty);
          if (d < best) best = d;
        }
      }
    }
    return best;
  }
  return core::floodAt(flood, tgt.x, tgt.y);
}

ml::TargetVec buildTargetFeatures(Game& g, const Side& side, const std::vector<Army*>& stack,
                                  const CandidateTarget& tgt, float nearbyFriendlyStr,
                                  float cautionBias, int pathCost) {
  ml::TargetVec t{};
  t[(int)tgt.kind] = 1.0f;

  int sx = stack[0]->x, sy = stack[0]->y;
  int minMove = std::max(2, move::stackMoves(stack));
  int effCost = pathCost >= 0 ? pathCost : (move::distance(sx, sy, tgt.x, tgt.y) * 2);
  float turnsToReach = (float)effCost / (float)std::max(4, minMove);

  float winNow = 0.0f, winConverged = 0.0f, survRatio = 0.0f;
  float seenFraction = 1.0f;
  bool isCombat = (tgt.kind == TargetKind::NEUTRAL_CITY ||
                   tgt.kind == TargetKind::ENEMY_CITY ||
                   tgt.kind == TargetKind::ENEMY_STACK);
  if (isCombat) {
    auto est = belief::estimateCombat(g, side, stack, tgt.x, tgt.y, cautionBias);
    winNow = est.winProb;
    survRatio = est.expectedSurvivors / (float)std::max<size_t>(1, stack.size());
    seenFraction = est.seenTileFraction;

    float myStr = 0.0f;
    for (Army* a : stack) myStr += (float)a->strength;
    int freeSlots = std::max(0, rules::MAX_STACK - (int)stack.size());
    float addStr = std::min(nearbyFriendlyStr + tgt.assignedStrength, (float)freeSlots * 5.0f);
    if (addStr > 0.5f && est.estimatedDefenderStrength > 0.0f) {
      float combinedRatio = (myStr + addStr) / (myStr + addStr + est.estimatedDefenderStrength);
      winConverged = std::clamp(std::max(winNow, combinedRatio), 0.0f, 1.0f);
    } else {
      winConverged = winNow;
    }
  }

  bool flies = (move::modeOf(g, stack) == move::FLYING);
  bool atSea = false;
  for (Army* a : stack) {
    if (a->atSea) atSea = true;
  }
  int tileDist = std::max(1, move::distance(sx, sy, tgt.x, tgt.y));
  bool requiresBoatEmbark = !flies && !atSea &&
                            (tgt.isWater || (effCost >= tileDist * 2 + 24));

  t[8] = 1.0f / (1.0f + turnsToReach);
  t[9] = (turnsToReach <= 1.0f) ? 1.0f : 0.0f;
  t[10] = winNow;
  t[11] = winConverged;
  t[12] = survRatio;
  t[13] = std::tanh(tgt.value / 30.0f);
  t[14] = tgt.isQuestOrOwnCap ? 1.0f : 0.0f;
  t[15] = tgt.isEnemyCapital ? 1.0f : 0.0f;
  t[16] = std::tanh(tgt.enemyThreat / 25.0f);
  t[17] = std::tanh((tgt.assignedStrength + nearbyFriendlyStr) / 25.0f);
  t[18] = std::tanh((float)tgt.assignedStacks / 2.0f);
  t[19] = requiresBoatEmbark ? 1.0f : 0.0f;
  t[20] = seenFraction;
  return t;
}

void runDiplomacyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       std::array<bool, 8>& decided) {
  if (g.map->options.diplomacy == 0) return;
  int me = side.index;
  const SideBelief& b = belief::ensure(side);
  const auto& prof = personality::forSide(side);

  int totalVisCities = 0, maxVisCities = 0, leaderSide = NONE;
  for (int s = 0; s < 8; s++) {
    totalVisCities += b.visibleCitiesBySide[s];
    if (b.visibleCitiesBySide[s] > maxVisCities) {
      maxVisCities = b.visibleCitiesBySide[s];
      leaderSide = s;
    }
  }

  int activeWars = 0;
  int primaryEnemy = NONE;
  for (int s = 0; s < 8; s++) {
    if (s != me && g.map->sides[s].inUse && g.map->sides[s].alive &&
        (diplomacy::state(g, me, s) == diplomacy::WAR ||
         (decided[s] && diplomacy::proposal(g, me, s) != diplomacy::PEACE))) {
      activeWars++;
      if (primaryEnemy == NONE &&
          (diplomacy::state(g, me, s) == diplomacy::WAR ||
           diplomacy::proposal(g, me, s) == diplomacy::WAR)) {
        primaryEnemy = s;
      }
    }
  }

  float totalMyMobileStr = 0.0f;
  for (Army* a : g.armies) {
    if (a->owner == me && a->x != NONE && !a->transit) totalMyMobileStr += (float)a->strength;
  }

  auto myCitiesList = game::sideCities(g, side);
  std::array<float, 8> rivalMinDist;
  rivalMinDist.fill(120.0f);
  int closestRival = NONE;
  float closestRivalDist = 999.0f;
  for (int otherIdx = 0; otherIdx < 8; otherIdx++) {
    Side& other = g.map->sides[otherIdx];
    if (otherIdx == me || !other.inUse || !other.alive) continue;
    if (b.visibleCitiesBySide[otherIdx] == 0 && b.theirBorderStrength[otherIdx] == 0.0f) continue;
    float md = 120.0f;
    for (City* mc : myCitiesList) {
      for (City* vc : b.visibleCities) {
        if (vc->ownerIndex == otherIdx) {
          md = std::min(md, (float)move::distance(mc->x, mc->y, vc->x, vc->y));
        }
      }
    }
    rivalMinDist[otherIdx] = md;
    if (md < closestRivalDist) {
      closestRivalDist = md;
      closestRival = otherIdx;
    }
  }

  int warsCommitted = activeWars;

  for (int otherIdx = 0; otherIdx < 8; otherIdx++) {
    Side& other = g.map->sides[otherIdx];
    if (otherIdx == me || !other.inUse || !other.alive || decided[otherIdx]) continue;
    int curSt = diplomacy::state(g, me, otherIdx);
    if (g.map->options.hiddenMap != 0 && b.visibleCitiesBySide[otherIdx] == 0 &&
        b.theirBorderStrength[otherIdx] == 0.0f && curSt != diplomacy::WAR) {
      continue;
    }
    decided[otherIdx] = true;

    auto att = diplomacy::computeAttitude(g, me, otherIdx);
    bool curAllied = diplomacy::isAllied(g, me, otherIdx);
    float minDist = rivalMinDist[otherIdx];

    int sharedWars = 0;
    for (int s = 0; s < 8; s++) {
      if (s != me && s != otherIdx && g.map->sides[s].alive &&
          diplomacy::state(g, me, s) == diplomacy::WAR &&
          diplomacy::state(g, otherIdx, s) == diplomacy::WAR) {
        sharedWars++;
      }
    }

    int grudge = 0;
    if (side.ai) {
      grudge = side.ai->citiesLost[otherIdx] * 8 + side.ai->heroesKilled[otherIdx] * 6 +
               side.ai->lost[otherIdx] * 2;
    }

    int otherWars = warsCommitted - (curSt == diplomacy::WAR ? 1 : 0);
    float effMyStr = std::max(b.myBorderStrength[otherIdx],
                              totalMyMobileStr * std::clamp(1.1f - minDist / 45.0f, 0.0f, 0.8f));
    bool isLeaderOrClosest =
        (leaderSide == otherIdx && b.visibleCitiesBySide[otherIdx] * 3 >= totalVisCities) ||
        (otherIdx == closestRival);

    ml::RivalVec rv{};
    rv[0] = curAllied ? 1.0f : 0.0f;
    rv[1] = (!curAllied && curSt == diplomacy::PEACE) ? 1.0f : 0.0f;
    rv[2] = (curSt == diplomacy::INTERMEDIATE) ? 1.0f : 0.0f;
    rv[3] = (curSt == diplomacy::WAR) ? 1.0f : 0.0f;
    rv[4] = (float)diplomacy::truceTurns(g, me, otherIdx) / 15.0f;
    rv[5] = (float)att.trustScore / 100.0f;
    rv[6] = (float)att.totalScore / 100.0f;
    rv[7] = std::tanh(effMyStr / 35.0f);
    rv[8] = std::tanh(b.theirBorderStrength[otherIdx] / 35.0f);
    rv[9] = std::tanh((effMyStr - b.theirBorderStrength[otherIdx]) / 20.0f);
    rv[10] = (float)b.visibleCitiesBySide[otherIdx] / (float)std::max(1, totalVisCities);
    rv[11] = isLeaderOrClosest ? 1.0f : 0.0f;
    rv[12] = std::min(1.0f, (float)sharedWars / 2.0f);
    rv[13] = std::tanh((float)grudge / 30.0f);
    rv[14] = (side.capital && side.capital->ownerIndex == otherIdx) ? 1.0f : 0.0f;
    rv[15] = std::tanh((float)diplomacy::goldOffer(g, otherIdx, me) / 200.0f);
    rv[16] = std::tanh(minDist / 45.0f);
    rv[17] = std::tanh((float)otherWars / 3.0f);

    ml::DiploInVec dIn = ml::packDiploInput(ctx, rv);
    auto raw = policy.diploNet.forward(dIn);
    std::vector<float> stanceVec = {raw[0], raw[1], raw[2], raw[3]};
    ml::DiploStepRecord rec;
    rec.input = dIn;
    int bestChoice = ml::selectFromLogits(stanceVec, sampler, &rec.oldStanceLogProb);
    rec.chosenStance = bestChoice;
    float bestLogit = stanceVec[bestChoice];

    if (bestChoice == 0) {
      diplomacy::propose(g, me, otherIdx, diplomacy::PEACE);
      diplomacy::proposeAlliance(g, me, otherIdx, true);
    } else if (bestChoice == 1) {
      diplomacy::propose(g, me, otherIdx, diplomacy::PEACE);
      diplomacy::proposeAlliance(g, me, otherIdx, false);
    } else if (bestChoice == 2) {
      diplomacy::propose(g, me, otherIdx, diplomacy::INTERMEDIATE);
      if (curSt != diplomacy::WAR) warsCommitted++;
    } else {
      diplomacy::propose(g, me, otherIdx, diplomacy::WAR);
      if (curSt != diplomacy::WAR) warsCommitted++;
      if (primaryEnemy == NONE) primaryEnemy = otherIdx;
    }

    // Binary sub-decision (output 4): sweeten a peace offer to a side we are at war with
    bool friendly = (bestChoice == 0 || bestChoice == 1);
    diplomacy::setGoldOffer(g, me, otherIdx, 0);
    if (friendly && curSt == diplomacy::WAR && side.gold >= 100) {
      rec.hasTribute = true;
      rec.tributeChosen = ml::selectBinary(raw[4], sampler, &rec.oldTributeLogProb);
      if (rec.tributeChosen) {
        int offer = std::clamp(side.gold / 4 / 25 * 25, 50, std::min(300, side.gold / 2));
        diplomacy::setGoldOffer(g, me, otherIdx, offer);
      }
    }

    // Binary sub-decision (output 5): ask a friendly side to join our war
    diplomacy::setWarTarget(g, me, otherIdx, NONE);
    if (friendly && primaryEnemy != NONE && primaryEnemy != otherIdx) {
      rec.hasJointWar = true;
      rec.jointWarChosen = ml::selectBinary(raw[5], sampler, &rec.oldJointWarLogProb);
      if (rec.jointWarChosen) diplomacy::setWarTarget(g, me, otherIdx, primaryEnemy);
    }

    if (sampler && sampler->currentTurn) {
      sampler->currentTurn->diploSteps.push_back(rec);
    }

    const char* stanceNames[4] = {"ALLIED", "PEACE", "UNEASY", "WAR"};
    side.aiThoughts.push_back(AIThought{
        "Diplomacy", side.name, other.name, side.capX, side.capY, other.capX, other.capY,
        bestLogit, 0.0f, false,
        fmt("Stance -> %s (Attitude %+d, Trust %+d): %s",
            stanceNames[bestChoice], att.totalScore, att.trustScore, att.stanceSummary.c_str())});

    if (!other.computer && other.missives.empty() && g.turn > 1 &&
        g.turn - other.lastMissiveTurn[me] >= 4) {
      bool sendMissive = false;
      DiplomaticMissive m;
      m.fromSide = me;
      m.toSide = otherIdx;
      m.reasonSummary = att.whatItTakes;

      if (bestChoice == 0 && !curAllied) {
        sendMissive = true;
        m.proposedState = diplomacy::PEACE;
        m.proposeAlliance = true;
        m.jointWarTarget = diplomacy::warTarget(g, me, otherIdx);
        m.headline = fmt("Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
        m.body = fmt("\"%s\" We propose a full Military Alliance with shared vision!", prof.motto.c_str());
      } else if (bestChoice == 2 && curSt == diplomacy::PEACE && b.theirBorderStrength[otherIdx] > 6.0f) {
        sendMissive = true;
        m.proposedState = diplomacy::INTERMEDIATE;
        m.headline = fmt("Border Warning from %s (%s)", side.name.c_str(), prof.name.c_str());
        m.body = "Your armies mass along our frontier. Withdraw them or our peace shall end!";
      } else if (bestChoice == 2 && curSt == diplomacy::PEACE && rv[9] > 0.35f) {
        // Human-only flavour (never reached in all-AI training): demand tribute
        // when our border forces clearly outmatch theirs.
        sendMissive = true;
        m.proposedState = diplomacy::PEACE;
        m.goldDemand = std::max(75, att.peaceGoldNeeded > 0 ? att.peaceGoldNeeded : 100);
        m.headline = fmt("Tribute Demand from %s (%s)", side.name.c_str(), prof.name.c_str());
        m.body = fmt("\"%s\" Pay %d gold in tribute to preserve the peace between us!",
                     prof.motto.c_str(), m.goldDemand);
      } else if (bestChoice == 1 && curSt == diplomacy::WAR) {
        sendMissive = true;
        m.proposedState = diplomacy::PEACE;
        int offered = diplomacy::goldOffer(g, me, otherIdx);
        m.goldDemand = -offered;
        m.headline = fmt("Peace Envoy from %s (%s)", side.name.c_str(), prof.name.c_str());
        m.body = offered > 0
                     ? fmt("We offer %d gold and a 10-turn truce to end this war.", offered)
                     : "Let us lay down arms and sign a 10-turn truce.";
      }

      if (sendMissive) {
        other.missives.push_back(std::move(m));
        other.lastMissiveTurn[me] = g.turn;
      }
    }
  }
}

void decideCaptureSpoils(Game& g, Side& side, City& city, const std::vector<Army*>& attackers,
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx,
                         ml::RolloutSampler* sampler) {
  if (city.razed || city.ownerIndex != side.index) return;
  if (ai::questCapture(g, side, city, attackers)) return;

  const SideBelief& b = belief::ensure(side);
  float localThreat = city.index < (int)b.cityThreat1Turn.size() ? b.cityThreat1Turn[city.index] : 0.0f;
  float distEnemy = city.index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[city.index] : 60.0f;
  int sackGold = ai::groups::sackValue(g, city);
  int pillageGold = city.slots.size() >= 2
                        ? std::abs(g.types.byId(city.slots.back().type)->price) / 2
                        : 0;

  auto makeSpoilIn = [&](float ongoingIncome, float instantGold, float diploPen) {
    ml::EconVec e{};
    e[6] = 1.0f;
    e[7] = ongoingIncome / 35.0f;
    e[14] = std::tanh(localThreat / 25.0f);
    e[15] = std::tanh(distEnemy / 50.0f);
    e[18] = std::tanh(instantGold / 250.0f);
    e[19] = diploPen / 40.0f;
    return ml::packEconInput(ctx, e);
  };

  bool isAnyCapital = false;
  for (int s = 0; s < 8; s++) {
    if (g.map->sides[s].capital == &city) isAnyCapital = true;
  }

  std::vector<int> modes = {0};
  std::vector<ml::EconInVec> candInputs = {makeSpoilIn((float)city.income, 0.0f, 0.0f)};
  if (pillageGold > 0) {
    modes.push_back(1);
    candInputs.push_back(makeSpoilIn((float)city.income * 0.8f, (float)pillageGold, 4.0f));
  }
  if (sackGold > 0) {
    modes.push_back(2);
    candInputs.push_back(makeSpoilIn((float)city.income * 0.5f, (float)sackGold, 15.0f));
  }
  if (!isAnyCapital) {
    modes.push_back(3);
    candInputs.push_back(makeSpoilIn(0.0f, 0.0f, 38.0f));
  }

  std::vector<float> scores(candInputs.size());
  for (size_t i = 0; i < candInputs.size(); i++) {
    scores[i] = policy.econNet.forward(candInputs[i])[0];
  }
  float logProb = 0.0f;
  int pick = ml::selectFromLogits(scores, sampler, &logProb);
  if (sampler && sampler->currentTurn && candInputs.size() > 1) {
    sampler->currentTurn->econSteps.push_back(
        ml::EconStepRecord{std::move(candInputs), {}, pick, logProb});
  }

  int bestMode = modes[pick];
  if (bestMode == 1) {
    game::pillage(g, side, city, attackers);
    if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "pillaged");
  } else if (bestMode == 2) {
    game::sack(g, side, city, attackers);
    if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "sacked");
  } else if (bestMode == 3) {
    game::raze(g, side, city, attackers);
    if (ai::hooks.onSpoils) ai::hooks.onSpoils(g, side, city, "razed");
  } else {
    game::occupy(g, side, city, attackers);
  }
}

bool executeStackMission(Game& g, Side& side, std::vector<Army*>& stack, CandidateTarget& tgt,
                         bool stageNear, const ml::PolicyNetwork& policy,
                         const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
  stack.erase(std::remove_if(stack.begin(), stack.end(),
                             [&](Army* a) { return !g.alive(a) || a->moves <= 0; }),
              stack.end());
  if (stack.empty()) return false;

  if (tgt.kind == TargetKind::HOLD_GARRISON) {
    for (Army* a : stack) a->aiMoved = true;
    return true;
  }

  int me = side.index;
  std::map<Army*, int> startMoves;
  for (Army* a : stack) startMoves[a] = a->moves;

  // A coalition drawn from one owned 2x2 city may stand on different corners.
  // Members walk (by the normal rules, paying moves) onto the leader's tile;
  // any that cannot join without breaking the 8-army stack limit stay behind
  // and remain free for later waves.
  {
    int lx = stack[0]->x, ly = stack[0]->y;
    int onLeaderTile = (int)game::armiesAt(g, lx, ly).size();
    std::vector<Army*> joined;
    for (Army* a : stack) {
      if (a->x == lx && a->y == ly) {
        joined.push_back(a);
        continue;
      }
      if (onLeaderTile + 1 > rules::MAX_STACK) continue;
      auto path = move::findPath(g, {a}, a->x, a->y, lx, ly);
      if (!path || path->empty()) continue;
      move::WalkResult jr = move::walk(g, {a}, *path);
      if (g.alive(a) && a->x == lx && a->y == ly && jr.steps > 0) {
        ai::walked(g, {a}, jr);
        joined.push_back(a);
        onLeaderTile++;
      }
    }
    stack = std::move(joined);
    stack.erase(std::remove_if(stack.begin(), stack.end(), [&](Army* a) { return a->moves <= 0; }),
                stack.end());
    if (stack.empty()) return false;
  }

  int destX = tgt.x, destY = tgt.y;
  if (tgt.city && !stageNear) {
    if (tgt.city->ownerIndex == me) {
      for (int oy = 0; oy <= 1; oy++) {
        for (int ox = 0; ox <= 1; ox++) {
          int tx = tgt.city->x + ox, ty = tgt.city->y + oy;
          if ((int)(game::armiesAt(g, tx, ty).size() + stack.size()) <= rules::MAX_STACK) {
            destX = tx;
            destY = ty;
            break;
          }
        }
      }
    } else {
      destX = tgt.city->x + (tgt.city->x < stack[0]->x ? 1 : 0);
      destY = tgt.city->y + (tgt.city->y < stack[0]->y ? 1 : 0);
    }
  }

  if (stageNear && (tgt.kind == TargetKind::ENEMY_CITY || tgt.kind == TargetKind::NEUTRAL_CITY ||
                    tgt.kind == TargetKind::ENEMY_STACK)) {
    int bestSx = destX, bestSy = destY;
    int bestDist = 999;
    int sx0 = stack[0]->x, sy0 = stack[0]->y;
    for (int dy = -2; dy <= 2; dy++) {
      for (int dx = -2; dx <= 2; dx++) {
        int rad = std::max(std::abs(dx), std::abs(dy));
        if (rad < 1 || rad > 2) continue;
        int nx = tgt.x + dx, ny = tgt.y + dy;
        if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height) continue;
        if (tgt.city && game::cityAt(g, nx, ny) == tgt.city) continue;
        if (!belief::isTileVisible(g, me, nx, ny)) continue;
        int terr = g.map->terrainType[g.map->tiles[ny * g.map->width + nx] & 0xff];
        if (terr == move::WATER || terr == move::SHORE || terr == move::MOUNTAINS) continue;
        auto occ = game::armiesAt(g, nx, ny);
        if (!occ.empty() &&
            (occ[0]->owner != me || (int)(occ.size() + stack.size()) > rules::MAX_STACK)) {
          continue;
        }
        int d = move::distance(sx0, sy0, nx, ny) - (!occ.empty() ? 15 : 0);
        if (d < bestDist) {
          bestDist = d;
          bestSx = nx;
          bestSy = ny;
        }
      }
    }
    destX = bestSx;
    destY = bestSy;
  }

  core::Sel sel = core::select(g, stack);
  if (sel.armies.empty() || !sel.leader) return false;

  bool isFogMission = (tgt.kind == TargetKind::FOG_FRONTIER);
  bool madeProgress = false;
  bool completedObjective = false;

  for (int pass = 0; pass < 24 && !sel.armies.empty() && sel.minMoves > 0; pass++) {
    int lx = sel.leader->x, ly = sel.leader->y;
    if (lx == destX && ly == destY) {
      if (isFogMission) completedObjective = true;
      break;
    }

    auto path = move::findPath(g, sel.armies, lx, ly, destX, destY);
    if (!path || path->empty()) break;

    std::vector<move::Step> walkSteps = *path;
    if (isFogMission && walkSteps.size() > 6) {
      walkSteps.resize(6);
    }

    int seenBoxBefore = 0;
    if (isFogMission && g.map->options.hiddenMap != 0) {
      for (int oy = -7; oy <= 7; oy++) {
        for (int ox = -7; ox <= 7; ox++) {
          int ux = lx + ox, uy = ly + oy;
          if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
              belief::isTileVisible(g, me, ux, uy)) {
            seenBoxBefore++;
          }
        }
      }
    }

    move::WalkResult r = move::walk(g, sel.armies, walkSteps);
    if (r.steps > 0) {
      madeProgress = true;
      ai::walked(g, sel.armies, r);
    }
    core::refresh(g, sel);
    core::pickUp(g, sel);

    int ax = NONE, ay = NONE;
    City* attCity = nullptr;
    if (r.stopped == "attack" && r.attack && !stageNear) {
      ax = r.attack->x;
      ay = r.attack->y;
      attCity = r.attack->city;
    } else if (r.stopped == "at peace" && !stageNear && r.steps < (int)walkSteps.size()) {
      const move::Step& ahead = walkSteps[r.steps];
      ax = ahead.x;
      ay = ahead.y;
      attCity = game::cityAt(g, ax, ay);
    }

    if (ax != NONE && ay != NONE) {
      auto defArmies = game::armiesAt(g, ax, ay);
      int defOwner = defArmies.empty() ? (attCity ? attCity->ownerIndex : NONE) : defArmies[0]->owner;

      // Breaking a treaty is DiploNet's call, not the mission's: an attack that
      // needs a declaration (the game's own mustDeclare rule) only goes ahead
      // when this side is no longer proposing peace to the defender.
      if (diplomacy::mustDeclare(g, me, defOwner, attCity)) {
        if (diplomacy::proposal(g, me, defOwner) == diplomacy::PEACE) break;
        diplomacy::attackAnyway(g, me, defOwner);
      }

      // Gather friendly units on the launching tile so converged allies fight as one combined force
      std::vector<Army*> combinedHere = core::collect(g, me, sel.leader->x, sel.leader->y, 1);
      if (combinedHere.empty()) combinedHere = sel.armies;

      core::Sel combSel = core::select(g, combinedHere);
      int prevOwner = attCity ? attCity->ownerIndex : NONE;
      Battle bRes = core::fight(g, combSel, ax, ay);
      madeProgress = true;
      belief::recordBattleObservation(g, me, ax, ay);
      if (attCity && attCity->ownerIndex == me && prevOwner != me && !combSel.armies.empty()) {
        decideCaptureSpoils(g, side, *attCity, combSel.armies, policy, ctx, sampler);
      }
      sel = combSel;

      if (!bRes.won || sel.armies.empty()) {
        break;
      }
      if (tgt.city && tgt.city->ownerIndex == me) {
        completedObjective = true;
        break;
      }
      if (tgt.kind == TargetKind::ENEMY_STACK && ax == tgt.x && ay == tgt.y) {
        completedObjective = true;
        break;
      }
      // Won an intermediate road/blocker battle and still have moves: keep advancing toward dest!
      continue;
    }

    if (isFogMission && r.steps > 0 && g.map->options.hiddenMap != 0) {
      int seenBoxAfter = 0;
      for (int oy = -7; oy <= 7; oy++) {
        for (int ox = -7; ox <= 7; ox++) {
          int ux = lx + ox, uy = ly + oy;
          if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
              belief::isTileVisible(g, me, ux, uy)) {
            seenBoxAfter++;
          }
        }
      }
      if (seenBoxAfter > seenBoxBefore ||
          (sel.leader->x == destX && sel.leader->y == destY)) {
        // Yield back to the neural wave loop as soon as fog movement reveals new map tiles,
        // allowing MissionNet itself to evaluate newly uncovered castles or sites!
        completedObjective = true;
        break;
      }
    }

    if (r.steps == 0 || r.stopped == "out of moves" || r.stopped == "blocked" ||
        r.stopped == "at peace") {
      if (stageNear && r.steps > 0) {
        // Arrived at or advanced toward staging position
        madeProgress = true;
      }
      break;
    }
  }

  // Interact with open temple or ruin/sage if standing on one with moves left
  if (!sel.armies.empty() && sel.leader && sel.minMoves > 0) {
    int fx = sel.leader->x, fy = sel.leader->y;
    Site* s = core::siteAt(g, fx, fy);
    if (s && core::siteOpen(*s)) {
      if (s->content == site::TEMPLE) {
        site::search(g, sel.armies, fx, fy, false);
        madeProgress = true;
        completedObjective = true;
        if (g.map->options.quests != 0 && sel.hero && (!side.quest || !side.quest->hasTarget())) {
          quest::assign(g, side, sel.hero, true, core::flies(g, sel.hero));
        }
      } else if (sel.hero && g.alive(sel.hero)) {
        site::search(g, sel.armies, fx, fy, false);
        madeProgress = true;
        completedObjective = true;
      }
    }
    core::pickUp(g, sel);
  }

  for (Army* a : sel.armies) {
    if (!g.alive(a)) continue;
    auto it = startMoves.find(a);
    bool spentMoves = (it != startMoves.end() && a->moves < it->second);
    if (completedObjective && spentMoves && a->moves >= 2 && !stageNear &&
        tgt.kind != TargetKind::REINFORCE_CITY) {
      // Allow remaining movement points to be dispatched in the next wave of this turn
      a->aiMoved = false;
    } else {
      a->aiMoved = madeProgress;
    }
  }
  return madeProgress;
}

std::vector<size_t> buildCoalitionSizes(const Game& g, Army* seed,
                                        const std::vector<Army*>& escorts) {
  std::vector<size_t> sizes = {1};
  auto addUnique = [&](size_t m) {
    if (m >= 1 && m <= escorts.size() + 1 &&
        std::find(sizes.begin(), sizes.end(), m) == sizes.end()) {
      sizes.push_back(m);
    }
  };
  if (!escorts.empty()) {
    addUnique(2);
    addUnique(4);
    addUnique(escorts.size());
    addUnique(escorts.size() + 1);

    // Also include fast/flying-matching prefix size if distinct
    bool seedFlies = core::flies(g, seed);
    size_t fastCount = 1;
    for (Army* e : escorts) {
      if (seedFlies ? core::flies(g, e) : (e->moves >= 12)) {
        fastCount++;
      } else {
        break;
      }
    }
    addUnique(fastCount);
  }
  return sizes;
}

void runMilitaryAndExplorationPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx, const PauseCallback& pause,
                                    ml::RolloutSampler* sampler, std::vector<MissionSnapshot>* snapshots = nullptr) {
  int me = side.index;
  SideBelief& b = belief::ensure(side);
  float caution = 0.0f;

  std::vector<CandidateTarget> targets;
  targets.reserve(80);

  for (City* c : b.visibleCities) {
    if (c->razed) continue;
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    bool isQuestOrOwn = (side.capital == c) || (side.quest && side.quest->city == c);
    bool isEnemyCap = false;
    for (int s = 0; s < 8; s++) {
      if (s != me && g.map->sides[s].capital == c) isEnemyCap = true;
    }

    if (c->ownerIndex == me) {
      float existingGarrisonStr = 0.0f;
      float stationaryGarrisonStr = 0.0f;
      int existingCount = 0;
      for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
          for (Army* a : game::armiesAt(g, c->x + dx, c->y + dy)) {
            if (a->owner == me) {
              existingGarrisonStr += (float)a->strength;
              existingCount++;
              if (a->moves <= 0 || a->aiMoved) {
                stationaryGarrisonStr += (float)a->strength;
              }
            }
          }
        }
      }
      CandidateTarget hold;
      hold.kind = TargetKind::HOLD_GARRISON;
      hold.x = c->x;
      hold.y = c->y;
      hold.city = c;
      hold.owner = me;
      hold.value = (float)c->income + (float)c->defence * 3.0f;
      hold.isQuestOrOwnCap = isQuestOrOwn;
      hold.enemyThreat = threat;
      hold.assignedStrength = stationaryGarrisonStr;
      hold.label = "Hold " + c->name;
      targets.push_back(std::move(hold));

      if (existingCount < 32) {
        CandidateTarget reinf;
        reinf.kind = TargetKind::REINFORCE_CITY;
        reinf.x = c->x;
        reinf.y = c->y;
        reinf.city = c;
        reinf.owner = me;
        reinf.value = (float)c->income + (float)c->defence * 3.0f;
        reinf.isQuestOrOwnCap = isQuestOrOwn;
        reinf.enemyThreat = threat;
        reinf.assignedStrength = existingGarrisonStr;
        reinf.label = "Reinforce " + c->name;
        targets.push_back(std::move(reinf));
      }
    } else if (c->ownerIndex == NONE) {
      CandidateTarget neut;
      neut.kind = TargetKind::NEUTRAL_CITY;
      neut.x = c->x;
      neut.y = c->y;
      neut.city = c;
      neut.owner = NONE;
      neut.value = (float)c->income + (float)c->slots.size() * 5.0f;
      neut.isQuestOrOwnCap = isQuestOrOwn;
      neut.isEnemyCapital = isEnemyCap;
      neut.enemyThreat = threat;
      neut.label = "Capture Neutral " + c->name;
      targets.push_back(std::move(neut));
    } else if (attackable(g, me, c->ownerIndex, c)) {
      CandidateTarget enem;
      enem.kind = TargetKind::ENEMY_CITY;
      enem.x = c->x;
      enem.y = c->y;
      enem.city = c;
      enem.owner = c->ownerIndex;
      enem.value = (float)c->income + (float)c->defence * 6.0f;
      if (diplomacy::alliedTargetCity(g, c->ownerIndex) == c->index) enem.value += 10.0f;
      enem.isQuestOrOwnCap = isQuestOrOwn;
      enem.isEnemyCapital = isEnemyCap;
      enem.enemyThreat = threat;
      enem.label = "Siege " + c->name;
      targets.push_back(std::move(enem));
    }
  }

  for (const auto& [tileKey, obs] : b.stacksByTile) {
    if (obs.owner == me || obs.owner == NONE) continue;
    if (game::cityAt(g, obs.x, obs.y)) continue;
    if (!attackable(g, me, obs.owner, nullptr)) continue;
    CandidateTarget st;
    st.kind = TargetKind::ENEMY_STACK;
    st.x = obs.x;
    st.y = obs.y;
    st.owner = obs.owner;
    st.value = (float)obs.visibleCount * 4.0f;
    st.isQuestOrOwnCap = (side.quest && side.quest->side && side.quest->side->index == obs.owner);
    st.label = fmt("Intercept stack (%d,%d)", obs.x, obs.y);
    targets.push_back(std::move(st));
  }

  for (Site* s : b.visibleSites) {
    if (!core::siteOpen(*s)) continue;
    CandidateTarget st;
    st.x = s->x;
    st.y = s->y;
    st.site = s;
    if (s->content == site::TEMPLE) {
      st.kind = TargetKind::TEMPLE_OR_ITEM;
      st.value = 18.0f;
      st.label = "Bless at " + s->name;
    } else {
      st.kind = TargetKind::RUIN_OR_SAGE;
      st.value = s->rich ? 28.0f : 22.0f;
      st.label = "Search " + s->name;
    }
    targets.push_back(std::move(st));
  }

  for (Item* it : b.visibleGroundItems) {
    CandidateTarget gt;
    gt.kind = TargetKind::TEMPLE_OR_ITEM;
    gt.x = it->x;
    gt.y = it->y;
    gt.item = it;
    gt.value = 20.0f;
    gt.isQuestOrOwnCap = (side.quest && side.quest->item == it);
    gt.label = "Pick up " + it->name;
    targets.push_back(std::move(gt));
  }

  for (const auto& fc : b.fogFrontiers) {
    CandidateTarget ft;
    ft.kind = TargetKind::FOG_FRONTIER;
    ft.x = fc.x;
    ft.y = fc.y;
    ft.isWater = fc.isWater;
    ft.value = (float)fc.regionUnseenCount * 2.0f + (fc.onRoad ? 6.0f : 0.0f);
    ft.label = fmt("Explore %s (%d,%d)", fc.isWater ? "Sea" : "Fog", fc.x, fc.y);
    targets.push_back(std::move(ft));
  }

  if (targets.empty()) return;

  for (auto& t : targets) {
    if (t.kind == TargetKind::NEUTRAL_CITY || t.kind == TargetKind::ENEMY_CITY ||
        t.kind == TargetKind::ENEMY_STACK) {
      float nearStr = 0.0f;
      for (Army* a : g.armies) {
        if (a->owner == me && a->x != NONE && !a->transit && a->moves > 0 &&
            move::distance(a->x, a->y, t.x, t.y) <= 14) {
          nearStr += (float)a->strength;
        }
      }
      t.nearbyFriendlyStr = nearStr;
    }
  }

  struct LocGroup {
    int x = 0, y = 0;
    City* city = nullptr;
    std::vector<Army*> units;
  };
  std::map<int, LocGroup> groupsByLoc;
  for (Army* a : g.armies) {
    if (a->owner != me || a->x == NONE || a->transit || a->moves <= 0 || a->aiMoved) continue;
    City* c = game::cityAt(g, a->x, a->y);
    int key = c ? -(c->index + 1) : (a->y * g.map->width + a->x);
    auto& lg = groupsByLoc[key];
    lg.x = a->x;
    lg.y = a->y;
    lg.city = c;
    lg.units.push_back(a);
  }

  int bestEnemyCityTarget = NONE;
  float bestEnemyCityScore = -1e9f;

  for (auto& [locKey, lg] : groupsByLoc) {
    std::vector<Army*> pool = lg.units;
    std::sort(pool.begin(), pool.end(), [&](Army* a, Army* b2) {
      if (a->hero() != b2->hero()) return a->hero() > b2->hero();
      bool af = core::flies(g, a), bf = core::flies(g, b2);
      if (af != bf) return af > bf;
      int sa = a->strength * 2 + a->maxMoves;
      int sb = b2->strength * 2 + b2->maxMoves;
      if (sa != sb) return sa > sb;
      return a->moves > b2->moves;
    });

    core::Flood landFlood = core::floodFrom(g, me, lg.x, lg.y, 72, move::LAND, me);
    // The flying flood only matters to a coalition that can fly; skipping it
    // for flightless groups also keeps land stacks off fog only fliers can reach
    bool anyFlier = false;
    for (Army* a : pool) anyFlier = anyFlier || core::flies(g, a);
    core::Flood flyFlood = anyFlier ? core::floodFrom(g, me, lg.x, lg.y, 72, move::FLYING, me) : core::Flood{};
    auto flyDist = [&](int x, int y) { return anyFlier ? core::floodAt(flyFlood, x, y) : core::UNREACHED; };

    // Ensure local reachable fog frontiers (land or coastal sea) are included near this stack
    if (g.map->options.hiddenMap != 0) {
      int reachableFogCount = 0;
      for (const auto& t : targets) {
        if (t.kind == TargetKind::FOG_FRONTIER &&
            core::floodAt(landFlood, t.x, t.y) < core::UNREACHED) {
          reachableFogCount++;
        }
      }
      if (reachableFogCount < 4) {
        int W = g.map->width, H = g.map->height;
        for (int r = 1; r <= 28 && reachableFogCount < 6; r++) {
          for (int dy = -r; dy <= r && reachableFogCount < 6; dy++) {
            for (int dx = -r; dx <= r && reachableFogCount < 6; dx++) {
              if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
              int fx = lg.x + dx, fy = lg.y + dy;
              if (fx < 0 || fy < 0 || fx >= W || fy >= H) continue;
              int fterr = g.map->terrainType[g.map->tiles[fy * W + fx] & 0xff];
              if (fterr == move::MOUNTAINS) continue;
              if (core::floodAt(landFlood, fx, fy) >= core::UNREACHED) continue;
              int adjU = 0;
              for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                  int ux = fx + ox, uy = fy + oy;
                  if (ux >= 0 && uy >= 0 && ux < W && uy < H &&
                      !belief::isTileVisible(g, me, ux, uy)) {
                    adjU++;
                  }
                }
              }
              if (adjU >= 1) {
                bool isW = (fterr == move::WATER || fterr == move::SHORE);
                bool dup = false;
                for (const auto& t : targets) {
                  if (t.kind == TargetKind::FOG_FRONTIER && t.isWater == isW &&
                      move::distance(fx, fy, t.x, t.y) < 3) {
                    dup = true;
                    break;
                  }
                }
                if (!dup) {
                  CandidateTarget ft;
                  ft.kind = TargetKind::FOG_FRONTIER;
                  ft.x = fx;
                  ft.y = fy;
                  ft.isWater = isW;
                  bool road = !isW && (scn::roadAt(*g.map, fx, fy) % 0x20 != 0);
                  ft.value = (float)adjU * 2.0f + (road ? 6.0f : 0.0f);
                  ft.label = fmt("Explore %s (%d,%d)", isW ? "Sea" : "Fog", fx, fy);
                  targets.push_back(std::move(ft));
                  reachableFogCount++;
                }
              }
            }
          }
        }
      }
    }

    // Select up to 6 closest reachable land fog frontiers + 4 closest sea frontiers for this stack
    std::set<size_t> localFogIndices;
    if (g.map->options.hiddenMap != 0) {
      std::vector<std::pair<int, size_t>> landFogs, seaFogs;
      for (size_t ti = 0; ti < targets.size(); ti++) {
        if (targets[ti].kind != TargetKind::FOG_FRONTIER) continue;
        int dLand = core::floodAt(landFlood, targets[ti].x, targets[ti].y);
        int dFly = flyDist(targets[ti].x, targets[ti].y);
        int bestD = std::min(dLand, dFly);
        if (bestD >= core::UNREACHED) continue;
        if (targets[ti].isWater) seaFogs.emplace_back(bestD, ti);
        else landFogs.emplace_back(bestD, ti);
      }
      std::sort(landFogs.begin(), landFogs.end());
      std::sort(seaFogs.begin(), seaFogs.end());
      for (size_t i = 0; i < std::min<size_t>(6, landFogs.size()); i++) {
        localFogIndices.insert(landFogs[i].second);
      }
      for (size_t i = 0; i < std::min<size_t>(4, seaFogs.size()); i++) {
        localFogIndices.insert(seaFogs[i].second);
      }
    }

    std::set<size_t> blockedTargets;

    while (!pool.empty()) {
      Army* seed = pool.front();
      pool.erase(pool.begin());
      if (!g.alive(seed) || seed->moves <= 0 || seed->aiMoved) continue;

      std::vector<Army*> eligibleEscorts;
      for (auto it = pool.begin(); it != pool.end();) {
        Army* cand = *it;
        if (!g.alive(cand) || cand->moves <= 0 || cand->aiMoved) {
          it = pool.erase(it);
          continue;
        }
        if (eligibleEscorts.size() + 1 < rules::MAX_STACK) {
          eligibleEscorts.push_back(cand);
        }
        ++it;
      }

      float remGroupStr = (float)seed->strength;
      for (Army* rem : pool) remGroupStr += (float)rem->strength;

      float committedCityGuardStr = 0.0f;
      float originThreat = 0.0f;
      if (lg.city && lg.city->ownerIndex == me) {
        if (lg.city->index < (int)b.cityThreat2Turn.size()) {
          originThreat = b.cityThreat2Turn[lg.city->index];
        }
        for (int dy = 0; dy <= 1; dy++) {
          for (int dx = 0; dx <= 1; dx++) {
            for (Army* a : game::armiesAt(g, lg.city->x + dx, lg.city->y + dy)) {
              if (a->owner == me && (a->moves <= 0 || a->aiMoved)) {
                committedCityGuardStr += (float)a->strength;
              }
            }
          }
        }
      }

      std::vector<size_t> sizes = buildCoalitionSizes(g, seed, eligibleEscorts);

      struct PrebuiltStack {
        size_t size = 1;
        std::vector<Army*> units;
        ml::StackVec sv{};
        bool flies = false;
        float str = 0.0f;
      };
      std::vector<PrebuiltStack> preStacks;
      preStacks.reserve(sizes.size());
      for (size_t sz : sizes) {
        PrebuiltStack ps;
        ps.size = sz;
        ps.units.push_back(seed);
        for (size_t i = 0; i + 1 < sz && i < eligibleEscorts.size(); i++) {
          ps.units.push_back(eligibleEscorts[i]);
        }
        for (Army* a : ps.units) ps.str += (float)a->strength;
        float remGuardAfterLeave =
            (lg.city && lg.city->ownerIndex == me)
                ? (committedCityGuardStr + std::max(0.0f, remGroupStr - ps.str))
                : 20.0f;
        ps.sv = buildStackFeatures(g, ps.units, seed->moves, remGuardAfterLeave, originThreat);
        ps.flies = (move::modeOf(g, ps.units) == move::FLYING);
        preStacks.push_back(std::move(ps));
      }

      struct JointOption {
        size_t targetIdx = 0;
        size_t stackOptIdx = 0;
        ml::MissionInVec inVec{};
        ml::TargetVec tv{};
        ml::MissionOutput out{};
      };
      std::vector<JointOption> options;
      options.reserve(targets.size() * preStacks.size());

      // Scoring options only reads the board: answer per-tile army queries
      // (combat estimates) from one index instead of scanning every army.
      // The scope closes before the chosen stack moves.
      {
      belief::ScopedTileIndex scoringIndex(g);
      for (size_t ti = 0; ti < targets.size(); ti++) {
        if (blockedTargets.count(ti)) continue;
        const CandidateTarget& tgt = targets[ti];

        if ((tgt.kind == TargetKind::NEUTRAL_CITY || tgt.kind == TargetKind::ENEMY_CITY) &&
            tgt.city && (tgt.city->ownerIndex == me || tgt.city->razed)) {
          continue;
        }
        if (tgt.kind == TargetKind::ENEMY_STACK && belief::isTileVisible(g, me, tgt.x, tgt.y)) {
          // Only re-check stacks still in view; one that walked into fog stays a belief
          auto curAt = belief::armiesAt(g, tgt.x, tgt.y);
          if (curAt.empty() || curAt[0]->owner == me) continue;
        }
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && tgt.site && !core::siteOpen(*tgt.site)) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.item &&
            (tgt.item->status != 1 || tgt.item->x == NONE)) {
          continue;
        }
        if (tgt.kind == TargetKind::FOG_FRONTIER && g.map->options.hiddenMap != 0) {
          if (tgt.assignedStacks >= 1) continue;
          if (!localFogIndices.empty() && !localFogIndices.count(ti)) continue;
          bool stillHasUnseen = false;
          for (int oy = -1; oy <= 1 && !stillHasUnseen; oy++) {
            for (int ox = -1; ox <= 1 && !stillHasUnseen; ox++) {
              int ux = tgt.x + ox, uy = tgt.y + oy;
              if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
                  !belief::isTileVisible(g, me, ux, uy)) {
                stillHasUnseen = true;
              }
            }
          }
          if (!stillHasUnseen) continue;
        }

        if (tgt.kind == TargetKind::HOLD_GARRISON && tgt.city != lg.city) continue;
        if (tgt.kind == TargetKind::REINFORCE_CITY && tgt.city == lg.city) continue;
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && !seed->hero()) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.item && !seed->hero()) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.site &&
            !seed->blessings.empty() && !seed->hero()) {
          continue;
        }
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.assignedStacks >= 1) continue;
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && tgt.assignedStacks >= 1) continue;

        bool isCombatTgt = (tgt.kind == TargetKind::NEUTRAL_CITY ||
                            tgt.kind == TargetKind::ENEMY_CITY ||
                            tgt.kind == TargetKind::ENEMY_STACK);
        for (size_t si = 0; si < preStacks.size(); si++) {
          const PrebuiltStack& ps = preStacks[si];
          const core::Flood& activeFlood = ps.flies ? flyFlood : landFlood;
          int pCost = targetPathCost(g, activeFlood, tgt);
          if (pCost >= core::UNREACHED) continue;

          float nearStr = 0.0f;
          ml::StackVec optSv = ps.sv;
          if (tgt.kind == TargetKind::HOLD_GARRISON) {
            nearStr = remGroupStr;
            optSv[14] = std::tanh((committedCityGuardStr + ps.str) / 10.0f);
          } else if (isCombatTgt) {
            nearStr = std::max(0.0f, tgt.nearbyFriendlyStr - remGroupStr);
          }

          ml::TargetVec tv = buildTargetFeatures(g, side, ps.units, tgt, nearStr, caution, pCost);
          ml::MissionInVec mIn = ml::packMissionInput(ctx, optSv, tv);
          auto raw = policy.missionNet.forward(mIn);
          options.push_back(
              JointOption{ti, si, mIn, tv, ml::MissionOutput{raw[0], raw[1]}});
        }
      }
      }  // scoringIndex

      if (options.empty()) {
        seed->aiMoved = true;
        continue;
      }

      std::array<int, 8> kindCounts{};
      for (const auto& op : options) {
        kindCounts[(int)targets[op.targetIdx].kind]++;
      }
      std::vector<float> logits(options.size());
      std::vector<float> logMult(options.size());
      for (size_t i = 0; i < options.size(); i++) {
        logits[i] = options[i].out.utility;
        int kc = std::max(1, kindCounts[(int)targets[options[i].targetIdx].kind]);
        logMult[i] = std::log((float)kc);
      }
      if (snapshots) {
        // Dry run: record this group's first decision and move on, touching nothing
        MissionSnapshot snap;
        snap.candidates.reserve(options.size());
        for (const auto& op : options) {
          const CandidateTarget& t = targets[op.targetIdx];
          snap.candidates.push_back(op.inVec);
          snap.kind.push_back((int)t.kind);
          snap.cityIndex.push_back(t.city ? t.city->index : NONE);
          snap.tx.push_back(t.x);
          snap.ty.push_back(t.y);
          snap.stackSize.push_back((int)preStacks[op.stackOptIdx].units.size());
        }
        snap.logMult = logMult;
        snap.seed = reinterpret_cast<std::uintptr_t>(seed);
        for (Army* a : lg.units) snap.group.push_back(reinterpret_cast<std::uintptr_t>(a));
        snap.startX = lg.x;
        snap.startY = lg.y;
        snap.startCity = (lg.city && lg.city->ownerIndex == me) ? lg.city->index : NONE;
        snapshots->push_back(std::move(snap));
        break;
      }
      float chosenLogProb = 0.0f;
      int chosenOptIdx = ml::selectFromLogits(logits, sampler, &chosenLogProb, &logMult);
      const JointOption& chosenOpt = options[chosenOptIdx];
      CandidateTarget& chosenTgt = targets[chosenOpt.targetIdx];
      std::vector<Army*> currentStack = preStacks[chosenOpt.stackOptIdx].units;
      float stackStr = preStacks[chosenOpt.stackOptIdx].str;

      // The per-stack critic only matters while recording a training rollout;
      // it reuses the privileged features computed as the turn began.
      const bool recording = sampler && sampler->currentTurn;
      ml::StackValueInVec preGroupState =
          recording ? ml::packStackValueInput(ctx, preStacks.back().sv, sampler->currentTurn->startCritic)
                    : ml::StackValueInVec{};
      float preGroupValue = recording ? policy.evaluateStackValue(preGroupState) : 0.0f;

      std::vector<std::uintptr_t> chosenUnitIds;
      std::vector<std::uintptr_t> poolUnitIds;
      if (sampler && sampler->currentTurn) {
        chosenUnitIds.reserve(currentStack.size());
        for (Army* a : currentStack) chosenUnitIds.push_back(reinterpret_cast<std::uintptr_t>(a));
        const auto& fullPool = preStacks.back().units;
        poolUnitIds.reserve(fullPool.size());
        for (Army* a : fullPool) poolUnitIds.push_back(reinterpret_cast<std::uintptr_t>(a));
      }

      for (size_t i = 1; i < currentStack.size(); i++) {
        auto it = std::find(pool.begin(), pool.end(), currentStack[i]);
        if (it != pool.end()) pool.erase(it);
      }

      bool isCombatTarget = (chosenTgt.kind == TargetKind::NEUTRAL_CITY ||
                             chosenTgt.kind == TargetKind::ENEMY_CITY ||
                             chosenTgt.kind == TargetKind::ENEMY_STACK);
      bool stageNear = false;
      float stageLogProb = 0.0f;
      if (isCombatTarget) {
        stageNear = ml::selectBinary(chosenOpt.out.stageNearLogit, sampler, &stageLogProb);
      }

      float scoreBefore = (sampler && sampler->currentTurn) ? ml::computeEmpireScore(g, me) : 0.0f;

      if (chosenTgt.kind == TargetKind::HOLD_GARRISON) {
        for (Army* a : currentStack) a->aiMoved = true;
        chosenTgt.assignedStacks++;
        chosenTgt.assignedStrength += stackStr;
      } else {
        chosenTgt.assignedStacks++;
        chosenTgt.assignedStrength += stackStr;

        if (chosenTgt.kind == TargetKind::ENEMY_CITY && chosenTgt.city &&
            chosenOpt.out.utility > bestEnemyCityScore) {
          bestEnemyCityScore = chosenOpt.out.utility;
          bestEnemyCityTarget = chosenTgt.city->index;
        }

        bool ok = executeStackMission(g, side, currentStack, chosenTgt, stageNear, policy, ctx, sampler);
        if (!ok) {
          blockedTargets.insert(chosenOpt.targetIdx);
          chosenTgt.assignedStacks = std::max(0, chosenTgt.assignedStacks - 1);
          chosenTgt.assignedStrength = std::max(0.0f, chosenTgt.assignedStrength - stackStr);
          for (Army* a : currentStack) {
            if (g.alive(a) && a->moves > 0) a->aiMoved = true;
          }
        } else {
          if (!currentStack.empty() && g.alive(currentStack[0])) {
            float invDisc = chosenOpt.tv[8] > 1e-4f ? (1.0f / chosenOpt.tv[8] - 1.0f) : 0.0f;
            side.aiThoughts.push_back(AIThought{
                "Mission",
                fmt("%d-unit stack (%s)", (int)currentStack.size(), currentStack[0]->name.c_str()),
                chosenTgt.label,
                currentStack[0]->x, currentStack[0]->y,
                chosenTgt.x, chosenTgt.y,
                chosenOpt.out.utility,
                chosenOpt.tv[10],
                stageNear,
                fmt("U=%.2f Win=%.0f%% Turns=%.1f %s", chosenOpt.out.utility,
                    chosenOpt.tv[10] * 100.0f, invDisc,
                    stageNear ? "[STAGE FOR SYNC]" : "[EXECUTE]")});
          }

          // Add newly revealed neutral or enemy cities immediately so remaining stacks can act on them
          for (auto& c : g.map->cities) {
            if (c.razed || c.ownerIndex == me || !belief::isCityVisible(g, me, c)) continue;
            bool exists = false;
            for (const auto& t : targets) {
              if (t.city == &c) {
                exists = true;
                break;
              }
            }
            if (!exists) {
              bool isEnemyCap = false;
              for (int s = 0; s < 8; s++) {
                if (s != me && g.map->sides[s].capital == &c) isEnemyCap = true;
              }
              if (c.ownerIndex == NONE) {
                CandidateTarget neut;
                neut.kind = TargetKind::NEUTRAL_CITY;
                neut.x = c.x;
                neut.y = c.y;
                neut.city = &c;
                neut.owner = NONE;
                neut.value = (float)c.income + (float)c.slots.size() * 5.0f;
                neut.isEnemyCapital = isEnemyCap;
                neut.label = "Capture Neutral " + c.name;
                targets.push_back(std::move(neut));
              } else if (attackable(g, me, c.ownerIndex, &c)) {
                CandidateTarget enem;
                enem.kind = TargetKind::ENEMY_CITY;
                enem.x = c.x;
                enem.y = c.y;
                enem.city = &c;
                enem.owner = c.ownerIndex;
                enem.value = (float)c.income + (float)c.defence * 6.0f;
                enem.isEnemyCapital = isEnemyCap;
                enem.label = "Siege " + c.name;
                targets.push_back(std::move(enem));
              }
            }
          }
        }
      }

      // Departure credit (training only): did this move leave its own city empty?
      int vacatedCity = NONE;
      if (recording && lg.city && lg.city->ownerIndex == me && chosenTgt.kind != TargetKind::HOLD_GARRISON) {
        bool anyLeft = false;
        for (int dy = 0; dy <= 1 && !anyLeft; dy++) {
          for (int dx = 0; dx <= 1 && !anyLeft; dx++) {
            for (Army* a : game::armiesAt(g, lg.city->x + dx, lg.city->y + dy)) {
              if (a->owner == me) {
                anyLeft = true;
                break;
              }
            }
          }
        }
        if (!anyLeft) vacatedCity = lg.city->index;
      }

      if (sampler && sampler->currentTurn && (options.size() > 1 || isCombatTarget)) {
        float scoreAfter = ml::computeEmpireScore(g, me);
        ml::MissionStepRecord rec;
        rec.candidates.reserve(options.size());
        for (const auto& op : options) rec.candidates.push_back(op.inVec);
        rec.logMult = std::move(logMult);
        rec.chosenIdx = chosenOptIdx;
        rec.oldLogProb = chosenLogProb;
        rec.hasStageChoice = isCombatTarget;
        rec.stageChosen = stageNear;
        rec.oldStageLogProb = stageLogProb;
        rec.immediateDeltaScore = scoreAfter - scoreBefore;
        rec.stackValueIn = preGroupState;
        rec.stackValuePred = preGroupValue;
        rec.unitIds = std::move(chosenUnitIds);
        rec.poolUnitIds = std::move(poolUnitIds);
        rec.vacatedCity = vacatedCity;
        sampler->currentTurn->missionSteps.push_back(std::move(rec));
      }
    }
  }

  if (snapshots) return;
  diplomacy::setAlliedTargetCity(g, me, bestEnemyCityTarget);
  if (pause) pause("missions");
}

void runEconomyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                     const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
  int me = side.index;
  const SideBelief& b = belief::ensure(side);

  auto makeHoldGoldIn = [&](const ml::ContextVec& curCtx) {
    ml::EconVec e{};
    e[0] = 1.0f;
    e[10] = std::tanh((float)(game::upkeep(g, side) - game::income(g, side)) / 40.0f);
    return ml::packEconInput(curCtx, e);
  };

  // Army retention vs disbanding: only offered when upkeep exceeds income, the
  // one situation where the rules make disbanding worth anything. Keeping an
  // irreversible action off the table otherwise stops an untrained policy from
  // dissolving its own army during early training.
  std::vector<Army*> myArmies = game::sideArmies(g, side);
  bool overBudget = game::upkeep(g, side) > game::income(g, side);
  if (overBudget && myArmies.size() > 3) {
    for (Army* a : myArmies) {
      if (!g.alive(a) || a->hero() || a->transit || a->x == NONE) continue;
      City* c = game::cityAt(g, a->x, a->y);
      if (!c || c->ownerIndex != me) continue;
      int cityArmies = 0;
      for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
          cityArmies += (int)game::armiesAt(g, c->x + dx, c->y + dy).size();
        }
      }
      if (cityArmies <= 2) continue;

      const ArmyType* at = g.types.byId(a->type);
      float threat = (c->index < (int)b.cityThreat2Turn.size()) ? b.cityThreat2Turn[c->index] : 0.0f;
      float distE = (c->index < (int)b.cityNearestEnemyDist.size()) ? b.cityNearestEnemyDist[c->index] : 100.0f;

      ml::ContextVec curCtx = ml::buildContext(g, side);

      ml::EconVec e{};
      e[5] = 1.0f;
      e[7] = (float)a->strength / 9.0f;
      e[8] = (float)a->maxMoves / 24.0f;
      e[10] = (float)a->upkeep / 15.0f;
      e[11] = (at && at->flies) ? 1.0f : 0.0f;
      e[12] = (at && (at->siege || at->bonus[48] != 0)) ? 1.0f : 0.0f;
      e[13] = (a->maxMoves >= 14 && (!at || !at->flies)) ? 1.0f : 0.0f;
      e[14] = std::tanh(threat / 25.0f);
      e[15] = std::tanh(distE / 60.0f);
      e[ml::ECON_CURRENT] = 1.0f;   // keeping the army is the status quo
      ml::EconInVec keepIn = ml::packEconInput(curCtx, e);

      ml::EconVec disbandVec = e;
      disbandVec[10] = -e[10];
      disbandVec[ml::ECON_CURRENT] = 0.0f;
      ml::EconInVec disbandIn = ml::packEconInput(curCtx, disbandVec);

      std::vector<ml::EconInVec> cands = {keepIn, disbandIn};
      std::vector<float> logits = {policy.econNet.forward(keepIn)[0],
                                   policy.econNet.forward(disbandIn)[0]};
      float lp = 0.0f;
      int pick = ml::selectFromLogits(logits, sampler, &lp);
      if (sampler && sampler->currentTurn) {
        sampler->currentTurn->econSteps.push_back(
            ml::EconStepRecord{std::move(cands), {}, pick, lp});
      }
      if (pick == 1) {
        game::disband(g, side, {a});
      }
    }
  }

  std::vector<City*> ownCities = game::sideCities(g, side);

  // Empire-wide slot upgrade rounds (up to 3 purchases/turn): each round asks EconNet
  // to compare HoldGold against every legal (city, unit) upgrade, stopping immediately
  // as soon as EconNet chooses HoldGold so multi-city empires neither hoard 10k gold
  // nor suffer 1 - p^N treasury-draining coin-flip cascades across N cities.
  auto menu = game::buyableTypes(g);
  for (int buyRound = 0; buyRound < 3 && !ownCities.empty() && side.gold >= 250; buyRound++) {
    struct BuyOption {
      City* city = nullptr;
      int typeId = NONE;
      ml::EconInVec inVec{};
    };
    ml::ContextVec buyCtx = ml::buildContext(g, side);
    std::vector<BuyOption> buyOpts;
    buyOpts.push_back(BuyOption{nullptr, NONE, makeHoldGoldIn(buyCtx)});

    for (City* c : ownCities) {
      bool fullSlots = (int)c->slots.size() >= rules::PRODUCTION_SLOTS;
      int replacePrice = (!c->slots.empty()) ? c->slots[0].price : 0;
      int bestExistingStr = 0;
      for (const auto& sl : c->slots) bestExistingStr = std::max(bestExistingStr, sl.strength);
      float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
      float distE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 100.0f;
      for (const ArmyType* at : menu) {
        int price = std::abs(at->price);
        if (price <= 0 || price + 100 > side.gold) continue;
        if (fullSlots && price <= replacePrice) continue;
        if (!game::cannotBuy(g, side, *c, *at).empty()) continue;

        ml::EconVec e{};
        e[3] = 1.0f;
        e[7] = (float)at->strength / 9.0f;
        e[8] = (float)at->move / 24.0f;
        e[9] = (float)at->time / 6.0f;
        e[10] = (float)price / std::max(100.0f, (float)(side.gold - 80));
        e[11] = at->flies ? 1.0f : 0.0f;
        e[12] = (at->siege || at->bonus[48] != 0) ? 1.0f : 0.0f;
        e[13] = (at->move >= 14 && !at->flies) ? 1.0f : 0.0f;
        e[14] = std::tanh(threat / 25.0f);
        e[15] = std::tanh(distE / 60.0f);
        e[16] = (float)c->slots.size() / (float)rules::PRODUCTION_SLOTS;
        e[17] = (float)(at->strength - bestExistingStr) / 9.0f;
        e[18] = (float)(at->strength * at->strength) / (float)std::max(1, at->time * 16);
        buyOpts.push_back(BuyOption{c, at->id, ml::packEconInput(buyCtx, e)});
      }
    }

    if (buyOpts.size() <= 1) break;
    std::vector<float> logits(buyOpts.size());
    std::vector<float> logMult(buyOpts.size(), 0.0f);
    for (size_t i = 0; i < buyOpts.size(); i++) {
      logits[i] = policy.econNet.forward(buyOpts[i].inVec)[0];
      if (i > 0 && buyOpts.size() > 2) logMult[i] = std::log((float)(buyOpts.size() - 1));
    }
    float lp = 0.0f;
    int pick = ml::selectFromLogits(logits, sampler, &lp, &logMult);
    if (sampler && sampler->currentTurn) {
      std::vector<ml::EconInVec> cands;
      cands.reserve(buyOpts.size());
      for (const auto& bo : buyOpts) cands.push_back(bo.inVec);
      sampler->currentTurn->econSteps.push_back(
          ml::EconStepRecord{std::move(cands), std::move(logMult), pick, lp});
    }
    if (pick == 0 || !buyOpts[pick].city) break;

    City* bestBuyCity = buyOpts[pick].city;
    int boughtType = buyOpts[pick].typeId;
    int slotIdx = game::buySlot(*bestBuyCity);
    game::buyProduction(g, side, *bestBuyCity, slotIdx, boughtType);
    game::sortProduction(*bestBuyCity);
    for (int si = 0; si < (int)bestBuyCity->slots.size(); si++) {
      if (bestBuyCity->slots[si].type == boughtType) {
        game::setProduction(g, *bestBuyCity, si);
        break;
      }
    }
  }

  // City production macro-action: queried whenever a city has no active build
  // or has just completed a build (`c->countdown == slot.time`, matching
  // game.cpp:1331), so multi-turn unit builds complete without mid-countdown
  // cancellation while cities remain free to switch units after each build.
  ml::ContextVec prodCtx = ml::buildContext(g, side);
  for (City* c : ownCities) {
    bool midCountdown = (c->producing >= 0 && c->producing < (int)c->slots.size() &&
                         c->countdown > 0 && c->countdown < c->slots[c->producing].time);
    if (c->slots.empty() || midCountdown) continue;
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    float distE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 100.0f;
    int garCount = 0;
    for (int dy = 0; dy <= 1; dy++) {
      for (int dx = 0; dx <= 1; dx++) {
        garCount += (int)game::armiesAt(g, c->x + dx, c->y + dy).size();
      }
    }

    ml::EconVec stopVec{};
    stopVec[2] = 1.0f;
    stopVec[10] = std::tanh((float)(game::upkeep(g, side) - game::income(g, side)) / 30.0f);
    stopVec[14] = std::tanh(threat / 25.0f);
    stopVec[15] = std::tanh(distE / 60.0f);
    stopVec[16] = std::tanh((float)garCount / 6.0f);
    stopVec[ml::ECON_CURRENT] = (c->producing == NONE) ? 1.0f : 0.0f;

    std::vector<int> slotIndices = {NONE};
    std::vector<ml::EconInVec> cands = {ml::packEconInput(prodCtx, stopVec)};
    for (size_t si = 0; si < c->slots.size(); si++) {
      const Slot& sl = c->slots[si];
      const ArmyType* at = g.types.byId(sl.type);
      bool isFlier = at && at->flies;
      bool isSiegeOrSpec = at && (at->siege || at->bonus[48] != 0 || at->bonus[52] != 0);
      bool isCav = sl.move >= 14 && !isFlier;

      ml::EconVec e{};
      e[1] = 1.0f;
      e[7] = (float)sl.strength / 9.0f;
      e[8] = (float)sl.move / 24.0f;
      e[9] = (float)sl.time / 6.0f;
      e[10] = (float)sl.cost / 20.0f;
      e[11] = isFlier ? 1.0f : 0.0f;
      e[12] = isSiegeOrSpec ? 1.0f : 0.0f;
      e[13] = isCav ? 1.0f : 0.0f;
      e[14] = std::tanh(threat / 25.0f);
      e[15] = std::tanh(distE / 60.0f);
      e[16] = std::tanh((float)garCount / 6.0f);
      e[18] = (float)(sl.strength * sl.strength) / (float)std::max(1, sl.time * 16);
      e[ml::ECON_CURRENT] = (c->producing == (int)si) ? 1.0f : 0.0f;
      slotIndices.push_back((int)si);
      cands.push_back(ml::packEconInput(prodCtx, e));
    }

    if (cands.size() == 1) {
      game::setProduction(g, *c, NONE);
      continue;
    }

    std::vector<float> logits(cands.size());
    std::vector<float> logMult(cands.size(), 0.0f);
    for (size_t i = 0; i < cands.size(); i++) {
      logits[i] = policy.econNet.forward(cands[i])[0];
      if (i > 0 && cands.size() > 2) logMult[i] = std::log((float)(cands.size() - 1));
    }
    float lp = 0.0f;
    int pick = ml::selectFromLogits(logits, sampler, &lp, &logMult);
    if (sampler && sampler->currentTurn) {
      sampler->currentTurn->econSteps.push_back(
          ml::EconStepRecord{std::move(cands), std::move(logMult), pick, lp});
    }

    if (pick == 0) {
      game::setProduction(g, *c, NONE);
    } else if (c->producing != slotIndices[pick] || !game::producing(*c)) {
      game::setProduction(g, *c, slotIndices[pick]);
    }
  }

  // City vectoring: re-evaluated each turn, but the vector already set is a
  // flagged candidate (ECON_CURRENT) rather than wiped, so keeping it is a choice
  for (City* src : ownCities) {
    if (!game::producing(*src) || src->producing < 0 || src->producing >= (int)src->slots.size()) {
      src->vectorTo = NONE;
      continue;
    }
    const Slot& sl = src->slots[src->producing];
    float srcThreat = src->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[src->index] : 0.0f;
    float srcDistE = src->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[src->index] : 100.0f;

    ml::EconVec keepHere{};
    keepHere[4] = 1.0f;
    keepHere[14] = std::tanh(srcThreat / 25.0f);
    keepHere[15] = std::tanh(srcDistE / 60.0f);
    keepHere[17] = 0.0f;
    keepHere[ml::ECON_CURRENT] = src->vectorTo == NONE ? 1.0f : 0.0f;

    std::vector<City*> destCities = {nullptr};
    std::vector<ml::EconInVec> cands = {ml::packEconInput(prodCtx, keepHere)};

    for (City* dst : ownCities) {
      if (dst == src) continue;
      bool isCurrent = (src->vectorTo == dst->index);
      if (!isCurrent && (int)game::vectoredTo(g, dst).size() >= game::MAX_VECTORED_TO) continue;
      int vTurns = std::max(1, move::distance(src->x, src->y, dst->x, dst->y) / 14 + 1);
      if (vTurns > 4) continue;
      float dstThreat = dst->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[dst->index] : 0.0f;
      float dstDistE = dst->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[dst->index] : 100.0f;
      float threatGain = (dstThreat - srcThreat) / 25.0f + (srcDistE - dstDistE) / 45.0f;

      ml::EconVec v{};
      v[4] = 1.0f;
      v[7] = (float)sl.strength / 9.0f;
      v[8] = (float)sl.move / 24.0f;
      v[9] = (float)vTurns / 4.0f;
      v[14] = std::tanh(dstThreat / 25.0f);
      v[15] = std::tanh(dstDistE / 60.0f);
      v[17] = std::tanh(threatGain);
      v[ml::ECON_CURRENT] = isCurrent ? 1.0f : 0.0f;
      destCities.push_back(dst);
      cands.push_back(ml::packEconInput(prodCtx, v));
    }

    if (cands.size() > 1) {
      std::vector<float> logits(cands.size());
      std::vector<float> logMult(cands.size(), 0.0f);
      for (size_t i = 0; i < cands.size(); i++) {
        logits[i] = policy.econNet.forward(cands[i])[0];
        if (i > 0 && cands.size() > 2) logMult[i] = std::log((float)(cands.size() - 1));
      }
      float lp = 0.0f;
      int pick = ml::selectFromLogits(logits, sampler, &lp, &logMult);
      if (sampler && sampler->currentTurn) {
        sampler->currentTurn->econSteps.push_back(
            ml::EconStepRecord{std::move(cands), std::move(logMult), pick, lp});
      }
      City* bestDest = destCities[pick];
      game::vector(g, *src, bestDest);
      if (bestDest) {
        side.aiThoughts.push_back(AIThought{
            "Economy", src->name, bestDest->name, src->x, src->y, bestDest->x, bestDest->y,
            logits[pick], 1.0f, false,
            fmt("Vectoring %s from %s -> %s", sl.name.c_str(), src->name.c_str(),
                bestDest->name.c_str())});
      }
    }
  }
}

}  // namespace

void playTurn(Game& g, Side& side, const PauseCallback& pause,
              const ml::PolicyNetwork* customPolicy, ml::RolloutSampler* sampler) {
  const ml::PolicyNetwork& policy = customPolicy ? *customPolicy : ml::activePolicy();
  // Outside training the policy is still sampled, as in training, but at the
  // deployment temperature and from a generator seeded by the game state (never
  // advancing the game's own dice), so play stays reproducible. Taking the single
  // best-scoring option instead would follow a different policy from the one
  // trained: for a spread-out distribution its mode is not its typical behaviour.
  ml::RolloutSampler deploySampler;
  if (!sampler) {
    deploySampler.temperature = ml::deployTemperature();
    deploySampler.rngState = (g.rng.state * 2654435761u) ^ (uint32_t)(g.turn * 40503u) ^
                             (uint32_t)((side.index + 1) * 2246822519u);
    sampler = &deploySampler;
  }
  for (Army* a : g.armies) {
    if (a->owner == side.index) a->aiMoved = false;
  }
  side.aiThoughts.clear();

  std::array<bool, 8> diploDecided{};
  for (int wave = 0; wave < 6; wave++) {
    belief::update(g, side);
    ml::ContextVec waveCtx = ml::buildContext(g, side);
    if (wave == 0) {
      ml::CriticVec critic = ml::buildCriticFeatures(g, side);
      float v = policy.evaluateValue(waveCtx, critic);
      side.lastBoardValue = v;
      if (sampler && sampler->currentTurn) {
        sampler->currentTurn->startCtx = waveCtx;
        sampler->currentTurn->startCritic = critic;
        sampler->currentTurn->valuePred = v;
        sampler->currentTurn->startScore = ml::computeEmpireScore(g, side.index);
      }
    }
    runDiplomacyPhase(g, side, policy, waveCtx, sampler, diploDecided);
    if (wave == 0 && pause) pause("diplomacy");
    runMilitaryAndExplorationPhase(g, side, policy, waveCtx, wave == 0 ? pause : nullptr, sampler);
    bool anyLeft = false;
    for (Army* a : g.armies) {
      if (a->owner == side.index && a->x != NONE && !a->transit && !a->aiMoved && a->moves >= 2) {
        anyLeft = true;
        break;
      }
    }
    if (!anyLeft) break;
  }

  // A city emptied by one move but re-garrisoned by a later one was not left open
  if (sampler && sampler->currentTurn) {
    for (auto& ms : sampler->currentTurn->missionSteps) {
      if (ms.vacatedCity == NONE || ms.vacatedCity >= (int)g.map->cities.size()) continue;
      const City& vc = g.map->cities[ms.vacatedCity];
      for (int dy = 0; dy <= 1 && ms.vacatedCity != NONE; dy++) {
        for (int dx = 0; dx <= 1 && ms.vacatedCity != NONE; dx++) {
          for (Army* a : game::armiesAt(g, vc.x + dx, vc.y + dy)) {
            if (a->owner == side.index) {
              ms.vacatedCity = NONE;
              break;
            }
          }
        }
      }
    }
  }

  belief::update(g, side);
  ml::ContextVec ctx = ml::buildContext(g, side);
  side.lastBoardValue = policy.evaluateValue(ctx, ml::buildCriticFeatures(g, side));

  runEconomyPhase(g, side, policy, ctx, sampler);
  if (pause) pause("economy");
}

std::vector<MissionSnapshot> snapshotMissions(Game& g, Side& side, const ml::PolicyNetwork& policy) {
  std::vector<MissionSnapshot> out;
  std::vector<bool> movedBefore;
  for (Army* a : g.armies) movedBefore.push_back(a->aiMoved);
  for (Army* a : g.armies) {
    if (a->owner == side.index) a->aiMoved = false;
  }
  belief::update(g, side);
  ml::ContextVec ctx = ml::buildContext(g, side);
  runMilitaryAndExplorationPhase(g, side, policy, ctx, nullptr, nullptr, &out);
  for (size_t i = 0; i < g.armies.size() && i < movedBefore.size(); i++) g.armies[i]->aiMoved = movedBefore[i];
  return out;
}

City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* customPolicy) {
  // No policy head is trained for where a new hero appears, so this is an
  // explicit rule rather than an untrained proxy: the own city under the
  // highest visible threat, else the game's default (the capital).
  (void)customPolicy;
  belief::update(g, side);
  const SideBelief& b = belief::ensure(side);
  City* best = dflt;
  float bestThreat = 0.0f;
  for (City* c : game::sideCities(g, side)) {
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    if (threat > bestThreat) {
      bestThreat = threat;
      best = c;
    }
  }
  return best;
}

}  // namespace w2::ai::modern
