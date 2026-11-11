// Modern-AI behaviour diagnostic (`w2aidiag`): plays one game with the Modern
// AI on every other side and the 1993 Classic AI on the rest, then prints
//   - every 5 turns, each side's cities, armies, strength, gold and upkeep;
//   - the share of Modern armies that moved on their own turn;
//   - the missions the Modern AI chose (by kind) and what its cities built;
//   - every battle by camp and stack size, and for Modern missions how the win
//     estimate MissionNet saw compares with the true odds and the result.
// Used to see *why* a checkpoint wins or loses, which the city-share
// benchmark in w2arena does not show.
//
//   bazel-bin/cpp/w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1]
//
// Run from the repository root, so that original/ is found.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"

#include "opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/attitude.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, scenario, "ERYTHEA", "Scenario directory name (or RANDOM).");
ABSL_FLAG(int, seed, 800001, "Random seed.");
ABSL_FLAG(int, turns, 40, "Maximum turns to play.");
ABSL_FLAG(int, hidden_map, 0, "Hidden Map option (0 or 1).");
ABSL_FLAG(int, active_sides, 8, "Number of active sides (2..8).");
ABSL_FLAG(int, crop_margin, 0, "Crop margin around active capitals (0 = full map).");
ABSL_FLAG(int, solo_seat, -1, "Solo Modern seat index (-1 = alternating 4v4).");
ABSL_FLAG(int, diplomacy, 0, "Diplomacy option (0 or 1).");
ABSL_FLAG(std::string, opponent, "classic", "Opponent personality: classic, raider, or warden.");
ABSL_FLAG(int, modern_level, -1, "Modern AI difficulty level override (-1 = default).");
ABSL_FLAG(int, modern_card, -1, "Modern AI ruler personality card override (-1 = default).");
ABSL_FLAG(bool, card_matrix, false,
          "Run parallel 27-card behavioral fingerprint matrix across Knights, Lords, and Warlords.");
ABSL_FLAG(int, matrix_seeds, 8, "Number of games per card in --card-matrix mode.");
ABSL_FLAG(int, threads, 0, "Worker thread count for --card-matrix (0 = hardware_concurrency).");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;

namespace {

class ScopedMissionFightCallback {
  public:
    explicit ScopedMissionFightCallback(ai::modern::MissionFightCallback callback) {
        ai::modern::SetMissionFightCallback(std::move(callback));
    }
    ~ScopedMissionFightCallback() { ai::modern::SetMissionFightCallback(nullptr); }

    ScopedMissionFightCallback(const ScopedMissionFightCallback&) = delete;
    ScopedMissionFightCallback& operator=(const ScopedMissionFightCallback&) = delete;
};

class DiagTurnEvents final : public IAiTurnEvents {
  public:
    DiagTurnEvents(const bool* modern, std::array<std::array<long, 3>, 2>& battles,
                   std::array<std::array<long, 3>, 2>& battle_wins, float& last_true_odds)
            : modern_(modern)
            , battles_(battles)
            , battle_wins_(battle_wins)
            , last_true_odds_(last_true_odds) {}

    bool OnFight(Game& game, const ArmyStack& /*attackers*/, int x, int y,
                 Battle& result) override {
        std::vector<Army*> attackers(result.attackers.begin(), result.attackers.end());
        attackers.insert(attackers.end(), result.dead_attackers.begin(),
                         result.dead_attackers.end());
        std::vector<Army*> defenders(result.defenders.begin(), result.defenders.end());
        defenders.insert(defenders.end(), result.dead_defenders.begin(),
                         result.dead_defenders.end());
        if (attackers.empty() || attackers[0]->OwnerIndex() < 0 ||
            attackers[0]->OwnerIndex() >= 8) {
            return true;
        }
        int camp = modern_[attackers[0]->OwnerIndex()] ? 0 : 1;
        int bucket = attackers.size() <= 1 ? 0 : (attackers.size() <= 3 ? 1 : 2);
        battles_[camp][bucket]++;
        if (result.won) battle_wins_[camp][bucket]++;
        last_true_odds_ = 1.0F;
        if (camp == 0 && !defenders.empty()) {
            uint32_t saved = game.GetRng().State();
            int wins = 0;
            for (int sim = 0; sim < 32; sim++) {
                uint32_t next_state =
                        saved ^ static_cast<uint32_t>(0x9e3779b9U * static_cast<uint32_t>(sim + 1));
                game.GetRng().SetState(next_state == 0U ? 1U : next_state);
                if (combat::Resolve(game, attackers, defenders, x, y).won) wins++;
            }
            game.GetRng().SetState(saved);
            last_true_odds_ = static_cast<float>(wins) / 32.0F;
        }
        return true;
    }

