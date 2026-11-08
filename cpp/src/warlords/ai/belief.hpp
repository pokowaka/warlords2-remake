/**
 * @file
 * @brief Fair-information observation filter and per-side belief memory.
 *
 * Holds the AI to what a human player could know under the Hidden Map and
 * View Enemies settings. Remembers the enemy stacks it saw in battles, makes
 * up estimated defenders for stacks it has not inspected, finds the edges of
 * the fog of war worth exploring and works out how threatened each city is.
 */
#pragma once

#include <array>
#include <map>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

/** @brief What a side last saw of an enemy stack on one tile. */
struct ObservedStack {
    int x = kNone, y = kNone;          ///< The tile.
    int owner = kNone;                 ///< The side the stack belongs to.
    int visible_count = 0;             ///< How many armies the stack shows.
    int top_type = 0;                  ///< Army type shown on top of the stack.
    int last_inspected_turn = kNone;   ///< Turn its make-up was last seen in full.
    std::vector<int> known_types;      ///< Army types seen, when inspected.
    std::vector<int> known_strengths;  ///< Their strengths, in the same order.
};

/** @brief A seen tile next to unseen ones: a place worth exploring from. */
struct FrontierCluster {
    int x = 0, y = 0;             ///< The anchor tile of the cluster.
    int unseen_neighbours = 0;    ///< Unseen tiles around the anchor tile.
    int region_unseen_count = 0;  ///< Unseen tiles around all tiles merged into it.
    bool is_water = false;        ///< Water or shore, reached by boat.
    bool on_road = false;         ///< The anchor is on a road.
};

/** @brief Everything a side believes about the world, rebuilt each turn. */
struct SideBelief {
    int updated_turn = 0;                         ///< The turn the belief was last rebuilt.
    float frontier_ratio = 0.0F;                  ///< Share of the playing area still unseen, 0..1.
    std::map<int, ObservedStack> stacks_by_tile;  ///< Enemy stacks seen, by tile key.
    /// Running average of each side's army strength, as seen.
    std::array<float, 8> seen_enemy_avg_strength{4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F};
    std::vector<City*> visible_cities;             ///< Cities the side can see.
    std::vector<Site*> visible_sites;              ///< Ruins and temples it can see.
    std::vector<Item*> visible_ground_items;       ///< Items lying on seen tiles.
    std::vector<FrontierCluster> fog_frontiers;    ///< Where to explore, nearest first.
    std::vector<float> city_threat1_turn;          ///< Enemy strength a turn away, by city.
    std::vector<float> city_threat2_turn;          ///< Enemy strength two turns away, by city.
    std::vector<float> city_nearest_enemy_dist;    ///< Distance to the nearest enemy, by city.
    std::array<float, 8> my_border_strength{};     ///< Own strength near each side's cities.
    std::array<float, 8> their_border_strength{};  ///< Each side's strength near ours.
    std::array<int, 8> visible_cities_by_side{};   ///< Visible cities, by owner.
};

namespace ai::belief {

/** @brief The predicted outcome of an attack, from what the attacker can see. */
struct CombatEstimate {
    float win_prob = 1.0F;                     ///< Chance of winning, 0..1.
    float expected_survivors = 0.0F;           ///< Attackers expected to survive.
    float expected_enemy_losses = 0.0F;        ///< Defenders expected to die.
    int estimated_defender_count = 0;          ///< Defenders believed to be there.
    float estimated_defender_strength = 0.0F;  ///< Their summed strength.
    float seen_tile_fraction = 1.0F;           ///< Share of the target's tiles seen.
};

/** @brief Gives the side a SideBelief if it has none yet, and returns it. */
SideBelief& Ensure(Side& side);

/**
 * @brief Whether the tile is visible to the side under the current rules,
 * including allied shared vision when Hidden Map is on.
 */
bool IsTileVisible(const Game& game, int side_index, int x, int y);

/**
 * @brief Whether any of the city's 2x2 tiles is visible to the side, or the
 * city is the target of the side's quest.
 */
bool IsCityVisible(const Game& game, int side_index, const City& city);

/** @brief Whether the site's tile is visible to the side, or a sage or quest revealed it. */
bool IsSiteVisible(const Game& game, int side_index, const Site& site);

/**
 * @brief Rebuilds the side's belief for the current turn: what it sees,
 * threat maps, border strengths and fog-of-war frontiers.
 */
void Update(Game& game, Side& side);

/** @brief Records the make-up of the enemy stack a side has just fought at (x, y). */
void RecordBattleObservation(Game& game, int observer_side, int x, int y);

/**
 * @brief Estimates the outcome of attacking a tile from what a human could see.
 *
 * When View Enemies is off, defenders that have not been inspected are made
 * up from what the side has seen of their owner. Simulates a few battles
 * and restores the game's random number generator afterwards, so the game
 * itself is not changed.
 */
CombatEstimate EstimateCombat(Game& game, const Side& side, const std::vector<Army*>& attackers,
                              int target_x, int target_y, float caution_bias = 0.0F);

/**
 * @brief Answers belief's per-tile army queries from an index built once.
 *
 * While alive, ArmiesAt() reads an index built from `game` (in the same
 * order as game::ArmiesAt) instead of scanning every army per tile. Only
 * valid while no army moves: open it around read-only work such as scoring
 * a stack's options, never across a walk or a battle. Scopes nest.
 */
class ScopedTileIndex {
  public:
    explicit ScopedTileIndex(const Game& game);
    ~ScopedTileIndex();
    ScopedTileIndex(const ScopedTileIndex&) = delete;
    ScopedTileIndex& operator=(const ScopedTileIndex&) = delete;
    /** @brief The armies on a tile, newest first; empty off the map. */
    const std::vector<Army*>& At(int x, int y) const;
    /** @brief The game the index was built from. */
    const Game& GetGame() const { return game_; }

  private:
    const Game& game_;                         ///< The game the index was built from.
    std::vector<std::vector<Army*>> by_tile_;  ///< Armies by tile, row by row.
    const ScopedTileIndex* prev_;              ///< The enclosing index, if any.
};

/** @brief game::ArmiesAt, answered by the innermost ScopedTileIndex for `game` when one is open. */
std::vector<Army*> ArmiesAt(const Game& game, int x, int y);

}  // namespace ai::belief
}  // namespace w2
