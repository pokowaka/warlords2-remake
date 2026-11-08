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
    int x = kNone, y = kNone;
    int owner = kNone;
    int visible_count = 0;
    int top_type = 0;
    int last_inspected_turn = kNone;
    std::vector<int> known_types;
    std::vector<int> known_strengths;
};

struct FrontierCluster {
    int x = 0, y = 0;
    int unseen_neighbours = 0;
    int region_unseen_count = 0;
    bool is_water = false;
    bool on_road = false;
};

struct SideBelief {
    int updated_turn = 0;
    float frontier_ratio = 0.0F;
    std::map<int, ObservedStack> stacks_by_tile;
    std::array<float, 8> seen_enemy_avg_strength{4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F};
    std::vector<City*> visible_cities;
    std::vector<Site*> visible_sites;
    std::vector<Item*> visible_ground_items;
    std::vector<FrontierCluster> fog_frontiers;
    std::vector<float> city_threat1_turn;
    std::vector<float> city_threat2_turn;
    std::vector<float> city_nearest_enemy_dist;
    std::array<float, 8> my_border_strength{};
    std::array<float, 8> their_border_strength{};
    std::array<int, 8> visible_cities_by_side{};
};

namespace ai::belief {

struct CombatEstimate {
    float win_prob = 1.0F;
    float expected_survivors = 0.0F;
    float expected_enemy_losses = 0.0F;
    int estimated_defender_count = 0;
    float estimated_defender_strength = 0.0F;
    float seen_tile_fraction = 1.0F;
};

/**
 * Ensures the side has an initialized SideBelief instance and returns it.
 */
SideBelief& Ensure(Side& side);

/**
 * Returns true if tile (x, y) is visible to sideIndex under current game rules
 * (including Allied shared vision when Hidden Map is active).
 */
bool IsTileVisible(const Game& g, int side_index, int x, int y);

/**
 * Returns true if any of the city's 2x2 footprint tiles are visible to sideIndex,
 * or if the city is the target of sideIndex's active quest.
 */
bool IsCityVisible(const Game& g, int side_index, const City& city);

/**
 * Returns true if the site's tile is visible to sideIndex or revealed by a sage/quest.
 */
bool IsSiteVisible(const Game& g, int side_index, const Site& site);

/**
 * Rebuilds the side's fair-information observation state, threat heatmaps,
 * border strength ratios, and fog-of-war frontier clusters for the current turn.
 */
void Update(Game& g, Side& side);

/**
 * Records the revealed composition of an enemy stack after a battle at (x, y).
 */
void RecordBattleObservation(Game& g, int observer_side, int x, int y);

/**
 * Estimates combat outcome for attacking (tx, ty) using strictly human-visible
 * information (synthetic defender estimation when View Enemies is off) without
 * mutating g.rng.
 */
CombatEstimate EstimateCombat(Game& g, const Side& side, const std::vector<Army*>& attackers,
                              int tx, int ty, float caution_bias = 0.0F);

/**
 * While alive, answers belief's per-tile army queries from an index built once
 * from `g` (same order as game::armiesAt) instead of scanning every army per
 * tile. Only valid while no army moves: open it around read-only work such as
 * scoring a stack's options, never across a walk or a battle. Scopes nest.
 */
class ScopedTileIndex {
  public:
    explicit ScopedTileIndex(const Game& g);
    ~ScopedTileIndex();
    ScopedTileIndex(const ScopedTileIndex&) = delete;
    ScopedTileIndex& operator=(const ScopedTileIndex&) = delete;
    const std::vector<Army*>& At(int x, int y) const;
    const Game& GetGame() const { return g_; }

  private:
    const Game& g_;
    std::vector<std::vector<Army*>> by_tile_;
    const ScopedTileIndex* prev_;
};

/** game::armiesAt, answered by the innermost ScopedTileIndex for `g` when one is open. */
std::vector<Army*> ArmiesAt(const Game& g, int x, int y);

}  // namespace ai::belief
}  // namespace w2
