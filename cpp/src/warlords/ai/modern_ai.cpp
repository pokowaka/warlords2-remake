#include "warlords/ai/modern_ai.hpp"

#include <algorithm>
#include <cmath>
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

struct CandidateTarget {
  TargetKind kind = TargetKind::HOLD_GARRISON;
  int x = 0, y = 0;
  City* city = nullptr;
  Site* site = nullptr;
  Item* item = nullptr;
  int owner = NONE;
  float value = 0.0f;
  bool isCapitalOrQuest = false;
  float enemyThreat = 0.0f;
  float nearbyFriendlyStr = 0.0f;
  int assignedStacks = 0;
  float assignedStrength = 0.0f;
  std::string label;
};

ml::StackVec buildStackFeatures(const Game& g, const std::vector<Army*>& stack, int prevMinMove,
                                bool isExploreTarget = false) {
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
  s[1] = std::min(1.5f, (float)totalStr / 50.0f);
  s[2] = (float)totalStr / (float)(stack.size() * 9);
  s[3] = std::min(1.5f, (float)minMove / 20.0f);
  s[4] = hasHero ? 1.0f : 0.0f;
  s[5] = hasFlier ? 1.0f : 0.0f;
  s[6] = allFly ? 1.0f : 0.0f;
  s[7] = hasSiege ? 1.0f : 0.0f;
  s[8] = hasSpecial ? 1.0f : 0.0f;
  s[9] = (float)blessed / (float)stack.size();
  s[10] = std::min(1.5f, (float)totalUpkeep / 25.0f);
  s[11] = std::min(1.5f, (float)heroCmd / 4.0f);
  s[12] = (stack.size() == 1 && isExploreTarget && (allFly || minMove >= 14)) ? 1.0f : 0.0f;
  s[13] = speedDrop;
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
                                  float cautionBias, int pathCost = -1) {
  ml::TargetVec t{};
  t[(int)tgt.kind] = 1.0f;

  int sx = stack[0]->x, sy = stack[0]->y;
  int minMove = std::max(2, move::stackMoves(stack));
  int effCost = pathCost >= 0 ? pathCost : (move::distance(sx, sy, tgt.x, tgt.y) * 2);
  float turnsToReach = std::clamp((float)effCost / (float)std::max(6, minMove), 0.0f, 20.0f);

  float winNow = 0.0f, winNext = 0.0f, survRatio = 0.0f;
  float effValue = std::min(1.5f, tgt.value / 35.0f);
  float alreadyAssigned = (float)tgt.assignedStacks / 3.0f;
  if (tgt.kind == TargetKind::NEUTRAL_CITY || tgt.kind == TargetKind::ENEMY_CITY ||
      tgt.kind == TargetKind::ENEMY_STACK) {
    auto est = belief::estimateCombat(g, side, stack, tgt.x, tgt.y, cautionBias);
    float myStr = 0.0f;
    for (Army* a : stack) myStr += (float)a->strength;
    float defStr = std::max(1.0f, est.estimatedDefenderStrength);
    float reqStr = std::max(3.5f, est.estimatedDefenderStrength * 1.35f);
    float alreadyCover = std::clamp(tgt.assignedStrength / reqStr, 0.0f, 1.0f);

    int freeSlots = std::max(0, rules::MAX_STACK - (int)stack.size());
    float maxMergeStr = std::min(nearbyFriendlyStr + tgt.assignedStrength, (float)freeSlots * 5.5f);
    float smoothRatio = myStr / (myStr + defStr);
    winNow = 0.8f * est.winProb + 0.2f * smoothRatio;
    survRatio = 0.8f * (est.expectedSurvivors / (float)std::max<size_t>(1, stack.size())) + 0.2f * smoothRatio;
    winNext = std::clamp(winNow + 0.45f * maxMergeStr / defStr, 0.0f, 1.0f);
    if (alreadyCover >= 0.85f) {
      // Earlier stack(s) already have sufficient strength to take this target;
      // scale marginal conquest utility by remaining uncovered fraction
      float remFrac = std::max(0.0f, 1.0f - alreadyCover);
      winNow *= remFrac;
      winNext *= remFrac;
      survRatio *= remFrac;
      effValue *= remFrac;
      alreadyAssigned = (float)tgt.assignedStacks + 2.0f * alreadyCover;
    } else if (maxMergeStr > 0.0f && freeSlots > 0) {
      winNow = std::clamp(winNow + 0.35f * maxMergeStr / defStr, 0.0f, 1.0f);
    }
  } else if (tgt.kind == TargetKind::FOG_FRONTIER) {
    float myStr = 0.0f;
    for (Army* a : stack) {
      myStr += (float)a->strength;
      if (a->hero()) myStr += 2.0f;
    }
    if (stack.size() == 1 && core::flies(g, stack[0])) {
      winNow = 0.55f;
      survRatio = 0.80f;
    } else {
      float expDefStr = 6.5f;
      winNow = std::clamp((myStr - 2.5f) / (expDefStr + 3.5f), 0.05f, 0.78f);
      survRatio = std::clamp((myStr - 3.0f) / (expDefStr + 4.5f), 0.05f, 0.75f);
      winNext = std::clamp(winNow + 0.25f * std::min(nearbyFriendlyStr, 10.0f) / expDefStr, 0.0f, 0.82f);
    }
  }

  float marginalHoldGain = 0.0f;
  if (tgt.kind == TargetKind::HOLD_GARRISON || tgt.kind == TargetKind::REINFORCE_CITY) {
    float stackStr = 0.0f;
    bool hasFastOrHero = false;
    for (Army* a : stack) {
      stackStr += (float)a->strength;
      if (a->hero() || a->maxMoves >= 14 || g.types.byId(a->type)->flies) hasFastOrHero = true;
    }
    float remFriendlyStr = tgt.assignedStrength + nearbyFriendlyStr;
    if (tgt.enemyThreat > 0.0f) {
      float neededStr = std::max(5.0f, tgt.enemyThreat * 1.15f);
      float beforeCover = std::min(1.0f, remFriendlyStr / neededStr);
      float afterCover = std::min(1.0f, (remFriendlyStr + stackStr) / neededStr);
      marginalHoldGain = std::max(0.0f, afterCover - beforeCover) * 1.5f;
      if (tgt.isCapitalOrQuest) marginalHoldGain *= 1.35f;
    } else if (tgt.kind == TargetKind::HOLD_GARRISON) {
      // If leaving would abandon our owned city with 0 defenders (remFriendlyStr < 1.0f),
      // keeping the last non-flying/non-hero guard prevents free enemy city capture
      if (remFriendlyStr < 1.0f && !(hasFastOrHero && g.turn <= 3)) {
        marginalHoldGain = 1.65f;
      } else if (remFriendlyStr < 5.5f && stack.size() == 1 && stackStr < 6.0f && !hasFastOrHero &&
                 tgt.city && game::producing(*tgt.city) && tgt.city->countdown <= 2) {
        // Lone slow infantry in a city whose next unit finishes in 1-2 turns stages for its partner
        marginalHoldGain = 0.70f;
      } else {
        marginalHoldGain = -1.2f;
      }
    }
  }

  t[8] = std::min(2.0f, turnsToReach / 8.0f);
  t[9] = winNow;
  t[10] = winNext;
  t[11] = survRatio;
  t[12] = effValue;
  t[13] = tgt.isCapitalOrQuest ? 1.0f : 0.0f;
  t[14] = std::min(1.5f, tgt.enemyThreat / 35.0f);
  t[15] = marginalHoldGain;
  t[16] = alreadyAssigned;
  t[17] = std::min(1.5f, tgt.enemyThreat / 50.0f);
  return t;
}

void runDiplomacyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy, const ml::ContextVec& ctx) {
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
    if (s != me && g.map->sides[s].alive && diplomacy::state(g, me, s) == diplomacy::WAR) {
      activeWars++;
      if (primaryEnemy == NONE) primaryEnemy = s;
    }
  }

  float totalMyMobileStr = 0.0f;
  for (Army* a : g.armies) {
    if (a->owner == me && a->x != NONE && !a->transit) totalMyMobileStr += (float)a->strength;
  }

  int closestRival = NONE;
  float closestRivalDist = 999.0f;
  for (int otherIdx = 0; otherIdx < 8; otherIdx++) {
    Side& other = g.map->sides[otherIdx];
    if (otherIdx == me || !other.inUse || !other.alive) continue;
    if (b.visibleCitiesBySide[otherIdx] == 0 && b.theirBorderStrength[otherIdx] == 0.0f) continue;
    float md = 120.0f;
    for (City* mc : game::sideCities(g, side)) {
      for (City* vc : b.visibleCities) {
        if (vc->ownerIndex == otherIdx) {
          md = std::min(md, (float)move::distance(mc->x, mc->y, vc->x, vc->y));
        }
      }
    }
    if (md < closestRivalDist) {
      closestRivalDist = md;
      closestRival = otherIdx;
    }
  }

  for (int otherIdx = 0; otherIdx < 8; otherIdx++) {
    Side& other = g.map->sides[otherIdx];
    if (otherIdx == me || !other.inUse || !other.alive) continue;
    int curSt = diplomacy::state(g, me, otherIdx);
    // Under Hidden Map, do not alter diplomacy with factions we haven't made contact with yet
    if (g.map->options.hiddenMap != 0 && b.visibleCitiesBySide[otherIdx] == 0 &&
        b.theirBorderStrength[otherIdx] == 0.0f && curSt != diplomacy::WAR) {
      continue;
    }

    auto att = diplomacy::computeAttitude(g, me, otherIdx);
    bool curAllied = diplomacy::isAllied(g, me, otherIdx);

    float minDist = 120.0f;
    for (City* mc : game::sideCities(g, side)) {
      for (City* vc : b.visibleCities) {
        if (vc->ownerIndex == otherIdx) {
          minDist = std::min(minDist, (float)move::distance(mc->x, mc->y, vc->x, vc->y));
        }
      }
    }

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

    int otherWars = activeWars - (curSt == diplomacy::WAR ? 1 : 0);
    float effMyStr = std::max(b.myBorderStrength[otherIdx],
                              totalMyMobileStr * std::clamp(1.1f - minDist / 45.0f, 0.0f, 0.8f));
    bool isPrimaryTarget = (leaderSide == otherIdx && b.visibleCitiesBySide[otherIdx] * 3 >= totalVisCities) ||
                           (activeWars == 0 && otherIdx == closestRival && diplomacy::truceTurns(g, me, otherIdx) == 0);

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
    rv[11] = isPrimaryTarget ? 1.0f : 0.0f;
    rv[12] = std::min(1.0f, (float)sharedWars / 2.0f);
    rv[13] = std::tanh((float)grudge / 30.0f);
    rv[14] = (side.capital && side.capital->ownerIndex == otherIdx) ? 1.0f : 0.0f;
    rv[15] = std::tanh((float)diplomacy::goldOffer(g, otherIdx, me) / 200.0f);
    rv[16] = std::min(1.5f, minDist / 60.0f);
    rv[17] = std::min(1.5f, (float)otherWars / 3.0f);

    ml::DiploOutput dOut = policy.scoreDiplomacy(ctx, rv);

    int bestChoice = 1;  // 0=Allied, 1=Peace, 2=Uneasy, 3=War
    float bestLogit = dOut.stanceLogits[1];
    for (int k = 0; k < 4; k++) {
      if (dOut.stanceLogits[k] > bestLogit) {
        bestLogit = dOut.stanceLogits[k];
        bestChoice = k;
      }
    }

    if (bestChoice == 0) {
      diplomacy::propose(g, me, otherIdx, diplomacy::PEACE);
      diplomacy::proposeAlliance(g, me, otherIdx, true);
    } else if (bestChoice == 1) {
      diplomacy::propose(g, me, otherIdx, diplomacy::PEACE);
      diplomacy::proposeAlliance(g, me, otherIdx, false);
    } else if (bestChoice == 2) {
      diplomacy::propose(g, me, otherIdx, diplomacy::INTERMEDIATE);
    } else {
      diplomacy::propose(g, me, otherIdx, diplomacy::WAR);
      if (primaryEnemy == NONE) primaryEnemy = otherIdx;
    }

    // Learnable tribute / gold offer attachment
    if ((bestChoice == 0 || bestChoice == 1) && curSt == diplomacy::WAR &&
        dOut.tributeSignal < -0.25f && side.gold >= 100) {
      int offer = std::clamp((int)std::round(std::abs(dOut.tributeSignal) * (float)side.gold * 0.25f) / 25 * 25,
                             50, std::min(300, side.gold / 2));
      diplomacy::setGoldOffer(g, me, otherIdx, offer);
    }

    if ((bestChoice == 0 || bestChoice == 1) && dOut.jointWarSignal > 0.30f &&
        primaryEnemy != NONE && primaryEnemy != otherIdx) {
      diplomacy::setWarTarget(g, me, otherIdx, primaryEnemy);
    }

    const char* stanceNames[4] = {"ALLIED", "PEACE", "UNEASY", "WAR"};
    side.aiThoughts.push_back(AIThought{
        "Diplomacy", side.name, other.name, side.capX, side.capY, other.capX, other.capY,
        bestLogit, 0.0f, false,
        fmt("Stance -> %s (Attitude %+d, Trust %+d): %s",
            stanceNames[bestChoice], att.totalScore, att.trustScore, att.stanceSummary.c_str())});

    // Queue a throttled Turn-Start Envoy Missive to human players when a significant stance
    // or deal is proposed (at most 1 missive per human turn, 4-turn cooldown per sender)
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
      } else if (bestChoice == 2 && curSt == diplomacy::PEACE && dOut.tributeSignal > 0.35f) {
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
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx) {
  if (city.razed || city.ownerIndex != side.index) return;
  if (ai::questCapture(g, side, city, attackers)) return;

  const SideBelief& b = belief::ensure(side);
  float localThreat = city.index < (int)b.cityThreat1Turn.size() ? b.cityThreat1Turn[city.index] : 0.0f;
  float distEnemy = city.index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[city.index] : 20.0f;
  int sackGold = ai::groups::sackValue(g, city);
  int pillageGold = city.slots.size() >= 2
                        ? std::abs(g.types.byId(city.slots.back().type)->price) / 2
                        : 0;

  // Score all 4 legal spoil choices with Head 3:
  // 0=Occupy, 1=Pillage, 2=Sack, 3=Raze
  auto makeSpoilVec = [&](float ongoingIncome, float instantGold, float diploPen) {
    ml::EconVec e{};
    e[6] = 1.0f;  // isSpoilsChoice
    e[7] = ongoingIncome / 35.0f;
    e[14] = std::min(1.5f, localThreat / 35.0f);
    e[15] = std::min(1.5f, distEnemy / 60.0f);
    e[18] = std::min(1.5f, instantGold / 300.0f);
    e[19] = diploPen / 40.0f;
    return e;
  };

  float uOccupy = policy.scoreEconomy(ctx, makeSpoilVec((float)city.income, 0.0f, 0.0f));
  float uPillage = pillageGold > 0
                       ? policy.scoreEconomy(ctx, makeSpoilVec((float)city.income * 0.8f, (float)pillageGold, 4.0f))
                       : -1e9f;
  float uSack = sackGold > 0
                    ? policy.scoreEconomy(ctx, makeSpoilVec((float)city.income * 0.5f, (float)sackGold, 15.0f))
                    : -1e9f;

  bool isAnyCapital = false;
  for (int s = 0; s < 8; s++) {
    if (g.map->sides[s].capital == &city) isAnyCapital = true;
  }
  // Razing yields 0 ongoing income (negative ongoing asset value unless threatened by counter-attack)
  float razeOngoing = (localThreat > 15.0f ? 5.0f : -25.0f) +
                      personality::forSide(side).traits[personality::SCORCHED_EARTH] * 18.0f;
  float uRaze = !isAnyCapital
                    ? policy.scoreEconomy(ctx, makeSpoilVec(razeOngoing, 0.0f, 38.0f))
                    : -1e9f;

  int bestMode = 0;
  float bestScore = uOccupy;
  if (uPillage > bestScore) { bestScore = uPillage; bestMode = 1; }
  if (uSack > bestScore) { bestScore = uSack; bestMode = 2; }
  if (uRaze > bestScore) { bestScore = uRaze; bestMode = 3; }

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
                         bool stageNear, const ml::PolicyNetwork& policy, const ml::ContextVec& ctx) {
  stack.erase(std::remove_if(stack.begin(), stack.end(), [&](Army* a) { return !g.alive(a) || a->moves <= 0; }),
              stack.end());
  if (stack.empty()) return false;

  if (tgt.kind == TargetKind::HOLD_GARRISON) {
    for (Army* a : stack) a->aiMoved = true;
    return true;
  }

  int me = side.index;
  // If stack members are on different tiles of the same owned 2x2 city, gather them onto
  // stack[0]'s tile before walking so the entire stack moves together
  if (City* startCity = game::cityAt(g, stack[0]->x, stack[0]->y)) {
    if (startCity->ownerIndex == me) {
      for (Army* a : stack) {
        if (game::cityAt(g, a->x, a->y) == startCity) {
          a->x = stack[0]->x;
          a->y = stack[0]->y;
        }
      }
    }
  }

  int destX = tgt.x, destY = tgt.y;
  if (tgt.city && !stageNear) {
    if (tgt.city->ownerIndex == me) {
      // Find a tile of our city that has room for this stack
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
      // Aim for the nearest corner of the 2x2 target city, OR if friendly forces are already
      // rallying on an adjacent ring tile (with room to merge), join them on that tile first!
      destX = tgt.city->x + (tgt.city->x < stack[0]->x ? 1 : 0);
      destY = tgt.city->y + (tgt.city->y < stack[0]->y ? 1 : 0);
      float cBias = personality::forSide(side).traits[personality::CAUTION] * 0.5f;
      auto soloEst = belief::estimateCombat(g, side, stack, tgt.city->x, tgt.city->y, cBias);
      if (soloEst.winProb < 0.65f) {
        int bestRx = NONE, bestRy = NONE, bestRScore = -999;
        for (int dy = -1; dy <= 2; dy++) {
          for (int dx = -1; dx <= 2; dx++) {
            if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1) continue;
            int nx = tgt.city->x + dx, ny = tgt.city->y + dy;
            if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height) continue;
            if (!belief::isTileVisible(g, me, nx, ny)) continue;
            int terr = g.map->terrainType[g.map->tiles[ny * g.map->width + nx] & 0xff];
            if (terr == move::WATER || terr == move::SHORE || terr == move::MOUNTAINS) continue;
            auto occ = game::armiesAt(g, nx, ny);
            if (!occ.empty() && occ[0]->owner == me &&
                (int)(occ.size() + stack.size()) <= rules::MAX_STACK) {
              int sc = (int)occ.size() * 20 - move::distance(stack[0]->x, stack[0]->y, nx, ny);
              if (sc > bestRScore) {
                bestRScore = sc;
                bestRx = nx;
                bestRy = ny;
              }
            }
          }
        }
        if (bestRx != NONE) {
          destX = bestRx;
          destY = bestRy;
        }
      }
    }
  }

  // If Head 2 chose StageNear (waiting 1 turn to synchronize a multi-stack attack),
  // pick the safest visible walkable tile at distance 1-2 from the target, strongly preferring
  // a tile where friendly units are already staging so the stacks merge!
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
        if (!occ.empty() && (occ[0]->owner != me || (int)(occ.size() + stack.size()) > rules::MAX_STACK)) continue;
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

  // Walk step-by-step along the fog-respecting path
  int startX = sel.leader->x, startY = sel.leader->y;
  bool isFogMission = (tgt.kind == TargetKind::FOG_FRONTIER);
  bool madeProgress = false;
  for (int pass = 0; pass < 24 && !sel.armies.empty() && sel.minMoves > 0; pass++) {
    int lx = sel.leader->x, ly = sel.leader->y;
    if (isFogMission && sel.minMoves >= 1) {
      // Check if our exploration just uncovered a winnable neutral/enemy city or open site nearby
      float cBias = personality::forSide(side).traits[personality::CAUTION] * 0.5f;
      City* foundCity = nullptr;
      for (int dy = -4; dy <= 4 && !foundCity; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
          int nx = lx + dx, ny = ly + dy;
          if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height) continue;
          if (!belief::isTileVisible(g, me, nx, ny)) continue;
          City* nc = game::cityAt(g, nx, ny);
          if (nc && !nc->razed && nc->ownerIndex != me &&
              (nc->ownerIndex == NONE || diplomacy::mayAttack(g, me, nc->ownerIndex))) {
            auto est = belief::estimateCombat(g, side, sel.armies, nc->x, nc->y, cBias);
            if (est.winProb >= 0.45f) {
              foundCity = nc;
              break;
            }
          }
        }
      }
      if (foundCity) {
        destX = foundCity->x + (foundCity->x < lx ? 1 : 0);
        destY = foundCity->y + (foundCity->y < ly ? 1 : 0);
      } else if (lx == destX && ly == destY) {
        int bestNx = NONE, bestNy = NONE, bestScore = 0;
        for (int dy = -3; dy <= 3; dy++) {
          for (int dx = -3; dx <= 3; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = lx + dx, ny = ly + dy;
            if (nx < 0 || ny < 0 || nx >= g.map->width || ny >= g.map->height) continue;
            if (!belief::isTileVisible(g, me, nx, ny)) continue;
            int terr = g.map->terrainType[g.map->tiles[ny * g.map->width + nx] & 0xff];
            if (terr == move::WATER || terr == move::SHORE || terr == move::MOUNTAINS) continue;
            auto occ = game::armiesAt(g, nx, ny);
            if (!occ.empty() && occ[0]->owner != me) continue;
            int adjU = 0;
            for (int oy = -2; oy <= 2; oy++) {
              for (int ox = -2; ox <= 2; ox++) {
                int ux = nx + ox, uy = ny + oy;
                if (ux >= 0 && uy >= 0 && ux < g.map->width && uy < g.map->height &&
                    !belief::isTileVisible(g, me, ux, uy)) {
                  adjU++;
                }
              }
            }
            if (adjU > 0) {
              int sc = adjU * 3 + move::distance(startX, startY, nx, ny) * 2 -
                       move::distance(lx, ly, nx, ny);
              if (scn::roadAt(*g.map, nx, ny) % 0x20 != 0) sc += 15;
              if (sc > bestScore) {
                bestScore = sc;
                bestNx = nx;
                bestNy = ny;
              }
            }
          }
        }
        if (bestNx != NONE) {
          destX = bestNx;
          destY = bestNy;
        } else {
          break;
        }
      }
    } else if (lx == destX && ly == destY) {
      // If we merged onto a rally tile adjacent to our target enemy/neutral city, now aim into the city!
      if (!stageNear && tgt.city && tgt.city->ownerIndex != me && !tgt.city->razed &&
          std::abs(lx - tgt.city->x) <= 2 && std::abs(ly - tgt.city->y) <= 2) {
        destX = tgt.city->x + (tgt.city->x < lx ? 1 : 0);
        destY = tgt.city->y + (tgt.city->y < ly ? 1 : 0);
        if (lx == destX && ly == destY) break;
      } else {
        break;
      }
    }

    auto path = move::findPath(g, sel.armies, lx, ly, destX, destY);
    if (!path || path->empty()) break;

    std::vector<move::Step> walkSteps = *path;
    if (isFogMission && walkSteps.size() > 2) {
      walkSteps.resize(2);
    }
    move::WalkResult r = move::walk(g, sel.armies, walkSteps);
    if (r.steps > 0) {
      madeProgress = true;
      ai::walked(g, sel.armies, r);
    }
    core::refresh(g, sel);
    core::pickUp(g, sel);

    // Handle both "attack" and "at peace" (when we proposed war or met a city on the path)
    int ax = NONE, ay = NONE;
    City* attCity = nullptr;
    if (r.stopped == "attack" && r.attack && !stageNear) {
      ax = r.attack->x;
      ay = r.attack->y;
      attCity = r.attack->city;
    } else if (r.stopped == "at peace" && !stageNear && r.steps < (int)path->size()) {
      const move::Step& ahead = (*path)[r.steps];
      ax = ahead.x;
      ay = ahead.y;
      attCity = game::cityAt(g, ax, ay);
    }

    if (ax != NONE && ay != NONE) {
      auto defArmies = game::armiesAt(g, ax, ay);
      int defOwner = defArmies.empty() ? (attCity ? attCity->ownerIndex : NONE) : defArmies[0]->owner;

      bool allowed = diplomacy::mayAttack(g, me, defOwner);
      if (!allowed && defOwner != NONE &&
          (diplomacy::proposal(g, me, defOwner) != diplomacy::PEACE ||
           diplomacy::state(g, me, defOwner) == diplomacy::INTERMEDIATE ||
           (attCity && attCity == tgt.city))) {
        diplomacy::attackAnyway(g, me, defOwner);
        allowed = true;
      }
      if (!allowed) break;

      // Gather all friendly armies on our current tile that have movement left so converged
      // stacks fight together as a single combined force!
      std::vector<Army*> combinedHere = core::collect(g, me, sel.leader->x, sel.leader->y, 1);
      if (combinedHere.empty()) combinedHere = sel.armies;

      float caution = personality::forSide(side).traits[personality::CAUTION] * 0.5f;
      auto est = belief::estimateCombat(g, side, combinedHere, ax, ay, caution);
      float combStr = 0.0f;
      for (Army* a : combinedHere) combStr += (float)a->strength;
      bool moreAlliesComing = (tgt.nearbyFriendlyStr > combStr + 3.0f);
      if (est.winProb >= 0.45f || (!moreAlliesComing && est.winProb >= 0.30f)) {
        core::Sel combSel = core::select(g, combinedHere);
        int prevOwner = attCity ? attCity->ownerIndex : NONE;
        core::fight(g, combSel, ax, ay);
        madeProgress = true;
        belief::recordBattleObservation(g, me, ax, ay);
        if (attCity && attCity->ownerIndex == me && prevOwner != me && !combSel.armies.empty()) {
          decideCaptureSpoils(g, side, *attCity, combSel.armies, policy, ctx);
        }
        sel = combSel;
      } else {
        // Hold position on this rally tile adjacent to the target so converging friendly
        // stacks can merge onto our tile and attack together at full strength!
        madeProgress = true;
      }
      break;
    }
    if (r.steps == 0 || r.stopped == "out of moves" || r.stopped == "blocked" || r.stopped == "at peace") {
      break;
    }
  }

  // If the stack ended on a site with a hero (or a temple), interact with it
  if (!sel.armies.empty() && sel.leader && sel.minMoves > 0) {
    int fx = sel.leader->x, fy = sel.leader->y;
    Site* s = core::siteAt(g, fx, fy);
    if (s && core::siteOpen(*s)) {
      if (s->content == site::TEMPLE) {
        site::search(g, sel.armies, fx, fy, false);
        madeProgress = true;
        if (g.map->options.quests != 0 && sel.hero && (!side.quest || !side.quest->hasTarget())) {
          quest::assign(g, side, sel.hero, true, core::flies(g, sel.hero));
        }
      } else if (sel.hero && g.alive(sel.hero)) {
        site::search(g, sel.armies, fx, fy, false);
        madeProgress = true;
      }
    }
    core::pickUp(g, sel);
  }

  for (Army* a : sel.armies) {
    if (g.alive(a)) {
      a->aiMoved = madeProgress;
    }
  }
  return madeProgress;
}

