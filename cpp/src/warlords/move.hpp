/**
 * @file
 * @brief Movement: the cost grid, stack modes, pathfinding and walking a path.
 *
 * This follows the original game: it builds a one-byte-per-tile cost grid for
 * the moving side, runs a wavefront over it from the destination and walks
 * the resulting path. See docs/rules.md > Movement and > Moving a stack.
 */
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2::move {

/** @brief Terrain type ids, numbered as the scenario's own tile -> type table numbers them. */
constexpr int kRoad = 0, kBridge = 1, kWater = 2, kShore = 3;
constexpr int kForest = 4, kHills = 5, kMountains = 6, kPlain = 7;
constexpr int kMarsh = 8, kTower = 9, kCity = 10, kSite = 11;

/** @brief The display name of each terrain type, indexed by terrain id. */
extern const char* const kTerrainNames[12];

/**
 * @brief The moves it costs to enter each terrain type, indexed by terrain id;
 * 0 = impassable. See docs/rules.md > Terrain costs.
 */
constexpr int kCost[12] = {1, 1, 1, 2, 4, 6, 0, 2, 5, 2, 1, 2};

/**
 * @brief Cost-grid flags. A grid byte holds the cost in its low 3 bits and
 * these flags above: water, crossing (where a stack may change between land
 * and water), hills, forest and city.
 */
constexpr int kWaterF = 0x08, kCrossF = 0x10;
constexpr int kHillsF = 0x20, kForestF = 0x40, kCityF = 0x80;

/** @brief Stack modes, numbered as the original numbers them. */
constexpr int kBoat = 0, kLand = 1, kFlying = 2;

/** @brief Whether a cost-grid byte carries a flag. */
inline bool Has(int byte, int flag) {
    return (byte & flag) != 0;
}

constexpr int kWaterPenalty = 10;    ///< A land stack's route leaving open water.
constexpr int kWaterPenaltyTo = 20;  ///< ... when the whole move is aimed at water.
constexpr int kPastShore = 0x80;     ///< The cost of a step after going to sea or ashore.
constexpr int kMaxPath = 200;        ///< A path is at most 200 compass steps.

/**
 * @brief The compass directions as (dx, dy), 0 = north, clockwise (the order
 * the game stores paths in).
 */
constexpr int kDirs[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

/** @brief A tile the stack may never enter, in the search's distance grid. */
constexpr int kShut = 30001;

/** @brief A stack: the armies standing together on one tile. */
using Stack = std::vector<Army*>;

/** @brief How a stack moves, and what bonuses it carries. */
struct Mode {
    int mode = kLand;     ///< kBoat, kLand or kFlying.
    bool woods = false;   ///< Some army has the woods move bonus: forest costs 2.
    bool hills = false;   ///< Some army has the hills move bonus: hills cost 2.
    bool at_sea = false;  ///< The stack is at sea (land rules, no bonuses).
};

/**
 * @brief Works out how a stack moves from its armies. See docs/rules.md >
 * How a stack moves.
 */
Mode StackMode(const Game& game, const Stack& stack);

/** @brief Just the mode, for the many callers that want only it. */
inline int ModeOf(const Game& game, const Stack& stack) {
    return StackMode(game, stack).mode;
}

/**
 * @brief Whether a city is a port: does any tile bordering its footprint hold
 * a bridge, water or shore? Gives up the moment its walk would leave the map.
 */
bool IsPort(const Game& game, const City& city);

/**
 * @brief The cost grid for one side, cached on the game state; Invalidate()
 * drops it.
 */
std::vector<int>& Grid(Game& game, int side_index);

/** @brief Drops the cached grids. Call after a city changes hands. */
void Invalidate(Game& game);

/**
 * @brief The wavefront's weight for spreading from one tile to another, or
 * none if it cannot spread there. This picks the route only.
 */
std::optional<int> StepCost(int from_byte, int to_byte, int mode, bool woods, bool hills,
                            int penalty);

/** @brief The moves a walk spends stepping onto a tile. */
int WalkCost(int byte, int mode, bool woods, bool hills);

/** @brief Whether a land stack stepping onto this tile goes to sea (or comes ashore). */
bool CrossesShore(int byte, bool at_sea);

/** @brief The movement points a stack shares: the lowest of its armies'. */
int StackMoves(const Stack& stack);

/** @brief One step of a path. */
struct Step {
    int x = 0;     ///< The tile stepped onto.
    int y = 0;     ///< The tile stepped onto.
    int cost = 0;  ///< The moves the walk spends on it (kPastShore after the shoreline).
};

/** @brief A route, without the tile it starts on. */
using Path = std::vector<Step>;

/** @brief The route a stack would take to a tile and how much of it it can walk now. */
struct Preview {
    Path path;      ///< The whole route.
    int reach = 0;  ///< How many of its steps the stack's moves pay for now.
};

/** @brief The route a stack would take to (x, y), or none if there is none. */
std::optional<Preview> PreviewRoute(Game& game, const Stack& stack, int x, int y);

/** @brief The compass direction from one tile towards another, or -1 when they are the same. */
int Direction(int from_x, int from_y, int to_x, int to_y);

/** @brief The straight-line distance between two tiles, rounded down. */
int Distance(int from_x, int from_y, int to_x, int to_y);

/**
 * @brief The search's distance grid before the search: nothing reached, and
 * what the stack can never enter shut.
 */
std::vector<int> Prepare(Game& game, const std::vector<int>& grid, int side_index, int mode,
                         int dest_x, int dest_y);

/** @brief What the road builder reads off the map it is making. */
struct RoadGround {
    std::function<int(int, int)> terrain;    ///< The terrain id at (x, y).
    std::function<bool(int, int)> road;      ///< Whether (x, y) has a road.
    std::function<bool(int, int)> crossing;  ///< Whether (x, y) is a marked crossing.
};

/**
 * @brief The route the random map generator lays a road along.
 *
 * This is the pathfinder run as pseudo-player 14. Its terrain costs are its
 * own (`costs`, the second table in docs/rules.md > Terrain costs) and leave
 * no ground impassable but a city; it moves by land rules, crossing water
 * only at bridges and crossings; one pass of the wavefront, spreading only
 * straight; and a trace that steps diagonally only over water.
 */
std::optional<std::vector<std::pair<int, int>>> RoadRoute(const RoadGround& ground,
                                                          const int costs[12], int width,
                                                          int height, int start_x, int start_y,
                                                          int dest_x, int dest_y);

/**
 * @brief The number of routes the pathfinder keeps: slot 0 for routes under 15
 * tiles, 1-19 for longer ones, kept from game start or load for every side.
 */
constexpr int kCacheSlots = 20;

/** @brief A route the pathfinder keeps, to reuse for a later move. */
struct CachedRoute {
    int mode = 0;           ///< The stack mode it was found for.
    int flags = 0;          ///< The route flags: 8 at sea, else 1 hills and 2 woods bonus.
    int sx = 0;             ///< Where the route starts.
    int sy = 0;             ///< Where the route starts.
    int dx = 0;             ///< Where the route ends.
    int dy = 0;             ///< Where the route ends.
    std::vector<int> dirs;  ///< The route as compass directions.
};
}  // namespace w2::move
namespace w2 {
/** @brief The routes the pathfinder keeps. See docs/rules.md > The search. */
struct PathCache {
    std::array<std::optional<move::CachedRoute>, move::kCacheSlots> slots;  ///< Empty when unused.
};
}  // namespace w2
namespace w2::move {
/** @brief An empty route cache. */
std::shared_ptr<PathCache> NewPathCache();

/**
 * @brief The path from (start_x, start_y) to (dest_x, dest_y) for a stack, or
 * none if there is no route.
 *
 * A route cached for the same destination that the stack stands on is taken
 * as it is; then a step to a neighbouring tile; then the search, its route
 * kept. An empty path means "already there".
 */
std::optional<Path> FindPath(Game& game, const Stack& stack, int start_x, int start_y, int dest_x,
                             int dest_y);

/**
 * @brief The flood the computer players measure distance with.
 *
 * The pathfinder run from (x, y) to itself with flag 0x10 -- no route, no
 * cache -- which runs the wavefront for `range` sweeps at most, each
 * spreading every tile of a square that grows by one round (x, y), and stops
 * early only when a sweep spreads nothing. A cost travels across the scan in
 * one sweep but only a tile a sweep against it, so a winding route can be
 * left long, or not reached.
 *
 * @param grid_side Whose cost grid to use.
 * @param fog_side Whose hidden map shuts what it has not seen (only a
 *     human's, mid-turn).
 * @return The grid: kShut, 30000 unreached, else the cost from (x, y) plus 1
 *     (negative while the tile had still to spread).
 */
std::vector<int> Flood(Game& game, int grid_side, int x, int y, int range, int mode, bool woods,
                       bool hills, int fog_side);

/** @brief Where a walk stopped to attack. */
struct Attack {
    int x = 0;             ///< The tile attacked.
    int y = 0;             ///< The tile attacked.
    City* city = nullptr;  ///< The city attacked, or nullptr for a stack.
};

/** @brief What became of a walk. */
struct WalkResult {
    int steps = 0;  ///< The steps walked.
    int spent = 0;  ///< The moves they cost.
    /// Why it stopped: "arrived", "out of moves", "attack", "at peace", "blocked" or "no route".
    std::string stopped;
    std::optional<Attack> attack;             ///< The attack it stopped for, if any.
    std::vector<std::pair<int, int>> walked;  ///< The tiles walked over, in order.
    bool out_of_moves = false;  ///< The walk left the stack out of moves: a human's is put down.
    /// Whether the stack was at sea as it set off, which is how the walk is shown: every step is
    /// drawn as it set off, and only after the last is it put to sea or ashore.
    bool sea_before = false;
};

/**
 * @brief Going to sea and coming ashore, at the end of a walk.
 * @return "to sea" or "ashore" when that happened, "" otherwise.
 */
std::string SettleSea(Game& game, const Stack& stack, int x, int y, int mode, bool was_at_sea);

/** @brief Walks `stack` along `path`; a tile left empty loses its encampment. */
WalkResult Walk(Game& game, const Stack& stack, const Path& path);

/** @brief Moves a stack towards a tile: pathfind, then walk. */
WalkResult MoveTo(Game& game, const Stack& stack, int x, int y);

}  // namespace w2::move
