/**
 * @file
 * @brief Self-play reinforcement-learning trainer for the Modern AI (`w2arena`).
 *
 * Trains the 4-head PolicyNetwork with PPO + GAE in all-computer games.
 *
 * Objective. A side's reward each turn is the change in its empire score S
 * (ComputeEmpireScore: 2 x city share + 0.5 x income share + 0.25 x army share,
 * plus a small reconnaissance term under Hidden Map), measured from the start
 * of its turn to the start of its next one, so losses suffered during rivals'
 * turns count. Winning adds +1 and elimination -0.5 (and S drops to 0).
 * Training games are cut off after --turns turns; that cut-off is a time limit,
 * not an end of the game, so the critic's estimate is bootstrapped there.
 * The objective is therefore the discounted growth of S with no fixed horizon,
 * which matches open-ended real games. S is a hand-chosen definition of doing
 * well; everything else (targets, coalitions, production, diplomacy) is learned.
 *
 * Opponents. Each game is either a mirror match (every side plays the current
 * learner and is recorded), a league match (half the sides play a snapshot
 * from the hall of fame), or -- only with --classic-frac > 0 -- a match against
 * the 1993 Classic AI. By default the Classic AI is used solely for evaluation.
 *
 * Evaluation. Two disjoint seed sets, both disjoint from training seeds:
 *   - selection: decides which checkpoint is kept (--select-seeds);
 *   - report: measured once at the end, never used for any decision
 *     (--report-seeds).
 *
 * Each seed is played twice with the Modern/Classic side assignment swapped,
 * half the sides each. The figure of merit is the Modern AI's share of the
 * cities held by the two camps when the game is cut off, with a 95% interval.
 *
 * Usage (from the repository root):
 *
 *     bazel-bin/cpp/w2arena [--iterations N] [--games-per-iter G] [--turns T]
 *                       [--eval-every E] [--eval-turns T] [--select-seeds S]
 *                       [--report-seeds R] [--classic-frac F] [--from-scratch]
 *                       [--out PATH] [--log CSV] [--benchmark-only]
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/strings/str_format.h"

#include "opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"
#include "warlords/save.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(int, iterations, 300, "Training iterations to run.");
ABSL_FLAG(int, games_per_iter, 48, "Rollout games per iteration.");
ABSL_FLAG(int, turns, 40, "Training games cut-off turn.");
ABSL_FLAG(int, eval_every, 10, "Iterations between selection evaluations.");
ABSL_FLAG(int, eval_turns, 40, "Evaluation games cut-off turn.");
ABSL_FLAG(int, select_seeds, 12, "Seeds in the selection set.");
ABSL_FLAG(int, report_seeds, 32, "Seeds in the held-out report set.");
ABSL_FLAG(int, epochs, 4, "PPO passes over each iteration's turns.");
ABSL_FLAG(int, minibatches, 8, "Minibatches per epoch.");
ABSL_FLAG(double, lr, 3.0e-3, "Initial Adam learning rate.");
ABSL_FLAG(double, temp_start, 1.0, "Initial sampling temperature.");
ABSL_FLAG(double, temp_end, w2::ai::ml::kTrainTemperatureEnd, "Final sampling temperature.");
ABSL_FLAG(double, eval_temp, w2::ai::ml::kDeployTemperature, "Evaluation sampling temperature.");
ABSL_FLAG(uint32_t, seed_offset, 0, "Offset added to every training seed.");
ABSL_FLAG(double, classic_frac, 0.0, "Share of training games played against Classic AI.");
ABSL_FLAG(double, random_map_frac, 0.0, "Share of training games played on random maps.");
ABSL_FLAG(int, active_sides, 8, "Active sides in play (2 to 8).");
ABSL_FLAG(int, crop_margin, 0, "Crop margin around active capitals (0 = full map).");
ABSL_FLAG(double, lambda, 0.95, "GAE lambda.");
ABSL_FLAG(double, local_weight, 1.0, "Weight of per-stack local advantage.");
ABSL_FLAG(std::string, in, "",
          "Optional initial checkpoint path to warm-start from without overwriting it.");
ABSL_FLAG(std::string, out, "cpp/ai_weights.json", "Checkpoint path read and written.");
ABSL_FLAG(std::string, log, "", "CSV file path with one line per iteration.");
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(bool, solo_learner, false, "Play one learner seat against K-1 opponents.");
ABSL_FLAG(bool, from_scratch, false, "Ignore any existing --out checkpoint.");
ABSL_FLAG(bool, benchmark_only, false, "Evaluate the checkpoint and exit.");
ABSL_FLAG(std::string, eval_mode, "mixed", "Evaluation seat split: mixed or solo.");
ABSL_FLAG(std::string, compare, "", "B checkpoint path for paired A/B comparison.");
ABSL_FLAG(int, dev_seeds, 0, "Benchmark/compare seed count on the development set.");
ABSL_FLAG(bool, breakdown, false, "Print city shares broken down by preset and scenario.");
ABSL_FLAG(int, critic_warmup, 0, "Iterations where only the critics learn.");
ABSL_FLAG(double, mixed_frac, 0.0, "Fraction of solo-learner games played 4v4 instead.");
ABSL_FLAG(int, save_every, 0, "Write <out>.iterN.json snapshots every N iterations.");
ABSL_FLAG(double, raider_frac, 0.0, "Share of Classic training seats played by Raider.");
ABSL_FLAG(double, warden_frac, 0.0, "Share of Classic training seats played by Warden.");
ABSL_FLAG(int, eval_preset, -1, "Evaluate on one preset only (0..3, -1 = all).");
ABSL_FLAG(double, departure_weight, 0.0, "Credit a city loss to the move that vacated it.");
ABSL_FLAG(double, lr_end, 3.0e-4, "Final Adam learning rate (linear decay).");
ABSL_FLAG(bool, mup, false, "Scale hidden-layer learning rates by 1/width.");
ABSL_FLAG(bool, clip_per_head, false, "Clip gradient norm per head instead of globally.");
ABSL_FLAG(bool, yield_blockers, false, "Re-decide on the learner (or A) when blocked.");
ABSL_FLAG(bool, b_yield_blockers, false, "Re-decide on --compare B when blocked.");
ABSL_FLAG(bool, a_legacy_blockers, false, "Disable obstacle detouring on A for paired A/B.");
ABSL_FLAG(std::string, b_detour_bias, "",
          "Override detour bias on --compare B (empty = keep checkpoint value).");
ABSL_FLAG(int, eval_level, -1, "Override Modern difficulty tier in eval games (-1 = default).");
ABSL_FLAG(bool, a_no_personalities, false, "Disable 27-card personalities on --out (A) in eval.");
ABSL_FLAG(bool, b_no_personalities, false, "Disable 27-card personalities on --compare B.");
ABSL_FLAG(bool, b_warlord_lookahead, false,
          "Enable fair 1-round Warlord lookahead on --compare B.");
ABSL_FLAG(double, army_weight, 0.25, "Army-power share weight in the empire score.");
ABSL_FLAG(bool, upgrade_v7, false, "Upgrade a loaded legacy (< v7) checkpoint to v7.");
ABSL_FLAG(bool, upgrade_v8, false, "Upgrade a loaded checkpoint to v8.");
ABSL_FLAG(bool, use_commander, false, "Enable and train the v7/v8 Commander posture projection.");
ABSL_FLAG(bool, use_muster, false, "Enable and train v8 MusterNet castle hub selection.");
ABSL_FLAG(bool, use_standard, false, "Enable and train v8 MusterNet Side Standard planting.");
ABSL_FLAG(bool, avoid_blockers, false,
          "Enable obstacle detouring and train detourBias + MissionNet detour choices.");
ABSL_FLAG(bool, causal_offturn, false,
          "Attribute off-turn defensive battle and vacated-city outcomes to per-stack G_pool.");
ABSL_FLAG(double, offturn_repel_weight, 0.5,
          "Saved-city weight credited to garrisons that actively repel an enemy siege.");
ABSL_FLAG(double, offturn_vacate_weight, 0.5,
          "Lost-city weight subtracted from G_pool when a move vacates a city that falls.");
ABSL_FLAG(double, explore_eps, 0.0,
          "Fraction of rollout decisions sampled at --explore-temp instead of sharp temperature.");
ABSL_FLAG(double, explore_temp, 0.65,
          "Exploration temperature used on --explore-eps rollout decisions.");
ABSL_FLAG(bool, temp_rescale, false,
          "Rescale per-decision policy gradients by T_step / 0.50 for temperature-invariant steps.");
ABSL_FLAG(int, threads, 0, "Worker thread count (0 = hardware_concurrency).");
ABSL_FLAG(bool, econ_lineage, false,
          "Propagate post-birth unit returns back to BuySlot, Produce, and Vector decisions.");
ABSL_FLAG(double, econ_lineage_weight, 1.0,
          "Weight on discounted post-birth unit return added to economy advantages.");
ABSL_FLAG(bool, econ_family_norm, false,
          "Standardize and frequency-balance EconNet advantages separately per action family.");
ABSL_FLAG(double, concentration_weight, 0.0,
          "Stack-concentration synergy weight in ComputeEmpireScore.");
ABSL_FLAG(std::string, frozen_opponent, "", "Fixed Modern checkpoint path for opponent seats.");
ABSL_FLAG(double, frozen_frac, 0.0, "Share of non-learner seats played by --frozen-opponent.");
ABSL_FLAG(int, search_k, 0, "Candidate turns per learner turn (0 = no search training).");
ABSL_FLAG(int, search_r, 2, "Rollout copies per candidate turn.");
ABSL_FLAG(int, search_h, 2, "Rounds played forward after a candidate turn.");
ABSL_FLAG(std::string, eval_opponent, "classic",
          "Evaluation opponent personality: classic, raider, warden, or frozen.");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

/**
 * @brief A combination of game options that training and evaluation games use.
 *
 * Games rotate through the presets so the policy learns to play with and
 * without hidden map, diplomacy and neutral cities.
 */
struct TrainingPreset {
    const char* label;   ///< Short name printed in the per-preset breakdown.
    int hidden_map;      ///< Value of the `hiddenMap` game option (0 or 1).
    int diplomacy;       ///< Value of the `diplomacy` game option (0 or 1).
    int neutral_cities;  ///< Value of the `neutralCities` game option (0 to 2).
};

/** @brief The four presets games cycle through. */
constexpr TrainingPreset kPresets[4] = {
    {"Beginner-Open", 0, 0, 0},
    {"Interm-Diplo", 0, 1, 1},
    {"Adv-Hidden", 1, 1, 2},
    {"Fog-TotalWar", 1, 0, 1},
};

/** @brief The shipped scenarios games cycle through. */
const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};

// Seed ranges: training uses iter * 1009 + game * 97 + 17 (< 700000 for any
// sane run); evaluation sets live well above it and never overlap.
/** @brief First seed of the selection set, which picks the checkpoint kept. */
constexpr uint32_t kSelectSeedBase = 700000;
/** @brief First seed of the held-out report set, measured once at the end. */
constexpr uint32_t kReportSeedBase = 800000;
/**
 * @brief First seed of the development set (--compare / --dev-seeds).
 *
 * Development A/B comparisons use their own set, so neither in-run selection
 * nor the held-out report is spent on tuning.
 */
constexpr uint32_t kDevSeedBase = 750000;

/** @brief How evaluation games split the seats between Modern and Classic AI. */
enum class EvalMode {
    kMixed,  ///< Half the sides each, alternating seats (4 vs 4).
    kSolo,   ///< One Modern side against K-1 Classic sides.
};

