#include "warlords/ai/belief.hpp"

#include <algorithm>
#include <cmath>

#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::belief {

SideBelief& ensure(Side& side) {
  if (!side.belief) side.belief = std::make_shared<SideBelief>();
  return *side.belief;
}

bool isTileVisible(const Game& g, int sideIndex, int x, int y) {
  return game::seen(g, sideIndex, x, y);
}

bool isCityVisible(const Game& g, int sideIndex, const City& city) {
  if (city.razed) return false;
  if (g.map->options.hiddenMap == 0) return true;
  for (int dy = 0; dy <= 1; dy++) {
    for (int dx = 0; dx <= 1; dx++) {
      if (game::seen(g, sideIndex, city.x + dx, city.y + dy)) return true;
    }
  }
  const Side* s = g.map->side(sideIndex);
  if (s && s->quest && s->quest->city == &city) return true;
  return false;
}

bool isSiteVisible(const Game& g, int sideIndex, const Site& site) {
  if (game::seen(g, sideIndex, site.x, site.y)) return true;
  const Side* s = g.map->side(sideIndex);
  return s && site::shownTo(site, *s);
}

void recordBattleObservation(Game& g, int observerSide, int x, int y) {
  if (observerSide < 0 || observerSide >= 8) return;
  Side* side = g.map->side(observerSide);
  if (!side) return;
  SideBelief& b = ensure(*side);

  std::vector<std::pair<int, int>> tiles;
  if (City* c = game::cityAt(g, x, y)) {
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++) tiles.emplace_back(c->x + dx, c->y + dy);
  } else {
    tiles.emplace_back(x, y);
  }

  for (auto [tx, ty] : tiles) {
    int key = ty * g.map->width + tx;
    auto armies = game::armiesAt(g, tx, ty);
    if (armies.empty() || armies[0]->owner == observerSide) {
      b.stacksByTile.erase(key);
      continue;
    }
    ObservedStack obs;
    obs.x = tx;
    obs.y = ty;
    obs.owner = armies[0]->owner;
    obs.visibleCount = (int)armies.size();
    obs.topType = armies[0]->type;
    obs.lastInspectedTurn = g.turn;
    float sumStr = 0.0f;
    for (Army* a : armies) {
      obs.knownTypes.push_back(a->type);
      obs.knownStrengths.push_back(a->strength);
      sumStr += (float)a->strength;
    }
    if (obs.owner >= 0 && obs.owner < 8 && !armies.empty()) {
      float avg = sumStr / (float)armies.size();
      b.seenEnemyAvgStrength[obs.owner] = 0.7f * b.seenEnemyAvgStrength[obs.owner] + 0.3f * avg;
    }
    b.stacksByTile[key] = std::move(obs);
  }
}

