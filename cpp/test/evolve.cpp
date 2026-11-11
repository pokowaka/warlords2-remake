// Paired-seed Antithetic Evolution Strategies trainer (`w2evolve`).
//
// Optimizes the PolicyNetwork's actor heads (DiploNet, MissionNet, EconNet,
// Commander, GateNet, MusterNet, or subsets via --heads) directly on full-game
// outcomes at deployment temperature (T = 0.02), bypassing per-move PPO credit
// assignment and value-critic bias.
//
// Each generation samples K Gaussian directions eps_k ~ N(0, I) (normalized to
// sqrt(active_dim)) over the active parameter block. For each direction and
// paired match, both theta + sigma * eps_k and theta - sigma * eps_k play the
// exact same scenario, preset, seat assignment, and initial RNG seed
// (optionally forked from a shared mid-game state at --fork-turn via
// save::Encode/Decode so post-expansion consolidation is trained directly).
//
// Within-pair differences Delta_k = mean_m(F^+_{k,m} - F^-_{k,m}) cancel map,
// preset, and starting-seat luck. Centered magnitude-rank shaping maps Delta_k
// to unit-RMS weights w_k, and Adam updates theta along (1/K) sum_k w_k eps_k.
//
// Seed hygiene: training seeds stay strictly in [1, 680000]; selection uses
// 700000+ (kSelectSeedBase); development (750000+) and held-out report
// (800000+) are never touched here.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"

#include "test/opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/model/army.hpp"
#include "warlords/model/city.hpp"
#include "warlords/model/map.hpp"
#include "warlords/model/side.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/save.hpp"
#include "warlords/types.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, in, "cpp/ai_weights.json", "Input checkpoint JSON path.");
ABSL_FLAG(std::string, out, "runs/evolve/weights.json", "Output best checkpoint JSON path.");
ABSL_FLAG(std::string, log, "", "Optional CSV training log output path.");
ABSL_FLAG(std::string, heads, "actor",
          "Parameter slice to evolve: actor, mission, econ, diplo, mission+econ, muster, "
          "muster+commander, standard+mission, gate, commander, v7-focus, readout, "
          "mission-readout, archetype, or actor-readout.");
ABSL_FLAG(int, generations, 60, "Number of evolution generations.");
ABSL_FLAG(int, dirs, 72, "Number of antithetic perturbation directions per generation.");
ABSL_FLAG(int, seeds_per_dir, 2, "Paired games per perturbation direction.");
ABSL_FLAG(int, turns, 50, "Maximum turns per training game.");
ABSL_FLAG(int, fork_turn, 22, "Center turn for cached mid-game state forks.");
ABSL_FLAG(double, fork_frac, 0.0, "Fraction of directions forked from cached mid-game states.");
ABSL_FLAG(double, sigma, -1.0, "Fixed or initial ES noise scale (negative keeps default 0.03).");
ABSL_FLAG(double, sigma_end, -1.0, "Final ES noise scale (negative keeps --sigma or 0.02).");
ABSL_FLAG(double, lr, -1.0, "Fixed or initial Adam learning rate (negative keeps default 0.006).");
ABSL_FLAG(double, lr_end, -1.0, "Final Adam learning rate (negative keeps --lr or 0.002).");
ABSL_FLAG(double, l2, 1.0e-4, "L2 weight decay toward zero.");
ABSL_FLAG(double, anchor_l2, 0.0, "L2 weight decay toward the initial checkpoint.");
ABSL_FLAG(double, eval_temp, -1.0,
          "Deployment softmax temperature during ES playouts (negative keeps default).");
ABSL_FLAG(double, warden_frac, 0.25, "Probability an opponent seat uses Warden.");
ABSL_FLAG(double, raider_frac, 0.10, "Probability an opponent seat uses Raider.");
ABSL_FLAG(double, warband_frac, 0.0, "Probability an opponent seat uses fair-fog Warband.");
ABSL_FLAG(bool, fair_classic, false, "Mask hidden-map fog omniscience on Classic opponents.");
ABSL_FLAG(double, totalwar_frac, 0.50, "Share of training games played on Total War presets.");
ABSL_FLAG(std::string, frozen_opponent, "",
          "Optional checkpoint path for Frozen league opponents.");
ABSL_FLAG(double, frozen_frac, 0.0, "Probability an opponent seat uses Frozen.");
ABSL_FLAG(double, random_map_frac, 0.10, "Fraction of full games played on random maps.");
ABSL_FLAG(int, active_sides, 8, "Number of active scenario sides (2..8).");
ABSL_FLAG(int, crop_margin, 0, "Map crop margin around active capitals (0 disables cropping).");
ABSL_FLAG(bool, solo, false, "Train a single candidate seat against 7 opponents.");
ABSL_FLAG(bool, from_scratch, false, "Initialize random weights instead of loading --in.");
ABSL_FLAG(bool, upgrade_v7, false, "Upgrade loaded checkpoint to v7 before training.");
ABSL_FLAG(bool, upgrade_v8, false, "Upgrade loaded checkpoint to v8 before training.");
ABSL_FLAG(bool, use_commander, false, "Enable learned Commander posture channels.");
ABSL_FLAG(bool, use_muster, false, "Enable learned Muster Hub selection (MusterNet).");
ABSL_FLAG(bool, use_standard, false, "Enable learned Side Standard planting/vectoring.");
ABSL_FLAG(bool, avoid_blockers, false, "Enable obstacle-aware detour scoring.");
ABSL_FLAG(double, detour_bias, -999.0,
          "Initial detour utility bias (setting > -900 enables --avoid_blockers).");
ABSL_FLAG(std::string, archetype, "imperial",
          "Fitness archetype: imperial, berserker, guardian, schemer, or questor.");
ABSL_FLAG(bool, qd_doctrine, false,
          "Enable CharacterDoctrine on candidate seats during Quality-Diversity evolution.");
ABSL_FLAG(std::string, init_moe, "",
          "Comma-separated expert checkpoint paths to initialize MoE experts.");
ABSL_FLAG(int, eval_every, 10, "Run selection evaluation every N generations.");
ABSL_FLAG(int, eval_turns, 45, "Maximum turns per selection evaluation game.");
ABSL_FLAG(int, select_seeds, 32, "Number of paired selection seeds (plays 2x games).");
ABSL_FLAG(int, save_every, 20, "Save periodic snapshots every N generations (0 disables).");
ABSL_FLAG(uint32_t, seed_offset, 10000U, "Offset added to training seed schedule.");
ABSL_FLAG(int, threads, 0, "Worker thread count (0 = hardware_concurrency).");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

struct Preset {
    const char* label = "";
    int hidden_map = 0;
    int diplomacy = 0;
    int neutral_cities = 0;
};

constexpr std::array<Preset, 4> kPresets{{
    {"Beginner-Open", 0, 0, 0},
    {"Interm-Diplo", 0, 1, 1},
    {"Adv-Hidden", 1, 1, 2},
    {"Fog-TotalWar", 1, 0, 1},
}};

constexpr std::array<std::string_view, 6> kScenarios{"ERYTHEA", "ISLADIA", "DRAGON",
                                                     "TUTORIA", "SORCERY", "HADESHA"};

constexpr uint32_t kTrainSeedMod = 680000U;
constexpr uint32_t kSelectSeedBase = 700000U;

struct Config {
    std::string data_dir = "original";
    std::string in_path = "cpp/ai_weights.json";
    std::string out_path = "runs/evolve/weights.json";
    std::string log_path;
    std::string heads = "actor";
    int generations = 60;
    int dirs = 72;
    int seeds_per_dir = 2;
    int max_turns = 50;
    int fork_turn = 22;
    float fork_frac = 0.0F;
    float sigma_start = 0.03F;
    float sigma_end = 0.02F;
    float lr_start = 0.006F;
    float lr_end = 0.002F;
    float l2 = 1.0e-4F;
    float anchor_l2 = 0.0F;
    float eval_temp = 0.02F;
    float warden_frac = 0.25F;
    float raider_frac = 0.10F;
    float warband_frac = 0.0F;
    bool fair_classic = false;
    float total_war_frac = 0.50F;
    std::string frozen_path;
    float frozen_frac = 0.0F;
    float random_map_frac = 0.10F;
    int active_sides = 8;
    int crop_margin = 0;
    bool solo = false;
    bool from_scratch = false;
    bool upgrade_v7 = false;
    bool upgrade_v8 = false;
    bool use_commander = false;
    bool use_muster = false;
    bool use_standard = false;
    bool avoid_blockers = false;
    float init_detour_bias = -0.05F;
    std::string archetype = "imperial";
    bool qd_doctrine = false;
    std::string init_moe;
    int eval_every = 10;
    int eval_turns = 45;
    int select_seeds = 32;
    int save_every = 20;
    uint32_t seed_offset = 10000U;
};

