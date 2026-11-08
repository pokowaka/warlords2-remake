// Lookahead search for the Modern AI (`w2search`): does thinking ahead beat
// the network's instinct?
//
// A Modern side's turn is normally one pass of the policy network. Here, at a
// searched turn, the side instead considers K+1 *candidate turns*: the greedy
// turn (what it would play anyway) and K turns sampled from the same policy at
// a higher temperature. Each candidate is copied R times (different dice) and
// played on for H rounds by everyone's normal AI; the candidate whose copies end
// with the best average empire score is the turn actually played.
//
//   probe: play benchmark games normally, and at a few turns measure how often
//          a sampled candidate beats the greedy one, and by how much (D2).
//   play:  every Modern turn is searched; reports the benchmark city share, to
//          be compared with the same seeds played without search.
//
//   cpp/build-train/w2search WEIGHTS [probe|play] [SEEDS] [K] [R] [H] [TURNS] [peek|honest|fair]
//
// Copies are made with save::encode/decode, so only shipped scenarios are used
// (random maps live in memory only).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/game.hpp"
#include "warlords/save.hpp"

using namespace w2;
namespace ml = w2::ai::ml;

namespace {

const std::string kData = "original";
const std::vector<std::string> kScenarios = {"ERYTHEA", "ISLADIA", "DRAGON",
                                             "TUTORIA", "SORCERY", "HADESHA"};
struct Preset {
    const char* label;
    int hidden, diplomacy, neutral;
};
const Preset kPresets[4] = {{"Beginner-Open", 0, 0, 0},
                            {"Interm-Diplo", 0, 1, 1},
                            {"Adv-Hidden", 1, 1, 2},
                            {"Fog-TotalWar", 1, 0, 1}};
constexpr uint32_t kSeedBase = 750000;  // the development seed set

// How candidates are judged:
//   peek:   each candidate turn is played once on a copy with the real game's
//           dice and that very result is kept -- it sees its own battles'
//           outcome before committing (an upper bound, not a fair player);
//   honest: a candidate is a way of deciding (the policy with one sampling
//           seed); every rollout replays its turn with fresh dice, and the
//           chosen candidate is then played in the real game with its dice;
//   fair:   honest, and every rollout copy first loses the enemy armies the
//           side cannot see (field armies on unseen tiles and in transit).
enum class Judge { kPeek, kHonest, kFair };

struct Settings {
    const ml::PolicyNetwork* policy = nullptr;
    Judge judge = Judge::kHonest;
    int candidates = 4;      // K sampled turns besides the greedy one
    int rollouts = 4;        // R copies per candidate
    int horizon = 2;         // H rounds played on after the candidate turn
    float cand_temp = 0.5F;  // temperature the sampled candidates are drawn at
    float switch_z = 0.0F;   // only leave the greedy turn when a candidate beats it by switchZ
                             // standard errors
};

std::unique_ptr<Game> CopyGame(const Game& g) {
    return save::Decode(save::Encode(g), kData);
}

// The side's view of the board: enemy field armies it cannot see are removed
// (garrisons of cities stay -- a known approximation), and armies in transit.
void RestrictToView(Game& g, int me) {
    std::array<std::vector<Army*>, 8> unseen;
    for (Army* a : g.armies) {
        if (a->owner < 0 || a->owner >= 8 || a->owner == me || a->disbanded) continue;
        bool hidden = a->transit || a->x == kNone;
        if (!hidden && !game::CityAt(g, a->x, a->y))
            hidden = !ai::belief::IsTileVisible(g, me, a->x, a->y);
        if (hidden) unseen[a->owner].push_back(a);
    }
    for (int s = 0; s < 8; s++) {
        if (!unseen[s].empty()) game::Disband(g, g.map->sides[s], unseen[s]);
    }
}

// Plays `side`'s turn as candidate k: k == 0 is the greedy turn, k > 0 the
// policy sampled at the candidate temperature with seed k.
void PlayCandidate(Game& g, int side_index, int k, uint32_t base_seed, const Settings& st) {
    Side& side = *g.map->Side(side_index);
    if (k == 0) {
        ai::modern::PlayTurn(g, side, nullptr, st.policy, nullptr);
        return;
    }
    ml::RolloutSampler sampler;
    sampler.temperature = st.cand_temp;
    sampler.rng_state = base_seed ^ static_cast<uint32_t>(k * 2246822519U);
    ai::modern::PlayTurn(g, side, nullptr, st.policy, &sampler);
}

// Plays the current side's turn with its own kind of AI (Modern: greedy).
void PlayOwnTurn(Game& g, Side& side, const ml::PolicyNetwork& policy) {
    if (side.classic_ai)
        ai::PlayTurn(g, side, nullptr);
    else
        ai::modern::PlayTurn(g, side, nullptr, &policy, nullptr);
}

// From a game in which `side` has just played its turn, plays on for `rounds`
// rounds (other sides and later turns of `side` with their normal AIs) and
// returns `side`'s empire score.
float Rollout(Game& g, int side_index, int rounds, const ml::PolicyNetwork& policy) {
    const int stop_turn = g.turn + rounds;
    Side* cur = game::EndTurn(g);
    int safety = 0;
    while (cur && !g.over && !g.won && g.turn < stop_turn && safety++ < rounds * 12) {
        if (cur->computer && cur->alive) PlayOwnTurn(g, *cur, policy);
        cur = game::EndTurn(g);
    }
    return ml::ComputeEmpireScore(g, side_index);
}

struct CandidateResult {
    std::unique_ptr<Game> after;  // peek only: the game right after the candidate turn
    double mean = 0.0, sd = 0.0;
    int k = 0;
    uint32_t base_seed = 0;
};

// Builds and scores the K+1 candidates for `side` in `g` (its turn is next).
std::vector<CandidateResult> ScoreCandidates(const Game& g, int side_index, const Settings& st) {
    std::vector<CandidateResult> out;
    const uint32_t base_seed = static_cast<uint32_t>(g.rng.state * 2654435761U) ^
                               static_cast<uint32_t>(g.turn * 40503U);
    for (int k = 0; k <= st.candidates; k++) {
        CandidateResult c;
        std::vector<double> v;
        if (st.judge == Judge::kPeek) {
            c.after = CopyGame(g);
            PlayCandidate(*c.after, side_index, k, base_seed, st);
            for (int r = 0; r < st.rollouts; r++) {
                auto copy = CopyGame(*c.after);
                copy->rng.state ^= static_cast<uint32_t>((r + 1) * 0x9e3779b9U);
                v.push_back(Rollout(*copy, side_index, st.horizon, *st.policy));
            }
        } else {
            for (int r = 0; r < st.rollouts; r++) {
                auto copy = CopyGame(g);
                copy->rng.state ^= static_cast<uint32_t>(
                        (r + 1) * 0x9e3779b9U);  // fresh dice, also for the candidate turn
                if (st.judge == Judge::kFair) RestrictToView(*copy, side_index);
                PlayCandidate(*copy, side_index, k, base_seed, st);
                v.push_back(Rollout(*copy, side_index, st.horizon, *st.policy));
            }
        }
        for (double x : v) c.mean += x;
        c.mean /= std::max<size_t>(1, v.size());
        for (double x : v) c.sd += (x - c.mean) * (x - c.mean);
        c.sd = std::sqrt(c.sd / std::max<size_t>(1, v.size() - 1));
        c.k = k;
        c.base_seed = base_seed;
        out.push_back(std::move(c));
    }
    return out;
}

std::unique_ptr<Game> NewBenchmarkGame(int case_index, uint32_t seed, int& preset_out,
                                       std::string& scen_out) {
    int i = case_index / 2;
    scen_out = kScenarios[(i + i / 4) % kScenarios.size()];
    preset_out = i % 4;
    const Preset& p = kPresets[preset_out];
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
    return game::NewGame(kData, scen_out, o);
}

// Modern on alternating seats (swap 0/1), Classic on the rest, as in w2arena.
std::array<bool, 8> AssignSeats(Game& g, int swap) {
    std::array<bool, 8> modern{};
    int idx = 0;
    for (int s = 0; s < 8; s++) {
        Side& side = g.map->sides[s];
        if (!side.in_use) continue;
        modern[s] = ((idx + swap) % 2 == 0);
        side.classic_ai = !modern[s];
        idx++;
    }
    return modern;
}

double ModernShare(const Game& g, const std::array<bool, 8>& modern) {
    int m = 0, k = 0;
    for (const auto& c : g.map->cities) {
        if (c.razed || c.owner_index < 0 || c.owner_index >= 8) continue;
        (modern[c.owner_index] ? m : k)++;
    }
    return (m + k) > 0 ? static_cast<double>(m) / (m + k) : 0.5;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: w2search WEIGHTS [probe|play] [SEEDS] [K] [R] [H] [TURNS] "
                     "[peek|honest|fair] [CAND_TEMP] [SWITCH_Z] "
                     "[SEED_OFFSET]\n");
        return 2;
    }
    ml::PolicyNetwork policy;
    if (!policy.LoadJson(argv[1])) {
        std::fprintf(stderr, "w2search: cannot load weights '%s'\n", argv[1]);
        return 1;
    }
    std::string mode = argc > 2 ? argv[2] : "probe";
    int seeds = argc > 3 ? std::atoi(argv[3]) : 16;
    Settings st;
    st.policy = &policy;
    st.candidates = argc > 4 ? std::atoi(argv[4]) : 4;
    st.rollouts = argc > 5 ? std::atoi(argv[5]) : 4;
    st.horizon = argc > 6 ? std::atoi(argv[6]) : 2;
    int turns = argc > 7 ? std::atoi(argv[7]) : 40;
    std::string judge = argc > 8 ? argv[8] : "honest";
    st.judge = judge == "peek" ? Judge::kPeek : judge == "fair" ? Judge::kFair : Judge::kHonest;
    st.cand_temp = argc > 9 ? static_cast<float>(std::atof(argv[9])) : 0.5F;
    st.switch_z = argc > 10 ? static_cast<float>(std::atof(argv[10])) : 0.0F;
    const uint32_t seed_offset = argc > 11 ? static_cast<uint32_t>(std::atoi(argv[11])) : 0U;
    const bool play = (mode == "play");

    const int cases = seeds * 2;
    std::vector<double> shares(cases, 0.5), plain_shares(cases, 0.5);
    std::mutex mu;
    // probe statistics
    long probes = 0, beaten = 0, beaten_clearly = 0;
    double gain_sum = 0.0, rollout_sd_sum = 0.0;
    std::atomic<int> next{0};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    unsigned workers = std::max(2U, std::thread::hardware_concurrency());
    for (unsigned w = 0; w < workers; w++) {
        pool.emplace_back([&]() {
            for (int ci; (ci = next.fetch_add(1)) < cases;) {
                int preset = 0;
                std::string scen;
                uint32_t seed = kSeedBase + seed_offset + static_cast<uint32_t>(ci / 2);
                auto gp = NewBenchmarkGame(ci + 2 * static_cast<int>(seed_offset), seed, preset,
                                           scen);
                auto modern = AssignSeats(*gp, ci % 2);
                Side* cur = game::Begin(*gp);
                int safety = 0;
                while (cur && !gp->over && !gp->won && gp->turn <= turns && safety++ < turns * 12) {
                    const int s = cur->index;
                    const bool search_here =
                            cur->computer && cur->alive && modern[s] &&
                            (play ||
                             (!play && (gp->turn == 10 || gp->turn == 20 || gp->turn == 30)));
                    if (search_here) {
                        auto cands = ScoreCandidates(*gp, s, st);
                        size_t best = 0;
                        for (size_t k = 1; k < cands.size(); k++)
                            if (cands[k].mean > cands[best].mean) best = k;
                        if (best != 0 && st.switch_z > 0.0F) {
                            // Conservative switching: noise alone must not pull us off the greedy
                            // turn
                            double se = std::sqrt(
                                    (cands[best].sd * cands[best].sd + cands[0].sd * cands[0].sd) /
                                    std::max(1, st.rollouts));
                            if (cands[best].mean - cands[0].mean <= st.switch_z * se) best = 0;
                        }
                        if (!play) {
                            std::lock_guard<std::mutex> lock(mu);
                            probes++;
                            double gain = cands[best].mean - cands[0].mean;
                            gain_sum += gain;
                            if (best != 0) beaten++;
                            double se = std::sqrt(
                                    (cands[best].sd * cands[best].sd + cands[0].sd * cands[0].sd) /
                                    std::max(1, st.rollouts));
                            if (best != 0 && gain > 2.0 * se) beaten_clearly++;
                            rollout_sd_sum += cands[0].sd;
                            PlayOwnTurn(*gp, *cur,
                                        policy);  // probe only: the game itself goes on unsearched
                        } else if (st.judge == Judge::kPeek) {
                            gp = std::move(cands[best].after);
                            cur = gp->map->Side(s);
                        } else {
                            PlayCandidate(*gp, s, cands[best].k, cands[best].base_seed,
                                          st);  // real dice
                        }
                    } else if (cur->computer && cur->alive) {
                        PlayOwnTurn(*gp, *cur, policy);
                    }
                    cur = game::EndTurn(*gp);
                }
                double share = ModernShare(*gp, modern);
                std::lock_guard<std::mutex> lock(mu);
                shares[ci] = share;
                std::printf("  case %3d %-8s %-14s share %.3f\n", ci, scen.c_str(),
                            kPresets[preset].label, share);
                std::fflush(stdout);
            }
        });
    }
    for (auto& t : pool) t.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    double mean = 0.0;
    for (double x : shares) mean += x;
    mean /= std::max(1, cases);
    double var = 0.0;
    for (double x : shares) var += (x - mean) * (x - mean);
    var /= std::max(1, cases - 1);
    std::printf("=== %s | %s | %d games | K=%d R=%d H=%d T=%.2f z=%.1f | %.0f s ===\n",
                mode.c_str(), judge.c_str(), cases, st.candidates, st.rollouts, st.horizon,
                st.cand_temp, st.switch_z, sec);
    std::printf("Modern city share %.1f%% +/- %.1f%% (95%%)\n", 100.0 * mean,
                196.0 * std::sqrt(var / std::max(1, cases)));
    if (!play && probes > 0) {
        std::printf(
                "probes %ld | a sampled turn scored best in %.0f%% | clearly best (> 2 SE) in "
                "%.0f%% | "
                "mean gain of best over greedy %.4f empire-score | rollout SD of greedy %.4f\n",
                probes, 100.0 * beaten / probes, 100.0 * beaten_clearly / probes, gain_sum / probes,
                rollout_sd_sum / probes);
    }
    return 0;
}
