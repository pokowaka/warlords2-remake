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
//   cpp/build-train/w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]
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

#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

const std::string kData = "original";
const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};
struct Preset {
    int hidden, diplomacy, neutral;
};
const Preset kPresets[4] = {{0, 0, 0}, {0, 1, 1}, {1, 1, 2}, {1, 0, 1}};
constexpr uint32_t kSeedBase = 600000;  // disjoint from evaluation seeds (700000+)
constexpr float kBcTemperature = 0.5F;  // the training temperature PPO uses

enum Kind { kHold = 0, kNeutralCity = 1, kEnemyCity = 2, kReinforce = 7 };

struct Example {
    std::vector<ml::MissionInVec> candidates;
    std::vector<float> log_mult;
    std::vector<int> kind;
    int label = 0;
    bool validation = false;
};

int Dist(int x0, int y0, int x1, int y1) {
    return std::max(std::abs(x0 - x1), std::abs(y0 - y1));
}

// The option that matches what the Classic AI did with the group, or -1.
//
// The Modern AI's first decision for a group is "where does a coalition led by
// the group's first army go, and how big is it"; the armies left behind get
// their own decisions afterwards. So the label follows the group's *moving*
// part: if any of its armies left, the first decision is the target they went
// for, with as many armies as went there; only when none left is it "hold".
int LabelSnapshot(Game& g, int me, const ai::modern::MissionSnapshot& sn) {
    const size_t n = sn.candidates.size();
    City* start_city = sn.start_city != kNone ? &g.map->cities[sn.start_city] : nullptr;
    auto stayed = [&](Army* a) {
        if (a->x == kNone) return false;
        if (start_city) return game::CityAt(g, a->x, a->y) == start_city;
        return a->x == sn.start_x && a->y == sn.start_y;
    };

    // The reference army: the first of the group (in the Modern AI's order) that left alive
    Army* mover = nullptr;
    int movers = 0, dead_movers = 0;
    for (std::uintptr_t id : sn.group) {
        Army* a = reinterpret_cast<Army*>(id);
        if (!g.Alive(a)) {
            dead_movers++;
            continue;
        }
        if (stayed(a)) continue;
        movers++;
        if (!mover) mover = a;
    }
    // the seed first if it moved (it leads the Modern AI's coalition)
    Army* seed = reinterpret_cast<Army*>(sn.seed);
    if (g.Alive(seed) && !stayed(seed)) mover = seed;

    auto pick_size = [&](const std::vector<size_t>& matches, int along) {
        size_t best = matches[0];
        for (size_t i : matches)
            if (std::abs(sn.stack_size[i] - along) < std::abs(sn.stack_size[best] - along))
                best = i;
        return static_cast<int>(best);
    };

    if (!mover) {
        if (dead_movers > 0) return -1;  // the ones that left all fell: target unknown
        if (!start_city) return -1;      // stood still in the field: no Modern option says that
        std::vector<size_t> holds;
        for (size_t i = 0; i < n; i++)
            if (sn.kind[i] == kHold && sn.city_index[i] == sn.start_city) holds.push_back(i);
        if (holds.empty()) return -1;
        return pick_size(holds, static_cast<int>(sn.group.size()));
    }

    const int ex = mover->x, ey = mover->y;
    if (ex == kNone) return -1;
    City* end_city = game::CityAt(g, ex, ey);
    auto city_options = [&](int city_index) {
        std::vector<size_t> out;
        for (size_t i = 0; i < n; i++)
            if (sn.city_index[i] == city_index &&
                (sn.kind[i] == kNeutralCity || sn.kind[i] == kEnemyCity ||
                 sn.kind[i] == kReinforce))
                out.push_back(i);
        return out;
    };
    std::vector<size_t> matches;
    if (end_city && end_city->owner_index == me && end_city != start_city) {
        matches = city_options(end_city->index);
    } else if (mover->ai_order == ai::core::kOrderCity && mover->ai_dest != kNone) {
        matches = city_options(mover->ai_dest);
    } else {
        // the target it ended closest to, among those it moved towards
        int best_d = 1 << 30;
        for (size_t i = 0; i < n; i++) {
            if (sn.kind[i] == kHold) continue;
            int d_end = Dist(ex, ey, sn.tx[i], sn.ty[i]);
            int d_start = Dist(sn.start_x, sn.start_y, sn.tx[i], sn.ty[i]);
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
            for (size_t i : matches)
                if (sn.kind[i] == sn.kind[first] && sn.tx[i] == sn.tx[first] &&
                    sn.ty[i] == sn.ty[first])
                    same.push_back(i);
            matches = same;
        }
    }
    if (matches.empty()) return -1;
    // coalition size: the movers that ended with the reference army
    int along = 0;
    for (std::uintptr_t id : sn.group) {
        Army* a = reinterpret_cast<Army*>(id);
        if (!g.Alive(a) || a->x == kNone || stayed(a)) continue;
        City* c = game::CityAt(g, a->x, a->y);
        if ((end_city && c == end_city) || Dist(a->x, a->y, ex, ey) <= 1) along++;
    }
    (void)movers;
    return pick_size(matches, std::max(1, along));
}