Config LoadConfigFromFlags() {
    Config cfg;
    cfg.data_dir = absl::GetFlag(FLAGS_data);
    cfg.in_path = absl::GetFlag(FLAGS_in);
    cfg.out_path = absl::GetFlag(FLAGS_out);
    cfg.log_path = absl::GetFlag(FLAGS_log);
    cfg.heads = absl::GetFlag(FLAGS_heads);
    cfg.generations = std::max(0, absl::GetFlag(FLAGS_generations));
    cfg.dirs = std::max(2, absl::GetFlag(FLAGS_dirs));
    cfg.seeds_per_dir = std::max(1, absl::GetFlag(FLAGS_seeds_per_dir));
    cfg.max_turns = std::max(10, absl::GetFlag(FLAGS_turns));
    cfg.fork_turn = std::max(4, absl::GetFlag(FLAGS_fork_turn));
    cfg.fork_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_fork_frac)), 0.0F, 1.0F);
    if (absl::GetFlag(FLAGS_sigma) > 0.0) {
        cfg.sigma_start = cfg.sigma_end =
                std::max(1e-4F, static_cast<float>(absl::GetFlag(FLAGS_sigma)));
    }
    if (absl::GetFlag(FLAGS_sigma_end) > 0.0) {
        cfg.sigma_end = std::max(1e-4F, static_cast<float>(absl::GetFlag(FLAGS_sigma_end)));
    }
    if (absl::GetFlag(FLAGS_lr) > 0.0) {
        cfg.lr_start = cfg.lr_end = std::max(1e-5F, static_cast<float>(absl::GetFlag(FLAGS_lr)));
    }
    if (absl::GetFlag(FLAGS_lr_end) > 0.0) {
        cfg.lr_end = std::max(1e-5F, static_cast<float>(absl::GetFlag(FLAGS_lr_end)));
    }
    cfg.l2 = std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_l2)));
    cfg.anchor_l2 = std::max(0.0F, static_cast<float>(absl::GetFlag(FLAGS_anchor_l2)));
    cfg.eval_temp = absl::GetFlag(FLAGS_eval_temp) > 0.0
                            ? std::max(0.01F, static_cast<float>(absl::GetFlag(FLAGS_eval_temp)))
                            : ml::DeployTemperature();
    cfg.warden_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_warden_frac)), 0.0F, 1.0F);
    cfg.raider_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_raider_frac)), 0.0F, 1.0F);
    cfg.warband_frac =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_warband_frac)), 0.0F, 1.0F);
    cfg.fair_classic = absl::GetFlag(FLAGS_fair_classic);
    cfg.total_war_frac =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_totalwar_frac)), 0.0F, 1.0F);
    cfg.frozen_path = absl::GetFlag(FLAGS_frozen_opponent);
    cfg.frozen_frac = std::clamp(static_cast<float>(absl::GetFlag(FLAGS_frozen_frac)), 0.0F, 1.0F);
    cfg.random_map_frac =
            std::clamp(static_cast<float>(absl::GetFlag(FLAGS_random_map_frac)), 0.0F, 1.0F);
    cfg.active_sides = std::clamp(absl::GetFlag(FLAGS_active_sides), 2, 8);
    cfg.crop_margin = std::max(0, absl::GetFlag(FLAGS_crop_margin));
    cfg.solo = absl::GetFlag(FLAGS_solo);
    cfg.from_scratch = absl::GetFlag(FLAGS_from_scratch);
    cfg.upgrade_v7 = absl::GetFlag(FLAGS_upgrade_v7);
    cfg.upgrade_v8 = absl::GetFlag(FLAGS_upgrade_v8);
    cfg.use_commander = absl::GetFlag(FLAGS_use_commander);
    cfg.use_muster = absl::GetFlag(FLAGS_use_muster);
    cfg.use_standard = absl::GetFlag(FLAGS_use_standard);
    cfg.avoid_blockers = absl::GetFlag(FLAGS_avoid_blockers);
    double raw_detour = absl::GetFlag(FLAGS_detour_bias);
    if (raw_detour > -900.0) {
        cfg.avoid_blockers = true;
        cfg.init_detour_bias = static_cast<float>(raw_detour);
    }
    cfg.archetype = absl::GetFlag(FLAGS_archetype);
    cfg.qd_doctrine = absl::GetFlag(FLAGS_qd_doctrine);
    cfg.init_moe = absl::GetFlag(FLAGS_init_moe);
    cfg.eval_every = std::max(1, absl::GetFlag(FLAGS_eval_every));
    cfg.eval_turns = std::max(10, absl::GetFlag(FLAGS_eval_turns));
    cfg.select_seeds = std::max(4, absl::GetFlag(FLAGS_select_seeds));
    cfg.save_every = std::max(0, absl::GetFlag(FLAGS_save_every));
    cfg.seed_offset = absl::GetFlag(FLAGS_seed_offset);
    return cfg;
}

const Preset& PickTrainingPreset(const Config& cfg, uint32_t key) {
    float roll = static_cast<float>(((key * 2246822519U) ^ 0x85ebca6bU) >> 8) /
                 static_cast<float>(1U << 24);
    if (roll < cfg.total_war_frac) {
        return kPresets[(key & 1U) ? 3 : 0];
    }
    return kPresets[(key & 1U) ? 2 : 1];
}

template <typename Mlp>
void AddReadoutIndices(size_t base_off, std::vector<size_t>& idx) {
    constexpr size_t kSkipCount = static_cast<size_t>(Mlp::kOutDim) * Mlp::kInDim;
    constexpr size_t kTailCount = static_cast<size_t>(Mlp::kOutDim) * Mlp::kH2Dim + Mlp::kOutDim;
    for (size_t i = 0; i < kSkipCount; i++) idx.push_back(base_off + i);
    for (size_t i = Mlp::kParamCount - kTailCount; i < Mlp::kParamCount; i++) {
        idx.push_back(base_off + i);
    }
}

template <typename Mlp>
void AddW1ColumnIndices(size_t base_off, int col_begin, int col_end, std::vector<size_t>& idx) {
    const size_t w1_off = base_off + static_cast<size_t>(Mlp::kOutDim) * Mlp::kInDim;
    for (int row = 0; row < Mlp::kH1Dim; row++) {
        for (int col = col_begin; col < col_end && col < Mlp::kInDim; col++) {
            idx.push_back(w1_off + static_cast<size_t>(row * Mlp::kInDim + col));
        }
    }
}

template <typename Mlp>
void AddSkipAndW1ColumnIndices(size_t base_off, int col_begin, int col_end,
                               std::vector<size_t>& idx) {
    for (int row = 0; row < Mlp::kOutDim; row++) {
        for (int col = col_begin; col < col_end && col < Mlp::kInDim; col++) {
            idx.push_back(base_off + static_cast<size_t>(row * Mlp::kInDim + col));
        }
    }
    AddW1ColumnIndices<Mlp>(base_off, col_begin, col_end, idx);
}

constexpr size_t kCmdParamOff = ml::PolicyNetwork::kTotalParams;
constexpr size_t kCmdParamCount = 5 * ml::PolicyNetwork::kGateInDim + 5;
constexpr size_t kGateParamOff = kCmdParamOff + kCmdParamCount;
constexpr size_t kGateParamCount = ml::PolicyNetwork::GateNet::kParamCount;
constexpr size_t kMusterParamOff = kGateParamOff + kGateParamCount;
constexpr size_t kMusterParamCount = ml::PolicyNetwork::MusterNet::kParamCount;
constexpr size_t kDetourBiasOff = kMusterParamOff + kMusterParamCount;