void update(Game& g, Side& side) {
  SideBelief& b = ensure(side);
  b.updatedTurn = g.turn;
  int me = side.index;
  int W = g.map->width, H = g.map->height;

  b.visibleCities.clear();
  b.visibleSites.clear();
  b.visibleGroundItems.clear();
  b.fogFrontiers.clear();
  b.visibleCitiesBySide.fill(0);
  b.myBorderStrength.fill(0.0f);
  b.theirBorderStrength.fill(0.0f);

  // Clean stale tile observations and scan visible enemy stacks
  std::map<int, ObservedStack> refreshed;
  int unseenTotal = 0;
  std::vector<FrontierCluster> rawFrontiers;

  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (!isTileVisible(g, me, x, y)) {
        unseenTotal++;
        continue;
      }
      int key = y * W + x;
      auto armies = game::armiesAt(g, x, y);
      if (!armies.empty() && armies[0]->owner != me && armies[0]->owner != NONE) {
        int owner = armies[0]->owner;
        bool tower = game::towerAt(g, x, y);
        bool fullView = g.map->options.viewEnemies != 0;
        ObservedStack obs;
        obs.x = x;
        obs.y = y;
        obs.owner = owner;
        obs.topType = armies[0]->type;
        obs.visibleCount = (tower && !fullView) ? 4 : (int)armies.size();

        const ArmyType* topAt = g.types.byId(obs.topType);
        if (topAt && owner >= 0 && owner < 8) {
          b.seenEnemyAvgStrength[owner] =
              0.9f * b.seenEnemyAvgStrength[owner] + 0.1f * (float)std::max(2, topAt->strength);
        }

        auto prevIt = b.stacksByTile.find(key);
        if (fullView) {
          obs.lastInspectedTurn = g.turn;
          for (Army* a : armies) {
            obs.knownTypes.push_back(a->type);
            obs.knownStrengths.push_back(a->strength);
          }
        } else if (prevIt != b.stacksByTile.end() && prevIt->second.owner == owner &&
                   prevIt->second.visibleCount == (int)armies.size() &&
                   g.turn - prevIt->second.lastInspectedTurn <= 2) {
          obs.lastInspectedTurn = prevIt->second.lastInspectedTurn;
          obs.knownTypes = prevIt->second.knownTypes;
          obs.knownStrengths = prevIt->second.knownStrengths;
        }
        refreshed[key] = std::move(obs);
      }

      // Check if this visible tile is on the fog frontier (walkable land and adjacent to unseen)
      if (g.map->options.hiddenMap != 0) {
        int terr = g.map->terrainType[g.map->tiles[key] & 0xff];
        if (terr != move::WATER && terr != move::SHORE && terr != move::MOUNTAINS) {
          bool isRoad = (scn::roadAt(*g.map, x, y) % 0x20 != 0);
          int adjUnseen = 0;
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              int nx = x + dx, ny = y + dy;
              if (nx >= 0 && ny >= 0 && nx < W && ny < H && !isTileVisible(g, me, nx, ny)) {
                adjUnseen++;
              }
            }
          }
          if (adjUnseen >= 2 || (isRoad && adjUnseen >= 1)) {
            int weight = adjUnseen + (isRoad ? 6 : 0);
            rawFrontiers.push_back(FrontierCluster{x, y, weight, weight});
          }
        }
      }
    }
  }
  b.stacksByTile = std::move(refreshed);
  b.frontierRatio = (float)unseenTotal / (float)(W * H);

  // Sort rawFrontiers by proximity to our own cities/armies so local expansion frontiers
  // are always represented in the top clusters
  std::vector<std::pair<int, int>> myAnchors;
  for (const auto& c : g.map->cities) {
    if (!c.razed && c.ownerIndex == me) myAnchors.emplace_back(c.x, c.y);
  }
  for (const Army* a : g.armies) {
    if (a->owner == me && a->x != NONE && !a->transit) myAnchors.emplace_back(a->x, a->y);
  }
  std::sort(rawFrontiers.begin(), rawFrontiers.end(), [&](const FrontierCluster& fa, const FrontierCluster& fb) {
    int da = 999, db = 999;
    for (const auto& [ax, ay] : myAnchors) {
      da = std::min(da, move::distance(fa.x, fa.y, ax, ay));
      db = std::min(db, move::distance(fb.x, fb.y, ax, ay));
    }
    return da - fa.unseenNeighbours < db - fb.unseenNeighbours;
  });

  // Cluster fog frontiers so they are spaced at least 3 tiles apart, keeping the closest
  // anchor tile (never overwriting with a tile across water/mountains)
  for (const auto& fc : rawFrontiers) {
    bool merged = false;
    for (auto& existing : b.fogFrontiers) {
      if (move::distance(fc.x, fc.y, existing.x, existing.y) < 3) {
        existing.regionUnseenCount += fc.unseenNeighbours;
        merged = true;
        break;
      }
    }
    if (!merged && b.fogFrontiers.size() < 64) {
      b.fogFrontiers.push_back(fc);
    }
  }

  for (auto& c : g.map->cities) {
    if (isCityVisible(g, me, c)) {
      b.visibleCities.push_back(&c);
      if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
        b.visibleCitiesBySide[c.ownerIndex]++;
      }
    }
  }

  for (auto& s : g.map->sites) {
    if (isSiteVisible(g, me, s)) b.visibleSites.push_back(&s);
  }

  for (auto& it : g.map->items) {
    if (it.status == 1 && it.x != NONE && !it.planted && isTileVisible(g, me, it.x, it.y)) {
      b.visibleGroundItems.push_back(&it);
    }
  }

  // Compute per-city threat and nearest non-friendly distance using visible information only
  size_t nCities = g.map->cities.size();
  b.cityThreat1Turn.assign(nCities, 0.0f);
  b.cityThreat2Turn.assign(nCities, 0.0f);
  b.cityNearestEnemyDist.assign(nCities, 200.0f);

  for (City* c : b.visibleCities) {
    float minDist = 200.0f;
    for (City* other : b.visibleCities) {
      if (other->ownerIndex != me && !other->razed) {
        float d = (float)move::distance(c->x, c->y, other->x, other->y);
        if (d < minDist) minDist = d;
      }
    }
    for (const auto& fc : b.fogFrontiers) {
      float d = (float)move::distance(c->x, c->y, fc.x, fc.y);
      if (d < minDist) minDist = d;
    }
    b.cityNearestEnemyDist[c->index] = minDist;

    float t1 = 0.0f, t2 = 0.0f;
    for (const auto& [tileKey, obs] : b.stacksByTile) {
      if (obs.owner == me || obs.owner == NONE) continue;
      int d = move::distance(c->x, c->y, obs.x, obs.y);
      const ArmyType* topAt = g.types.byId(obs.topType);
      int estMove = topAt ? std::max(10, topAt->move) : 12;
      int reach1 = std::max(6, estMove / 2);
      int reach2 = reach1 * 2;
      float estStr = 0.0f;
      if (!obs.knownStrengths.empty()) {
        for (int s : obs.knownStrengths) estStr += (float)s;
      } else {
        float topStr = topAt ? (float)topAt->strength : 4.0f;
        float restStr = (obs.owner >= 0 && obs.owner < 8) ? b.seenEnemyAvgStrength[obs.owner] : 4.0f;
        estStr = topStr + (float)std::max(0, obs.visibleCount - 1) * restStr;
      }
      if (d <= reach1) t1 += estStr;
      if (d <= reach2) t2 += estStr;

      if (c->ownerIndex == me && d <= 18 && obs.owner >= 0 && obs.owner < 8) {
        b.theirBorderStrength[obs.owner] += estStr;
      }
    }
    b.cityThreat1Turn[c->index] = t1;
    b.cityThreat2Turn[c->index] = t2;
  }

  // Compute our own border strength near each rival's visible cities
  for (Army* a : g.armies) {
    if (a->owner != me || a->x == NONE || a->transit) continue;
    for (City* c : b.visibleCities) {
      if (c->ownerIndex >= 0 && c->ownerIndex < 8 && c->ownerIndex != me) {
        if (move::distance(a->x, a->y, c->x, c->y) <= 18) {
          b.myBorderStrength[c->ownerIndex] += (float)a->strength;
        }
      }
    }
  }
}

