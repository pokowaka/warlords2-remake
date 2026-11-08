// Lookahead search for the Modern AI (`w2search`): does thinking ahead beat
// the network's instinct?
//
// A Modern side's turn is normally one pass of the policy network. Here, at a
// searched turn, the side instead considers K+1 *candidate turns*: the greedy
// turn (what it would play anyway) and K turns sampled from the same policy at
// a higher temperature. Each candidate is copied R times (different dice) and
// played on for H rounds by everyone's normal AI; the candidate whose copies end
// with the best average empire score is the turn actually played.
//
//   probe: play benchmark games normally, and at a few turns measure how often
//          a sampled candidate beats the greedy one, and by how much (D2).
//   play:  every Modern turn is searched; reports the benchmark city share, to
//          be compared with the same seeds played without search.
//
//   bazel-bin/cpp/w2search WEIGHTS [probe|play] [SEEDS] [K] [R] [H] [TURNS] [peek|honest|fair]
//
// Copies are made with save::encode/decode, so only shipped scenarios are used
// (random maps live in memory only).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/save.hpp"

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

/** @brief Directory holding the original game data. */
const std::string kData = "original";
/** @brief The built-in scenarios the benchmark games cycle through. */
const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};
/** @brief One combination of game options a benchmark game is played with. */
struct Preset {
    const char* label;  ///< Name printed in the per-game report.
    int hidden;         ///< The "hiddenMap" option (fog of war).
    int diplomacy;      ///< The "diplomacy" option.
    int neutral;        ///< The "neutralCities" option.
};
/** @brief The benchmark option presets. */
const Preset kPresets[4] = {{"Beginner-Open", 0, 0, 0},
                            {"Interm-Diplo", 0, 1, 1},
                            {"Adv-Hidden", 1, 1, 2},
                            {"Fog-TotalWar", 1, 0, 1}};
constexpr uint32_t kSeedBase = 750000;  ///< The development seed set.

/** @brief How candidate turns are judged. */
enum class Judge {
    kPeek,    ///< Each candidate turn is played once on a copy with the real game's
              ///< dice and that very result is kept -- it sees its own battles'
              ///< outcome before committing (an upper bound, not a fair player).
    kHonest,  ///< A candidate is a way of deciding (the policy with one sampling
              ///< seed); every rollout replays its turn with fresh dice, and the
              ///< chosen candidate is then played in the real game with its dice.
    kFair     ///< Honest, and every rollout copy first loses the enemy armies the
              ///< side cannot see (field armies on unseen tiles and in transit).
};

/** @brief The search parameters, from the command line. */
struct Settings {
    const ml::PolicyNetwork* policy = nullptr;  ///< The network every Modern side plays.
    Judge judge = Judge::kHonest;               ///< How candidates are judged.
    int candidates = 4;                         ///< K: sampled turns besides the greedy one.
    int rollouts = 4;                           ///< R: copies per candidate.
    int horizon = 2;                            ///< H: rounds played on after the candidate turn.
    float cand_temp = 0.5F;  ///< Temperature the sampled candidates are drawn at.
    float switch_z = 0.0F;   ///< Only leave the greedy turn when a candidate beats it by
                             ///< this many standard errors.
};

/** @brief A deep copy of @p game, made through a save/load round trip. */
std::unique_ptr<Game> CopyGame(const Game& game) {
    return save::Decode(save::Encode(game), kData);
}

/**
 * @brief Reduces @p game to the side's view of the board.
 *
 * Enemy field armies it cannot see are removed (garrisons of cities stay --
 * a known approximation), and so are armies in transit.
 */
void RestrictToView(Game& game, int my_side) {
    std::array<std::vector<Army*>, 8> unseen;
    for (Army* army : game.armies) {
        if (army->owner < 0 || army->owner >= 8 || army->owner == my_side || army->disbanded)
            continue;
        bool hidden = army->transit || army->x == kNone;
        if (!hidden && !game::CityAt(game, army->x, army->y))
            hidden = !ai::belief::IsTileVisible(game, my_side, army->x, army->y);
        if (hidden) unseen[army->owner].push_back(army);
    }
    for (int side_index = 0; side_index < 8; side_index++) {
        if (!unseen[side_index].empty())
            game::Disband(game, game.map->sides[side_index], unseen[side_index]);
    }
}