std::vector<float> PackEvolveTheta(const ml::PolicyNetwork& net) {
    std::vector<float> out = net.Flatten();
    out.insert(out.end(), net.CommanderW().begin(), net.CommanderW().end());
    out.insert(out.end(), net.CommanderB().begin(), net.CommanderB().end());
    net.GateHead().ToFlat(out);
    net.MusterHead().ToFlat(out);
    out.push_back(net.DetourBias());
    return out;
}

void UnpackEvolveTheta(ml::PolicyNetwork& net, const std::vector<float>& vec) {
    net.Unflatten(vec);
    size_t off = kCmdParamOff;
    for (size_t i = 0; i < net.CommanderW().size() && off < vec.size(); i++) {
        net.CommanderW()[i] = vec[off++];
    }
    for (size_t i = 0; i < net.CommanderB().size() && off < vec.size(); i++) {
        net.CommanderB()[i] = vec[off++];
    }
    net.GateHead().FromFlat(vec, kGateParamOff);
    net.MusterHead().FromFlat(vec, kMusterParamOff);
    if (kDetourBiasOff < vec.size()) net.SetDetourBias(vec[kDetourBiasOff]);
}

std::vector<size_t> BuildActiveIndices(const std::string& heads) {
    using DiploNet = ml::PolicyNetwork::DiploNet;
    using MissionNet = ml::PolicyNetwork::MissionNet;
    using EconNet = ml::PolicyNetwork::EconNet;
    using MusterNet = ml::PolicyNetwork::MusterNet;

    constexpr size_t kOffDiplo = 0;
    constexpr size_t kOffMission = kOffDiplo + DiploNet::kParamCount;
    constexpr size_t kOffEcon = kOffMission + MissionNet::kParamCount;
    constexpr size_t kOffCritics = kOffEcon + EconNet::kParamCount;

    std::vector<size_t> idx;
    if (heads == "muster" || heads == "muster+commander" || heads == "standard+mission") {
        for (size_t i = 0; i < kMusterParamCount; i++) idx.push_back(kMusterParamOff + i);
        // Evolve ONLY the v8 muster columns while keeping prior weights and readouts frozen so
        // non-hub behaviour cannot drift.
        AddSkipAndW1ColumnIndices<MissionNet>(
                kOffMission, MissionNet::kInDim - ml::kAddedV8TargetDim, MissionNet::kInDim, idx);
        AddSkipAndW1ColumnIndices<EconNet>(kOffEcon, EconNet::kInDim - ml::kAddedV8EconDim,
                                           EconNet::kInDim, idx);
        if (heads == "standard+mission") {
            AddReadoutIndices<MissionNet>(kOffMission, idx);
            idx.push_back(kDetourBiasOff);
        }
        if (heads == "muster+commander") {
            for (size_t i = 0; i < kCmdParamCount; i++) idx.push_back(kCmdParamOff + i);
            AddSkipAndW1ColumnIndices<MissionNet>(kOffMission, ml::kGlobalDim, ml::kContextDim,
                                                  idx);
            AddSkipAndW1ColumnIndices<EconNet>(kOffEcon, ml::kGlobalDim, ml::kContextDim, idx);
        }
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        return idx;
    }
    if (heads == "gate") {
        for (size_t i = 0; i < kGateParamCount; i++) idx.push_back(kGateParamOff + i);
        return idx;
    }
    if (heads == "commander" || heads == "v7-focus") {
        if (heads == "commander") {
            for (size_t i = 0; i < kCmdParamCount; i++) idx.push_back(kCmdParamOff + i);
        }
        AddReadoutIndices<MissionNet>(kOffMission, idx);
        AddReadoutIndices<EconNet>(kOffEcon, idx);
        for (int row = 0; row < DiploNet::kOutDim; row++) {
            for (int col = ml::kGlobalDim; col < ml::kContextDim; col++) {
                idx.push_back(kOffDiplo + static_cast<size_t>(row * DiploNet::kInDim + col));
            }
        }
        AddW1ColumnIndices<DiploNet>(kOffDiplo, ml::kGlobalDim, ml::kContextDim, idx);
        AddW1ColumnIndices<MissionNet>(kOffMission, ml::kGlobalDim, ml::kContextDim, idx);
        AddW1ColumnIndices<MissionNet>(kOffMission, MissionNet::kInDim - ml::kAddedTargetDim,
                                       MissionNet::kInDim, idx);
        AddW1ColumnIndices<EconNet>(kOffEcon, ml::kGlobalDim, ml::kContextDim, idx);
        AddW1ColumnIndices<EconNet>(kOffEcon, EconNet::kInDim - ml::kAddedEconDim, EconNet::kInDim,
                                    idx);
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        return idx;
    }
    if (heads == "readout") {
        AddReadoutIndices<MissionNet>(kOffMission, idx);
        AddReadoutIndices<EconNet>(kOffEcon, idx);
        return idx;
    }
    if (heads == "mission-readout") {
        AddReadoutIndices<MissionNet>(kOffMission, idx);
        idx.push_back(kDetourBiasOff);
        return idx;
    }
    if (heads == "archetype") {
        AddReadoutIndices<DiploNet>(kOffDiplo, idx);
        AddReadoutIndices<MissionNet>(kOffMission, idx);
        AddReadoutIndices<EconNet>(kOffEcon, idx);
        AddReadoutIndices<MusterNet>(kMusterParamOff, idx);
        idx.push_back(kDetourBiasOff);
        return idx;
    }
    if (heads == "actor-readout") {
        AddReadoutIndices<DiploNet>(kOffDiplo, idx);
        AddReadoutIndices<MissionNet>(kOffMission, idx);
        AddReadoutIndices<EconNet>(kOffEcon, idx);
        return idx;
    }
    const bool use_diplo = (heads == "actor" || heads == "diplo");
    const bool use_mission = (heads == "actor" || heads == "mission" || heads == "mission+econ");
    const bool use_econ = (heads == "actor" || heads == "econ" || heads == "mission+econ");
    if (use_diplo) {
        for (size_t i = kOffDiplo; i < kOffMission; i++) idx.push_back(i);
    }
    if (use_mission) {
        for (size_t i = kOffMission; i < kOffEcon; i++) idx.push_back(i);
    }
    if (use_econ) {
        for (size_t i = kOffEcon; i < kOffCritics; i++) idx.push_back(i);
    }
    return idx;
}

game::NewGameOptions MakeOpts(const Config& cfg, const Preset& preset, uint32_t seed) {
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
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        opts.sides[side_idx].computer = true;
        opts.sides[side_idx].level = 2;
        opts.sides[side_idx].card =
                static_cast<int>((seed + static_cast<uint32_t>(side_idx * 5)) % 9U);
    }
    return opts;
}

std::unique_ptr<Game> CreateGame(const Config& cfg, const std::string& scenario,
                                 const Preset& preset, uint32_t seed, bool allow_random) {
    game::NewGameOptions opts = MakeOpts(cfg, preset, seed);
    float map_roll = static_cast<float>(((seed * 1597334677U) ^ 0x9e3779b9U) >> 8) /
                     static_cast<float>(1U << 24);
    if (allow_random && map_roll < cfg.random_map_frac) {
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options ropts;
        ropts.data_dir = cfg.data_dir;
        ropts.rng = &map_rng;
        ropts.sliders = {3, 3, 2, 3};
        ropts.allies = preset.diplomacy != 0;
        ropts.terrain_set = 0;
        auto files = randommap::GenerateNow(ropts);
        ABSL_CHECK_OK(files);
        opts.memory_files = &*files;
        auto loaded = game::NewGame(cfg.data_dir, "RANDOM", opts);
        ABSL_CHECK_OK(loaded);
        return std::move(*loaded);
    }
    auto loaded = game::NewGame(cfg.data_dir, scenario, opts);
    ABSL_CHECK_OK(loaded);
    return std::move(*loaded);
}

struct SeatPlan {
    std::array<bool, 8> is_candidate{};
    std::array<opponents::Kind, 8> opp_kind{};
};