std::unique_ptr<Game> NewClassicGame(int gi, randommap::Files& files) {
    uint32_t seed = kSeedBase + static_cast<uint32_t>(gi) * 13U;
    const Preset& p = kPresets[gi % 4];
    game::NewGameOptions o;
    o.seed = seed;
    o.options = {{"hiddenMap", p.hidden},
                 {"viewEnemies", 0},
                 {"diplomacy", p.diplomacy},
                 {"neutralCities", p.neutral},
                 {"quests", 1}};
    for (int s = 0; s < 8; s++) {
        o.sides[s].computer = true;
        o.sides[s].level = 2;
        o.sides[s].card = static_cast<int>((seed + s * 5) % 9);
    }
    if (gi % 5 == 4) {  // a fifth on random maps, as in training
        Rng map_rng(static_cast<double>(seed ^ 0x5bf03635U));
        randommap::Options ro;
        ro.data_dir = kData;
        ro.rng = &map_rng;
        ro.sliders = {3, 3, 2, 3};
        ro.allies = p.diplomacy != 0;
        files = randommap::GenerateNow(ro);
        o.memory_files = &files;
        return game::NewGame(kData, "RANDOM", o);
    }
    return game::NewGame(kData, kScenarios[(gi / 4) % kScenarios.size()], o);
}

