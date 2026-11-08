// An all-computer game, one line per side's turn: the C++ half of the
// Lua-vs-C++ comparison (web/test/trace.lua prints the same lines).
//
//     cpp/build/w2trace ERYTHEA 1 10 [hidden] [diplo] [save]
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

static void Trace(const Game& g, const Side& side) {
    long long h = 0;
    for (Army* a : g.armies) {
        h = (h * 31 + static_cast<int64_t>(a->x == kNone ? 999 : a->x) * 7 +
             static_cast<int64_t>(a->y == kNone ? 999 : a->y) * 13 +
             static_cast<int64_t>(a->type) * 17 +
             static_cast<int64_t>(a->owner == kNone ? 15 : a->owner) * 19 + a->strength +
             static_cast<int64_t>(a->moves) * 3) %
            1000000007;
    }
    std::string owners;
    for (auto& c : g.map->cities)
        owners += c.razed                  ? 'x'
                  : c.owner_index != kNone ? static_cast<char>('0' + c.owner_index)
                                           : '.';
    printf("%d %d gold=%d armies=%d rng=%u h=%lld %s\n", g.turn, side.index, side.gold,
           static_cast<int>(g.armies.size()), g.rng.state, h, owners.c_str());
}

int main(int argc, char** argv) {
    std::string scen = argc > 1 ? argv[1] : "ERYTHEA";
    double seed = argc > 2 ? atof(argv[2]) : 1;
    int turns = argc > 3 ? atoi(argv[3]) : 10;
    bool hidden = false, round_trip = false, diplo = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "hidden")) hidden = true;
        // "save": write the game out and read it back after every turn, which
        // must change nothing but the route cache -- a load empties it, as the
        // original's does (623c:0dff)
        if (!strcmp(argv[i], "save")) round_trip = true;
        // "diplo": with Diplomacy on, so that sides make peace
        if (!strcmp(argv[i], "diplo")) diplo = true;
    }
    game::NewGameOptions opts;
    opts.seed = seed;
    if (hidden) opts.options.emplace_back("hiddenMap", 1);
    if (diplo) opts.options.emplace_back("diplomacy", 1);
    // RANDOM: a world made from the seed first, as cpp/test/trace.lua makes it
    if (scen == randommap::kDir) {
        Rng r(seed);
        randommap::Options ro;
        ro.data_dir = "original";
        ro.rng = &r;
        randommap::Install("original", randommap::GenerateNow(ro));
    }
    auto g = game::NewGame("original", scen, opts);
    for (Side* s : g->sides) s->computer = true;
    Side* side = game::Begin(*g);
    while (side && g->turn <= turns) {
        ai::PlayTurn(*g, *side);
        Trace(*g, *side);
        if (getenv("DUMP")) {
            int i = 1;
            for (Army* a : g->armies) {
                auto v = [](int n) { return n == kNone ? std::string("nil") : std::to_string(n); };
                printf("%d\t%s\t%s\t%d\t%s\t%d\t%d\n", i++, v(a->x).c_str(), v(a->y).c_str(),
                       a->type, v(a->owner).c_str(), a->strength, a->moves);
            }
        }
        side = game::EndTurn(*g);
        if (round_trip && side) {
            g = save::Decode(save::Encode(*g), "original");
            side = g->side;
        }
    }
    return 0;
}
