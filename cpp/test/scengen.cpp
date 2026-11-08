// Scenario generator (`w2scengen`): makes a new world and writes it as a
// scenario folder the game can play (not the original's; docs/scengen.md).
//
// Each world is checked with the map evaluator (warlords/mapeval) as it is
// made; one that breaks a hard rule is made again from the next seed.
//
// Usage (from the repository root):
//   cpp/build/w2scengen [--seed N] [--shape continents|pangaea|islands]
//                       [--cities N] [--land 0.55] [--name NAME] [--data DIR]
//                       [--tries N] [--keep-failures] [--quiet]
//
//   --name NAME   the folder and file names, at most 8 letters (default
//                 GEN<seed>); the scenario is written to DATA/NAME/NAME.*
//   --tries N     seeds to try before giving up on the hard rules (default 20)
//
// Then play it:  cpp/build/warlords2 --window --scenario NAME

#include <cstdio>
#include <filesystem>
#include <string>

#include "util/util.hpp"
#include "warlords/mapeval.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/scn.hpp"

using namespace w2;

int main(int argc, char** argv) {
  std::string data = "original", name;
  long seed = 1;
  int tries = 20;
  bool keepFailures = false, quiet = false;
  scengen::Settings world;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
      return argv[++i];
    };
    if (a == "--seed") seed = std::stol(next());
    else if (a == "--shape") {
      std::string s = next();
      if (!scengen::parseShape(s, world.shape)) {
        fprintf(stderr, "unknown shape '%s': use continents, pangaea or islands\n", s.c_str());
        return 2;
      }
    } else if (a == "--cities") world.cities = std::stoi(next());
    else if (a == "--land") world.land = std::stod(next());
    else if (a == "--name") name = upper(next());
    else if (a == "--data") data = next();
    else if (a == "--tries") tries = std::max(1, std::stoi(next()));
    else if (a == "--keep-failures") keepFailures = true;
    else if (a == "--quiet") quiet = true;
    else {
      fprintf(stderr, "unknown argument: %s (see the top of cpp/test/scengen.cpp)\n", a.c_str());
      return 2;
    }
  }
  if (world.cities < 8 || world.cities > 100) {
    fprintf(stderr, "--cities must be 8 to 100: the .SCN holds at most 100 cities\n");
    return 2;
  }
  if (!fileExists(data + "/RANDOM/RANDOM.DAT")) {
    fprintf(stderr, "cannot find '%s/RANDOM/RANDOM.DAT': run from the repository root, or pass --data DIR\n",
            data.c_str());
    return 1;
  }
  randommap::Files files;
  mapeval::Evaluation eval;
  long used = seed;
  for (int t = 0; t < tries; t++, used++) {
    Rng r((double)used);
    randommap::Options o;
    o.dataDir = data;
    o.rng = &r;
    o.scenario = true;
    o.world = world;
    files = randommap::generateNow(o);
    auto map = scn::loadFromFiles(data + "/" + randommap::DIR, randommap::DIR, files);
    eval = mapeval::evaluate(*map, "scenario#" + std::to_string(used));
    if (eval.passed() || keepFailures) break;
    if (!quiet) {
      for (auto& is : eval.issues)
        if (mapeval::isHard(is.check)) {
          printf("seed %ld: %s at (%d,%d): %s; trying the next seed\n", used, is.check.c_str(), is.x, is.y,
                 is.detail.c_str());
          break;
        }
    }
  }
  if (!eval.passed() && !keepFailures) {
    fprintf(stderr, "no world from seeds %ld-%ld passed the hard rules; --keep-failures writes the last anyway\n",
            seed, used - 1);
    return 1;
  }
  if (name.empty()) name = "GEN" + std::to_string(used % 100000);
  if (name.size() > 8) {
    fprintf(stderr, "--name: '%s' is longer than the 8 letters the game's file names allow\n", name.c_str());
    return 2;
  }
  std::string dir = data + "/" + name;
  std::filesystem::create_directories(dir);
  for (auto& [ext, bytes] : files)
    if (!writeFile(dir + "/" + name + "." + ext, bytes)) {
      fprintf(stderr, "could not write %s/%s.%s\n", dir.c_str(), name.c_str(), ext.c_str());
      return 1;
    }
  // the item pool the random worlds use
  if (auto itm = readFile(data + "/RANDOM/RANDOM.ITM")) writeFile(dir + "/" + name + ".ITM", *itm);
  if (!quiet) {
    printf("%s: seed %ld, %s, %s the hard rules\n", dir.c_str(), used, scengen::shapeName(world.shape),
           eval.passed() ? "passes" : "FAILS");
    for (const char* k : {"cities", "landFrac", "clarkEvansR", "cityRoadCoverage", "capitalGapMinTurns",
                          "expansionGini", "remoteCityFrac", "roadsToNowhere"})
      printf("  %-20s %.2f\n", k, eval.metric(k));
    printf("play it:  cpp/build/warlords2 --window --scenario %s\n", name.c_str());
  }
  return 0;
}
