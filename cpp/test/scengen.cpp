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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "util/util.hpp"
#include "warlords/mapeval.hpp"
#include "warlords/pck.hpp"
#include "warlords/preview.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/scenarios.hpp"
#include "warlords/scn.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(int64_t, seed, 1, "Starting random seed.");
ABSL_FLAG(int, count, 1, "Number of worlds to generate.");
ABSL_FLAG(std::string, shape, "continents", "Landmass shape: continents, pangaea, or islands.");
ABSL_FLAG(int, cities, 80, "Target city count (8 to 100).");
ABSL_FLAG(double, land, -1.0, "Target land fraction (< 0 uses the shape's default).");
ABSL_FLAG(std::string, name, "", "Scenario folder and file name (at most 8 characters).");
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(int, tries, 20, "Seeds to try before giving up on the hard rules.");
ABSL_FLAG(bool, keep_failures, false, "Write the last world even if hard rules fail.");
ABSL_FLAG(bool, open, false, "Open generated preview PNGs in the system image viewer.");
ABSL_FLAG(bool, quiet, false, "Suppress progress output.");
ABSL_FLAG(std::string, preview, "", "Only render DATA/NAME/NAME.png for an existing scenario.");
ABSL_FLAG(std::string, dump, "",
          "Write raw generated files to DIR/<shape>-<seed>/RANDOM.* for cross-port comparison.");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;

namespace {

/** @brief Shows a picture in the system's image viewer. */
void OpenPicture(const std::filesystem::path& path) {
#if defined(__APPLE__)
    std::string cmd = "open \"" + path.string() + "\"";
#elif defined(_WIN32)
    std::string cmd = "start \"\" \"" + path.string() + "\"";
#else
    std::string cmd = "xdg-open \"" + path.string() + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) {
        fprintf(stderr, "could not open %s; open it yourself\n", path.string().c_str());
    }
}

/** @brief Draws the preview picture beside a scenario's files (`DATA/NAME/NAME.png`). */
absl::StatusOr<std::filesystem::path> WritePreview(const std::filesystem::path& data,
                                                   const std::string& name) {
    auto map = scn::Load(data / name, name);
    if (!map.ok()) return map.status();
    auto png_bytes = preview::Png(**map, data);
    if (!png_bytes.ok()) return png_bytes.status();
    std::filesystem::path path = data / name / (name + ".png");
    if (absl::Status status = WriteBytes(path, *png_bytes); !status.ok()) return status;
    return path;
}

