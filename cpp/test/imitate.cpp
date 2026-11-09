// Imitation of the Classic AI (`w2imitate`): a head start for the Modern AI.
//
// The 1993 Classic AI is stronger than the Modern AI exactly where the Modern
// AI is weak (garrisons, total war, long games). This tool plays games in which
// every side is the Classic AI. Before a recorded side's turn it asks the
// Modern AI, without moving anything, which mission options it would weigh for
// each army group (modern::snapshotMissions, fair information). After the
// Classic AI has played, it labels each group's decision with the option that
// matches what the Classic AI actually did with the group's first army:
//   stayed in its city               -> hold that city
//   ended in an own city             -> reinforce / capture that city
//   walking to a city (Classic order) -> that city
//   otherwise moved                  -> the target it ended closest to
// and the coalition size by how many of the group went along. MissionNet is
// then trained by behaviour cloning (cross-entropy on those labels), starting
// from the given weights; the other heads are untouched.
//
//   bazel-bin/cpp/w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"

ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(int, games, 144, "Number of Classic AI games to record (0 = rewrite weights only).");
ABSL_FLAG(int, turns, 40, "Maximum turns per recorded Classic AI game.");
ABSL_FLAG(int, epochs, 6, "Behaviour-cloning epochs over the dataset.");
ABSL_FLAG(double, lr, 1e-3, "Adam learning rate.");

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

/** @brief The built-in scenarios the games cycle through. */
const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};
/** @brief One combination of game options a game is played with. */
struct Preset {
    int hidden;     ///< The "hiddenMap" option (fog of war).
    int diplomacy;  ///< The "diplomacy" option.
    int neutral;    ///< The "neutralCities" option.
};
/** @brief The option presets; game `i` uses preset `i % 4`. */
const Preset kPresets[4] = {{0, 0, 0}, {0, 1, 1}, {1, 1, 2}, {1, 0, 1}};
constexpr uint32_t kSeedBase = 600000;  ///< Disjoint from the evaluation seeds (700000+).
constexpr float kBcTemperature = 0.5F;  ///< The training temperature PPO uses.

/** @brief The mission option kinds (MissionSnapshot::kind) the labelling looks at. */
enum Kind {
    kHold = 0,         ///< Stay in (hold) a city.
    kNeutralCity = 1,  ///< Capture a neutral city.
    kEnemyCity = 2,    ///< Capture an enemy city.
    kReinforce = 7     ///< Reinforce an own city.
};

/** @brief One labelled group decision: the Modern AI's options and the Classic AI's choice. */
struct Example {
    std::vector<ml::MissionInVec> candidates;  ///< MissionNet inputs, one per option.
    std::vector<float> log_mult;               ///< Log multiplicity of each option.
    std::vector<int> kind;                     ///< Kind of each option (see Kind).
    int label = 0;                             ///< Index of the option the Classic AI took.
    bool validation = false;                   ///< Held out for validation, not trained on.
};

/** @brief The distance between two tiles in king moves (Chebyshev distance). */
int Dist(int from_x, int from_y, int to_x, int to_y) {
    return std::max(std::abs(from_x - to_x), std::abs(from_y - to_y));
}

/**
 * @brief The option that matches what the Classic AI did with the group, or -1.
 *
 * The Modern AI's first decision for a group is "where does a coalition led by
 * the group's first army go, and how big is it"; the armies left behind get
 * their own decisions afterwards. So the label follows the group's *moving*
 * part: if any of its armies left, the first decision is the target they went
 * for, with as many armies as went there; only when none left is it "hold".
 *
 * @param game The game after the Classic AI has played the turn.
 * @param my_side Index of the side whose turn it was.
 * @param snapshot The Modern AI's options for the group, taken before the turn.
 */
