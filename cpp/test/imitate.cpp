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
constexpr float kBcTemperature = 0.5f;  // the training temperature PPO uses

enum Kind { HOLD = 0, NEUTRAL_CITY = 1, ENEMY_CITY = 2, REINFORCE = 7 };

struct Example {
    std::vector<ml::MissionInVec> candidates;
    std::vector<float> logMult;
    std::vector<int> kind;
    int label = 0;
    bool validation = false;
};

int dist(int x0, int y0, int x1, int y1) {
    return std::max(std::abs(x0 - x1), std::abs(y0 - y1));
}

// The option that matches what the Classic AI did with the group, or -1.
//
// The Modern AI's first decision for a group is "where does a coalition led by
// the group's first army go, and how big is it"; the armies left behind get
// their own decisions afterwards. So the label follows the group's *moving*
// part: if any of its armies left, the first decision is the target they went
// for, with as many armies as went there; only when none left is it "hold".
int labelSnapshot(Game& g, int me, const ai::modern::MissionSnapshot& sn) {
    const size_t n = sn.candidates.size();
    City* startCity = sn.startCity != NONE ? &g.map->cities[sn.startCity] : nullptr;
    auto stayed = [&](Army* a) {
        if (a->x == NONE) return false;
        if (startCity) return game::cityAt(g, a->x, a->y) == startCity;
        return a->x == sn.startX && a->y == sn.startY;
    };

    // The reference army: the first of the group (in the Modern AI's order) that left alive
    Army* mover = nullptr;
    int movers = 0, deadMovers = 0;
    for (std::uintptr_t id : sn.group) {
        Army* a = reinterpret_cast<Army*>(id);
        if (!g.alive(a)) {
            deadMovers++;
            continue;
        }
        if (stayed(a)) continue;
        movers++;
        if (!mover) mover = a;
    }
    // the seed first if it moved (it leads the Modern AI's coalition)
    Army* seed = reinterpret_cast<Army*>(sn.seed);
    if (g.alive(seed) && !stayed(seed)) mover = seed;

    auto pickSize = [&](const std::vector<size_t>& matches, int along) {
        size_t best = matches[0];
        for (size_t i : matches)
            if (std::abs(sn.stackSize[i] - along) < std::abs(sn.stackSize[best] - along)) best = i;
        return (int)best;
    };

    if (!mover) {
        if (deadMovers > 0) return -1;  // the ones that left all fell: target unknown
        if (!startCity) return -1;      // stood still in the field: no Modern option says that
        std::vector<size_t> holds;
        for (size_t i = 0; i < n; i++)
            if (sn.kind[i] == HOLD && sn.cityIndex[i] == sn.startCity) holds.push_back(i);
        if (holds.empty()) return -1;
        return pickSize(holds, (int)sn.group.size());
    }

    const int ex = mover->x, ey = mover->y;
    if (ex == NONE) return -1;
    City* endCity = game::cityAt(g, ex, ey);
    auto cityOptions = [&](int cityIndex) {
        std::vector<size_t> out;
        for (size_t i = 0; i < n; i++)
            if (sn.cityIndex[i] == cityIndex &&
                (sn.kind[i] == NEUTRAL_CITY || sn.kind[i] == ENEMY_CITY || sn.kind[i] == REINFORCE))
                out.push_back(i);
        return out;
    };
    std::vector<size_t> matches;
    if (endCity && endCity->ownerIndex == me && endCity != startCity) {
        matches = cityOptions(endCity->index);
    } else if (mover->aiOrder == ai::core::ORDER_CITY && mover->aiDest != NONE) {
        matches = cityOptions(mover->aiDest);
    } else {
        // the target it ended closest to, among those it moved towards
        int bestD = 1 << 30;
        for (size_t i = 0; i < n; i++) {
            if (sn.kind[i] == HOLD) continue;
            int dEnd = dist(ex, ey, sn.tx[i], sn.ty[i]);
            int dStart = dist(sn.startX, sn.startY, sn.tx[i], sn.ty[i]);
            if (dEnd >= dStart) continue;
            if (dEnd < bestD) {
                bestD = dEnd;
                matches.clear();
            }
            if (dEnd == bestD) matches.push_back(i);
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
        if (!g.alive(a) || a->x == NONE || stayed(a)) continue;
        City* c = game::cityAt(g, a->x, a->y);
        if ((endCity && c == endCity) || dist(a->x, a->y, ex, ey) <= 1) along++;
    }
    (void)movers;
    return pickSize(matches, std::max(1, along));
}

std::unique_ptr<Game> newClassicGame(int gi, randommap::Files& files) {
    uint32_t seed = kSeedBase + (uint32_t)gi * 13u;
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
        o.sides[s].card = (int)((seed + s * 5) % 9);
    }
    if (gi % 5 == 4) {  // a fifth on random maps, as in training
        Rng mapRng((double)(seed ^ 0x5bf03635u));
        randommap::Options ro;
        ro.dataDir = kData;
        ro.rng = &mapRng;
        ro.sliders = {3, 3, 2, 3};
        ro.allies = p.diplomacy != 0;
        files = randommap::generateNow(ro);
        o.memoryFiles = &files;
        return game::newGame(kData, "RANDOM", o);
    }
    return game::newGame(kData, kScenarios[(gi / 4) % kScenarios.size()], o);
}