/**
 * @brief Plays the side's turn as candidate number @p candidate.
 *
 * Candidate 0 is the greedy turn; candidate k > 0 is the policy sampled at the
 * candidate temperature with seed k.
 */
void PlayCandidate(Game& game, int side_index, int candidate, uint32_t base_seed,
                   const Settings& settings) {
    Side& side = *game.map->Side(side_index);
    if (candidate == 0) {
        ai::modern::PlayTurn(game, side, nullptr, settings.policy, nullptr);
        return;
    }
    ml::RolloutSampler sampler;
    sampler.temperature = settings.cand_temp;
    sampler.rng_state = base_seed ^ static_cast<uint32_t>(candidate * 2246822519U);
    ai::modern::PlayTurn(game, side, nullptr, settings.policy, &sampler);
}

/** @brief Plays the current side's turn with its own kind of AI (Modern: greedy). */
void PlayOwnTurn(Game& game, Side& side, const ml::PolicyNetwork& policy) {
    if (side.classic_ai)
        ai::PlayTurn(game, side, nullptr);
    else
        ai::modern::PlayTurn(game, side, nullptr, &policy, nullptr);
}

/**
 * @brief Plays on from a game in which the side has just played its turn.
 *
 * Plays @p rounds rounds (other sides and later turns of the side, all with
 * their normal AIs).
 *
 * @return The side's empire score at the end.
 */
float Rollout(Game& game, int side_index, int rounds, const ml::PolicyNetwork& policy) {
    const int stop_turn = game.turn + rounds;
    Side* current = game::EndTurn(game);
    int safety = 0;
    while (current && !game.over && !game.won && game.turn < stop_turn && safety++ < rounds * 12) {
        if (current->computer && current->alive) PlayOwnTurn(game, *current, policy);
        current = game::EndTurn(game);
    }
    return ml::ComputeEmpireScore(game, side_index);
}

/** @brief The rollout scores of one candidate turn. */
struct CandidateResult {
    std::unique_ptr<Game> after;  ///< Peek only: the game right after the candidate turn.
    double mean = 0.0;            ///< Mean empire score over the rollouts.
    double sd = 0.0;              ///< Standard deviation of the rollout scores.
    int k = 0;                    ///< Candidate number (0 = greedy).
    uint32_t base_seed = 0;       ///< Seed the sampled candidates are derived from.
};

/** @brief Builds and scores the K+1 candidates for the side in @p game (its turn is next). */
std::vector<CandidateResult> ScoreCandidates(const Game& game, int side_index,
                                             const Settings& settings) {
    std::vector<CandidateResult> out;
    const uint32_t base_seed = static_cast<uint32_t>(game.rng.state * 2654435761U) ^
                               static_cast<uint32_t>(game.turn * 40503U);
    for (int candidate = 0; candidate <= settings.candidates; candidate++) {
        CandidateResult result;
        std::vector<double> scores;
        if (settings.judge == Judge::kPeek) {
            result.after = CopyGame(game);
            PlayCandidate(*result.after, side_index, candidate, base_seed, settings);
            for (int rollout = 0; rollout < settings.rollouts; rollout++) {
                auto copy = CopyGame(*result.after);
                copy->rng.state ^= static_cast<uint32_t>((rollout + 1) * 0x9e3779b9U);
                scores.push_back(Rollout(*copy, side_index, settings.horizon, *settings.policy));
            }
        } else {
            for (int rollout = 0; rollout < settings.rollouts; rollout++) {
                auto copy = CopyGame(game);
                copy->rng.state ^= static_cast<uint32_t>(
                        (rollout + 1) * 0x9e3779b9U);  // fresh dice, also for the candidate turn
                if (settings.judge == Judge::kFair) RestrictToView(*copy, side_index);
                PlayCandidate(*copy, side_index, candidate, base_seed, settings);
                scores.push_back(Rollout(*copy, side_index, settings.horizon, *settings.policy));
            }
        }
        for (double score : scores) result.mean += score;
        result.mean /= std::max<size_t>(1, scores.size());
        for (double score : scores) result.sd += (score - result.mean) * (score - result.mean);
        result.sd = std::sqrt(result.sd / std::max<size_t>(1, scores.size() - 1));
        result.k = candidate;
        result.base_seed = base_seed;
        out.push_back(std::move(result));
    }
    return out;
}

