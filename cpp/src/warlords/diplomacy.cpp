#include "warlords/diplomacy.hpp"

#include <algorithm>
#include <cmath>

#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"

namespace w2::diplomacy {

const char* const STATE_NAMES[3] = {"peace", "uneasy", "war"};
const char* const TITLES[8] = {"Statesman", "Diplomat", "Pragmatist", "Politician",
                               "Deceiver", "Scoundrel", "Turncoat", "Running Dog"};

const std::vector<int>& ratingRanks(int n) {
  static const std::vector<int> ranks[9] = {
      {}, {1}, {1, 8}, {1, 4, 8}, {1, 2, 6, 8}, {1, 2, 4, 6, 8},
      {1, 2, 4, 6, 7, 8}, {1, 2, 4, 5, 6, 7, 8}, {1, 2, 3, 4, 5, 6, 7, 8}};
  if (n < 1 || n > 8) return ranks[8];
  return ranks[n];
}

static int key(int a, int b) { return a * 8 + b; }
static bool validPair(int a, int b) { return a >= 0 && b >= 0 && a < 8 && b < 8 && a != b; }

void init(Game& g) {
  int start = g.map->options.diplomacy != 0 ? PEACE : WAR;
  g.diplomacy.allied.fill(false);
  g.diplomacy.proposeAllied.fill(false);
  g.diplomacy.truceTurns.fill(0);
  g.diplomacy.goldOffer.fill(0);
  g.diplomacy.warTarget.fill(NONE);
  g.diplomacy.trust.fill(0);
  g.diplomacy.alliedTargetCity.fill(NONE);
  for (int a = 0; a < 8; a++) {
    for (int b = 0; b < 8; b++) {
      int v = start;
      g.diplomacy.state[key(a, b)] = v;
      g.diplomacy.proposal[key(a, b)] = v;
    }
  }
}

int state(const Game& g, int a, int b) {
  if (a < 0 || b < 0 || a > 7 || b > 7) return WAR;
  int v = g.diplomacy.state[key(a, b)];
  return v == NONE ? WAR : v;
}

bool mayAttack(const Game& g, int a, int b) {
  if (a == NONE || b == NONE) return true;        // neutrals are fair game
  return state(g, a, b) != PEACE;
}

void propose(Game& g, int a, int b, int st) {
  if (!validPair(a, b)) return;
  g.diplomacy.proposal[key(a, b)] = st;
  if (st != PEACE) {
    g.diplomacy.proposeAllied[key(a, b)] = false;
  }
}

int proposal(const Game& g, int a, int b) {
  if (a < 0 || b < 0 || a > 7 || b > 7) return state(g, a, b);
  int p = g.diplomacy.proposal[key(a, b)];
  if (p == NONE) p = state(g, a, b);
  return p;
}

bool isAllied(const Game& g, int a, int b) {
  if (!validPair(a, b)) return false;
  return state(g, a, b) == PEACE && g.diplomacy.allied[key(a, b)];
}

void setAllied(Game& g, int a, int b, bool v) {
  if (!validPair(a, b)) return;
  if (v) {
    g.diplomacy.state[key(a, b)] = PEACE;
    g.diplomacy.state[key(b, a)] = PEACE;
    g.diplomacy.proposal[key(a, b)] = PEACE;
    g.diplomacy.proposal[key(b, a)] = PEACE;
  }
  g.diplomacy.allied[key(a, b)] = v;
  g.diplomacy.allied[key(b, a)] = v;
  g.diplomacy.proposeAllied[key(a, b)] = v;
  g.diplomacy.proposeAllied[key(b, a)] = v;
}

bool proposalAllied(const Game& g, int a, int b) {
  if (!validPair(a, b)) return false;
  return g.diplomacy.proposeAllied[key(a, b)];
}

void proposeAlliance(Game& g, int a, int b, bool v) {
  if (!validPair(a, b)) return;
  g.diplomacy.proposeAllied[key(a, b)] = v;
  if (v) g.diplomacy.proposal[key(a, b)] = PEACE;
}

int truceTurns(const Game& g, int a, int b) {
  if (!validPair(a, b)) return 0;
  return g.diplomacy.truceTurns[key(a, b)];
}

void setTruce(Game& g, int a, int b, int turns) {
  if (!validPair(a, b)) return;
  int t = std::max(0, turns);
  g.diplomacy.truceTurns[key(a, b)] = t;
  g.diplomacy.truceTurns[key(b, a)] = t;
}

int goldOffer(const Game& g, int a, int b) {
  if (!validPair(a, b)) return 0;
  return g.diplomacy.goldOffer[key(a, b)];
}

void setGoldOffer(Game& g, int a, int b, int gold) {
  if (!validPair(a, b)) return;
  g.diplomacy.goldOffer[key(a, b)] = gold;
}

int warTarget(const Game& g, int a, int b) {
  if (!validPair(a, b)) return NONE;
  return g.diplomacy.warTarget[key(a, b)];
}

void setWarTarget(Game& g, int a, int b, int targetSide) {
  if (!validPair(a, b)) return;
  g.diplomacy.warTarget[key(a, b)] = targetSide;
}

int trust(const Game& g, int a, int b) {
  if (!validPair(a, b)) return 0;
  return g.diplomacy.trust[key(a, b)];
}

void addTrust(Game& g, int a, int b, int delta) {
  if (!validPair(a, b)) return;
  g.diplomacy.trust[key(a, b)] = std::clamp(g.diplomacy.trust[key(a, b)] + delta, -100, 100);
}

int alliedTargetCity(const Game& g, int side) {
  if (side < 0 || side >= 8) return NONE;
  return g.diplomacy.alliedTargetCity[side];
}

void setAlliedTargetCity(Game& g, int side, int cityIndex) {
  if (side < 0 || side >= 8) return;
  g.diplomacy.alliedTargetCity[side] = cityIndex;
}

namespace {
void settleDealTransfers(Game& g, Side& side, Side& other, std::vector<std::string>& messages) {
  int a = side.index, b = other.index;
  int offerA = goldOffer(g, a, b);
  int offerB = goldOffer(g, b, a);
  if (offerA > 0 && side.gold > 0) {
    int paid = std::min(side.gold, offerA);
    side.gold -= paid;
    other.gold += paid;
    addTrust(g, b, a, std::min(30, paid / 10 + 5));
    messages.push_back(fmt("%s paid %d gold in tribute to %s.", side.name.c_str(), paid, other.name.c_str()));
  } else if (offerB > 0 && other.gold > 0) {
    int paid = std::min(other.gold, offerB);
    other.gold -= paid;
    side.gold += paid;
    addTrust(g, a, b, std::min(30, paid / 10 + 5));
    messages.push_back(fmt("%s paid %d gold in tribute to %s.", other.name.c_str(), paid, side.name.c_str()));
  }
  setGoldOffer(g, a, b, 0);
  setGoldOffer(g, b, a, 0);

  int wt = warTarget(g, a, b);
  if (wt == NONE) wt = warTarget(g, b, a);
  if (wt >= 0 && wt < 8 && wt != a && wt != b && g.map->sides[wt].alive) {
    for (int participant : {a, b}) {
      if (state(g, participant, wt) != WAR) {
        g.diplomacy.state[key(participant, wt)] = WAR;
        g.diplomacy.state[key(wt, participant)] = WAR;
        g.diplomacy.proposal[key(participant, wt)] = WAR;
        g.diplomacy.proposal[key(wt, participant)] = WAR;
        setAllied(g, participant, wt, false);
        messages.push_back(fmt("%s joined the war against %s!",
                               g.map->sides[participant].name.c_str(), g.map->sides[wt].name.c_str()));
      }
    }
  }
  setWarTarget(g, a, b, NONE);
  setWarTarget(g, b, a, NONE);
}
}  // namespace

std::vector<std::string> apply(Game& g, Side& side) {
  std::vector<std::string> messages;
  int a = side.index;
  for (int b = 0; b < 8; b++) {
    Side* other = g.map->side(b);
    if (b != a && other) {
      if (!side.classicAi && truceTurns(g, a, b) > 0 && state(g, a, b) == PEACE) {
        setTruce(g, a, b, truceTurns(g, a, b) - 1);
      }
      int now = state(g, a, b);
      int want = proposal(g, a, b);
      if (want != now) {
        if (want > now) {                            // escalation: at once
          bool wasAllied = isAllied(g, a, b);
          int truceLeft = truceTurns(g, a, b);
          setAllied(g, a, b, false);
          setTruce(g, a, b, 0);
          g.diplomacy.state[key(a, b)] = want;
          g.diplomacy.state[key(b, a)] = want;
          if (proposal(g, b, a) < want) g.diplomacy.proposal[key(b, a)] = want;
          if (!side.classicAi && (wasAllied || (truceLeft > 0 && want == WAR))) {
            addTrust(g, b, a, -50);
            for (int c = 0; c < 8; c++) {
              if (c != a && c != b) addTrust(g, c, a, -20);
            }
          }
          if (want == WAR) {
            history::deed(g, &side, history::WAR, a, b, "");          // 484e:0f15
            messages.push_back("War declared with " + other->name + "!");
          }
        } else if (proposal(g, b, a) <= want) {      // de-escalation: mutual
          g.diplomacy.state[key(a, b)] = want;
          g.diplomacy.state[key(b, a)] = want;
          if (want == PEACE) {
            history::deed(g, &side, history::PEACE, a, b, "");         // 484e:1027
            messages.push_back("Peace negotiated with " + other->name + "!");
            if (!side.classicAi || !other->classicAi) {
              setTruce(g, a, b, 10);
              addTrust(g, a, b, 15);
              addTrust(g, b, a, 15);
              settleDealTransfers(g, side, *other, messages);
            }
          }
        }
      }
      // Check mutual Alliance upgrade when at Peace
      if (state(g, a, b) == PEACE && !isAllied(g, a, b) &&
          proposalAllied(g, a, b) && proposalAllied(g, b, a)) {
        setAllied(g, a, b, true);
        setTruce(g, a, b, 15);
        addTrust(g, a, b, 25);
        addTrust(g, b, a, 25);
        settleDealTransfers(g, side, *other, messages);
        messages.push_back("Alliance forged with " + other->name + "!");
      }
    }
  }
  return messages;
}

bool mustDeclare(const Game& g, int a, int b, const City* city) {
  if (g.map->options.diplomacy == 0 || a == NONE || b == NONE) return false;
  int st = state(g, a, b);
  return st == PEACE || (st == INTERMEDIATE && city && city->index > 0);
}

void attackAnyway(Game& g, int a, int b) {
  if (a == NONE || b == NONE) return;
  Side* side = g.map->side(a);
  if (!side) return;
  int st = state(g, a, b);
  if (st == PEACE) game::addDiploScore(g, *side, g.rng.dice(1, 100, 100));
  else if (st == INTERMEDIATE) game::addDiploScore(g, *side, g.rng.dice(1, 15, 10));
  setAllied(g, a, b, false);
  setTruce(g, a, b, 0);
  addTrust(g, b, a, -60);
  for (int c = 0; c < 8; c++) {
    if (c != a && c != b) addTrust(g, c, a, -25);
  }
  for (int k : {key(a, b), key(b, a)}) {
    g.diplomacy.state[k] = WAR;
    g.diplomacy.proposal[k] = WAR;
  }
  history::deed(g, side, history::TREACHERY, a, b, "");
}

void scoreUpdate(Game& g, Side& side) {
  int a = side.index;
  for (int b = 0; b < 8; b++) {
    if (b != a) {
      int p = proposal(g, a, b), st = state(g, a, b);
      if (p < st && p < proposal(g, b, a)) addScore(g, side, st, p);
    }
  }
}

int addScore(Game& g, Side& side, int from, int to) {
  int gain = NONE;
  if (from == WAR && to == PEACE) gain = g.rng.dice(1, 10, 10);
  else if (to < from) gain = g.rng.dice(1, 2, 1);
  if (gain != NONE) game::addDiploScore(g, side, gain);
  return gain;
}

std::map<int, std::string> ratings(const Game& g) {
  struct E { Side* side; int score; int seq; };
  std::vector<E> order;
  // only the sides still in the game are ranked (484e:0aed)
  for (size_t i = 0; i < g.sides.size(); i++) {
    if (g.sides[i]->alive) order.push_back(E{g.sides[i], g.sides[i]->diploScore, (int)i});
  }
  luaSort(order, [](const E& p, const E& q) { return p.score != q.score ? p.score < q.score : p.seq < q.seq; });
  const std::vector<int>& ranks = ratingRanks((int)order.size());
  std::map<int, std::string> out;
  for (size_t i = 0; i < order.size(); i++) {
    int r = i < ranks.size() ? ranks[i] : 8;
    out[order[i].side->index] = TITLES[r - 1];
  }
  return out;
}

AttitudeBreakdown computeAttitude(const Game& g, int aiSide, int targetSide) {
  AttitudeBreakdown out;
  out.fromSide = aiSide;
  out.toSide = targetSide;
  if (!validPair(aiSide, targetSide)) return out;

  const Side& me = g.map->sides[aiSide];
  const Side& them = g.map->sides[targetSide];
  const auto& prof = ai::personality::forSide(me);
  out.personalityName = prof.name;
  out.personalityArchetype = prof.archetype;
  out.motto = prof.motto;
  out.trustScore = trust(g, aiSide, targetSide);
  out.truceRemaining = truceTurns(g, aiSide, targetSide);

  // Only cities the AI side can actually see count, so the attitude score (which
  // feeds the diplomacy policy) never leaks the true map under Hidden Map.
  int totalStandingCities = 0;
  int theirCities = 0, leaderCities = 0, leaderSide = NONE;
  int citiesTakenFromMe = 0;
  std::array<int, 8> sideCities{};
  for (const auto& c : g.map->cities) {
    if (c.razed || !ai::belief::isCityVisible(g, aiSide, c)) continue;
    totalStandingCities++;
    if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
      sideCities[c.ownerIndex]++;
      if (sideCities[c.ownerIndex] > leaderCities) {
        leaderCities = sideCities[c.ownerIndex];
        leaderSide = c.ownerIndex;
      }
    }
    if (c.ownerIndex == targetSide) {
      theirCities++;
      if (c.claim == aiSide) citiesTakenFromMe++;
    }
  }

