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

SideBelief& Ensure(Side& side) {
    if (!side.belief) side.belief = std::make_shared<SideBelief>();
    return *side.belief;
}

bool IsTileVisible(const Game& g, int side_index, int x, int y) {
    return game::Seen(g, side_index, x, y);
}

bool IsCityVisible(const Game& g, int side_index, const City& city) {
    if (city.razed) return false;
    if (g.map->options.hidden_map == 0) return true;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            if (game::Seen(g, side_index, city.x + dx, city.y + dy)) return true;
        }
    }
    const Side* s = g.map->Side(side_index);
    if (s && s->quest && s->quest->city == &city) return true;
    return false;
}

bool IsSiteVisible(const Game& g, int side_index, const Site& site) {
    if (game::Seen(g, side_index, site.x, site.y)) return true;
    const Side* s = g.map->Side(side_index);
    return s && site::ShownTo(site, *s);
}

namespace {
thread_local const ScopedTileIndex* active_index = nullptr;
}

ScopedTileIndex::ScopedTileIndex(const Game& g) : g_(g), prev_(active_index) {
    by_tile_.resize(static_cast<size_t>(g.map->width) * g.map->height);
    // newest army first, as game::armiesAt lists them
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->transit || a->x < 0 || a->y < 0 || a->x >= g.map->width || a->y >= g.map->height)
            continue;
        by_tile_[static_cast<size_t>(a->y) * g.map->width + a->x].push_back(a);
    }
    active_index = this;
}

ScopedTileIndex::~ScopedTileIndex() {
    active_index = prev_;
}

const std::vector<Army*>& ScopedTileIndex::At(int x, int y) const {
    static const std::vector<Army*> kNone;
    if (x < 0 || y < 0 || x >= g_.map->width || y >= g_.map->height) return kNone;
    return by_tile_[static_cast<size_t>(y) * g_.map->width + x];
}

std::vector<Army*> ArmiesAt(const Game& g, int x, int y) {
    if (active_index && &active_index->GetGame() == &g) return active_index->At(x, y);
    return game::ArmiesAt(g, x, y);
}

void RecordBattleObservation(Game& g, int observer_side, int x, int y) {
    if (observer_side < 0 || observer_side >= 8) return;
    Side* side = g.map->Side(observer_side);
    if (!side) return;
    SideBelief& b = Ensure(*side);

    std::vector<std::pair<int, int>> tiles;
    if (City* c = game::CityAt(g, x, y)) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) tiles.emplace_back(c->x + dx, c->y + dy);
    } else {
        tiles.emplace_back(x, y);
    }

    for (auto [tx, ty] : tiles) {
        int key = ty * g.map->width + tx;
        auto armies = game::ArmiesAt(g, tx, ty);
        if (armies.empty() || armies[0]->owner == observer_side) {
            b.stacks_by_tile.erase(key);
            continue;
        }
        ObservedStack obs;
        obs.x = tx;
        obs.y = ty;
        obs.owner = armies[0]->owner;
        obs.visible_count = static_cast<int>(armies.size());
        obs.top_type = armies[0]->type;
        obs.last_inspected_turn = g.turn;
        float sum_str = 0.0F;
        for (Army* a : armies) {
            obs.known_types.push_back(a->type);
            obs.known_strengths.push_back(a->strength);
            sum_str += static_cast<float>(a->strength);
        }
        if (obs.owner >= 0 && obs.owner < 8 && !armies.empty()) {
            float avg = sum_str / static_cast<float>(armies.size());
            b.seen_enemy_avg_strength[obs.owner] =
                    0.7F * b.seen_enemy_avg_strength[obs.owner] + 0.3F * avg;
        }
        b.stacks_by_tile[key] = std::move(obs);
    }
}