// Behaviour-cloning gradient of -log p(label) for one decision, added into grad.
double bcAccumulate(const ml::PolicyNetwork& net, const Example& ex, ml::PolicyNetwork& grad,
                    bool& correct) {
    const size_t K = ex.candidates.size();
    const float invT = 1.0f / kBcTemperature;
    std::vector<float> u(K), z(K), p(K);
    float maxZ = -1e30f;
    int argmax = 0;
    for (size_t i = 0; i < K; i++) {
        u[i] = net.missionNet.forward(ex.candidates[i])[0];
        // No multiplicity prior here: greedy play (T = 0.02) ranks options by
        // utility alone, so the utilities themselves must put the label on top
        z[i] = u[i] * invT;
        if (z[i] > maxZ) maxZ = z[i];
        if (u[i] > u[argmax]) argmax = (int)i;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < K; i++) {
        p[i] = std::exp(z[i] - maxZ);
        sum += p[i];
    }
    for (float& v : p) v /= sum;
    correct = (argmax == ex.label);  // greedy play takes the highest utility
    for (size_t i = 0; i < K; i++) {
        float d = invT * (p[i] - (i == (size_t)ex.label ? 1.0f : 0.0f));
        if (d != 0.0f) net.missionNet.backward(ex.candidates[i], {d, 0.0f}, grad.missionNet);
    }
    return -std::log(std::max(1e-12f, p[ex.label]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: w2imitate IN.json OUT.json [GAMES] [TURNS] [EPOCHS] [LR]\n");
        return 2;
    }
    ml::PolicyNetwork net;
    if (!net.loadJson(argv[1])) {
        std::fprintf(stderr, "w2imitate: cannot load weights '%s'\n", argv[1]);
        return 1;
    }
    const int games = argc > 3 ? std::atoi(argv[3]) : 144;
    const int turns = argc > 4 ? std::atoi(argv[4]) : 40;
    const int epochs = argc > 5 ? std::atoi(argv[5]) : 6;
    const float lr = argc > 6 ? (float)std::atof(argv[6]) : 1e-3f;
    const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    if (games == 0) {  // just rewrite the weights in the current format
        return net.saveJson(argv[2], 0, 0.0f) ? 0 : 1;
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
                auto gp = newClassicGame(gi, files);
                Game& g = *gp;
                std::vector<int> inUse;
                for (int s = 0; s < 8; s++) {
                    if (g.map->sides[s].inUse) inUse.push_back(s);
                    g.map->sides[s].classicAi = true;
                }
                if (inUse.empty()) continue;
                const int recA = inUse[(size_t)gi % inUse.size()],
                          recB = inUse[(size_t)(gi + 3) % inUse.size()];
                const bool validation = (gi % 10 == 9);
                std::vector<Example> local;
                long localSnaps = 0;
                Side* cur = game::begin(g);
                int safety = 0;
                while (cur && !g.over && !g.won && g.turn <= turns && safety++ < turns * 12) {
                    if (cur->computer && cur->alive) {
                        const bool rec = (cur->index == recA || cur->index == recB);
                        std::vector<ai::modern::MissionSnapshot> snaps;
                        if (rec) snaps = ai::modern::snapshotMissions(g, *cur, net);
                        ai::playTurn(g, *cur, nullptr);
                        for (auto& sn : snaps) {
                            localSnaps++;
                            if (sn.candidates.size() < 2) continue;
                            int label = labelSnapshot(g, cur->index, sn);
                            if (label < 0) continue;
                            Example ex;
                            ex.candidates = std::move(sn.candidates);
                            ex.logMult = std::move(sn.logMult);
                            ex.kind = sn.kind;
                            ex.label = label;
                            ex.validation = validation;
                            local.push_back(std::move(ex));
                        }
                    }
                    cur = game::endTurn(g);
                }
                std::lock_guard<std::mutex> lock(mu);
                snapshots += localSnaps;
                labelled += (long)local.size();
                for (auto& e : local) data.push_back(std::move(e));
            }
        });
    }
    for (auto& t : pool) t.join();
    double genSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<size_t> train, valid;
    for (size_t i = 0; i < data.size(); i++) (data[i].validation ? valid : train).push_back(i);
    std::printf(
            "=== w2imitate | %d Classic games x %d turns | %ld group decisions, %ld labelled "
            "(%.0f%%) | "
            "%zu train / %zu validation | %.0f s ===\n",
            games, turns, snapshots, labelled, 100.0 * labelled / std::max(1L, snapshots),
            train.size(), valid.size(), genSec);
    std::fflush(stdout);

    // ---- behaviour cloning on MissionNet
    auto evaluate = [&](const std::vector<size_t>& idx, double& loss, double& acc) {
        std::atomic<size_t> nx{0};
        std::vector<double> l(threads, 0.0), a(threads, 0.0);
        std::vector<std::thread> ps;
        for (unsigned w = 0; w < threads; w++) {
            ps.emplace_back([&, w]() {
                ml::PolicyNetwork scratch;
                scratch.zero();
                for (size_t i; (i = nx.fetch_add(1)) < idx.size();) {
                    bool ok = false;
                    l[w] += bcAccumulate(net, data[idx[i]], scratch, ok);
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
    auto kindHistogram = [&](const char* label) {
        const char* names[8] = {"hold", "neutral", "enemyCity", "enemyStack",
                                "ruin", "temple",  "fog",       "reinforce"};
        std::array<long, 8> classic{}, modern{};
        for (size_t i : valid) {
            const Example& ex = data[i];
            classic[ex.kind[ex.label]]++;
            int am = 0;
            float best = -1e30f;
            for (size_t k = 0; k < ex.candidates.size(); k++) {
                float u = net.missionNet.forward(ex.candidates[k])[0];
                if (u > best) {
                    best = u;
                    am = (int)k;
                }
            }
            modern[ex.kind[am]]++;
        }
        std::printf("%s kinds (Classic label / network greedy):", label);
        for (int k = 0; k < 8; k++) std::printf(" %s %ld/%ld", names[k], classic[k], modern[k]);
        std::printf("\n");
    };
    kindHistogram("before");
    double vl = 0, va = 0;
    evaluate(valid, vl, va);
    std::printf("before: validation loss %.3f | greedy agreement with Classic %.1f%%\n", vl,
                100.0 * va);
    std::fflush(stdout);

    std::vector<float> params = net.flatten();
    const size_t D = params.size();
    std::vector<float> m(D, 0.0f), v(D, 0.0f);
    int step = 0;
    std::mt19937 rng(7);
    const size_t batch = 512;
    for (int ep = 1; ep <= epochs; ep++) {
        std::shuffle(train.begin(), train.end(), rng);
        double trainLoss = 0.0;
        for (size_t b0 = 0; b0 < train.size(); b0 += batch) {
            size_t b1 = std::min(train.size(), b0 + batch);
            std::atomic<size_t> nx{b0};
            std::vector<ml::PolicyNetwork> grads(threads);
            std::vector<double> l(threads, 0.0);
            std::vector<std::thread> ps;
            for (unsigned w = 0; w < threads; w++) {
                grads[w].zero();
                ps.emplace_back([&, w]() {
                    for (size_t i; (i = nx.fetch_add(1)) < b1;) {
                        bool ok = false;
                        l[w] += bcAccumulate(net, data[train[i]], grads[w], ok);
                    }
                });
            }
            for (auto& t : ps) t.join();
            std::vector<float> g(D, 0.0f);
            for (unsigned w = 0; w < threads; w++) {
                auto fg = grads[w].flatten();
                for (size_t d = 0; d < D; d++) g[d] += fg[d];
                trainLoss += l[w];
            }
            const float scale = 1.0f / (float)(b1 - b0);
            step++;
            float bc1 = 1.0f - std::pow(0.9f, (float)step),
                  bc2 = 1.0f - std::pow(0.999f, (float)step);
            for (size_t d = 0; d < D; d++) {
                float gd = g[d] * scale;
                m[d] = 0.9f * m[d] + 0.1f * gd;
                v[d] = 0.999f * v[d] + 0.001f * gd * gd;
                params[d] -= lr * (m[d] / bc1) / (std::sqrt(v[d] / bc2) + 1e-8f);
            }
            net.unflatten(params);
        }
        evaluate(valid, vl, va);
        std::printf("epoch %d: train loss %.3f | validation loss %.3f | greedy agreement %.1f%%\n",
                    ep, trainLoss / std::max<size_t>(1, train.size()), vl, 100.0 * va);
        std::fflush(stdout);
        std::string path = std::string(argv[2]);
        net.saveJson(path, ep, 0.0f);
    }
    kindHistogram("after");
    return 0;
}