  auto pushFactor = [&](const std::string& label, int delta) {
    if (delta == 0) return;
    out.factors.push_back(AttitudeFactor{label, delta});
    out.totalScore += delta;
  };

  // Personality disposition baseline
  int baseDisp = (int)std::round((prof.traits[ai::personality::HONOUR] -
                                  prof.traits[ai::personality::AGGRESSION] * 0.6f) * 20.0f);
  pushFactor("Personality (" + prof.archetype + ")", baseDisp);

  // Relational trust & treaty history
  if (out.trustScore != 0) {
    pushFactor("Treaty and trust track record", out.trustScore / 2);
  }
  if (isAllied(g, aiSide, targetSide)) {
    pushFactor("Sworn military alliance", 35);
  } else if (out.truceRemaining > 0) {
    pushFactor(fmt("Active peace truce (%d turns left)", out.truceRemaining), 15);
  }

  // Shared enemies vs opposing wars
  int sharedWars = 0;
  for (int s = 0; s < 8; s++) {
    if (s != aiSide && s != targetSide && g.map->sides[s].alive) {
      if (state(g, aiSide, s) == WAR && state(g, targetSide, s) == WAR) sharedWars++;
    }
  }
  if (sharedWars > 0) {
    int bonus = sharedWars * (int)std::round(18.0f + 10.0f * prof.traits[ai::personality::COALITION_DRIVE]);
    pushFactor(fmt("Fighting %d common %s", sharedWars, sharedWars == 1 ? "enemy" : "enemies"), bonus);
  }