// Behaviour-cloning gradient of -log p(label) for one decision, added into grad.
double BcAccumulate(const ml::PolicyNetwork& net, const Example& ex, ml::PolicyNetwork& grad,
                    bool& correct) {
    const size_t candidate_count = ex.candidates.size();
    const float inv_t = 1.0F / kBcTemperature;
    std::vector<float> u(candidate_count), z(candidate_count), p(candidate_count);
    float max_z = -1e30F;
    int argmax = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        u[i] = net.mission_net.Forward(ex.candidates[i])[0];
        // No multiplicity prior here: greedy play (T = 0.02) ranks options by
        // utility alone, so the utilities themselves must put the label on top
        z[i] = u[i] * inv_t;
        if (z[i] > max_z) max_z = z[i];
        if (u[i] > u[argmax]) argmax = static_cast<int>(i);
    }
    float sum = 0.0F;
    for (size_t i = 0; i < candidate_count; i++) {
        p[i] = std::exp(z[i] - max_z);
        sum += p[i];
    }
    for (float& v : p) v /= sum;
    correct = (argmax == ex.label);  // greedy play takes the highest utility
    for (size_t i = 0; i < candidate_count; i++) {
        float d = inv_t * (p[i] - (i == static_cast<size_t>(ex.label) ? 1.0F : 0.0F));
        if (d != 0.0F) net.mission_net.Backward(ex.candidates[i], {d, 0.0F}, grad.mission_net);
    }
    return -std::log(std::max(1e-12F, p[ex.label]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]\n");
        return 2;
    }
    ml::PolicyNetwork net;
    if (!net.LoadJson(argv[1])) {
        std::fprintf(stderr, "w2imitate: cannot load weights '%s'\n", argv[1]);
        return 1;
    }
    const int games = argc > 3 ? std::atoi(argv[3]) : 144;
    const int turns = argc > 4 ? std::atoi(argv[4]) : 40;
    const int epochs = argc > 5 ? std::atoi(argv[5]) : 6;
    const float lr = argc > 6 ? static_cast<float>(std::atof(argv[6])) : 1e-3F;
    const unsigned threads = std::max(2U, std::thread::hardware_concurrency());
    if (games == 0) {  // just rewrite the weights in the current format
        return net.SaveJson(argv[2], 0, 0.0F) ? 0 : 1;
    }

    // ---- data: Classic games, two recorded sides each
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Example> data;
    std::mutex mu;
    std::atomic<int> next{0};
    long snapshots = 0, labelled = 0;
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; w++) {
        pool.emplace_back([&]() {
            for (int gi; (gi = next.fetch_add(1)) < games;) {
                randommap::Files files;
                auto gp = NewClassicGame(gi, files);
                Game& g = *gp;
                std::vector<int> in_use;
                for (int s = 0; s < 8; s++) {
                    if (g.map->sides[s].in_use) in_use.push_back(s);
                    g.map->sides[s].classic_ai = true;
                }
                if (in_use.empty()) continue;
                const int rec_a = in_use[static_cast<size_t>(gi) % in_use.size()],
                          rec_b = in_use[static_cast<size_t>(gi + 3) % in_use.size()];
                const bool validation = (gi % 10 == 9);
                std::vector<Example> local;
                long local_snaps = 0;
                Side* cur = game::Begin(g);
                int safety = 0;
                while (cur && !g.over && !g.won && g.turn <= turns && safety++ < turns * 12) {
                    if (cur->computer && cur->alive) {
                        const bool rec = (cur->index == rec_a || cur->index == rec_b);
                        std::vector<ai::modern::MissionSnapshot> snaps;
                        if (rec) snaps = ai::modern::SnapshotMissions(g, *cur, net);
                        ai::PlayTurn(g, *cur, nullptr);
                        for (auto& sn : snaps) {
                            local_snaps++;
                            if (sn.candidates.size() < 2) continue;
                            int label = LabelSnapshot(g, cur->index, sn);
                            if (label < 0) continue;
                            Example ex;
                            ex.candidates = std::move(sn.candidates);
                            ex.log_mult = std::move(sn.log_mult);
                            ex.kind = sn.kind;
                            ex.label = label;
                            ex.validation = validation;
                            local.push_back(std::move(ex));
                        }
                    }
                    cur = game::EndTurn(g);
                }
                std::lock_guard<std::mutex> lock(mu);
                snapshots += local_snaps;
                labelled += static_cast<long>(local.size());
                for (auto& e : local) data.push_back(std::move(e));
            }
        });
    }
    for (auto& t : pool) t.join();
    double gen_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
    auto evaluate = [&](const std::vector<size_t>& idx, double& loss, double& acc) {
        std::atomic<size_t> nx{0};
        std::vector<double> l(threads, 0.0), a(threads, 0.0);
        std::vector<std::thread> ps;
        for (unsigned w = 0; w < threads; w++) {
            ps.emplace_back([&, w]() {
                ml::PolicyNetwork scratch;
                scratch.Zero();
                for (size_t i; (i = nx.fetch_add(1)) < idx.size();) {
                    bool ok = false;
                    l[w] += BcAccumulate(net, data[idx[i]], scratch, ok);
                    a[w] += ok ? 1.0 : 0.0;
                }
            });
        }
        for (auto& t : ps) t.join();
        loss = acc = 0.0;
        for (unsigned w = 0; w < threads; w++) {
            loss += l[w];
            acc += a[w];
        }
        loss /= std::max<size_t>(1, idx.size());
        acc /= std::max<size_t>(1, idx.size());
    };
    // What the Classic AI chose, and what the network picks greedily, by target kind
    auto kind_histogram = [&](const char* label) {
        const char* names[8] = {"hold", "neutral", "enemyCity", "enemyStack",
                                "ruin", "temple",  "fog",       "reinforce"};
        std::array<long, 8> classic{}, modern{};
        for (size_t i : valid) {
            const Example& ex = data[i];
            classic[ex.kind[ex.label]]++;
            int am = 0;
            float best = -1e30F;
            for (size_t k = 0; k < ex.candidates.size(); k++) {
                float u = net.mission_net.Forward(ex.candidates[k])[0];
                if (u > best) {
                    best = u;
                    am = static_cast<int>(k);
                }
            }
            modern[ex.kind[am]]++;
        }
        std::printf("%s kinds (Classic label / network greedy):", label);
        for (int k = 0; k < 8; k++) std::printf(" %s %ld/%ld", names[k], classic[k], modern[k]);
        std::printf("\n");
    };
    kind_histogram("before");
    double vl = 0, va = 0;
    evaluate(valid, vl, va);
    std::printf("before: validation loss %.3f | greedy agreement with Classic %.1f%%\n", vl,
                100.0 * va);
    std::fflush(stdout);

    std::vector<float> params = net.Flatten();
    const size_t param_count = params.size();
    std::vector<float> m(param_count, 0.0F), v(param_count, 0.0F);
    int step = 0;
    std::mt19937 rng(7);
    const size_t batch = 512;
    for (int ep = 1; ep <= epochs; ep++) {
        std::shuffle(train.begin(), train.end(), rng);
        double train_loss = 0.0;
        for (size_t b0 = 0; b0 < train.size(); b0 += batch) {
            size_t b1 = std::min(train.size(), b0 + batch);
            std::atomic<size_t> nx{b0};
            std::vector<ml::PolicyNetwork> grads(threads);
            std::vector<double> l(threads, 0.0);
            std::vector<std::thread> ps;
            for (unsigned w = 0; w < threads; w++) {
                grads[w].Zero();
                ps.emplace_back([&, w]() {
                    for (size_t i; (i = nx.fetch_add(1)) < b1;) {
                        bool ok = false;
                        l[w] += BcAccumulate(net, data[train[i]], grads[w], ok);
                    }
                });
            }
            for (auto& t : ps) t.join();
            std::vector<float> g(param_count, 0.0F);
            for (unsigned w = 0; w < threads; w++) {
                auto fg = grads[w].Flatten();
                for (size_t d = 0; d < param_count; d++) g[d] += fg[d];
                train_loss += l[w];
            }
            const float scale = 1.0F / static_cast<float>(b1 - b0);
            step++;
            float bc1 = 1.0F - std::pow(0.9F, static_cast<float>(step)),
                  bc2 = 1.0F - std::pow(0.999F, static_cast<float>(step));
            for (size_t d = 0; d < param_count; d++) {
                float gd = g[d] * scale;
                m[d] = 0.9F * m[d] + 0.1F * gd;
                v[d] = 0.999F * v[d] + 0.001F * gd * gd;
                params[d] -= lr * (m[d] / bc1) / (std::sqrt(v[d] / bc2) + 1e-8F);
            }
            net.Unflatten(params);
        }
        evaluate(valid, vl, va);
        std::printf("epoch %d: train loss %.3f | validation loss %.3f | greedy agreement %.1f%%\n",
                    ep, train_loss / std::max<size_t>(1, train.size()), vl, 100.0 * va);
        std::fflush(stdout);
        std::string path = std::string(argv[2]);
        net.SaveJson(path, ep, 0.0F);
    }
    kind_histogram("after");
    return 0;
}