/** @brief Training and evaluation settings, filled from the command line. */
struct Config {
    std::string data_dir = "original";             ///< --data: game data directory.
    std::string in_path;                           ///< --in: optional starting checkpoint.
    std::string out_path = "cpp/ai_weights.json";  ///< --out: checkpoint read and written.
    std::string log_path;      ///< --log: CSV file with one line per iteration (none if empty).
    int iterations = 300;      ///< --iterations: training iterations to run.
    int games_per_iter = 48;   ///< --games-per-iter: rollout games per iteration.
    int max_turns = 40;        ///< --turns: training games are cut off after this turn.
    int eval_every = 10;       ///< --eval-every: iterations between selection evaluations.
    int eval_turns = 40;       ///< --eval-turns: evaluation games are cut off after this turn.
    int select_seeds = 12;     ///< --select-seeds: seeds in the selection set.
    int report_seeds = 32;     ///< --report-seeds: seeds in the held-out report set.
    int epochs = 4;            ///< --epochs: PPO passes over each iteration's turns.
    int minibatches = 8;       ///< --minibatches: minibatches per epoch.
    float lr_start = 3.0e-3F;  ///< --lr: learning rate at the first iteration.
    float lr_end = 3.0e-4F;    ///< Learning rate at the last iteration (linear decay).
    float temp_start = 1.0F;   ///< --temp-start: sampling temperature at the first iteration.
    float temp_end = ml::kTrainTemperatureEnd;  ///< --temp-end: temperature at the last one.
    float eval_temp =
            ml::kDeployTemperature;    ///< --eval-temp: temperature evaluation games sample at.
    float clip_eps = 0.2F;             ///< PPO clipping range of the probability ratio.
    float ent_coef = 0.01F;            ///< Weight of the entropy bonus.
    float value_coef = 0.5F;           ///< Weight of the critics' losses.
    float gamma = 0.97F;               ///< Discount per own turn.
    float gae_lambda = 0.95F;          ///< --lambda: GAE lambda.
    float local_weight = 1.0F;         ///< --local-weight: weight of the per-stack local advantage.
    float target_kl = 0.03F;           ///< An iteration stops its epochs once the KL exceeds this.
    float max_grad_norm = 0.5F;        ///< Gradients are clipped to this global norm.
    float classic_frac = 0.0F;         ///< --classic-frac: share of training games against Classic.
    float random_map_frac = 0.0F;      ///< --random-map-frac: share of games on a random map.
    int active_sides = 8;              ///< --active-sides: sides in play (2 to 8).
    int crop_margin = 0;               ///< --crop-margin: crop the map around the sides in play.
    uint32_t seed_offset = 0;          ///< --seed-offset: added to every training seed.
    float win_reward = 1.0F;           ///< Terminal reward for winning the game.
    float elimination_reward = -0.5F;  ///< Terminal reward for being eliminated.
    bool solo_learner = false;         ///< --solo-learner: one learner seat against Classic AI.
    bool from_scratch = false;         ///< --from-scratch: ignore an existing checkpoint.
    bool benchmark_only = false;       ///< --benchmark-only: evaluate the checkpoint and stop.
    EvalMode eval_mode = EvalMode::kMixed;  ///< --eval-mode: seat split of evaluation games.
    std::string compare_path;               ///< --compare: B checkpoint, A is --out
    bool yield_blockers = false;            ///< --yield-blockers: learner/A yields to blockers.
    bool b_yield_blockers = false;          ///< --b-yield-blockers: same for --compare B.
    bool a_legacy_blockers = false;         ///< --a-legacy-blockers: disable A obstacle detouring.
    bool has_b_detour_bias = false;
    float b_detour_bias = -0.05F;
    int eval_level = -1;               ///< --eval-level: override Modern difficulty tier in eval.
    bool a_no_personalities = false;   ///< --a-no-personalities: disable 27-card personas on A.
    bool b_no_personalities = false;   ///< --b-no-personalities: disable 27-card personas on B.
    bool b_warlord_lookahead = false;  ///< --b-warlord-lookahead: enable Warlord lookahead on B.
    float army_weight = 0.25F;         ///< --army-weight: army-power share weight in empire score.
    bool upgrade_v7 = false;           ///< --upgrade-v7: upgrade a loaded (< v7) checkpoint to v7.
    bool upgrade_v8 = false;           ///< --upgrade-v8: upgrade a loaded checkpoint to v8.
    bool use_commander = false;        ///< --use-commander: train Commander posture projection.
    bool use_muster = false;           ///< --use-muster: train MusterNet hub selection.
    bool use_standard = false;         ///< --use-standard: train MusterNet Standard planting.
    bool avoid_blockers = false;       ///< --avoid-blockers: train detour choices + detourBias.
    bool causal_offturn = false;       ///< --causal-offturn: off-turn causal per-stack G_pool.
    float offturn_repel_weight = 0.5F;   ///< --offturn-repel-weight: siege repel city credit.
    float offturn_vacate_weight = 0.5F;  ///< --offturn-vacate-weight: vacated city loss penalty.
    float explore_eps = 0.0F;            ///< --explore-eps: sparse exploration decision fraction.
    float explore_temp = 0.65F;          ///< --explore-temp: temperature on exploration draws.
    bool temp_rescale = false;           ///< --temp-rescale: temperature-invariant grad scaling.
    float grad_ref_temp = 0.5F;          ///< Reference temperature for --temp-rescale.
    int threads = 0;                     ///< --threads: worker thread count (0 = hardware).
    bool econ_lineage = false;           ///< --econ-lineage: unit-lineage credit into EconNet.
    float econ_lineage_weight = 1.0F;    ///< --econ-lineage-weight: scale on lineage return.
    bool econ_family_norm = false;       ///< --econ-family-norm: per-family EconNet normalizers.
    float concentration_weight = 0.0F;   ///< --concentration-weight: stack synergy score weight.
    int dev_seeds = 0;                 ///< --dev-seeds: benchmark/compare on the development set
    bool breakdown = false;            ///< --breakdown: print shares per preset and scenario.
    int critic_warmup = 0;             ///< --critic-warmup: iterations where only the critics learn
    float mixed_frac = 0.0F;   ///< --mixed-frac: solo-learner games played 4 vs 4 instead (all
                               ///< learner seats recorded)
    int save_every = 0;        ///< --save-every: also write <out>.iterN.json snapshots
    float raider_frac = 0.0F;  ///< --raider-frac: share of Classic training seats played by Raider
    float warden_frac = 0.0F;  ///< --warden-frac: share of Classic training seats played by Warden
    int eval_preset = -1;      ///< --eval-preset: evaluate on one preset only (0..3, -1 = all)
    float departure_weight =
            0.0F;  ///< --departure-weight: credit a city loss to the move that emptied it
    bool width_scaled_lr = false;  ///< --mup: hidden-layer readouts learn 1/width as fast
    bool clip_per_head = false;    ///< --clip-per-head: clip each head's gradient norm separately
    std::string frozen_path;   ///< --frozen-opponent: fixed Modern checkpoint some opponent seats
                               ///< play
    float frozen_frac = 0.0F;  ///< --frozen-frac: share of non-learner training seats played by it
    int search_k = 0;  ///< --search-k: candidate turns per learner turn (0 = no search training)
    int search_r = 2;  ///< --search-r: rollouts (dice copies) per candidate
    int search_h = 2;  ///< --search-h: rounds played on after a candidate turn
    opponents::Kind eval_opponent =
            opponents::Kind::kClassic;  ///< --eval-opponent: classic|raider|warden|frozen
    const ml::PolicyNetwork* frozen_policy = nullptr;  ///< Loaded --frozen-opponent checkpoint.
};

/**
 * @brief Builds the options of an all-computer game for one preset and seed.
 *
 * The seed also picks each side's starting card, so games of different seeds
 * differ even on the same scenario.
 */
game::NewGameOptions GameOptions(const Config& cfg, const TrainingPreset& preset, uint32_t seed) {
    game::NewGameOptions opts;
    opts.seed = static_cast<double>(seed);
    opts.active_sides = cfg.active_sides;
    opts.crop_margin = cfg.crop_margin;
    opts.options = {
        {"hiddenMap", preset.hidden_map},
        {"viewEnemies", 0},
        {"diplomacy", preset.diplomacy},
        {"neutralCities", preset.neutral_cities},
        {"quests", 1},
    };
    for (int i = 0; i < 8; i++) {
        opts.sides[i].computer = true;
        opts.sides[i].level = 2;
        opts.sides[i].card = static_cast<int>((seed + i * 5) % 9);
    }
    return opts;
}

/**
 * @brief Starts a game of `scenario`, or of a freshly generated random map.
 *
 * A random map is used when the scenario is "RANDOM", or, when
 * `allow_random_map` is set, for the --random-map-frac share of seeds (picked
 * by a hash of the seed, so the choice is reproducible).
 */
std::unique_ptr<Game> MakeArenaGame(const Config& cfg, const std::string& scenario,
                                    const TrainingPreset& preset, uint32_t seed,
                                    bool allow_random_map) {
    auto opts = GameOptions(cfg, preset, seed);
    float map_roll = static_cast<float>(((seed * 1597334677U) ^ 0x9e3779b9U) >> 8) /
                     static_cast<float>(1U << 24);
    if ((allow_random_map && map_roll < cfg.random_map_frac) || scenario == "RANDOM") {
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options map_options;
        map_options.data_dir = cfg.data_dir;
        map_options.rng = &map_rng;
        map_options.sliders = {3, 3, 2, 3};
        map_options.allies = preset.diplomacy != 0;
        map_options.terrain_set = 0;
        auto files = randommap::GenerateNow(map_options);
        ABSL_CHECK_OK(files);
        opts.memory_files = &*files;
        auto created = game::NewGame(cfg.data_dir, "RANDOM", opts);
        ABSL_CHECK_OK(created);
        return std::move(*created);
    }
    auto created = game::NewGame(cfg.data_dir, scenario, opts);
    ABSL_CHECK_OK(created);
    return std::move(*created);
}

// ---------------------------------------------------------------- rollouts

/** @brief The recorded turns of one learner side in one game, and how it ended. */
struct SideEpisode {
    int side_index = 0;                     ///< The side the turns were played for.
    std::vector<ml::TurnStepRecord> turns;  ///< The side's turns, in order.
    float final_score = 0.0F;               ///< S when the game ended or was cut off
    bool terminal = false;                  ///< eliminated or game over: no bootstrap
    float terminal_reward = 0.0F;           ///< win / elimination bonus
    float bootstrap_value = 0.0F;           ///< V(s) at the cut-off when not terminal
    int final_cities = 0;                   ///< Cities the side held at the end.
    bool searched = false;                  ///< turns carry search-based advantages (no GAE)
};

/** @brief What one rollout game produced: its episodes and a few totals for the log. */
struct RolloutSummary {
    std::vector<SideEpisode> episodes;  ///< One per recorded side that played a turn.
    int recorded_cities = 0;            ///< Cities the recorded sides held at the end, summed.
    int recorded_sides = 0;             ///< Number of recorded sides.
    int neutrals_left = 0;              ///< Cities still neutral at the end.
};

/** @brief Who the non-recorded sides of a training game are. */
enum class Opponent {
    kMirror,   ///< Every side plays the current learner and is recorded.
    kLeague,   ///< Half the sides play a hall-of-fame snapshot.
    kClassic,  ///< Half the sides play the Classic AI.
};

/**
 * @brief Records which of a turn's moves left an own city to be taken.
 *
 * Departure credit: for each move of `turn` that left an own city empty (and
 * still empty when the turn ended), record that city's weight in the empire
 * score if `side` no longer holds it now, i.e. it fell before this side's next
 * turn. The loss is already in the turn's reward; this only says which move
 * caused it.
 */
void MarkDepartures(const Game& game, int side, ml::TurnStepRecord& turn) {
    int standing = 0, total_income = 0;
    for (const auto& city : game.GetMap().Cities()) {
        if (city.IsRazed()) continue;
        standing++;
        total_income += std::max(1, city.Income());
    }
    for (auto& step : turn.mission_steps) {
        if (step.vacated_city == kNone || step.vacated_city >= game.GetMap().CityCount()) continue;
        const City& vacated = game.GetMap().Cities()[step.vacated_city];
        if (!vacated.IsRazed() && vacated.OwnedBy(side)) continue;
        step.vacated_loss = 2.0F / static_cast<float>(std::max(1, standing)) +
                            0.5F * static_cast<float>(std::max(1, vacated.Income())) /
                                    static_cast<float>(std::max(1, total_income));
    }
    for (auto& step : turn.econ_steps) {
        if (step.guard_city == kNone || step.guard_city >= game.GetMap().CityCount()) continue;
        const City& guarded = game.GetMap().Cities()[step.guard_city];
        if (!guarded.IsRazed() && guarded.OwnedBy(side)) continue;
        step.guard_loss = 2.0F / static_cast<float>(std::max(1, standing)) +
                          0.5F * static_cast<float>(std::max(1, guarded.Income())) /
                                  static_cast<float>(std::max(1, total_income));
    }
}

opponents::Kind PickTrainingOpponent(const Config& cfg, uint32_t seed, int side_index,
                                     bool allow_frozen, Game& game, Side& side) {
    auto hash_unit = [&](uint32_t mult1, uint32_t mult2) {
        return static_cast<float>(
                       ((seed * mult1) ^ (static_cast<uint32_t>(side_index + 1) * mult2)) >> 8) /
               static_cast<float>(1U << 24);
    };
    float raid_roll = hash_unit(747796405U, 2891336453U);
    float frozen_roll = hash_unit(2654435769U, 3266489917U);
    float warden_roll = hash_unit(1103515245U, 2654435761U);
    opponents::Kind selected =
            raid_roll < cfg.raider_frac ? opponents::Kind::kRaider : opponents::Kind::kClassic;
    if (selected == opponents::Kind::kClassic && warden_roll < cfg.warden_frac) {
        selected = opponents::Kind::kWarden;
    }
    if (allow_frozen && cfg.frozen_policy && frozen_roll < cfg.frozen_frac) {
        selected = opponents::Kind::kFrozen;
        ai::SetSideClassicAi(game, side, false);  // a Modern AI seat, just not a learning one
    }
    if (allow_frozen) {
        opponents::InstallOpponent(game, side, selected, cfg.frozen_policy);
    }
    return selected;
}

/**
 * @brief Plays one training game and returns the recorded sides' episodes.
 *
 * Seats are assigned per `mode` (or per --solo-learner / --mixed-frac): learner
 * seats sample from `learner` and are recorded, the others play the Classic
 * AI, the Raider, the Frozen checkpoint or `snapshot`. When the game is cut off,
 * each living recorded side's critic value is kept for bootstrapping.
 */
RolloutSummary RunRollout(const Config& cfg, const std::string& scenario,
                          const TrainingPreset& preset, uint32_t seed, float temperature,
                          const ml::PolicyNetwork& learner, const ml::PolicyNetwork& snapshot,
                          Opponent mode) {
    auto owned_game = MakeArenaGame(cfg, scenario, preset, seed, true);
    Game& game = *owned_game;

    std::array<bool, 8> recorded{};
    std::array<bool, 8> classic{};
    std::array<opponents::Kind, 8> classic_kind{};
    std::array<SideEpisode, 8> episodes{};
    std::array<ml::RolloutSampler, 8> samplers{};
    int active_count = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (game.GetMap().Sides()[side_index].InUse()) active_count++;
    }
    int solo_seat =
            active_count > 0 ? static_cast<int>(seed % static_cast<uint32_t>(active_count)) : 0;
    // --mixed-frac: some solo-learner games are played like the benchmark instead,
    // alternating learner and Classic seats, with every learner seat recorded.
    float mix_roll = static_cast<float>(((seed * 2246822519U) ^ 0x85ebca6bU) >> 8) /
                     static_cast<float>(1U << 24);
    const bool solo = cfg.solo_learner && !(mix_roll < cfg.mixed_frac);
    const bool mixed_vs_classic = cfg.solo_learner && !solo;
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        Side& side = game.GetMap().Sides()[side_index];
        if (!side.InUse()) continue;
        bool learner_side = solo ? (seat == solo_seat)
                            : mixed_vs_classic
                                    ? ((seat + seed) % 2 == 0)
                                    : ((mode == Opponent::kMirror) || ((seat + seed) % 2 == 0));
        seat++;
        recorded[side_index] = learner_side;
        classic[side_index] = (solo || mixed_vs_classic)
                                      ? !learner_side
                                      : ((mode == Opponent::kClassic) && !learner_side);
        ai::SetSideClassicAi(game, side, classic[side_index]);
        classic_kind[side_index] =
                PickTrainingOpponent(cfg, seed, side_index, classic[side_index], game, side);
        episodes[side_index].side_index = side_index;
        samplers[side_index].rng_state =
                (seed * 2654435761U) ^ static_cast<uint32_t>((side_index + 1) * 2246822519U);
        samplers[side_index].temperature = temperature;
        samplers[side_index].explore_eps = cfg.explore_eps;
        samplers[side_index].explore_temp = cfg.explore_temp;
    }

    Side* current = game::Begin(game);
    int safety = 0;
    while (current && !game.IsOver() && !game.IsWon() && game.Turn() <= cfg.max_turns &&
           safety < cfg.max_turns * 12) {
        safety++;
        int side_index = current->Index();
        if (current->IsComputerControlled() && current->IsAlive()) {
            if (recorded[side_index]) {
                ModernMlPlayer& mod_player = ModernMlPlayer::EnsureOn(*current);
                mod_player.SetPendingOffTurn(nullptr);
                if (!episodes[side_index].turns.empty())
                    MarkDepartures(game, side_index, episodes[side_index].turns.back());
                episodes[side_index].turns.emplace_back();
                samplers[side_index].current_turn = &episodes[side_index].turns.back();
                ai::modern::PlayTurn(game, *current, nullptr, &learner, &samplers[side_index]);
                samplers[side_index].current_turn = nullptr;
                if (cfg.causal_offturn) {
                    mod_player.SetPendingOffTurn(&episodes[side_index].turns.back(),
                                                 cfg.offturn_repel_weight);
                }
            } else if (classic[side_index]) {
                opponents::PlayTurn(game, *current, classic_kind[side_index], cfg.frozen_policy);
            } else {
                ml::RolloutSampler opponent_sampler;
                opponent_sampler.rng_state = samplers[side_index].rng_state ^
                                             static_cast<uint32_t>(game.Turn() * 374761393U);
                opponent_sampler.temperature = temperature;
                opponent_sampler.explore_eps = cfg.explore_eps;
                opponent_sampler.explore_temp = cfg.explore_temp;
                ai::modern::PlayTurn(game, *current, nullptr, &snapshot, &opponent_sampler);
            }
        }
        current = game::EndTurn(game);
    }

    RolloutSummary summary;
    std::array<int, 8> cities{};
    for (const auto& city : game.GetMap().Cities()) {
        if (city.IsRazed()) continue;
        int owner_index = city.OwnerIndex();
        if (owner_index == kNone)
            summary.neutrals_left++;
        else if (owner_index >= 0 && owner_index < 8)
            cities[owner_index]++;
    }
    for (int side_index = 0; side_index < 8; side_index++) {
        if (!recorded[side_index] || episodes[side_index].turns.empty()) continue;
        Side& side = game.GetMap().Sides()[side_index];
        if (ModernMlPlayer* mod_player = ModernMlPlayer::FindMutableOn(side)) {
            mod_player->SetPendingOffTurn(nullptr);
        }
        SideEpisode& episode = episodes[side_index];
        MarkDepartures(game, side_index, episode.turns.back());
        episode.final_cities = cities[side_index];
        episode.final_score = ml::ComputeEmpireScore(game, side_index);
        if (!side.IsAlive()) {
            episode.terminal = true;
            episode.terminal_reward = cfg.elimination_reward;
        } else if (game.IsOver() || game.IsWon()) {
            episode.terminal = true;
            bool won_game =
                    (game.EndingState().winner && game.EndingState().winner->Index() == side_index);
            episode.terminal_reward = won_game ? cfg.win_reward : 0.0F;
        } else {
            ai::belief::Update(game, side);
            episode.bootstrap_value = learner.EvaluateValue(ml::BuildContext(game, side, &learner),
                                                            ml::BuildCriticFeatures(game, side));
        }
        summary.recorded_cities += cities[side_index];
        summary.recorded_sides++;
        summary.episodes.push_back(std::move(episode));
    }
    return summary;
}