int LabelSnapshot(Game& game, int my_side, const ai::modern::MissionSnapshot& snapshot) {
    const size_t candidate_count = snapshot.candidates.size();
    City* start_city =
            snapshot.start_city != kNone ? game.GetMap().CityAt(snapshot.start_city) : nullptr;
    auto stayed = [&](Army* army) {
        if (army->X() == kNone) return false;
        if (start_city) return game::CityAt(game, army->X(), army->Y()) == start_city;
        return army->X() == snapshot.start_x && army->Y() == snapshot.start_y;
    };

    // The reference army: the first of the group (in the Modern AI's order) that left alive
    Army* mover = nullptr;
    int movers = 0, dead_movers = 0;
    for (std::uintptr_t army_id : snapshot.group) {
        Army* army = reinterpret_cast<Army*>(army_id);
        if (!game.Alive(army)) {
            dead_movers++;
            continue;
        }
        if (stayed(army)) continue;
        movers++;
        if (!mover) mover = army;
    }
    // the seed first if it moved (it leads the Modern AI's coalition)
    Army* seed = reinterpret_cast<Army*>(snapshot.seed);
    if (game.Alive(seed) && !stayed(seed)) mover = seed;

    auto pick_size = [&](const std::vector<size_t>& matches, int along) {
        size_t best = matches[0];
        for (size_t option : matches)
            if (std::abs(snapshot.stack_size[option] - along) <
                std::abs(snapshot.stack_size[best] - along))
                best = option;
        return static_cast<int>(best);
    };

    if (!mover) {
        if (dead_movers > 0) return -1;  // the ones that left all fell: target unknown
        if (!start_city) return -1;      // stood still in the field: no Modern option says that
        std::vector<size_t> holds;
        for (size_t i = 0; i < candidate_count; i++)
            if (snapshot.kind[i] == kHold && snapshot.city_index[i] == snapshot.start_city)
                holds.push_back(i);
        if (holds.empty()) return -1;
        return pick_size(holds, static_cast<int>(snapshot.group.size()));
    }

    const int end_x = mover->X(), end_y = mover->Y();
    if (end_x == kNone) return -1;
    City* end_city = game::CityAt(game, end_x, end_y);
    auto city_options = [&](int city_index) {
        std::vector<size_t> out;
        for (size_t i = 0; i < candidate_count; i++)
            if (snapshot.city_index[i] == city_index &&
                (snapshot.kind[i] == kNeutralCity || snapshot.kind[i] == kEnemyCity ||
                 snapshot.kind[i] == kReinforce))
                out.push_back(i);
        return out;
    };
    std::vector<size_t> matches;
    const ClassicArmyOrder& mover_ord = ai::core::OrderOf(game, mover);
    if (end_city && end_city->OwnedBy(my_side) && end_city != start_city) {
        matches = city_options(end_city->Index());
    } else if (mover_ord.order == ai::core::kOrderCity && mover_ord.dest != kNone) {
        matches = city_options(mover_ord.dest);
    } else {
        // the target it ended closest to, among those it moved towards
        int best_d = 1 << 30;
        for (size_t i = 0; i < candidate_count; i++) {
            if (snapshot.kind[i] == kHold) continue;
            int d_end = Dist(end_x, end_y, snapshot.tx[i], snapshot.ty[i]);
            int d_start = Dist(snapshot.start_x, snapshot.start_y, snapshot.tx[i], snapshot.ty[i]);
            if (d_end >= d_start) continue;
            if (d_end < best_d) {
                best_d = d_end;
                matches.clear();
            }
            if (d_end == best_d) matches.push_back(i);
        }
        if (!matches.empty()) {
            size_t first = matches[0];
            std::vector<size_t> same;
            for (size_t option : matches)
                if (snapshot.kind[option] == snapshot.kind[first] &&
                    snapshot.tx[option] == snapshot.tx[first] &&
                    snapshot.ty[option] == snapshot.ty[first])
                    same.push_back(option);
            matches = same;
        }
    }
    if (matches.empty()) return -1;
    // coalition size: the movers that ended with the reference army
    int along = 0;
    for (std::uintptr_t army_id : snapshot.group) {
        Army* army = reinterpret_cast<Army*>(army_id);
        if (!game.Alive(army) || army->X() == kNone || stayed(army)) continue;
        City* city = game::CityAt(game, army->X(), army->Y());
        if ((end_city && city == end_city) || Dist(army->X(), army->Y(), end_x, end_y) <= 1)
            along++;
    }
    (void)movers;
    return pick_size(matches, std::max(1, along));
}

/**
 * @brief Starts game number @p game_index with every side played by the Classic AI.
 *
 * The seed, option preset and scenario follow from the index; every fifth game
 * is on a random map, as in training.
 *
 * @param files Receives the generated map files of a random-map game; they must
 *        outlive the game.
 */