/**
 * @brief Starts benchmark game number @p case_index, every side a level-2 computer.
 *
 * Consecutive pairs of cases share a scenario and preset (they swap seats).
 *
 * @param preset_out Receives the index of the preset used.
 * @param scen_out Receives the scenario name.
 */
std::unique_ptr<Game> NewBenchmarkGame(int case_index, uint32_t seed, int& preset_out,
                                       std::string& scen_out) {
    int pair_index = case_index / 2;
    scen_out = kScenarios[(pair_index + pair_index / 4) % kScenarios.size()];
    preset_out = pair_index % 4;
    const Preset& preset = kPresets[preset_out];
    game::NewGameOptions game_options;
    game_options.seed = seed;
    game_options.options = {{"hiddenMap", preset.hidden},
                            {"viewEnemies", 0},
                            {"diplomacy", preset.diplomacy},
                            {"neutralCities", preset.neutral},
                            {"quests", 1}};
    for (int side = 0; side < 8; side++) {
        game_options.sides[side].computer = true;
        game_options.sides[side].level = 2;
        game_options.sides[side].card = static_cast<int>((seed + side * 5) % 9);
    }
    return game::NewGame(kData, scen_out, game_options);
}

/**
 * @brief Puts the Modern AI on alternating seats (@p swap 0/1), the Classic AI on the rest,
 *        as in w2arena.
 * @return Which sides play the Modern AI.
 */
std::array<bool, 8> AssignSeats(Game& game, int swap) {
    std::array<bool, 8> modern{};
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        Side& side = game.map->sides[side_index];
        if (!side.in_use) continue;
        modern[side_index] = ((seat + swap) % 2 == 0);
        side.classic_ai = !modern[side_index];
        seat++;
    }
    return modern;
}

/** @brief The share of the standing owned cities that Modern sides hold (0.5 if none). */
double ModernShare(const Game& game, const std::array<bool, 8>& modern) {
    int modern_cities = 0, classic_cities = 0;
    for (const auto& city : game.map->cities) {
        if (city.razed || city.owner_index < 0 || city.owner_index >= 8) continue;
        (modern[city.owner_index] ? modern_cities : classic_cities)++;
    }
    return (modern_cities + classic_cities) > 0
                   ? static_cast<double>(modern_cities) / (modern_cities + classic_cities)
                   : 0.5;
}

}  // namespace