// ---------------------------------------------------------------- search training
//
// Search as a teacher (--search-k K): every learner turn is played K times from
// the same position, each time sampled from the policy at the training
// temperature and recorded. Each candidate turn is then copied R times
// (different dice) and played on for H rounds by every side's normal AI; its
// value is the learner's average empire score at the end. A candidate's
// advantage is its value minus the mean of the K candidates, so every turn is
// compared with alternatives from the *same* position (no critic, no GAE). The
// game continues from the best candidate. Copies are save::encode/decode, so
// these games use shipped scenarios only. The copies see the true game state:
// that is privileged information, used only to train; the policy still plays
// on fair information.

/** @brief Returns an independent copy of `game`, made by saving and loading it. */
std::unique_ptr<Game> CopyGame(const Config& cfg, const Game& game) {
    auto copy = save::Decode(save::Encode(game), cfg.data_dir);
    ABSL_CHECK_OK(copy);
    return std::move(*copy);
}

/**
 * @brief Plays one search-training game (--search-k) for a single learner seat.
 *
 * See the section comment above: each learner turn is tried --search-k times,
 * each candidate is valued by --search-r rollouts of --search-h rounds, and the
 * game goes on from the best candidate. All candidates are recorded, with
 * their value minus the candidates' mean as the advantage.
 */
RolloutSummary RunSearchRollout(const Config& cfg, const std::string& scenario,
                                const TrainingPreset& preset, uint32_t seed, float temperature,
                                const ml::PolicyNetwork& learner) {
    auto current_game = MakeArenaGame(cfg, scenario, preset, seed, false);
    int active_count = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (current_game->GetMap().Sides()[side_index].InUse()) active_count++;
    }
    const int solo_seat =
            active_count > 0 ? static_cast<int>(seed % static_cast<uint32_t>(active_count)) : 0;
    int learner_side = kNone;
    std::array<opponents::Kind, 8> kind{};
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        Side& side = current_game->GetMap().Sides()[side_index];
        if (!side.InUse()) continue;
        const bool is_learner = (seat++ == solo_seat);
        if (is_learner) learner_side = side_index;
        ai::SetSideClassicAi(*current_game, side, !is_learner);
        kind[side_index] =
                PickTrainingOpponent(cfg, seed, side_index, !is_learner, *current_game, side);
    }

    // Every side's normal play; the learner plays greedily outside its searched turn
    auto play_side = [&](Game& game, Side& side) {
        if (side.Index() == learner_side)
            ai::modern::PlayTurn(game, side, nullptr, &learner, nullptr);
        else
            opponents::PlayTurn(game, side, kind[side.Index()], cfg.frozen_policy);
    };
    // Plays a copy on for --search-h rounds and returns the learner's empire score
    auto rollout_value = [&](Game& game) {
        const int stop_turn = game.Turn() + cfg.search_h;
        Side* current = game::EndTurn(game);
        int safety = 0;
        while (current && !game.IsOver() && !game.IsWon() && game.Turn() < stop_turn &&
               safety++ < cfg.search_h * 12) {
            if (current->IsComputerControlled() && current->IsAlive()) play_side(game, *current);
            current = game::EndTurn(game);
        }
        return ml::ComputeEmpireScore(game, learner_side);
    };

    RolloutSummary summary;
    SideEpisode search_episode;
    search_episode.side_index = learner_side;
    search_episode.searched = true;
    Side* current = game::Begin(*current_game);
    int safety = 0;
    while (current && !current_game->IsOver() && !current_game->IsWon() &&
           current_game->Turn() <= cfg.max_turns && safety++ < cfg.max_turns * 12) {
        const int side_index = current->Index();
        if (current->IsComputerControlled() && current->IsAlive() && side_index == learner_side) {
            std::vector<std::unique_ptr<Game>> candidate_games;
            std::vector<ml::TurnStepRecord> candidate_turns(cfg.search_k);
            std::vector<double> values(cfg.search_k, 0.0);
            for (int candidate = 0; candidate < cfg.search_k; candidate++) {
                auto candidate_game = CopyGame(cfg, *current_game);
                ml::RolloutSampler sampler;
                sampler.temperature = temperature;
                sampler.rng_state = (seed * 2654435761U) ^
                                    static_cast<uint32_t>((current_game->Turn() + 1) * 40503U) ^
                                    static_cast<uint32_t>((candidate + 1) * 2246822519U);
                sampler.current_turn = &candidate_turns[candidate];
                ai::modern::PlayTurn(*candidate_game, *candidate_game->GetMap().SideAt(side_index),
                                     nullptr, &learner, &sampler);
                sampler.current_turn = nullptr;
                for (int rollout = 0; rollout < cfg.search_r; rollout++) {
                    auto copy = CopyGame(cfg, *candidate_game);
                    copy->GetRng().SetState(copy->GetRng().State() ^
                                            static_cast<uint32_t>((rollout + 1) * 0x9e3779b9U));
                    values[candidate] += rollout_value(*copy);
                }
                values[candidate] /= cfg.search_r;
                candidate_games.push_back(std::move(candidate_game));
            }
            double mean = 0.0;
            for (double value : values) mean += value;
            mean /= std::max(1, cfg.search_k);
            size_t best = 0;
            for (int candidate = 0; candidate < cfg.search_k; candidate++) {
                if (values[candidate] > values[best]) best = static_cast<size_t>(candidate);
                auto& turn = candidate_turns[candidate];
                turn.advantage = static_cast<float>(values[candidate] - mean);
                turn.return_target = std::numeric_limits<float>::quiet_NaN();  // no critic target
                for (auto& step : turn.mission_steps) {
                    step.advantage = turn.advantage;
                    step.stack_return_target = std::numeric_limits<float>::quiet_NaN();
                }
                search_episode.turns.push_back(std::move(turn));
            }
            current_game = std::move(candidate_games[best]);
            current = current_game->GetMap().SideAt(side_index);
        } else if (current->IsComputerControlled() && current->IsAlive()) {
            play_side(*current_game, *current);
        }
        current = game::EndTurn(*current_game);
    }
    for (const auto& city : current_game->GetMap().Cities()) {
        if (city.IsRazed()) continue;
        if (city.IsNeutral())
            summary.neutrals_left++;
        else if (city.OwnedBy(learner_side))
            summary.recorded_cities++;
    }
    summary.recorded_sides = 1;
    if (!search_episode.turns.empty()) summary.episodes.push_back(std::move(search_episode));
    return summary;
}

/**
 * @brief Fills in each turn's and each move's advantage and critic targets.
 *
 * GAE over each side's own turns, plus optional per-stack multi-turn local
 * credit tracked over non-recycled Game::pool Army* identities. Episodes from
 * search training already carry their advantages and are skipped.
 */
void ComputeAdvantages(std::vector<SideEpisode>& episodes, const Config& cfg) {
    const float gamma = cfg.gamma;
    const float lambda = cfg.gae_lambda;
    const float local_weight = cfg.local_weight;
    const float departure_weight = cfg.departure_weight;
    constexpr float kStackGamma = 0.90F;
    // Departure credit is centred over all moves that emptied a city: emptying
    // one that then fell scores below average, emptying one that held scores
    // above, so the signal teaches *when* to leave, not "never leave".
    double vac_sum = 0.0, guard_sum = 0.0;
    int vac_count = 0, guard_count = 0;
    for (const auto& episode : episodes) {
        for (const auto& turn : episode.turns) {
            for (const auto& step : turn.mission_steps) {
                if (step.vacated_city == kNone) continue;
                vac_sum += step.vacated_loss;
                vac_count++;
            }
            for (const auto& econ_step : turn.econ_steps) {
                if (econ_step.guard_city == kNone) continue;
                guard_sum += econ_step.guard_loss;
                guard_count++;
            }
        }
    }
    const float vac_mean = vac_count > 0 ? static_cast<float>(vac_sum / vac_count) : 0.0F;
    const float guard_mean = guard_count > 0 ? static_cast<float>(guard_sum / guard_count) : 0.0F;
    for (auto& episode : episodes) {
        if (episode.searched) continue;  // advantages came from comparing candidate turns
        const int turn_count = static_cast<int>(episode.turns.size());
        float gae = 0.0F;
        std::unordered_map<std::uintptr_t, float> unit_future_local;
        ml::UnitReturnMap unit_birth_returns;
        for (int turn_index = turn_count - 1; turn_index >= 0; turn_index--) {
            auto& turn = episode.turns[turn_index];
            bool last = (turn_index == turn_count - 1);
            float next_score =
                    last ? episode.final_score : episode.turns[turn_index + 1].start_score;
            float next_value = last ? (episode.terminal ? 0.0F : episode.bootstrap_value)
                                    : episode.turns[turn_index + 1].value_pred;
            float reward =
                    (next_score - turn.start_score) + (last ? episode.terminal_reward : 0.0F);
            float delta = reward + gamma * next_value - turn.value_pred;
            gae = delta + gamma * lambda * gae;
            turn.advantage = gae;
            turn.return_target = gae + turn.value_pred;

            for (auto& [unit_id, future_local] : unit_future_local) future_local *= kStackGamma;
            if (cfg.causal_offturn) {
                for (const auto& off_ev : turn.off_turn_unit_deltas) {
                    unit_future_local[off_ev.unit_id] += off_ev.delta_score;
                }
            }
            for (int step_index = static_cast<int>(turn.mission_steps.size()) - 1; step_index >= 0;
                 step_index--) {
                auto& step = turn.mission_steps[step_index];
                if (!step.unit_ids.empty()) {
                    float per_unit =
                            step.immediate_delta_score / static_cast<float>(step.unit_ids.size());
                    for (std::uintptr_t unit_id : step.unit_ids) {
                        unit_future_local[unit_id] += per_unit;
                    }
                }
                const auto& eval_ids =
                        step.pool_unit_ids.empty() ? step.unit_ids : step.pool_unit_ids;
                float group_return = 0.0F;
                for (std::uintptr_t unit_id : eval_ids) {
                    auto found = unit_future_local.find(unit_id);
                    if (found != unit_future_local.end()) group_return += found->second;
                }
                if (cfg.causal_offturn && step.vacated_city != kNone && step.vacated_loss > 0.0F) {
                    group_return -= cfg.offturn_vacate_weight * step.vacated_loss;
                }
                step.stack_return_target = group_return;
                float local_adv = step.stack_return_target - step.stack_value_pred;
                step.advantage = turn.advantage + local_weight * local_adv;
                if (step.vacated_city != kNone)
                    step.advantage -= departure_weight * (step.vacated_loss - vac_mean);
            }
            for (auto& econ_step : turn.econ_steps) {
                econ_step.advantage = turn.advantage;
                if (econ_step.guard_city != kNone) {
                    econ_step.advantage -= departure_weight * (econ_step.guard_loss - guard_mean);
                }
            }
            for (auto& muster_step : turn.muster_steps) {
                muster_step.advantage = turn.advantage;
            }
            if (cfg.econ_lineage) {
                for (const auto& prod_ev : turn.produced_units) {
                    auto found = unit_future_local.find(prod_ev.unit_id);
                    if (found != unit_future_local.end()) {
                        unit_birth_returns[prod_ev.unit_id] = found->second;
                    }
                }
            }
        }
        if (cfg.econ_lineage && !unit_birth_returns.empty()) {
            ml::PropagateEconomyLineage(absl::MakeSpan(episode.turns), unit_birth_returns,
                                        kStackGamma);
            for (auto& turn : episode.turns) {
                for (auto& econ_step : turn.econ_steps) {
                    econ_step.advantage += cfg.econ_lineage_weight * econ_step.lineage_return;
                }
            }
        }
    }
}

/**
 * @brief Standardises advantages to zero mean and unit spread, clamped to +/-5.
 */
