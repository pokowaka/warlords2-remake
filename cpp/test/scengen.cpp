// Scenario generator (`w2scengen`): makes new worlds and writes each as a
// scenario folder the game can play, with a preview picture beside it (not
// the original's; docs/scengen.md).
//
// Each world is checked with the map evaluator (warlords/mapeval) as it is
// made; one that breaks a hard rule is made again from another seed.
//
// Usage (from the repository root):
//   cpp/build/w2scengen [--seed N] [--count N] [--shape continents|pangaea|islands]
//                       [--cities N] [--land 0.55] [--name NAME] [--data DIR]
//                       [--tries N] [--keep-failures] [--open] [--quiet]
//   cpp/build/w2scengen --preview NAME [--open]
//
//   --count N       make N worlds, from seeds N, N+1, ... (each its own folder)
//   --name NAME     the folder and file names, at most 8 letters (default
//                   GEN<seed>); the scenario is written to DATA/NAME/NAME.*
//   --tries N       seeds to try before giving up on the hard rules (default 20)
//   --open          open the preview pictures in the system's image viewer
//   --preview NAME  only draw DATA/NAME/NAME.png for an existing scenario, any
//                   scenario (ERYTHEA too), and make nothing
//   --dump DIR      write exactly the seeds asked for, failures too, as
//                   DIR/<shape>-<seed>/RANDOM.*, and nothing else: what the
//                   JavaScript and Lua ports are checked against, byte for
//                   byte (cpp/test/scengen_compare.sh)
//
// Every world gets DATA/NAME/NAME.png: the map in the game's own tiles, with
// roads, cities, capitals (in their sides' colours) and sites drawn over it.
// It also gets NAME.INF and PICS/SCENARIO.PCK, and is named in
// DATA/DATA/SCENGEN.LST, which put it in the game's New Scenario list (all
// three games), after the shipped scenarios, with its picture on the start
// menu (warlords/scenarios). Or play it straight away:
//   cpp/build/warlords2 --window --scenario NAME

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "util/util.hpp"
#include "warlords/mapeval.hpp"
#include "warlords/preview.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/scenarios.hpp"
#include "warlords/scn.hpp"

using namespace w2;

namespace {

/** Show a picture in the system's viewer. */
void OpenPicture(const std::string& path) {
#if defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
#elif defined(_WIN32)
    std::string cmd = "start \"\" \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0)
        fprintf(stderr, "could not open %s; open it yourself\n", path.c_str());
}

/** The preview picture beside a scenario's files; its path. */
std::string WritePreview(const std::string& data, const std::string& name) {
    auto map = scn::load(data + "/" + name, name);
    std::string path = data + "/" + name + "/" + name + ".png";
    if (!writeFile(path, preview::png(*map, data)))
        throw std::runtime_error("could not write " + path);
    return path;
}

/** What puts a world in New Scenario's list: NAME.INF, and the start menu's
 *  picture, PICS/SCENARIO.PCK (warlords/scenarios). */
void WriteListing(const std::string& data, const std::string& dir, const std::string& title,
                  const std::string& text) {
    auto map = scn::load(data + "/" + dir, dir);
    scenarios::Entry e;
    e.name = title;
    e.text = text;
    e.cities = static_cast<int>(map->cities.size());
    e.ruins = static_cast<int>(map->sites.size());
    for (auto& sd : map->sides) e.players += sd.capital != nullptr;
    std::string base = data + "/" + dir + "/";
    std::filesystem::create_directories(base + "PICS");
    if (!writeFile(base + dir + ".INF", scenarios::info(e)) ||
        !writeFile(base + "PICS/SCENARIO.PCK", pck::encode(preview::menuPicture(*map, data))) ||
        !scenarios::remember(data, dir))
        throw std::runtime_error("could not write the listing in " + base);
}

}  // namespace