std::unique_ptr<Game> NewClassicGame(int game_index, const std::string& data_dir,
                                     randommap::Files& files) {
    uint32_t seed = kSeedBase + static_cast<uint32_t>(game_index) * 13U;
    const Preset& preset = kPresets[game_index % 4];
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
    if (game_index % 5 == 4) {  // a fifth on random maps, as in training
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options map_options;
        map_options.data_dir = data_dir;
        map_options.rng = &map_rng;
        map_options.sliders = {3, 3, 2, 3};
        map_options.allies = preset.diplomacy != 0;
        auto generated = randommap::GenerateNow(map_options);
        ABSL_CHECK_OK(generated);
        files = std::move(*generated);
        game_options.memory_files = &files;
        auto created = game::NewGame(data_dir, "RANDOM", game_options);
        ABSL_CHECK_OK(created);
        return std::move(*created);
    }
    auto created =
            game::NewGame(data_dir, kScenarios[(game_index / 4) % kScenarios.size()], game_options);
    ABSL_CHECK_OK(created);
    return std::move(*created);
}

/**
 * @brief Adds the behaviour-cloning gradient of -log p(label) for one decision into @p grad.
 * @param correct Set to whether greedy play (the highest utility) picks the label.
 * @return The decision's loss, -log p(label).
 */
double BcAccumulate(const ml::PolicyNetwork& net, const Example& example, ml::PolicyNetwork& grad,
                    bool& correct) {
    const size_t candidate_count = example.candidates.size();
    const float inverse_temperature = 1.0F / kBcTemperature;
    std::vector<float> utilities(candidate_count), logits(candidate_count),
            probabilities(candidate_count);
    float max_logit = -1e30F;
    int argmax = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        utilities[i] = net.MissionHead().Forward(example.candidates[i])[0];
        // No multiplicity prior here: greedy play (T = 0.02) ranks options by
        // utility alone, so the utilities themselves must put the label on top
        logits[i] = utilities[i] * inverse_temperature;
        if (logits[i] > max_logit) max_logit = logits[i];
        if (utilities[i] > utilities[argmax]) argmax = static_cast<int>(i);
    }
    float sum = 0.0F;
    for (size_t i = 0; i < candidate_count; i++) {
        probabilities[i] = std::exp(logits[i] - max_logit);
        sum += probabilities[i];
    }
    for (float& probability : probabilities) probability /= sum;
    correct = (argmax == example.label);  // greedy play takes the highest utility
    for (size_t i = 0; i < candidate_count; i++) {
        float logit_grad =
                inverse_temperature *
                (probabilities[i] - (i == static_cast<size_t>(example.label) ? 1.0F : 0.0F));
        if (logit_grad != 0.0F)
            net.MissionHead().Backward(example.candidates[i], {logit_grad, 0.0F},
                                       grad.MissionHead());
    }
    return -std::log(std::max(1e-12F, probabilities[example.label]));
}

}  // namespace

/**
 * @brief Plays the Classic AI games, labels the Modern AI's options and trains MissionNet.
 *
 * Usage: `w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]`. The weights
 * are written to OUT.json after every epoch; GAMES = 0 only rewrites IN.json in
 * the current format.
 */
