// Map evaluator (`w2mapeval`): scores maps so map generators can be compared.
//
// Evaluates the hand-made scenarios as the reference, then any number of
// generated maps against it, and prints a report: how often each hard rule
// fails, and each scored metric's spread next to the range the designers'
// maps span. See docs/mapeval.md.
//
// Usage (from the repository root):
//   bazel-bin/cpp/w2mapeval [--generator classic] [--seeds 1-50] [--sliders 3,3,2,3]
//                       [--allies] [--scenarios ERYTHEA,...] [--reference A,B,...]
//                       [--json PATH] [--issues N] [--threads N] [--data DIR]
//
//   --generator NAME   which generator makes the maps (classic: the 1993 port;
//                      fair: the same with capitals placed for a fair start;
//                      scenario: the new scenario generator, docs/scengen.md);
//   --shape SHAPE      the scenario generator's land: continents, pangaea, islands
//                      "none" evaluates only --scenarios
//   --seeds A-B|A,B,C  the seeds to make maps from
//   --scenarios LIST   scenarios to evaluate as well, by folder name
//   --reference LIST   the hand-made maps the metrics are scored against
//                      (default ERYTHEA,HADESHA,ISLADIA,DRAGON,SORCERY)
//   --json PATH        every evaluation, its issues and score, as JSON
//   --export DIR       write each generated map's files to DIR/<generator>-<seed>/
//                      (RANDOM.MAP, .RD, ...), for tools/mapren.py and tools/mapvote.py
//   --issues N         print the first N issues of each map (default 0)

#include "warlords/mapeval.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "util/util.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/scn.hpp"

using namespace w2;

namespace {

/** @brief A map generator: makes one random map's files from the given options. */
using Generator = std::function<randommap::Files(const randommap::Options&)>;

/**
 * @brief The generators `--generator` chooses from, by name.
 *
 * A new generator goes here.
 */
const std::map<std::string, Generator>& Generators() {
    static const std::map<std::string, Generator> kGenerators = {
        {"classic",
         [](const randommap::Options& options) { return randommap::GenerateNow(options); }},
        {"fair",
         [](const randommap::Options& options) {
             auto fair_options = options;
             fair_options.fair_capitals = true;
             return randommap::GenerateNow(fair_options);
         }},
        {"scenario",
         [](const randommap::Options& options) {
             auto scenario_options = options;
             scenario_options.scenario = true;
             return randommap::GenerateNow(scenario_options);
         }},
    };
    return kGenerators;
}

/**
 * @brief Splits a comma-separated list into its non-empty parts.
 * @param list The text of the list, e.g. "ERYTHEA,HADESHA".
 */
std::vector<std::string> SplitList(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream stream(list);
    std::string part;
    while (std::getline(stream, part, ','))
        if (!part.empty()) out.push_back(part);
    return out;
}

/**
 * @brief Parses a seed list such as "1-50" or "3,7,9" (ranges and single
 *        seeds may be mixed) into the seeds it names, in order.
 */
std::vector<int> ParseSeeds(const std::string& list) {
    std::vector<int> out;
    for (const std::string& part : SplitList(list)) {
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::stoi(part));
            continue;
        }
        int first = std::stoi(part.substr(0, dash)), last = std::stoi(part.substr(dash + 1));
        for (int i = first; i <= last; i++) out.push_back(i);
    }
    return out;
}

/**
 * @brief The quantile of a sample, interpolating linearly between the two
 *        nearest values.
 * @param values The sample (taken by value, as it is sorted here).
 * @param fraction Which quantile, 0 to 1 (0.5 is the median).
 * @return The quantile, or 0 for an empty sample.
 */
double Quantile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    double position = fraction * static_cast<double>(values.size() - 1);
    size_t below = static_cast<size_t>(std::floor(position)),
           above = static_cast<size_t>(std::ceil(position));
    return values[below] +
           (values[above] - values[below]) * (position - static_cast<double>(below));
}

/** @brief Loads the scenario in folder `name` under `data` and evaluates it. */
mapeval::Evaluation EvaluateScenario(const std::string& data, const std::string& name) {
    auto map = scn::Load(data + "/" + name, name);
    return mapeval::Evaluate(*map, name);
}

/**
 * @brief Prints the maps' hard-rule failures and metric spread, against the
 *        reference.
 *
 * Shows how many maps pass every hard rule, how far they score from the
 * reference, how often each check fails, and each metric's 5th, 50th and 95th
 * percentile next to the range the reference maps span.
 *
 * @param title The heading for this group of maps.
 * @param maps The evaluated maps; nothing is printed when it is empty.
 * @param ref The reference ranges the metrics are scored against.
 */