SeatPlan AssignSeats(Game& game, const Config& cfg, uint32_t seed, int parity, bool has_frozen,
                     bool pure_classic_eval) {
    SeatPlan seat_plan;
    const Map& map = game.GetMap();
    int active_count = 0;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        if (map.Sides()[side_idx].InUse()) active_count++;
    }
    int solo_seat = active_count > 0 ? static_cast<int>((seed + static_cast<uint32_t>(parity)) %
                                                        static_cast<uint32_t>(active_count))
                                     : 0;
    int active_pos = 0;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        Side* side = game.GetMap().SideAt(side_idx);
        if (side == nullptr || !side->InUse()) continue;
        bool cand = cfg.solo && !pure_classic_eval ? (active_pos == solo_seat)
                                                   : ((active_pos + parity) % 2 == 0);
        active_pos++;
        seat_plan.is_candidate[side_idx] = cand;
        ai::SetSideClassicAi(game, *side, !cand);
        if (cand && cfg.qd_doctrine) {
            int rep_card = cfg.archetype == "berserker"  ? 1
                           : cfg.archetype == "guardian" ? 2
                           : cfg.archetype == "schemer"  ? 8
                           : cfg.archetype == "questor"  ? 7
                                                         : 0;
            side->SetLevel(2);
            side->SetCard(rep_card);
        }
        seat_plan.opp_kind[side_idx] =
                cfg.fair_classic ? opponents::Kind::kFairClassic : opponents::Kind::kClassic;
        if (!cand && !pure_classic_eval) {
            uint32_t salt1 = static_cast<uint32_t>((side_idx + 1) * 2891336453U);
            uint32_t salt2 = static_cast<uint32_t>((side_idx + 1) * 2654435761U);
            uint32_t salt3 = static_cast<uint32_t>((side_idx + 1) * 3266489917U);
            uint32_t salt4 = static_cast<uint32_t>((side_idx + 1) * 1597334677U);
            float roll1 = static_cast<float>(((seed * 747796405U) ^ salt1) >> 8) /
                          static_cast<float>(1U << 24);
            float roll2 = static_cast<float>(((seed * 1103515245U) ^ salt2) >> 8) /
                          static_cast<float>(1U << 24);
            float roll3 = static_cast<float>(((seed * 2654435769U) ^ salt3) >> 8) /
                          static_cast<float>(1U << 24);
            float roll4 = static_cast<float>(((seed * 2246822519U) ^ salt4) >> 8) /
                          static_cast<float>(1U << 24);
            if (roll4 < cfg.warband_frac) {
                seat_plan.opp_kind[side_idx] = opponents::Kind::kWarband;
            } else if (roll1 < cfg.raider_frac) {
                seat_plan.opp_kind[side_idx] = opponents::Kind::kRaider;
            } else if (roll2 < cfg.warden_frac) {
                seat_plan.opp_kind[side_idx] = opponents::Kind::kWarden;
            }
            if (has_frozen && roll3 < cfg.frozen_frac) {
                seat_plan.opp_kind[side_idx] = opponents::Kind::kFrozen;
                ai::SetSideClassicAi(game, *side, false);
            }
        }
    }
    return seat_plan;
}

float CampCityShare(const Game& game, const std::array<bool, 8>& is_candidate) {
    int my_cities = 0;
    int opp_cities = 0;
    for (const City& city : game.GetMap().Cities()) {
        int owner = city.OwnerIndex();
        if (city.IsRazed() || owner < 0 || owner >= 8) continue;
        if (is_candidate[owner])
            my_cities++;
        else
            opp_cities++;
    }
    return (my_cities + opp_cities) > 0
                   ? static_cast<float>(my_cities) / static_cast<float>(my_cities + opp_cities)
                   : 0.5F;
}

float CampArmyShare(const Game& game, const std::array<bool, 8>& is_candidate) {
    float my_power = 0.0F;
    float opp_power = 0.0F;
    for (const Army* army : game.Armies()) {
        int owner = army->OwnerIndex();
        if (army->IsDisbanded() || (army->X() == kNone && !army->InTransit()) || owner < 0 ||
            owner >= 8) {
            continue;
        }
        float power = static_cast<float>(army->Strength() * army->Strength());
        if (is_candidate[owner])
            my_power += power;
        else
            opp_power += power;
    }
    return (my_power + opp_power) > 0.0F ? my_power / (my_power + opp_power) : 0.5F;
}

void AdvanceGame(Game& game, Side*& cur, int stop_turn, const SeatPlan& seat_plan,
                 const ml::PolicyNetwork& cand_net, const ml::PolicyNetwork* frozen_net,
                 float* mid_share_out = nullptr, int mid_turn = -1) {
    int safety = 0;
    const int max_steps = (stop_turn + 4) * 12;
    while (cur != nullptr && !game.IsOver() && !game.IsWon() && game.Turn() <= stop_turn &&
           safety++ < max_steps) {
        if (mid_share_out != nullptr && mid_turn > 0 && game.Turn() == mid_turn &&
            *mid_share_out < 0.0F) {
            *mid_share_out = CampCityShare(game, seat_plan.is_candidate);
        }
        if (cur->IsComputerControlled() && cur->IsAlive()) {
            int side_idx = cur->Index();
            if (seat_plan.is_candidate[side_idx]) {
                ai::modern::PlayTurn(game, *cur, nullptr, &cand_net, nullptr);
            } else {
                opponents::PlayTurn(game, *cur, seat_plan.opp_kind[side_idx], frozen_net);
            }
        }
        cur = game::EndTurn(game);
    }
}