int main(int argc, char** argv) {
    auto args =
            ParseCliFlags(argc, argv, "w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]");
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]\n");
        return 2;
    }
    std::string data_dir = absl::GetFlag(FLAGS_data);
    ml::PolicyNetwork net;
    if (absl::Status status = net.LoadJson(args[0]); !status.ok()) {
        std::fprintf(stderr, "w2imitate: %s\n", std::string(status.message()).c_str());
        return 1;
    }
    const int games = args.size() > 2 ? static_cast<int>(ToInt(args[2]).value_or(144))
                                      : absl::GetFlag(FLAGS_games);
    const int turns = args.size() > 3 ? static_cast<int>(ToInt(args[3]).value_or(40))
                                      : absl::GetFlag(FLAGS_turns);
    const int epochs = args.size() > 4 ? static_cast<int>(ToInt(args[4]).value_or(6))
                                       : absl::GetFlag(FLAGS_epochs);
    const float learning_rate = args.size() > 5
                                        ? static_cast<float>(std::strtod(args[5].c_str(), nullptr))
                                        : static_cast<float>(absl::GetFlag(FLAGS_lr));
    const unsigned threads = std::max(2U, std::thread::hardware_concurrency());
    if (games == 0) {  // just rewrite the weights in the current format
        return net.SaveJson(args[1], 0, 0.0F).ok() ? 0 : 1;
    }

    // ---- data: Classic games, two recorded sides each
    auto start_time = std::chrono::steady_clock::now();
    std::vector<Example> data;
    std::mutex data_mutex;
    std::atomic<int> next{0};
    long snapshots = 0, labelled = 0;
    std::vector<std::thread> pool;
    for (unsigned worker = 0; worker < threads; worker++) {
        pool.emplace_back([&]() {
            for (int game_index; (game_index = next.fetch_add(1)) < games;) {
                randommap::Files files;
                auto game_ptr = NewClassicGame(game_index, data_dir, files);
                Game& game = *game_ptr;
                std::vector<int> in_use;
                for (int side = 0; side < 8; side++) {
                    if (game.GetMap().Sides()[side].InUse()) in_use.push_back(side);
                    ai::SetSideClassicAi(game, game.GetMap().Sides()[side], true);
                }
                if (in_use.empty()) continue;
                const int recorded_a = in_use[static_cast<size_t>(game_index) % in_use.size()],
                          recorded_b = in_use[static_cast<size_t>(game_index + 3) % in_use.size()];
                const bool validation = (game_index % 10 == 9);
                std::vector<Example> local;
                long local_snaps = 0;
                Side* current = game::Begin(game);
                int safety = 0;
                while (current && !game.IsOver() && !game.IsWon() && game.Turn() <= turns &&
                       safety++ < turns * 12) {
                    if (current->IsComputerControlled() && current->IsAlive()) {
                        const bool recorded =
                                (current->Index() == recorded_a || current->Index() == recorded_b);
                        std::vector<ai::modern::MissionSnapshot> snaps;
                        if (recorded) snaps = ai::modern::SnapshotMissions(game, *current, net);
                        ai::PlayTurn(game, *current, nullptr);
                        for (auto& snapshot : snaps) {
                            local_snaps++;
                            if (snapshot.candidates.size() < 2) continue;
                            int label = LabelSnapshot(game, current->Index(), snapshot);
                            if (label < 0) continue;
                            Example example;
                            example.candidates = std::move(snapshot.candidates);
                            example.log_mult = std::move(snapshot.log_mult);
                            example.kind = snapshot.kind;
                            example.label = label;
                            example.validation = validation;
                            local.push_back(std::move(example));
                        }
                    }
                    current = game::EndTurn(game);
                }
                std::lock_guard<std::mutex> lock(data_mutex);
                snapshots += local_snaps;
                labelled += static_cast<long>(local.size());
                for (auto& example : local) data.push_back(std::move(example));
            }
        });
    }
    for (auto& thread : pool) thread.join();
    double gen_sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    std::vector<size_t> train, valid;
    for (size_t i = 0; i < data.size(); i++) (data[i].validation ? valid : train).push_back(i);
    std::printf(
            "=== w2imitate | %d Classic games x %d turns | %ld group decisions, %ld labelled "
            "(%.0f%%) | "
            "%zu train / %zu validation | %.0f s ===\n",
            games, turns, snapshots, labelled, 100.0 * labelled / std::max(1L, snapshots),
            train.size(), valid.size(), gen_sec);
    std::fflush(stdout);

    // ---- behaviour cloning on MissionNet
    auto evaluate = [&](const std::vector<size_t>& indices, double& loss, double& acc) {
        std::atomic<size_t> next_index{0};
        std::vector<double> worker_loss(threads, 0.0), worker_correct(threads, 0.0);
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < threads; worker++) {
            workers.emplace_back([&, worker]() {
                ml::PolicyNetwork scratch;
                scratch.Zero();
                for (size_t i; (i = next_index.fetch_add(1)) < indices.size();) {
                    bool correct = false;
                    worker_loss[worker] += BcAccumulate(net, data[indices[i]], scratch, correct);
                    worker_correct[worker] += correct ? 1.0 : 0.0;
                }
            });
        }
        for (auto& thread : workers) thread.join();
        loss = acc = 0.0;
        for (unsigned worker = 0; worker < threads; worker++) {
            loss += worker_loss[worker];
            acc += worker_correct[worker];
        }
        loss /= std::max<size_t>(1, indices.size());
        acc /= std::max<size_t>(1, indices.size());
    };
    // What the Classic AI chose, and what the network picks greedily, by target kind
    auto kind_histogram = [&](const char* label) {
        const char* names[8] = {"hold", "neutral", "enemyCity", "enemyStack",
                                "ruin", "temple",  "fog",       "reinforce"};
        std::array<long, 8> classic{}, modern{};
        for (size_t example_index : valid) {
            const Example& example = data[example_index];
            classic[example.kind[example.label]]++;
            int greedy_choice = 0;
            float best = -1e30F;
            for (size_t candidate = 0; candidate < example.candidates.size(); candidate++) {
                float utility = net.MissionHead().Forward(example.candidates[candidate])[0];
                if (utility > best) {
                    best = utility;
                    greedy_choice = static_cast<int>(candidate);
                }
            }
            modern[example.kind[greedy_choice]]++;
        }
        std::printf("%s kinds (Classic label / network greedy):", label);
        for (int kind = 0; kind < 8; kind++)
            std::printf(" %s %ld/%ld", names[kind], classic[kind], modern[kind]);
        std::printf("\n");
    };
    kind_histogram("before");
    double valid_loss = 0, valid_accuracy = 0;
    evaluate(valid, valid_loss, valid_accuracy);
    std::printf("before: validation loss %.3f | greedy agreement with Classic %.1f%%\n", valid_loss,
                100.0 * valid_accuracy);
    std::fflush(stdout);

    // Adam over the flattened parameters
    std::vector<float> params = net.Flatten();
    const size_t param_count = params.size();
    std::vector<float> first_moment(param_count, 0.0F), second_moment(param_count, 0.0F);
    int step = 0;
    std::mt19937 rng(7);
    const size_t batch = 512;
    for (int epoch = 1; epoch <= epochs; epoch++) {
        std::shuffle(train.begin(), train.end(), rng);
        double train_loss = 0.0;
        for (size_t batch_start = 0; batch_start < train.size(); batch_start += batch) {
            size_t batch_end = std::min(train.size(), batch_start + batch);
            std::atomic<size_t> next_index{batch_start};
            std::vector<ml::PolicyNetwork> grads(threads);
            std::vector<double> worker_loss(threads, 0.0);
            std::vector<std::thread> workers;
            for (unsigned worker = 0; worker < threads; worker++) {
                grads[worker].Zero();
                workers.emplace_back([&, worker]() {
                    for (size_t i; (i = next_index.fetch_add(1)) < batch_end;) {
                        bool correct = false;
                        worker_loss[worker] +=
                                BcAccumulate(net, data[train[i]], grads[worker], correct);
                    }
                });
            }
            for (auto& thread : workers) thread.join();
            std::vector<float> grad_sum(param_count, 0.0F);
            for (unsigned worker = 0; worker < threads; worker++) {
                auto worker_grad = grads[worker].Flatten();
                for (size_t param = 0; param < param_count; param++)
                    grad_sum[param] += worker_grad[param];
                train_loss += worker_loss[worker];
            }
            const float scale = 1.0F / static_cast<float>(batch_end - batch_start);
            step++;
            float bias_correction1 = 1.0F - std::pow(0.9F, static_cast<float>(step)),
                  bias_correction2 = 1.0F - std::pow(0.999F, static_cast<float>(step));
            for (size_t param = 0; param < param_count; param++) {
                float mean_grad = grad_sum[param] * scale;
                first_moment[param] = 0.9F * first_moment[param] + 0.1F * mean_grad;
                second_moment[param] =
                        0.999F * second_moment[param] + 0.001F * mean_grad * mean_grad;
                params[param] -= learning_rate * (first_moment[param] / bias_correction1) /
                                 (std::sqrt(second_moment[param] / bias_correction2) + 1e-8F);
            }
            net.Unflatten(params);
        }
        evaluate(valid, valid_loss, valid_accuracy);
        std::printf("epoch %d: train loss %.3f | validation loss %.3f | greedy agreement %.1f%%\n",
                    epoch, train_loss / std::max<size_t>(1, train.size()), valid_loss,
                    100.0 * valid_accuracy);
        std::fflush(stdout);
        std::string path = args[1];
        net.SaveJson(path, epoch, 0.0F).IgnoreError();
    }
    kind_histogram("after");
    return 0;
}
