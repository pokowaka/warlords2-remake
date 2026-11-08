// Map evaluator (`w2mapeval`): scores maps so map generators can be compared.
//
// Evaluates the hand-made scenarios as the reference, then any number of
// generated maps against it, and prints a report: how often each hard rule
// fails, and each scored metric's spread next to the range the designers'
// maps span. See docs/mapeval.md.
//
// Usage (from the repository root):
//   cpp/build/w2mapeval [--generator classic] [--seeds 1-50] [--sliders 3,3,2,3]
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

using Generator = std::function<randommap::Files(const randommap::Options&)>;

/** The generators to choose from. A new one goes here. */
const std::map<std::string, Generator>& generators() {
    static const std::map<std::string, Generator> g = {
        {"classic", [](const randommap::Options& o) { return randommap::generateNow(o); }},
        {"fair",
         [](const randommap::Options& o) {
             auto f = o;
             f.fairCapitals = true;
             return randommap::generateNow(f);
         }},
        {"scenario",
         [](const randommap::Options& o) {
             auto f = o;
             f.scenario = true;
             return randommap::generateNow(f);
         }},
    };
    return g;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ','))
        if (!part.empty()) out.push_back(part);
    return out;
}

std::vector<int> parseSeeds(const std::string& s) {
    std::vector<int> out;
    for (const std::string& part : splitList(s)) {
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::stoi(part));
            continue;
        }
        int a = std::stoi(part.substr(0, dash)), b = std::stoi(part.substr(dash + 1));
        for (int i = a; i <= b; i++) out.push_back(i);
    }
    return out;
}

double quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double at = q * (double)(v.size() - 1);
    size_t lo = (size_t)std::floor(at), hi = (size_t)std::ceil(at);
    return v[lo] + (v[hi] - v[lo]) * (at - (double)lo);
}

mapeval::Evaluation evaluateScenario(const std::string& data, const std::string& name) {
    auto map = scn::load(data + "/" + name, name);
    return mapeval::evaluate(*map, name);
}

/** The maps' hard-rule failures and metric spread, against the reference. */
void report(const std::string& title, const std::vector<mapeval::Evaluation>& maps,
            const mapeval::Reference& ref) {
    if (maps.empty()) return;
    printf("\n== %s: %zu map(s) ==\n", title.c_str(), maps.size());
    int passed = 0;
    std::vector<double> penalties;
    for (auto& e : maps) {
        passed += e.passed();
        penalties.push_back(mapeval::score(e, ref).penalty);
    }
    printf("passed every hard rule: %d of %zu (%.0f%%)\n", passed, maps.size(),
           100.0 * passed / maps.size());
    printf("penalty against the reference: median %.2f, p95 %.2f\n", quantile(penalties, 0.5),
           quantile(penalties, 0.95));
    printf("\n%-16s %5s %10s %12s %12s\n", "rule", "kind", "maps hit", "mean issues", "max issues");
    for (const std::string& c : mapeval::checks()) {
        int hit = 0, max = 0;
        double sum = 0;
        for (auto& e : maps) {
            int n = e.count(c);
            hit += n > 0;
            sum += n;
            max = std::max(max, n);
        }
        printf("%-16s %5s %9.0f%% %12.2f %12d\n", c.c_str(), mapeval::isHard(c) ? "hard" : "soft",
               100.0 * hit / maps.size(), sum / maps.size(), max);
    }
    printf("\n%-26s %19s %10s %10s %10s %8s\n", "metric", "reference", "p5", "median", "p95",
           "outside");
    for (auto& [key, value] : maps[0].metrics) {
        std::vector<double> xs;
        for (auto& e : maps) xs.push_back(e.metric(key));
        auto r = ref.find(key);
        char range[32] = "";
        int outside = 0;
        if (r != ref.end()) {
            snprintf(range, sizeof range, "%.2f..%.2f", r->second.min, r->second.max);
            for (double x : xs) outside += x < r->second.min - 1e-9 || x > r->second.max + 1e-9;
        }
        char out[16] = "";
        if (r != ref.end()) snprintf(out, sizeof out, "%.0f%%", 100.0 * outside / xs.size());
        printf("%-26s %19s %10.2f %10.2f %10.2f %8s\n", key.c_str(), range, quantile(xs, 0.05),
               quantile(xs, 0.5), quantile(xs, 0.95), out);
    }
}

