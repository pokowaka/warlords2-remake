// Modern-AI behaviour diagnostic (`w2aidiag`): plays one game with the Modern
// AI on every other side and the 1993 Classic AI on the rest, then prints
//   - every 5 turns, each side's cities, armies, strength, gold and upkeep;
//   - the share of Modern armies that moved on their own turn;
//   - the missions the Modern AI chose (by kind) and what its cities built.
// Used to see *why* a checkpoint wins or loses, which the city-share
// benchmark in w2arena does not show.
//
//   cpp/build/w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1]
//
// Run from the repository root, so that original/ is found.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "opponents.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

using namespace w2;

static void printSides(Game& g, const bool* modern) {
  std::printf("--- turn %d\n", g.turn);
  for (int s = 0; s < 8; s++) {
    Side& sd = g.map->sides[s];
    if (!sd.inUse) continue;
    int cities = 0, armies = 0, str = 0, inCity = 0;
    for (auto& c : g.map->cities) if (!c.razed && c.ownerIndex == s) cities++;
    for (Army* a : g.armies) {
      if (a->owner != s || a->x == NONE) continue;
      armies++;
      str += a->strength;
      if (game::cityAt(g, a->x, a->y)) inCity++;
    }
    std::printf("  side %d %s %s cities=%2d armies=%3d strength=%4d in-city=%3d gold=%5d income=%4d upkeep=%4d\n", s,
                modern[s] ? "MODERN " : "CLASSIC", sd.alive ? "     " : "(dead)", cities, armies, str, inCity, sd.gold,
                game::income(g, sd), game::upkeep(g, sd));
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1] [ACTIVE_SIDES] [CROP_MARGIN] "
                 "[SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider]\n");
    return 2;
  }
  std::string weights = argv[1];
  std::string scen = argc > 2 ? argv[2] : "ERYTHEA";
  int seed = argc > 3 ? std::atoi(argv[3]) : 800001;
  int turns = argc > 4 ? std::atoi(argv[4]) : 40;
  int hidden = argc > 5 ? std::atoi(argv[5]) : 0;
  int activeSides = argc > 6 ? std::atoi(argv[6]) : 8;
  int cropMargin = argc > 7 ? std::atoi(argv[7]) : 0;
  int soloSeat = argc > 8 ? std::atoi(argv[8]) : -1;
  int diplo = argc > 9 ? std::atoi(argv[9]) : 0;
  opponents::Kind opp = (argc > 10 && std::string(argv[10]) == "raider") ? opponents::Kind::Raider
                                                                         : opponents::Kind::Classic;
  ai::ml::PolicyNetwork net;
  if (!net.loadJson(weights)) {
    std::fprintf(stderr, "w2aidiag: cannot load weights '%s' (missing, or trained for different input sizes)\n",
                 weights.c_str());
    return 1;
  }
  game::NewGameOptions o;
  o.seed = seed;
  o.activeSides = activeSides;
  o.cropMargin = cropMargin;
  o.options = {{"hiddenMap", hidden}, {"viewEnemies", 0}, {"diplomacy", diplo}, {"neutralCities", 1}, {"quests", 1}};
  for (int i = 0; i < 8; i++) {
    o.sides[i].computer = true;
    o.sides[i].level = 2;
    o.sides[i].card = (seed + i * 5) % 9;
  }
  randommap::Files rfiles;
  if (scen == "RANDOM") {
    Rng mapRng((double)(seed ^ 0x5bf03635u));
    randommap::Options ro;
    ro.dataDir = "original";
    ro.rng = &mapRng;
    ro.sliders = {3, 3, 2, 3};
    rfiles = randommap::generateNow(ro);
    o.memoryFiles = &rfiles;
  }
  auto gp = game::newGame("original", scen, o);
  Game& g = *gp;
  std::printf("=== %s seed=%d sides=%d crop=%d cities=%zu box=[%d..%d]x[%d..%d] ===\n", scen.c_str(), seed, activeSides,
              cropMargin, g.map->cities.size(), g.map->activeX0, g.map->activeX1, g.map->activeY0, g.map->activeY1);
  bool modern[8] = {};
  int idx = 0;
  for (int s = 0; s < 8; s++) {
    if (!g.map->sides[s].inUse) continue;
    modern[s] = (soloSeat >= 0) ? (idx == soloSeat) : (idx % 2 == 0);
    idx++;
    g.map->sides[s].classicAi = !modern[s];
  }

  std::map<std::string, int> missions, built;
  long armyTurns = 0, moved = 0;
  long cityTurns = 0, producingTurns = 0, purchases = 0, richTurns = 0, sideTurns = 0;
  int lastPrinted = 0;
  Side* cur = game::begin(g);
  // City changes of hands: losses by camp of the loser, bucketed by the
  // garrison (armies in the city) the loser had when the attacker's turn began.
  long lostByCamp[2][3] = {}, capturedFromEnemy[2] = {}, capturedNeutral[2] = {};
  auto garrisonOf = [&](const City& c) {
    int n = 0;
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++)
        for (Army* a : game::armiesAt(g, c.x + dx, c.y + dy))
          if (a->owner == c.ownerIndex) n++;
    return n;
  };
  while (cur && !g.over && !g.won && g.turn <= turns) {
    std::vector<int> ownerBefore(g.map->cities.size()), garBefore(g.map->cities.size());
    for (size_t ci = 0; ci < g.map->cities.size(); ci++) {
      ownerBefore[ci] = g.map->cities[ci].ownerIndex;
      garBefore[ci] = g.map->cities[ci].ownerIndex >= 0 ? garrisonOf(g.map->cities[ci]) : 0;
    }
    const int mover = cur->index;
    if (cur->alive) {
      if (modern[cur->index]) {
        std::map<Army*, int> before;
        for (Army* a : g.armies) if (a->owner == cur->index && a->x != NONE) before[a] = a->y * 1000 + a->x;
        std::map<int, std::vector<int>> slotsBefore;
        for (auto& c : g.map->cities) {
          if (c.ownerIndex != cur->index) continue;
          for (const auto& sl : c.slots) slotsBefore[c.index].push_back(sl.type);
        }
        ai::modern::playTurn(g, *cur, nullptr, &net, nullptr);
        sideTurns++;
        if (cur->gold >= 1500) richTurns++;
        for (auto& c : g.map->cities) {
          if (c.ownerIndex != cur->index || c.razed) continue;
          cityTurns++;
          if (game::producing(c)) producingTurns++;
          auto it = slotsBefore.find(c.index);
          for (const auto& sl : c.slots) {
            if (it != slotsBefore.end() &&
                std::find(it->second.begin(), it->second.end(), sl.type) == it->second.end()) {
              purchases++;
            }
          }
        }
        for (auto& [a, pos] : before) {
          if (!g.alive(a)) continue;
          armyTurns++;
          if (a->x != NONE && a->y * 1000 + a->x != pos) moved++;
        }
        for (auto& t : cur->aiThoughts) {
          if (t.category != "Mission") continue;
          std::string label = t.targetName.substr(0, t.targetName.find(' '));
          if (t.targetName.rfind("Capture Neutral", 0) == 0) label = "CaptureNeutral";
          missions[label]++;
        }
        for (auto& c : g.map->cities) {
          if (c.ownerIndex == cur->index && c.producing != NONE && c.producing < (int)c.slots.size()) {
            built[c.slots[c.producing].name]++;
          }
        }
      } else {
        opponents::playTurn(g, *cur, opp);
      }
    }
    for (size_t ci = 0; ci < g.map->cities.size(); ci++) {
      const City& c = g.map->cities[ci];
      if (c.razed || c.ownerIndex != mover || ownerBefore[ci] == mover) continue;
      int moverCamp = modern[mover] ? 0 : 1;
      if (ownerBefore[ci] < 0 || ownerBefore[ci] >= 8) {
        capturedNeutral[moverCamp]++;
      } else {
        capturedFromEnemy[moverCamp]++;
        lostByCamp[modern[ownerBefore[ci]] ? 0 : 1][std::min(2, garBefore[ci])]++;
      }
    }
    cur = game::endTurn(g);
    if (g.turn != lastPrinted && (g.turn % 5 == 0 || g.turn == 2)) {
      lastPrinted = g.turn;
      printSides(g, modern);
    }
  }
  std::printf("\nModern armies that moved on their turn: %ld of %ld army-turns (%.0f%%)\n", moved, armyTurns,
              100.0 * moved / std::max(1L, armyTurns));
  std::printf("Modern economy: cities producing %ld of %ld city-turns (%.0f%%) | slot purchases %ld | "
              "side-turns ending with >= 1500 gold: %ld of %ld\n",
              producingTurns, cityTurns, 100.0 * producingTurns / std::max(1L, cityTurns), purchases, richTurns,
              sideTurns);
  for (int camp = 0; camp < 2; camp++) {
    std::printf("%s: captured %ld neutral + %ld enemy cities | lost %ld cities (garrison 0: %ld, 1: %ld, 2+: %ld)\n",
                camp == 0 ? "Modern " : "Classic", capturedNeutral[camp], capturedFromEnemy[camp],
                lostByCamp[camp][0] + lostByCamp[camp][1] + lostByCamp[camp][2], lostByCamp[camp][0],
                lostByCamp[camp][1], lostByCamp[camp][2]);
  }
  std::printf("Modern missions other than holding a city (kind: count):\n");
  for (auto& [k, v] : missions) std::printf("  %-16s %d\n", k.c_str(), v);
  std::printf("Modern city production (unit: city-turns):\n");
  for (auto& [k, v] : built) std::printf("  %-16s %d\n", k.c_str(), v);
  return 0;
}