struct Normalizer {
    float mean = 0.0F;     ///< Mean of the fitted values.
    float inv_std = 1.0F;  ///< One over their standard deviation.
    /** @brief Returns `value` standardised and clamped to [-5, 5]. */
    float operator()(float value) const {
        return std::clamp((value - mean) * inv_std, -5.0F, 5.0F);
    }
    /** @brief Fits a normalizer to `values`; fewer than two values give the identity. */
    static Normalizer Fit(const std::vector<float>& values) {
        Normalizer normalizer;
        if (values.size() < 2) return normalizer;
        double sum = 0.0, squared_deviations = 0.0;
        for (float value : values) sum += value;
        normalizer.mean = static_cast<float>(sum / values.size());
        for (float value : values)
            squared_deviations += (static_cast<double>(value) - normalizer.mean) *
                                  (static_cast<double>(value) - normalizer.mean);
        normalizer.inv_std =
                1.0F /
                std::max(1e-4F, static_cast<float>(std::sqrt(squared_deviations / values.size())));
        return normalizer;
    }
};

// ---------------------------------------------------------------- PPO maths

/** @brief Decision counts and loss sums of one or more PPO gradient passes. */
struct HeadStats {
    int mission = 0;           ///< Mission-head decisions (move choices, stage, and detour).
    int econ = 0;              ///< Economy-head decisions.
    int diplo = 0;             ///< Diplomacy-head decisions (stance, tribute, joint war).
    int muster = 0;            ///< Muster-head decisions (castle hubs and Standard).
    int detour = 0;            ///< Detour-bias binary decisions.
    int turns = 0;             ///< Turns with a critic target.
    int stack_values = 0;      ///< Moves with a stack-critic target.
    double policy_loss = 0.0;  ///< Sum of the clipped surrogate losses.
    double value_loss = 0.0;   ///< Sum of the critics' squared-error losses.
    double entropy = 0.0;      ///< Sum of the normalised entropies of categorical choices.
    double kl = 0.0;           ///< Sum of the KL estimates against the playing policy.
    int kl_samples = 0;        ///< Number of terms in `kl`.
    int ent_samples = 0;       ///< Number of terms in `entropy`.
    /** @brief Adds `other`'s counts and sums to these. */
    void Add(const HeadStats& other) {
        mission += other.mission;
        econ += other.econ;
        diplo += other.diplo;
        muster += other.muster;
        detour += other.detour;
        turns += other.turns;
        stack_values += other.stack_values;
        policy_loss += other.policy_loss;
        value_loss += other.value_loss;
        entropy += other.entropy;
        kl += other.kl;
        kl_samples += other.kl_samples;
        ent_samples += other.ent_samples;
    }
};

/**
 * @brief PPO gradient of one categorical choice.
 *
 * Clipped-surrogate gradient for a categorical choice over options with policy
 * logits z_i = u_i / T - logMult_i (u_i = `scores`), plus a KL pull toward the
 * multiplicity prior q_i ~ exp(-logMult_i) (entropy regularisation over action
 * families). Writes dLoss/du_i into `d_scores`.
 */
void CategoricalPpo(const std::vector<float>& scores, const std::vector<float>* log_mult,
                    int chosen, float old_log_prob, float adv, float step_inv_t, float grad_inv_t,
                    const Config& cfg, std::vector<float>& d_scores, HeadStats& stats,
                    std::vector<float>& logits, std::vector<float>& probs,
                    std::vector<float>& log_probs) {
    const size_t candidate_count = scores.size();
    d_scores.assign(candidate_count, 0.0F);
    if (candidate_count <= 1) return;
    auto lm_of = [&](size_t index) {
        return (log_mult && index < log_mult->size()) ? (*log_mult)[index] : 0.0F;
    };
    float max_logit = -1e30F;
    logits.resize(candidate_count);
    for (size_t i = 0; i < candidate_count; i++) {
        logits[i] = scores[i] * step_inv_t - lm_of(i);
        max_logit = std::max(max_logit, logits[i]);
    }
    probs.resize(candidate_count);
    float sum = 0.0F;
    for (size_t i = 0; i < candidate_count; i++) {
        probs[i] = std::exp(std::max(-30.0F, logits[i] - max_logit));
        sum += probs[i];
    }
    for (float& prob : probs) prob /= sum;

    log_probs.resize(candidate_count);
    float mean_adj = 0.0F, entropy = 0.0F;
    for (size_t j = 0; j < candidate_count; j++) {
        float log_prob = std::log(std::max(1e-12F, probs[j]));
        log_probs[j] = log_prob;
        mean_adj += probs[j] * (log_prob + lm_of(j));
        entropy -= probs[j] * log_prob;
    }

    float new_log_prob = log_probs[chosen];
    float log_ratio = std::clamp(new_log_prob - old_log_prob, -10.0F, 10.0F);
    float ratio = std::exp(log_ratio);
    stats.policy_loss += -std::min(
            ratio * adv, std::clamp(ratio, 1.0F - cfg.clip_eps, 1.0F + cfg.clip_eps) * adv);
    stats.kl += (ratio - 1.0F) - log_ratio;
    stats.kl_samples++;
    bool clipped = (adv > 0.0F && ratio > 1.0F + cfg.clip_eps) ||
                   (adv < 0.0F && ratio < 1.0F - cfg.clip_eps);
    float d_loss_d_log_p = clipped ? 0.0F : -ratio * adv;

    float norm_log_k = std::max(1.0F, std::log(static_cast<float>(candidate_count)));
    stats.entropy += entropy / norm_log_k;
    stats.ent_samples++;
    float kl_coef = cfg.ent_coef / norm_log_k;
    for (size_t j = 0; j < candidate_count; j++) {
        float indicator = (static_cast<int>(j) == chosen) ? 1.0F : 0.0F;
        d_scores[j] = d_loss_d_log_p * (indicator - probs[j]) * grad_inv_t +
                      kl_coef * probs[j] * ((log_probs[j] + lm_of(j)) - mean_adj) * grad_inv_t;
    }
}

/**
 * @brief PPO gradient of one yes/no choice.
 *
 * Clipped-surrogate gradient for a yes/no choice with logit `logit`
 * (P(yes) = sigmoid(logit / T)), plus an entropy bonus. Returns dLoss/dlogit.
 */
float BinaryPpo(float logit, bool chosen, float old_log_prob, float adv, float step_inv_t,
                float grad_inv_t, const Config& cfg, HeadStats& stats) {
    float scaled_logit = std::clamp(logit * step_inv_t, -15.0F, 15.0F);
    float prob_yes = 1.0F / (1.0F + std::exp(-scaled_logit));
    float new_log_prob = std::log(std::max(1e-12F, chosen ? prob_yes : 1.0F - prob_yes));
    float log_ratio = std::clamp(new_log_prob - old_log_prob, -10.0F, 10.0F);
    float ratio = std::exp(log_ratio);
    stats.policy_loss += -std::min(
            ratio * adv, std::clamp(ratio, 1.0F - cfg.clip_eps, 1.0F + cfg.clip_eps) * adv);
    stats.kl += (ratio - 1.0F) - log_ratio;
    stats.kl_samples++;
    bool clipped = (adv > 0.0F && ratio > 1.0F + cfg.clip_eps) ||
                   (adv < 0.0F && ratio < 1.0F - cfg.clip_eps);
    float d_loss_d_log_p = clipped ? 0.0F : -ratio * adv;
    float chosen_value = chosen ? 1.0F : 0.0F;
    // d(-H)/dz = p (1 - p) z for the Bernoulli entropy H, with z the scaled
    // logit and p = P(yes)
    return d_loss_d_log_p * (chosen_value - prob_yes) * grad_inv_t +
           cfg.ent_coef * prob_yes * (1.0F - prob_yes) * scaled_logit * grad_inv_t;
}

constexpr size_t kNumEconFamilies = static_cast<size_t>(ml::EconFamily::kCount);

/** @brief One advantage normalizer per policy head (plus optional per-family economy slices). */
struct Normalizers {
    Normalizer mission;  ///< For the mission head's per-move advantages.
    Normalizer econ;     ///< For the economy head's pooled advantages.
    std::array<Normalizer, kNumEconFamilies> econ_family{};  ///< Per-family economy normalizers.
    std::array<bool, kNumEconFamilies> has_econ_family{};    ///< Active when family count >= 8.
    std::array<float, kNumEconFamilies> econ_family_weight{};  ///< Dampened inverse-sqrt weights.
    Normalizer diplo;    ///< For the diplomacy head's per-turn advantages.
    Normalizer muster;   ///< For the muster head's per-turn advantages.

    /** @brief Returns normalized advantage for `step`, using per-family stats when `by_family`. */
    float NormalizeEcon(const ml::EconStepRecord& step, bool by_family) const {
        const size_t fam = static_cast<size_t>(step.family);
        if (by_family && fam < kNumEconFamilies && has_econ_family[fam]) {
            return econ_family[fam](step.advantage) * econ_family_weight[fam];
        }
        return econ(step.advantage);
    }
};

/**
 * @brief Accumulates the gradients of one turn's decisions and its critic target.
 *
 * Adds to `grad` and `stats`; `scores`, `d_scores`, and activation buffers are
 * scratch buffers reused between calls.
 */