void printIssues(const mapeval::Evaluation& e, int limit) {
    if (limit <= 0 || e.issues.empty()) return;
    printf("\n%s: %zu issue(s)\n", e.name.c_str(), e.issues.size());
    for (int i = 0; i < (int)e.issues.size() && i < limit; i++) {
        auto& is = e.issues[i];
        printf("  %-12s (%3d,%3d) %s\n", is.check.c_str(), is.x, is.y, is.detail.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string data = "original", generator = "classic", jsonPath, exportDir;
    std::string referenceList = "ERYTHEA,HADESHA,ISLADIA,DRAGON,SORCERY";
    std::vector<std::string> scenarios;
    std::vector<int> seeds = parseSeeds("1-50");
    std::array<int, 4> sliders{{3, 3, 2, 3}};
    bool allies = false;
    scengen::Shape shape = scengen::Shape::Continents;
    int issueLimit = 0;
    int threads = (int)std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--data")
            data = next();
        else if (a == "--generator")
            generator = next();
        else if (a == "--seeds")
            seeds = parseSeeds(next());
        else if (a == "--scenarios")
            scenarios = splitList(next());
        else if (a == "--reference")
            referenceList = next();
        else if (a == "--json")
            jsonPath = next();
        else if (a == "--export")
            exportDir = next();
        else if (a == "--issues")
            issueLimit = std::stoi(next());
        else if (a == "--threads")
            threads = std::max(1, std::stoi(next()));
        else if (a == "--allies")
            allies = true;
        else if (a == "--shape") {
            std::string sh = next();
            if (!scengen::parseShape(sh, shape)) {
                fprintf(stderr, "unknown shape '%s'\n", sh.c_str());
                return 2;
            }
        } else if (a == "--sliders") {
            auto v = splitList(next());
            if (v.size() != 4) {
                fprintf(stderr, "--sliders takes Water,Hills,Cities,Forest (0-6, 7 for chance)\n");
                return 2;
            }
            for (int k = 0; k < 4; k++) sliders[k] = std::stoi(v[k]);
        } else {
            fprintf(stderr, "unknown argument: %s (see the top of cpp/test/mapeval.cpp)\n",
                    a.c_str());
            return 2;
        }
    }
    if (!fileExists(data + "/TERRAIN0/ARMYTYPE.DAT")) {
        fprintf(stderr,
                "cannot find the game's files in '%s': run from the repository root, or pass "
                "--data DIR\n",
                data.c_str());
        return 1;
    }

    // the reference: the designers' own maps
    std::vector<mapeval::Evaluation> refMaps;
    for (const std::string& s : splitList(referenceList))
        refMaps.push_back(evaluateScenario(data, s));
    auto ref = mapeval::reference(refMaps);
    report("reference (hand-made)", refMaps, ref);
    for (auto& e : refMaps) printIssues(e, issueLimit);

    std::vector<mapeval::Evaluation> extra;
    for (const std::string& s : scenarios) extra.push_back(evaluateScenario(data, s));
    report("scenarios", extra, ref);
    for (auto& e : extra) printIssues(e, issueLimit);

    std::vector<mapeval::Evaluation> made;
    if (generator != "none") {
        auto g = generators().find(generator);
        if (g == generators().end()) {
            fprintf(stderr, "unknown generator '%s'; known:", generator.c_str());
            for (auto& [k, _] : generators()) fprintf(stderr, " %s", k.c_str());
            fprintf(stderr, "\n");
            return 2;
        }
        made.resize(seeds.size());
        std::atomic<size_t> nextJob{0};
        std::mutex errors;
        auto work = [&]() {
            for (size_t k; (k = nextJob++) < seeds.size();) {
                try {
                    Rng r(seeds[k]);
                    randommap::Options o;
                    o.dataDir = data;
                    o.rng = &r;
                    o.sliders = randommap::settle(sliders, r);
                    o.allies = allies;
                    o.world.shape = shape;
                    auto files = g->second(o);
                    if (!exportDir.empty()) {
                        std::string dir =
                                exportDir + "/" + generator + "-" + std::to_string(seeds[k]);
                        std::filesystem::create_directories(dir);
                        for (auto& [ext, bytes] : files)
                            if (!writeFile(dir + "/" + randommap::DIR + "." + ext, bytes))
                                throw std::runtime_error("could not write " + dir);
                    }
                    // straight from memory: nothing is written to the data folder
                    auto map =
                            scn::loadFromFiles(data + "/" + randommap::DIR, randommap::DIR, files);
                    made[k] = mapeval::evaluate(*map, generator + "#" + std::to_string(seeds[k]));
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lock(errors);
                    fprintf(stderr, "seed %d: %s\n", seeds[k], ex.what());
                    made[k].name = generator + "#" + std::to_string(seeds[k]);
                    made[k].issues.push_back({"error", 0, 0, ex.what()});
                }
            }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < std::min<int>(threads, (int)seeds.size()); t++) pool.emplace_back(work);
        for (auto& t : pool) t.join();
        report("generator " + generator, made, ref);
        for (auto& e : made) printIssues(e, issueLimit);
    }

    if (!jsonPath.empty()) {
        Json out = Json::object();
        Json refJson = Json::object();
        for (auto& [k, r] : ref) {
            Json o = Json::object();
            o.set("min", r.min);
            o.set("max", r.max);
            o.set("mean", r.mean);
            o.set("sd", r.sd);
            refJson.set(k, o);
        }
        out.set("reference", refJson);
        Json maps = Json::array();
        for (auto* list : {&refMaps, &extra, &made})
            for (auto& e : *list) {
                auto s = mapeval::score(e, ref);
                maps.push(mapeval::toJson(e, &s));
            }
        out.set("maps", maps);
        if (!writeFile(jsonPath, out.dump())) {
            fprintf(stderr, "could not write %s\n", jsonPath.c_str());
            return 1;
        }
        printf("\nwrote %s\n", jsonPath.c_str());
    }
    return 0;
}
