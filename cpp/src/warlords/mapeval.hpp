/**
 * @file
 * @brief Map evaluation: scores a loaded scenario map so that map generators
 *        can be compared by numbers rather than by eye (w2mapeval,
 *        test/mapeval.cpp).
 *
 * The evaluator looks only at the Map that scn::Load produced, never inside a
 * generator, so any generator -- or a hand-made .SCN -- is judged the same
 * way. Two kinds of result:
 *
 *   Issues   hard-rule failures with the tile they happen on: a road that
 *            leads nowhere, cities on top of each other, a city no capital
 *            can reach, a signpost that points the wrong way...
 *   Metrics  named numbers (city spacing, road detours, landmass shape...)
 *            that a Reference built from the hand-made scenarios turns into
 *            a penalty: 0 inside the range the designers' maps span, growing
 *            with the distance outside it.
 */
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/json.hpp"
#include "warlords/types.hpp"

namespace w2::mapeval {

/** @brief One rule failure, on the tile where it shows. */
struct Issue {
    std::string check;   ///< Which check raised it, e.g. "stray-road".
    int x = 0;           ///< The tile's column.
    int y = 0;           ///< The tile's row.
    std::string detail;  ///< What is wrong, in words.
};

/** @brief Everything the evaluator found out about one map. */
struct Evaluation {
    std::string name;                                     ///< The map's name.
    std::vector<Issue> issues;                            ///< Every rule failure.
    std::vector<std::pair<std::string, double>> metrics;  ///< Named numbers, in a fixed order.

    /** @brief No hard rule broken (soft ones, like a spur road, are only counted). */
    bool Passed() const;
    /** @brief The metric's value, or `fallback` when it was not measured. */
    double Metric(const std::string& key, double fallback = 0) const;
    /** @brief How many issues a check raised. */
    int Count(const std::string& check) const;
};

/** @brief Every check, by name, in the order they are reported. */
const std::vector<std::string>& Checks();
/**
 * @brief Is a check a hard rule?
 *
 * The hard ones are those the hand-made scenarios never break; the soft ones
 * (a spur road to a lone city, say) the designers did now and then, so they
 * are counted and scored, not failed.
 */
bool IsHard(const std::string& check);
/**
 * @brief The metrics that are scored against a Reference (scale-free ones
 *        only: a count of road tiles says nothing until it is divided by
 *        something).
 */
const std::vector<std::string>& ScoredMetrics();

/** @brief Evaluates one map. */
Evaluation Evaluate(const Map& map, const std::string& name);

/** @brief A metric's spread over the reference maps. */
struct Range {
    double min = 0;   ///< The smallest value.
    double max = 0;   ///< The largest value.
    double mean = 0;  ///< The mean.
    double sd = 0;    ///< The standard deviation.
};
/** @brief Each scored metric's spread over the reference maps, by name. */
using Reference = std::map<std::string, Range>;

/** @brief The spread of every scored metric over some maps (the hand-made ones). */
Reference MakeReference(const std::vector<Evaluation>& maps);

/**
 * @brief How far a map sits outside the reference, metric by metric: 0
 *        inside the range, else the distance outside it in reference
 *        standard deviations.
 */
struct Score {
    double penalty = 0;                                      ///< The sum of the deviations.
    std::vector<std::pair<std::string, double>> deviations;  ///< The non-zero ones, signed.
};
/** @brief Scores an evaluation against a reference. */
Score ScoreAgainst(const Evaluation& evaluation, const Reference& ref);

/** @brief The evaluation as JSON, with its score when there is a reference. */
Json ToJson(const Evaluation& evaluation, const Score* score = nullptr);

}  // namespace w2::mapeval