  // Border military pressure (from fair-info belief if available)
  float theirBorderStr = 0.0f, myBorderStr = 0.0f;
  if (me.belief) {
    theirBorderStr = me.belief->theirBorderStrength[targetSide];
    myBorderStr = me.belief->myBorderStrength[targetSide];
  }
  if (theirBorderStr > 4.0f) {
    int borderPen = -std::min(40, (int)std::round(theirBorderStr * 1.2f));
    pushFactor("Your armies massed near our borders", borderPen);
  } else if (myBorderStr > theirBorderStr + 12.0f && prof.traits[ai::personality::AGGRESSION] > 0.2f) {
    pushFactor("Our strike forces outmatch your border garrison", -18);
  }

  // Past battle casualties & lost cities
  if (me.ai) {
    int grudge = me.ai->citiesLost[targetSide] * 10 + me.ai->heroesKilled[targetSide] * 8 +
                 me.ai->lost[targetSide] * 3;
    if (grudge > 0) {
      pushFactor("Blood feud (cities and heroes lost)", -std::min(50, grudge));
    }
  }
  if (citiesTakenFromMe > 0) {
    pushFactor(fmt("You hold %d of our claimed cities", citiesTakenFromMe), -citiesTakenFromMe * 8);
  }
  if (me.capital && me.capital->ownerIndex == targetSide) {
    pushFactor("You occupy our ancestral Capital!", -60);
  }