void AccumulateTurn(const ml::PolicyNetwork& net, const ml::TurnStepRecord& turn,
                    const Normalizers& normalizers, float inv_t, const Config& cfg,
                    ml::PolicyNetwork& grad, HeadStats& stats, std::vector<float>& scores,
                    std::vector<float>& d_scores, std::vector<float>& logits_buf,
                    std::vector<float>& probs_buf, std::vector<float>& log_probs_buf,
                    std::vector<ml::PolicyNetwork::MissionNet::Activations>& mission_acts,
                    std::vector<ml::PolicyNetwork::EconNet::Activations>& econ_acts,
                    std::vector<ml::PolicyNetwork::MusterNet::Activations>& muster_acts,
                    std::vector<ml::MissionInVec>& mission_in_buf,
                    std::vector<ml::EconInVec>& econ_in_buf,
                    std::vector<ml::MusterInVec>& muster_in_buf) {
    const bool use_cmd = net.UseCommander();
    auto step_rates = [&](float sample_temp, float& out_step_inv_t, float& out_grad_inv_t) {
        out_step_inv_t = (sample_temp > 0.0F) ? (1.0F / sample_temp) : inv_t;
        out_grad_inv_t = cfg.temp_rescale ? (1.0F / cfg.grad_ref_temp) : out_step_inv_t;
    };

    // Critic: 0.5 (V - G)^2 on a linear head (search-trained turns have no return target)
    const bool has_critic_target = !std::isnan(turn.return_target);
    if (has_critic_target) {
        ml::ValueInVec value_in = ml::PackValueInput(turn.start_ctx, turn.start_critic);
        if (use_cmd) ml::RefreshCommanderPosture(value_in.data(), net);
        ml::PolicyNetwork::ValueNet::Activations val_act;
        float predicted_value = net.ValueHead().ForwardWithActivations(value_in, val_act)[0];
        float value_error = predicted_value - turn.return_target;
        stats.value_loss += 0.5 * value_error * value_error;
        stats.turns++;
        net.ValueHead().BackwardWithActivations(value_in, val_act, {value_error}, grad.ValueHead());
    }

    for (const auto& step : turn.mission_steps) {
        if (has_critic_target) {
            ml::StackValueInVec stack_in = step.stack_value_in;
            if (use_cmd) ml::RefreshCommanderPosture(stack_in.data(), net);
            ml::PolicyNetwork::StackValueNet::Activations stack_val_act;
            float predicted_stack_value =
                    net.StackValueHead().ForwardWithActivations(stack_in, stack_val_act)[0];
            float stack_value_error = predicted_stack_value - step.stack_return_target;
            stats.value_loss += 0.25 * stack_value_error * stack_value_error;
            stats.stack_values++;
            net.StackValueHead().BackwardWithActivations(stack_in, stack_val_act,
                                                        {stack_value_error}, grad.StackValueHead());
        }

        float adv = normalizers.mission(step.advantage);
        const size_t candidate_count = step.candidates.size();
        if (candidate_count > 1) {
            scores.resize(candidate_count);
            mission_acts.resize(candidate_count);
            const ml::MissionInVec* cands_ptr = step.candidates.data();
            if (use_cmd) {
                mission_in_buf = step.candidates;
                for (auto& cand_in : mission_in_buf) {
                    ml::RefreshCommanderPosture(cand_in.data(), net);
                }
                cands_ptr = mission_in_buf.data();
            }
            for (size_t candidate = 0; candidate < candidate_count; candidate++) {
                scores[candidate] = net.MissionHead().ForwardWithActivations(
                        cands_ptr[candidate], mission_acts[candidate])[0];
            }
            float step_inv_t = inv_t, grad_inv_t = inv_t;
            step_rates(step.sample_temp, step_inv_t, grad_inv_t);
            CategoricalPpo(scores, &step.log_mult, step.chosen_idx, step.old_log_prob, adv,
                           step_inv_t, grad_inv_t, cfg, d_scores, stats, logits_buf, probs_buf,
                           log_probs_buf);
            std::array<float, 5> d_posture{};
            for (size_t candidate = 0; candidate < candidate_count; candidate++) {
                if (d_scores[candidate] != 0.0F) {
                    net.MissionHead().BackwardWithActivations(
                            cands_ptr[candidate], mission_acts[candidate],
                            {d_scores[candidate], 0.0F}, grad.MissionHead(),
                            use_cmd ? &d_posture : nullptr);
                }
            }
            if (use_cmd) ml::AccumulateCommanderGrad(cands_ptr[0].data(), d_posture, grad);
            stats.mission++;
        }
        if (step.has_stage_choice && step.chosen_idx < static_cast<int>(candidate_count)) {
            ml::MissionInVec chosen_in = step.candidates[step.chosen_idx];
            if (use_cmd) ml::RefreshCommanderPosture(chosen_in.data(), net);
            ml::PolicyNetwork::MissionNet::Activations stage_act;
            float stage_logit = net.MissionHead().ForwardWithActivations(chosen_in, stage_act)[1];
            float step_inv_t = inv_t, grad_inv_t = inv_t;
            step_rates(step.stage_sample_temp, step_inv_t, grad_inv_t);
            float d_stage_logit = BinaryPpo(stage_logit, step.stage_chosen, step.old_stage_log_prob,
                                            adv, step_inv_t, grad_inv_t, cfg, stats);
            std::array<float, 5> d_posture{};
            net.MissionHead().BackwardWithActivations(chosen_in, stage_act, {0.0F, d_stage_logit},
                                                      grad.MissionHead(),
                                                      use_cmd ? &d_posture : nullptr);
            if (use_cmd) ml::AccumulateCommanderGrad(chosen_in.data(), d_posture, grad);
            stats.mission++;
        }
        for (const auto& detour : step.detour_steps) {
            ml::MissionInVec det_in = detour.detour_in;
            ml::MissionInVec thr_in = detour.through_in;
            if (use_cmd) {
                ml::RefreshCommanderPosture(det_in.data(), net);
                ml::RefreshCommanderPosture(thr_in.data(), net);
            }
            ml::PolicyNetwork::MissionNet::Activations det_act, thr_act;
            float u_det = net.MissionHead().ForwardWithActivations(det_in, det_act)[0];
            float u_thr = net.MissionHead().ForwardWithActivations(thr_in, thr_act)[0];
            float det_logit = (u_det + net.DetourBias()) - u_thr;
            float step_inv_t = inv_t, grad_inv_t = inv_t;
            step_rates(detour.sample_temp, step_inv_t, grad_inv_t);
            float d_logit = BinaryPpo(det_logit, detour.detour_chosen, detour.old_log_prob, adv,
                                      step_inv_t, grad_inv_t, cfg, stats);
            grad.SetDetourBias(grad.DetourBias() + d_logit);
            std::array<float, 5> d_posture{};
            net.MissionHead().BackwardWithActivations(det_in, det_act, {d_logit, 0.0F},
                                                      grad.MissionHead(),
                                                      use_cmd ? &d_posture : nullptr);
            net.MissionHead().BackwardWithActivations(thr_in, thr_act, {-d_logit, 0.0F},
                                                      grad.MissionHead(),
                                                      use_cmd ? &d_posture : nullptr);
            if (use_cmd) ml::AccumulateCommanderGrad(det_in.data(), d_posture, grad);
            stats.mission++;
            stats.detour++;
        }
    }

    for (const auto& econ_step : turn.econ_steps) {
        const size_t candidate_count = econ_step.candidates.size();
        if (candidate_count <= 1) continue;
        float econ_adv = normalizers.NormalizeEcon(econ_step, cfg.econ_family_norm);
        scores.resize(candidate_count);
        econ_acts.resize(candidate_count);
        const ml::EconInVec* cands_ptr = econ_step.candidates.data();
        if (use_cmd) {
            econ_in_buf = econ_step.candidates;
            for (auto& cand_in : econ_in_buf) {
                ml::RefreshCommanderPosture(cand_in.data(), net);
            }
            cands_ptr = econ_in_buf.data();
        }
        for (size_t candidate = 0; candidate < candidate_count; candidate++) {
            scores[candidate] = net.EconHead().ForwardWithActivations(
                    cands_ptr[candidate], econ_acts[candidate])[0];
        }
        float step_inv_t = inv_t, grad_inv_t = inv_t;
        step_rates(econ_step.sample_temp, step_inv_t, grad_inv_t);
        CategoricalPpo(scores, &econ_step.log_mult, econ_step.chosen_idx, econ_step.old_log_prob,
                       econ_adv, step_inv_t, grad_inv_t, cfg, d_scores, stats, logits_buf,
                       probs_buf, log_probs_buf);
        std::array<float, 5> d_posture{};
        for (size_t candidate = 0; candidate < candidate_count; candidate++) {
            if (d_scores[candidate] != 0.0F) {
                net.EconHead().BackwardWithActivations(
                        cands_ptr[candidate], econ_acts[candidate], {d_scores[candidate]},
                        grad.EconHead(), use_cmd ? &d_posture : nullptr);
            }
        }
        if (use_cmd) ml::AccumulateCommanderGrad(cands_ptr[0].data(), d_posture, grad);
        stats.econ++;
    }

    for (const auto& muster_step : turn.muster_steps) {
        const size_t candidate_count = muster_step.candidates.size();
        if (candidate_count <= 1) continue;
        float muster_adv = normalizers.muster(muster_step.advantage);
        scores.resize(candidate_count);
        muster_acts.resize(candidate_count);
        const ml::MusterInVec* cands_ptr = muster_step.candidates.data();
        if (use_cmd) {
            muster_in_buf = muster_step.candidates;
            for (auto& cand_in : muster_in_buf) {
                ml::RefreshCommanderPosture(cand_in.data(), net);
            }
            cands_ptr = muster_in_buf.data();
        }
        for (size_t candidate = 0; candidate < candidate_count; candidate++) {
            scores[candidate] = net.MusterHead().ForwardWithActivations(
                    cands_ptr[candidate], muster_acts[candidate])[0];
        }
        float step_inv_t = inv_t, grad_inv_t = inv_t;
        step_rates(muster_step.sample_temp, step_inv_t, grad_inv_t);
        CategoricalPpo(scores, &muster_step.log_mult, muster_step.chosen_idx,
                       muster_step.old_log_prob, muster_adv, step_inv_t, grad_inv_t, cfg, d_scores,
                       stats, logits_buf, probs_buf, log_probs_buf);
        std::array<float, 5> d_posture{};
        for (size_t candidate = 0; candidate < candidate_count; candidate++) {
            if (d_scores[candidate] != 0.0F) {
                net.MusterHead().BackwardWithActivations(
                        cands_ptr[candidate], muster_acts[candidate], {d_scores[candidate]},
                        grad.MusterHead(), use_cmd ? &d_posture : nullptr);
            }
        }
        if (use_cmd) ml::AccumulateCommanderGrad(cands_ptr[0].data(), d_posture, grad);
        stats.muster++;
    }

    float diplo_adv = normalizers.diplo(turn.advantage);
    for (const auto& diplo_step : turn.diplo_steps) {
        ml::DiploInVec diplo_in = diplo_step.input;
        if (use_cmd) ml::RefreshCommanderPosture(diplo_in.data(), net);
        ml::PolicyNetwork::DiploNet::Activations diplo_act;
        auto raw = net.DiploHead().ForwardWithActivations(diplo_in, diplo_act);
        std::array<float, ml::kDiploOutDim> d_out{};
        scores.resize(4);
        scores[0] = raw[0];
        scores[1] = raw[1];
        scores[2] = raw[2];
        scores[3] = raw[3];
        float stance_inv_t = inv_t, stance_grad_inv_t = inv_t;
        step_rates(diplo_step.stance_sample_temp, stance_inv_t, stance_grad_inv_t);
        CategoricalPpo(scores, nullptr, diplo_step.chosen_stance, diplo_step.old_stance_log_prob,
                       diplo_adv, stance_inv_t, stance_grad_inv_t, cfg, d_scores, stats, logits_buf,
                       probs_buf, log_probs_buf);
        for (int stance = 0; stance < 4; stance++) d_out[stance] = d_scores[stance];
        stats.diplo++;
        if (diplo_step.has_tribute) {
            float trib_inv_t = inv_t, trib_grad_inv_t = inv_t;
            step_rates(diplo_step.tribute_sample_temp, trib_inv_t, trib_grad_inv_t);
            d_out[4] = BinaryPpo(raw[4], diplo_step.tribute_chosen, diplo_step.old_tribute_log_prob,
                                 diplo_adv, trib_inv_t, trib_grad_inv_t, cfg, stats);
            stats.diplo++;
        }
        if (diplo_step.has_joint_war) {
            float jw_inv_t = inv_t, jw_grad_inv_t = inv_t;
            step_rates(diplo_step.joint_war_sample_temp, jw_inv_t, jw_grad_inv_t);
            d_out[5] = BinaryPpo(raw[5], diplo_step.joint_war_chosen,
                                 diplo_step.old_joint_war_log_prob, diplo_adv, jw_inv_t,
                                 jw_grad_inv_t, cfg, stats);
            stats.diplo++;
        }
        std::array<float, 5> d_posture{};
        net.DiploHead().BackwardWithActivations(diplo_in, diplo_act, d_out, grad.DiploHead(),
                                                use_cmd ? &d_posture : nullptr);
        if (use_cmd) ml::AccumulateCommanderGrad(diplo_in.data(), d_posture, grad);
    }
}

// ---------------------------------------------------------------- evaluation

/** @brief One evaluation game: the scenario/preset/seed/seat assignment and its outcome. */
struct EvalCase {
    std::string scenario;  ///< Scenario played.
    int preset = 0;        ///< Index into kPresets.
    uint32_t seed = 0;     ///< Game seed.
    int swap = 0;          ///< 1 for the game of the seed with the seats swapped.
    double share = 0.5;    ///< Modern share of the cities held by both camps
    int top = 0;           ///< 1 when the largest side is a Modern side
    int modern = 0;        ///< Cities the Modern sides held at the end.
    int classic = 0;       ///< Cities the opponent sides held at the end.
    int modern_sides = 0;  ///< Seats played by the Modern AI.
    int sides = 0;         ///< Seats in play.
};

/** @brief The summary of a set of evaluation games. */
struct EvalResult {
    int games = 0;                ///< Number of games played.
    double mean_share = 0.0;      ///< Modern AI's share of the cities held by both camps
    double ci95 = 0.0;            ///< half-width of the 95% interval on mean_share
    int modern_top = 0;           ///< games whose largest side is a Modern AI side
    int modern_cities = 0;        ///< Cities the Modern sides held, over all games.
    int classic_cities = 0;       ///< Cities the opponent sides held, over all games.
    double parity = 0.5;          ///< share an equally strong Modern side would get
    std::vector<EvalCase> cases;  ///< The individual games.
};

/**
 * @brief Tells whether evaluation games seat one Modern side against the rest.
 *
 * Solo evaluation (1 Modern vs K-1 Classic) is used when asked for, and during
 * cropped-map curriculum selection where the training setup is solo.
 */
bool UseSolo(const Config& cfg, bool is_selection) {
    if (cfg.eval_mode == EvalMode::kSolo) return true;
    return cfg.solo_learner && is_selection && (cfg.active_sides < 8 || cfg.crop_margin > 0);
}

/**
 * @brief Lists the games of an evaluation set: each seed twice, seats swapped.
 *
 * Scenario and preset rotate with the seed so every set covers all of them.
 */
std::vector<EvalCase> EvalCases(uint32_t seed_base, int seeds, int eval_preset) {
    std::vector<EvalCase> cases;
    for (int i = 0; i < seeds; i++) {
        for (int swap = 0; swap < 2; swap++) {
            EvalCase eval_case;
            eval_case.scenario = kScenarios[(i + i / 4) % kScenarios.size()];
            eval_case.preset = eval_preset >= 0 ? eval_preset : i % 4;
            eval_case.seed = seed_base + static_cast<uint32_t>(i);
            eval_case.swap = swap;
            cases.push_back(eval_case);
        }
    }
    return cases;
}

/**
 * @brief Plays one evaluation game and records its outcome in `eval_case`.
 *
 * Modern seats play `policy` without a rollout sampler (at the deploy
 * temperature, --eval-temp); the other seats play --eval-opponent. Fills in
 * the outcome fields of `eval_case`.
 */
void PlayEvalCase(const Config& cfg, const ml::PolicyNetwork& policy, EvalCase& eval_case,
                  bool solo, bool allow_random, int turns) {
    auto owned_game = MakeArenaGame(cfg, eval_case.scenario, kPresets[eval_case.preset],
                                    eval_case.seed, allow_random);
    Game& game = *owned_game;
    int active_count = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (game.GetMap().Sides()[side_index].InUse()) active_count++;
    }
    // Solo: the Modern seat rotates with the seed; the swapped game takes the seat
    // half-way round the table so both games of a seed differ in position.
    int solo_seat =
            active_count > 0
                    ? static_cast<int>((eval_case.seed +
                                        static_cast<uint32_t>(eval_case.swap *
                                                              std::max(1, active_count / 2))) %
                                       static_cast<uint32_t>(active_count))
                    : 0;
    std::array<bool, 8> modern{};
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (!game.GetMap().Sides()[side_index].InUse()) continue;
        modern[side_index] = solo ? (seat == solo_seat) : ((seat + eval_case.swap) % 2 == 0);
        // Opponent seats are Classic AI sides unless the Frozen Modern checkpoint plays them
        if (modern[side_index]) {
            Side& side = game.GetMap().Sides()[side_index];
            if (cfg.eval_level >= 0) side.SetLevel(cfg.eval_level);
            ai::SetSideClassicAi(game, side, false);
            eval_case.modern_sides++;
        } else {
            opponents::InstallOpponent(game, game.GetMap().Sides()[side_index], cfg.eval_opponent,
                                       cfg.frozen_policy);
        }
        eval_case.sides++;
        seat++;
    }
    Side* current = game::Begin(game);
    int safety = 0;
    while (current && !game.IsOver() && !game.IsWon() && game.Turn() <= turns &&
           safety < turns * 12) {
        safety++;
        if (current->IsComputerControlled() && current->IsAlive()) {
            if (modern[current->Index()])
                ai::modern::PlayTurn(game, *current, nullptr, &policy, nullptr);
            else
                opponents::PlayTurn(game, *current, cfg.eval_opponent, cfg.frozen_policy);
        }
        current = game::EndTurn(game);
    }
    std::array<int, 8> city_counts{};
    int modern_cities = 0, classic_cities = 0, largest_side = kNone, largest_count = -1;
    for (const auto& city : game.GetMap().Cities()) {
        int owner_index = city.OwnerIndex();
        if (city.IsRazed() || owner_index < 0 || owner_index >= 8) continue;
        city_counts[owner_index]++;
        (modern[owner_index] ? modern_cities : classic_cities)++;
    }
    for (int side_index = 0; side_index < 8; side_index++) {
        if (city_counts[side_index] > largest_count) {
            largest_count = city_counts[side_index];
            largest_side = side_index;
        }
    }
    eval_case.share = (modern_cities + classic_cities) > 0
                              ? static_cast<double>(modern_cities) /
                                        static_cast<double>(modern_cities + classic_cities)
                              : 0.5;
    eval_case.top = (largest_side != kNone && modern[largest_side]) ? 1 : 0;
    eval_case.modern = modern_cities;
    eval_case.classic = classic_cities;
}

/**
 * @brief Plays each of `policies` on the same evaluation cases.
 *
 * Plays `policies.size()` checkpoints on the same cases (same maps, seats and
 * seeds), in parallel over all hardware threads.
 * @return One list of played cases per policy, in the order of `policies`.
 */
int ScenarioCostWeight(std::string_view scenario) {
    if (scenario == "HADESHA" || scenario == "ERYTHEA" || scenario == "ISLADIA" ||
        scenario == "RANDOM") {
        return 4;
    }
    if (scenario == "DRAGON") return 2;
    return 1;
}

