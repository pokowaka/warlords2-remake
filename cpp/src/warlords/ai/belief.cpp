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

bool isTileVisible(const Game& g, int side_index, int x, int y) {
    return game::seen(g, side_index, x, y);
}

bool isCityVisible(const Game& g, int side_index, const City& city) {
    if (city.razed) return false;
    if (g.map->options.hiddenMap == 0) return true;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            if (game::seen(g, side_index, city.x + dx, city.y + dy)) return true;
        }
    }
    const Side* s = g.map->side(side_index);
    if (s && s->quest && s->quest->city == &city) return true;
    return false;
}

bool isSiteVisible(const Game& g, int side_index, const Site& site) {
    if (game::seen(g, side_index, site.x, site.y)) return true;
    const Side* s = g.map->side(side_index);
    return s && site::shownTo(site, *s);
}

namespace {
thread_local const ScopedTileIndex* active_index = nullptr;
}

ScopedTileIndex::ScopedTileIndex(const Game& g) : g_(g), prev_(active_index) {
    byTile_.resize(static_cast<size_t>(g.map->width) * g.map->height);
    // newest army first, as game::armiesAt lists them
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->transit || a->x < 0 || a->y < 0 || a->x >= g.map->width || a->y >= g.map->height)
            continue;
        byTile_[static_cast<size_t>(a->y) * g.map->width + a->x].push_back(a);
    }
    active_index = this;
}

ScopedTileIndex::~ScopedTileIndex() {
    active_index = prev_;
}

const std::vector<Army*>& ScopedTileIndex::at(int x, int y) const {
    static const std::vector<Army*> kNone;
    if (x < 0 || y < 0 || x >= g_.map->width || y >= g_.map->height) return kNone;
    return byTile_[static_cast<size_t>(y) * g_.map->width + x];
}

std::vector<Army*> armiesAt(const Game& g, int x, int y) {
    if (active_index && &active_index->game() == &g) return active_index->at(x, y);
    return game::armiesAt(g, x, y);
}

void recordBattleObservation(Game& g, int observer_side, int x, int y) {
    if (observer_side < 0 || observer_side >= 8) return;
    Side* side = g.map->side(observer_side);
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
        if (armies.empty() || armies[0]->owner == observer_side) {
            b.stacksByTile.erase(key);
            continue;
        }
        ObservedStack obs;
        obs.x = tx;
        obs.y = ty;
        obs.owner = armies[0]->owner;
        obs.visibleCount = static_cast<int>(armies.size());
        obs.topType = armies[0]->type;
        obs.lastInspectedTurn = g.turn;
        float sum_str = 0.0F;
        for (Army* a : armies) {
            obs.knownTypes.push_back(a->type);
            obs.knownStrengths.push_back(a->strength);
            sum_str += static_cast<float>(a->strength);
        }
        if (obs.owner >= 0 && obs.owner < 8 && !armies.empty()) {
            float avg = sum_str / static_cast<float>(armies.size());
            b.seenEnemyAvgStrength[obs.owner] =
                    0.7F * b.seenEnemyAvgStrength[obs.owner] + 0.3F * avg;
        }
        b.stacksByTile[key] = std::move(obs);
    }
}

