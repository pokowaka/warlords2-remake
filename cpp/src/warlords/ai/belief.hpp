// Fair-information observation filter and per-side belief memory.
//
// Architectural Role: Enforces strict human-equivalent information rules for the
// AI under Hidden Map and View Enemies settings. Tracks last-observed enemy stack
// compositions from battles, constructs synthetic defender estimates for uninspected
// stacks, identifies fog-of-war exploration frontiers, and computes spatial threat maps.
#pragma once

#include <array>
#include <map>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct ObservedStack {
  int x = NONE, y = NONE;
  int owner = NONE;
  int visibleCount = 0;
  int topType = 0;
  int lastInspectedTurn = NONE;
  std::vector<int> knownTypes;
  std::vector<int> knownStrengths;
};

struct FrontierCluster {
  int x = 0, y = 0;
  int unseenNeighbours = 0;
  int regionUnseenCount = 0;
  bool isWater = false;
  bool onRoad = false;
};

struct SideBelief {
  int updatedTurn = 0;
  float frontierRatio = 0.0f;
  std::map<int, ObservedStack> stacksByTile;
  std::array<float, 8> seenEnemyAvgStrength{4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f};
  std::vector<City*> visibleCities;
  std::vector<Site*> visibleSites;
  std::vector<Item*> visibleGroundItems;
  std::vector<FrontierCluster> fogFrontiers;
  std::vector<float> cityThreat1Turn;
  std::vector<float> cityThreat2Turn;
  std::vector<float> cityNearestEnemyDist;
  std::array<float, 8> myBorderStrength{};
  std::array<float, 8> theirBorderStrength{};
  std::array<int, 8> visibleCitiesBySide{};
};

namespace ai::belief {

struct CombatEstimate {
  float winProb = 1.0f;
  float expectedSurvivors = 0.0f;
  float expectedEnemyLosses = 0.0f;
  int estimatedDefenderCount = 0;
  float estimatedDefenderStrength = 0.0f;
  float seenTileFraction = 1.0f;
};

/**
 * Ensures the side has an initialized SideBelief instance and returns it.
 */
SideBelief& ensure(Side& side);

/**
 * Returns true if tile (x, y) is visible to sideIndex under current game rules
 * (including Allied shared vision when Hidden Map is active).
 */
bool isTileVisible(const Game& g, int sideIndex, int x, int y);

/**
 * Returns true if any of the city's 2x2 footprint tiles are visible to sideIndex,
 * or if the city is the target of sideIndex's active quest.
 */
bool isCityVisible(const Game& g, int sideIndex, const City& city);

/**
 * Returns true if the site's tile is visible to sideIndex or revealed by a sage/quest.
 */
bool isSiteVisible(const Game& g, int sideIndex, const Site& site);

/**
 * Rebuilds the side's fair-information observation state, threat heatmaps,
 * border strength ratios, and fog-of-war frontier clusters for the current turn.
 */
void update(Game& g, Side& side);

/**
 * Records the revealed composition of an enemy stack after a battle at (x, y).
 */
void recordBattleObservation(Game& g, int observerSide, int x, int y);

/**
 * Estimates combat outcome for attacking (tx, ty) using strictly human-visible
 * information (synthetic defender estimation when View Enemies is off) without
 * mutating g.rng.
 */
CombatEstimate estimateCombat(Game& g, const Side& side, const std::vector<Army*>& attackers,
                              int tx, int ty, float cautionBias = 0.0f);

}  // namespace ai::belief
}  // namespace w2