std::vector<std::vector<EvalCase>> RunEval(const Config& cfg,
                                           const std::vector<const ml::PolicyNetwork*>& policies,
                                           uint32_t seed_base, int seeds, int turns) {
    bool is_selection = (seed_base == kSelectSeedBase);
    bool solo = UseSolo(cfg, is_selection);
    std::vector<std::vector<EvalCase>> results(policies.size(),
                                               EvalCases(seed_base, seeds, cfg.eval_preset));
    const size_t per_policy = results[0].size();
    const size_t total = per_policy * policies.size();
    std::vector<size_t> task_order(total);
    for (size_t i = 0; i < total; ++i) task_order[i] = i;
    std::stable_sort(task_order.begin(), task_order.end(), [&](size_t lhs, size_t rhs) {
        const EvalCase& c_lhs = results[lhs % policies.size()][lhs / policies.size()];
        const EvalCase& c_rhs = results[rhs % policies.size()][rhs / policies.size()];
        return ScenarioCostWeight(c_lhs.scenario) > ScenarioCostWeight(c_rhs.scenario);
    });
    std::atomic<size_t> next{0};
    unsigned int workers =
            cfg.threads > 0 ? static_cast<unsigned int>(cfg.threads)
                            : std::max(2U, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned int worker = 0; worker < workers; worker++) {
        pool.emplace_back([&]() {
            for (size_t slot; (slot = next.fetch_add(1)) < total;) {
                size_t task_idx = task_order[slot];
                size_t policy_index = task_idx % policies.size();
                size_t case_index = task_idx / policies.size();
                PlayEvalCase(cfg, *policies[policy_index], results[policy_index][case_index], solo,
                             is_selection, turns);
            }
        });
    }
    for (auto& thread : pool) thread.join();
    return results;
}

/** @brief Sums up played evaluation cases: mean share, its 95% interval and totals. */
EvalResult Summarize(std::vector<EvalCase> cases) {
    EvalResult result;
    result.games = static_cast<int>(cases.size());
    double parity = 0.0;
    for (const auto& eval_case : cases) {
        result.mean_share += eval_case.share;
        result.modern_top += eval_case.top;
        result.modern_cities += eval_case.modern;
        result.classic_cities += eval_case.classic;
        parity += eval_case.sides > 0 ? static_cast<double>(eval_case.modern_sides) /
                                                static_cast<double>(eval_case.sides)
                                      : 0.5;
    }
    result.mean_share /= std::max(1, result.games);
    result.parity = parity / std::max(1, result.games);
    double variance = 0.0;
    for (const auto& eval_case : cases)
        variance += (eval_case.share - result.mean_share) * (eval_case.share - result.mean_share);
    variance /= std::max(1, result.games - 1);
    result.ci95 = 1.96 * std::sqrt(variance / std::max(1, result.games));
    result.cases = std::move(cases);
    return result;
}

/** @brief Plays `policy` against --eval-opponent on an evaluation set and sums it up. */
EvalResult EvaluateVsClassic(const Config& cfg, const ml::PolicyNetwork& policy, uint32_t seed_base,
                             int seeds, int turns) {
    return Summarize(std::move(RunEval(cfg, {&policy}, seed_base, seeds, turns)[0]));
}

/** @brief Prints one line summing up an evaluation, tagged with `label`. */
void PrintEval(const char* label, const EvalResult& result) {
    std::printf(
            "  [%s] %d games: Modern city share %.1f%% +/- %.1f%% (95%%, parity %.1f%%) | cities "
            "%d vs %d | "
            "Modern largest side in %d/%d\n",
            label, result.games, result.mean_share * 100.0, result.ci95 * 100.0,
            result.parity * 100.0, result.modern_cities, result.classic_cities, result.modern_top,
            result.games);
    std::fflush(stdout);
}

/** @brief Prints the mean share per preset and per scenario (--breakdown). */
void PrintBreakdown(const EvalResult& result) {
    auto print_line = [&](const char* kind, const std::string& key, auto match) {
        double share_sum = 0.0;
        int games = 0, modern_cities = 0, classic_cities = 0;
        for (const auto& eval_case : result.cases) {
            if (!match(eval_case)) continue;
            share_sum += eval_case.share;
            games++;
            modern_cities += eval_case.modern;
            classic_cities += eval_case.classic;
        }
        if (games > 0)
            std::printf("    %-8s %-14s %4d games  share %5.1f%%  cities %5d vs %5d\n", kind,
                        key.c_str(), games, 100.0 * share_sum / games, modern_cities,
                        classic_cities);
    };
    for (int preset = 0; preset < 4; preset++)
        print_line("preset", kPresets[preset].label,
                   [&](const EvalCase& eval_case) { return eval_case.preset == preset; });
    for (const auto& scenario : kScenarios)
        print_line("scenario", scenario,
                   [&](const EvalCase& eval_case) { return eval_case.scenario == scenario; });
    std::fflush(stdout);
}

/**
 * @brief Prints a paired comparison of two checkpoints evaluated on the same cases.
 *
 * Paired comparison: the same cases played by A and B; the mean per-game share
 * difference has a much tighter interval than two independent means.
 */
void PrintPaired(const char* name_a, const EvalResult& result_a, const char* name_b,
                 const EvalResult& result_b) {
    const size_t game_count = std::min(result_a.cases.size(), result_b.cases.size());
    std::vector<double> differences(game_count);
    double mean = 0.0;
    int wins = 0, losses = 0;
    for (size_t i = 0; i < game_count; i++) {
        differences[i] = result_b.cases[i].share - result_a.cases[i].share;
        mean += differences[i];
        wins += differences[i] > 1e-9;
        losses += differences[i] < -1e-9;
    }
    mean /= std::max<size_t>(1, game_count);
    double variance = 0.0;
    for (double difference : differences) variance += (difference - mean) * (difference - mean);
    variance /= std::max<size_t>(1, game_count - 1);
    double half_width = 1.96 * std::sqrt(variance / std::max<size_t>(1, game_count));
    std::printf(
            "  [paired] B - A = %+.2f%% +/- %.2f%% (95%%) over %zu games | B better in %d, worse "
            "in %d | %s\n",
            mean * 100.0, half_width * 100.0, game_count, wins, losses,
            (mean - half_width > 0)   ? "B significantly better"
            : (mean + half_width < 0) ? "B significantly worse"
                                      : "no significant difference");
    std::printf("    A = %s\n    B = %s\n", name_a, name_b);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- main

/**
 * @brief Reads the command-line options into `cfg`.
 * @return false, after printing why, on an unknown option or value.
 */
bool ParseArgs(int argc, char** argv, Config& cfg) {
    auto extra_args = ParseCliFlags(
            argc, argv, "w2arena [--iterations N] [--games-per-iter G] [--turns T] [--out PATH]");
    if (!extra_args.empty()) {
        std::fprintf(stderr,
                     "Unknown argument '%s'; see the usage at the top of cpp/test/arena.cpp.\n",
                     extra_args[0].c_str());
        return false;
    }
    cfg.iterations = std::max(0, absl::GetFlag(FLAGS_iterations));
    cfg.games_per_iter = std::max(2, absl::GetFlag(FLAGS_games_per_iter));
    cfg.max_turns = std::max(6, absl::GetFlag(FLAGS_turns));
    cfg.eval_every = std::max(1, absl::GetFlag(FLAGS_eval_every));
    cfg.eval_turns = std::max(6, absl::GetFlag(FLAGS_eval_turns));
    cfg.select_seeds = std::max(1, absl::GetFlag(FLAGS_select_seeds));
    cfg.report_seeds = std::max(0, absl::GetFlag(FLAGS_report_seeds));
    cfg.epochs = std::max(1, absl::GetFlag(FLAGS_epochs));
    cfg.minibatches = std::max(1, absl::GetFlag(FLAGS_minibatches));
    cfg.lr_start = static_cast<float>(absl::GetFlag(FLAGS_lr));
    cfg.lr_end = std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_lr_end)));
    cfg.width_scaled_lr = absl::GetFlag(FLAGS_mup);
    cfg.clip_per_head = absl::GetFlag(FLAGS_clip_per_head);
    cfg.temp_start = std::max(0.02F, static_cast<float>(absl::GetFlag(FLAGS_temp_start)));
    cfg.temp_end = std::max(0.02F, static_cast<float>(absl::GetFlag(FLAGS_temp_end)));
    cfg.eval_temp = std::max(0.01F, static_cast<float>(absl::GetFlag(FLAGS_eval_temp)));
    cfg.seed_offset = std::min<uint32_t>(500000U, absl::GetFlag(FLAGS_seed_offset));
    cfg.classic_frac =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_classic_frac)), 0.0F, 1.0F);
    cfg.random_map_frac =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_random_map_frac)), 0.0F, 1.0F);
    cfg.active_sides = std::clamp(absl::GetFlag(FLAGS_active_sides), 2, 8);
    cfg.crop_margin = std::max(0, absl::GetFlag(FLAGS_crop_margin));
    cfg.gae_lambda = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_lambda)), 0.0F, 1.0F);
    cfg.local_weight = std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_local_weight)));
    cfg.in_path = absl::GetFlag(FLAGS_in);
    cfg.out_path = absl::GetFlag(FLAGS_out);
    cfg.log_path = absl::GetFlag(FLAGS_log);
    cfg.data_dir = absl::GetFlag(FLAGS_data);
    cfg.solo_learner = absl::GetFlag(FLAGS_solo_learner);
    cfg.from_scratch = absl::GetFlag(FLAGS_from_scratch);
    cfg.benchmark_only = absl::GetFlag(FLAGS_benchmark_only);
    std::string mode = absl::GetFlag(FLAGS_eval_mode);
    if (mode == "solo") {
        cfg.eval_mode = EvalMode::kSolo;
    } else if (mode == "mixed") {
        cfg.eval_mode = EvalMode::kMixed;
    } else {
        std::fprintf(stderr,
                     "Unknown --eval-mode '%s'; expected 'mixed' (4 vs 4) or 'solo' (1 vs K-1).\n",
                     mode.c_str());
        return false;
    }
    cfg.compare_path = absl::GetFlag(FLAGS_compare);
    cfg.dev_seeds = std::max(0, absl::GetFlag(FLAGS_dev_seeds));
    cfg.breakdown = absl::GetFlag(FLAGS_breakdown);
    cfg.critic_warmup = std::max(0, absl::GetFlag(FLAGS_critic_warmup));
    cfg.mixed_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_mixed_frac)), 0.0F, 1.0F);
    cfg.save_every = std::max(0, absl::GetFlag(FLAGS_save_every));
    cfg.raider_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_raider_frac)), 0.0F, 1.0F);
    cfg.warden_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_warden_frac)), 0.0F, 1.0F);
    cfg.eval_preset = std::clamp(absl::GetFlag(FLAGS_eval_preset), -1, 3);
    cfg.departure_weight =
            std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_departure_weight)));
    cfg.yield_blockers = absl::GetFlag(FLAGS_yield_blockers);
    cfg.b_yield_blockers = absl::GetFlag(FLAGS_b_yield_blockers);
    cfg.a_legacy_blockers = absl::GetFlag(FLAGS_a_legacy_blockers);
    if (std::string detour_str = absl::GetFlag(FLAGS_b_detour_bias); !detour_str.empty()) {
        cfg.has_b_detour_bias = true;
        cfg.b_detour_bias = static_cast<float>(std::strtod(detour_str.c_str(), nullptr));
    }
    cfg.eval_level = std::clamp(absl::GetFlag(FLAGS_eval_level), -1, 3);
    cfg.a_no_personalities = absl::GetFlag(FLAGS_a_no_personalities);
    cfg.b_no_personalities = absl::GetFlag(FLAGS_b_no_personalities);
    cfg.b_warlord_lookahead = absl::GetFlag(FLAGS_b_warlord_lookahead);
    cfg.army_weight = std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_army_weight)));
    cfg.upgrade_v7 = absl::GetFlag(FLAGS_upgrade_v7);
    cfg.upgrade_v8 = absl::GetFlag(FLAGS_upgrade_v8);
    cfg.use_commander = absl::GetFlag(FLAGS_use_commander);
    cfg.use_muster = absl::GetFlag(FLAGS_use_muster);
    cfg.use_standard = absl::GetFlag(FLAGS_use_standard);
    cfg.avoid_blockers = absl::GetFlag(FLAGS_avoid_blockers);
    cfg.causal_offturn = absl::GetFlag(FLAGS_causal_offturn);
    cfg.offturn_repel_weight =
            std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_offturn_repel_weight)));
    cfg.offturn_vacate_weight =
            std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_offturn_vacate_weight)));
    cfg.explore_eps =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_explore_eps)), 0.0F, 0.99F);
    cfg.explore_temp = std::max(0.05F, static_cast<float>(absl::GetFlag(FLAGS_explore_temp)));
    cfg.temp_rescale = absl::GetFlag(FLAGS_temp_rescale);
    cfg.threads = std::max(0, absl::GetFlag(FLAGS_threads));
    cfg.econ_lineage = absl::GetFlag(FLAGS_econ_lineage);
    cfg.econ_lineage_weight =
            std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_econ_lineage_weight)));
    cfg.econ_family_norm = absl::GetFlag(FLAGS_econ_family_norm);
    cfg.concentration_weight =
            std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_concentration_weight)));
    cfg.frozen_path = absl::GetFlag(FLAGS_frozen_opponent);
    cfg.frozen_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_frozen_frac)), 0.0F, 1.0F);
    cfg.search_k = std::max(0, absl::GetFlag(FLAGS_search_k));
    cfg.search_r = std::max(1, absl::GetFlag(FLAGS_search_r));
    cfg.search_h = std::max(1, absl::GetFlag(FLAGS_search_h));
    std::string opponent = absl::GetFlag(FLAGS_eval_opponent);
    if (opponent == "raider") {
        cfg.eval_opponent = opponents::Kind::kRaider;
    } else if (opponent == "frozen") {
        cfg.eval_opponent = opponents::Kind::kFrozen;
    } else if (opponent == "warden") {
        cfg.eval_opponent = opponents::Kind::kWarden;
    } else if (opponent == "classic") {
        cfg.eval_opponent = opponents::Kind::kClassic;
    } else {
        std::fprintf(stderr,
                     "Unknown --eval-opponent '%s'; expected 'classic', 'raider', 'warden' or "
                     "'frozen'.\n",
                     opponent.c_str());
        return false;
    }
    return true;
}

}  // namespace

/**
 * @brief Runs the trainer, or only a benchmark or an A/B comparison.
 *
 * Each iteration plays the rollout games in parallel, computes advantages,
 * runs PPO with Adam on the learner and, every --eval-every iterations, keeps
 * the checkpoint with the best selection-set share.
 */