void runMilitaryAndExplorationPhase(Game& g, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx,
                                    const std::function<void(const char*)>& pause) {
  int me = side.index;
  SideBelief& b = belief::ensure(side);
  float caution = personality::forSide(side).traits[personality::CAUTION] * 0.5f;

  // Build candidate targets strictly from visible fair-information state
  std::vector<CandidateTarget> targets;
  targets.reserve(64);

  for (City* c : b.visibleCities) {
    if (c->razed) continue;
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    bool isCapOrQuest = (side.capital == c) || (side.quest && side.quest->city == c);
    for (int s = 0; s < 8; s++) {
      if (g.map->sides[s].capital == c) isCapOrQuest = true;
    }

    if (c->ownerIndex == me) {
      float existingGarrisonStr = 0.0f;
      int existingCount = 0;
      for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
          for (Army* a : game::armiesAt(g, c->x + dx, c->y + dy)) {
            if (a->owner == me) {
              existingGarrisonStr += (float)a->strength;
              existingCount++;
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
      hold.isCapitalOrQuest = (side.capital == c);
      hold.enemyThreat = threat;
      hold.label = "Hold " + c->name;
      targets.push_back(std::move(hold));

      // Create a Reinforce / Forward-Rally target when the city faces an active enemy threat
      // or is a frontline city with room for reinforcements from deep rear cities
      float cDistE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 60.0f;
      if ((threat > existingGarrisonStr || cDistE <= 22.0f) && existingCount < 16) {
        CandidateTarget reinf;
        reinf.kind = TargetKind::REINFORCE_CITY;
        reinf.x = c->x;
        reinf.y = c->y;
        reinf.city = c;
        reinf.owner = me;
        reinf.value = (float)c->income + threat * 0.6f + std::max(0.0f, 24.0f - cDistE);
        reinf.isCapitalOrQuest = (side.capital == c);
        reinf.enemyThreat = std::max(threat, 12.0f);
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
      neut.isCapitalOrQuest = isCapOrQuest;
      neut.enemyThreat = threat;
      neut.label = "Capture Neutral " + c->name;
      targets.push_back(std::move(neut));
    } else if (diplomacy::mayAttack(g, me, c->ownerIndex) ||
               diplomacy::proposal(g, me, c->ownerIndex) != diplomacy::PEACE) {
      CandidateTarget enem;
      enem.kind = TargetKind::ENEMY_CITY;
      enem.x = c->x;
      enem.y = c->y;
      enem.city = c;
      enem.owner = c->ownerIndex;
      enem.value = (float)c->income + (float)c->defence * 6.0f;
      if (diplomacy::alliedTargetCity(g, c->ownerIndex) == c->index) enem.value += 10.0f;
      enem.isCapitalOrQuest = isCapOrQuest;
      enem.enemyThreat = threat;
      enem.label = "Siege " + c->name;
      targets.push_back(std::move(enem));
    }
  }

  for (const auto& [tileKey, obs] : b.stacksByTile) {
    if (obs.owner == me || obs.owner == NONE) continue;
    if (game::cityAt(g, obs.x, obs.y)) continue;
    if (!diplomacy::mayAttack(g, me, obs.owner)) continue;
    CandidateTarget st;
    st.kind = TargetKind::ENEMY_STACK;
    st.x = obs.x;
    st.y = obs.y;
    st.owner = obs.owner;
    st.value = (float)obs.visibleCount * 3.5f;
    st.isCapitalOrQuest = (side.quest && side.quest->side && side.quest->side->index == obs.owner);
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
    gt.isCapitalOrQuest = (side.quest && side.quest->item == it);
    gt.label = "Pick up " + it->name;
    targets.push_back(std::move(gt));
  }

  for (const auto& fc : b.fogFrontiers) {
    CandidateTarget ft;
    ft.kind = TargetKind::FOG_FRONTIER;
    ft.x = fc.x;
    ft.y = fc.y;
    ft.value = (float)fc.regionUnseenCount * 1.8f;
    ft.label = fmt("Explore Fog (%d,%d)", fc.x, fc.y);
    targets.push_back(std::move(ft));
  }

  if (targets.empty()) return;

  // Precompute friendly mobile strength within 14 tiles of each combat target so separate
  // stacks in the field/nearby cities see their combined siege potential and converge!
  int visibleCombatTargets = 0;
  for (auto& t : targets) {
    if (t.kind == TargetKind::NEUTRAL_CITY || t.kind == TargetKind::ENEMY_CITY ||
        t.kind == TargetKind::ENEMY_STACK) {
      visibleCombatTargets++;
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
  int fogMissionsLaunched = 0;

  // Group our placed armies by location (either city index or tile key)
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

  // Target-First Marginal Coalition Assembly at each location:
  for (auto& [locKey, lg] : groupsByLoc) {
    std::vector<Army*> pool = lg.units;
    // Sort pool so Heroes, fliers, and strong/fast assault units evaluate mission seeds first,
    // leaving the slowest non-flying infantry at the back of `pool` to serve as city guard
    std::sort(pool.begin(), pool.end(), [&](Army* a, Army* b2) {
      if (a->hero() != b2->hero()) return a->hero() > b2->hero();
      bool af = core::flies(g, a), bf = core::flies(g, b2);
      if (af != bf) return af > bf;
      int sa = a->strength * 2 + a->maxMoves;
      int sb = b2->strength * 2 + b2->maxMoves;
      if (sa != sb) return sa > sb;
      return a->moves > b2->moves;
    });

    // Compute fair-information flood reachability from this location for land and air
    core::Flood landFlood = core::floodFrom(g, me, lg.x, lg.y, 72, move::LAND, me);
    core::Flood flyFlood = core::floodFrom(g, me, lg.x, lg.y, 72, move::FLYING, me);

    // Ensure reachable fog frontiers exist for land units at this location
    if (g.map->options.hiddenMap != 0) {
      int reachableFogCount = 0;
      for (const auto& t : targets) {
        if (t.kind == TargetKind::FOG_FRONTIER && t.assignedStacks == 0) {
          int fc = core::floodAt(landFlood, t.x, t.y);
          int dTile = std::max(1, move::distance(lg.x, lg.y, t.x, t.y));
          if (fc < core::UNREACHED && fc <= dTile * 4 + 20) {
            reachableFogCount++;
          }
        }
      }
      if (reachableFogCount < 6) {
        int W = g.map->width, H = g.map->height;
        for (int r = 1; r <= 32 && reachableFogCount < 8; r++) {
          for (int dy = -r; dy <= r && reachableFogCount < 8; dy++) {
            for (int dx = -r; dx <= r && reachableFogCount < 8; dx++) {
              if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
              int fx = lg.x + dx, fy = lg.y + dy;
              if (fx < 0 || fy < 0 || fx >= W || fy >= H) continue;
              int fterr = g.map->terrainType[g.map->tiles[fy * W + fx] & 0xff];
              if (fterr == move::WATER || fterr == move::SHORE || fterr == move::MOUNTAINS) continue;
              int fc = core::floodAt(landFlood, fx, fy);
              int dTile = std::max(1, move::distance(lg.x, lg.y, fx, fy));
              if (fc >= core::UNREACHED || fc > dTile * 4 + 20) continue;
              int adjU = 0;
              for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                  int ux = fx + ox, uy = fy + oy;
                  if (ux >= 0 && uy >= 0 && ux < W && uy < H && !belief::isTileVisible(g, me, ux, uy)) {
                    adjU++;
                  }
                }
              }
              if (adjU >= 1) {
                bool dup = false;
                for (const auto& t : targets) {
                  if (t.kind == TargetKind::FOG_FRONTIER && move::distance(fx, fy, t.x, t.y) < 3) {
                    dup = true;
                    break;
                  }
                }
                if (!dup) {
                  CandidateTarget ft;
                  ft.kind = TargetKind::FOG_FRONTIER;
                  ft.x = fx;
                  ft.y = fy;
                  ft.value = (float)(adjU + 4) * 1.8f;
                  ft.label = fmt("Explore Fog (%d,%d)", fx, fy);
                  targets.push_back(std::move(ft));
                  reachableFogCount++;
                }
              }
            }
          }
        }
      }
    }

    std::set<size_t> blockedTargets;
    float lgDistE = (lg.city && lg.city->index < (int)b.cityNearestEnemyDist.size())
                        ? b.cityNearestEnemyDist[lg.city->index]
                        : 30.0f;

    while (!pool.empty()) {
      Army* seed = pool.front();
      pool.erase(pool.begin());
      if (!g.alive(seed) || seed->moves <= 0 || seed->aiMoved) continue;

      std::vector<Army*> currentStack = {seed};
      int bestTgtIdx = NONE;
      ml::MissionOutput bestOut{-1e9f, 0.0f};
      bool seedFlies = (move::modeOf(g, currentStack) == move::FLYING);
      const core::Flood& activeFlood = seedFlies ? flyFlood : landFlood;

      float remGroupStr = (float)seed->strength;
      for (Army* rem : pool) remGroupStr += (float)rem->strength;

      // Score candidate targets for the seed unit (considering whether compatible escorts exist)
      ml::StackVec seedSv = buildStackFeatures(g, currentStack, seed->moves, false);
      ml::StackVec seedExploreSv = buildStackFeatures(g, currentStack, seed->moves, true);
      for (size_t ti = 0; ti < targets.size(); ti++) {
        if (blockedTargets.count(ti)) continue;
        const CandidateTarget& tgt = targets[ti];

        // Skip targets already captured, searched, or uncovered earlier in this turn
        if ((tgt.kind == TargetKind::NEUTRAL_CITY || tgt.kind == TargetKind::ENEMY_CITY) &&
            tgt.city && (tgt.city->ownerIndex == me || tgt.city->razed)) {
          continue;
        }
        if (tgt.kind == TargetKind::ENEMY_STACK) {
          auto curAt = game::armiesAt(g, tgt.x, tgt.y);
          if (curAt.empty() || curAt[0]->owner == me) continue;
        }
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && tgt.site && !core::siteOpen(*tgt.site)) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.item &&
            (tgt.item->status != 1 || tgt.item->x == NONE)) {
          continue;
        }
        if (tgt.kind == TargetKind::FOG_FRONTIER && g.map->options.hiddenMap != 0) {
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
        if (tgt.kind == TargetKind::REINFORCE_CITY) {
          if (tgt.city == lg.city) continue;
          if (lg.city && lg.city->index < (int)b.cityThreat2Turn.size() && b.cityThreat2Turn[lg.city->index] > 0.0f) continue;
          float tgtDistE = (tgt.city && tgt.city->index < (int)b.cityNearestEnemyDist.size())
                               ? b.cityNearestEnemyDist[tgt.city->index]
                               : 30.0f;
          // Only reinforce forward toward the enemy (never laterally/backward between cities!)
          if (tgtDistE + 10.0f >= lgDistE && tgt.enemyThreat <= 12.0f) continue;
        }
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && !seed->hero()) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.item && !seed->hero()) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.site && !seed->blessings.empty() && !seed->hero()) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && tgt.assignedStacks >= 1) continue;
        if (tgt.kind == TargetKind::TEMPLE_OR_ITEM && !seed->hero() &&
            move::distance(seed->x, seed->y, tgt.x, tgt.y) > seed->moves) continue;
        if (tgt.kind == TargetKind::RUIN_OR_SAGE && tgt.assignedStacks >= 1) continue;

        int pCost = targetPathCost(g, activeFlood, tgt);
        if (pCost >= core::UNREACHED) continue;
        // Land units should not board 4-gold/turn boats (WATER_PENALTY = 30) just to explore fog
        if (tgt.kind == TargetKind::FOG_FRONTIER && !seedFlies && !seed->atSea) {
          int dTile = std::max(1, move::distance(seed->x, seed->y, tgt.x, tgt.y));
          if (pCost > dTile * 4 + 20) continue;
        }

        bool isExplore = (tgt.kind == TargetKind::FOG_FRONTIER || tgt.kind == TargetKind::RUIN_OR_SAGE ||
                          tgt.kind == TargetKind::TEMPLE_OR_ITEM);
        // Preview potential stack with local escorts for combat targets so a single unit
        // in a large city accurately sees the utility of forming an assault stack
        std::vector<Army*> previewStack = currentStack;
        if (tgt.kind == TargetKind::NEUTRAL_CITY || tgt.kind == TargetKind::ENEMY_CITY ||
            tgt.kind == TargetKind::ENEMY_STACK ||
            (tgt.kind == TargetKind::FOG_FRONTIER && !seedFlies)) {
          size_t maxPrev = (tgt.kind == TargetKind::FOG_FRONTIER) ? 3 : 8;
          for (Army* cand : pool) {
            if (previewStack.size() >= maxPrev) break;
            if (seedFlies && !core::flies(g, cand) && targetPathCost(g, landFlood, tgt) >= core::UNREACHED) continue;
            previewStack.push_back(cand);
          }
        }
        float nearStr = 0.0f;
        if (tgt.kind == TargetKind::HOLD_GARRISON) {
          for (Army* rem : pool) nearStr += (float)rem->strength;
        } else if (tgt.kind == TargetKind::NEUTRAL_CITY || tgt.kind == TargetKind::ENEMY_CITY ||
                   tgt.kind == TargetKind::ENEMY_STACK) {
          nearStr = std::max(0.0f, tgt.nearbyFriendlyStr - remGroupStr);
        }
        ml::StackVec sv = previewStack.size() > 1
                              ? buildStackFeatures(g, previewStack, seed->moves, false)
                              : (isExplore ? seedExploreSv : seedSv);
        ml::TargetVec tv = buildTargetFeatures(g, side, previewStack, tgt, nearStr, caution, pCost);
        if (tgt.kind == TargetKind::FOG_FRONTIER && visibleCombatTargets > 0) {
          tv[16] += (float)fogMissionsLaunched * 0.45f;
        }
        ml::MissionOutput out = policy.scoreMission(ctx, sv, tv);
        if (out.utility > bestOut.utility) {
          bestOut = out;
          bestTgtIdx = (int)ti;
        }
      }

      if (bestTgtIdx == NONE) {
        seed->aiMoved = true;
        continue;
      }

      CandidateTarget& chosenTgt = targets[bestTgtIdx];
      if (chosenTgt.kind == TargetKind::HOLD_GARRISON) {
        seed->aiMoved = true;
        chosenTgt.assignedStacks++;
        chosenTgt.assignedStrength += (float)seed->strength;
        continue;
      }

      // Neural Coalition Size Selection: evaluate prefix coalitions {seed, cand_1, ..., cand_k}
      // up to MAX_STACK (8) and pick the coalition size k* that maximizes neural Mission Utility
      // (avoiding any non-convex 1->2 unit valley on fortified cities while keeping single units for scouting)
      bool chosenExplore = (chosenTgt.kind == TargetKind::FOG_FRONTIER ||
                            chosenTgt.kind == TargetKind::RUIN_OR_SAGE ||
                            chosenTgt.kind == TargetKind::TEMPLE_OR_ITEM);
      int chosenCost = targetPathCost(g, activeFlood, chosenTgt);
      float externalFriendlyStr = std::max(0.0f, chosenTgt.nearbyFriendlyStr - remGroupStr);
      ml::TargetVec curTv = buildTargetFeatures(g, side, currentStack, chosenTgt, externalFriendlyStr, caution, chosenCost);
      ml::MissionOutput curOut = policy.scoreMission(ctx, chosenExplore ? seedExploreSv : seedSv, curTv);

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

      float totalPoolStr = 0.0f;
      for (Army* rem : pool) totalPoolStr += (float)rem->strength;
      float committedFromPoolStr = 0.0f;

      std::vector<Army*> trialStack = currentStack;
      std::vector<Army*> bestStack = currentStack;
      for (Army* cand : eligibleEscorts) {
        int prevMinMove = move::stackMoves(trialStack);
        trialStack.push_back(cand);
        bool trialFlies = (move::modeOf(g, trialStack) == move::FLYING);
        int trialCost = targetPathCost(g, trialFlies ? flyFlood : landFlood, chosenTgt);
        if (trialCost >= core::UNREACHED) {
          trialStack.pop_back();
          continue;
        }
        committedFromPoolStr += (float)cand->strength;
        float remAfterCandLeaves = std::max(0.0f, totalPoolStr - committedFromPoolStr);

        // Compute cand's HoldGarrison utility if in an owned city
        float holdAlt = -1e9f;
        if (lg.city && lg.city->ownerIndex == me) {
          for (const auto& t : targets) {
            if (t.kind == TargetKind::HOLD_GARRISON && t.city == lg.city) {
              std::vector<Army*> solo = {cand};
              ml::StackVec soloSv = buildStackFeatures(g, solo, cand->moves, false);
              ml::TargetVec holdTv = buildTargetFeatures(g, side, solo, t, remAfterCandLeaves, caution, 0);
              holdAlt = policy.scoreMission(ctx, soloSv, holdTv).utility;
              break;
            }
          }
        }
        ml::StackVec trialSv = buildStackFeatures(g, trialStack, prevMinMove, chosenExplore);
        ml::TargetVec trialTv = buildTargetFeatures(g, side, trialStack, chosenTgt, externalFriendlyStr, caution, trialCost);
        ml::MissionOutput trialOut = policy.scoreMission(ctx, trialSv, trialTv);

        if (trialOut.utility > curOut.utility && holdAlt < trialOut.utility) {
          bestStack = trialStack;
          curOut = trialOut;
          curTv = trialTv;
        }
      }
      currentStack = bestStack;
      for (size_t i = 1; i < currentStack.size(); i++) {
        auto it = std::find(pool.begin(), pool.end(), currentStack[i]);
        if (it != pool.end()) pool.erase(it);
      }

      float stackStr = 0.0f;
      for (Army* a : currentStack) stackStr += (float)a->strength;
      chosenTgt.assignedStacks++;
      chosenTgt.assignedStrength += stackStr;
      if (chosenTgt.kind == TargetKind::FOG_FRONTIER) fogMissionsLaunched++;

      if (chosenTgt.kind == TargetKind::ENEMY_CITY && chosenTgt.city &&
          curOut.utility > bestEnemyCityScore) {
        bestEnemyCityScore = curOut.utility;
        bestEnemyCityTarget = chosenTgt.city->index;
      }

      bool stageNear = (curOut.stageNearLogit > 0.0f) &&
                       (chosenTgt.kind == TargetKind::ENEMY_CITY || chosenTgt.kind == TargetKind::ENEMY_STACK) &&
                       curTv[9] < 0.65f && curTv[10] > curTv[9] + 0.10f;

      bool ok = executeStackMission(g, side, currentStack, chosenTgt, stageNear, policy, ctx);
      if (!ok) {
        blockedTargets.insert((size_t)bestTgtIdx);
        chosenTgt.assignedStacks = std::max(0, chosenTgt.assignedStacks - 1);
        chosenTgt.assignedStrength = std::max(0.0f, chosenTgt.assignedStrength - stackStr);
        for (Army* a : currentStack) {
          if (g.alive(a) && a->moves > 0) a->aiMoved = true;
        }
      } else {
        side.aiThoughts.push_back(AIThought{
            "Mission",
            fmt("%d-unit stack (%s)", (int)currentStack.size(), currentStack[0]->name.c_str()),
            chosenTgt.label,
            currentStack[0]->x, currentStack[0]->y,
            chosenTgt.x, chosenTgt.y,
            curOut.utility,
            curTv[9],
            stageNear,
            fmt("U=%.2f Win=%.0f%% Turns=%.1f %s", curOut.utility, curTv[9] * 100.0f,
                curTv[8] * 8.0f, stageNear ? "[STAGE FOR SYNC]" : "[EXECUTE]")});
        // Add any newly revealed neutral or enemy cities to targets so remaining stacks can act on them
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
            if (c.ownerIndex == NONE) {
              CandidateTarget neut;
              neut.kind = TargetKind::NEUTRAL_CITY;
              neut.x = c.x;
              neut.y = c.y;
              neut.city = &c;
              neut.owner = NONE;
              neut.value = (float)c.income + (float)c.slots.size() * 5.0f;
              neut.label = "Capture Neutral " + c.name;
              targets.push_back(std::move(neut));
            } else if (diplomacy::mayAttack(g, me, c.ownerIndex) ||
                       diplomacy::proposal(g, me, c.ownerIndex) != diplomacy::PEACE) {
              CandidateTarget enem;
              enem.kind = TargetKind::ENEMY_CITY;
              enem.x = c.x;
              enem.y = c.y;
              enem.city = &c;
              enem.owner = c.ownerIndex;
              enem.value = (float)c.income + (float)c.defence * 6.0f;
              enem.label = "Siege " + c.name;
              targets.push_back(std::move(enem));
            }
          }
        }
      }
    }
  }

  // Broadcast our primary enemy city target to Allied partners
  diplomacy::setAlliedTargetCity(g, me, bestEnemyCityTarget);
  if (pause) pause("missions");
}

