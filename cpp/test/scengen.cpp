// Scenario generator (`w2scengen`): makes new worlds and writes each as a
// scenario folder the game can play, with a preview picture beside it (not
// the original's; docs/scengen.md).
//
// Each world is checked with the map evaluator (warlords/mapeval) as it is
// made; one that breaks a hard rule is made again from another seed.
//
// Usage (from the repository root):
//   bazel-bin/cpp/w2scengen [--seed N] [--count N] [--shape continents|pangaea|islands]
//                       [--cities N] [--land 0.55] [--name NAME] [--data DIR]
//                       [--tries N] [--keep-failures] [--open] [--quiet]
//   bazel-bin/cpp/w2scengen --preview NAME [--open]
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
//   bazel-bin/cpp/warlords2 --window --scenario NAME

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

/**
 * @brief Shows a picture in the system's image viewer.
 *
 * Prints a note to stderr when the viewer cannot be started.
 */
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

/**
 * @brief Draws the preview picture beside a scenario's files.
 * @param data The game's data folder.
 * @param name The scenario's folder and file name.
 * @return The path of the picture written, DATA/NAME/NAME.png.
 * @throws std::runtime_error when the picture cannot be written.
 */
std::string WritePreview(const std::string& data, const std::string& name) {
    auto map = scn::Load(data + "/" + name, name);
    std::string path = data + "/" + name + "/" + name + ".png";
    if (!WriteFile(path, preview::Png(*map, data)))
        throw std::runtime_error("could not write " + path);
    return path;
}

/**
 * @brief Writes what puts a world in New Scenario's list.
 *
 * That is NAME.INF, the start menu's picture PICS/SCENARIO.PCK, and the
 * world's entry in the list of generated scenarios (warlords/scenarios).
 *
 * @param data The game's data folder.
 * @param dir The scenario's folder and file name.
 * @param title The name shown in the list.
 * @param text The description shown with it.
 * @throws std::runtime_error when any of it cannot be written.
 */
void WriteListing(const std::string& data, const std::string& dir, const std::string& title,
                  const std::string& text) {
    auto map = scn::Load(data + "/" + dir, dir);
    scenarios::Entry entry;
    entry.name = title;
    entry.text = text;
    entry.cities = static_cast<int>(map->cities.size());
    entry.ruins = static_cast<int>(map->sites.size());
    for (auto& side : map->sides) entry.players += side.capital != nullptr;
    std::string base = data + "/" + dir + "/";
    std::filesystem::create_directories(base + "PICS");
    if (!WriteFile(base + dir + ".INF", scenarios::Info(entry)) ||
        !WriteFile(base + "PICS/SCENARIO.PCK", pck::Encode(preview::MenuPicture(*map, data))) ||
        !scenarios::Remember(data, dir))
        throw std::runtime_error("could not write the listing in " + base);
}

}  // namespace

/**
 * @brief Parses the command line (see the top of this file) and makes the
 *        worlds asked for, or only draws a preview (`--preview`) or dumps the
 *        raw generated files (`--dump`).
 * @return 0 on success, 1 when files are missing, cannot be written or no seed
 *         passes the hard rules, 2 on a bad command line.
 */
