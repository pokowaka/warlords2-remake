// Self-play reinforcement-learning trainer for the Modern AI (`w2arena`).
//
// Trains the 4-head PolicyNetwork with PPO + GAE in all-computer games.
//
// Objective. A side's reward each turn is the change in its empire score S
// (computeEmpireScore: 2 x city share + 0.5 x income share + 0.25 x army share,
// plus a small reconnaissance term under Hidden Map), measured from the start
// of its turn to the start of its next one, so losses suffered during rivals'
// turns count. Winning adds +1 and elimination -0.5 (and S drops to 0).
// Training games are cut off after --turns turns; that cut-off is a time limit,
// not an end of the game, so the critic's estimate is bootstrapped there.
// The objective is therefore the discounted growth of S with no fixed horizon,
// which matches open-ended real games. S is a hand-chosen definition of doing
// well; everything else (targets, coalitions, production, diplomacy) is learned.
//
// Opponents. Each game is either a mirror match (every side plays the current
// learner and is recorded), a league match (half the sides play a snapshot
// from the hall of fame), or -- only with --classic-frac > 0 -- a match against
// the 1993 Classic AI. By default the Classic AI is used solely for evaluation.
//
// Evaluation. Two disjoint seed sets, both disjoint from training seeds:
//   selection: decides which checkpoint is kept (--select-seeds);
//   report:    measured once at the end, never used for any decision
//              (--report-seeds).
// Each seed is played twice with the Modern/Classic side assignment swapped,
// half the sides each. The figure of merit is the Modern AI's share of the
// cities held by the two camps when the game is cut off, with a 95% interval.
//
// Usage (from the repository root):
//   bazel-bin/cpp/w2arena [--iterations N] [--games-per-iter G] [--turns T]
//                     [--eval-every E] [--eval-turns T] [--select-seeds S]
//                     [--report-seeds R] [--classic-frac F] [--from-scratch]
//                     [--out PATH] [--log CSV] [--benchmark-only]

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

#include "opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"
#include "warlords/save.hpp"

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

struct TrainingPreset {
    const char* label;
    int hidden_map;
    int diplomacy;
    int neutral_cities;
};

constexpr TrainingPreset kPresets[4] = {
    {"Beginner-Open", 0, 0, 0},
    {"Interm-Diplo", 0, 1, 1},
    {"Adv-Hidden", 1, 1, 2},
    {"Fog-TotalWar", 1, 0, 1},
};

const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};

// Seed ranges: training uses iter * 1009 + game * 97 + 17 (< 700000 for any
// sane run); evaluation sets live well above it and never overlap.
constexpr uint32_t kSelectSeedBase = 700000;
constexpr uint32_t kReportSeedBase = 800000;
// Development A/B comparisons (--compare / --dev-seeds) use their own set, so
// neither in-run selection nor the held-out report is spent on tuning.
constexpr uint32_t kDevSeedBase = 750000;

enum class EvalMode { kMixed, kSolo };

struct Config {
    std::string data_dir = "original";
    std::string out_path = "cpp/ai_weights.json";
    std::string log_path;
    int iterations = 300;
    int games_per_iter = 48;
    int max_turns = 40;
    int eval_every = 10;
    int eval_turns = 40;
    int select_seeds = 12;
    int report_seeds = 32;
    int epochs = 4;
    int minibatches = 8;
    float lr_start = 3.0e-3F;
    float lr_end = 3.0e-4F;
    float temp_start = 1.0F;
    float temp_end = ml::kTrainTemperatureEnd;
    float eval_temp =
            ml::kDeployTemperature;  // --eval-temp: temperature evaluation games sample at
    float clip_eps = 0.2F;
    float ent_coef = 0.01F;
    float value_coef = 0.5F;
    float gamma = 0.97F;
    float gae_lambda = 0.95F;
    float local_weight = 1.0F;
    float target_kl = 0.03F;
    float max_grad_norm = 0.5F;
    float classic_frac = 0.0F;
    float random_map_frac = 0.0F;
    int active_sides = 8;
    int crop_margin = 0;
    uint32_t seed_offset = 0;
    float win_reward = 1.0F;
    float elimination_reward = -0.5F;
    bool solo_learner = false;
    bool from_scratch = false;
    bool benchmark_only = false;
    EvalMode eval_mode = EvalMode::kMixed;
    std::string compare_path;  // --compare: B checkpoint, A is --out
    int dev_seeds = 0;         // --dev-seeds: benchmark/compare on the development set
    bool breakdown = false;
    int critic_warmup = 0;    // --critic-warmup: iterations where only the critics learn
    float mixed_frac = 0.0F;  // --mixed-frac: solo-learner games played 4 vs 4 instead (all learner
                              // seats recorded)
    int save_every = 0;       // --save-every: also write <out>.iterN.json snapshots
    float raider_frac =
            0.0F;  // --raider-frac: share of Classic training seats played by the Raider
    float departure_weight =
            0.0F;             // --departure-weight: credit a city loss to the move that emptied it
    std::string frozen_path;  // --frozen-opponent: fixed Modern checkpoint some opponent seats play
    float frozen_frac = 0.0F;  // --frozen-frac: share of non-learner training seats played by it
    int search_k = 0;  // --search-k: candidate turns per learner turn (0 = no search training)
    int search_r = 2;  // --search-r: rollouts (dice copies) per candidate
    int search_h = 2;  // --search-h: rounds played on after a candidate turn
    opponents::Kind eval_opponent = opponents::Kind::kClassic;  // --eval-opponent classic|raider
};

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

std::unique_ptr<Game> MakeArenaGame(const Config& cfg, const std::string& scenario,
                                    const TrainingPreset& preset, uint32_t seed,
                                    bool allow_random_map) {
    auto opts = GameOptions(cfg, preset, seed);
    float map_roll = static_cast<float>(((seed * 1597334677U) ^ 0x9e3779b9U) >> 8) /
                     static_cast<float>(1U << 24);
    if ((allow_random_map && map_roll < cfg.random_map_frac) || scenario == "RANDOM") {
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options ropts;
        ropts.data_dir = cfg.data_dir;
        ropts.rng = &map_rng;
        ropts.sliders = {3, 3, 2, 3};
        ropts.allies = preset.diplomacy != 0;
        ropts.terrain_set = 0;
        auto files = randommap::GenerateNow(ropts);
        opts.memory_files = &files;
        return game::NewGame(cfg.data_dir, "RANDOM", opts);
    }
    return game::NewGame(cfg.data_dir, scenario, opts);
}

// ---------------------------------------------------------------- rollouts

struct SideEpisode {
    int side_index = 0;
    std::vector<ml::TurnStepRecord> turns;
    float final_score = 0.0F;      // S when the game ended or was cut off
    bool terminal = false;         // eliminated or game over: no bootstrap
    float terminal_reward = 0.0F;  // win / elimination bonus
    float bootstrap_value = 0.0F;  // V(s) at the cut-off when not terminal
    int final_cities = 0;
    bool searched = false;  // turns carry search-based advantages (no GAE)
};

struct RolloutSummary {
    std::vector<SideEpisode> episodes;
    int recorded_cities = 0;
    int recorded_sides = 0;
    int neutrals_left = 0;
};

enum class Opponent { kMirror, kLeague, kClassic };

// The fixed Modern checkpoint Frozen opponents play (--frozen-opponent); set
// once in main before any game starts.
const ml::PolicyNetwork* g_frozen = nullptr;

