// An all-computer game, one line per side's turn: the C++ half of the
// Lua-vs-C++ comparison (web/test/trace.lua prints the same lines).
//
//     bazel-bin/cpp/w2trace ERYTHEA 1 10 [hidden] [diplo] [save]
//
// Run from the repository root, so that original/ is found.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "warlords/ai.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"
#include "warlords/save.hpp"

using namespace w2;

/**
 * @brief Prints one trace line for the side that just played: turn, side,
 *        gold, army count, random state, a hash of every army and each
 *        city's owner.
 *
 * The format must stay byte-identical to trace.lua's: compare.sh and the
 * golden traces compare the lines verbatim.
 */
static void Trace(const Game& game, const Side& side) {
    long long hash = 0;
    for (Army* army : game.armies) {
        hash = (hash * 31 + static_cast<int64_t>(army->x == kNone ? 999 : army->x) * 7 +
                static_cast<int64_t>(army->y == kNone ? 999 : army->y) * 13 +
                static_cast<int64_t>(army->type) * 17 +
                static_cast<int64_t>(army->owner == kNone ? 15 : army->owner) * 19 +
                army->strength + static_cast<int64_t>(army->moves) * 3) %
               1000000007;
    }
    std::string owners;
    for (auto& city : game.map->cities)
        owners += city.razed                  ? 'x'
                  : city.owner_index != kNone ? static_cast<char>('0' + city.owner_index)
                                              : '.';
    printf("%d %d gold=%d armies=%d rng=%u h=%lld %s\n", game.turn, side.index, side.gold,
           static_cast<int>(game.armies.size()), game.rng.state, hash, owners.c_str());
}

/**
 * @brief Plays the game given on the command line with every side computer
 *        controlled and prints a trace line after each side's turn.
 */
int main(int argc, char** argv) {
    std::string scenario = argc > 1 ? argv[1] : "ERYTHEA";
    double seed = argc > 2 ? atof(argv[2]) : 1;
    int turns = argc > 3 ? atoi(argv[3]) : 10;
    bool hidden = false, round_trip = false, diplo = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "hidden")) hidden = true;
        // "save": write the game out and read it back after every turn, which
        // must change nothing but the route cache -- a load empties it, as the
        // original's does
        if (!strcmp(argv[i], "save")) round_trip = true;
        // "diplo": with Diplomacy on, so that sides make peace
        if (!strcmp(argv[i], "diplo")) diplo = true;
    }
    game::NewGameOptions options;
    options.seed = seed;
    if (hidden) options.options.emplace_back("hiddenMap", 1);
    if (diplo) options.options.emplace_back("diplomacy", 1);
    // RANDOM: a world made from the seed first, as cpp/test/trace.lua makes it
    if (scenario == randommap::kDir) {
        Rng rng(seed);
        randommap::Options map_options;
        map_options.data_dir = "original";
        map_options.rng = &rng;
        randommap::Install("original", randommap::GenerateNow(map_options));
    }
    auto game = game::NewGame("original", scenario, options);
    for (Side* player : game->sides) player->computer = true;
    Side* side = game::Begin(*game);
    while (side && game->turn <= turns) {
        ai::PlayTurn(*game, *side);
        Trace(*game, *side);
        if (getenv("DUMP")) {
            int number = 1;
            for (Army* army : game->armies) {
                auto show = [](int value) {
                    return value == kNone ? std::string("nil") : std::to_string(value);
                };
                printf("%d\t%s\t%s\t%d\t%s\t%d\t%d\n", number++, show(army->x).c_str(),
                       show(army->y).c_str(), army->type, show(army->owner).c_str(), army->strength,
                       army->moves);
            }
        }
        side = game::EndTurn(*game);
        if (round_trip && side) {
            game = save::Decode(save::Encode(*game), "original");
            side = game->side;
        }
    }
    return 0;
}