float ComputeFitness(const Game& game, const SeatPlan& seat_plan, float mid_share,
                     const std::string& archetype) {
    float end_city = CampCityShare(game, seat_plan.is_candidate);
    float end_army = CampArmyShare(game, seat_plan.is_candidate);
    float mid = mid_share >= 0.0F ? mid_share : end_city;
    if (archetype.empty() || archetype == "imperial") {
        return 0.75F * end_city + 0.15F * mid + 0.10F * end_army;
    }

    int razed_count = 0;
    int cand_cities = 0;
    int cand_gar1 = 0;
    int cand_gar2 = 0;
    for (const City& city : game.GetMap().Cities()) {
        if (city.IsRazed()) {
            razed_count++;
            continue;
        }
        int owner = city.OwnerIndex();
        if (owner < 0 || owner >= 8 || !seat_plan.is_candidate[owner]) continue;
        cand_cities++;
        int defenders = 0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                for (const Army* army :
                     game::ArmiesOnTileSpan(game, city.X() + dx, city.Y() + dy)) {
                    if (army->OwnerIndex() == owner) defenders++;
                }
            }
        }
        if (defenders >= 1) cand_gar1++;
        if (defenders >= 2) cand_gar2++;
    }
    float razed_score = std::tanh(static_cast<float>(razed_count) / 3.0F);
    float garrison_frac = cand_cities > 0 ? (0.60F * static_cast<float>(cand_gar1) +
                                             0.40F * static_cast<float>(cand_gar2)) /
                                                    static_cast<float>(cand_cities)
                                          : 0.0F;

    float my_gold = 0.0F;
    float opp_gold = 0.0F;
    float my_econ = 0.0F;
    float opp_econ = 0.0F;
    int pairs = 0;
    int war_pairs = 0;
    int peace_pairs = 0;
    int ally_pairs = 0;
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        const Side* side = game.GetMap().SideAt(side_idx);
        if (side == nullptr || !side->InUse() || !side->IsAlive()) continue;
        float gold = static_cast<float>(std::max(0, side->Gold()));
        float econ = gold + 20.0F * static_cast<float>(game::Income(game, *side));
        if (seat_plan.is_candidate[side_idx]) {
            my_gold += gold;
            my_econ += econ;
            for (int opp_idx = 0; opp_idx < 8; opp_idx++) {
                const Side* opp = game.GetMap().SideAt(opp_idx);
                if (opp_idx == side_idx || opp == nullptr || !opp->InUse() || !opp->IsAlive()) {
                    continue;
                }
                pairs++;
                if (diplomacy::State(game, side_idx, opp_idx) == diplomacy::kWar) war_pairs++;
                if (diplomacy::State(game, side_idx, opp_idx) == diplomacy::kPeace) peace_pairs++;
                if (diplomacy::IsAllied(game, side_idx, opp_idx)) ally_pairs++;
            }
        } else {
            opp_gold += gold;
            opp_econ += econ;
        }
    }
    float gold_share = (my_gold + opp_gold) > 0.0F ? my_gold / (my_gold + opp_gold) : 0.5F;
    float econ_share = (my_econ + opp_econ) > 0.0F ? my_econ / (my_econ + opp_econ) : 0.5F;
    float war_frac = pairs > 0 ? static_cast<float>(war_pairs) / static_cast<float>(pairs) : 0.5F;
    float peace_honor_frac =
            pairs > 0 ? static_cast<float>(peace_pairs + ally_pairs) / static_cast<float>(pairs)
                      : 0.5F;
    float ally_score = pairs > 0 ? (0.70F * static_cast<float>(ally_pairs) +
                                    0.30F * static_cast<float>(peace_pairs)) /
                                           static_cast<float>(pairs)
                                 : 0.0F;

    float my_fast = 0.0F;
    float opp_fast = 0.0F;
    float my_hero_pts = 0.0F;
    float opp_hero_pts = 0.0F;
    int my_field = 0;
    int my_heavy_legion = 0;
    int my_blessed = 0;
    for (const Army* army : game.Armies()) {
        int owner = army->OwnerIndex();
        if (army->IsDisbanded() || (army->X() == kNone && !army->InTransit()) || owner < 0 ||
            owner >= 8) {
            continue;
        }
        bool cand = seat_plan.is_candidate[owner];
        if (army->MaxMoves() >= 14 && army->X() != kNone &&
            game::CityAt(game, army->X(), army->Y()) == nullptr) {
            if (cand)
                my_fast += static_cast<float>(army->Strength());
            else
                opp_fast += static_cast<float>(army->Strength());
        }
        if (army->Hero()) {
            float hero_pts = 10.0F + 6.0F * static_cast<float>(std::max(1, army->Level())) +
                             8.0F * static_cast<float>(army->Items().size());
            if (cand)
                my_hero_pts += hero_pts;
            else
                opp_hero_pts += hero_pts;
        }
        if (cand) {
            if (!army->Blessings().empty()) my_blessed++;
            if (army->X() != kNone && !army->InTransit()) {
                my_field++;
                int stack_cnt =
                        static_cast<int>(game::ArmiesOnTileSpan(game, army->X(), army->Y()).size());
                if (stack_cnt >= 4 || army->Strength() >= 6) my_heavy_legion++;
            }
        }
    }
    float fast_share = (my_fast + opp_fast) > 0.0F ? my_fast / (my_fast + opp_fast) : 0.5F;
    float hero_share =
            (my_hero_pts + opp_hero_pts) > 0.0F ? my_hero_pts / (my_hero_pts + opp_hero_pts) : 0.5F;
    float heavy_frac = my_field > 0
                               ? static_cast<float>(my_heavy_legion) / static_cast<float>(my_field)
                               : 0.0F;
    float bless_score = std::tanh(static_cast<float>(my_blessed) / 5.0F);

    if (archetype == "berserker") {
        return 0.40F * end_city + 0.20F * mid + 0.15F * end_army + 0.12F * razed_score +
               0.08F * fast_share + 0.05F * war_frac;
    }
    if (archetype == "guardian") {
        return 0.45F * end_city + 0.25F * garrison_frac + 0.15F * peace_honor_frac +
               0.15F * econ_share - 0.10F * razed_score;
    }
    if (archetype == "schemer") {
        return 0.45F * end_city + 0.22F * ally_score + 0.23F * gold_share + 0.10F * end_army;
    }
    if (archetype == "questor") {
        return 0.45F * end_city + 0.22F * hero_share + 0.20F * heavy_frac + 0.13F * bless_score;
    }
    return 0.75F * end_city + 0.15F * mid + 0.10F * end_army;
}

struct PairOutcome {
    float fit_pos = 0.5F;
    float fit_neg = 0.5F;
};

struct MidGameSnap {
    std::string bytes;
    SeatPlan seat_plan;
    float fork_share = 0.5F;
    int map_weight = 1;
};

int ScenarioCostWeight(std::string_view scenario) {
    if (scenario == "HADESHA" || scenario == "ERYTHEA" || scenario == "ISLADIA") return 4;
    if (scenario == "DRAGON") return 2;
    return 1;
}

std::vector<MidGameSnap> BuildSnapshotBank(const Config& cfg, const ml::PolicyNetwork& center_net,
                                           const ml::PolicyNetwork* frozen_net,
                                           unsigned int threads) {
    std::vector<MidGameSnap> bank;
    if (cfg.fork_frac <= 0.0F || cfg.fork_turn < 4 || cfg.fork_turn >= cfg.max_turns - 4) {
        return bank;
    }
    constexpr int kBankSize = 144;
    std::vector<MidGameSnap> raw(kBankSize);
    std::vector<char> valid(kBankSize, 0);
    std::vector<int> bank_order(kBankSize);
    for (int i = 0; i < kBankSize; ++i) bank_order[i] = i;
    std::stable_sort(bank_order.begin(), bank_order.end(), [](int lhs, int rhs) {
        auto w_lhs = ScenarioCostWeight(kScenarios[static_cast<size_t>(lhs / 2) % kScenarios.size()]);
        auto w_rhs = ScenarioCostWeight(kScenarios[static_cast<size_t>(rhs / 2) % kScenarios.size()]);
        return w_lhs > w_rhs;
    });
    std::atomic<int> next_idx{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned int thread_idx = 0; thread_idx < threads; thread_idx++) {
        pool.emplace_back([&]() {
            while (true) {
                int slot = next_idx.fetch_add(1);
                if (slot >= kBankSize) break;
                int bank_idx = bank_order[static_cast<size_t>(slot)];
                uint32_t seed =
                        1U + ((cfg.seed_offset + static_cast<uint32_t>(bank_idx * 4099 + 313)) %
                              kTrainSeedMod);
                int parity = bank_idx % 2;
                std::string scenario(
                        kScenarios[static_cast<size_t>(bank_idx / 2) % kScenarios.size()]);
                const Preset& preset = PickTrainingPreset(cfg, static_cast<uint32_t>(bank_idx / 2));
                int fork_turn =
                        std::clamp(cfg.fork_turn - 3 + (bank_idx % 7), 4, cfg.max_turns - 5);
                auto game = CreateGame(cfg, scenario, preset, seed, false);
                SeatPlan seat_plan =
                        AssignSeats(*game, cfg, seed, parity, frozen_net != nullptr, false);
                Side* cur = game::Begin(*game);
                AdvanceGame(*game, cur, fork_turn - 1, seat_plan, center_net, frozen_net);
                if (!game->IsOver() && !game->IsWon() && cur != nullptr) {
                    raw[bank_idx].bytes = save::Encode(*game);
                    raw[bank_idx].seat_plan = seat_plan;
                    raw[bank_idx].fork_share = CampCityShare(*game, seat_plan.is_candidate);
                    raw[bank_idx].map_weight = ScenarioCostWeight(scenario);
                    valid[bank_idx] = 1;
                }
            }
        });
    }
    for (auto& worker : pool) worker.join();
    for (int bank_idx = 0; bank_idx < kBankSize; bank_idx++) {
        if (valid[bank_idx]) bank.push_back(std::move(raw[bank_idx]));
    }
    return bank;
}

float EvalSingleCandidate(const Config& cfg, uint32_t seed, int parity, const MidGameSnap* snap,
                          const ml::PolicyNetwork& net, const ml::PolicyNetwork* frozen_net) {
    if (snap != nullptr) {
        auto decoded = save::Decode(snap->bytes, cfg.data_dir);
        ABSL_CHECK_OK(decoded);
        Game& game = **decoded;
        const SeatPlan& seat_plan = snap->seat_plan;
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            Side* side = game.GetMap().SideAt(side_idx);
            if (side != nullptr && side->InUse()) {
                ai::SetSideClassicAi(
                        game, *side,
                        !seat_plan.is_candidate[side_idx] &&
                                seat_plan.opp_kind[side_idx] != opponents::Kind::kFrozen);
            }
        }
        Side* cur = game.GetMap().SideAt(game.CurrentSideIndex());
        AdvanceGame(game, cur, cfg.max_turns, seat_plan, net, frozen_net);
        return ComputeFitness(game, seat_plan, snap->fork_share, cfg.archetype);
    }
    std::string scenario(kScenarios[seed % kScenarios.size()]);
    const Preset& preset = PickTrainingPreset(cfg, seed / 7U);
    const int mid_turn = std::max(2, cfg.max_turns / 2);
    auto game = CreateGame(cfg, scenario, preset, seed, true);
    SeatPlan seat_plan = AssignSeats(*game, cfg, seed, parity, frozen_net != nullptr, false);
    Side* cur = game::Begin(*game);
    float mid = -1.0F;
    AdvanceGame(*game, cur, cfg.max_turns, seat_plan, net, frozen_net, &mid, mid_turn);
    return ComputeFitness(*game, seat_plan, mid, cfg.archetype);
}