int main(int argc, char** argv) {
    std::string data = "original", name, preview_only, dump_dir;
    long seed = 1;
    int tries = 20, count = 1;
    bool keep_failures = false, quiet = false, open = false;
    scengen::Settings world;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        // the option's value: the next argument, which it consumes
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", arg.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (arg == "--seed")
            seed = std::stol(next());
        else if (arg == "--count")
            count = std::max(1, std::stoi(next()));
        else if (arg == "--shape") {
            std::string shape_name = next();
            if (!scengen::ParseShape(shape_name, world.shape)) {
                fprintf(stderr, "unknown shape '%s': use continents, pangaea or islands\n",
                        shape_name.c_str());
                return 2;
            }
        } else if (arg == "--cities")
            world.cities = std::stoi(next());
        else if (arg == "--land")
            world.land = std::stod(next());
        else if (arg == "--name")
            name = Upper(next());
        else if (arg == "--data")
            data = next();
        else if (arg == "--tries")
            tries = std::max(1, std::stoi(next()));
        else if (arg == "--keep-failures")
            keep_failures = true;
        else if (arg == "--open")
            open = true;
        else if (arg == "--quiet")
            quiet = true;
        else if (arg == "--preview")
            preview_only = Upper(next());
        else if (arg == "--dump")
            dump_dir = next();
        else {
            fprintf(stderr, "unknown argument: %s (see the top of cpp/test/scengen.cpp)\n",
                    arg.c_str());
            return 2;
        }
    }
    if (!preview_only.empty()) {
        if (!FileExists(data + "/" + preview_only + "/" + preview_only + ".SCN")) {
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
    if (!FileExists(data + "/RANDOM/RANDOM.DAT")) {
        fprintf(stderr,
                "cannot find '%s/RANDOM/RANDOM.DAT': run from the repository root, or pass --data "
                "DIR\n",
                data.c_str());
        return 1;
    }
    if (!dump_dir.empty()) {
        for (int world_index = 0; world_index < count; world_index++) {
            long world_seed = seed + world_index;
            Rng rng(static_cast<double>(world_seed));
            randommap::Options options;
            options.data_dir = data;
            options.rng = &rng;
            options.scenario = true;
            options.world = world;
            auto files = randommap::GenerateNow(options);
            std::string dir = dump_dir + "/" + scengen::ShapeName(world.shape) + "-" +
                              std::to_string(world_seed);
            std::filesystem::create_directories(dir);
            for (auto& [ext, bytes] : files)
                WriteFile(dir + "/" + randommap::kDir + "." + ext, bytes);
            auto title = scengen::MakeTitle(world.shape, world_seed);
            WriteFile(dir + "/TITLE.TXT", title.name + "\n" + title.text + "\n");
        }
        return 0;
    }
    std::vector<std::string> pictures;
    for (int world_index = 0; world_index < count; world_index++) {
        randommap::Files files;
        mapeval::Evaluation eval;
        long first = seed + world_index, used = first;
        // a failed seed steps on by the count, so the worlds' seeds never collide
        for (int attempt = 0; attempt < tries; attempt++, used += count) {
            Rng rng(static_cast<double>(used));
            randommap::Options options;
            options.data_dir = data;
            options.rng = &rng;
            options.scenario = true;
            options.world = world;
            files = randommap::GenerateNow(options);
            auto map = scn::LoadFromFiles(data + "/" + randommap::kDir, randommap::kDir, files);
            eval = mapeval::Evaluate(*map, "scenario#" + std::to_string(used));
            if (eval.Passed() || keep_failures) break;
            if (!quiet) {
                // report the first hard-rule failure only
                for (auto& issue : eval.issues)
                    if (mapeval::IsHard(issue.check)) {
                        printf("seed %ld: %s at (%d,%d): %s; trying another seed\n", used,
                               issue.check.c_str(), issue.x, issue.y, issue.detail.c_str());
                        break;
                    }
            }
        }
        if (!eval.Passed() && !keep_failures) {
            fprintf(stderr,
                    "no world from seed %ld on passed the hard rules; --keep-failures writes the "
                    "last anyway\n",
                    first);
            return 1;
        }
        std::string folder = name.empty() ? "GEN" + std::to_string(used % 100000) : name;
        if (folder.size() > 8) {
            fprintf(stderr,
                    "--name: '%s' is longer than the 8 letters the game's file names allow\n",
                    folder.c_str());
            return 2;
        }
        std::string dir = data + "/" + folder;
        std::filesystem::create_directories(dir);
        for (auto& [ext, bytes] : files)
            if (!WriteFile(dir + "/" + folder + "." + ext, bytes)) {
                fprintf(stderr, "could not write %s/%s.%s\n", dir.c_str(), folder.c_str(),
                        ext.c_str());
                return 1;
            }
        // the item pool the random worlds use
        if (auto itm = ReadFile(data + "/RANDOM/RANDOM.ITM"))
            WriteFile(dir + "/" + folder + ".ITM", *itm);
        std::string picture;
        try {
            picture = WritePreview(data, folder);
            pictures.push_back(picture);
            auto title = scengen::MakeTitle(world.shape, used);
            WriteListing(data, folder, name.empty() ? title.name : folder, title.text);
        } catch (const std::exception& ex) {
            fprintf(stderr, "%s: no preview or listing: %s\n", folder.c_str(), ex.what());
        }
        if (!quiet) {
            printf("%s: \"%s\" (seed %ld, %s), %s the hard rules\n", dir.c_str(),
                   (name.empty() ? scengen::MakeTitle(world.shape, used).name : folder).c_str(),
                   used, scengen::ShapeName(world.shape), eval.Passed() ? "passes" : "FAILS");
            if (count == 1)
                for (const char* key :
                     {"cities", "landFrac", "clarkEvansR", "cityRoadCoverage", "capitalGapMinTurns",
                      "expansionGini", "remoteCityFrac", "roadsToNowhere"})
                    printf("  %-20s %.2f\n", key, eval.Metric(key));
            if (!picture.empty()) printf("  preview: %s\n", picture.c_str());
            printf("  play it: bazel-bin/cpp/warlords2 --window --scenario %s\n", folder.c_str());
        }
    }
    if (open)
        for (auto& picture_path : pictures) OpenPicture(picture_path);
    return 0;
}