int main(int argc, char** argv) {
    Config cfg;
    if (!ParseArgs(argc, argv, cfg)) return 2;
    if (!FileExists(std::filesystem::path(cfg.data_dir) / "TERRAIN0/ARMYTYPE.DAT")) {
        std::fprintf(stderr,
                     "Cannot find the game data in '%s': run from the repository root or pass "
                     "--data DIR.\n",
                     cfg.data_dir.c_str());
        return 1;
    }
    unsigned int threads =
            cfg.threads > 0 ? static_cast<unsigned int>(cfg.threads)
                            : std::max(2U, std::thread::hardware_concurrency());
    ml::SetDeployTemperature(cfg.eval_temp);
    ml::SetRecordThoughts(false);
    ml::PolicyNetwork frozen;
    if (!cfg.frozen_path.empty()) {
        if (!frozen.LoadJson(cfg.frozen_path).ok()) {
            std::fprintf(stderr,
                         "Cannot load --frozen-opponent '%s'; check the path and JSON format.\n",
                         cfg.frozen_path.c_str());
            return 1;
        }
        cfg.frozen_policy = &frozen;
    }
    if (cfg.eval_opponent == opponents::Kind::kFrozen && !cfg.frozen_policy) {
        std::fprintf(stderr, "--eval-opponent frozen needs --frozen-opponent PATH.\n");
        return 2;
    }

    ml::PolicyNetwork learner;
    bool loaded = false;
    const std::string load_path = !cfg.in_path.empty() ? cfg.in_path : cfg.out_path;
    if (!cfg.from_scratch && std::filesystem::exists(load_path)) {
        loaded = learner.LoadJson(load_path).ok();
    }
    if (!loaded && !cfg.in_path.empty() && !cfg.from_scratch) {
        std::fprintf(stderr, "Cannot load --in checkpoint '%s'; check the path and JSON format.\n",
                     cfg.in_path.c_str());
        return 1;
    }
    if (!loaded) {
        learner.InitRandomWeights(2026);
    } else if (cfg.upgrade_v8) {
        learner.UpgradeToV8();
    } else if (cfg.upgrade_v7) {
        learner.UpgradeToV7();
    }
    if (cfg.use_commander) {
        if (learner.Version() < 7) learner.UpgradeToV7();
        if (!learner.UseCommander()) {
            std::mt19937 cmd_rng(555U);
            std::normal_distribution<float> cmd_norm(0.0F, 0.22F);
            for (float& weight : learner.CommanderW()) weight = cmd_norm(cmd_rng);
            learner.CommanderB().fill(0.0F);
        }
        learner.SetUseCommander(true);
    }
    if (cfg.use_muster || cfg.use_standard) {
        learner.UpgradeToV8();
        bool muster_uninit = true;
        for (float weight : learner.MusterHead().w1) {
            if (weight != 0.0F) {
                muster_uninit = false;
                break;
            }
        }
        if (muster_uninit) {
            learner.MusterHead().Zero();
            std::mt19937 muster_rng(888U);
            std::normal_distribution<float> muster_n1(
                    0.0F, 1.0F / std::sqrt(static_cast<float>(ml::kMusterInDim)));
            std::normal_distribution<float> muster_n2(0.0F, 1.0F / std::sqrt(12.0F));
            for (float& weight : learner.MusterHead().w1) weight = muster_n1(muster_rng);
            for (float& weight : learner.MusterHead().w2) weight = muster_n2(muster_rng);
            learner.MusterHead().w_skip[ml::kContextDim + 0] = 0.04F;
            learner.MusterHead().w_skip[22] = -0.04F;
        }
        if (cfg.use_muster) learner.SetUseMuster(true);
        if (cfg.use_standard) learner.SetUseStandard(true);
    }
    if (cfg.avoid_blockers) {
        learner.SetAvoidBlockers(true);
    }
    learner.SetYieldToBlockers(cfg.yield_blockers);
    if (cfg.a_legacy_blockers) learner.SetAvoidBlockers(false);
    if (cfg.a_no_personalities) learner.SetUsePersonalities(false);
    ml::SetArmyScoreWeight(cfg.army_weight);
    ml::SetConcentrationScoreWeight(cfg.concentration_weight);
    std::printf("=== w2arena: PPO self-play | params %zu | threads %u | %s ===\n",
                ml::PolicyNetwork::kV8TrainableParams, threads,
                loaded ? ("loaded " + load_path).c_str() : "random init");

    if (cfg.benchmark_only || cfg.iterations == 0 || !cfg.compare_path.empty()) {
        // Seed set: development (--dev-seeds) > held-out report (--report-seeds) > selection.
        uint32_t base = cfg.dev_seeds > 0      ? kDevSeedBase
                        : cfg.report_seeds > 0 ? kReportSeedBase
                                               : kSelectSeedBase;
        int seeds = cfg.dev_seeds > 0      ? cfg.dev_seeds
                    : cfg.report_seeds > 0 ? cfg.report_seeds
                                           : cfg.select_seeds;
        const char* label = cfg.dev_seeds > 0 ? "dev" : cfg.report_seeds > 0 ? "report" : "select";
        if (!loaded) {
            std::fprintf(
                    stderr,
                    "Cannot load checkpoint '%s' for benchmarking; pass an existing --out file.\n",
                    load_path.c_str());
            return 1;
        }
        if (cfg.compare_path.empty()) {
            EvalResult result =
                    Summarize(std::move(RunEval(cfg, {&learner}, base, seeds, cfg.eval_turns)[0]));
            PrintEval(label, result);
            if (cfg.breakdown) PrintBreakdown(result);
            return 0;
        }
        ml::PolicyNetwork other;
        other.SetYieldToBlockers(cfg.b_yield_blockers);
        if (!other.LoadJson(cfg.compare_path).ok()) {
            std::fprintf(stderr,
                         "Cannot load --compare checkpoint '%s'; check the path and JSON format.\n",
                         cfg.compare_path.c_str());
            return 1;
        }
        if (cfg.has_b_detour_bias) {
            other.SetAvoidBlockers(true);
            other.SetDetourBias(cfg.b_detour_bias);
        }
        if (cfg.b_no_personalities) other.SetUsePersonalities(false);
        if (cfg.b_warlord_lookahead) other.SetWarlordLookahead(true);
        auto both = RunEval(cfg, {&learner, &other}, base, seeds, cfg.eval_turns);
        EvalResult result_a = Summarize(std::move(both[0])),
                   result_b = Summarize(std::move(both[1]));
        PrintEval((std::string(label) + " A").c_str(), result_a);
        if (cfg.breakdown) PrintBreakdown(result_a);
        PrintEval((std::string(label) + " B").c_str(), result_b);
        if (cfg.breakdown) PrintBreakdown(result_b);
        PrintPaired(load_path.c_str(), result_a, cfg.compare_path.c_str(), result_b);
        return 0;
    }

    // Trunk PPO modifies hidden layers (w1, w2), invalidating frozen specialist ArchetypeHeads
    // until re-evolved; evaluate and save the trained trunk directly without stale persona overrides.
    learner.SetUsePersonalities(false);
    learner.SetHasArchetypes(false);

    if (std::filesystem::path out_parent = std::filesystem::path(cfg.out_path).parent_path();
        !out_parent.empty()) {
        std::filesystem::create_directories(out_parent);
    }
    if (!cfg.log_path.empty()) {
        if (std::filesystem::path log_parent = std::filesystem::path(cfg.log_path).parent_path();
            !log_parent.empty()) {
            std::filesystem::create_directories(log_parent);
        }
    }

    std::string mode_label = cfg.solo_learner
                                     ? "solo-learner (1 vs K-1 Classic)"
                                     : absl::StrFormat("classic-frac %.2f", cfg.classic_frac);
    std::printf(
            "iters %d | games/iter %d | turns %d | sides %d (crop %d, rand %.2f) | epochs %d x %d "
            "minibatches | "
            "%s\n",
            cfg.iterations, cfg.games_per_iter, cfg.max_turns, cfg.active_sides, cfg.crop_margin,
            cfg.random_map_frac, cfg.epochs, cfg.minibatches, mode_label.c_str());

    FILE* logf = cfg.log_path.empty() ? nullptr : std::fopen(cfg.log_path.c_str(), "w");
    if (logf) {
        std::fprintf(logf,
                     "iter,seconds,temperature,lr,avg_cities,neutrals_left,entropy,value_loss,kl,"
                     "epochs_run,"
                     "decisions,select_share,select_ci,ev_value,ev_stack\n");
    }

    std::vector<ml::PolicyNetwork> hall_of_fame = {learner};
    ml::PolicyNetwork best = learner;
    int best_iter = 0;
    float best_share = -1.0F;

    const size_t param_count = ml::PolicyNetwork::kV8TrainableParams;
    const size_t off_diplo = 0;
    const size_t off_mission = off_diplo + ml::PolicyNetwork::DiploNet::kParamCount;
    const size_t off_econ = off_mission + ml::PolicyNetwork::MissionNet::kParamCount;
    const size_t off_value = off_econ + ml::PolicyNetwork::EconNet::kParamCount;
    const size_t off_stack_value = off_value + ml::PolicyNetwork::ValueNet::kParamCount;
    const size_t off_muster = off_stack_value + ml::PolicyNetwork::StackValueNet::kParamCount;
    const size_t off_commander = off_muster + ml::PolicyNetwork::MusterNet::kParamCount;
    const size_t off_detour = off_commander + ml::PolicyNetwork::kCommanderWeights + 5U;
    std::vector<float> params = learner.FlattenTrainable(), adam_m(param_count, 0.0F),
                       adam_v(param_count, 0.0F);
    std::vector<float> lr_mult(param_count, 1.0F);
    if (cfg.width_scaled_lr) {
        size_t offset = 0;
        auto mark = [&](auto tag, int base_h1) {
            using Mlp = decltype(tag);
            const int width_mult = std::max(1, Mlp::kH1Dim / base_h1);
            const float hidden = width_mult > 1 ? 1.0F / static_cast<float>(width_mult) : 1.0F;
            offset += static_cast<size_t>(Mlp::kOutDim) * Mlp::kInDim;
            offset += static_cast<size_t>(Mlp::kH1Dim) * Mlp::kInDim + Mlp::kH1Dim;
            size_t count = static_cast<size_t>(Mlp::kH2Dim) * Mlp::kH1Dim + Mlp::kH2Dim +
                           static_cast<size_t>(Mlp::kOutDim) * Mlp::kH2Dim + Mlp::kOutDim;
            for (size_t idx = 0; idx < count; idx++) lr_mult[offset + idx] = hidden;
            offset += count;
        };
        mark(ml::PolicyNetwork::DiploNet{}, 24);
        mark(ml::PolicyNetwork::MissionNet{}, 32);
        mark(ml::PolicyNetwork::EconNet{}, 24);
        mark(ml::PolicyNetwork::ValueNet{}, 24);
        mark(ml::PolicyNetwork::StackValueNet{}, 24);
        mark(ml::PolicyNetwork::MusterNet{}, 12);
    }
    int adam_step = 0;
    std::mt19937 shuffle_rng(12345);

    for (int iter = 1; iter <= cfg.iterations; iter++) {
        auto start_time = std::chrono::steady_clock::now();
        float progress =
                static_cast<float>(iter - 1) / static_cast<float>(std::max(1, cfg.iterations - 1));
        float temperature = cfg.temp_start + (cfg.temp_end - cfg.temp_start) * progress;
        float learning_rate = cfg.lr_start + (cfg.lr_end - cfg.lr_start) * progress;
        float inv_t = 1.0F / temperature;

        // ---- rollouts (scheduled Longest-Processing-Time-First to avoid tail stragglers)
        std::vector<RolloutSummary> games(cfg.games_per_iter);
        std::vector<int> game_order(cfg.games_per_iter);
        std::vector<int> game_cost(cfg.games_per_iter);
        for (int gi = 0; gi < cfg.games_per_iter; ++gi) {
            game_order[gi] = gi;
            uint32_t seed = cfg.seed_offset + static_cast<uint32_t>(iter * 1009 + gi * 97 + 17);
            const std::string& scen = kScenarios[(iter * 7 + gi) % kScenarios.size()];
            float map_roll = static_cast<float>(((seed * 1597334677U) ^ 0x9e3779b9U) >> 8) /
                             static_cast<float>(1U << 24);
            bool uses_random_map = (cfg.search_k == 0) && (map_roll < cfg.random_map_frac);
            int cost = uses_random_map ? 5 : ScenarioCostWeight(scen);
            float roll =
                    static_cast<float>((seed * 2654435761U) >> 8) / static_cast<float>(1U << 24);
            if (!cfg.solo_learner && !(roll < cfg.classic_frac)) cost *= 2;
            if ((kPresets[(iter + gi) % 4].hidden_map) != 0) cost += 1;
            game_cost[gi] = cost;
        }
        std::stable_sort(game_order.begin(), game_order.end(),
                         [&](int lhs, int rhs) { return game_cost[lhs] > game_cost[rhs]; });

        std::atomic<int> next_game{0};
        std::vector<std::thread> pool;
        for (unsigned int worker = 0; worker < std::min<unsigned int>(threads, cfg.games_per_iter);
             worker++) {
            pool.emplace_back([&]() {
                for (int slot; (slot = next_game.fetch_add(1)) < cfg.games_per_iter;) {
                    int game_idx = game_order[slot];
                    uint32_t seed = cfg.seed_offset +
                                    static_cast<uint32_t>(iter * 1009 + game_idx * 97 + 17);
                    const std::string& scen = kScenarios[(iter * 7 + game_idx) % kScenarios.size()];
                    const TrainingPreset& preset = kPresets[(iter + game_idx) % 4];
                    float roll = static_cast<float>((seed * 2654435761U) >> 8) /
                                 static_cast<float>(1U << 24);
                    Opponent mode = roll < cfg.classic_frac ? Opponent::kClassic
                                    : (game_idx % 2 == 0)   ? Opponent::kMirror
                                                            : Opponent::kLeague;
                    const auto& snap = hall_of_fame[(iter + game_idx) % hall_of_fame.size()];
                    games[game_idx] = cfg.search_k > 0 ? RunSearchRollout(cfg, scen, preset, seed,
                                                                          temperature, learner)
                                                       : RunRollout(cfg, scen, preset, seed,
                                                                    temperature, learner, snap,
                                                                    mode);
                }
            });
        }
        for (auto& thread : pool) thread.join();

        std::vector<SideEpisode> episodes;
        int cities = 0, sides = 0, neutrals = 0;
        for (auto& game_summary : games) {
            cities += game_summary.recorded_cities;
            sides += game_summary.recorded_sides;
            neutrals += game_summary.neutrals_left;
            for (auto& episode : game_summary.episodes) episodes.push_back(std::move(episode));
        }
        ComputeAdvantages(episodes, cfg);

        std::vector<const ml::TurnStepRecord*> turns;
        std::vector<float> m_adv, e_adv, d_adv, mst_adv;
        std::array<std::vector<float>, kNumEconFamilies> econ_fam_adv;
        for (const auto& episode : episodes) {
            for (const auto& turn : episode.turns) {
                turns.push_back(&turn);
                for (const auto& mission_step : turn.mission_steps)
                    m_adv.push_back(mission_step.advantage);
                for (const auto& econ_step : turn.econ_steps) {
                    e_adv.push_back(econ_step.advantage);
                    const size_t fam = static_cast<size_t>(econ_step.family);
                    if (fam < kNumEconFamilies) econ_fam_adv[fam].push_back(econ_step.advantage);
                }
                for (const auto& muster_step : turn.muster_steps)
                    mst_adv.push_back(muster_step.advantage);
                if (!turn.diplo_steps.empty()) d_adv.push_back(turn.advantage);
            }
        }
        Normalizers normalizers;
        normalizers.mission = Normalizer::Fit(m_adv);
        normalizers.econ = Normalizer::Fit(e_adv);
        normalizers.diplo = Normalizer::Fit(d_adv);
        normalizers.muster = Normalizer::Fit(mst_adv);
        size_t active_econ_fams = 0;
        size_t active_econ_total = 0;
        for (size_t fam = 0; fam < kNumEconFamilies; ++fam) {
            if (econ_fam_adv[fam].size() >= 8) {
                normalizers.econ_family[fam] = Normalizer::Fit(econ_fam_adv[fam]);
                normalizers.has_econ_family[fam] = true;
                active_econ_fams++;
                active_econ_total += econ_fam_adv[fam].size();
            } else {
                normalizers.econ_family_weight[fam] = 1.0F;
            }
        }
        if (active_econ_fams > 0) {
            const float mean_fam_steps =
                    static_cast<float>(active_econ_total) / static_cast<float>(active_econ_fams);
            for (size_t fam = 0; fam < kNumEconFamilies; ++fam) {
                if (normalizers.has_econ_family[fam]) {
                    float raw_w = std::sqrt(mean_fam_steps /
                                            static_cast<float>(econ_fam_adv[fam].size()));
                    normalizers.econ_family_weight[fam] = std::clamp(raw_w, 0.5F, 2.5F);
                }
            }
        }

        // Explained variance of the critics' rollout-time predictions:
        // 1 - Var(target - prediction) / Var(target); 1 is perfect, <= 0 is useless.
        auto explained_variance = [](const std::vector<float>& target,
                                     const std::vector<float>& pred) {
            if (target.size() < 2) return 0.0;
            double mean_target = 0.0, mean_residual = 0.0;
            for (size_t i = 0; i < target.size(); i++) {
                mean_target += target[i];
                mean_residual += target[i] - pred[i];
            }
            mean_target /= target.size();
            mean_residual /= target.size();
            double var_target = 0.0, var_residual = 0.0;
            for (size_t i = 0; i < target.size(); i++) {
                var_target += (target[i] - mean_target) * (target[i] - mean_target);
                double residual = target[i] - pred[i] - mean_residual;
                var_residual += residual * residual;
            }
            return var_target > 1e-12 ? 1.0 - var_residual / var_target : 0.0;
        };
        std::vector<float> v_target, v_pred, sv_target, sv_pred;
        for (const auto* turn : turns) {
            if (std::isnan(turn->return_target)) continue;
            v_target.push_back(turn->return_target);
            v_pred.push_back(turn->value_pred);
            for (const auto& mission_step : turn->mission_steps) {
                sv_target.push_back(mission_step.stack_return_target);
                sv_pred.push_back(mission_step.stack_value_pred);
            }
        }
        const double ev_value = explained_variance(v_target, v_pred),
                     ev_stack = explained_variance(sv_target, sv_pred);
        // How often a move empties an own city, and how often that city then falls
        long vacated = 0, vacated_lost = 0, mission_moves = 0;
        for (const auto* turn : turns) {
            for (const auto& mission_step : turn->mission_steps) {
                mission_moves++;
                if (mission_step.vacated_city == kNone) continue;
                vacated++;
                if (mission_step.vacated_loss > 0.0F) vacated_lost++;
            }
        }
        // Critic warm-up: only the critics learn for the first --critic-warmup iterations
        const bool warmup = iter <= cfg.critic_warmup;

        // ---- PPO: epochs x minibatches of turns, per-head mean losses
        HeadStats last_epoch;
        int epochs_run = 0;
        for (int epoch = 0; epoch < cfg.epochs; epoch++) {
            std::shuffle(turns.begin(), turns.end(), shuffle_rng);
            HeadStats epoch_stats;
            size_t mb_size = (turns.size() + cfg.minibatches - 1) / cfg.minibatches;
            for (size_t mb0 = 0; mb0 < turns.size(); mb0 += mb_size) {
                size_t mb1 = std::min(turns.size(), mb0 + mb_size);
                std::atomic<size_t> next_turn{mb0};
                unsigned int grad_workers = std::min<unsigned int>(
                        threads, static_cast<unsigned int>(std::max<size_t>(1, mb1 - mb0)));
                std::vector<ml::PolicyNetwork> grads(grad_workers);
                std::vector<HeadStats> stats(grad_workers);
                std::vector<std::thread> gpool;
                for (unsigned int worker = 0; worker < grad_workers; worker++) {
                    grads[worker].Zero();
                    gpool.emplace_back([&, worker]() {
                        std::vector<float> scores, d_scores, logits_buf, probs_buf, log_probs_buf;
                        std::vector<ml::PolicyNetwork::MissionNet::Activations> mission_acts;
                        std::vector<ml::PolicyNetwork::EconNet::Activations> econ_acts;
                        std::vector<ml::PolicyNetwork::MusterNet::Activations> muster_acts;
                        std::vector<ml::MissionInVec> mission_in_buf;
                        std::vector<ml::EconInVec> econ_in_buf;
                        std::vector<ml::MusterInVec> muster_in_buf;
                        for (size_t ti; (ti = next_turn.fetch_add(1)) < mb1;) {
                            AccumulateTurn(learner, *turns[ti], normalizers, inv_t, cfg,
                                           grads[worker], stats[worker], scores, d_scores,
                                           logits_buf, probs_buf, log_probs_buf, mission_acts,
                                           econ_acts, muster_acts, mission_in_buf, econ_in_buf,
                                           muster_in_buf);
                        }
                    });
                }
                for (auto& thread : gpool) thread.join();

                HeadStats mb_stats;
                std::vector<float> total_grad(param_count, 0.0F);
                for (unsigned int worker = 0; worker < grad_workers; worker++) {
                    auto flat_grad = grads[worker].FlattenTrainable();
                    for (size_t param_idx = 0; param_idx < param_count; param_idx++)
                        total_grad[param_idx] += flat_grad[param_idx];
                    mb_stats.Add(stats[worker]);
                }
                // Each head's loss is a mean over its own decisions; the critic's over turns
                auto scale = [&](size_t begin_idx, size_t end_idx, float factor) {
                    for (size_t param_idx = begin_idx; param_idx < end_idx; param_idx++)
                        total_grad[param_idx] *= factor;
                };
                const int actor_decisions =
                        mb_stats.diplo + mb_stats.mission + mb_stats.econ + mb_stats.muster;
                scale(off_diplo, off_mission, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.diplo));
                scale(off_mission, off_econ, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.mission));
                scale(off_econ, off_value, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.econ));
                scale(off_value, off_stack_value, cfg.value_coef / std::max(1, mb_stats.turns));
                scale(off_stack_value, off_muster,
                      cfg.value_coef / std::max(1, mb_stats.stack_values));
                scale(off_muster, off_commander,
                      (warmup || mb_stats.muster == 0) ? 0.0F
                                                       : (1.0F / static_cast<float>(mb_stats.muster)));
                scale(off_commander, off_detour,
                      (warmup || !learner.UseCommander() || actor_decisions == 0)
                              ? 0.0F
                              : (1.0F / static_cast<float>(actor_decisions)));
                scale(off_detour, param_count,
                      (warmup || !learner.AvoidBlockers() || mb_stats.detour == 0)
                              ? 0.0F
                              : (1.0F / static_cast<float>(mb_stats.detour)));
                std::vector<float> clip_of(param_count, 1.0F);
                auto clip_range = [&](size_t begin_idx, size_t end_idx) {
                    double norm = 0.0;
                    for (size_t idx = begin_idx; idx < end_idx; idx++) {
                        norm += static_cast<double>(total_grad[idx]) * total_grad[idx];
                    }
                    float scale_norm =
                            std::sqrt(norm) > cfg.max_grad_norm
                                    ? cfg.max_grad_norm / static_cast<float>(std::sqrt(norm))
                                    : 1.0F;
                    for (size_t idx = begin_idx; idx < end_idx; idx++) clip_of[idx] = scale_norm;
                };
                if (cfg.clip_per_head) {
                    clip_range(off_diplo, off_mission);
                    clip_range(off_mission, off_econ);
                    clip_range(off_econ, off_value);
                    clip_range(off_value, off_stack_value);
                    clip_range(off_stack_value, off_muster);
                    clip_range(off_muster, off_commander);
                    clip_range(off_commander, off_detour);
                    clip_range(off_detour, param_count);
                } else {
                    clip_range(0, param_count);
                }
                adam_step++;
                float bc1 = 1.0F - std::pow(0.9F, static_cast<float>(adam_step)),
                      bc2 = 1.0F - std::pow(0.999F, static_cast<float>(adam_step));
                for (size_t param_idx = 0; param_idx < param_count; param_idx++) {
                    float clipped_grad = total_grad[param_idx] * clip_of[param_idx];
                    adam_m[param_idx] = 0.9F * adam_m[param_idx] + 0.1F * clipped_grad;
                    adam_v[param_idx] =
                            0.999F * adam_v[param_idx] + 0.001F * clipped_grad * clipped_grad;
                    params[param_idx] -= learning_rate * lr_mult[param_idx] *
                                         (adam_m[param_idx] / bc1) /
                                         (std::sqrt(adam_v[param_idx] / bc2) + 1e-8F);
                }
                learner.UnflattenTrainable(params);
                epoch_stats.Add(mb_stats);
            }
            last_epoch = epoch_stats;
            epochs_run++;
            double epoch_kl = epoch_stats.kl / std::max(1, epoch_stats.kl_samples);
            if (epoch_kl > cfg.target_kl)
                break;  // the policy has moved far enough from the one that played
        }

        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time)
                             .count();
        double ent = last_epoch.entropy / std::max(1, last_epoch.ent_samples);
        double vloss = last_epoch.value_loss / std::max(1, last_epoch.turns);
        double mean_kl = last_epoch.kl / std::max(1, last_epoch.kl_samples);
        float avg_cities = static_cast<float>(cities) / static_cast<float>(std::max(1, sides));
        float avg_neutrals = static_cast<float>(neutrals) / static_cast<float>(cfg.games_per_iter);
        size_t decisions = m_adv.size() + e_adv.size() + d_adv.size() + mst_adv.size();
        std::printf(
                "[iter %3d] %.1fs T=%.2f lr=%.1e | cities/side %.2f | neutrals %.1f | ent %.3f | "
                "vloss %.4f | "
                "ev %.2f/%.2f | kl %.4f (%d ep) | %zu turns | emptied %.1f%% of moves, %.0f%% "
                "fell%s\n",
                iter, sec, temperature, learning_rate, avg_cities, avg_neutrals, ent, vloss,
                ev_value, ev_stack, mean_kl, epochs_run, turns.size(),
                100.0 * vacated / std::max(1L, mission_moves),
                100.0 * vacated_lost / std::max(1L, vacated), warmup ? " | critic warm-up" : "");
        std::fflush(stdout);

        if (cfg.save_every > 0 && iter % cfg.save_every == 0) {
            std::filesystem::path snap = cfg.out_path;
            snap.replace_extension(absl::StrFormat(".iter%d.json", iter));
            learner.SaveJson(snap, iter, 0.0F).IgnoreError();
        }
        if (iter % 10 == 0) {
            if (hall_of_fame.size() < 12)
                hall_of_fame.push_back(learner);
            else
                hall_of_fame[(iter / 10) % hall_of_fame.size()] = learner;
        }

        float sel_share = NAN, sel_ci = NAN;
        if (iter % cfg.eval_every == 0 || iter == cfg.iterations) {
            EvalResult sel = EvaluateVsClassic(cfg, learner, kSelectSeedBase, cfg.select_seeds,
                                               cfg.eval_turns);
            PrintEval(("select @" + std::to_string(iter)).c_str(), sel);
            sel_share = static_cast<float>(sel.mean_share);
            sel_ci = static_cast<float>(sel.ci95);
            if (sel_share > best_share) {
                best_share = sel_share;
                best = learner;
                best_iter = iter;
                best.SaveJson(cfg.out_path, best_iter, best_share).IgnoreError();
            }
        }
        if (logf) {
            std::fprintf(
                    logf, "%d,%.2f,%.3f,%.2e,%.3f,%.2f,%.4f,%.5f,%.5f,%d,%zu,%.4f,%.4f,%.4f,%.4f\n",
                    iter, sec, temperature, learning_rate, avg_cities, avg_neutrals, ent, vloss,
                    mean_kl, epochs_run, decisions, sel_share, sel_ci, ev_value, ev_stack);
            std::fflush(logf);
        }
    }
    if (logf) std::fclose(logf);

    best.SaveJson(cfg.out_path, best_iter, best_share).IgnoreError();
    {
        std::filesystem::path fin = cfg.out_path;
        fin.replace_extension(".final.json");
        learner.SaveJson(fin, cfg.iterations, 0.0F).IgnoreError();
    }
    if (cfg.report_seeds > 0) {
        std::printf("Kept iteration %d (selection share %.1f%%) in %s. Held-out report:\n",
                    best_iter, best_share * 100.0F, cfg.out_path.c_str());
        PrintEval("report",
                  EvaluateVsClassic(cfg, best, kReportSeedBase, cfg.report_seeds, cfg.eval_turns));
    } else {
        std::printf(
                "Kept iteration %d (selection share %.1f%%) in %s (report skipped: --report-seeds "
                "0).\n",
                best_iter, best_share * 100.0F, cfg.out_path.c_str());
    }
    return 0;
}