struct SelectEval {
    float mean_share = 0.0F;
    float ci95 = 0.0F;
    float mean_fitness = 0.0F;
    int games = 0;
};

SelectEval RunSelectionEval(const Config& cfg, const ml::PolicyNetwork& net, unsigned int threads) {
    const int total_games = cfg.select_seeds * 2;
    std::vector<float> shares(total_games, 0.5F);
    std::vector<float> fits(total_games, 0.5F);
    std::vector<int> eval_order(total_games);
    for (int i = 0; i < total_games; ++i) eval_order[i] = i;
    std::stable_sort(eval_order.begin(), eval_order.end(), [](int lhs, int rhs) {
        auto w_lhs = ScenarioCostWeight(kScenarios[static_cast<size_t>(lhs / 2) % kScenarios.size()]);
        auto w_rhs = ScenarioCostWeight(kScenarios[static_cast<size_t>(rhs / 2) % kScenarios.size()]);
        return w_lhs > w_rhs;
    });
    std::atomic<int> next_idx{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned int thread_idx = 0; thread_idx < threads; thread_idx++) {
        pool.emplace_back([&]() {
            while (true) {
                int slot = next_idx.fetch_add(1);
                if (slot >= total_games) break;
                int game_idx = eval_order[static_cast<size_t>(slot)];
                uint32_t seed = kSelectSeedBase + static_cast<uint32_t>(game_idx / 2);
                int parity = game_idx % 2;
                std::string scenario(
                        kScenarios[static_cast<size_t>(game_idx / 2) % kScenarios.size()]);
                const Preset& preset = kPresets[static_cast<size_t>(game_idx / 2) % 4];
                auto game = CreateGame(cfg, scenario, preset, seed, false);
                SeatPlan seat_plan = AssignSeats(*game, cfg, seed, parity, false, true);
                Side* cur = game::Begin(*game);
                float mid = -1.0F;
                AdvanceGame(*game, cur, cfg.eval_turns, seat_plan, net, nullptr, &mid,
                            std::max(2, cfg.eval_turns / 2));
                shares[game_idx] = CampCityShare(*game, seat_plan.is_candidate);
                fits[game_idx] = ComputeFitness(*game, seat_plan, mid, cfg.archetype);
            }
        });
    }
    for (auto& worker : pool) worker.join();

    double sum_share = 0.0;
    double sum_fit = 0.0;
    for (float share : shares) sum_share += share;
    for (float fit : fits) sum_fit += fit;
    float mean = total_games > 0 ? static_cast<float>(sum_share / total_games) : 0.0F;
    float mean_fit = total_games > 0 ? static_cast<float>(sum_fit / total_games) : 0.0F;
    double sq_err = 0.0;
    for (float share : shares) {
        double diff = static_cast<double>(share) - mean;
        sq_err += diff * diff;
    }
    float std_err =
            total_games > 1
                    ? static_cast<float>(std::sqrt(
                              sq_err / (static_cast<double>(total_games) * (total_games - 1))))
                    : 0.0F;
    return SelectEval{mean, 1.96F * std_err, mean_fit, total_games};
}

}  // namespace