/**
 * @brief Runs the benchmark games in `probe` or `play` mode and prints the results.
 *
 * See the file comment for the arguments.
 */
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: w2search WEIGHTS [probe|play] [SEEDS] [K] [R] [H] [TURNS] "
                     "[peek|honest|fair] [CAND_TEMP] [SWITCH_Z] "
                     "[SEED_OFFSET]\n");
        return 2;
    }
    ml::PolicyNetwork policy;
    if (!policy.LoadJson(argv[1])) {
        std::fprintf(stderr, "w2search: cannot load weights '%s'\n", argv[1]);
        return 1;
    }
    std::string mode = argc > 2 ? argv[2] : "probe";
    int seeds = argc > 3 ? std::atoi(argv[3]) : 16;
    Settings settings;
    settings.policy = &policy;
    settings.candidates = argc > 4 ? std::atoi(argv[4]) : 4;
    settings.rollouts = argc > 5 ? std::atoi(argv[5]) : 4;
    settings.horizon = argc > 6 ? std::atoi(argv[6]) : 2;
    int turns = argc > 7 ? std::atoi(argv[7]) : 40;
    std::string judge = argc > 8 ? argv[8] : "honest";
    settings.judge = judge == "peek"   ? Judge::kPeek
                     : judge == "fair" ? Judge::kFair
                                       : Judge::kHonest;
    settings.cand_temp = argc > 9 ? static_cast<float>(std::atof(argv[9])) : 0.5F;
    settings.switch_z = argc > 10 ? static_cast<float>(std::atof(argv[10])) : 0.0F;
    const uint32_t seed_offset = argc > 11 ? static_cast<uint32_t>(std::atoi(argv[11])) : 0U;
    const bool play = (mode == "play");

    const int cases = seeds * 2;
    std::vector<double> shares(cases, 0.5), plain_shares(cases, 0.5);
    std::mutex results_mutex;
    // probe statistics
    long probes = 0, beaten = 0, beaten_clearly = 0;
    double gain_sum = 0.0, rollout_sd_sum = 0.0;
    std::atomic<int> next{0};
    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    unsigned workers = std::max(2U, std::thread::hardware_concurrency());
    for (unsigned worker = 0; worker < workers; worker++) {
        pool.emplace_back([&]() {
            for (int case_index; (case_index = next.fetch_add(1)) < cases;) {
                int preset = 0;
                std::string scenario;
                uint32_t seed = kSeedBase + seed_offset + static_cast<uint32_t>(case_index / 2);
                auto game = NewBenchmarkGame(case_index + 2 * static_cast<int>(seed_offset), seed,
                                             preset, scenario);
                auto modern = AssignSeats(*game, case_index % 2);
                Side* current = game::Begin(*game);
                int safety = 0;
                while (current && !game->over && !game->won && game->turn <= turns &&
                       safety++ < turns * 12) {
                    const int side_index = current->index;
                    const bool search_here =
                            current->computer && current->alive && modern[side_index] &&
                            (play ||
                             (!play && (game->turn == 10 || game->turn == 20 || game->turn == 30)));
                    if (search_here) {
                        auto candidates = ScoreCandidates(*game, side_index, settings);
                        size_t best = 0;
                        for (size_t candidate = 1; candidate < candidates.size(); candidate++)
                            if (candidates[candidate].mean > candidates[best].mean)
                                best = candidate;
                        if (best != 0 && settings.switch_z > 0.0F) {
                            // Conservative switching: noise alone must not pull us off the greedy
                            // turn
                            double std_error =
                                    std::sqrt((candidates[best].sd * candidates[best].sd +
                                               candidates[0].sd * candidates[0].sd) /
                                              std::max(1, settings.rollouts));
                            if (candidates[best].mean - candidates[0].mean <=
                                settings.switch_z * std_error)
                                best = 0;
                        }
                        if (!play) {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            probes++;
                            double gain = candidates[best].mean - candidates[0].mean;
                            gain_sum += gain;
                            if (best != 0) beaten++;
                            double std_error =
                                    std::sqrt((candidates[best].sd * candidates[best].sd +
                                               candidates[0].sd * candidates[0].sd) /
                                              std::max(1, settings.rollouts));
                            if (best != 0 && gain > 2.0 * std_error) beaten_clearly++;
                            rollout_sd_sum += candidates[0].sd;
                            PlayOwnTurn(*game, *current,
                                        policy);  // probe only: the game itself goes on unsearched
                        } else if (settings.judge == Judge::kPeek) {
                            game = std::move(candidates[best].after);
                            current = game->map->Side(side_index);
                        } else {
                            PlayCandidate(*game, side_index, candidates[best].k,
                                          candidates[best].base_seed, settings);  // real dice
                        }
                    } else if (current->computer && current->alive) {
                        PlayOwnTurn(*game, *current, policy);
                    }
                    current = game::EndTurn(*game);
                }
                double share = ModernShare(*game, modern);
                std::lock_guard<std::mutex> lock(results_mutex);
                shares[case_index] = share;
                std::printf("  case %3d %-8s %-14s share %.3f\n", case_index, scenario.c_str(),
                            kPresets[preset].label, share);
                std::fflush(stdout);
            }
        });
    }
    for (auto& thread : pool) thread.join();
    double sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();

    double mean = 0.0;
    for (double share : shares) mean += share;
    mean /= std::max(1, cases);
    double var = 0.0;
    for (double share : shares) var += (share - mean) * (share - mean);
    var /= std::max(1, cases - 1);
    std::printf("=== %s | %s | %d games | K=%d R=%d H=%d T=%.2f z=%.1f | %.0f s ===\n",
                mode.c_str(), judge.c_str(), cases, settings.candidates, settings.rollouts,
                settings.horizon, settings.cand_temp, settings.switch_z, sec);
    std::printf("Modern city share %.1f%% +/- %.1f%% (95%%)\n", 100.0 * mean,
                196.0 * std::sqrt(var / std::max(1, cases)));
    if (!play && probes > 0) {
        std::printf(
                "probes %ld | a sampled turn scored best in %.0f%% | clearly best (> 2 SE) in "
                "%.0f%% | "
                "mean gain of best over greedy %.4f empire-score | rollout SD of greedy %.4f\n",
                probes, 100.0 * beaten / probes, 100.0 * beaten_clearly / probes, gain_sum / probes,
                rollout_sd_sum / probes);
    }
    return 0;
}
