// Map evaluator (`w2mapeval`): scores maps so map generators can be compared.
//
// Evaluates the hand-made scenarios as the reference, then any number of
// generated maps against it, and prints a report: how often each hard rule
// fails, and each scored metric's spread next to the range the designers'
// maps span. See docs/mapeval.md.

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

#include "absl/flags/flag.h"
#include "absl/status/statusor.h"

#include "nlohmann/json.hpp"
#include "util/util.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/scn.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, generator, "classic",
          "Map generator to evaluate: classic, fair, scenario, or none.");
ABSL_FLAG(std::string, seeds, "1-50", "Seed list or ranges, e.g. 1-50 or 3,7,9.");
ABSL_FLAG(std::string, scenarios, "", "Extra comma-separated scenarios to evaluate.");
ABSL_FLAG(std::string, reference, "ERYTHEA,HADESHA,ISLADIA,DRAGON,SORCERY",
          "Comma-separated reference scenarios.");
ABSL_FLAG(std::string, json, "", "Path to write full JSON evaluation report.");
ABSL_FLAG(std::string, export_dir, "",
          "Export generated map files to DIR/<generator>-<seed>/ (also accepts --export).");
ABSL_FLAG(int, issues, 0, "Maximum number of issues to print per map.");
ABSL_FLAG(int, threads, 0, "Worker thread count (0 = hardware_concurrency).");
ABSL_FLAG(bool, allies, false, "Allow magical ally production in generated maps.");
ABSL_FLAG(std::string, shape, "continents",
          "Scenario generator landmass shape: continents, pangaea, or islands.");
ABSL_FLAG(std::string, sliders, "3,3,2,3",
          "Water,Hills,Cities,Forest sliders (0-6, or 7 for random).");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;

namespace {

/** @brief A map generator: makes one random map's files from the given options. */
using Generator = std::function<absl::StatusOr<randommap::Files>(const randommap::Options&)>;

/** @brief The generators `--generator` chooses from, by name. */
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

/** @brief Splits a comma-separated list into its non-empty parts. */
std::vector<std::string> SplitList(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream stream(list);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

/** @brief Parses a seed list such as "1-50" or "3,7,9" into ordered seeds. */
bool ParseSeeds(const std::string& list, std::vector<int>& out) {
    out.clear();
    for (const std::string& part : SplitList(list)) {
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            auto single = ToInt(part);
            if (!single) return false;
            out.push_back(static_cast<int>(*single));
            continue;
        }
        auto first = ToInt(std::string_view(part).substr(0, dash));
        auto last = ToInt(std::string_view(part).substr(dash + 1));
        if (!first || !last) return false;
        for (int i = static_cast<int>(*first); i <= static_cast<int>(*last); i++) out.push_back(i);
    }
    return true;
}

/** @brief Linearly interpolated quantile of `values`. */
double Quantile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    double position = fraction * static_cast<double>(values.size() - 1);
    size_t below = static_cast<size_t>(std::floor(position));
    size_t above = static_cast<size_t>(std::ceil(position));
    return values[below] +
           (values[above] - values[below]) * (position - static_cast<double>(below));
}

/** @brief Loads the scenario in folder `name` under `data` and evaluates it. */
absl::StatusOr<mapeval::Evaluation> EvaluateScenario(const std::filesystem::path& data,
                                                     const std::string& name) {
    auto map = scn::Load(data / name, name);
    if (!map.ok()) return map.status();
    return mapeval::Evaluate(**map, name);
}

/** @brief Prints the maps' hard-rule failures and metric spread against `ref`. */
void Report(const std::string& title, const std::vector<mapeval::Evaluation>& maps,
            const mapeval::Reference& ref) {
    if (maps.empty()) return;
    printf("\n== %s: %zu map(s) ==\n", title.c_str(), maps.size());
    int passed = 0;
    std::vector<double> penalties;
    for (const auto& evaluation : maps) {
        passed += evaluation.Passed();
        penalties.push_back(mapeval::ScoreAgainst(evaluation, ref).penalty);
    }
    printf("passed every hard rule: %d of %zu (%.0f%%)\n", passed, maps.size(),
           100.0 * passed / maps.size());
    printf("penalty against the reference: median %.2f, p95 %.2f\n", Quantile(penalties, 0.5),
           Quantile(penalties, 0.95));
    printf("\n%-16s %5s %10s %12s %12s\n", "rule", "kind", "maps hit", "mean issues", "max issues");
    for (const std::string& check : mapeval::Checks()) {
        int hit = 0;
        int max_issues = 0;
        double sum = 0;
        for (const auto& evaluation : maps) {
            int issue_count = evaluation.Count(check);
            hit += issue_count > 0;
            sum += issue_count;
            max_issues = std::max(max_issues, issue_count);
        }
        printf("%-16s %5s %9.0f%% %12.2f %12d\n", check.c_str(),
               mapeval::IsHard(check) ? "hard" : "soft", 100.0 * hit / maps.size(),
               sum / maps.size(), max_issues);
    }
    printf("\n%-26s %19s %10s %10s %10s %8s\n", "metric", "reference", "p5", "median", "p95",
           "outside");
    for (const auto& [key, unused_value] : maps[0].metrics) {
        (void)unused_value;
        std::vector<double> samples;
        for (const auto& evaluation : maps) samples.push_back(evaluation.Metric(key));
        auto bounds = ref.find(key);
        char range[32] = "";
        int outside = 0;
        if (bounds != ref.end()) {
            snprintf(range, sizeof range, "%.2f..%.2f", bounds->second.min, bounds->second.max);
            for (double sample : samples) {
                outside += sample < bounds->second.min - 1e-9 || sample > bounds->second.max + 1e-9;
            }
        }
        char outside_percent[16] = "";
        if (bounds != ref.end()) {
            snprintf(outside_percent, sizeof outside_percent, "%.0f%%",
                     100.0 * outside / samples.size());
        }
        printf("%-26s %19s %10.2f %10.2f %10.2f %8s\n", key.c_str(), range, Quantile(samples, 0.05),
               Quantile(samples, 0.5), Quantile(samples, 0.95), outside_percent);
    }
}

