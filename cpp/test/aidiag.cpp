// Modern-AI behaviour diagnostic (`w2aidiag`): plays one game with the Modern
// AI on every other side and the 1993 Classic AI on the rest, then prints
//   - every 5 turns, each side's cities, armies, strength, gold and upkeep;
//   - the share of Modern armies that moved on their own turn;
//   - the missions the Modern AI chose (by kind) and what its cities built.
// Used to see *why* a checkpoint wins or loses, which the city-share
// benchmark in w2arena does not show.
//
//   cpp/build/w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1]
//
// Run from the repository root, so that original/ is found.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "opponents.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

using namespace w2;

static void PrintSides(Game& g, const bool* modern) {
    std::printf("--- turn %d\n", g.turn);
    for (int s = 0; s < 8; s++) {
        Side& sd = g.map->sides[s];
        if (!sd.in_use) continue;
        int cities = 0, armies = 0, str = 0, in_city = 0;
        for (auto& c : g.map->cities)
            if (!c.razed && c.owner_index == s) cities++;
        for (Army* a : g.armies) {
            if (a->owner != s || a->x == kNone) continue;
            armies++;
            str += a->strength;
            if (game::CityAt(g, a->x, a->y)) in_city++;
        }
        std::printf(
                "  side %d %s %s cities=%2d armies=%3d strength=%4d in-city=%3d gold=%5d "
                "income=%4d upkeep=%4d\n",
                s, modern[s] ? "MODERN " : "CLASSIC", sd.alive ? "     " : "(dead)", cities, armies,
                str, in_city, sd.gold, game::Income(g, sd), game::Upkeep(g, sd));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: w2aidiag WEIGHTS [SCENARIO] [SEED] [TURNS] [HIDDEN_MAP 0|1] "
                     "[ACTIVE_SIDES] [CROP_MARGIN] "
                     "[SOLO_SEAT -1|0..7] [DIPLOMACY 0|1] [OPPONENT classic|raider]\n");
        return 2;
    }
    std::string weights = argv[1];
    std::string scen = argc > 2 ? argv[2] : "ERYTHEA";
    int seed = argc > 3 ? std::atoi(argv[3]) : 800001;
    int turns = argc > 4 ? std::atoi(argv[4]) : 40;
    int hidden = argc > 5 ? std::atoi(argv[5]) : 0;
    int active_sides = argc > 6 ? std::atoi(argv[6]) : 8;
    int crop_margin = argc > 7 ? std::atoi(argv[7]) : 0;
    int solo_seat = argc > 8 ? std::atoi(argv[8]) : -1;
    int diplo = argc > 9 ? std::atoi(argv[9]) : 0;
    opponents::Kind opp = (argc > 10 && std::string(argv[10]) == "raider")
                                  ? opponents::Kind::kRaider
                                  : opponents::Kind::kClassic;
    ai::ml::PolicyNetwork net;
    if (!net.LoadJson(weights)) {
        std::fprintf(stderr,
                     "w2aidiag: cannot load weights '%s' (missing, or trained for different input "
                     "sizes)\n",
                     weights.c_str());
        return 1;
    }
    game::NewGameOptions o;
    o.seed = seed;
    o.active_sides = active_sides;
    o.crop_margin = crop_margin;
    o.options = {{"hiddenMap", hidden},
                 {"viewEnemies", 0},
                 {"diplomacy", diplo},
                 {"neutralCities", 1},
                 {"quests", 1}};
    for (int i = 0; i < 8; i++) {
        o.sides[i].computer = true;
        o.sides[i].level = 2;
        o.sides[i].card = (seed + i * 5) % 9;
    }
    randommap::Files rfiles;
    if (scen == "RANDOM") {
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options ro;
        ro.data_dir = "original";
        ro.rng = &map_rng;
        ro.sliders = {3, 3, 2, 3};
        rfiles = randommap::GenerateNow(ro);
        o.memory_files = &rfiles;
    }
    auto gp = game::NewGame("original", scen, o);
    Game& g = *gp;
    std::printf("=== %s seed=%d sides=%d crop=%d cities=%zu box=[%d..%d]x[%d..%d] ===\n",
                scen.c_str(), seed, active_sides, crop_margin, g.map->cities.size(),
                g.map->active_x0, g.map->active_x1, g.map->active_y0, g.map->active_y1);
    bool modern[8] = {};
    int idx = 0;
    for (int s = 0; s < 8; s++) {
        if (!g.map->sides[s].in_use) continue;
        modern[s] = (solo_seat >= 0) ? (idx == solo_seat) : (idx % 2 == 0);
        idx++;
        g.map->sides[s].classic_ai = !modern[s];
    }

    std::map<std::string, int> missions, built;
    long army_turns = 0, moved = 0;
    long city_turns = 0, producing_turns = 0, purchases = 0, rich_turns = 0, side_turns = 0;
    int last_printed = 0;
    Side* cur = game::Begin(g);
    // City changes of hands: losses by camp of the loser, bucketed by the
    // garrison (armies in the city) the loser had when the attacker's turn began.
    long lost_by_camp[2][3] = {}, captured_from_enemy[2] = {}, captured_neutral[2] = {};
    auto garrison_of = [&](const City& c) {
        int n = 0;
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++)
                for (Army* a : game::ArmiesAt(g, c.x + dx, c.y + dy))
                    if (a->owner == c.owner_index) n++;
        return n;
    };
    while (cur && !g.over && !g.won && g.turn <= turns) {
        std::vector<int> owner_before(g.map->cities.size()), gar_before(g.map->cities.size());
        for (size_t ci = 0; ci < g.map->cities.size(); ci++) {
            owner_before[ci] = g.map->cities[ci].owner_index;
            gar_before[ci] =
                    g.map->cities[ci].owner_index >= 0 ? garrison_of(g.map->cities[ci]) : 0;
        }
        const int mover = cur->index;
        if (cur->alive) {
            if (modern[cur->index]) {
                std::map<Army*, int> before;
                for (Army* a : g.armies)
                    if (a->owner == cur->index && a->x != kNone) before[a] = a->y * 1000 + a->x;
                std::map<int, std::vector<int>> slots_before;
                for (auto& c : g.map->cities) {
                    if (c.owner_index != cur->index) continue;
                    for (const auto& sl : c.slots) slots_before[c.index].push_back(sl.type);
                }
                ai::modern::PlayTurn(g, *cur, nullptr, &net, nullptr);
                side_turns++;
                if (cur->gold >= 1500) rich_turns++;
                for (auto& c : g.map->cities) {
                    if (c.owner_index != cur->index || c.razed) continue;
                    city_turns++;
                    if (game::Producing(c)) producing_turns++;
                    auto it = slots_before.find(c.index);
                    for (const auto& sl : c.slots) {
                        if (it != slots_before.end() &&
                            std::find(it->second.begin(), it->second.end(), sl.type) ==
                                    it->second.end()) {
                            purchases++;
                        }
                    }
                }
                for (auto& [a, pos] : before) {
                    if (!g.Alive(a)) continue;
                    army_turns++;
                    if (a->x != kNone && a->y * 1000 + a->x != pos) moved++;
                }
                for (auto& t : cur->ai_thoughts) {
                    if (t.category != "Mission") continue;
                    std::string label = t.target_name.substr(0, t.target_name.find(' '));
                    if (t.target_name.rfind("Capture Neutral", 0) == 0) label = "CaptureNeutral";
                    missions[label]++;
                }
                for (auto& c : g.map->cities) {
                    if (c.owner_index == cur->index && c.producing != kNone &&
                        c.producing < static_cast<int>(c.slots.size())) {
                        built[c.slots[c.producing].name]++;
                    }
                }
            } else {
                opponents::PlayTurn(g, *cur, opp);
            }
        }
        for (size_t ci = 0; ci < g.map->cities.size(); ci++) {
            const City& c = g.map->cities[ci];
            if (c.razed || c.owner_index != mover || owner_before[ci] == mover) continue;
            int mover_camp = modern[mover] ? 0 : 1;
            if (owner_before[ci] < 0 || owner_before[ci] >= 8) {
                captured_neutral[mover_camp]++;
            } else {
                captured_from_enemy[mover_camp]++;
                lost_by_camp[modern[owner_before[ci]] ? 0 : 1][std::min(2, gar_before[ci])]++;
            }
        }
        cur = game::EndTurn(g);
        if (g.turn != last_printed && (g.turn % 5 == 0 || g.turn == 2)) {
            last_printed = g.turn;
            PrintSides(g, modern);
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
    for (auto& [k, v] : missions) std::printf("  %-16s %d\n", k.c_str(), v);
    std::printf("Modern city production (unit: city-turns):\n");
    for (auto& [k, v] : built) std::printf("  %-16s %d\n", k.c_str(), v);
    return 0;
}