CombatEstimate estimateCombat(Game& g, const Side& side, const std::vector<Army*>& attackers,
                              int tx, int ty, float cautionBias) {
  CombatEstimate est;
  if (attackers.empty()) {
    est.winProb = 0.0f;
    return est;
  }
  int me = side.index;
  City* city = game::cityAt(g, tx, ty);
  bool targetVisible = city ? isCityVisible(g, me, *city) : isTileVisible(g, me, tx, ty);
  if (!targetVisible) {
    est.winProb = 0.85f;
    est.expectedSurvivors = (float)attackers.size() * 0.85f;
    return est;
  }

  const SideBelief& b = side.belief ? *side.belief : ensure(const_cast<Side&>(side));

  // Gather visible defender tiles (4 tiles if city, 1 tile otherwise)
  std::vector<std::pair<int, int>> defTiles;
  if (city) {
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++) defTiles.emplace_back(city->x + dx, city->y + dy);
  } else {
    defTiles.emplace_back(tx, ty);
  }

  bool fullView = g.map->options.viewEnemies != 0;
  std::vector<Army> synthPool;
  synthPool.reserve(32);
  std::vector<Army*> defenders;

  for (auto [dx, dy] : defTiles) {
    if (!isTileVisible(g, me, dx, dy)) continue;
    int key = dy * g.map->width + dx;
    auto actual = game::armiesAt(g, dx, dy);
    if (actual.empty() || actual[0]->owner == me) continue;

    if (fullView) {
      for (Army* a : actual) {
        defenders.push_back(a);
        est.estimatedDefenderStrength += (float)a->strength;
      }
      est.estimatedDefenderCount += (int)actual.size();
      continue;
    }

    auto it = b.stacksByTile.find(key);
    int owner = actual[0]->owner;
    int topType = actual[0]->type;
    bool tower = game::towerAt(g, dx, dy);
    int count = tower ? 4 : (int)actual.size();
    if (it != b.stacksByTile.end() && !it->second.knownTypes.empty()) {
      for (size_t i = 0; i < it->second.knownTypes.size(); i++) {
        Army syn;
        syn.owner = owner;
        syn.x = dx;
        syn.y = dy;
        syn.type = it->second.knownTypes[i];
        syn.strength = it->second.knownStrengths[i];
        synthPool.push_back(syn);
        defenders.push_back(&synthPool.back());
        est.estimatedDefenderStrength += (float)syn.strength;
      }
      est.estimatedDefenderCount += (int)it->second.knownTypes.size();
    } else {
      float avgStr = (owner >= 0 && owner < 8) ? b.seenEnemyAvgStrength[owner] : 3.5f;
      int estUnderStr = std::clamp((int)std::round(avgStr + cautionBias), 2, 8);
      for (int i = 0; i < count; i++) {
        Army syn;
        syn.owner = owner;
        syn.x = dx;
        syn.y = dy;
        if (i == 0) {
          syn.type = topType;
          const ArmyType* at = g.types.byId(topType);
          syn.strength = at ? at->strength : estUnderStr;
        } else {
          syn.type = 2;  // standard infantry baseline type
          syn.strength = estUnderStr;
        }
        synthPool.push_back(syn);
        defenders.push_back(&synthPool.back());
        est.estimatedDefenderStrength += (float)syn.strength;
      }
      est.estimatedDefenderCount += count;
    }
  }

  if (defenders.empty() && city && city->ownerIndex != me && !city->razed) {
    // If any of the 4 tiles of a 2x2 city is still in the fog (e.g. Classic AI places garrisons
    // on (city->x + 1, city->y)), a fair human player knows an unprobed Neutral or Enemy city
    // has a standard garrison on its unseen tile(s) rather than 0 defenders!
    bool allCityTilesSeen = true;
    for (int dy = 0; dy <= 1; dy++) {
      for (int dx = 0; dx <= 1; dx++) {
        if (!isTileVisible(g, me, city->x + dx, city->y + dy)) allCityTilesSeen = false;
      }
    }
    if (!allCityTilesSeen) {
      int owner = city->ownerIndex;
      int estCount = (owner == NONE) ? std::max(1, g.map->options.neutralCities + 1) : 3;
      float avgStr = (owner >= 0 && owner < 8) ? b.seenEnemyAvgStrength[owner] : 3.5f;
      int estStr = std::clamp((int)std::round(avgStr + cautionBias), 2, 7);
      for (int i = 0; i < estCount; i++) {
        Army syn;
        syn.owner = owner;
        syn.x = city->x;
        syn.y = city->y;
        syn.type = 2;
        syn.strength = estStr;
        synthPool.push_back(syn);
        defenders.push_back(&synthPool.back());
        est.estimatedDefenderStrength += (float)estStr;
      }
      est.estimatedDefenderCount += estCount;
    }
  }

  if (defenders.empty()) {
    est.winProb = 1.0f;
    est.expectedSurvivors = (float)attackers.size();
    return est;
  }

  // Save RNG state so combat estimation is 100% side-effect free
  uint32_t savedRng = g.rng.state;
  g.rng.state = savedRng ^ (uint32_t)(tx * 73856093 ^ ty * 19349663 ^ (int)attackers.size() * 83492791);
  if (g.rng.state == 0) g.rng.state = 1;

  constexpr int SIMS = 8;
  int wins = 0;
  int totalSurv = 0;
  int totalEnemyDead = 0;
  for (int i = 0; i < SIMS; i++) {
    Battle res = combat::resolve(g, attackers, defenders, tx, ty);
    if (res.won) wins++;
    totalSurv += (int)res.attackers.size();
    totalEnemyDead += (int)res.deadDefenders.size();
  }

  g.rng.state = savedRng;
  est.winProb = (float)wins / (float)SIMS;
  est.expectedSurvivors = (float)totalSurv / (float)SIMS;
  est.expectedEnemyLosses = (float)totalEnemyDead / (float)SIMS;
  return est;
}

}  // namespace w2::ai::belief
