// Map evaluation: scores a loaded scenario map so that map generators can be
// compared by numbers rather than by eye (w2mapeval, test/mapeval.cpp).
//
// The evaluator looks only at the Map that scn::load produced, never inside a
// generator, so any generator -- or a hand-made .SCN -- is judged the same
// way. Two kinds of result:
//
//   Issues   hard-rule failures with the tile they happen on: a road that
//            leads nowhere, cities on top of each other, a city no capital
//            can reach, a signpost that points the wrong way...
//   Metrics  named numbers (city spacing, road detours, landmass shape...)
//            that a Reference built from the hand-made scenarios turns into
//            a penalty: 0 inside the range the designers' maps span, growing
//            with the distance outside it.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/json.hpp"
#include "warlords/types.hpp"

namespace w2::mapeval {

/** One hard-rule failure, on the tile where it shows. */
struct Issue {
    std::string check;  // e.g. "stray-road"
    int x = 0, y = 0;
    std::string detail;
};

struct Evaluation {
    std::string name;
    std::vector<Issue> issues;
    std::vector<std::pair<std::string, double>> metrics;  // in a fixed order

    /** No hard rule broken (soft ones, like a spur road, are only counted). */
    bool Passed() const;
    /** The metric's value, or `dflt` when it was not measured. */
    double Metric(const std::string& key, double dflt = 0) const;
    /** How many issues a check raised. */
    int Count(const std::string& check) const;
};

/** Every check, by name, in the order they are reported. */
const std::vector<std::string>& Checks();
/** Is a check a hard rule? The hard ones are those the hand-made scenarios
 *  never break; the soft ones (a spur road to a lone city, say) the
 *  designers did now and then, so they are counted and scored, not failed. */
bool IsHard(const std::string& check);
/** The metrics that are scored against a Reference (scale-free ones only:
 *  a count of road tiles says nothing until it is divided by something). */
const std::vector<std::string>& ScoredMetrics();

/** Evaluate one map. */
Evaluation Evaluate(const Map& map, const std::string& name);

/** A metric's spread over the reference maps. */
struct Range {
    double min = 0, max = 0, mean = 0, sd = 0;
};
using Reference = std::map<std::string, Range>;

/** The spread of every scored metric over some maps (the hand-made ones). */
Reference MakeReference(const std::vector<Evaluation>& maps);

/** How far a map sits outside the reference, metric by metric: 0 inside the
 *  range, else the distance outside it in reference standard deviations. */
struct Score {
    double penalty = 0;                                      // the sum
    std::vector<std::pair<std::string, double>> deviations;  // the non-zero ones
};
Score ScoreAgainst(const Evaluation& e, const Reference& ref);

/** The evaluation as JSON, with its score when there is a reference. */
Json ToJson(const Evaluation& e, const Score* s = nullptr);

}  // namespace w2::mapeval
