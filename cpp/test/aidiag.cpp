// Modern-AI behaviour diagnostic (`w2aidiag`): plays one game with the Modern
// AI on every other side and the 1993 Classic AI on the rest, then prints
//   - every 5 turns, each side's cities, armies, strength, gold and upkeep;
//   - the share of Modern armies that moved on their own turn;
//   - the missions the Modern AI chose (by kind) and what its cities built.
// Used to see *why* a checkpoint wins or loses, which the city-share
// benchmark in w2arena does not show.
//
//   bazel-bin/cpp/w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1]
//
// Run from the repository root, so that original/ is found.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"

#include "opponents.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, scenario, "ERYTHEA", "Scenario directory name (or RANDOM).");
ABSL_FLAG(int, seed, 800001, "Random seed.");
ABSL_FLAG(int, turns, 40, "Maximum turns to play.");
ABSL_FLAG(int, hidden_map, 0, "Hidden Map option (0 or 1).");
ABSL_FLAG(int, active_sides, 8, "Number of active sides (2..8).");
ABSL_FLAG(int, crop_margin, 0, "Crop margin around active capitals (0 = full map).");
ABSL_FLAG(int, solo_seat, -1, "Solo Modern seat index (-1 = alternating 4v4).");
ABSL_FLAG(int, diplomacy, 0, "Diplomacy option (0 or 1).");
ABSL_FLAG(std::string, opponent, "classic", "Opponent personality: classic or raider.");

using namespace w2;

/**
 * @brief Prints one line per side in use: cities, armies, strength, gold and upkeep.
 * @param modern Which of the eight sides the Modern AI plays.
 */
static void PrintSides(Game& game, const bool* modern) {
    std::printf("--- turn %d\n", game.turn);
    for (int side_index = 0; side_index < 8; side_index++) {
        Side& side = game.map->sides[side_index];
        if (!side.InUse()) continue;
        int cities = 0, armies = 0, strength = 0, in_city = 0;
        for (auto& city : game.map->cities)
            if (!city.IsRazed() && city.OwnedBy(side_index)) cities++;
        for (Army* army : game.armies) {
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
            "[CROP_MARGIN] [SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider]");
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1] "
                     "[ACTIVE_SIDES] [CROP_MARGIN] "
                     "[SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider]\n");
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
    opponents::Kind opponent =
            opponent_name == "raider" ? opponents::Kind::kRaider : opponents::Kind::kClassic;
    ai::ml::PolicyNetwork net;
    if (absl::Status status = net.LoadJson(weights); !status.ok()) {
        std::fprintf(stderr, "w2aidiag: %s\n", std::string(status.message()).c_str());
        return 1;
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
                scenario.c_str(), seed, active_sides, crop_margin, game.map->cities.size(),
                game.map->active_x0, game.map->active_x1, game.map->active_y0, game.map->active_y1);
    bool modern[8] = {};
    int seat = 0;
    for (int side_index = 0; side_index < 8; side_index++) {
        if (!game.map->sides[side_index].InUse()) continue;
        modern[side_index] = (solo_seat >= 0) ? (seat == solo_seat) : (seat % 2 == 0);
        seat++;
        if (modern[side_index]) {
            ai::SetSideClassicAi(game, game.map->sides[side_index], false);
        } else {
            opponents::InstallOpponent(game, game.map->sides[side_index], opponent);
        }
    }

    std::map<std::string, int> missions, built;
    long army_turns = 0, moved = 0;
    long city_turns = 0, producing_turns = 0, purchases = 0, rich_turns = 0, side_turns = 0;
    int last_printed = 0;
    Side* current = game::Begin(game);
    // City changes of hands: losses by camp of the loser, bucketed by the
    // garrison (armies in the city) the loser had when the attacker's turn began.
    long lost_by_camp[2][3] = {}, captured_from_enemy[2] = {}, captured_neutral[2] = {};
    auto garrison_of = [&](const City& city) {
        int count = 0;
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                for (Army* army : game::ArmiesAt(game, city.X() + dx, city.Y() + dy))
                    if (army->Owner() == city.Owner()) count++;
        return count;
    };
    while (current && !game.over && !game.won && game.turn <= turns) {
        std::vector<int> owner_before(game.map->cities.size()),
                garrison_before(game.map->cities.size());
        for (size_t city_index = 0; city_index < game.map->cities.size(); city_index++) {
            owner_before[city_index] = game.map->cities[city_index].OwnerIndex();
            garrison_before[city_index] = !game.map->cities[city_index].IsNeutral()
                                                  ? garrison_of(game.map->cities[city_index])
                                                  : 0;
        }
        const int mover = current->Index();
        if (current->IsAlive()) {
            if (modern[current->Index()]) {
                std::map<Army*, int> before;
                for (Army* army : game.armies)
                    if (army->OwnedBy(current->Id()) && army->X() != kNone)
                        before[army] = army->Y() * 1000 + army->X();
                std::map<int, std::vector<int>> slots_before;
                for (auto& city : game.map->cities) {
                    if (!city.OwnedBy(current->Id())) continue;
                    for (const auto& slot : city.Slots())
                        slots_before[city.Index()].push_back(slot.type);
                }
                ai::modern::PlayTurn(game, *current, nullptr, &net, nullptr);
                side_turns++;
                if (current->Gold() >= 1500) rich_turns++;
                for (auto& city : game.map->cities) {
                    if (!city.OwnedBy(current->Id()) || city.IsRazed()) continue;
                    city_turns++;
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
                    if (thought.category != "Mission") continue;
                    std::string label =
                            thought.target_name.substr(0, thought.target_name.find(' '));
                    if (thought.target_name.rfind("Capture Neutral", 0) == 0)
                        label = "CaptureNeutral";
                    missions[label]++;
                }
                for (auto& city : game.map->cities) {
                    if (city.OwnedBy(current->Id()) && city.ProducingSlot() != kNone &&
                        city.ProducingSlot() < city.SlotCount()) {
                        built[city.SlotAt(city.ProducingSlot()).name]++;
                    }
                }
            } else {
                opponents::PlayTurn(game, *current, opponent);
            }
        }
        for (size_t city_index = 0; city_index < game.map->cities.size(); city_index++) {
            const City& city = game.map->cities[city_index];
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
        if (game.turn != last_printed && (game.turn % 5 == 0 || game.turn == 2)) {
            last_printed = game.turn;
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
                "%s: captured %ld neutral + %ld enemy cities | lost %ld cities (garrison 0: %ld, "
                "1: %ld, 2+: %ld)\n",
                camp == 0 ? "Modern " : "Classic", captured_neutral[camp],
                captured_from_enemy[camp],
                lost_by_camp[camp][0] + lost_by_camp[camp][1] + lost_by_camp[camp][2],
                lost_by_camp[camp][0], lost_by_camp[camp][1], lost_by_camp[camp][2]);
    }
    std::printf("Modern missions other than holding a city (kind: count):\n");
    for (auto& [kind, count] : missions) std::printf("  %-16s %d\n", kind.c_str(), count);
    std::printf("Modern city production (unit: city-turns):\n");
    for (auto& [unit, count] : built) std::printf("  %-16s %d\n", unit.c_str(), count);
    return 0;
}