void runEconomyPhase(Game& g, Side& side, const ml::PolicyNetwork& policy, const ml::ContextVec& ctx) {
  int me = side.index;
  const SideBelief& b = belief::ensure(side);

  auto makeHoldGoldVec = [&]() {
    ml::EconVec e{};
    e[0] = 1.0f;  // isHoldTreasury
    e[10] = std::tanh((float)(game::upkeep(g, side) - game::income(g, side)) / 40.0f);
    return e;
  };
  float uHoldGold = policy.scoreEconomy(ctx, makeHoldGoldVec());

  // 1. Learnable Army Retention vs Disbanding:
  // Compare each non-hero placed army's RetainArmy score against uHoldGold
  std::vector<Army*> myArmies = game::sideArmies(g, side);
  if (myArmies.size() > 3) {
    struct RetentionCand {
      Army* army = nullptr;
      float score = 0.0f;
    };
    std::vector<RetentionCand> cands;
    for (Army* a : myArmies) {
      if (!g.alive(a) || a->hero() || a->transit || a->x == NONE) continue;
      City* c = game::cityAt(g, a->x, a->y);
      // Only consider disbanding surplus garrison armies sitting inside rear cities,
      // never active field armies or the last defender of a city
      if (!c || c->ownerIndex != me) continue;
      int cityArmies = 0;
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
          cityArmies += (int)game::armiesAt(g, c->x + dx, c->y + dy).size();
      if (cityArmies <= 2) continue;

      const ArmyType* at = g.types.byId(a->type);
      float threat = (c->index < (int)b.cityThreat2Turn.size()) ? b.cityThreat2Turn[c->index] : 0.0f;
      float distE = (c->index < (int)b.cityNearestEnemyDist.size()) ? b.cityNearestEnemyDist[c->index] : 30.0f;

      ml::EconVec e{};
      e[5] = 1.0f;  // isRetainArmy
      e[7] = (float)a->strength / 9.0f;
      e[8] = (float)a->maxMoves / 24.0f;
      e[10] = (float)a->upkeep / 15.0f;
      e[11] = (at && at->flies) ? 1.0f : 0.0f;
      e[12] = (at && (at->siege || at->bonus[48] != 0)) ? 1.0f : 0.0f;
      e[13] = (a->maxMoves >= 14 && (!at || !at->flies)) ? 1.0f : 0.0f;
      e[14] = std::min(1.5f, threat / 35.0f);
      e[15] = std::min(1.5f, distE / 60.0f);

      float uKeep = policy.scoreEconomy(ctx, e);
      if (uKeep < uHoldGold) {
        cands.push_back(RetentionCand{a, uKeep});
      }
    }
    std::sort(cands.begin(), cands.end(), [](const RetentionCand& x, const RetentionCand& y) {
      return x.score < y.score;
    });
    for (const auto& rc : cands) {
      ml::ContextVec updatedCtx = ml::buildContext(g, side);
      uHoldGold = policy.scoreEconomy(updatedCtx, makeHoldGoldVec());
      if (rc.score < uHoldGold) {
        game::disband(g, side, {rc.army});
      } else {
        break;
      }
    }
  }

  // 2. Learnable City Production & Stop Decision (run before slot buying so working production
  // reserve is preserved):
  std::vector<City*> ownCities = game::sideCities(g, side);
  const auto& prof = personality::forSide(side);
  for (City* c : ownCities) {
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    float distE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 30.0f;
    int garCount = 0;
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++)
        garCount += (int)game::armiesAt(g, c->x + dx, c->y + dy).size();

    // Score StopProduction option
    ml::EconVec stopVec{};
    stopVec[2] = 1.0f;  // isStopProduction
    stopVec[10] = std::tanh((float)(game::upkeep(g, side) - game::income(g, side)) / 30.0f);
    stopVec[14] = std::min(1.5f, threat / 35.0f);
    stopVec[15] = std::min(1.5f, distE / 60.0f);
    stopVec[16] = std::min(2.0f, (float)garCount / 8.0f);
    float bestSlotScore = policy.scoreEconomy(ctx, stopVec);
    int bestSlot = NONE;

    for (size_t si = 0; si < c->slots.size(); si++) {
      const Slot& sl = c->slots[si];
      const ArmyType* at = g.types.byId(sl.type);
      bool isFlier = at && at->flies;
      bool isSiegeOrSpec = at && (at->siege || at->bonus[48] != 0 || at->bonus[52] != 0);
      bool isCav = sl.move >= 14 && !isFlier;

      // Actual remaining turns to complete this slot (preserves progress on mid-build units!)
      int turnsLeft = (c->producing == (int)si && c->countdown > 0) ? c->countdown : sl.time;

      ml::EconVec e{};
      e[1] = 1.0f;  // isProduceSlot
      e[7] = (float)sl.strength / 9.0f;
      e[8] = (float)sl.move / 24.0f;
      e[9] = (float)turnsLeft / 6.0f;
      e[10] = (float)sl.cost / 20.0f;
      e[11] = isFlier ? 1.0f : 0.0f;
      e[12] = isSiegeOrSpec ? 1.0f : 0.0f;
      e[13] = isCav ? 1.0f : 0.0f;
      e[14] = std::min(1.5f, threat / 35.0f);
      e[15] = std::min(1.5f, distE / 60.0f);
      e[16] = std::min(2.0f, (float)garCount / 8.0f);

      float sc = policy.scoreEconomy(ctx, e);
      // Apply personality unit-class affinities learned via context
      if (isCav) sc += 0.35f * prof.traits[personality::CAVALRY_AFFINITY];
      if (isFlier) sc += 0.35f * prof.traits[personality::FLIER_AFFINITY];
      if (isSiegeOrSpec) sc += 0.35f * prof.traits[personality::SIEGE_AFFINITY];

      if (sc > bestSlotScore) {
        bestSlotScore = sc;
        bestSlot = (int)si;
      }
    }

    if (bestSlot == NONE) {
      game::setProduction(g, *c, NONE);
    } else if (c->producing != bestSlot || !game::producing(*c)) {
      game::setCityProduction(g, &side, *c, bestSlot);
    }
  }

  // 3. Learnable Production Slot Buying (Rebuilding with surplus treasury above working reserve):
  if (!ownCities.empty() && side.gold >= 250) {
    City* bestBuyCity = nullptr;
    int bestBuyTypeId = NONE;
    float bestBuyScore = uHoldGold;
    auto menu = game::buyableTypes(g);

    for (City* c : ownCities) {
      if ((int)c->slots.size() >= rules::PRODUCTION_SLOTS) continue;
      float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
      float distE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 30.0f;
      for (const ArmyType* at : menu) {
        if (!game::cannotBuy(g, side, *c, *at).empty()) continue;
        int price = std::abs(at->price);
        if (price <= 0 || price + 100 > side.gold) continue;
        ml::EconVec e{};
        e[3] = 1.0f;  // isBuyProductionSlot
        e[7] = (float)at->strength / 9.0f;
        e[8] = (float)at->move / 24.0f;
        e[9] = (float)at->time / 6.0f;
        e[10] = (float)price / std::max(100.0f, (float)(side.gold - 80));
        e[11] = at->flies ? 1.0f : 0.0f;
        e[12] = (at->siege || at->bonus[48] != 0) ? 1.0f : 0.0f;
        e[13] = (at->move >= 14 && !at->flies) ? 1.0f : 0.0f;
        e[14] = std::min(1.5f, threat / 35.0f);
        e[15] = std::min(1.5f, distE / 60.0f);
        float sc = policy.scoreEconomy(ctx, e);
        if (sc > bestBuyScore) {
          bestBuyScore = sc;
          bestBuyCity = c;
          bestBuyTypeId = at->id;
        }
      }
    }
    if (bestBuyCity && bestBuyTypeId != NONE) {
      int slotIdx = game::buySlot(*bestBuyCity);
      game::buyProduction(g, side, *bestBuyCity, slotIdx, bestBuyTypeId);
      game::sortProduction(*bestBuyCity);
      if (!game::producing(*bestBuyCity) && !bestBuyCity->slots.empty()) {
        game::setCityProduction(g, &side, *bestBuyCity, (int)bestBuyCity->slots.size() - 1);
      }
    }
  }

  // 4. Learnable City Vectoring:
  for (City* c : ownCities) c->vectorTo = NONE;
  std::map<int, int> vecCount;
  for (City* src : ownCities) {
    if (!game::producing(*src) || src->producing < 0 || src->producing >= (int)src->slots.size()) continue;
    const Slot& sl = src->slots[src->producing];
    float srcThreat = src->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[src->index] : 0.0f;
    float srcDistE = src->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[src->index] : 30.0f;

    // Baseline: keep production at `src` (NoVector)
    ml::EconVec keepHere{};
    keepHere[4] = 1.0f;
    keepHere[14] = std::min(1.5f, srcThreat / 35.0f);
    keepHere[15] = std::min(1.5f, srcDistE / 60.0f);
    keepHere[17] = 0.0f;
    float bestVecScore = policy.scoreEconomy(ctx, keepHere);
    City* bestDest = nullptr;

    for (City* dst : ownCities) {
      if (dst == src || vecCount[dst->index] >= game::MAX_VECTORED_TO) continue;
      int vTurns = std::max(1, move::distance(src->x, src->y, dst->x, dst->y) / 14 + 1);
      if (vTurns > 4) continue;
      float dstThreat = dst->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[dst->index] : 0.0f;
      float dstDistE = dst->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[dst->index] : 30.0f;
      float threatGain = (dstThreat - srcThreat) / 25.0f + (srcDistE - dstDistE) / 35.0f;

      ml::EconVec v{};
      v[4] = 1.0f;  // isVectorRoute
      v[7] = (float)sl.strength / 9.0f;
      v[8] = (float)sl.move / 24.0f;
      v[9] = (float)vTurns / 4.0f;
      v[14] = std::min(1.5f, dstThreat / 35.0f);
      v[15] = std::min(1.5f, dstDistE / 60.0f);
      v[17] = threatGain;

      float sc = policy.scoreEconomy(ctx, v);
      if (sc > bestVecScore) {
        bestVecScore = sc;
        bestDest = dst;
      }
    }
    if (bestDest) {
      game::vector(g, *src, bestDest);
      vecCount[bestDest->index]++;
      side.aiThoughts.push_back(AIThought{
          "Economy", src->name, bestDest->name, src->x, src->y, bestDest->x, bestDest->y,
          bestVecScore, 1.0f, false,
          fmt("Vectoring %s from %s -> %s", sl.name.c_str(), src->name.c_str(), bestDest->name.c_str())});
    }
  }
}

}  // namespace