  // Balance of power / anti-runaway coalition logic
  float theirShare = (float)theirCities / (float)std::max(1, totalStandingCities);
  if (leaderSide == targetSide && theirShare >= 0.30f) {
    int hegemonyPen = -(int)std::round((theirShare - 0.25f) * 120.0f *
                                       (1.0f + 0.5f * prof.traits[ai::personality::COALITION_DRIVE]));
    pushFactor(fmt("Fear of your growing hegemony (%d%% of cities)", (int)std::round(theirShare * 100.0f)),
               hegemonyPen);
  } else if (leaderSide != NONE && leaderSide != aiSide && leaderSide != targetSide &&
             (float)leaderCities / (float)std::max(1, totalStandingCities) >= 0.32f) {
    pushFactor("Need for coalition against the runaway leader", 22);
  }

  // Diplomatic reputation / atrocities
  if (them.diploScore > 60) {
    int atrocityPen = -std::min(35, (them.diploScore - 40) / 4);
    if (prof.traits[ai::personality::HONOUR] > 0.0f) {
      pushFactor("Your reputation for razing and treachery", atrocityPen);
    }
  }

  // Active tribute offer on the table
  int offeredToAi = goldOffer(g, targetSide, aiSide);
  if (offeredToAi > 0) {
    int goldBonus = (int)std::round((float)offeredToAi / 8.0f *
                                    (1.0f + 0.6f * prof.traits[ai::personality::AVARICE]));
    pushFactor(fmt("Gold tribute offered (%d gold)", offeredToAi), goldBonus);
  }