/** @brief Prints the first issues found on one map. */
void PrintIssues(const mapeval::Evaluation& evaluation, int limit) {
    if (limit <= 0 || evaluation.issues.empty()) return;
    printf("\n%s: %zu issue(s)\n", evaluation.name.c_str(), evaluation.issues.size());
    for (int i = 0; i < static_cast<int>(evaluation.issues.size()) && i < limit; i++) {
        const auto& issue = evaluation.issues[static_cast<size_t>(i)];
        printf("  %-12s (%3d,%3d) %s\n", issue.check.c_str(), issue.x, issue.y,
               issue.detail.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    auto extra_args = ParseCliFlags(
            argc, argv,
            "w2mapeval [--generator classic] [--seeds 1-50] [--sliders 3,3,2,3] [--export DIR]");
    if (!extra_args.empty()) {
        fprintf(stderr, "unknown argument: %s (see the top of cpp/test/mapeval.cpp)\n",
                extra_args[0].c_str());
        return 2;
    }

    std::filesystem::path data = absl::GetFlag(FLAGS_data);
    std::string generator = absl::GetFlag(FLAGS_generator);
    std::filesystem::path json_path = absl::GetFlag(FLAGS_json);
    std::filesystem::path export_dir = absl::GetFlag(FLAGS_export_dir);
    std::string reference_list = absl::GetFlag(FLAGS_reference);
    std::vector<std::string> scenarios = SplitList(absl::GetFlag(FLAGS_scenarios));
    std::vector<int> seeds;
    if (!ParseSeeds(absl::GetFlag(FLAGS_seeds), seeds)) {
        fprintf(stderr, "invalid --seeds '%s'\n", absl::GetFlag(FLAGS_seeds).c_str());
        return 2;
    }
    std::array<int, 4> sliders{{3, 3, 2, 3}};
    auto slider_values = SplitList(absl::GetFlag(FLAGS_sliders));
    if (slider_values.size() != 4) {
        fprintf(stderr, "--sliders takes Water,Hills,Cities,Forest (0-6, 7 for chance)\n");
        return 2;
    }
    for (int slider = 0; slider < 4; slider++) {
        auto parsed_slider = ToInt(slider_values[static_cast<size_t>(slider)]);
        if (!parsed_slider) {
            fprintf(stderr, "--sliders takes Water,Hills,Cities,Forest (0-6, 7 for chance)\n");
            return 2;
        }
        sliders[static_cast<size_t>(slider)] = static_cast<int>(*parsed_slider);
    }

    bool allies = absl::GetFlag(FLAGS_allies);
    scengen::Shape shape = scengen::Shape::kContinents;
    if (!scengen::ParseShape(absl::GetFlag(FLAGS_shape), shape)) {
        fprintf(stderr, "unknown shape '%s'\n", absl::GetFlag(FLAGS_shape).c_str());
        return 2;
    }
    int issue_limit = absl::GetFlag(FLAGS_issues);
    int flag_threads = absl::GetFlag(FLAGS_threads);
    int threads = flag_threads > 0
                          ? flag_threads
                          : static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));

    if (!FileExists(data / "TERRAIN0/ARMYTYPE.DAT")) {
        fprintf(stderr,
                "cannot find the game's files in '%s': run from the repository root, or pass "
                "--data DIR\n",
                data.string().c_str());
        return 1;
    }

    std::vector<mapeval::Evaluation> ref_maps;
    for (const std::string& scenario : SplitList(reference_list)) {
        auto evaluated = EvaluateScenario(data, scenario);
        if (!evaluated.ok()) {
            fprintf(stderr, "%s\n", std::string(evaluated.status().message()).c_str());
            return 1;
        }
        ref_maps.push_back(std::move(*evaluated));
    }
    auto ref = mapeval::MakeReference(ref_maps);
    Report("reference (hand-made)", ref_maps, ref);
    for (const auto& evaluation : ref_maps) PrintIssues(evaluation, issue_limit);

    std::vector<mapeval::Evaluation> extra;
    for (const std::string& scenario : scenarios) {
        auto evaluated = EvaluateScenario(data, scenario);
        if (!evaluated.ok()) {
            fprintf(stderr, "%s\n", std::string(evaluated.status().message()).c_str());
            return 1;
        }
        extra.push_back(std::move(*evaluated));
    }
    Report("scenarios", extra, ref);
    for (const auto& evaluation : extra) PrintIssues(evaluation, issue_limit);

    std::vector<mapeval::Evaluation> made;
    if (generator != "none") {
        auto chosen = Generators().find(generator);
        if (chosen == Generators().end()) {
            fprintf(stderr, "unknown generator '%s'; known:", generator.c_str());
            for (const auto& [known_name, unused_generator] : Generators()) {
                (void)unused_generator;
                fprintf(stderr, " %s", known_name.c_str());
            }
            fprintf(stderr, "\n");
            return 2;
        }
        made.resize(seeds.size());
        std::atomic<size_t> next_job{0};
        std::mutex stderr_mutex;
        auto record_failure = [&](size_t job, std::string_view message) {
            std::lock_guard<std::mutex> lock(stderr_mutex);
            std::string msg_str(message);
            fprintf(stderr, "seed %d: %s\n", seeds[job], msg_str.c_str());
            made[job].name = generator + "#" + std::to_string(seeds[job]);
            made[job].issues.push_back({"error", 0, 0, msg_str});
        };
        auto work = [&]() {
            for (size_t job; (job = next_job++) < seeds.size();) {
                Rng rng(seeds[job]);
                randommap::Options options;
                options.data_dir = data;
                options.rng = &rng;
                options.sliders = randommap::Settle(sliders, rng);
                options.allies = allies;
                options.world.shape = shape;
                auto files = chosen->second(options);
                if (!files.ok()) {
                    record_failure(job, files.status().message());
                    continue;
                }
                if (!export_dir.empty()) {
                    std::filesystem::path dir =
                            export_dir / (generator + "-" + std::to_string(seeds[job]));
                    std::filesystem::create_directories(dir);
                    bool write_ok = true;
                    for (const auto& [ext, bytes] : *files) {
                        if (absl::Status status = WriteBytes(
                                    dir / (std::string(randommap::kDir) + "." + ext), bytes);
                            !status.ok()) {
                            record_failure(job, status.message());
                            write_ok = false;
                            break;
                        }
                    }
                    if (!write_ok) continue;
                }
                auto map = scn::LoadFromFiles(data / randommap::kDir, randommap::kDir, *files);
                if (!map.ok()) {
                    record_failure(job, map.status().message());
                    continue;
                }
                made[job] = mapeval::Evaluate(**map, generator + "#" + std::to_string(seeds[job]));
            }
        };
        std::vector<std::thread> pool;
        for (int worker = 0; worker < std::min<int>(threads, static_cast<int>(seeds.size()));
             worker++) {
            pool.emplace_back(work);
        }
        for (auto& worker : pool) worker.join();
        Report("generator " + generator, made, ref);
        for (const auto& evaluation : made) PrintIssues(evaluation, issue_limit);
    }

    if (!json_path.empty()) {
        nlohmann::ordered_json out = nlohmann::ordered_json::object();
        nlohmann::ordered_json ref_json = nlohmann::ordered_json::object();
        for (const auto& [metric, range] : ref) {
            nlohmann::ordered_json range_json = nlohmann::ordered_json::object();
            range_json["min"] = range.min;
            range_json["max"] = range.max;
            range_json["mean"] = range.mean;
            range_json["sd"] = range.sd;
            ref_json[metric] = std::move(range_json);
        }
        out["reference"] = std::move(ref_json);
        nlohmann::ordered_json maps = nlohmann::ordered_json::array();
        for (const auto* list : {&ref_maps, &extra, &made}) {
            for (const auto& evaluation : *list) {
                auto score = mapeval::ScoreAgainst(evaluation, ref);
                maps.push_back(mapeval::ToJson(evaluation, &score));
            }
        }
        out["maps"] = std::move(maps);
        std::string dumped = out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!WriteFile(json_path, dumped).ok()) {
            fprintf(stderr, "could not write %s\n", json_path.string().c_str());
            return 1;
        }
        printf("\nwrote %s\n", json_path.string().c_str());
    }
    return 0;
}