/** @brief Writes `NAME.INF`, `PICS/SCENARIO.PCK`, and records `dir` in `SCENGEN.LST`. */
absl::Status WriteListing(const std::filesystem::path& data, const std::string& dir,
                          const std::string& title, const std::string& text) {
    auto map = scn::Load(data / dir, dir);
    if (!map.ok()) return map.status();
    scenarios::Entry entry;
    entry.name = title;
    entry.text = text;
    entry.cities = (*map)->CityCount();
    entry.ruins = (*map)->SiteCount();
    for (const auto& side : (*map)->Sides()) entry.players += side.CapitalCity(**map) != nullptr;
    std::filesystem::path base = data / dir;
    std::filesystem::create_directories(base / "PICS");
    if (absl::Status status = WriteFile(base / (dir + ".INF"), scenarios::Info(entry));
        !status.ok()) {
        return status;
    }
    auto menu_pic = preview::MenuPicture(**map, data);
    if (!menu_pic.ok()) return menu_pic.status();
    auto encoded_pic = pck::Encode(*menu_pic);
    if (!encoded_pic.ok()) return encoded_pic.status();
    if (absl::Status status = WriteBytes(base / "PICS/SCENARIO.PCK", *encoded_pic); !status.ok()) {
        return status;
    }
    if (!scenarios::Remember(data, dir)) {
        return absl::InternalError("Failed to update DATA/SCENGEN.LST in '" + data.string() +
                                   "'; verify write permissions on the DATA directory.");
    }
    return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
    auto extra_args = ParseCliFlags(
            argc, argv,
            "w2scengen [--seed N] [--count N] [--shape continents|pangaea|islands] [--dump DIR]");
    if (!extra_args.empty()) {
        fprintf(stderr, "unknown argument: %s (see the top of cpp/test/scengen.cpp)\n",
                extra_args[0].c_str());
        return 2;
    }

    std::filesystem::path data = absl::GetFlag(FLAGS_data);
    std::filesystem::path dump_dir = absl::GetFlag(FLAGS_dump);
    if (const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
        workspace != nullptr && *workspace != '\0') {
        if (data.is_relative()) data = std::filesystem::path(workspace) / data;
        if (!dump_dir.empty() && dump_dir.is_relative()) {
            dump_dir = std::filesystem::path(workspace) / dump_dir;
        }
    }
    std::string name = Upper(absl::GetFlag(FLAGS_name));
    std::string preview_only = Upper(absl::GetFlag(FLAGS_preview));
    long seed = static_cast<long>(absl::GetFlag(FLAGS_seed));
    int tries = std::max(1, absl::GetFlag(FLAGS_tries));
    int count = std::max(1, absl::GetFlag(FLAGS_count));
    bool keep_failures = absl::GetFlag(FLAGS_keep_failures);
    bool quiet = absl::GetFlag(FLAGS_quiet);
    bool open = absl::GetFlag(FLAGS_open);

    scengen::Settings world;
    std::string shape_name = absl::GetFlag(FLAGS_shape);
    if (!scengen::ParseShape(shape_name, world.shape)) {
        fprintf(stderr, "unknown shape '%s': use continents, pangaea or islands\n",
                shape_name.c_str());
        return 2;
    }
    world.cities = absl::GetFlag(FLAGS_cities);
    world.land = absl::GetFlag(FLAGS_land);

    if (!preview_only.empty()) {
        std::filesystem::path scn_path = data / preview_only / (preview_only + ".SCN");
        if (!FileExists(scn_path)) {
            fprintf(stderr, "no scenario '%s'\n", scn_path.string().c_str());
            return 1;
        }
        auto written = WritePreview(data, preview_only);
        if (!written.ok()) {
            fprintf(stderr, "could not draw the preview: %s\n",
                    std::string(written.status().message()).c_str());
            return 1;
        }
        printf("%s\n", written->string().c_str());
        if (open) OpenPicture(*written);
        return 0;
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
    if (!FileExists(data / "RANDOM/RANDOM.DAT")) {
        fprintf(stderr,
                "cannot find '%s/RANDOM/RANDOM.DAT': run from the repository root, or pass --data "
                "DIR\n",
                data.string().c_str());
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
            if (!files.ok()) {
                fprintf(stderr, "could not generate seed %ld: %s\n", world_seed,
                        std::string(files.status().message()).c_str());
                return 1;
            }
            std::filesystem::path dir = dump_dir / (std::string(scengen::ShapeName(world.shape)) +
                                                    "-" + std::to_string(world_seed));
            std::filesystem::create_directories(dir);
            for (const auto& [ext, bytes] : *files) {
                if (absl::Status status =
                            WriteBytes(dir / (std::string(randommap::kDir) + "." + ext), bytes);
                    !status.ok()) {
                    fprintf(stderr, "%s\n", std::string(status.message()).c_str());
                    return 1;
                }
            }
            auto title = scengen::MakeTitle(world.shape, world_seed);
            if (absl::Status status =
                        WriteFile(dir / "TITLE.TXT", title.name + "\n" + title.text + "\n");
                !status.ok()) {
                fprintf(stderr, "%s\n", std::string(status.message()).c_str());
                return 1;
            }
        }
        return 0;
    }
    std::vector<std::filesystem::path> pictures;
    for (int world_index = 0; world_index < count; world_index++) {
        randommap::Files files;
        mapeval::Evaluation eval;
        long first = seed + world_index;
        long used = first;
        // a failed seed steps on by the count, so the worlds' seeds never collide
        for (int attempt = 0; attempt < tries; attempt++, used += count) {
            Rng rng(static_cast<double>(used));
            randommap::Options options;
            options.data_dir = data;
            options.rng = &rng;
            options.scenario = true;
            options.world = world;
            auto generated = randommap::GenerateNow(options);
            if (!generated.ok()) {
                fprintf(stderr, "%s\n", std::string(generated.status().message()).c_str());
                return 1;
            }
            files = std::move(*generated);
            auto map = scn::LoadFromFiles(data / randommap::kDir, randommap::kDir, files);
            if (!map.ok()) {
                fprintf(stderr, "%s\n", std::string(map.status().message()).c_str());
                return 1;
            }
            eval = mapeval::Evaluate(**map, "scenario#" + std::to_string(used));
            if (eval.Passed() || keep_failures) break;
            if (!quiet) {
                for (const auto& issue : eval.issues) {
                    if (mapeval::IsHard(issue.check)) {
                        printf("seed %ld: %s at (%d,%d): %s; trying another seed\n", used,
                               issue.check.c_str(), issue.x, issue.y, issue.detail.c_str());
                        break;
                    }
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
        std::filesystem::path dir = data / folder;
        std::filesystem::create_directories(dir);
        for (const auto& [ext, bytes] : files) {
            if (!WriteBytes(dir / (folder + "." + ext), bytes).ok()) {
                fprintf(stderr, "could not write %s/%s.%s\n", dir.string().c_str(), folder.c_str(),
                        ext.c_str());
                return 1;
            }
        }
        if (auto itm = ReadBytes(data / "RANDOM/RANDOM.ITM"); itm.ok()) {
            WriteBytes(dir / (folder + ".ITM"), *itm).IgnoreError();
        }
        std::filesystem::path picture;
        if (auto written = WritePreview(data, folder); written.ok()) {
            picture = *written;
            pictures.push_back(picture);
            auto title = scengen::MakeTitle(world.shape, used);
            if (absl::Status list_status =
                        WriteListing(data, folder, name.empty() ? title.name : folder, title.text);
                !list_status.ok()) {
                fprintf(stderr, "%s: no listing: %s\n", folder.c_str(),
                        std::string(list_status.message()).c_str());
            }
        } else {
            fprintf(stderr, "%s: no preview: %s\n", folder.c_str(),
                    std::string(written.status().message()).c_str());
        }
        if (!quiet) {
            printf("%s: \"%s\" (seed %ld, %s), %s the hard rules\n", dir.string().c_str(),
                   (name.empty() ? scengen::MakeTitle(world.shape, used).name : folder).c_str(),
                   used, scengen::ShapeName(world.shape), eval.Passed() ? "passes" : "FAILS");
            if (count == 1) {
                for (const char* key :
                     {"cities", "landFrac", "clarkEvansR", "cityRoadCoverage", "capitalGapMinTurns",
                      "expansionGini", "remoteCityFrac", "roadsToNowhere"}) {
                    printf("  %-20s %.2f\n", key, eval.Metric(key));
                }
            }
            if (!picture.empty()) printf("  preview: %s\n", picture.string().c_str());
            printf("  play it: bazel-bin/cpp/warlords2 --window --scenario %s\n", folder.c_str());
        }
    }
    if (open) {
        for (const auto& picture_path : pictures) OpenPicture(picture_path);
    }
    return 0;
}
