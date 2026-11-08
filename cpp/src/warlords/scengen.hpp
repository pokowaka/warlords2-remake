// The scenario generator's world and road planners (not the original's).
//
// randommap's classic generator is a port of the 1993 one, byte for byte.
// The scenario generator is a new design. It uses the same file machinery:
// RANDOM.DAT's tile tables turn terrain into tiles, and the same city, site
// and side records are written. What differs is the order and the methods:
//
//   1. land     domain-warped fractal noise, sea at the edges, a sea level
//               picked for a target land share, small islands and lakes
//               cleaned away (the noise approach of Red Blob Games' mapgen
//               articles and of many 4X map scripts);
//   2. relief   mountain ranges along ridged noise rather than blobs, hills
//               round them, every mountain-walled pocket given a pass;
//   3. rivers   from the feet of the mountains downhill to the sea, winding
//               by a smooth noise, joining where they meet;
//   4. growth   forest and marsh by a moisture field, wetter by the rivers;
//   5. cities   spread evenly by best-candidate (blue noise) sampling, so no
//               clumps and no empty stretches, preferring coasts and rivers;
//   6. sites    the ruins and temples spread the same way, in the wilds;
//   7. roads    after the tiles are drawn: a minimum spanning tree of the
//               cities by travel cost, plus a few extra links where the tree
//               makes a long detour, each laid by A* and merged into roads
//               already there, bridges where they cross a river.
//
// randommap (Options::style = Scenario) calls makeWorld, draws the tiles,
// calls planRoads, then chooses fair capitals and writes the files.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "warlords/rng.hpp"

namespace w2::scengen {

enum class Shape { Continents, Pangaea, Islands };

struct Settings {
  Shape shape = Shape::Continents;
  double land = -1;      // the land share wanted; < 0 picks the shape's own
  int cities = 80;       // at most 100, as the .SCN holds no more
  int sites = 40;        // the .SCN holds exactly 40
  double mountains = 0.08, hills = 0.10, forest = 0.17;   // shares of the land
  int rivers = 5;
};

/** A world's name for New Scenario's list (at most 20 characters, and no
 *  wider than "The Frozen Steppe" in the list's font, so it stays inside the
 *  list's box) and its line of description (at most 30), drawn from the
 *  seed alone -- "The
 *  Shattered Isles", "Kingdoms of Ravenmere" -- so the same seed always gets
 *  the same name, and naming never changes the world. */
struct Title {
  std::string name, text;
};
Title title(Shape shape, long seed);

/** Parse "continents", "pangaea" or "islands"; false if it is none. */
bool parseShape(const std::string& s, Shape& out);
const char* shapeName(Shape s);

struct World {
  std::vector<uint8_t> terrain;   // W x H terrain type ids (move::WATER ...)
  std::vector<std::pair<int, int>> cities;   // top-left of each 2x2 footprint
  std::vector<std::pair<int, int>> sites;
};

/** Steps 1-6: the ground, the cities and the sites, on a W x H grid. */
World makeWorld(const Settings& s, Rng& rng, int w, int h);

/** A river crossing a bridge may span: two water tiles in a row with firm
 *  ground beyond both ends. `ew` spans east-west (x, x+1), else north-south. */
struct Span {
  int x = 0, y = 0;
  bool ew = true;
};

struct Roads {
  std::vector<uint8_t> road;      // W x H, 1 where a road runs
  std::vector<int> bridges;       // indices into the spans given, the ones used
};

/** Step 7: the roads between the cities over the finished terrain. */
Roads planRoads(const std::vector<uint8_t>& terrain, int w, int h, const std::vector<std::pair<int, int>>& cities,
                const std::vector<Span>& spans, Rng& rng);

}  // namespace w2::scengen
