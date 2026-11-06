#include "warlords/ai/belief.hpp"

#include <algorithm>
#include <cmath>

#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
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

namespace {
thread_local const ScopedTileIndex* activeIndex = nullptr;
}

ScopedTileIndex::ScopedTileIndex(const Game& g) : g_(g), prev_(activeIndex) {
  byTile_.resize((size_t)g.map->width * g.map->height);
  // newest army first, as game::armiesAt lists them
  for (int i = (int)g.armies.size() - 1; i >= 0; i--) {
    Army* a = g.armies[i];
    if (a->transit || a->x < 0 || a->y < 0 || a->x >= g.map->width || a->y >= g.map->height) continue;
    byTile_[(size_t)a->y * g.map->width + a->x].push_back(a);
  }
  activeIndex = this;
}

ScopedTileIndex::~ScopedTileIndex() { activeIndex = prev_; }

const std::vector<Army*>& ScopedTileIndex::at(int x, int y) const {
  static const std::vector<Army*> none;
  if (x < 0 || y < 0 || x >= g_.map->width || y >= g_.map->height) return none;
  return byTile_[(size_t)y * g_.map->width + x];
}

std::vector<Army*> armiesAt(const Game& g, int x, int y) {
  if (activeIndex && &activeIndex->game() == &g) return activeIndex->at(x, y);
  return game::armiesAt(g, x, y);
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
  ScopedTileIndex tileIndex(g);   // nothing moves while the belief is rebuilt
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
      auto armies = armiesAt(g, x, y);
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

      // Record visible land and coastal/sea tiles bordering unseen fog
      if (g.map->options.hiddenMap != 0) {
        int terr = g.map->terrainType[g.map->tiles[key] & 0xff];
        if (terr != move::MOUNTAINS) {
          bool isWater = (terr == move::WATER || terr == move::SHORE);
          bool isRoad = !isWater && (scn::roadAt(*g.map, x, y) % 0x20 != 0);
          int adjUnseen = 0;
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              int nx = x + dx, ny = y + dy;
              if (nx >= 0 && ny >= 0 && nx < W && ny < H && !isTileVisible(g, me, nx, ny)) {
                adjUnseen++;
              }
            }
          }
          if (adjUnseen >= 1) {
            rawFrontiers.push_back(FrontierCluster{x, y, adjUnseen, adjUnseen, isWater, isRoad});
          }
        }
      }
    }
  }
  b.stacksByTile = std::move(refreshed);
  int activeArea = std::max(1, (g.map->activeX1 - g.map->activeX0 + 1) * (g.map->activeY1 - g.map->activeY0 + 1));
  b.frontierRatio = std::min(1.0f, (float)unseenTotal / (float)activeArea);

  // Sort rawFrontiers by proximity to our own cities/armies (preferring road tiles
  // that connect cities) so local expansion frontiers are represented in top clusters.
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
    int sa = da - (fa.onRoad ? 4 : 0);
    int sb = db - (fb.onRoad ? 4 : 0);
    if (sa != sb) return sa < sb;
    return fa.onRoad > fb.onRoad;
  });

  // Cluster land frontiers separately from water frontiers so coastal land does not suppress
  // amphibious sea lanes (and vice versa); snap cluster anchor onto road tiles when merged.
  int landCount = 0, waterCount = 0;
  for (const auto& fc : rawFrontiers) {
    bool merged = false;
    for (auto& existing : b.fogFrontiers) {
      if (existing.isWater == fc.isWater && move::distance(fc.x, fc.y, existing.x, existing.y) < 3) {
        existing.regionUnseenCount += fc.unseenNeighbours;
        if (fc.onRoad && !existing.onRoad) {
          existing.x = fc.x;
          existing.y = fc.y;
          existing.onRoad = true;
        }
        merged = true;
        break;
      }
    }
    if (!merged) {
      if (!fc.isWater && landCount < 48) {
        b.fogFrontiers.push_back(fc);
        landCount++;
      } else if (fc.isWater && waterCount < 24) {
        b.fogFrontiers.push_back(fc);
        waterCount++;
      }
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

  // Compute per-city threat and nearest visible rival distance using fair-information state only
  size_t nCities = g.map->cities.size();
  b.cityThreat1Turn.assign(nCities, 0.0f);
  b.cityThreat2Turn.assign(nCities, 0.0f);
  b.cityNearestEnemyDist.assign(nCities, 200.0f);

  for (City* c : b.visibleCities) {
    float minDist = 200.0f;
    for (City* other : b.visibleCities) {
      if (other->ownerIndex >= 0 && other->ownerIndex != me && !other->razed &&
          !diplomacy::isAllied(g, me, other->ownerIndex)) {
        float d = (float)move::distance(c->x, c->y, other->x, other->y);
        if (d < minDist) minDist = d;
      }
    }
    for (const auto& [tileKey, obs] : b.stacksByTile) {
      if (obs.owner >= 0 && obs.owner != me && !diplomacy::isAllied(g, me, obs.owner)) {
        float d = (float)move::distance(c->x, c->y, obs.x, obs.y);
        if (d < minDist) minDist = d;
      }
    }
    b.cityNearestEnemyDist[c->index] = minDist;

    float t1 = 0.0f, t2 = 0.0f;
    for (const auto& [tileKey, obs] : b.stacksByTile) {
      if (obs.owner == me || obs.owner == NONE || diplomacy::isAllied(g, me, obs.owner)) continue;
      int d = move::distance(c->x, c->y, obs.x, obs.y);
      const ArmyType* topAt = g.types.byId(obs.topType);
      int estMove = topAt ? std::max(10, topAt->move) : 12;
      int reach1 = std::max(8, (estMove * 3) / 4);
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

  int seenTiles = 0;
  for (auto [dx, dy] : defTiles) {
    if (!isTileVisible(g, me, dx, dy)) continue;
    seenTiles++;
    int key = dy * g.map->width + dx;
    auto actual = armiesAt(g, dx, dy);
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
          syn.type = 2;
          syn.strength = estUnderStr;
        }
        synthPool.push_back(syn);
        defenders.push_back(&synthPool.back());
        est.estimatedDefenderStrength += (float)syn.strength;
      }
      est.estimatedDefenderCount += count;
    }
  }

  est.seenTileFraction = defTiles.empty() ? 1.0f : (float)seenTiles / (float)defTiles.size();

  // Calibrated partial-visibility belief when some tiles of a 2x2 city remain in fog
  // and no defenders were observed on the visible tiles:
  int unseenEnemyTiles = 0;
  std::vector<Army*> unseenFallbackDefenders;
  if (defenders.empty() && city && city->ownerIndex != me && !city->razed && seenTiles < 4) {
    if (city->ownerIndex == NONE) {
      // Neutral garrisons strictly occupy (city->x, city->y). Only when (city->x, city->y)
      // itself is unseen can a neutral garrison be hidden there.
      if (!isTileVisible(g, me, city->x, city->y)) {
        Army syn;
        syn.owner = NONE;
        syn.x = city->x;
        syn.y = city->y;
        syn.type = (g.map->options.neutralCities <= 0) ? 0 : 2;
        syn.strength = (g.map->options.neutralCities <= 0) ? 1 : 4;
        synthPool.push_back(syn);
        defenders.push_back(&synthPool.back());
        est.estimatedDefenderCount = 1;
        est.estimatedDefenderStrength = (float)syn.strength;
      }
    } else {
      // Enemy city with unseen corner(s): sample unseen garrison occupancy proportionally
      // to (4 - seenTiles) / 4 across the Monte Carlo rollouts rather than assuming a guaranteed 3-stack
      unseenEnemyTiles = 4 - seenTiles;
      int owner = city->ownerIndex;
      float avgStr = (owner >= 0 && owner < 8) ? b.seenEnemyAvgStrength[owner] : 4.0f;
      int estStr = std::clamp((int)std::round(avgStr + cautionBias), 2, 7);
      for (int i = 0; i < 2; i++) {
        Army syn;
        syn.owner = owner;
        syn.x = city->x;
        syn.y = city->y;
        syn.type = 2;
        syn.strength = estStr;
        synthPool.push_back(syn);
        unseenFallbackDefenders.push_back(&synthPool.back());
      }
      float unseenFrac = (float)unseenEnemyTiles / 4.0f;
      est.estimatedDefenderCount = (int)std::round(2.0f * unseenFrac);
      est.estimatedDefenderStrength = 2.0f * (float)estStr * unseenFrac;
    }
  }

  if (defenders.empty() && unseenFallbackDefenders.empty()) {
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
    const std::vector<Army*>& simDefs =
        !defenders.empty()
            ? defenders
            : ((i * 4 < unseenEnemyTiles * SIMS) ? unseenFallbackDefenders : defenders);
    if (simDefs.empty()) {
      wins++;
      totalSurv += (int)attackers.size();
      continue;
    }
    Battle res = combat::resolve(g, attackers, simDefs, tx, ty);
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