int main(int argc, char** argv) {
    std::string data = "original", name, preview_only, dump_dir;
    long seed = 1;
    int tries = 20, count = 1;
    bool keep_failures = false, quiet = false, open = false;
    scengen::Settings world;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--seed")
            seed = std::stol(next());
        else if (a == "--count")
            count = std::max(1, std::stoi(next()));
        else if (a == "--shape") {
            std::string s = next();
            if (!scengen::parseShape(s, world.shape)) {
                fprintf(stderr, "unknown shape '%s': use continents, pangaea or islands\n",
                        s.c_str());
                return 2;
            }
        } else if (a == "--cities")
            world.cities = std::stoi(next());
        else if (a == "--land")
            world.land = std::stod(next());
        else if (a == "--name")
            name = upper(next());
        else if (a == "--data")
            data = next();
        else if (a == "--tries")
            tries = std::max(1, std::stoi(next()));
        else if (a == "--keep-failures")
            keep_failures = true;
        else if (a == "--open")
            open = true;
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--preview")
            preview_only = upper(next());
        else if (a == "--dump")
            dump_dir = next();
        else {
            fprintf(stderr, "unknown argument: %s (see the top of cpp/test/scengen.cpp)\n",
                    a.c_str());
            return 2;
        }
    }
    if (!preview_only.empty()) {
        if (!fileExists(data + "/" + preview_only + "/" + preview_only + ".SCN")) {
            fprintf(stderr, "no scenario '%s/%s/%s.SCN'\n", data.c_str(), preview_only.c_str(),
                    preview_only.c_str());
            return 1;
        }
        try {
            std::string path = WritePreview(data, preview_only);
            printf("%s\n", path.c_str());
            if (open) OpenPicture(path);
            return 0;
        } catch (const std::exception& ex) {
            fprintf(stderr, "could not draw the preview: %s\n", ex.what());
            return 1;
        }
    }
    if (world.cities < 8 || world.cities > 100) {
        fprintf(stderr, "--cities must be 8 to 100: the .SCN holds at most 100 cities\n");
        return 2;
    }
    if (count > 1 && !name.empty()) {
        fprintf(stderr,
                "--name names one world; leave it out with --count to get GEN<seed> folders\n");
        return 2;
    }
    if (!fileExists(data + "/RANDOM/RANDOM.DAT")) {
        fprintf(stderr,
                "cannot find '%s/RANDOM/RANDOM.DAT': run from the repository root, or pass --data "
                "DIR\n",
                data.c_str());
        return 1;
    }
    if (!dump_dir.empty()) {
        for (int k = 0; k < count; k++) {
            long s0 = seed + k;
            Rng r(static_cast<double>(s0));
            randommap::Options o;
            o.dataDir = data;
            o.rng = &r;
            o.scenario = true;
            o.world = world;
            auto files = randommap::generateNow(o);
            std::string dir =
                    dump_dir + "/" + scengen::shapeName(world.shape) + "-" + std::to_string(s0);
            std::filesystem::create_directories(dir);
            for (auto& [ext, bytes] : files)
                writeFile(dir + "/" + randommap::DIR + "." + ext, bytes);
            auto t = scengen::title(world.shape, s0);
            writeFile(dir + "/TITLE.TXT", t.name + "\n" + t.text + "\n");
        }
        return 0;
    }
    std::vector<std::string> pictures;
    for (int k = 0; k < count; k++) {
        randommap::Files files;
        mapeval::Evaluation eval;
        long first = seed + k, used = first;
        // a failed seed steps on by the count, so the worlds' seeds never collide
        for (int t = 0; t < tries; t++, used += count) {
            Rng r(static_cast<double>(used));
            randommap::Options o;
            o.dataDir = data;
            o.rng = &r;
            o.scenario = true;
            o.world = world;
            files = randommap::generateNow(o);
            auto map = scn::loadFromFiles(data + "/" + randommap::DIR, randommap::DIR, files);
            eval = mapeval::evaluate(*map, "scenario#" + std::to_string(used));
            if (eval.passed() || keep_failures) break;
            if (!quiet) {
                for (auto& is : eval.issues)
                    if (mapeval::isHard(is.check)) {
                        printf("seed %ld: %s at (%d,%d): %s; trying another seed\n", used,
                               is.check.c_str(), is.x, is.y, is.detail.c_str());
                        break;
                    }
            }
        }
        if (!eval.passed() && !keep_failures) {
            fprintf(stderr,
                    "no world from seed %ld on passed the hard rules; --keep-failures writes the "
                    "last anyway\n",
                    first);
            return 1;
        }
        std::string n = name.empty() ? "GEN" + std::to_string(used % 100000) : name;
        if (n.size() > 8) {
            fprintf(stderr,
                    "--name: '%s' is longer than the 8 letters the game's file names allow\n",
                    n.c_str());
            return 2;
        }
        std::string dir = data + "/" + n;
        std::filesystem::create_directories(dir);
        for (auto& [ext, bytes] : files)
            if (!writeFile(dir + "/" + n + "." + ext, bytes)) {
                fprintf(stderr, "could not write %s/%s.%s\n", dir.c_str(), n.c_str(), ext.c_str());
                return 1;
            }
        // the item pool the random worlds use
        if (auto itm = readFile(data + "/RANDOM/RANDOM.ITM"))
            writeFile(dir + "/" + n + ".ITM", *itm);
        std::string picture;
        try {
            picture = WritePreview(data, n);
            pictures.push_back(picture);
            auto fun = scengen::title(world.shape, used);
            WriteListing(data, n, name.empty() ? fun.name : n, fun.text);
        } catch (const std::exception& ex) {
            fprintf(stderr, "%s: no preview or listing: %s\n", n.c_str(), ex.what());
        }
        if (!quiet) {
            printf("%s: \"%s\" (seed %ld, %s), %s the hard rules\n", dir.c_str(),
                   (name.empty() ? scengen::title(world.shape, used).name : n).c_str(), used,
                   scengen::shapeName(world.shape), eval.passed() ? "passes" : "FAILS");
            if (count == 1)
                for (const char* key :
                     {"cities", "landFrac", "clarkEvansR", "cityRoadCoverage", "capitalGapMinTurns",
                      "expansionGini", "remoteCityFrac", "roadsToNowhere"})
                    printf("  %-20s %.2f\n", key, eval.metric(key));
            if (!picture.empty()) printf("  preview: %s\n", picture.c_str());
            printf("  play it: cpp/build/warlords2 --window --scenario %s\n", n.c_str());
        }
    }
    if (open)
        for (auto& p : pictures) OpenPicture(p);
    return 0;
}