void Report(const std::string& title, const std::vector<mapeval::Evaluation>& maps,
            const mapeval::Reference& ref) {
    if (maps.empty()) return;
    printf("\n== %s: %zu map(s) ==\n", title.c_str(), maps.size());
    int passed = 0;
    std::vector<double> penalties;
    for (auto& evaluation : maps) {
        passed += evaluation.Passed();
        penalties.push_back(mapeval::ScoreAgainst(evaluation, ref).penalty);
    }
    printf("passed every hard rule: %d of %zu (%.0f%%)\n", passed, maps.size(),
           100.0 * passed / maps.size());
    printf("penalty against the reference: median %.2f, p95 %.2f\n", Quantile(penalties, 0.5),
           Quantile(penalties, 0.95));
    printf("\n%-16s %5s %10s %12s %12s\n", "rule", "kind", "maps hit", "mean issues", "max issues");
    for (const std::string& check : mapeval::Checks()) {
        int hit = 0, max = 0;
        double sum = 0;
        for (auto& evaluation : maps) {
            int issue_count = evaluation.Count(check);
            hit += issue_count > 0;
            sum += issue_count;
            max = std::max(max, issue_count);
        }
        printf("%-16s %5s %9.0f%% %12.2f %12d\n", check.c_str(),
               mapeval::IsHard(check) ? "hard" : "soft", 100.0 * hit / maps.size(),
               sum / maps.size(), max);
    }
    printf("\n%-26s %19s %10s %10s %10s %8s\n", "metric", "reference", "p5", "median", "p95",
           "outside");
    for (auto& [key, value] : maps[0].metrics) {
        std::vector<double> samples;
        for (auto& evaluation : maps) samples.push_back(evaluation.Metric(key));
        auto bounds = ref.find(key);
        char range[32] = "";
        int outside = 0;
        if (bounds != ref.end()) {
            snprintf(range, sizeof range, "%.2f..%.2f", bounds->second.min, bounds->second.max);
            for (double sample : samples)
                outside += sample < bounds->second.min - 1e-9 || sample > bounds->second.max + 1e-9;
        }
        char outside_percent[16] = "";
        if (bounds != ref.end())
            snprintf(outside_percent, sizeof outside_percent, "%.0f%%",
                     100.0 * outside / samples.size());
        printf("%-26s %19s %10.2f %10.2f %10.2f %8s\n", key.c_str(), range, Quantile(samples, 0.05),
               Quantile(samples, 0.5), Quantile(samples, 0.95), outside_percent);
    }
}

/**
 * @brief Prints the first issues found on one map.
 * @param evaluation The map's evaluation.
 * @param limit How many issues to print at most; 0 or less prints nothing.
 */
void PrintIssues(const mapeval::Evaluation& evaluation, int limit) {
    if (limit <= 0 || evaluation.issues.empty()) return;
    printf("\n%s: %zu issue(s)\n", evaluation.name.c_str(), evaluation.issues.size());
    for (int i = 0; i < static_cast<int>(evaluation.issues.size()) && i < limit; i++) {
        auto& issue = evaluation.issues[i];
        printf("  %-12s (%3d,%3d) %s\n", issue.check.c_str(), issue.x, issue.y,
               issue.detail.c_str());
    }
}

}  // namespace

/**
 * @brief Parses the command line (see the top of this file), evaluates the
 *        reference maps, the named scenarios and the generated maps, prints a
 *        report for each group, and optionally writes them all as JSON.
 * @return 0 on success, 1 when files are missing or cannot be written, 2 on a
 *         bad command line.
 */