void playTurn(Game& g, Side& side, const std::function<void(const char*)>& pause,
              const ml::PolicyNetwork* customPolicy) {
  const ml::PolicyNetwork& policy = customPolicy ? *customPolicy : ml::activePolicy();
  for (Army* a : g.armies) {
    if (a->owner == side.index) a->aiMoved = false;
  }
  side.aiThoughts.clear();

  for (int wave = 0; wave < 3; wave++) {
    belief::update(g, side);
    ml::ContextVec waveCtx = ml::buildContext(g, side);
    if (wave == 0) {
      side.lastBoardValue = policy.evaluateValue(waveCtx);
      runDiplomacyPhase(g, side, policy, waveCtx);
      if (pause) pause("diplomacy");
    }
    runMilitaryAndExplorationPhase(g, side, policy, waveCtx, wave == 0 ? pause : nullptr);
    bool anyLeft = false;
    for (Army* a : g.armies) {
      if (a->owner == side.index && a->x != NONE && !a->transit && !a->aiMoved && a->moves >= 2) {
        anyLeft = true;
        break;
      }
    }
    if (!anyLeft) break;
  }

  // Refresh belief after military moves & captures before running economy
  belief::update(g, side);
  ml::ContextVec ctx = ml::buildContext(g, side);
  side.lastBoardValue = policy.evaluateValue(ctx);

  runEconomyPhase(g, side, policy, ctx);
  if (pause) pause("economy");
}

City* selectHeroCity(Game& g, Side& side, City* dflt, const ml::PolicyNetwork* customPolicy) {
  const ml::PolicyNetwork& policy = customPolicy ? *customPolicy : ml::activePolicy();
  belief::update(g, side);
  const SideBelief& b = belief::ensure(side);
  ml::ContextVec ctx = ml::buildContext(g, side);

  City* best = dflt;
  float bestScore = -1e9f;
  for (City* c : game::sideCities(g, side)) {
    float threat = c->index < (int)b.cityThreat2Turn.size() ? b.cityThreat2Turn[c->index] : 0.0f;
    float distE = c->index < (int)b.cityNearestEnemyDist.size() ? b.cityNearestEnemyDist[c->index] : 30.0f;
    ml::EconVec e{};
    e[1] = 1.0f;
    e[7] = 0.8f;
    e[8] = 0.7f;
    e[14] = std::min(1.5f, threat / 35.0f);
    e[15] = std::min(1.5f, distE / 60.0f);
    float sc = policy.scoreEconomy(ctx, e);
    if (sc > bestScore) {
      bestScore = sc;
      best = c;
    }
  }
  return best;
}

}  // namespace w2::ai::modern