void update(Game& g, Side& side) {
    SideBelief& b = ensure(side);
    ScopedTileIndex tile_index(g);  // nothing moves while the belief is rebuilt
    b.updatedTurn = g.turn;
    int me = side.index;
    int w = g.map->width, h = g.map->height;

    b.visibleCities.clear();
    b.visibleSites.clear();
    b.visibleGroundItems.clear();
    b.fogFrontiers.clear();
    b.visibleCitiesBySide.fill(0);
    b.myBorderStrength.fill(0.0F);
    b.theirBorderStrength.fill(0.0F);

    // Clean stale tile observations and scan visible enemy stacks
    std::map<int, ObservedStack> refreshed;
    int unseen_total = 0;
    std::vector<FrontierCluster> raw_frontiers;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!isTileVisible(g, me, x, y)) {
                unseen_total++;
                continue;
            }
            int key = y * w + x;
            auto armies = armiesAt(g, x, y);
            if (!armies.empty() && armies[0]->owner != me && armies[0]->owner != NONE) {
                int owner = armies[0]->owner;
                bool tower = game::towerAt(g, x, y);
                bool full_view = g.map->options.viewEnemies != 0;
                ObservedStack obs;
                obs.x = x;
                obs.y = y;
                obs.owner = owner;
                obs.topType = armies[0]->type;
                obs.visibleCount = (tower && !full_view) ? 4 : static_cast<int>(armies.size());

                const ArmyType* top_at = g.types.byId(obs.topType);
                if (top_at && owner >= 0 && owner < 8) {
                    b.seenEnemyAvgStrength[owner] =
                            0.9F * b.seenEnemyAvgStrength[owner] +
                            0.1F * static_cast<float>(std::max(2, top_at->strength));
                }

                auto prev_it = b.stacksByTile.find(key);
                if (full_view) {
                    obs.lastInspectedTurn = g.turn;
                    for (Army* a : armies) {
                        obs.knownTypes.push_back(a->type);
                        obs.knownStrengths.push_back(a->strength);
                    }
                } else if (prev_it != b.stacksByTile.end() && prev_it->second.owner == owner &&
                           prev_it->second.visibleCount == static_cast<int>(armies.size()) &&
                           g.turn - prev_it->second.lastInspectedTurn <= 2) {
                    obs.lastInspectedTurn = prev_it->second.lastInspectedTurn;
                    obs.knownTypes = prev_it->second.knownTypes;
                    obs.knownStrengths = prev_it->second.knownStrengths;
                }
                refreshed[key] = std::move(obs);
            }

            // Record visible land and coastal/sea tiles bordering unseen fog
            if (g.map->options.hiddenMap != 0) {
                int terr = g.map->terrainType[g.map->tiles[key] & 0xff];
                if (terr != move::MOUNTAINS) {
                    bool is_water = (terr == move::WATER || terr == move::SHORE);
                    bool is_road = !is_water && (scn::roadAt(*g.map, x, y) % 0x20 != 0);
                    int adj_unseen = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < w && ny < h &&
                                !isTileVisible(g, me, nx, ny)) {
                                adj_unseen++;
                            }
                        }
                    }
                    if (adj_unseen >= 1) {
                        raw_frontiers.push_back(
                                FrontierCluster{x, y, adj_unseen, adj_unseen, is_water, is_road});
                    }
                }
            }
        }
    }
    b.stacksByTile = std::move(refreshed);
    int active_area = std::max(
            1, (g.map->activeX1 - g.map->activeX0 + 1) * (g.map->activeY1 - g.map->activeY0 + 1));
    b.frontierRatio =
            std::min(1.0F, static_cast<float>(unseen_total) / static_cast<float>(active_area));

    // Sort rawFrontiers by proximity to our own cities/armies (preferring road tiles
    // that connect cities) so local expansion frontiers are represented in top clusters.
    std::vector<std::pair<int, int>> my_anchors;
    for (const auto& c : g.map->cities) {
        if (!c.razed && c.ownerIndex == me) my_anchors.emplace_back(c.x, c.y);
    }
    for (const Army* a : g.armies) {
        if (a->owner == me && a->x != NONE && !a->transit) my_anchors.emplace_back(a->x, a->y);
    }
    std::sort(raw_frontiers.begin(), raw_frontiers.end(),
              [&](const FrontierCluster& fa, const FrontierCluster& fb) {
                  int da = 999, db = 999;
                  for (const auto& [ax, ay] : my_anchors) {
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
    int land_count = 0, water_count = 0;
    for (const auto& fc : raw_frontiers) {
        bool merged = false;
        for (auto& existing : b.fogFrontiers) {
            if (existing.isWater == fc.isWater &&
                move::distance(fc.x, fc.y, existing.x, existing.y) < 3) {
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
            if (!fc.isWater && land_count < 48) {
                b.fogFrontiers.push_back(fc);
                land_count++;
            } else if (fc.isWater && water_count < 24) {
                b.fogFrontiers.push_back(fc);
                water_count++;
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
    size_t n_cities = g.map->cities.size();
    b.cityThreat1Turn.assign(n_cities, 0.0F);
    b.cityThreat2Turn.assign(n_cities, 0.0F);
    b.cityNearestEnemyDist.assign(n_cities, 200.0F);

    for (City* c : b.visibleCities) {
        float min_dist = 200.0F;
        for (City* other : b.visibleCities) {
            if (other->ownerIndex >= 0 && other->ownerIndex != me && !other->razed &&
                !diplomacy::isAllied(g, me, other->ownerIndex)) {
                float d = static_cast<float>(move::distance(c->x, c->y, other->x, other->y));
                if (d < min_dist) min_dist = d;
            }
        }
        for (const auto& [tile_key, obs] : b.stacksByTile) {
            if (obs.owner >= 0 && obs.owner != me && !diplomacy::isAllied(g, me, obs.owner)) {
                float d = static_cast<float>(move::distance(c->x, c->y, obs.x, obs.y));
                if (d < min_dist) min_dist = d;
            }
        }
        b.cityNearestEnemyDist[c->index] = min_dist;

        float t1 = 0.0F, t2 = 0.0F;
        for (const auto& [tile_key, obs] : b.stacksByTile) {
            if (obs.owner == me || obs.owner == NONE || diplomacy::isAllied(g, me, obs.owner))
                continue;
            int d = move::distance(c->x, c->y, obs.x, obs.y);
            const ArmyType* top_at = g.types.byId(obs.topType);
            int est_move = top_at ? std::max(10, top_at->move) : 12;
            int reach1 = std::max(8, (est_move * 3) / 4);
            int reach2 = reach1 * 2;
            float est_str = 0.0F;
            if (!obs.knownStrengths.empty()) {
                for (int s : obs.knownStrengths) est_str += static_cast<float>(s);
            } else {
                float top_str = top_at ? static_cast<float>(top_at->strength) : 4.0F;
                float rest_str = (obs.owner >= 0 && obs.owner < 8)
                                         ? b.seenEnemyAvgStrength[obs.owner]
                                         : 4.0F;
                est_str =
                        top_str + static_cast<float>(std::max(0, obs.visibleCount - 1)) * rest_str;
            }
            if (d <= reach1) t1 += est_str;
            if (d <= reach2) t2 += est_str;

            if (c->ownerIndex == me && d <= 18 && obs.owner >= 0 && obs.owner < 8) {
                b.theirBorderStrength[obs.owner] += est_str;
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
                    b.myBorderStrength[c->ownerIndex] += static_cast<float>(a->strength);
                }
            }
        }
    }
}

CombatEstimate estimateCombat(Game& g, const Side& side, const std::vector<Army*>& attackers,
                              int tx, int ty, float caution_bias) {
    CombatEstimate est;
    if (attackers.empty()) {
        est.winProb = 0.0F;
        return est;
    }
    int me = side.index;
    City* city = game::cityAt(g, tx, ty);
    bool target_visible = city ? isCityVisible(g, me, *city) : isTileVisible(g, me, tx, ty);
    if (!target_visible) {
        est.winProb = 0.85F;
        est.expectedSurvivors = static_cast<float>(attackers.size()) * 0.85F;
        return est;
    }

    const SideBelief& b = side.belief ? *side.belief : ensure(const_cast<Side&>(side));

    // Gather visible defender tiles (4 tiles if city, 1 tile otherwise)
    std::vector<std::pair<int, int>> def_tiles;
    if (city) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) def_tiles.emplace_back(city->x + dx, city->y + dy);
    } else {
        def_tiles.emplace_back(tx, ty);
    }

    bool full_view = g.map->options.viewEnemies != 0;
    std::vector<Army> synth_pool;
    synth_pool.reserve(32);
    std::vector<Army*> defenders;

    int seen_tiles = 0;
    for (auto [dx, dy] : def_tiles) {
        if (!isTileVisible(g, me, dx, dy)) continue;
        seen_tiles++;
        int key = dy * g.map->width + dx;
        auto actual = armiesAt(g, dx, dy);
        if (actual.empty() || actual[0]->owner == me) continue;

        if (full_view) {
            for (Army* a : actual) {
                defenders.push_back(a);
                est.estimatedDefenderStrength += static_cast<float>(a->strength);
            }
            est.estimatedDefenderCount += static_cast<int>(actual.size());
            continue;
        }

        auto it = b.stacksByTile.find(key);
        int owner = actual[0]->owner;
        int top_type = actual[0]->type;
        bool tower = game::towerAt(g, dx, dy);
        int count = tower ? 4 : static_cast<int>(actual.size());
        if (it != b.stacksByTile.end() && !it->second.knownTypes.empty()) {
            for (size_t i = 0; i < it->second.knownTypes.size(); i++) {
                Army syn;
                syn.owner = owner;
                syn.x = dx;
                syn.y = dy;
                syn.type = it->second.knownTypes[i];
                syn.strength = it->second.knownStrengths[i];
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimatedDefenderStrength += static_cast<float>(syn.strength);
            }
            est.estimatedDefenderCount += static_cast<int>(it->second.knownTypes.size());
        } else {
            float avg_str = (owner >= 0 && owner < 8) ? b.seenEnemyAvgStrength[owner] : 3.5F;
            int est_under_str =
                    std::clamp(static_cast<int>(std::round(avg_str + caution_bias)), 2, 8);
            for (int i = 0; i < count; i++) {
                Army syn;
                syn.owner = owner;
                syn.x = dx;
                syn.y = dy;
                if (i == 0) {
                    syn.type = top_type;
                    const ArmyType* at = g.types.byId(top_type);
                    syn.strength = at ? at->strength : est_under_str;
                } else {
                    syn.type = 2;
                    syn.strength = est_under_str;
                }
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimatedDefenderStrength += static_cast<float>(syn.strength);
            }
            est.estimatedDefenderCount += count;
        }
    }

    est.seenTileFraction = def_tiles.empty() ? 1.0F
                                             : static_cast<float>(seen_tiles) /
                                                       static_cast<float>(def_tiles.size());

    // Calibrated partial-visibility belief when some tiles of a 2x2 city remain in fog
    // and no defenders were observed on the visible tiles:
    int unseen_enemy_tiles = 0;
    std::vector<Army*> unseen_fallback_defenders;
    if (defenders.empty() && city && city->ownerIndex != me && !city->razed && seen_tiles < 4) {
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
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimatedDefenderCount = 1;
                est.estimatedDefenderStrength = static_cast<float>(syn.strength);
            }
        } else {
            // Enemy city with unseen corner(s): sample unseen garrison occupancy proportionally
            // to (4 - seenTiles) / 4 across the Monte Carlo rollouts rather than assuming a
            // guaranteed 3-stack
            unseen_enemy_tiles = 4 - seen_tiles;
            int owner = city->ownerIndex;
            float avg_str = (owner >= 0 && owner < 8) ? b.seenEnemyAvgStrength[owner] : 4.0F;
            int est_str = std::clamp(static_cast<int>(std::round(avg_str + caution_bias)), 2, 7);
            for (int i = 0; i < 2; i++) {
                Army syn;
                syn.owner = owner;
                syn.x = city->x;
                syn.y = city->y;
                syn.type = 2;
                syn.strength = est_str;
                synth_pool.push_back(syn);
                unseen_fallback_defenders.push_back(&synth_pool.back());
            }
            float unseen_frac = static_cast<float>(unseen_enemy_tiles) / 4.0F;
            est.estimatedDefenderCount = static_cast<int>(std::round(2.0F * unseen_frac));
            est.estimatedDefenderStrength = 2.0F * static_cast<float>(est_str) * unseen_frac;
        }
    }

    if (defenders.empty() && unseen_fallback_defenders.empty()) {
        est.winProb = 1.0F;
        est.expectedSurvivors = static_cast<float>(attackers.size());
        return est;
    }

    // Save RNG state so combat estimation is 100% side-effect free
    uint32_t saved_rng = g.rng.state;
    g.rng.state = saved_rng ^ static_cast<uint32_t>(tx * 73856093 ^ ty * 19349663 ^
                                                    static_cast<int>(attackers.size()) * 83492791);
    if (g.rng.state == 0) g.rng.state = 1;

    constexpr int kSims = 8;
    int wins = 0;
    int total_surv = 0;
    int total_enemy_dead = 0;
    for (int i = 0; i < kSims; i++) {
        const std::vector<Army*>& sim_defs =
                !defenders.empty()
                        ? defenders
                        : ((i * 4 < unseen_enemy_tiles * kSims) ? unseen_fallback_defenders
                                                                : defenders);
        if (sim_defs.empty()) {
            wins++;
            total_surv += static_cast<int>(attackers.size());
            continue;
        }
        Battle res = combat::resolve(g, attackers, sim_defs, tx, ty);
        if (res.won) wins++;
        total_surv += static_cast<int>(res.attackers.size());
        total_enemy_dead += static_cast<int>(res.deadDefenders.size());
    }

    g.rng.state = saved_rng;
    est.winProb = static_cast<float>(wins) / static_cast<float>(kSims);
    est.expectedSurvivors = static_cast<float>(total_surv) / static_cast<float>(kSims);
    est.expectedEnemyLosses = static_cast<float>(total_enemy_dead) / static_cast<float>(kSims);
    return est;
}

}  // namespace w2::ai::belief