// Departure credit: for each move of `turn` that left an own city empty (and
// still empty when the turn ended), record that city's weight in the empire
// score if `side` no longer holds it now, i.e. it fell before this side's next
// turn. The loss is already in the turn's reward; this only says which move
// caused it.
void MarkDepartures(const Game& g, int side, ml::TurnStepRecord& turn) {
    int standing = 0, total_income = 0;
    for (const auto& c : g.map->cities) {
        if (c.razed) continue;
        standing++;
        total_income += std::max(1, c.income);
    }
    for (auto& ms : turn.mission_steps) {
        if (ms.vacated_city == kNone || ms.vacated_city >= static_cast<int>(g.map->cities.size()))
            continue;
        const City& c = g.map->cities[ms.vacated_city];
        if (!c.razed && c.owner_index == side) continue;
        ms.vacated_loss = 2.0F / static_cast<float>(std::max(1, standing)) +
                          0.5F * static_cast<float>(std::max(1, c.income)) /
                                  static_cast<float>(std::max(1, total_income));
    }
}

RolloutSummary RunRollout(const Config& cfg, const std::string& scenario,
                          const TrainingPreset& preset, uint32_t seed, float temperature,
                          const ml::PolicyNetwork& learner, const ml::PolicyNetwork& snapshot,
                          Opponent mode) {
    auto gp = MakeArenaGame(cfg, scenario, preset, seed, true);
    Game& g = *gp;

    std::array<bool, 8> recorded{};
    std::array<bool, 8> classic{};
    std::array<opponents::Kind, 8> classic_kind{};
    std::array<SideEpisode, 8> eps{};
    std::array<ml::RolloutSampler, 8> samplers{};
    int active_count = 0;
    for (int s = 0; s < 8; s++) {
        if (g.map->sides[s].in_use) active_count++;
    }
    int solo_seat =
            active_count > 0 ? static_cast<int>(seed % static_cast<uint32_t>(active_count)) : 0;
    // --mixed-frac: some solo-learner games are played like the benchmark instead,
    // alternating learner and Classic seats, with every learner seat recorded.
    float mix_roll = static_cast<float>(((seed * 2246822519U) ^ 0x85ebca6bU) >> 8) /
                     static_cast<float>(1U << 24);
    const bool solo = cfg.solo_learner && !(mix_roll < cfg.mixed_frac);
    const bool mixed_vs_classic = cfg.solo_learner && !solo;
    int idx = 0;
    for (int s = 0; s < 8; s++) {
        Side& side = g.map->sides[s];
        if (!side.in_use) continue;
        bool learner_side = solo ? (idx == solo_seat)
                            : mixed_vs_classic
                                    ? ((idx + seed) % 2 == 0)
                                    : ((mode == Opponent::kMirror) || ((idx + seed) % 2 == 0));
        idx++;
        recorded[s] = learner_side;
        classic[s] = (solo || mixed_vs_classic) ? !learner_side
                                                : ((mode == Opponent::kClassic) && !learner_side);
        side.classic_ai = classic[s];
        float raid_roll =
                static_cast<float>(
                        ((seed * 747796405U) ^ static_cast<uint32_t>((s + 1) * 2891336453U)) >> 8) /
                static_cast<float>(1U << 24);
        float frozen_roll = static_cast<float>(((seed * 2654435769U) ^
                                                static_cast<uint32_t>((s + 1) * 3266489917U)) >>
                                               8) /
                            static_cast<float>(1U << 24);
        classic_kind[s] =
                raid_roll < cfg.raider_frac ? opponents::Kind::kRaider : opponents::Kind::kClassic;
        if (classic[s] && g_frozen && frozen_roll < cfg.frozen_frac) {
            classic_kind[s] = opponents::Kind::kFrozen;
            side.classic_ai = false;  // a Modern AI seat, just not a learning one
        }
        eps[s].side_index = s;
        samplers[s].rng_state = (seed * 2654435761U) ^ static_cast<uint32_t>((s + 1) * 2246822519U);
        samplers[s].temperature = temperature;
    }

    Side* cur = game::Begin(g);
    int safety = 0;
    while (cur && !g.over && !g.won && g.turn <= cfg.max_turns && safety < cfg.max_turns * 12) {
        safety++;
        int s = cur->index;
        if (cur->computer && cur->alive) {
            if (recorded[s]) {
                if (!eps[s].turns.empty()) MarkDepartures(g, s, eps[s].turns.back());
                eps[s].turns.emplace_back();
                samplers[s].current_turn = &eps[s].turns.back();
                ai::modern::PlayTurn(g, *cur, nullptr, &learner, &samplers[s]);
                samplers[s].current_turn = nullptr;
            } else if (classic[s]) {
                opponents::PlayTurn(g, *cur, classic_kind[s], g_frozen);
            } else {
                ml::RolloutSampler opp;
                opp.rng_state = samplers[s].rng_state ^ static_cast<uint32_t>(g.turn * 374761393U);
                opp.temperature = temperature;
                ai::modern::PlayTurn(g, *cur, nullptr, &snapshot, &opp);
            }
        }
        cur = game::EndTurn(g);
    }

    RolloutSummary out;
    std::array<int, 8> cities{};
    for (const auto& c : g.map->cities) {
        if (c.razed) continue;
        if (c.owner_index == kNone)
            out.neutrals_left++;
        else if (c.owner_index >= 0 && c.owner_index < 8)
            cities[c.owner_index]++;
    }
    for (int s = 0; s < 8; s++) {
        if (!recorded[s] || eps[s].turns.empty()) continue;
        Side& side = g.map->sides[s];
        SideEpisode& ep = eps[s];
        MarkDepartures(g, s, ep.turns.back());
        ep.final_cities = cities[s];
        ep.final_score = ml::ComputeEmpireScore(g, s);
        if (!side.alive) {
            ep.terminal = true;
            ep.terminal_reward = cfg.elimination_reward;
        } else if (g.over || g.won) {
            ep.terminal = true;
            bool won_game = (g.ending.winner && g.ending.winner->index == s);
            ep.terminal_reward = won_game ? cfg.win_reward : 0.0F;
        } else {
            ai::belief::Update(g, side);
            ep.bootstrap_value = learner.EvaluateValue(ml::BuildContext(g, side),
                                                       ml::BuildCriticFeatures(g, side));
        }
        out.recorded_cities += cities[s];
        out.recorded_sides++;
        out.episodes.push_back(std::move(ep));
    }
    return out;
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

std::unique_ptr<Game> CopyGame(const Config& cfg, const Game& g) {
    return save::Decode(save::Encode(g), cfg.data_dir);
}

RolloutSummary RunSearchRollout(const Config& cfg, const std::string& scenario,
                                const TrainingPreset& preset, uint32_t seed, float temperature,
                                const ml::PolicyNetwork& learner) {
    auto gp = MakeArenaGame(cfg, scenario, preset, seed, false);
    int active_count = 0;
    for (int s = 0; s < 8; s++) {
        if (gp->map->sides[s].in_use) active_count++;
    }
    const int solo_seat =
            active_count > 0 ? static_cast<int>(seed % static_cast<uint32_t>(active_count)) : 0;
    int learner_side = kNone;
    std::array<opponents::Kind, 8> kind{};
    int idx = 0;
    for (int s = 0; s < 8; s++) {
        Side& side = gp->map->sides[s];
        if (!side.in_use) continue;
        const bool me = (idx++ == solo_seat);
        if (me) learner_side = s;
        side.classic_ai = !me;
        float raid_roll =
                static_cast<float>(
                        ((seed * 747796405U) ^ static_cast<uint32_t>((s + 1) * 2891336453U)) >> 8) /
                static_cast<float>(1U << 24);
        float frozen_roll = static_cast<float>(((seed * 2654435769U) ^
                                                static_cast<uint32_t>((s + 1) * 3266489917U)) >>
                                               8) /
                            static_cast<float>(1U << 24);
        kind[s] =
                raid_roll < cfg.raider_frac ? opponents::Kind::kRaider : opponents::Kind::kClassic;
        if (!me && g_frozen && frozen_roll < cfg.frozen_frac) {
            kind[s] = opponents::Kind::kFrozen;
            side.classic_ai = false;
        }
    }

    // Every side's normal play; the learner plays greedily outside its searched turn
    auto play_side = [&](Game& g, Side& side) {
        if (side.index == learner_side)
            ai::modern::PlayTurn(g, side, nullptr, &learner, nullptr);
        else
            opponents::PlayTurn(g, side, kind[side.index], g_frozen);
    };
    auto rollout_value = [&](Game& g) {
        const int stop_turn = g.turn + cfg.search_h;
        Side* cur = game::EndTurn(g);
        int safety = 0;
        while (cur && !g.over && !g.won && g.turn < stop_turn && safety++ < cfg.search_h * 12) {
            if (cur->computer && cur->alive) play_side(g, *cur);
            cur = game::EndTurn(g);
        }
        return ml::ComputeEmpireScore(g, learner_side);
    };

    RolloutSummary out;
    SideEpisode search_ep;
    search_ep.side_index = learner_side;
    search_ep.searched = true;
    Side* cur = game::Begin(*gp);
    int safety = 0;
    while (cur && !gp->over && !gp->won && gp->turn <= cfg.max_turns &&
           safety++ < cfg.max_turns * 12) {
        const int s = cur->index;
        if (cur->computer && cur->alive && s == learner_side) {
            std::vector<std::unique_ptr<Game>> after;
            std::vector<ml::TurnStepRecord> recs(cfg.search_k);
            std::vector<double> value(cfg.search_k, 0.0);
            for (int k = 0; k < cfg.search_k; k++) {
                auto cand = CopyGame(cfg, *gp);
                ml::RolloutSampler sampler;
                sampler.temperature = temperature;
                sampler.rng_state = (seed * 2654435761U) ^
                                    static_cast<uint32_t>((gp->turn + 1) * 40503U) ^
                                    static_cast<uint32_t>((k + 1) * 2246822519U);
                sampler.current_turn = &recs[k];
                ai::modern::PlayTurn(*cand, *cand->map->Side(s), nullptr, &learner, &sampler);
                sampler.current_turn = nullptr;
                for (int r = 0; r < cfg.search_r; r++) {
                    auto copy = CopyGame(cfg, *cand);
                    copy->rng.state ^= static_cast<uint32_t>((r + 1) * 0x9e3779b9U);
                    value[k] += rollout_value(*copy);
                }
                value[k] /= cfg.search_r;
                after.push_back(std::move(cand));
            }
            double mean = 0.0;
            for (double v : value) mean += v;
            mean /= std::max(1, cfg.search_k);
            size_t best = 0;
            for (int k = 0; k < cfg.search_k; k++) {
                if (value[k] > value[best]) best = static_cast<size_t>(k);
                auto& tr = recs[k];
                tr.advantage = static_cast<float>(value[k] - mean);
                tr.return_target = std::numeric_limits<float>::quiet_NaN();  // no critic target
                for (auto& ms : tr.mission_steps) {
                    ms.advantage = tr.advantage;
                    ms.stack_return_target = std::numeric_limits<float>::quiet_NaN();
                }
                search_ep.turns.push_back(std::move(tr));
            }
            gp = std::move(after[best]);
            cur = gp->map->Side(s);
        } else if (cur->computer && cur->alive) {
            play_side(*gp, *cur);
        }
        cur = game::EndTurn(*gp);
    }
    for (const auto& c : gp->map->cities) {
        if (c.razed) continue;
        if (c.owner_index == kNone)
            out.neutrals_left++;
        else if (c.owner_index == learner_side)
            out.recorded_cities++;
    }
    out.recorded_sides = 1;
    if (!search_ep.turns.empty()) out.episodes.push_back(std::move(search_ep));
    return out;
}

// GAE over each side's own turns, plus optional per-stack multi-turn local
// credit tracked over non-recycled Game::pool Army* identities.
void ComputeAdvantages(std::vector<SideEpisode>& episodes, float gamma, float lambda,
                       float local_weight, float departure_weight) {
    constexpr float kStackGamma = 0.90F;
    // Departure credit is centred over all moves that emptied a city: emptying
    // one that then fell scores below average, emptying one that held scores
    // above, so the signal teaches *when* to leave, not "never leave".
    double vac_sum = 0.0;
    int vac_count = 0;
    for (const auto& ep : episodes) {
        for (const auto& tr : ep.turns) {
            for (const auto& ms : tr.mission_steps) {
                if (ms.vacated_city == kNone) continue;
                vac_sum += ms.vacated_loss;
                vac_count++;
            }
        }
    }
    const float vac_mean = vac_count > 0 ? static_cast<float>(vac_sum / vac_count) : 0.0F;
    for (auto& ep : episodes) {
        if (ep.searched) continue;  // advantages came from comparing candidate turns
        const int turn_count = static_cast<int>(ep.turns.size());
        float gae = 0.0F;
        std::unordered_map<std::uintptr_t, float> unit_future_local;
        for (int t = turn_count - 1; t >= 0; t--) {
            auto& cur = ep.turns[t];
            bool last = (t == turn_count - 1);
            float next_score = last ? ep.final_score : ep.turns[t + 1].start_score;
            float next_value =
                    last ? (ep.terminal ? 0.0F : ep.bootstrap_value) : ep.turns[t + 1].value_pred;
            float reward = (next_score - cur.start_score) + (last ? ep.terminal_reward : 0.0F);
            float delta = reward + gamma * next_value - cur.value_pred;
            gae = delta + gamma * lambda * gae;
            cur.advantage = gae;
            cur.return_target = gae + cur.value_pred;

            for (auto& [uid, val] : unit_future_local) val *= kStackGamma;
            for (int mi = static_cast<int>(cur.mission_steps.size()) - 1; mi >= 0; mi--) {
                auto& ms = cur.mission_steps[mi];
                if (!ms.unit_ids.empty()) {
                    float per_unit =
                            ms.immediate_delta_score / static_cast<float>(ms.unit_ids.size());
                    for (std::uintptr_t uid : ms.unit_ids) {
                        unit_future_local[uid] += per_unit;
                    }
                }
                const auto& eval_ids = ms.pool_unit_ids.empty() ? ms.unit_ids : ms.pool_unit_ids;
                float group_return = 0.0F;
                for (std::uintptr_t uid : eval_ids) {
                    auto it = unit_future_local.find(uid);
                    if (it != unit_future_local.end()) group_return += it->second;
                }
                ms.stack_return_target = group_return;
                float local_adv = ms.stack_return_target - ms.stack_value_pred;
                ms.advantage = cur.advantage + local_weight * local_adv;
                if (ms.vacated_city != kNone)
                    ms.advantage -= departure_weight * (ms.vacated_loss - vac_mean);
            }
        }
    }
}

struct Normalizer {
    float mean = 0.0F, inv_std = 1.0F;
    float operator()(float x) const { return std::clamp((x - mean) * inv_std, -5.0F, 5.0F); }
    static Normalizer Fit(const std::vector<float>& v) {
        Normalizer n;
        if (v.size() < 2) return n;
        double s = 0.0, sq = 0.0;
        for (float x : v) s += x;
        n.mean = static_cast<float>(s / v.size());
        for (float x : v)
            sq += (static_cast<double>(x) - n.mean) * (static_cast<double>(x) - n.mean);
        n.inv_std = 1.0F / std::max(1e-4F, static_cast<float>(std::sqrt(sq / v.size())));
        return n;
    }
};

// ---------------------------------------------------------------- PPO maths

struct HeadStats {
    int mission = 0, econ = 0, diplo = 0, turns = 0, stack_values = 0;
    double policy_loss = 0.0, value_loss = 0.0, entropy = 0.0, kl = 0.0;
    int kl_samples = 0, ent_samples = 0;
    void Add(const HeadStats& o) {
        mission += o.mission;
        econ += o.econ;
        diplo += o.diplo;
        turns += o.turns;
        stack_values += o.stack_values;
        policy_loss += o.policy_loss;
        value_loss += o.value_loss;
        entropy += o.entropy;
        kl += o.kl;
        kl_samples += o.kl_samples;
        ent_samples += o.ent_samples;
    }
};

// Clipped-surrogate gradient for a categorical choice over options with policy
// logits z_i = u_i / T - logMult_i, plus a KL pull toward the multiplicity
// prior q_i ~ exp(-logMult_i) (entropy regularisation over action families).
// Writes dLoss/du_i into dU.
void CategoricalPpo(const std::vector<float>& u, const std::vector<float>* log_mult, int chosen,
                    float old_log_prob, float adv, float inv_t, const Config& cfg,
                    std::vector<float>& d_u, HeadStats& st) {
    const size_t candidate_count = u.size();
    d_u.assign(candidate_count, 0.0F);
    if (candidate_count <= 1) return;
    auto lm_of = [&](size_t i) {
        return (log_mult && i < log_mult->size()) ? (*log_mult)[i] : 0.0F;
    };
    float max_z = -1e30F;
    std::vector<float> z(candidate_count);
    for (size_t i = 0; i < candidate_count; i++) {
        z[i] = u[i] * inv_t - lm_of(i);
        max_z = std::max(max_z, z[i]);
    }
    std::vector<float> p(candidate_count);
    float sum = 0.0F;
    for (size_t i = 0; i < candidate_count; i++) {
        p[i] = std::exp(std::max(-30.0F, z[i] - max_z));
        sum += p[i];
    }
    for (float& v : p) v /= sum;

    float new_lp = std::log(std::max(1e-12F, p[chosen]));
    float log_ratio = std::clamp(new_lp - old_log_prob, -10.0F, 10.0F);
    float ratio = std::exp(log_ratio);
    st.policy_loss += -std::min(ratio * adv,
                                std::clamp(ratio, 1.0F - cfg.clip_eps, 1.0F + cfg.clip_eps) * adv);
    st.kl += (ratio - 1.0F) - log_ratio;
    st.kl_samples++;
    bool clipped = (adv > 0.0F && ratio > 1.0F + cfg.clip_eps) ||
                   (adv < 0.0F && ratio < 1.0F - cfg.clip_eps);
    float d_loss_d_log_p = clipped ? 0.0F : -ratio * adv;

    float mean_adj = 0.0F, ent = 0.0F;
    for (size_t j = 0; j < candidate_count; j++) {
        float lp = std::log(std::max(1e-12F, p[j]));
        mean_adj += p[j] * (lp + lm_of(j));
        ent -= p[j] * lp;
    }
    float norm_log_k = std::max(1.0F, std::log(static_cast<float>(candidate_count)));
    st.entropy += ent / norm_log_k;
    st.ent_samples++;
    float kl_coef = cfg.ent_coef / norm_log_k;
    for (size_t j = 0; j < candidate_count; j++) {
        float ind = (static_cast<int>(j) == chosen) ? 1.0F : 0.0F;
        float lp = std::log(std::max(1e-12F, p[j]));
        d_u[j] = d_loss_d_log_p * (ind - p[j]) * inv_t +
                 kl_coef * p[j] * ((lp + lm_of(j)) - mean_adj) * inv_t;
    }
}

// Clipped-surrogate gradient for a yes/no choice with logit `logit`
// (P(yes) = sigmoid(logit / T)), plus an entropy bonus. Returns dLoss/dlogit.
float BinaryPpo(float logit, bool chosen, float old_log_prob, float adv, float inv_t,
                const Config& cfg, HeadStats& st) {
    float z = std::clamp(logit * inv_t, -15.0F, 15.0F);
    float p = 1.0F / (1.0F + std::exp(-z));
    float new_lp = std::log(std::max(1e-12F, chosen ? p : 1.0F - p));
    float log_ratio = std::clamp(new_lp - old_log_prob, -10.0F, 10.0F);
    float ratio = std::exp(log_ratio);
    st.policy_loss += -std::min(ratio * adv,
                                std::clamp(ratio, 1.0F - cfg.clip_eps, 1.0F + cfg.clip_eps) * adv);
    st.kl += (ratio - 1.0F) - log_ratio;
    st.kl_samples++;
    bool clipped = (adv > 0.0F && ratio > 1.0F + cfg.clip_eps) ||
                   (adv < 0.0F && ratio < 1.0F - cfg.clip_eps);
    float d_loss_d_log_p = clipped ? 0.0F : -ratio * adv;
    float y = chosen ? 1.0F : 0.0F;
    // d(-H)/dz = p (1 - p) z for the Bernoulli entropy H
    return d_loss_d_log_p * (y - p) * inv_t + cfg.ent_coef * p * (1.0F - p) * z * inv_t;
}

struct Normalizers {
    Normalizer mission, econ, diplo;
};

// Accumulate the gradients of one turn's decisions and its critic target.
void AccumulateTurn(const ml::PolicyNetwork& net, const ml::TurnStepRecord& tr,
                    const Normalizers& nz, float inv_t, const Config& cfg, ml::PolicyNetwork& grad,
                    HeadStats& st, std::vector<float>& u, std::vector<float>& d_u) {
    // Critic: 0.5 (V - G)^2 on a linear head (search-trained turns have no return target)
    const bool has_critic_target = !std::isnan(tr.return_target);
    if (has_critic_target) {
        const ml::ValueInVec value_in = ml::PackValueInput(tr.start_ctx, tr.start_critic);
        float v = net.value_net.Forward(value_in)[0];
        float dv = v - tr.return_target;
        st.value_loss += 0.5 * dv * dv;
        st.turns++;
        net.value_net.Backward(value_in, {dv}, grad.value_net);
    }

    for (const auto& ms : tr.mission_steps) {
        if (has_critic_target) {
            float sv = net.EvaluateStackValue(ms.stack_value_in);
            float dsv = sv - ms.stack_return_target;
            st.value_loss += 0.25 * dsv * dsv;
            st.stack_values++;
            net.stack_value_net.Backward(ms.stack_value_in, {dsv}, grad.stack_value_net);
        }

        float adv = nz.mission(ms.advantage);
        const size_t candidate_count = ms.candidates.size();
        if (candidate_count > 1) {
            u.resize(candidate_count);
            for (size_t k = 0; k < candidate_count; k++)
                u[k] = net.mission_net.Forward(ms.candidates[k])[0];
            CategoricalPpo(u, &ms.log_mult, ms.chosen_idx, ms.old_log_prob, adv, inv_t, cfg, d_u,
                           st);
            for (size_t k = 0; k < candidate_count; k++) {
                if (d_u[k] != 0.0F)
                    net.mission_net.Backward(ms.candidates[k], {d_u[k], 0.0F}, grad.mission_net);
            }
            st.mission++;
        }
        if (ms.has_stage_choice && ms.chosen_idx < static_cast<int>(candidate_count)) {
            float s = net.mission_net.Forward(ms.candidates[ms.chosen_idx])[1];
            float d = BinaryPpo(s, ms.stage_chosen, ms.old_stage_log_prob, adv, inv_t, cfg, st);
            net.mission_net.Backward(ms.candidates[ms.chosen_idx], {0.0F, d}, grad.mission_net);
            st.mission++;
        }
    }

    float econ_adv = nz.econ(tr.advantage);
    for (const auto& es : tr.econ_steps) {
        const size_t candidate_count = es.candidates.size();
        if (candidate_count <= 1) continue;
        u.resize(candidate_count);
        for (size_t k = 0; k < candidate_count; k++)
            u[k] = net.econ_net.Forward(es.candidates[k])[0];
        CategoricalPpo(u, &es.log_mult, es.chosen_idx, es.old_log_prob, econ_adv, inv_t, cfg, d_u,
                       st);
        for (size_t k = 0; k < candidate_count; k++) {
            if (d_u[k] != 0.0F) net.econ_net.Backward(es.candidates[k], {d_u[k]}, grad.econ_net);
        }
        st.econ++;
    }

    float diplo_adv = nz.diplo(tr.advantage);
    for (const auto& ds : tr.diplo_steps) {
        auto raw = net.diplo_net.Forward(ds.input);
        std::array<float, ml::kDiploOutDim> d_out{};
        u = {raw[0], raw[1], raw[2], raw[3]};
        CategoricalPpo(u, nullptr, ds.chosen_stance, ds.old_stance_log_prob, diplo_adv, inv_t, cfg,
                       d_u, st);
        for (int k = 0; k < 4; k++) d_out[k] = d_u[k];
        st.diplo++;
        if (ds.has_tribute) {
            d_out[4] = BinaryPpo(raw[4], ds.tribute_chosen, ds.old_tribute_log_prob, diplo_adv,
                                 inv_t, cfg, st);
            st.diplo++;
        }
        if (ds.has_joint_war) {
            d_out[5] = BinaryPpo(raw[5], ds.joint_war_chosen, ds.old_joint_war_log_prob, diplo_adv,
                                 inv_t, cfg, st);
            st.diplo++;
        }
        net.diplo_net.Backward(ds.input, d_out, grad.diplo_net);
    }
}

// ---------------------------------------------------------------- evaluation

// One evaluation game: the scenario/preset/seed/seat assignment and its outcome.
struct EvalCase {
    std::string scenario;
    int preset = 0;
    uint32_t seed = 0;
    int swap = 0;
    double share = 0.5;  // Modern share of the cities held by both camps
    int top = 0;         // 1 when the largest side is a Modern side
    int modern = 0, classic = 0;
    int modern_sides = 0, sides = 0;
};

struct EvalResult {
    int games = 0;
    double mean_share = 0.0;  // Modern AI's share of the cities held by both camps
    double ci95 = 0.0;        // half-width of the 95% interval on meanShare
    int modern_top = 0;       // games whose largest side is a Modern AI side
    int modern_cities = 0, classic_cities = 0;
    double parity = 0.5;  // share an equally strong Modern side would get
    std::vector<EvalCase> cases;
};

// Solo evaluation (1 Modern vs K-1 Classic) is used when asked for, and during
// cropped-map curriculum selection where the training setup is solo.
bool UseSolo(const Config& cfg, bool is_selection) {
    if (cfg.eval_mode == EvalMode::kSolo) return true;
    return cfg.solo_learner && is_selection && (cfg.active_sides < 8 || cfg.crop_margin > 0);
}

std::vector<EvalCase> EvalCases(uint32_t seed_base, int seeds) {
    std::vector<EvalCase> cases;
    for (int i = 0; i < seeds; i++) {
        for (int swap = 0; swap < 2; swap++) {
            EvalCase c;
            c.scenario = kScenarios[(i + i / 4) % kScenarios.size()];
            c.preset = i % 4;
            c.seed = seed_base + static_cast<uint32_t>(i);
            c.swap = swap;
            cases.push_back(c);
        }
    }
    return cases;
}

void PlayEvalCase(const Config& cfg, const ml::PolicyNetwork& policy, EvalCase& c, bool solo,
                  bool allow_random, int turns) {
    auto gp = MakeArenaGame(cfg, c.scenario, kPresets[c.preset], c.seed, allow_random);
    Game& g = *gp;
    int active_count = 0;
    for (int s = 0; s < 8; s++) {
        if (g.map->sides[s].in_use) active_count++;
    }
    // Solo: the Modern seat rotates with the seed; the swapped game takes the seat
    // half-way round the table so both games of a seed differ in position.
    int solo_seat =
            active_count > 0
                    ? static_cast<int>((c.seed + static_cast<uint32_t>(
                                                         c.swap * std::max(1, active_count / 2))) %
                                       static_cast<uint32_t>(active_count))
                    : 0;
    std::array<bool, 8> modern{};
    int idx = 0;
    for (int s = 0; s < 8; s++) {
        if (!g.map->sides[s].in_use) continue;
        modern[s] = solo ? (idx == solo_seat) : ((idx + c.swap) % 2 == 0);
        // Opponent seats are Classic AI sides unless the Frozen Modern checkpoint plays them
        g.map->sides[s].classic_ai = !modern[s] && cfg.eval_opponent != opponents::Kind::kFrozen;
        if (modern[s]) c.modern_sides++;
        c.sides++;
        idx++;
    }
    Side* cur = game::Begin(g);
    int safety = 0;
    while (cur && !g.over && !g.won && g.turn <= turns && safety < turns * 12) {
        safety++;
        if (cur->computer && cur->alive) {
            if (modern[cur->index])
                ai::modern::PlayTurn(g, *cur, nullptr, &policy, nullptr);
            else
                opponents::PlayTurn(g, *cur, cfg.eval_opponent, g_frozen);
        }
        cur = game::EndTurn(g);
    }
    std::array<int, 8> cnt{};
    int m = 0, k = 0, best = kNone, best_cnt = -1;
    for (const auto& city : g.map->cities) {
        if (city.razed || city.owner_index < 0 || city.owner_index >= 8) continue;
        cnt[city.owner_index]++;
        (modern[city.owner_index] ? m : k)++;
    }
    for (int s = 0; s < 8; s++) {
        if (cnt[s] > best_cnt) {
            best_cnt = cnt[s];
            best = s;
        }
    }
    c.share = (m + k) > 0 ? static_cast<double>(m) / static_cast<double>(m + k) : 0.5;
    c.top = (best != kNone && modern[best]) ? 1 : 0;
    c.modern = m;
    c.classic = k;
}

// Plays `policies.size()` checkpoints on the same cases (same maps, seats and
// seeds), in parallel over all hardware threads.
std::vector<std::vector<EvalCase>> RunEval(const Config& cfg,
                                           const std::vector<const ml::PolicyNetwork*>& policies,
                                           uint32_t seed_base, int seeds, int turns) {
    bool is_selection = (seed_base == kSelectSeedBase);
    bool solo = UseSolo(cfg, is_selection);
    std::vector<std::vector<EvalCase>> out(policies.size(), EvalCases(seed_base, seeds));
    const size_t per_policy = out[0].size();
    const size_t total = per_policy * policies.size();
    std::atomic<size_t> next{0};
    unsigned int workers = std::max(2U, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned int w = 0; w < workers; w++) {
        pool.emplace_back([&]() {
            for (size_t i; (i = next.fetch_add(1)) < total;) {
                size_t p = i % policies.size(), ci = i / policies.size();
                PlayEvalCase(cfg, *policies[p], out[p][ci], solo, is_selection, turns);
            }
        });
    }
    for (auto& t : pool) t.join();
    return out;
}

EvalResult Summarize(std::vector<EvalCase> cases) {
    EvalResult r;
    r.games = static_cast<int>(cases.size());
    double parity = 0.0;
    for (const auto& c : cases) {
        r.mean_share += c.share;
        r.modern_top += c.top;
        r.modern_cities += c.modern;
        r.classic_cities += c.classic;
        parity += c.sides > 0 ? static_cast<double>(c.modern_sides) / static_cast<double>(c.sides)
                              : 0.5;
    }
    r.mean_share /= std::max(1, r.games);
    r.parity = parity / std::max(1, r.games);
    double var = 0.0;
    for (const auto& c : cases) var += (c.share - r.mean_share) * (c.share - r.mean_share);
    var /= std::max(1, r.games - 1);
    r.ci95 = 1.96 * std::sqrt(var / std::max(1, r.games));
    r.cases = std::move(cases);
    return r;
}

EvalResult EvaluateVsClassic(const Config& cfg, const ml::PolicyNetwork& policy, uint32_t seed_base,
                             int seeds, int turns) {
    return Summarize(std::move(RunEval(cfg, {&policy}, seed_base, seeds, turns)[0]));
}

void PrintEval(const char* label, const EvalResult& r) {
    std::printf(
            "  [%s] %d games: Modern city share %.1f%% +/- %.1f%% (95%%, parity %.1f%%) | cities "
            "%d vs %d | "
            "Modern largest side in %d/%d\n",
            label, r.games, r.mean_share * 100.0, r.ci95 * 100.0, r.parity * 100.0, r.modern_cities,
            r.classic_cities, r.modern_top, r.games);
    std::fflush(stdout);
}

// Mean share per preset and per scenario.
void PrintBreakdown(const EvalResult& r) {
    auto line = [&](const char* kind, const std::string& key, auto match) {
        double s = 0.0;
        int n = 0, m = 0, k = 0;
        for (const auto& c : r.cases) {
            if (!match(c)) continue;
            s += c.share;
            n++;
            m += c.modern;
            k += c.classic;
        }
        if (n > 0)
            std::printf("    %-8s %-14s %4d games  share %5.1f%%  cities %5d vs %5d\n", kind,
                        key.c_str(), n, 100.0 * s / n, m, k);
    };
    for (int p = 0; p < 4; p++)
        line("preset", kPresets[p].label, [&](const EvalCase& c) { return c.preset == p; });
    for (const auto& sc : kScenarios)
        line("scenario", sc, [&](const EvalCase& c) { return c.scenario == sc; });
    std::fflush(stdout);
}

// Paired comparison: the same cases played by A and B; the mean per-game share
// difference has a much tighter interval than two independent means.
void PrintPaired(const char* name_a, const EvalResult& a, const char* name_b, const EvalResult& b) {
    const size_t n = std::min(a.cases.size(), b.cases.size());
    std::vector<double> d(n);
    double mean = 0.0;
    int wins = 0, losses = 0;
    for (size_t i = 0; i < n; i++) {
        d[i] = b.cases[i].share - a.cases[i].share;
        mean += d[i];
        wins += d[i] > 1e-9;
        losses += d[i] < -1e-9;
    }
    mean /= std::max<size_t>(1, n);
    double var = 0.0;
    for (double x : d) var += (x - mean) * (x - mean);
    var /= std::max<size_t>(1, n - 1);
    double ci = 1.96 * std::sqrt(var / std::max<size_t>(1, n));
    std::printf(
            "  [paired] B - A = %+.2f%% +/- %.2f%% (95%%) over %zu games | B better in %d, worse "
            "in %d | %s\n",
            mean * 100.0, ci * 100.0, n, wins, losses,
            (mean - ci > 0)   ? "B significantly better"
            : (mean + ci < 0) ? "B significantly worse"
                              : "no significant difference");
    std::printf("    A = %s\n    B = %s\n", name_a, name_b);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- main

bool ParseArgs(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--iterations")
            cfg.iterations = std::max(0, std::stoi(next()));
        else if (a == "--games-per-iter")
            cfg.games_per_iter = std::max(2, std::stoi(next()));
        else if (a == "--turns")
            cfg.max_turns = std::max(6, std::stoi(next()));
        else if (a == "--eval-every")
            cfg.eval_every = std::max(1, std::stoi(next()));
        else if (a == "--eval-turns")
            cfg.eval_turns = std::max(6, std::stoi(next()));
        else if (a == "--select-seeds")
            cfg.select_seeds = std::max(1, std::stoi(next()));
        else if (a == "--report-seeds")
            cfg.report_seeds = std::max(0, std::stoi(next()));
        else if (a == "--epochs")
            cfg.epochs = std::max(1, std::stoi(next()));
        else if (a == "--minibatches")
            cfg.minibatches = std::max(1, std::stoi(next()));
        else if (a == "--lr")
            cfg.lr_start = std::stof(next());
        else if (a == "--temp-start")
            cfg.temp_start = std::max(0.1F, std::stof(next()));
        else if (a == "--temp-end")
            cfg.temp_end = std::max(0.1F, std::stof(next()));
        else if (a == "--eval-temp")
            cfg.eval_temp = std::max(0.01F, std::stof(next()));
        else if (a == "--seed-offset")
            cfg.seed_offset =
                    std::min<uint32_t>(500000U, static_cast<uint32_t>(std::stoul(next())));
        else if (a == "--classic-frac")
            cfg.classic_frac = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--random-map-frac")
            cfg.random_map_frac = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--active-sides")
            cfg.active_sides = std::clamp(std::stoi(next()), 2, 8);
        else if (a == "--crop-margin")
            cfg.crop_margin = std::max(0, std::stoi(next()));
        else if (a == "--lambda")
            cfg.gae_lambda = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--local-weight")
            cfg.local_weight = std::max(0.0F, std::stof(next()));
        else if (a == "--out")
            cfg.out_path = next();
        else if (a == "--log")
            cfg.log_path = next();
        else if (a == "--data")
            cfg.data_dir = next();
        else if (a == "--solo-learner")
            cfg.solo_learner = true;
        else if (a == "--from-scratch")
            cfg.from_scratch = true;
        else if (a == "--benchmark-only")
            cfg.benchmark_only = true;
        else if (a == "--eval-mode") {
            std::string m = next();
            if (m == "solo")
                cfg.eval_mode = EvalMode::kSolo;
            else if (m == "mixed")
                cfg.eval_mode = EvalMode::kMixed;
            else {
                std::fprintf(stderr,
                             "Unknown --eval-mode '%s'; expected 'mixed' (4 vs 4) or 'solo' (1 vs "
                             "K-1).\n",
                             m.c_str());
                return false;
            }
        } else if (a == "--compare")
            cfg.compare_path = next();
        else if (a == "--dev-seeds")
            cfg.dev_seeds = std::max(0, std::stoi(next()));
        else if (a == "--breakdown")
            cfg.breakdown = true;
        else if (a == "--critic-warmup")
            cfg.critic_warmup = std::max(0, std::stoi(next()));
        else if (a == "--mixed-frac")
            cfg.mixed_frac = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--save-every")
            cfg.save_every = std::max(0, std::stoi(next()));
        else if (a == "--raider-frac")
            cfg.raider_frac = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--departure-weight")
            cfg.departure_weight = std::max(0.0F, std::stof(next()));
        else if (a == "--frozen-opponent")
            cfg.frozen_path = next();
        else if (a == "--frozen-frac")
            cfg.frozen_frac = std::clamp(std::stof(next()), 0.0F, 1.0F);
        else if (a == "--search-k")
            cfg.search_k = std::max(0, std::stoi(next()));
        else if (a == "--search-r")
            cfg.search_r = std::max(1, std::stoi(next()));
        else if (a == "--search-h")
            cfg.search_h = std::max(1, std::stoi(next()));
        else if (a == "--eval-opponent") {
            std::string o = next();
            if (o == "raider")
                cfg.eval_opponent = opponents::Kind::kRaider;
            else if (o == "frozen")
                cfg.eval_opponent = opponents::Kind::kFrozen;
            else if (o == "classic")
                cfg.eval_opponent = opponents::Kind::kClassic;
            else {
                std::fprintf(
                        stderr,
                        "Unknown --eval-opponent '%s'; expected 'classic', 'raider' or 'frozen'.\n",
                        o.c_str());
                return false;
            }
        }

        else {
            std::fprintf(stderr,
                         "Unknown argument '%s'; see the usage at the top of cpp/test/arena.cpp.\n",
                         a.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!ParseArgs(argc, argv, cfg)) return 2;
    if (!FileExists(cfg.data_dir + "/TERRAIN0/ARMYTYPE.DAT")) {
        std::fprintf(stderr,
                     "Cannot find the game data in '%s': run from the repository root or pass "
                     "--data DIR.\n",
                     cfg.data_dir.c_str());
        return 1;
    }
    unsigned int threads = std::max(2U, std::thread::hardware_concurrency());
    ml::SetDeployTemperature(cfg.eval_temp);
    ml::PolicyNetwork frozen;
    if (!cfg.frozen_path.empty()) {
        if (!frozen.LoadJson(cfg.frozen_path)) {
            std::fprintf(stderr,
                         "Cannot load --frozen-opponent '%s'; check the path and JSON format.\n",
                         cfg.frozen_path.c_str());
            return 1;
        }
        g_frozen = &frozen;
    }
    if (cfg.eval_opponent == opponents::Kind::kFrozen && !g_frozen) {
        std::fprintf(stderr, "--eval-opponent frozen needs --frozen-opponent PATH.\n");
        return 2;
    }

    ml::PolicyNetwork learner;
    bool loaded = false;
    if (!cfg.from_scratch && std::filesystem::exists(cfg.out_path))
        loaded = learner.LoadJson(cfg.out_path);
    if (!loaded) learner.InitRandomWeights(2026);
    std::printf("=== w2arena: PPO self-play | params %d | threads %u | %s ===\n",
                ml::PolicyNetwork::kTotalParams, threads,
                loaded ? ("loaded " + cfg.out_path).c_str() : "random init");

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
                    cfg.out_path.c_str());
            return 1;
        }
        if (cfg.compare_path.empty()) {
            EvalResult r =
                    Summarize(std::move(RunEval(cfg, {&learner}, base, seeds, cfg.eval_turns)[0]));
            PrintEval(label, r);
            if (cfg.breakdown) PrintBreakdown(r);
            return 0;
        }
        ml::PolicyNetwork other;
        if (!other.LoadJson(cfg.compare_path)) {
            std::fprintf(stderr,
                         "Cannot load --compare checkpoint '%s'; check the path and JSON format.\n",
                         cfg.compare_path.c_str());
            return 1;
        }
        auto both = RunEval(cfg, {&learner, &other}, base, seeds, cfg.eval_turns);
        EvalResult ra = Summarize(std::move(both[0])), rb = Summarize(std::move(both[1]));
        PrintEval((std::string(label) + " A").c_str(), ra);
        if (cfg.breakdown) PrintBreakdown(ra);
        PrintEval((std::string(label) + " B").c_str(), rb);
        if (cfg.breakdown) PrintBreakdown(rb);
        PrintPaired(cfg.out_path.c_str(), ra, cfg.compare_path.c_str(), rb);
        return 0;
    }
    std::string mode_label = cfg.solo_learner ? "solo-learner (1 vs K-1 Classic)"
                                              : Fmt("classic-frac %.2f", cfg.classic_frac);
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

    const size_t param_count = ml::PolicyNetwork::kTotalParams;
    const size_t off_diplo = 0;
    const size_t off_mission = off_diplo + ml::PolicyNetwork::DiploNet::kParamCount;
    const size_t off_econ = off_mission + ml::PolicyNetwork::MissionNet::kParamCount;
    const size_t off_value = off_econ + ml::PolicyNetwork::EconNet::kParamCount;
    const size_t off_stack_value = off_value + ml::PolicyNetwork::ValueNet::kParamCount;
    std::vector<float> params = learner.Flatten(), adam_m(param_count, 0.0F),
                       adam_v(param_count, 0.0F);
    int adam_step = 0;
    std::mt19937 shuffle_rng(12345);

    for (int iter = 1; iter <= cfg.iterations; iter++) {
        auto t0 = std::chrono::steady_clock::now();
        float progress =
                static_cast<float>(iter - 1) / static_cast<float>(std::max(1, cfg.iterations - 1));
        float temperature = cfg.temp_start + (cfg.temp_end - cfg.temp_start) * progress;
        float lr = cfg.lr_start + (cfg.lr_end - cfg.lr_start) * progress;
        float inv_t = 1.0F / temperature;

        // ---- rollouts
        std::vector<RolloutSummary> games(cfg.games_per_iter);
        std::atomic<int> next_game{0};
        std::vector<std::thread> pool;
        for (unsigned int w = 0; w < std::min<unsigned int>(threads, cfg.games_per_iter); w++) {
            pool.emplace_back([&]() {
                for (int gi; (gi = next_game.fetch_add(1)) < cfg.games_per_iter;) {
                    uint32_t seed =
                            cfg.seed_offset + static_cast<uint32_t>(iter * 1009 + gi * 97 + 17);
                    const std::string& scen = kScenarios[(iter * 7 + gi) % kScenarios.size()];
                    const TrainingPreset& preset = kPresets[(iter + gi) % 4];
                    float roll = static_cast<float>((seed * 2654435761U) >> 8) /
                                 static_cast<float>(1U << 24);
                    Opponent mode = roll < cfg.classic_frac ? Opponent::kClassic
                                    : (gi % 2 == 0)         ? Opponent::kMirror
                                                            : Opponent::kLeague;
                    const auto& snap = hall_of_fame[(iter + gi) % hall_of_fame.size()];
                    games[gi] = cfg.search_k > 0 ? RunSearchRollout(cfg, scen, preset, seed,
                                                                    temperature, learner)
                                                 : RunRollout(cfg, scen, preset, seed, temperature,
                                                              learner, snap, mode);
                }
            });
        }
        for (auto& t : pool) t.join();

        std::vector<SideEpisode> episodes;
        int cities = 0, sides = 0, neutrals = 0;
        for (auto& gs : games) {
            cities += gs.recorded_cities;
            sides += gs.recorded_sides;
            neutrals += gs.neutrals_left;
            for (auto& ep : gs.episodes) episodes.push_back(std::move(ep));
        }
        ComputeAdvantages(episodes, cfg.gamma, cfg.gae_lambda, cfg.local_weight,
                          cfg.departure_weight);

        std::vector<const ml::TurnStepRecord*> turns;
        std::vector<float> m_adv, e_adv, d_adv;
        for (const auto& ep : episodes) {
            for (const auto& tr : ep.turns) {
                turns.push_back(&tr);
                for (const auto& ms : tr.mission_steps) m_adv.push_back(ms.advantage);
                if (!tr.econ_steps.empty()) e_adv.push_back(tr.advantage);
                if (!tr.diplo_steps.empty()) d_adv.push_back(tr.advantage);
            }
        }
        Normalizers nz{Normalizer::Fit(m_adv), Normalizer::Fit(e_adv), Normalizer::Fit(d_adv)};

        // Explained variance of the critics' rollout-time predictions:
        // 1 - Var(target - prediction) / Var(target); 1 is perfect, <= 0 is useless.
        auto explained_variance = [](const std::vector<float>& target,
                                     const std::vector<float>& pred) {
            if (target.size() < 2) return 0.0;
            double mt = 0.0, mr = 0.0;
            for (size_t i = 0; i < target.size(); i++) {
                mt += target[i];
                mr += target[i] - pred[i];
            }
            mt /= target.size();
            mr /= target.size();
            double vt = 0.0, vr = 0.0;
            for (size_t i = 0; i < target.size(); i++) {
                vt += (target[i] - mt) * (target[i] - mt);
                double r = target[i] - pred[i] - mr;
                vr += r * r;
            }
            return vt > 1e-12 ? 1.0 - vr / vt : 0.0;
        };
        std::vector<float> v_target, v_pred, sv_target, sv_pred;
        for (const auto* tr : turns) {
            if (std::isnan(tr->return_target)) continue;
            v_target.push_back(tr->return_target);
            v_pred.push_back(tr->value_pred);
            for (const auto& ms : tr->mission_steps) {
                sv_target.push_back(ms.stack_return_target);
                sv_pred.push_back(ms.stack_value_pred);
            }
        }
        const double ev_value = explained_variance(v_target, v_pred),
                     ev_stack = explained_variance(sv_target, sv_pred);
        // How often a move empties an own city, and how often that city then falls
        long vacated = 0, vacated_lost = 0, mission_moves = 0;
        for (const auto* tr : turns) {
            for (const auto& ms : tr->mission_steps) {
                mission_moves++;
                if (ms.vacated_city == kNone) continue;
                vacated++;
                if (ms.vacated_loss > 0.0F) vacated_lost++;
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
                unsigned int gw = std::min<unsigned int>(
                        threads, static_cast<unsigned int>(std::max<size_t>(1, mb1 - mb0)));
                std::vector<ml::PolicyNetwork> grads(gw);
                std::vector<HeadStats> stats(gw);
                std::vector<std::thread> gpool;
                for (unsigned int w = 0; w < gw; w++) {
                    grads[w].Zero();
                    gpool.emplace_back([&, w]() {
                        std::vector<float> u, d_u;
                        for (size_t ti; (ti = next_turn.fetch_add(1)) < mb1;) {
                            AccumulateTurn(learner, *turns[ti], nz, inv_t, cfg, grads[w], stats[w],
                                           u, d_u);
                        }
                    });
                }
                for (auto& t : gpool) t.join();

                HeadStats mb_stats;
                std::vector<float> g(param_count, 0.0F);
                for (unsigned int w = 0; w < gw; w++) {
                    auto fg = grads[w].Flatten();
                    for (size_t d = 0; d < param_count; d++) g[d] += fg[d];
                    mb_stats.Add(stats[w]);
                }
                // Each head's loss is a mean over its own decisions; the critic's over turns
                auto scale = [&](size_t a, size_t b, float s) {
                    for (size_t d = a; d < b; d++) g[d] *= s;
                };
                scale(off_diplo, off_mission, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.diplo));
                scale(off_mission, off_econ, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.mission));
                scale(off_econ, off_value, warmup ? 0.0F : 1.0F / std::max(1, mb_stats.econ));
                scale(off_value, off_stack_value, cfg.value_coef / std::max(1, mb_stats.turns));
                scale(off_stack_value, param_count,
                      cfg.value_coef / std::max(1, mb_stats.stack_values));
                double norm = 0.0;
                for (float x : g) norm += static_cast<double>(x) * x;
                float clip = std::sqrt(norm) > cfg.max_grad_norm
                                     ? cfg.max_grad_norm / static_cast<float>(std::sqrt(norm))
                                     : 1.0F;
                adam_step++;
                float bc1 = 1.0F - std::pow(0.9F, static_cast<float>(adam_step)),
                      bc2 = 1.0F - std::pow(0.999F, static_cast<float>(adam_step));
                for (size_t d = 0; d < param_count; d++) {
                    float gd = g[d] * clip;
                    adam_m[d] = 0.9F * adam_m[d] + 0.1F * gd;
                    adam_v[d] = 0.999F * adam_v[d] + 0.001F * gd * gd;
                    params[d] -= lr * (adam_m[d] / bc1) / (std::sqrt(adam_v[d] / bc2) + 1e-8F);
                }
                learner.Unflatten(params);
                epoch_stats.Add(mb_stats);
            }
            last_epoch = epoch_stats;
            epochs_run++;
            double kl = epoch_stats.kl / std::max(1, epoch_stats.kl_samples);
            if (kl > cfg.target_kl)
                break;  // the policy has moved far enough from the one that played
        }

        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double ent = last_epoch.entropy / std::max(1, last_epoch.ent_samples);
        double vloss = last_epoch.value_loss / std::max(1, last_epoch.turns);
        double kl = last_epoch.kl / std::max(1, last_epoch.kl_samples);
        float avg_cities = static_cast<float>(cities) / static_cast<float>(std::max(1, sides));
        float avg_neutrals = static_cast<float>(neutrals) / static_cast<float>(cfg.games_per_iter);
        size_t decisions = m_adv.size() + e_adv.size() + d_adv.size();
        std::printf(
                "[iter %3d] %.1fs T=%.2f lr=%.1e | cities/side %.2f | neutrals %.1f | ent %.3f | "
                "vloss %.4f | "
                "ev %.2f/%.2f | kl %.4f (%d ep) | %zu turns | emptied %.1f%% of moves, %.0f%% "
                "fell%s\n",
                iter, sec, temperature, lr, avg_cities, avg_neutrals, ent, vloss, ev_value,
                ev_stack, kl, epochs_run, turns.size(),
                100.0 * vacated / std::max(1L, mission_moves),
                100.0 * vacated_lost / std::max(1L, vacated), warmup ? " | critic warm-up" : "");
        std::fflush(stdout);

        if (cfg.save_every > 0 && iter % cfg.save_every == 0) {
            std::filesystem::path snap = cfg.out_path;
            snap.replace_extension(Fmt(".iter%d.json", iter));
            learner.SaveJson(snap, iter, 0.0F);
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
                best.SaveJson(cfg.out_path, best_iter, best_share);
            }
        }
        if (logf) {
            std::fprintf(logf,
                         "%d,%.2f,%.3f,%.2e,%.3f,%.2f,%.4f,%.5f,%.5f,%d,%zu,%.4f,%.4f,%.4f,%.4f\n",
                         iter, sec, temperature, lr, avg_cities, avg_neutrals, ent, vloss, kl,
                         epochs_run, decisions, sel_share, sel_ci, ev_value, ev_stack);
            std::fflush(logf);
        }
    }
    if (logf) std::fclose(logf);

    best.SaveJson(cfg.out_path, best_iter, best_share);
    {
        std::filesystem::path fin = cfg.out_path;
        fin.replace_extension(".final.json");
        learner.SaveJson(fin, cfg.iterations, 0.0F);
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
