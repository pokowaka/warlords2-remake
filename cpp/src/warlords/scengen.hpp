/**
 * @file
 * @brief The scenario generator's world and road planners (not the original's).
 *
 * randommap's classic generator is a port of the 1993 one, byte for byte.
 * The scenario generator is a new design. It uses the same file machinery:
 * RANDOM.DAT's tile tables turn terrain into tiles, and the same city, site
 * and side records are written. What differs is the order and the methods:
 *
 *   1. land     domain-warped fractal noise, sea at the edges, a sea level
 *               picked for a target land share, small islands and lakes
 *               cleaned away (the noise approach of Red Blob Games' mapgen
 *               articles and of many 4X map scripts);
 *   2. relief   mountain ranges along ridged noise rather than blobs, hills
 *               round them, every mountain-walled pocket given a pass;
 *   3. rivers   from the feet of the mountains downhill to the sea, winding
 *               by a smooth noise, joining where they meet;
 *   4. growth   forest and marsh by a moisture field, wetter by the rivers;
 *   5. cities   spread evenly by best-candidate (blue noise) sampling, so no
 *               clumps and no empty stretches, preferring coasts and rivers;
 *   6. sites    the ruins and temples spread the same way, in the wilds;
 *   7. roads    after the tiles are drawn: a minimum spanning tree of the
 *               cities by travel cost, plus a few extra links where the tree
 *               makes a long detour, each laid by A* and merged into roads
 *               already there, bridges where they cross a river.
 *
 * randommap (Options::scenario) calls MakeWorld, draws the tiles, calls
 * PlanRoads, then chooses fair capitals and writes the files.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "warlords/rng.hpp"

namespace w2::scengen {

/** @brief Landmass layout style for generated scenarios. */
enum class Shape {
    kContinents,  ///< Several large landmasses separated by straits.
    kPangaea,     ///< One dominant central landmass surrounded by sea.
    kIslands      ///< Many smaller islands scattered across the sea.
};

/** @brief Parameters controlling landmass shape, terrain shares, and feature counts. */
struct Settings {
    Shape shape = Shape::kContinents;  ///< Landmass layout style.
    double land = -1;                  ///< The land share wanted; < 0 picks the shape's own.
    int cities = 80;                   ///< At most 100, as the .SCN holds no more.
    int sites = 40;                    ///< The .SCN holds exactly 40.
    double mountains = 0.08;           ///< Mountain share of the land.
    double hills = 0.10;               ///< Hill share of the land.
    double forest = 0.17;              ///< Forest share of the land.
    int rivers = 5;                    ///< Number of rivers to place.
};

/**
 * @brief A world's name for New Scenario's list (at most 20 characters, and no
 *        wider than "The Frozen Steppe" in the list's font, so it stays inside
 *        the list's box) and its line of description (at most 30), drawn from
 *        the seed alone -- "The Shattered Isles", "Kingdoms of Ravenmere" -- so
 *        the same seed always gets the same name, and naming never changes the
 *        world.
 */
struct Title {
    std::string name;  ///< Scenario name for the selection list.
    std::string text;  ///< Short flavour description line.
};

/**
 * @brief Generates a deterministic scenario title and description from `shape` and `seed`.
 * @param shape Landmass layout style selecting the noun pool.
 * @param seed World seed used to initialize an isolated RNG sequence.
 * @return Generated scenario `Title` guaranteed to fit the selection list box.
 */
Title MakeTitle(Shape shape, long seed);

/**
 * @brief Parses `"continents"`, `"pangaea"`, or `"islands"` into `out`.
 * @param text Lowercase shape identifier string.
 * @param out Destination `Shape` populated on success.
 * @return `true` when `text` names a valid shape; `false` otherwise.
 */
bool ParseShape(const std::string& text, Shape& out);

/**
 * @brief Returns the lowercase identifier (`"continents"`, `"pangaea"`, or `"islands"`) of `shape`.
 * @param shape Landmass layout style.
 * @return Null-terminated static string naming `shape`.
 */
const char* ShapeName(Shape shape);

/** @brief The terrain grid and placed city and site locations from steps 1-6. */
struct World {
    std::vector<uint8_t> terrain;             ///< width x height terrain type ids (move::kWater..).
    std::vector<std::pair<int, int>> cities;  ///< Top-left of each 2x2 footprint.
    std::vector<std::pair<int, int>> sites;   ///< Coordinates of each ruin or temple.
};

/**
 * @brief Generates the terrain grid, city footprints, and wild sites on a `width` x `height` map.
 * @param settings Landmass shape, terrain shares, and feature counts.
 * @param rng Dice sequence to draw noise permutations and sampling candidates from.
 * @param width Map width in tiles (`> 0`).
 * @param height Map height in tiles (`> 0`).
 * @return Generated `World` terrain and feature coordinates.
 */
World MakeWorld(const Settings& settings, Rng& rng, int width, int height);

/**
 * @brief A river crossing a bridge may span: two water tiles in a row with firm
 *        ground beyond both ends. `ew` spans east-west (x, x+1), else north-south.
 */
struct Span {
    int x = 0;       ///< Column of the first water tile.
    int y = 0;       ///< Row of the first water tile.
    bool ew = true;  ///< True if spanning east-west (x, x+1), false if north-south (y, y+1).
};

/** @brief The planned road network and chosen bridge spans from step 7. */
struct Roads {
    std::vector<uint8_t> road;  ///< width x height, 1 where a road runs.
    std::vector<int> bridges;   ///< Indices into the spans given, the ones used.
};

/**
 * @brief Plans the inter-city road network and bridge spans over finished terrain.
 * @param terrain Row-major `width` x `height` terrain type grid (`move::kWater` ..).
 * @param width Map width in tiles.
 * @param height Map height in tiles.
 * @param cities Top-left `(x, y)` coordinates of each 2x2 city footprint.
 * @param spans Candidate two-tile river spans eligible for bridges.
 * @param rng Dice sequence used when sampling optional shortcut links.
 * @return Planned `Roads` mask and selected bridge span indices.
 */
Roads PlanRoads(const std::vector<uint8_t>& terrain, int width, int height,
                const std::vector<std::pair<int, int>>& cities, const std::vector<Span>& spans,
                Rng& rng);

}  // namespace w2::scengen
