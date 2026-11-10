// An all-computer game, one line per side's turn: used for golden trace
// regression testing (//cpp:golden_test).
//
//     bazel-bin/cpp/w2trace ERYTHEA 1 10 [hidden] [diplo] [save]
//
// Run from the repository root, so that original/ is found.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "absl/flags/flag.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"
#include "warlords/save.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(bool, dump, false, "Print every army's coordinates and stats after each turn.");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

using namespace w2;

/**
 * @brief Prints one trace line for the side that just played: turn, side,
 *        gold, army count, random state, a hash of every army and each
 *        city's owner.
 *
 * The format must stay consistent: golden traces compare the lines verbatim.
 */
static void Trace(const Game& game, const Side& side) {
    long long hash = 0;
    for (Army* army : game.Armies()) {
        hash = (hash * 31 + static_cast<int64_t>(army->X() == kNone ? 999 : army->X()) * 7 +
                static_cast<int64_t>(army->Y() == kNone ? 999 : army->Y()) * 13 +
                static_cast<int64_t>(army->TypeIndex()) * 17 +
                static_cast<int64_t>(army->OwnerIndex() == kNone ? 15 : army->OwnerIndex()) * 19 +
                army->Strength() + static_cast<int64_t>(army->Moves()) * 3) %
               1000000007;
    }
    std::string owners;
    for (const auto& city : game.GetMap().Cities()) {
        owners += city.IsRazed()      ? 'x'
                  : !city.IsNeutral() ? static_cast<char>('0' + city.OwnerIndex())
                                      : '.';
    }
    printf("%d %d gold=%d armies=%d rng=%u h=%lld %s\n", game.Turn(), side.Index(), side.Gold(),
           static_cast<int>(game.Armies().size()), game.GetRng().State(), hash, owners.c_str());
}

/**
 * @brief Plays the game given on the command line with every side computer
 *        controlled and prints a trace line after each side's turn.
 */
int main(int argc, char** argv) {
    auto args =
            ParseCliFlags(argc, argv, "w2trace [SCENARIO] [SEED] [TURNS] [hidden] [diplo] [save]");
    std::string data_dir = absl::GetFlag(FLAGS_data);
    std::string scenario = !args.empty() ? args[0] : "ERYTHEA";
    double seed = args.size() > 1 ? std::strtod(args[1].c_str(), nullptr) : 1.0;
    int turns = args.size() > 2 ? static_cast<int>(ToInt(args[2]).value_or(10)) : 10;
    bool hidden = false;
    bool round_trip = false;
    bool diplo = false;
    for (size_t i = 3; i < args.size(); i++) {
        if (args[i] == "hidden") hidden = true;
        // "save": write the game out and read it back after every turn, which
        // must change nothing but the route cache -- a load empties it, as the
        // original's does
        if (args[i] == "save") round_trip = true;
        // "diplo": with Diplomacy on, so that sides make peace
        if (args[i] == "diplo") diplo = true;
    }
    game::NewGameOptions options;
    options.seed = seed;
    if (hidden) options.options.emplace_back("hiddenMap", 1);
    if (diplo) options.options.emplace_back("diplomacy", 1);
    randommap::Files generated_files;
    // RANDOM: a world made from the seed first, as cpp/test/trace.lua makes it
    if (scenario == randommap::kDir) {
        Rng rng(seed);
        randommap::Options map_options;
        map_options.data_dir = data_dir;
        map_options.rng = &rng;
        auto generated = randommap::GenerateNow(map_options);
        if (!generated.ok()) {
            fprintf(stderr, "w2trace: %s\n", std::string(generated.status().message()).c_str());
            return 1;
        }
        generated_files = std::move(*generated);
        options.memory_files = &generated_files;
    }
    auto created_game = game::NewGame(data_dir, scenario, options);
    if (!created_game.ok()) {
        fprintf(stderr, "w2trace: %s\n", std::string(created_game.status().message()).c_str());
        return 1;
    }
    std::unique_ptr<Game> game = std::move(*created_game);
    for (Side* player : game->Sides()) player->SetComputerControlled(true);
    Side* side = game::Begin(*game);
    bool dump_armies = absl::GetFlag(FLAGS_dump) || (std::getenv("DUMP") != nullptr);
    while (side && game->Turn() <= turns) {
        ai::PlayTurn(*game, *side);
        Trace(*game, *side);
        if (dump_armies) {
            int number = 1;
            for (Army* army : game->Armies()) {
                auto show = [](int value) {
                    return value == kNone ? std::string("nil") : std::to_string(value);
                };
                printf("%d\t%s\t%s\t%d\t%s\t%d\t%d\n", number++, show(army->X()).c_str(),
                       show(army->Y()).c_str(), army->TypeIndex(), show(army->OwnerIndex()).c_str(),
                       army->Strength(), army->Moves());
            }
        }
        side = game::EndTurn(*game);
        if (round_trip && side) {
            auto reloaded = save::Decode(save::Encode(*game), data_dir);
            if (!reloaded.ok()) {
                fprintf(stderr, "w2trace: %s\n", std::string(reloaded.status().message()).c_str());
                return 1;
            }
            game = std::move(*reloaded);
            side = game->CurrentSide();
        }
    }
    return 0;
}