  out.totalScore = std::clamp(out.totalScore, -100, 100);

  // Compute clear stance summary and "What It Takes" forecast
  int curState = state(g, aiSide, targetSide);
  if (isAllied(g, aiSide, targetSide)) {
    out.stanceSummary = "Allied Partner";
    out.whatItTakes = "Alliance is strong. Coordinate strikes against shared foes!";
    out.peaceGoldNeeded = 0;
    out.allyGoldNeeded = 0;
  } else if (me.capital && me.capital->ownerIndex == targetSide) {
    out.stanceSummary = "Mortal Enemy (Capital Held)";
    out.whatItTakes = "Refuses Peace while you hold " + me.capital->name + "!";
    out.peaceGoldNeeded = NONE;
    out.allyGoldNeeded = NONE;
  } else {
    float greedMult = std::max(0.5f, 1.2f - 0.5f * prof.traits[ai::personality::AVARICE]);
    if (out.totalScore >= 35) {
      out.stanceSummary = "Friendly, Cooperative";
      out.peaceGoldNeeded = 0;
      out.allyGoldNeeded = 0;
      out.whatItTakes = "Willing to accept Peace or forge a full Alliance!";
    } else if (out.totalScore >= 0) {
      out.stanceSummary = (curState == WAR) ? "Weary of War" : "Cautious, Pragmatic";
      out.peaceGoldNeeded = 0;
      out.allyGoldNeeded = std::max(50, (int)std::round((35 - out.totalScore) * 8.0f / greedMult) / 25 * 25);
      out.whatItTakes = fmt("Accepts Peace freely; Alliance for ~%d gold (or shared war).", out.allyGoldNeeded);
    } else if (out.totalScore >= -45) {
      out.stanceSummary = "Hostile, Distrustful";
      out.peaceGoldNeeded = std::max(50, (int)std::round((-out.totalScore + 10) * 6.0f / greedMult) / 25 * 25);
      out.allyGoldNeeded = NONE;
      out.whatItTakes = fmt("Demands %d gold tribute (or border withdrawal) for Peace.", out.peaceGoldNeeded);
    } else {
      out.stanceSummary = "Implacable Warpath";
      if (prof.traits[ai::personality::AVARICE] > 0.5f && out.totalScore > -75) {
        out.peaceGoldNeeded = std::max(150, (int)std::round((-out.totalScore) * 8.0f) / 50 * 50);
        out.whatItTakes = fmt("Only a heavy tribute of %d gold will stay their hand!", out.peaceGoldNeeded);
      } else {
        out.peaceGoldNeeded = NONE;
        out.whatItTakes = "Refuses Peace: determined to settle grievances on the battlefield!";
      }
      out.allyGoldNeeded = NONE;
    }
  }

  return out;
}

}  // namespace w2::diplomacy