int main(int argc, char** argv) {
    std::string data = "original", generator = "classic", json_path, export_dir;
    std::string reference_list = "ERYTHEA,HADESHA,ISLADIA,DRAGON,SORCERY";
    std::vector<std::string> scenarios;
    std::vector<int> seeds = ParseSeeds("1-50");
    std::array<int, 4> sliders{{3, 3, 2, 3}};
    bool allies = false;
    scengen::Shape shape = scengen::Shape::kContinents;
    int issue_limit = 0;
    int threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
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
        if (arg == "--data")
            data = next();
        else if (arg == "--generator")
            generator = next();
        else if (arg == "--seeds")
            seeds = ParseSeeds(next());
        else if (arg == "--scenarios")
            scenarios = SplitList(next());
        else if (arg == "--reference")
            reference_list = next();
        else if (arg == "--json")
            json_path = next();
        else if (arg == "--export")
            export_dir = next();
        else if (arg == "--issues")
            issue_limit = std::stoi(next());
        else if (arg == "--threads")
            threads = std::max(1, std::stoi(next()));
        else if (arg == "--allies")
            allies = true;
        else if (arg == "--shape") {
            std::string shape_name = next();
            if (!scengen::ParseShape(shape_name, shape)) {
                fprintf(stderr, "unknown shape '%s'\n", shape_name.c_str());
                return 2;
            }
        } else if (arg == "--sliders") {
            auto slider_values = SplitList(next());
            if (slider_values.size() != 4) {
                fprintf(stderr, "--sliders takes Water,Hills,Cities,Forest (0-6, 7 for chance)\n");
                return 2;
            }
            for (int slider = 0; slider < 4; slider++)
                sliders[slider] = std::stoi(slider_values[slider]);
        } else {
            fprintf(stderr, "unknown argument: %s (see the top of cpp/test/mapeval.cpp)\n",
                    arg.c_str());
            return 2;
        }
    }
    if (!FileExists(data + "/TERRAIN0/ARMYTYPE.DAT")) {
        fprintf(stderr,
                "cannot find the game's files in '%s': run from the repository root, or pass "
                "--data DIR\n",
                data.c_str());
        return 1;
    }

    // the reference: the designers' own maps
    std::vector<mapeval::Evaluation> ref_maps;
    for (const std::string& scenario : SplitList(reference_list))
        ref_maps.push_back(EvaluateScenario(data, scenario));
    auto ref = mapeval::MakeReference(ref_maps);
    Report("reference (hand-made)", ref_maps, ref);
    for (auto& evaluation : ref_maps) PrintIssues(evaluation, issue_limit);

    std::vector<mapeval::Evaluation> extra;
    for (const std::string& scenario : scenarios) extra.push_back(EvaluateScenario(data, scenario));
    Report("scenarios", extra, ref);
    for (auto& evaluation : extra) PrintIssues(evaluation, issue_limit);

    std::vector<mapeval::Evaluation> made;
    if (generator != "none") {
        auto chosen = Generators().find(generator);
        if (chosen == Generators().end()) {
            fprintf(stderr, "unknown generator '%s'; known:", generator.c_str());
            for (auto& [known_name, unused_generator] : Generators())
                fprintf(stderr, " %s", known_name.c_str());
            fprintf(stderr, "\n");
            return 2;
        }
        made.resize(seeds.size());
        std::atomic<size_t> next_job{0};
        std::mutex stderr_mutex;
        // each worker thread takes the next seed until none are left
        auto work = [&]() {
            for (size_t job; (job = next_job++) < seeds.size();) {
                try {
                    Rng rng(seeds[job]);
                    randommap::Options options;
                    options.data_dir = data;
                    options.rng = &rng;
                    options.sliders = randommap::Settle(sliders, rng);
                    options.allies = allies;
                    options.world.shape = shape;
                    auto files = chosen->second(options);
                    if (!export_dir.empty()) {
                        std::string dir =
                                export_dir + "/" + generator + "-" + std::to_string(seeds[job]);
                        std::filesystem::create_directories(dir);
                        for (auto& [ext, bytes] : files)
                            if (!WriteFile(dir + "/" + randommap::kDir + "." + ext, bytes))
                                throw std::runtime_error("could not write " + dir);
                    }
                    // straight from memory: nothing is written to the data folder
                    auto map = scn::LoadFromFiles(data + "/" + randommap::kDir, randommap::kDir,
                                                  files);
                    made[job] =
                            mapeval::Evaluate(*map, generator + "#" + std::to_string(seeds[job]));
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lock(stderr_mutex);
                    fprintf(stderr, "seed %d: %s\n", seeds[job], ex.what());
                    made[job].name = generator + "#" + std::to_string(seeds[job]);
                    made[job].issues.push_back({"error", 0, 0, ex.what()});
                }
            }
        };
        std::vector<std::thread> pool;
        for (int worker = 0; worker < std::min<int>(threads, static_cast<int>(seeds.size()));
             worker++)
            pool.emplace_back(work);
        for (auto& worker : pool) worker.join();
        Report("generator " + generator, made, ref);
        for (auto& evaluation : made) PrintIssues(evaluation, issue_limit);
    }

    if (!json_path.empty()) {
        Json out = Json::Object();
        Json ref_json = Json::Object();
        for (auto& [metric, range] : ref) {
            Json range_json = Json::Object();
            range_json.Set("min", range.min);
            range_json.Set("max", range.max);
            range_json.Set("mean", range.mean);
            range_json.Set("sd", range.sd);
            ref_json.Set(metric, range_json);
        }
        out.Set("reference", ref_json);
        Json maps = Json::Array();
        for (auto* list : {&ref_maps, &extra, &made})
            for (auto& evaluation : *list) {
                auto score = mapeval::ScoreAgainst(evaluation, ref);
                maps.Push(mapeval::ToJson(evaluation, &score));
            }
        out.Set("maps", maps);
        if (!WriteFile(json_path, out.Dump())) {
            fprintf(stderr, "could not write %s\n", json_path.c_str());
            return 1;
        }
        printf("\nwrote %s\n", json_path.c_str());
    }
    return 0;
}