  private:
    const bool* modern_;
    std::array<std::array<long, 3>, 2>& battles_;
    std::array<std::array<long, 3>, 2>& battle_wins_;
    float& last_true_odds_;
};

struct CardStats {
    double city_share_sum = 0.0;
    long games = 0;
    long prod_total = 0;
    long prod_cav = 0;
    long prod_fly = 0;
    long prod_heavy = 0;
    long captures = 0;
    long pillage_or_sack = 0;
    long razed = 0;
    long city_turns = 0;
    long garrisoned_turns = 0;
    long battles_total = 0;
    long battles_4plus = 0;
    long side_turns = 0;
    long gold_sum = 0;
    long diplo_pairs = 0;
    long ally_pairs = 0;
    long war_pairs = 0;
    long vendetta_wars = 0;
    long blessed_and_items = 0;

    void MergeFrom(const CardStats& other) {
        city_share_sum += other.city_share_sum;
        games += other.games;
        prod_total += other.prod_total;
        prod_cav += other.prod_cav;
        prod_fly += other.prod_fly;
        prod_heavy += other.prod_heavy;
        captures += other.captures;
        pillage_or_sack += other.pillage_or_sack;
        razed += other.razed;
        city_turns += other.city_turns;
        garrisoned_turns += other.garrisoned_turns;
        battles_total += other.battles_total;
        battles_4plus += other.battles_4plus;
        side_turns += other.side_turns;
        gold_sum += other.gold_sum;
        diplo_pairs += other.diplo_pairs;
        ally_pairs += other.ally_pairs;
        war_pairs += other.war_pairs;
        vendetta_wars += other.vendetta_wars;
        blessed_and_items += other.blessed_and_items;
    }
};

class MatrixTurnEvents final : public IAiTurnEvents {
  public:
    MatrixTurnEvents(const std::array<bool, 8>& is_modern, CardStats& stats)
            : is_modern_(is_modern), stats_(stats) {}

    bool OnFight(Game& /*game*/, const ArmyStack& /*attackers*/, int /*pos_x*/, int /*pos_y*/,
                 Battle& result) override {
        size_t atk_count = result.attackers.size() + result.dead_attackers.size();
        const Army* lead = !result.attackers.empty()
                                   ? result.attackers[0]
                                   : (!result.dead_attackers.empty() ? result.dead_attackers[0]
                                                                     : nullptr);
        if (lead && lead->OwnerIndex() >= 0 && lead->OwnerIndex() < 8 &&
            is_modern_[static_cast<size_t>(lead->OwnerIndex())]) {
            stats_.battles_total++;
            if (atk_count >= 4) stats_.battles_4plus++;
        }
        return true;
    }

    bool OnSpoils(Game& /*game*/, Side& side, City& /*city*/, std::string_view action) override {
        if (side.Index() >= 0 && side.Index() < 8 &&
            is_modern_[static_cast<size_t>(side.Index())]) {
            if (action == "pillaged" || action == "sacked") stats_.pillage_or_sack++;
            if (action == "razed") stats_.razed++;
        }
        return true;
    }