void Update(Game& g, Side& side) {
    SideBelief& b = Ensure(side);
    ScopedTileIndex tile_index(g);  // nothing moves while the belief is rebuilt
    b.updated_turn = g.turn;
    int me = side.index;
    int w = g.map->width, h = g.map->height;

    b.visible_cities.clear();
    b.visible_sites.clear();
    b.visible_ground_items.clear();
    b.fog_frontiers.clear();
    b.visible_cities_by_side.fill(0);
    b.my_border_strength.fill(0.0F);
    b.their_border_strength.fill(0.0F);

    // Clean stale tile observations and scan visible enemy stacks
    std::map<int, ObservedStack> refreshed;
    int unseen_total = 0;
    std::vector<FrontierCluster> raw_frontiers;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!IsTileVisible(g, me, x, y)) {
                unseen_total++;
                continue;
            }
            int key = y * w + x;
            auto armies = ArmiesAt(g, x, y);
            if (!armies.empty() && armies[0]->owner != me && armies[0]->owner != kNone) {
                int owner = armies[0]->owner;
                bool tower = game::TowerAt(g, x, y);
                bool full_view = g.map->options.view_enemies != 0;
                ObservedStack obs;
                obs.x = x;
                obs.y = y;
                obs.owner = owner;
                obs.top_type = armies[0]->type;
                obs.visible_count = (tower && !full_view) ? 4 : static_cast<int>(armies.size());

                const ArmyType* top_at = g.types.ById(obs.top_type);
                if (top_at && owner >= 0 && owner < 8) {
                    b.seen_enemy_avg_strength[owner] =
                            0.9F * b.seen_enemy_avg_strength[owner] +
                            0.1F * static_cast<float>(std::max(2, top_at->strength));
                }

                auto prev_it = b.stacks_by_tile.find(key);
                if (full_view) {
                    obs.last_inspected_turn = g.turn;
                    for (Army* a : armies) {
                        obs.known_types.push_back(a->type);
                        obs.known_strengths.push_back(a->strength);
                    }
                } else if (prev_it != b.stacks_by_tile.end() && prev_it->second.owner == owner &&
                           prev_it->second.visible_count == static_cast<int>(armies.size()) &&
                           g.turn - prev_it->second.last_inspected_turn <= 2) {
                    obs.last_inspected_turn = prev_it->second.last_inspected_turn;
                    obs.known_types = prev_it->second.known_types;
                    obs.known_strengths = prev_it->second.known_strengths;
                }
                refreshed[key] = std::move(obs);
            }

            // Record visible land and coastal/sea tiles bordering unseen fog
            if (g.map->options.hidden_map != 0) {
                int terr = g.map->terrain_type[g.map->tiles[key] & 0xff];
                if (terr != move::kMountains) {
                    bool is_water = (terr == move::kWater || terr == move::kShore);
                    bool is_road = !is_water && (scn::RoadAt(*g.map, x, y) % 0x20 != 0);
                    int adj_unseen = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < w && ny < h &&
                                !IsTileVisible(g, me, nx, ny)) {
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
    b.stacks_by_tile = std::move(refreshed);
    int active_area = std::max(1, (g.map->active_x1 - g.map->active_x0 + 1) *
                                          (g.map->active_y1 - g.map->active_y0 + 1));
    b.frontier_ratio =
            std::min(1.0F, static_cast<float>(unseen_total) / static_cast<float>(active_area));

    // Sort rawFrontiers by proximity to our own cities/armies (preferring road tiles
    // that connect cities) so local expansion frontiers are represented in top clusters.
    std::vector<std::pair<int, int>> my_anchors;
    for (const auto& c : g.map->cities) {
        if (!c.razed && c.owner_index == me) my_anchors.emplace_back(c.x, c.y);
    }
    for (const Army* a : g.armies) {
        if (a->owner == me && a->x != kNone && !a->transit) my_anchors.emplace_back(a->x, a->y);
    }
    std::sort(raw_frontiers.begin(), raw_frontiers.end(),
              [&](const FrontierCluster& fa, const FrontierCluster& fb) {
                  int da = 999, db = 999;
                  for (const auto& [ax, ay] : my_anchors) {
                      da = std::min(da, move::Distance(fa.x, fa.y, ax, ay));
                      db = std::min(db, move::Distance(fb.x, fb.y, ax, ay));
                  }
                  int sa = da - (fa.on_road ? 4 : 0);
                  int sb = db - (fb.on_road ? 4 : 0);
                  if (sa != sb) return sa < sb;
                  return fa.on_road > fb.on_road;
              });

    // Cluster land frontiers separately from water frontiers so coastal land does not suppress
    // amphibious sea lanes (and vice versa); snap cluster anchor onto road tiles when merged.
    int land_count = 0, water_count = 0;
    for (const auto& fc : raw_frontiers) {
        bool merged = false;
        for (auto& existing : b.fog_frontiers) {
            if (existing.is_water == fc.is_water &&
                move::Distance(fc.x, fc.y, existing.x, existing.y) < 3) {
                existing.region_unseen_count += fc.unseen_neighbours;
                if (fc.on_road && !existing.on_road) {
                    existing.x = fc.x;
                    existing.y = fc.y;
                    existing.on_road = true;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (!fc.is_water && land_count < 48) {
                b.fog_frontiers.push_back(fc);
                land_count++;
            } else if (fc.is_water && water_count < 24) {
                b.fog_frontiers.push_back(fc);
                water_count++;
            }
        }
    }

    for (auto& c : g.map->cities) {
        if (IsCityVisible(g, me, c)) {
            b.visible_cities.push_back(&c);
            if (c.owner_index >= 0 && c.owner_index < 8) {
                b.visible_cities_by_side[c.owner_index]++;
            }
        }
    }

    for (auto& s : g.map->sites) {
        if (IsSiteVisible(g, me, s)) b.visible_sites.push_back(&s);
    }

    for (auto& it : g.map->items) {
        if (it.status == 1 && it.x != kNone && !it.planted && IsTileVisible(g, me, it.x, it.y)) {
            b.visible_ground_items.push_back(&it);
        }
    }

    // Compute per-city threat and nearest visible rival distance using fair-information state only
    size_t n_cities = g.map->cities.size();
    b.city_threat1_turn.assign(n_cities, 0.0F);
    b.city_threat2_turn.assign(n_cities, 0.0F);
    b.city_nearest_enemy_dist.assign(n_cities, 200.0F);

    for (City* c : b.visible_cities) {
        float min_dist = 200.0F;
        for (City* other : b.visible_cities) {
            if (other->owner_index >= 0 && other->owner_index != me && !other->razed &&
                !diplomacy::IsAllied(g, me, other->owner_index)) {
                float d = static_cast<float>(move::Distance(c->x, c->y, other->x, other->y));
                if (d < min_dist) min_dist = d;
            }
        }
        for (const auto& [tile_key, obs] : b.stacks_by_tile) {
            if (obs.owner >= 0 && obs.owner != me && !diplomacy::IsAllied(g, me, obs.owner)) {
                float d = static_cast<float>(move::Distance(c->x, c->y, obs.x, obs.y));
                if (d < min_dist) min_dist = d;
            }
        }
        b.city_nearest_enemy_dist[c->index] = min_dist;

        float t1 = 0.0F, t2 = 0.0F;
        for (const auto& [tile_key, obs] : b.stacks_by_tile) {
            if (obs.owner == me || obs.owner == kNone || diplomacy::IsAllied(g, me, obs.owner))
                continue;
            int d = move::Distance(c->x, c->y, obs.x, obs.y);
            const ArmyType* top_at = g.types.ById(obs.top_type);
            int est_move = top_at ? std::max(10, top_at->move) : 12;
            int reach1 = std::max(8, (est_move * 3) / 4);
            int reach2 = reach1 * 2;
            float est_str = 0.0F;
            if (!obs.known_strengths.empty()) {
                for (int s : obs.known_strengths) est_str += static_cast<float>(s);
            } else {
                float top_str = top_at ? static_cast<float>(top_at->strength) : 4.0F;
                float rest_str = (obs.owner >= 0 && obs.owner < 8)
                                         ? b.seen_enemy_avg_strength[obs.owner]
                                         : 4.0F;
                est_str =
                        top_str + static_cast<float>(std::max(0, obs.visible_count - 1)) * rest_str;
            }
            if (d <= reach1) t1 += est_str;
            if (d <= reach2) t2 += est_str;

            if (c->owner_index == me && d <= 18 && obs.owner >= 0 && obs.owner < 8) {
                b.their_border_strength[obs.owner] += est_str;
            }
        }
        b.city_threat1_turn[c->index] = t1;
        b.city_threat2_turn[c->index] = t2;
    }

    // Compute our own border strength near each rival's visible cities
    for (Army* a : g.armies) {
        if (a->owner != me || a->x == kNone || a->transit) continue;
        for (City* c : b.visible_cities) {
            if (c->owner_index >= 0 && c->owner_index < 8 && c->owner_index != me) {
                if (move::Distance(a->x, a->y, c->x, c->y) <= 18) {
                    b.my_border_strength[c->owner_index] += static_cast<float>(a->strength);
                }
            }
        }
    }
}

CombatEstimate EstimateCombat(Game& g, const Side& side, const std::vector<Army*>& attackers,
                              int tx, int ty, float caution_bias) {
    CombatEstimate est;
    if (attackers.empty()) {
        est.win_prob = 0.0F;
        return est;
    }
    int me = side.index;
    City* city = game::CityAt(g, tx, ty);
    bool target_visible = city ? IsCityVisible(g, me, *city) : IsTileVisible(g, me, tx, ty);
    if (!target_visible) {
        est.win_prob = 0.85F;
        est.expected_survivors = static_cast<float>(attackers.size()) * 0.85F;
        return est;
    }

    const SideBelief& b = side.belief ? *side.belief : Ensure(const_cast<Side&>(side));

    // Gather visible defender tiles (4 tiles if city, 1 tile otherwise)
    std::vector<std::pair<int, int>> def_tiles;
    if (city) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) def_tiles.emplace_back(city->x + dx, city->y + dy);
    } else {
        def_tiles.emplace_back(tx, ty);
    }

    bool full_view = g.map->options.view_enemies != 0;
    std::vector<Army> synth_pool;
    synth_pool.reserve(32);
    std::vector<Army*> defenders;

    int seen_tiles = 0;
    for (auto [dx, dy] : def_tiles) {
        if (!IsTileVisible(g, me, dx, dy)) continue;
        seen_tiles++;
        int key = dy * g.map->width + dx;
        auto actual = ArmiesAt(g, dx, dy);
        if (actual.empty() || actual[0]->owner == me) continue;

        if (full_view) {
            for (Army* a : actual) {
                defenders.push_back(a);
                est.estimated_defender_strength += static_cast<float>(a->strength);
            }
            est.estimated_defender_count += static_cast<int>(actual.size());
            continue;
        }

        auto it = b.stacks_by_tile.find(key);
        int owner = actual[0]->owner;
        int top_type = actual[0]->type;
        bool tower = game::TowerAt(g, dx, dy);
        int count = tower ? 4 : static_cast<int>(actual.size());
        if (it != b.stacks_by_tile.end() && !it->second.known_types.empty()) {
            for (size_t i = 0; i < it->second.known_types.size(); i++) {
                Army syn;
                syn.owner = owner;
                syn.x = dx;
                syn.y = dy;
                syn.type = it->second.known_types[i];
                syn.strength = it->second.known_strengths[i];
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimated_defender_strength += static_cast<float>(syn.strength);
            }
            est.estimated_defender_count += static_cast<int>(it->second.known_types.size());
        } else {
            float avg_str = (owner >= 0 && owner < 8) ? b.seen_enemy_avg_strength[owner] : 3.5F;
            int est_under_str =
                    std::clamp(static_cast<int>(std::round(avg_str + caution_bias)), 2, 8);
            for (int i = 0; i < count; i++) {
                Army syn;
                syn.owner = owner;
                syn.x = dx;
                syn.y = dy;
                if (i == 0) {
                    syn.type = top_type;
                    const ArmyType* at = g.types.ById(top_type);
                    syn.strength = at ? at->strength : est_under_str;
                } else {
                    syn.type = 2;
                    syn.strength = est_under_str;
                }
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimated_defender_strength += static_cast<float>(syn.strength);
            }
            est.estimated_defender_count += count;
        }
    }

    est.seen_tile_fraction = def_tiles.empty() ? 1.0F
                                               : static_cast<float>(seen_tiles) /
                                                         static_cast<float>(def_tiles.size());

    // Calibrated partial-visibility belief when some tiles of a 2x2 city remain in fog
    // and no defenders were observed on the visible tiles:
    int unseen_enemy_tiles = 0;
    std::vector<Army*> unseen_fallback_defenders;
    if (defenders.empty() && city && city->owner_index != me && !city->razed && seen_tiles < 4) {
        if (city->owner_index == kNone) {
            // Neutral garrisons strictly occupy (city->x, city->y). Only when (city->x, city->y)
            // itself is unseen can a neutral garrison be hidden there.
            if (!IsTileVisible(g, me, city->x, city->y)) {
                Army syn;
                syn.owner = kNone;
                syn.x = city->x;
                syn.y = city->y;
                syn.type = (g.map->options.neutral_cities <= 0) ? 0 : 2;
                syn.strength = (g.map->options.neutral_cities <= 0) ? 1 : 4;
                synth_pool.push_back(syn);
                defenders.push_back(&synth_pool.back());
                est.estimated_defender_count = 1;
                est.estimated_defender_strength = static_cast<float>(syn.strength);
            }
        } else {
            // Enemy city with unseen corner(s): sample unseen garrison occupancy proportionally
            // to (4 - seenTiles) / 4 across the Monte Carlo rollouts rather than assuming a
            // guaranteed 3-stack
            unseen_enemy_tiles = 4 - seen_tiles;
            int owner = city->owner_index;
            float avg_str = (owner >= 0 && owner < 8) ? b.seen_enemy_avg_strength[owner] : 4.0F;
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
            est.estimated_defender_count = static_cast<int>(std::round(2.0F * unseen_frac));
            est.estimated_defender_strength = 2.0F * static_cast<float>(est_str) * unseen_frac;
        }
    }

    if (defenders.empty() && unseen_fallback_defenders.empty()) {
        est.win_prob = 1.0F;
        est.expected_survivors = static_cast<float>(attackers.size());
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
        Battle res = combat::Resolve(g, attackers, sim_defs, tx, ty);
        if (res.won) wins++;
        total_surv += static_cast<int>(res.attackers.size());
        total_enemy_dead += static_cast<int>(res.dead_defenders.size());
    }

    g.rng.state = saved_rng;
    est.win_prob = static_cast<float>(wins) / static_cast<float>(kSims);
    est.expected_survivors = static_cast<float>(total_surv) / static_cast<float>(kSims);
    est.expected_enemy_losses = static_cast<float>(total_enemy_dead) / static_cast<float>(kSims);
    return est;
}

}  // namespace w2::ai::belief