int main(int argc, char** argv) {
    auto extra_args = ParseCliFlags(
            argc, argv, "w2evolve [--in PATH] [--out PATH] [--heads actor|...] [--generations N]");
    if (!extra_args.empty()) {
        std::fprintf(stderr, "w2evolve: unexpected positional argument '%s'\n",
                     extra_args[0].c_str());
        return 2;
    }

    const Config cfg = LoadConfigFromFlags();
    const int req_threads = absl::GetFlag(FLAGS_threads);
    const unsigned int threads =
            req_threads > 0 ? static_cast<unsigned int>(req_threads)
                            : std::max(2U, std::thread::hardware_concurrency());
    ml::SetDeployTemperature(cfg.eval_temp);
    ml::SetRecordThoughts(false);

    ml::PolicyNetwork frozen;
    const ml::PolicyNetwork* frozen_ptr = nullptr;
    if (!cfg.frozen_path.empty()) {
        if (absl::Status status = frozen.LoadJson(cfg.frozen_path); !status.ok()) {
            std::fprintf(stderr, "w2evolve: cannot load --frozen_opponent '%s': %s\n",
                         cfg.frozen_path.c_str(), std::string(status.message()).c_str());
            return 1;
        }
        frozen_ptr = &frozen;
    }

    ml::PolicyNetwork center;
    bool loaded = false;
    if (!cfg.from_scratch && std::filesystem::exists(cfg.in_path)) {
        loaded = center.LoadJson(cfg.in_path).ok();
    }
    if (!loaded) {
        center.InitRandomWeights(2026U);
    } else {
        if (cfg.heads == "archetype" && center.HasArchetypes()) {
            int arch_idx = cfg.archetype == "berserker"  ? 1
                           : cfg.archetype == "guardian" ? 2
                           : cfg.archetype == "schemer"  ? 3
                           : cfg.archetype == "questor"  ? 4
                                                         : 0;
            center.ApplyArchetypeHead(center.Archetypes()[static_cast<size_t>(arch_idx)]);
        }
        center.SetUsePersonalities(false);
        center.SetHasArchetypes(false);
        if (cfg.upgrade_v8) {
            center.UpgradeToV8();
        } else if (cfg.upgrade_v7) {
            center.UpgradeToV7();
        }
    }
    center.SetPersonaActive(cfg.qd_doctrine);

    if (cfg.use_commander) {
        if (center.Version() < 7) center.UpgradeToV7();
        if (!center.UseCommander()) {
            // Seed commander_w with small random projections while actor columns 25..29 remain zero
            // so generation 0 is function-preserving with immediate first-order ES sensitivity.
            std::mt19937 cmd_rng(555U);
            std::normal_distribution<float> cmd_norm(0.0F, 0.22F);
            for (float& weight : center.CommanderW()) weight = cmd_norm(cmd_rng);
            center.CommanderB().fill(0.0F);
        }
        center.SetUseCommander(true);
    }
    if (cfg.use_muster || cfg.use_standard) {
        center.UpgradeToV8();
        if (!center.UseMuster() && !center.UseStandard()) {
            // Seed MusterNet hidden layers with small symmetry-breaking weights while leaving
            // output layers near the 50/50 boundary.
            center.MusterHead().Zero();
            std::mt19937 muster_rng(888U);
            std::normal_distribution<float> muster_n1(
                    0.0F, 1.0F / std::sqrt(static_cast<float>(ml::kMusterInDim)));
            std::normal_distribution<float> muster_n2(0.0F, 1.0F / std::sqrt(12.0F));
            for (float& weight : center.MusterHead().w1) weight = muster_n1(muster_rng);
            for (float& weight : center.MusterHead().w2) weight = muster_n2(muster_rng);
            center.MusterHead().w_skip[ml::kContextDim + 0] = 0.04F;
            center.MusterHead().w_skip[22] = -0.04F;
        }
        if (cfg.use_muster) center.SetUseMuster(true);
        if (cfg.use_standard) center.SetUseStandard(true);
    }
    if (cfg.avoid_blockers) {
        center.SetAvoidBlockers(true);
        center.SetDetourBias(cfg.init_detour_bias);
    }
    if (!cfg.init_moe.empty()) {
        center.SetVersion(7);
        center.SetEnableCityGuard(false);
        center.Experts().clear();
        center.GateHead().Zero();
        {
            std::mt19937 gate_rng(777U);
            std::normal_distribution<float> gate_norm(0.0F, 0.18F);
            for (float& weight : center.GateHead().w1) weight = gate_norm(gate_rng);
            for (float& weight : center.GateHead().w2) weight = gate_norm(gate_rng);
        }
        size_t pos = 0;
        while (pos < cfg.init_moe.size() &&
               static_cast<int>(center.Experts().size()) < ml::PolicyNetwork::kMaxExperts) {
            size_t comma = cfg.init_moe.find(',', pos);
            std::string expert_path = cfg.init_moe.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? cfg.init_moe.size() : comma + 1;
            if (expert_path.empty()) continue;
            ml::PolicyNetwork exp_net;
            if (absl::Status status = exp_net.LoadJson(expert_path); !status.ok()) {
                std::fprintf(stderr, "w2evolve: cannot load MoE expert '%s': %s\n",
                             expert_path.c_str(), std::string(status.message()).c_str());
                return 1;
            }
            ml::PolicyNetwork::ExpertHeads expert_heads;
            expert_heads.version = exp_net.Version();
            expert_heads.use_commander = exp_net.UseCommander();
            expert_heads.commander_w = exp_net.CommanderW();
            expert_heads.commander_b = exp_net.CommanderB();
            expert_heads.diplo_net = exp_net.DiploHead();
            expert_heads.mission_net = exp_net.MissionHead();
            expert_heads.econ_net = exp_net.EconHead();
            center.Experts().push_back(std::move(expert_heads));
        }
        std::printf("  [init-moe] loaded %zu experts into Mixture-of-Experts PolicyNetwork\n",
                    center.Experts().size());
    }

    const std::vector<size_t> active_idx = BuildActiveIndices(cfg.heads);
    if (active_idx.empty()) {
        std::fprintf(stderr, "w2evolve: --heads '%s' matched 0 parameters\n", cfg.heads.c_str());
        return 2;
    }
    const size_t active_dim = active_idx.size();
    std::vector<float> theta = PackEvolveTheta(center);
    const std::vector<float> theta_init = theta;

    std::filesystem::path out_parent = std::filesystem::path(cfg.out_path).parent_path();
    if (!out_parent.empty()) std::filesystem::create_directories(out_parent);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> log_file(nullptr, &std::fclose);
    if (!cfg.log_path.empty()) {
        log_file.reset(std::fopen(cfg.log_path.c_str(), "w"));
        if (log_file) {
            std::fprintf(log_file.get(),
                         "gen,seconds,lr,sigma,mean_fit,diff_frac,abs_diff,grad_rms,"
                         "weight_rms,select_share,select_ci\n");
        }
    }

    std::printf(
            "=== w2evolve: Paired Antithetic ES | heads=%s (%zu/%d params) | threads=%u | %s ===\n",
            cfg.heads.c_str(), active_dim, ml::PolicyNetwork::kTotalParams, threads,
            loaded ? ("loaded " + cfg.in_path).c_str() : "random init");
    std::printf(
            "gens %d | dirs %d x %d seeds (%d games/gen) | turns %d (fork %.0f%% @%d) | "
            "sigma %.3f->%.3f | lr %.4f->%.4f\n",
            cfg.generations, cfg.dirs, cfg.seeds_per_dir, cfg.dirs * cfg.seeds_per_dir * 2,
            cfg.max_turns, cfg.fork_frac * 100.0F, cfg.fork_turn, cfg.sigma_start, cfg.sigma_end,
            cfg.lr_start, cfg.lr_end);
    std::fflush(stdout);

    SelectEval init_ev = RunSelectionEval(cfg, center, threads);
    const float init_share = init_ev.mean_share;
    float best_share = init_ev.mean_share;
    float best_fit = init_ev.mean_fitness;
    int best_gen = 0;
    ml::PolicyNetwork best_net = center;
    if (absl::Status status = best_net.SaveJson(cfg.out_path, 0, best_share); !status.ok()) {
        std::fprintf(stderr, "w2evolve: failed saving '%s': %s\n", cfg.out_path.c_str(),
                     std::string(status.message()).c_str());
        return 1;
    }
    std::printf(
            "  [select @0] %d games: Modern city share %.1f%% +/- %.1f%% | fit %.3f "
            "(archetype=%s)\n",
            init_ev.games, init_ev.mean_share * 100.0F, init_ev.ci95 * 100.0F, init_ev.mean_fitness,
            cfg.archetype.c_str());

    std::vector<MidGameSnap> snap_bank =
            cfg.generations > 0 ? BuildSnapshotBank(cfg, center, frozen_ptr, threads)
                                : std::vector<MidGameSnap>{};
    if (!snap_bank.empty()) {
        std::printf("  [midgame bank] %zu snapshots cached (turns %d..%d)\n", snap_bank.size(),
                    std::max(4, cfg.fork_turn - 3), std::min(cfg.max_turns - 5, cfg.fork_turn + 3));
    }
    std::fflush(stdout);

    std::vector<float> adam_m1(active_dim, 0.0F);
    std::vector<float> adam_m2(active_dim, 0.0F);
    constexpr float kBeta1 = 0.9F;
    constexpr float kBeta2 = 0.999F;
    constexpr float kEpsAdam = 1.0e-8F;

    std::mt19937 noise_rng(20261108U ^ cfg.seed_offset);
    std::normal_distribution<float> norm01(0.0F, 1.0F);

    for (int gen = 1; gen <= cfg.generations; gen++) {
        auto start_time = std::chrono::steady_clock::now();
        float prog = cfg.generations > 1
                             ? static_cast<float>(gen - 1) / static_cast<float>(cfg.generations - 1)
                             : 0.0F;
        float sigma = cfg.sigma_start + (cfg.sigma_end - cfg.sigma_start) * prog;
        float step_lr = cfg.lr_start + (cfg.lr_end - cfg.lr_start) * prog;

        std::vector<std::vector<float>> eps(cfg.dirs, std::vector<float>(active_dim));
        std::vector<ml::PolicyNetwork> pos_nets(cfg.dirs, center);
        std::vector<ml::PolicyNetwork> neg_nets(cfg.dirs, center);
        for (int dir_idx = 0; dir_idx < cfg.dirs; dir_idx++) {
            double sq_norm = 0.0;
            for (size_t j = 0; j < active_dim; j++) {
                float val = norm01(noise_rng);
                eps[dir_idx][j] = val;
                sq_norm += static_cast<double>(val) * val;
            }
            float scale = sq_norm > 1e-12
                                  ? static_cast<float>(std::sqrt(static_cast<double>(active_dim)) /
                                                       std::sqrt(sq_norm))
                                  : 1.0F;
            std::vector<float> weights_pos = theta;
            std::vector<float> weights_neg = theta;
            for (size_t j = 0; j < active_dim; j++) {
                eps[dir_idx][j] *= scale;
                size_t idx = active_idx[j];
                weights_pos[idx] = theta[idx] + sigma * eps[dir_idx][j];
                weights_neg[idx] = theta[idx] - sigma * eps[dir_idx][j];
            }
            UnpackEvolveTheta(pos_nets[dir_idx], weights_pos);
            UnpackEvolveTheta(neg_nets[dir_idx], weights_neg);
        }

        struct GameWork {
            int pair_idx = 0;
            bool is_pos = true;
            uint32_t seed = 1;
            int parity = 0;
            const MidGameSnap* snap = nullptr;
            int est_cost = 1;
        };
        const int total_pairs = cfg.dirs * cfg.seeds_per_dir;
        std::vector<GameWork> work;
        work.reserve(static_cast<size_t>(total_pairs) * 2);
        for (int task_idx = 0; task_idx < total_pairs; task_idx++) {
            int dir_idx = task_idx / cfg.seeds_per_dir;
            int seed_idx = task_idx % cfg.seeds_per_dir;
            uint32_t raw_seed = cfg.seed_offset + static_cast<uint32_t>(gen * 997 + dir_idx * 13 +
                                                                        (seed_idx / 2) * 3);
            uint32_t seed = 1U + (raw_seed % kTrainSeedMod);
            int parity = seed_idx % 2;
            uint32_t fork_hash = (static_cast<uint32_t>(task_idx) * 2654435761U) ^
                                 (static_cast<uint32_t>(gen) * 1597334677U);
            float fork_roll = static_cast<float>(fork_hash >> 8) / static_cast<float>(1U << 24);
            const MidGameSnap* snap = nullptr;
            int est_cost = ScenarioCostWeight(kScenarios[seed % kScenarios.size()]) * 2;
            if (!snap_bank.empty() && fork_roll < cfg.fork_frac) {
                size_t snap_idx =
                        static_cast<size_t>(gen * 131 + dir_idx * 7 + seed_idx) % snap_bank.size();
                snap = &snap_bank[snap_idx];
                est_cost = snap->map_weight;
            }
            work.push_back(GameWork{task_idx, true, seed, parity, snap, est_cost});
            work.push_back(GameWork{task_idx, false, seed, parity, snap, est_cost});
        }
        std::sort(work.begin(), work.end(), [](const GameWork& lhs, const GameWork& rhs) {
            return lhs.est_cost > rhs.est_cost;
        });

        std::vector<PairOutcome> outcomes(total_pairs);
        std::atomic<size_t> next_work{0};
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned int thread_idx = 0; thread_idx < threads; thread_idx++) {
            pool.emplace_back([&]() {
                while (true) {
                    size_t work_idx = next_work.fetch_add(1);
                    if (work_idx >= work.size()) break;
                    const GameWork& item = work[work_idx];
                    int dir_idx = item.pair_idx / cfg.seeds_per_dir;
                    const ml::PolicyNetwork& net =
                            item.is_pos ? pos_nets[dir_idx] : neg_nets[dir_idx];
                    float fit = EvalSingleCandidate(cfg, item.seed, item.parity, item.snap, net,
                                                    frozen_ptr);
                    if (item.is_pos)
                        outcomes[item.pair_idx].fit_pos = fit;
                    else
                        outcomes[item.pair_idx].fit_neg = fit;
                }
            });
        }
        for (auto& worker : pool) worker.join();

        std::vector<float> delta(cfg.dirs, 0.0F);
        double sum_fit = 0.0;
        double sum_abs_diff = 0.0;
        int non_zero = 0;
        for (int dir_idx = 0; dir_idx < cfg.dirs; dir_idx++) {
            float diff_sum = 0.0F;
            for (int seed_idx = 0; seed_idx < cfg.seeds_per_dir; seed_idx++) {
                const PairOutcome& outcome = outcomes[dir_idx * cfg.seeds_per_dir + seed_idx];
                diff_sum += (outcome.fit_pos - outcome.fit_neg);
                sum_fit += 0.5 * (outcome.fit_pos + outcome.fit_neg);
            }
            diff_sum /= static_cast<float>(cfg.seeds_per_dir);
            delta[dir_idx] = diff_sum;
            sum_abs_diff += std::abs(diff_sum);
            if (std::abs(diff_sum) > 1e-6F) non_zero++;
        }
        float mean_fit = static_cast<float>(sum_fit / std::max(1, total_pairs));
        float diff_frac = static_cast<float>(non_zero) / static_cast<float>(cfg.dirs);
        float mean_abs_diff = static_cast<float>(sum_abs_diff / cfg.dirs);

        // Centered magnitude-rank shaping: ties at 0 get weight 0; largest |delta| gets +/- 1,
        // normalized to unit RMS across K directions.
        std::vector<int> nz_order;
        nz_order.reserve(non_zero);
        for (int dir_idx = 0; dir_idx < cfg.dirs; dir_idx++) {
            if (std::abs(delta[dir_idx]) > 1e-6F) nz_order.push_back(dir_idx);
        }
        std::sort(nz_order.begin(), nz_order.end(),
                  [&](int lhs, int rhs) { return std::abs(delta[lhs]) < std::abs(delta[rhs]); });

        std::vector<float> weights(cfg.dirs, 0.0F);
        for (size_t rank_idx = 0; rank_idx < nz_order.size(); rank_idx++) {
            int dir_idx = nz_order[rank_idx];
            float mag = static_cast<float>(rank_idx + 1) / static_cast<float>(nz_order.size());
            weights[dir_idx] = (delta[dir_idx] > 0.0F ? 1.0F : -1.0F) * mag;
        }
        double weight_sq = 0.0;
        for (float weight : weights) weight_sq += static_cast<double>(weight) * weight;
        if (weight_sq > 1e-12) {
            float weight_norm =
                    static_cast<float>(std::sqrt(static_cast<double>(cfg.dirs) / weight_sq));
            for (float& weight : weights) weight *= weight_norm;
        }

        float bc1 = 1.0F - std::pow(kBeta1, static_cast<float>(gen));
        float bc2 = 1.0F - std::pow(kBeta2, static_cast<float>(gen));
        double grad_sq = 0.0;
        double theta_sq = 0.0;
        for (size_t j = 0; j < active_dim; j++) {
            float grad = 0.0F;
            for (int dir_idx = 0; dir_idx < cfg.dirs; dir_idx++) {
                grad += weights[dir_idx] * eps[dir_idx][j];
            }
            grad /= static_cast<float>(cfg.dirs);
            size_t idx = active_idx[j];
            grad -= cfg.l2 * theta[idx];
            if (cfg.anchor_l2 > 0.0F) grad -= cfg.anchor_l2 * (theta[idx] - theta_init[idx]);

            grad_sq += static_cast<double>(grad) * grad;
            adam_m1[j] = kBeta1 * adam_m1[j] + (1.0F - kBeta1) * grad;
            adam_m2[j] = kBeta2 * adam_m2[j] + (1.0F - kBeta2) * grad * grad;
            float m_hat = adam_m1[j] / bc1;
            float v_hat = adam_m2[j] / bc2;
            theta[idx] += step_lr * m_hat / (std::sqrt(v_hat) + kEpsAdam);
            theta_sq += static_cast<double>(theta[idx]) * theta[idx];
        }
        UnpackEvolveTheta(center, theta);
        float grad_rms = static_cast<float>(std::sqrt(grad_sq / static_cast<double>(active_dim)));
        float weight_rms =
                static_cast<float>(std::sqrt(theta_sq / static_cast<double>(active_dim)));

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time)
                              .count();
        float sel_share = std::numeric_limits<float>::quiet_NaN();
        float sel_ci = std::numeric_limits<float>::quiet_NaN();

        std::printf(
                "[gen %3d] %.1fs lr=%.4f sig=%.3f | fit %.3f | diff %.0f%% (|d|=%.3f) | g_rms %.4f "
                "w_rms %.4f\n",
                gen, secs, step_lr, sigma, mean_fit, diff_frac * 100.0F, mean_abs_diff, grad_rms,
                weight_rms);

        if (cfg.save_every > 0 && gen % cfg.save_every == 0) {
            std::filesystem::path snap_path = cfg.out_path;
            snap_path.replace_extension(absl::StrFormat(".gen%d.json", gen));
            center.SaveJson(snap_path, gen, 0.0F).IgnoreError();
        }

        if (gen % cfg.eval_every == 0 || gen == cfg.generations) {
            SelectEval eval_res = RunSelectionEval(cfg, center, threads);
            sel_share = eval_res.mean_share;
            sel_ci = eval_res.ci95;
            std::printf("  [select @%d] %d games: Modern city share %.1f%% +/- %.1f%% | fit %.3f\n",
                        gen, eval_res.games, eval_res.mean_share * 100.0F, eval_res.ci95 * 100.0F,
                        eval_res.mean_fitness);
            bool custom_arch = (!cfg.archetype.empty() && cfg.archetype != "imperial");
            float share_tol = cfg.qd_doctrine ? 0.008F : 0.025F;
            bool better = custom_arch ? (eval_res.mean_fitness > best_fit &&
                                         eval_res.mean_share >= init_share - share_tol)
                                      : (eval_res.mean_share > best_share);
            if (better) {
                best_share = eval_res.mean_share;
                best_fit = eval_res.mean_fitness;
                best_gen = gen;
                best_net = center;
                best_net.SaveJson(cfg.out_path, best_gen, best_share).IgnoreError();
            }
        }
        std::fflush(stdout);

        if (log_file) {
            std::fprintf(log_file.get(), "%d,%.2f,%.5f,%.4f,%.4f,%.3f,%.4f,%.5f,%.5f,%.4f,%.4f\n",
                         gen, secs, step_lr, sigma, mean_fit, diff_frac, mean_abs_diff, grad_rms,
                         weight_rms, sel_share, sel_ci);
            std::fflush(log_file.get());
        }
    }

    std::filesystem::path fin_path = cfg.out_path;
    fin_path.replace_extension(".final.json");
    center.SaveJson(fin_path, cfg.generations, 0.0F).IgnoreError();
    std::printf("Kept generation %d (selection share %.1f%%) in %s; final in %s.\n", best_gen,
                best_share * 100.0F, cfg.out_path.c_str(), fin_path.string().c_str());
    return 0;
}