  private:
    const std::array<bool, 8>& is_modern_;
    CardStats& stats_;
};

int RunCardMatrix(const std::string& data_dir, const ai::ml::PolicyNetwork& net, int turns,
                  int games_per_card, opponents::Kind opponent, unsigned int num_threads) {
    ai::ml::SetRecordThoughts(false);
    constexpr std::array<std::string_view, 2> kMatrixScenarios{"ERYTHEA", "HADESHA"};
    struct MatrixJob {
        int card_flat = 0;
        int level = 0;
        int card = 0;
        int game_idx = 0;
        CardStats result{};
    };
    std::vector<MatrixJob> jobs;
    jobs.reserve(static_cast<size_t>(27 * games_per_card));
    for (int level = 0; level < 3; ++level) {
        for (int card = 0; card < 9; ++card) {
            for (int game_idx = 0; game_idx < games_per_card; ++game_idx) {
                jobs.push_back(MatrixJob{level * 9 + card, level, card, game_idx, {}});
            }
        }
    }

    std::atomic<size_t> next_job{0};
    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&]() {
            while (true) {
                size_t job_idx = next_job.fetch_add(1);
                if (job_idx >= jobs.size()) break;
                MatrixJob& job = jobs[job_idx];
                std::string scen(kMatrixScenarios[static_cast<size_t>(job.game_idx % 2)]);
                int diplo_opt = (job.game_idx % 4 < 2) ? 1 : 0;
                int hidden_opt = (job.game_idx % 4 >= 1 && job.game_idx % 4 <= 2) ? 1 : 0;
                int parity = (job.game_idx / 4) % 2;
                int seed_val = 750101 + (job.game_idx / 2) * 17;

                game::NewGameOptions gopts;
                gopts.seed = seed_val;
                gopts.active_sides = 8;
                gopts.options = {{"hiddenMap", hidden_opt},
                                 {"viewEnemies", 0},
                                 {"diplomacy", diplo_opt},
                                 {"neutralCities", 1},
                                 {"quests", 1}};
                for (int side_i = 0; side_i < 8; ++side_i) {
                    gopts.sides[side_i].computer = true;
                    gopts.sides[side_i].level = 2;
                    gopts.sides[side_i].card = (seed_val + side_i * 5) % 9;
                }
                auto created = game::NewGame(data_dir, scen, gopts);
                if (!created.ok()) continue;
                Game& game = **created;

                std::array<bool, 8> is_mod{};
                int seat = 0;
                for (int side_i = 0; side_i < 8; ++side_i) {
                    Side& side_ref = game.GetMap().Sides()[side_i];
                    if (!side_ref.InUse()) continue;
                    is_mod[static_cast<size_t>(side_i)] = ((seat + parity) % 2 == 0);
                    seat++;
                    if (is_mod[static_cast<size_t>(side_i)]) {
                        side_ref.SetLevel(job.level);
                        side_ref.SetCard(job.card);
                        ai::SetSideClassicAi(game, side_ref, false);
                    } else {
                        opponents::InstallOpponent(game, side_ref, opponent);
                    }
                }

                CardStats& stats = job.result;
                MatrixTurnEvents turn_events(is_mod, stats);
                Side* cur = game::Begin(game);
                int safety = 0;
                while (cur && !game.IsOver() && !game.IsWon() && game.Turn() <= turns &&
                       safety++ < turns * 12) {
                    std::vector<int> prev_owners(game.GetMap().Cities().size(), kNone);
                    for (size_t ci = 0; ci < game.GetMap().Cities().size(); ++ci) {
                        prev_owners[ci] = game.GetMap().Cities()[ci].OwnerIndex();
                    }
                    const int mover = cur->Index();
                    if (cur->IsAlive()) {
                        if (is_mod[static_cast<size_t>(mover)]) {
                            ai::modern::PlayTurn(game, *cur, &turn_events, &net, nullptr);
                            stats.side_turns++;
                            stats.gold_sum += cur->Gold();
                            if (diplo_opt != 0) {
                                const ModernMlPlayer* mod_player = ModernMlPlayer::FindOn(*cur);
                                for (int oth = 0; oth < 8; ++oth) {
                                    const Side& oth_side = game.GetMap().Sides()[oth];
                                    if (oth == mover || !oth_side.InUse() || !oth_side.IsAlive()) {
                                        continue;
                                    }
                                    stats.diplo_pairs++;
                                    if (diplomacy::IsAllied(game, mover, oth)) stats.ally_pairs++;
                                    if (diplomacy::State(game, mover, oth) == diplomacy::kWar) {
                                        stats.war_pairs++;
                                        if (mod_player &&
                                            ai::ComputeVendettaScore(mod_player->Grudge(), oth,
                                                                     game.Turn()) >= 0.30F) {
                                            stats.vendetta_wars++;
                                        }
                                    }
                                }
                            }
                            for (const City& city : game.GetMap().Cities()) {
                                if (city.IsRazed() || !city.OwnedBy(cur->Id())) continue;
                                stats.city_turns++;
                                int defs = 0;
                                for (int dy = 0; dy <= 1; ++dy) {
                                    for (int dx = 0; dx <= 1; ++dx) {
                                        for (Army* occ :
                                             game::ArmiesOnTileSpan(game, city.X() + dx,
                                                                    city.Y() + dy)) {
                                            if (occ->OwnedBy(cur->Id())) defs++;
                                        }
                                    }
                                }
                                if (defs > 0) stats.garrisoned_turns++;
                                if (city.ProducingSlot() >= 0 &&
                                    city.ProducingSlot() < city.SlotCount()) {
                                    const Slot& slot =
                                            city.SlotAt(static_cast<size_t>(city.ProducingSlot()));
                                    const ArmyType* atype = game.ArmyTypes().ById(slot.type);
                                    bool flies = atype && atype->flies;
                                    bool heavy = (atype && (atype->siege || atype->IsMagical())) ||
                                                 slot.strength >= 7;
                                    bool cav = (slot.move >= 14 && !flies);
                                    stats.prod_total++;
                                    if (cav) stats.prod_cav++;
                                    if (flies) stats.prod_fly++;
                                    if (heavy) stats.prod_heavy++;
                                }
                            }
                        } else {
                            opponents::PlayTurn(game, *cur, opponent, nullptr, &turn_events);
                        }
                    }
                    if (mover >= 0 && mover < 8 && is_mod[static_cast<size_t>(mover)]) {
                        for (size_t ci = 0; ci < game.GetMap().Cities().size(); ++ci) {
                            const City& city = game.GetMap().Cities()[ci];
                            if (!city.IsRazed() && city.OwnerIndex() == mover &&
                                prev_owners[ci] != mover) {
                                stats.captures++;
                            }
                        }
                    }
                    cur = game::EndTurn(game);
                }

                int mod_cities = 0;
                int all_cities = 0;
                for (const City& city : game.GetMap().Cities()) {
                    if (city.IsRazed() || city.IsNeutral()) continue;
                    int own = city.OwnerIndex();
                    if (own >= 0 && own < 8) {
                        all_cities++;
                        if (is_mod[static_cast<size_t>(own)]) mod_cities++;
                    }
                }
                stats.city_share_sum +=
                        all_cities > 0 ? static_cast<double>(mod_cities) / all_cities : 0.5;
                stats.games = 1;
                for (const Army* army : game.Armies()) {
                    int own = army->OwnerIndex();
                    if (own < 0 || own >= 8 || !is_mod[static_cast<size_t>(own)] ||
                        army->IsDisbanded()) {
                        continue;
                    }
                    if (!army->Blessings().empty()) stats.blessed_and_items++;
                    stats.blessed_and_items += static_cast<long>(army->Items().size());
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::array<CardStats, 27> agg{};
    for (const MatrixJob& job : jobs) {
        agg[static_cast<size_t>(job.card_flat)].MergeFrom(job.result);
    }

    const char* tier_names[3] = {"Knight", "Lord", "Warlord"};
    std::printf(
            "| Tier | Card | Archetype | Share%% | Cav%% | Fly%% | Heavy%% | Plunder/Raze | "
            "Garr%% | 4+Atk%% | Gold | Ally/War%% | QuestPts |\n");
    std::printf(
            "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");

    auto pct = [](long num, long den) {
        return 100.0 * static_cast<double>(num) / static_cast<double>(std::max(1L, den));
    };
    for (int level = 0; level < 3; ++level) {
        for (int card = 0; card < 9; ++card) {
            const CardStats& row = agg[static_cast<size_t>(level * 9 + card)];
            const auto& prof = ai::personality::ProfileFor(level, card);
            double share = 100.0 * row.city_share_sum / static_cast<double>(std::max(1L, row.games));
            std::printf(
                    "| %s | %s | %s | %.1f%% | %.1f%% | %.1f%% | %.1f%% | %ld/%ld | %.1f%% | "
                    "%.1f%% | %ld | %.1f/%.1f%% | %.1f |\n",
                    tier_names[level], prof.name.c_str(), prof.archetype.c_str(), share,
                    pct(row.prod_cav, row.prod_total), pct(row.prod_fly, row.prod_total),
                    pct(row.prod_heavy, row.prod_total), row.pillage_or_sack, row.razed,
                    pct(row.garrisoned_turns, row.city_turns),
                    pct(row.battles_4plus, row.battles_total),
                    row.gold_sum / std::max(1L, row.side_turns),
                    pct(row.ally_pairs, row.diplo_pairs), pct(row.war_pairs, row.diplo_pairs),
                    static_cast<double>(row.blessed_and_items) /
                            static_cast<double>(std::max(1L, row.games)));
        }
    }
    return 0;
}

}  // namespace

/**
 * @brief Prints one line per side in use: cities, armies, strength, gold and upkeep.
 * @param modern Which of the eight sides the Modern AI plays.
 */
static void PrintSides(Game& game, const bool* modern) {
    std::printf("--- turn %d\n", game.Turn());
    for (int side_index = 0; side_index < 8; side_index++) {
        Side& side = game.GetMap().Sides()[side_index];
        if (!side.InUse()) continue;
        int cities = 0, armies = 0, strength = 0, in_city = 0;
        for (auto& city : game.GetMap().Cities())
            if (!city.IsRazed() && city.OwnedBy(side_index)) cities++;
        for (Army* army : game.Armies()) {
            if (!army->OwnedBy(side_index) || army->X() == kNone) continue;
            armies++;
            strength += army->Strength();
            if (game::CityAt(game, army->X(), army->Y())) in_city++;
        }
        std::printf(
                "  side %d %s %s cities=%2d armies=%3d strength=%4d in-city=%3d gold=%5d "
                "income=%4d upkeep=%4d\n",
                side_index, modern[side_index] ? "MODERN " : "CLASSIC",
                side.IsAlive() ? "     " : "(dead)", cities, armies, strength, in_city, side.Gold(),
                game::Income(game, side), game::Upkeep(game, side));
    }
}

/**
 * @brief Plays the diagnostic game and prints the side table and the Modern AI's statistics.
 *
 * See the file comment for the arguments.
 */
int main(int argc, char** argv) {
    auto args = ParseCliFlags(
            argc, argv,
            "w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1] [ACTIVE_SIDES] "
            "[CROP_MARGIN] [SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider|warden] "
            "[MODERN_LEVEL -1|0..3] [MODERN_CARD -1|0..26]");
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1] "
                     "[ACTIVE_SIDES] [CROP_MARGIN] "
                     "[SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider|warden] "
                     "[MODERN_LEVEL -1|0..3] [MODERN_CARD -1|0..26]\n");
        return 2;
    }
    std::string data_dir = absl::GetFlag(FLAGS_data);
    std::string weights = args[0];
    std::string scenario = args.size() > 1 ? args[1] : absl::GetFlag(FLAGS_scenario);
    int seed = args.size() > 2 ? static_cast<int>(ToInt(args[2]).value_or(800001))
                               : absl::GetFlag(FLAGS_seed);
    int turns = args.size() > 3 ? static_cast<int>(ToInt(args[3]).value_or(40))
                                : absl::GetFlag(FLAGS_turns);
    int hidden = args.size() > 4 ? static_cast<int>(ToInt(args[4]).value_or(0))
                                 : absl::GetFlag(FLAGS_hidden_map);
    int active_sides = args.size() > 5 ? static_cast<int>(ToInt(args[5]).value_or(8))
                                       : absl::GetFlag(FLAGS_active_sides);
    int crop_margin = args.size() > 6 ? static_cast<int>(ToInt(args[6]).value_or(0))
                                      : absl::GetFlag(FLAGS_crop_margin);
    int solo_seat = args.size() > 7 ? static_cast<int>(ToInt(args[7]).value_or(-1))
                                    : absl::GetFlag(FLAGS_solo_seat);
    int diplo = args.size() > 8 ? static_cast<int>(ToInt(args[8]).value_or(0))
                                : absl::GetFlag(FLAGS_diplomacy);
    std::string opponent_name = args.size() > 9 ? args[9] : absl::GetFlag(FLAGS_opponent);
    int modern_level = args.size() > 10 ? static_cast<int>(ToInt(args[10]).value_or(-1))
                                        : absl::GetFlag(FLAGS_modern_level);
    int modern_card = args.size() > 11 ? static_cast<int>(ToInt(args[11]).value_or(-1))
                                       : absl::GetFlag(FLAGS_modern_card);
    opponents::Kind opponent = opponent_name == "raider"         ? opponents::Kind::kRaider
                               : opponent_name == "warden"       ? opponents::Kind::kWarden
                               : opponent_name == "warband"      ? opponents::Kind::kWarband
                               : (opponent_name == "fair-classic" ||
                                  opponent_name == "fair_classic")
                                       ? opponents::Kind::kFairClassic
                                       : opponents::Kind::kClassic;
    ai::ml::PolicyNetwork net;
    if (absl::Status status = net.LoadJson(weights); !status.ok()) {
        std::fprintf(stderr, "w2aidiag: %s\n", std::string(status.message()).c_str());
        return 1;
    }
    if (absl::GetFlag(FLAGS_card_matrix)) {
        int req_threads = absl::GetFlag(FLAGS_threads);
        unsigned int num_threads =
                req_threads > 0 ? static_cast<unsigned int>(req_threads)
                                : std::max(2U, std::thread::hardware_concurrency());
        int matrix_seeds = std::max(2, absl::GetFlag(FLAGS_matrix_seeds));
        return RunCardMatrix(data_dir, net, turns, matrix_seeds, opponent, num_threads);
    }
    game::NewGameOptions game_options;
    game_options.seed = seed;
    game_options.active_sides = active_sides;
    game_options.crop_margin = crop_margin;
    game_options.options = {{"hiddenMap", hidden},
                            {"viewEnemies", 0},
                            {"diplomacy", diplo},
                            {"neutralCities", 1},
                            {"quests", 1}};
    for (int i = 0; i < 8; i++) {
        game_options.sides[i].computer = true;
        game_options.sides[i].level = 2;
        game_options.sides[i].card = (seed + i * 5) % 9;
    }
    randommap::Files rfiles;
    if (scenario == "RANDOM") {
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options map_options;
        map_options.data_dir = data_dir;
        map_options.rng = &map_rng;
        map_options.sliders = {3, 3, 2, 3};
        auto generated = randommap::GenerateNow(map_options);
        if (!generated.ok()) {
            std::fprintf(stderr, "w2aidiag: %s\n",
                         std::string(generated.status().message()).c_str());
            return 1;
        }
        rfiles = std::move(*generated);
        game_options.memory_files = &rfiles;
    }
    auto created_game = game::NewGame(data_dir, scenario, game_options);
    if (!created_game.ok()) {
        std::fprintf(stderr, "w2aidiag: %s\n",
                     std::string(created_game.status().message()).c_str());
        return 1;
    }
    std::unique_ptr<Game> game_ptr = std::move(*created_game);
    Game& game = *game_ptr;
    std::printf("=== %s seed=%d sides=%d crop=%d cities=%zu box=[%d..%d]x[%d..%d] ===\n",
                scenario.c_str(), seed, active_sides, crop_margin, game.GetMap().Cities().size(),
                game.GetMap().ActiveX0(), game.GetMap().ActiveX1(), game.GetMap().ActiveY0(),
                game.GetMap().ActiveY1());
    bool modern[8] = {};
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (!game.GetMap().Sides()[side_index].InUse()) continue;
        modern[side_index] = (solo_seat >= 0) ? (seat == solo_seat) : (seat % 2 == 0);
        seat++;
        if (modern[side_index]) {
            Side& side = game.GetMap().Sides()[side_index];
            if (modern_level >= 0) side.SetLevel(modern_level);
            if (modern_card >= 0) side.SetCard(modern_card);
            ai::SetSideClassicAi(game, side, false);
        } else {
            opponents::InstallOpponent(game, game.GetMap().Sides()[side_index], opponent);
        }
    }

    std::map<std::string, int> missions, built;
    std::array<std::array<long, 3>, 2> battles{};
    std::array<std::array<long, 3>, 2> battle_wins{};
    float last_true_odds = 1.0F;
    DiagTurnEvents diag_events(modern, battles, battle_wins, last_true_odds);
    struct MissionStats {
        long count = 0;
        long won = 0;
        long shrunk = 0;
        double decided = 0.0;
        double odds = 0.0;
    };
    std::array<MissionStats, 2> mission_stats{};
    std::array<long, 10> cal_n{};
    std::array<long, 10> cal_won{};
    std::array<double, 10> cal_odds{};
    ScopedMissionFightCallback mission_fight_guard(
            [&](Game& /*cb_game*/, const ai::modern::MissionFight& mission_fight,
                const Battle& result) {
                MissionStats& stat = mission_stats[mission_fight.at_target ? 0 : 1];
                stat.count++;
                if (result.won) stat.won++;
                if (mission_fight.fight_size < mission_fight.decided_size) stat.shrunk++;
                stat.decided += mission_fight.decided_win;
                stat.odds += last_true_odds;
                if (mission_fight.at_target) {
                    int bin = std::clamp(static_cast<int>(mission_fight.decided_win * 10.0F), 0, 9);
                    cal_n[bin]++;
                    cal_odds[bin] += last_true_odds;
                    if (result.won) cal_won[bin]++;
                }
            });
    std::array<long, 9> attack_sizes{};
    long army_turns = 0, moved = 0;
    long city_turns = 0, producing_turns = 0, purchases = 0, rich_turns = 0, side_turns = 0;
    int last_printed = 0;
    Side* current = game::Begin(game);
    // City changes of hands: losses by camp of the loser, bucketed by the
    // garrison (armies in the city) the loser had when the attacker's turn began.
    long lost_by_camp[2][3] = {}, captured_from_enemy[2] = {}, captured_neutral[2] = {};
    std::array<long, 2> razed_by_camp{};
    long gold_sum = 0, ally_pairs = 0, war_pairs = 0, diplo_pairs = 0, garrisoned_city_turns = 0;
    auto garrison_of = [&](const City& city) {
        int count = 0;
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                for (Army* army : game::ArmiesAt(game, city.X() + dx, city.Y() + dy))
                    if (army->Owner() == city.Owner()) count++;
        return count;
    };
    while (current && !game.IsOver() && !game.IsWon() && game.Turn() <= turns) {
        std::vector<int> owner_before(game.GetMap().Cities().size()),
                garrison_before(game.GetMap().Cities().size());
        std::vector<char> razed_before(game.GetMap().Cities().size(), 0);
        for (size_t city_index = 0; city_index < game.GetMap().Cities().size(); city_index++) {
            owner_before[city_index] = game.GetMap().Cities()[city_index].OwnerIndex();
            garrison_before[city_index] = !game.GetMap().Cities()[city_index].IsNeutral()
                                                  ? garrison_of(game.GetMap().Cities()[city_index])
                                                  : 0;
            razed_before[city_index] = game.GetMap().Cities()[city_index].IsRazed() ? 1 : 0;
        }
        const int mover = current->Index();
        if (current->IsAlive()) {
            if (modern[current->Index()]) {
                std::map<Army*, int> before;
                for (Army* army : game.Armies())
                    if (army->OwnedBy(current->Id()) && army->X() != kNone)
                        before[army] = army->Y() * 1000 + army->X();
                std::map<int, std::vector<int>> slots_before;
                for (auto& city : game.GetMap().Cities()) {
                    if (!city.OwnedBy(current->Id())) continue;
                    for (const auto& slot : city.Slots())
                        slots_before[city.Index()].push_back(slot.type);
                }
                ai::modern::PlayTurn(game, *current, &diag_events, &net, nullptr);
                side_turns++;
                gold_sum += current->Gold();
                if (current->Gold() >= 1500) rich_turns++;
                for (int other_idx = 0; other_idx < 8; other_idx++) {
                    if (other_idx == current->Index() ||
                        !game.GetMap().Sides()[other_idx].InUse() ||
                        !game.GetMap().Sides()[other_idx].IsAlive()) {
                        continue;
                    }
                    diplo_pairs++;
                    if (diplomacy::IsAllied(game, current->Index(), other_idx)) ally_pairs++;
                    if (diplomacy::State(game, current->Index(), other_idx) == diplomacy::kWar) {
                        war_pairs++;
                    }
                }
                for (auto& city : game.GetMap().Cities()) {
                    if (!city.OwnedBy(current->Id()) || city.IsRazed()) continue;
                    city_turns++;
                    if (garrison_of(city) > 0) garrisoned_city_turns++;
                    if (game::Producing(city)) producing_turns++;
                    auto city_slots_before = slots_before.find(city.Index());
                    for (const auto& slot : city.Slots()) {
                        if (city_slots_before != slots_before.end() &&
                            std::find(city_slots_before->second.begin(),
                                      city_slots_before->second.end(),
                                      slot.type) == city_slots_before->second.end()) {
                            purchases++;
                        }
                    }
                }
                for (auto& [army, pos] : before) {
                    if (!game.Alive(army)) continue;
                    army_turns++;
                    if (army->X() != kNone && army->Y() * 1000 + army->X() != pos) moved++;
                }
                auto thoughts = current->ComputerPlayer() ? current->ComputerPlayer()->Thoughts()
                                                          : IComputerPlayer::ThoughtSpan{};
                for (const auto& thought : thoughts) {
                    if (thought.category == "Empire" &&
                        thought.actor_name.rfind("Muster Hub", 0) == 0) {
                        missions["MusterHubChosen"]++;
                        continue;
                    }
                    if (thought.category == "Empire" && thought.actor_name == "Standard") {
                        missions["StandardPlanted"]++;
                        continue;
                    }
                    if (thought.category == "Economy" && thought.target_name == "Standard") {
                        missions["VectorStandard"]++;
                        continue;
                    }
                    if (thought.category != "Mission") continue;
                    std::string label =
                            thought.target_name.substr(0, thought.target_name.find(' '));
                    if (thought.target_name.rfind("Capture Neutral", 0) == 0)
                        label = "CaptureNeutral";
                    missions[label]++;
                    if (label == "Siege" || label == "CaptureNeutral" || label == "Intercept") {
                        int size = std::atoi(thought.actor_name.c_str());
                        attack_sizes[std::clamp(size, 0, 8)]++;
                    }
                }
                for (auto& city : game.GetMap().Cities()) {
                    if (city.OwnedBy(current->Id()) && city.ProducingSlot() != kNone &&
                        city.ProducingSlot() < city.SlotCount()) {
                        built[city.SlotAt(city.ProducingSlot()).name]++;
                    }
                }
            } else {
                opponents::PlayTurn(game, *current, opponent, nullptr, &diag_events);
            }
        }
        for (size_t city_index = 0; city_index < game.GetMap().Cities().size(); city_index++) {
            const City& city = game.GetMap().Cities()[city_index];
            if (city.IsRazed() && !razed_before[city_index]) {
                razed_by_camp[modern[mover] ? 0 : 1]++;
                continue;
            }
            if (city.IsRazed() || !city.OwnedBy(mover) || owner_before[city_index] == mover)
                continue;
            int mover_camp = modern[mover] ? 0 : 1;
            if (owner_before[city_index] < 0 || owner_before[city_index] >= 8) {
                captured_neutral[mover_camp]++;
            } else {
                captured_from_enemy[mover_camp]++;
                lost_by_camp[modern[owner_before[city_index]] ? 0 : 1]
                            [std::min(2, garrison_before[city_index])]++;
            }
        }
        current = game::EndTurn(game);
        if (game.Turn() != last_printed && (game.Turn() % 5 == 0 || game.Turn() == 2)) {
            last_printed = game.Turn();
            PrintSides(game, modern);
        }
    }
    std::printf("\nModern armies that moved on their turn: %ld of %ld army-turns (%.0f%%)\n", moved,
                army_turns, 100.0 * moved / std::max(1L, army_turns));
    std::printf(
            "Modern economy: cities producing %ld of %ld city-turns (%.0f%%) | slot purchases %ld "
            "| "
            "side-turns ending with >= 1500 gold: %ld of %ld\n",
            producing_turns, city_turns, 100.0 * producing_turns / std::max(1L, city_turns),
            purchases, rich_turns, side_turns);
    for (int camp = 0; camp < 2; camp++) {
        std::printf(
                "%s: captured %ld neutral + %ld enemy cities (razed %ld) | lost %ld cities "
                "(garrison 0: %ld, 1: %ld, 2+: %ld)\n",
                camp == 0 ? "Modern " : "Classic", captured_neutral[camp],
                captured_from_enemy[camp], razed_by_camp[camp],
                lost_by_camp[camp][0] + lost_by_camp[camp][1] + lost_by_camp[camp][2],
                lost_by_camp[camp][0], lost_by_camp[camp][1], lost_by_camp[camp][2]);
    }
    {
        int mod_heroes = 0, mod_items = 0, mod_blessed = 0;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() < 0 || army->OwnerIndex() >= 8 || !modern[army->OwnerIndex()] ||
                army->IsDisbanded()) {
                continue;
            }
            if (army->Hero()) mod_heroes++;
            mod_items += static_cast<int>(army->Items().size());
            if (!army->Blessings().empty()) mod_blessed++;
        }
        std::printf(
                "Modern persona stats: garrisoned %.1f%% | mean gold %ld | ally %.1f%% war "
                "%.1f%% | heroes %d items %d blessed %d\n",
                100.0 * static_cast<double>(garrisoned_city_turns) /
                        static_cast<double>(std::max(1L, city_turns)),
                gold_sum / std::max(1L, side_turns),
                100.0 * static_cast<double>(ally_pairs) /
                        static_cast<double>(std::max(1L, diplo_pairs)),
                100.0 * static_cast<double>(war_pairs) /
                        static_cast<double>(std::max(1L, diplo_pairs)),
                mod_heroes, mod_items, mod_blessed);
    }
    {
        long total = 0;
        for (long val : attack_sizes) total += val;
        std::printf("Modern attack coalition sizes (siege/capture/intercept):");
        for (int idx = 1; idx <= 8; idx++) {
            std::printf(" %d:%.0f%%", idx,
                        100.0 * static_cast<double>(attack_sizes[idx]) /
                                static_cast<double>(std::max(1L, total)));
        }
        std::printf("  (%ld attacks)\n", total);
    }
    for (int camp = 0; camp < 2; camp++) {
        constexpr std::array<const char*, 3> kNames = {"1", "2-3", "4+"};
        long total = battles[camp][0] + battles[camp][1] + battles[camp][2];
        std::printf("%s battles started: %ld |", camp == 0 ? "Modern " : "Classic", total);
        for (int bucket = 0; bucket < 3; bucket++) {
            std::printf(" size %s: %ld (won %.0f%%)", kNames[bucket], battles[camp][bucket],
                        100.0 * static_cast<double>(battle_wins[camp][bucket]) /
                                static_cast<double>(std::max(1L, battles[camp][bucket])));
        }
        std::printf("\n");
    }
    for (int idx = 0; idx < 2; idx++) {
        const MissionStats& stat = mission_stats[idx];
        std::printf(
                "Modern mission battles %s: %ld | won %.2f | decided estimate %.2f | true odds "
                "%.2f | fewer armies than chosen %.2f\n",
                idx == 0 ? "on target " : "on the way", stat.count,
                static_cast<double>(stat.won) / static_cast<double>(std::max(1L, stat.count)),
                stat.decided / static_cast<double>(std::max(1L, stat.count)),
                stat.odds / static_cast<double>(std::max(1L, stat.count)),
                static_cast<double>(stat.shrunk) / static_cast<double>(std::max(1L, stat.count)));
    }
    std::printf("Decided win estimate vs result (on-target battles):\n");
    for (int bin = 0; bin < 10; bin++) {
        if (cal_n[bin] > 0) {
            std::printf("  cal %.1f-%.1f: %ld battles | true odds %.2f | won %.2f\n", bin / 10.0,
                        (bin + 1) / 10.0, cal_n[bin], cal_odds[bin] / cal_n[bin],
                        static_cast<double>(cal_won[bin]) / static_cast<double>(cal_n[bin]));
        }
    }
    std::printf("Modern missions other than holding a city (kind: count):\n");
    for (auto& [kind, count] : missions) std::printf("  %-16s %d\n", kind.c_str(), count);
    std::printf("Modern city production (unit: city-turns):\n");
    for (auto& [unit, count] : built) std::printf("  %-16s %d\n", unit.c_str(), count);
    return 0;
}
