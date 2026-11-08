#include "warlords/diplomacy.hpp"

#include <algorithm>
#include <cmath>

#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"

namespace w2::diplomacy {

const char* const STATE_NAMES[3] = {"peace", "uneasy", "war"};
const char* const TITLES[8] = {"Statesman", "Diplomat",  "Pragmatist", "Politician",
                               "Deceiver",  "Scoundrel", "Turncoat",   "Running Dog"};

const std::vector<int>& ratingRanks(int n) {
    static const std::vector<int> kRanks[9] = {{},
                                               {1},
                                               {1, 8},
                                               {1, 4, 8},
                                               {1, 2, 6, 8},
                                               {1, 2, 4, 6, 8},
                                               {1, 2, 4, 6, 7, 8},
                                               {1, 2, 4, 5, 6, 7, 8},
                                               {1, 2, 3, 4, 5, 6, 7, 8}};
    if (n < 1 || n > 8) return kRanks[8];
    return kRanks[n];
}

static int Key(int a, int b) {
    return a * 8 + b;
}
static bool ValidPair(int a, int b) {
    return a >= 0 && b >= 0 && a < 8 && b < 8 && a != b;
}

void init(Game& g) {
    int start = g.map->options.diplomacy != 0 ? PEACE : WAR;
    g.diplomacy.allied.fill(false);
    g.diplomacy.proposeAllied.fill(false);
    g.diplomacy.truceTurns.fill(0);
    g.diplomacy.goldOffer.fill(0);
    g.diplomacy.warTarget.fill(NONE);
    g.diplomacy.trust.fill(0);
    g.diplomacy.alliedTargetCity.fill(NONE);
    for (int a = 0; a < 8; a++) {
        for (int b = 0; b < 8; b++) {
            int v = start;
            g.diplomacy.state[Key(a, b)] = v;
            g.diplomacy.proposal[Key(a, b)] = v;
        }
    }
}

int state(const Game& g, int a, int b) {
    if (a < 0 || b < 0 || a > 7 || b > 7) return WAR;
    int v = g.diplomacy.state[Key(a, b)];
    return v == NONE ? WAR : v;
}

bool mayAttack(const Game& g, int a, int b) {
    if (a == NONE || b == NONE) return true;  // neutrals are fair game
    return state(g, a, b) != PEACE;
}

void propose(Game& g, int a, int b, int st) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.proposal[Key(a, b)] = st;
    if (st != PEACE) {
        g.diplomacy.proposeAllied[Key(a, b)] = false;
    }
}

int proposal(const Game& g, int a, int b) {
    if (a < 0 || b < 0 || a > 7 || b > 7) return state(g, a, b);
    int p = g.diplomacy.proposal[Key(a, b)];
    if (p == NONE) p = state(g, a, b);
    return p;
}

bool isAllied(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return false;
    return state(g, a, b) == PEACE && g.diplomacy.allied[Key(a, b)];
}

void setAllied(Game& g, int a, int b, bool v) {
    if (!ValidPair(a, b)) return;
    if (v) {
        g.diplomacy.state[Key(a, b)] = PEACE;
        g.diplomacy.state[Key(b, a)] = PEACE;
        g.diplomacy.proposal[Key(a, b)] = PEACE;
        g.diplomacy.proposal[Key(b, a)] = PEACE;
    }
    g.diplomacy.allied[Key(a, b)] = v;
    g.diplomacy.allied[Key(b, a)] = v;
    g.diplomacy.proposeAllied[Key(a, b)] = v;
    g.diplomacy.proposeAllied[Key(b, a)] = v;
}

bool proposalAllied(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return false;
    return g.diplomacy.proposeAllied[Key(a, b)];
}

void proposeAlliance(Game& g, int a, int b, bool v) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.proposeAllied[Key(a, b)] = v;
    if (v) g.diplomacy.proposal[Key(a, b)] = PEACE;
}

int truceTurns(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.truceTurns[Key(a, b)];
}

void setTruce(Game& g, int a, int b, int turns) {
    if (!ValidPair(a, b)) return;
    int t = std::max(0, turns);
    g.diplomacy.truceTurns[Key(a, b)] = t;
    g.diplomacy.truceTurns[Key(b, a)] = t;
}

int goldOffer(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.goldOffer[Key(a, b)];
}

void setGoldOffer(Game& g, int a, int b, int gold) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.goldOffer[Key(a, b)] = gold;
}

int warTarget(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return NONE;
    return g.diplomacy.warTarget[Key(a, b)];
}

void setWarTarget(Game& g, int a, int b, int target_side) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.warTarget[Key(a, b)] = target_side;
}

int trust(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.trust[Key(a, b)];
}

void addTrust(Game& g, int a, int b, int delta) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.trust[Key(a, b)] = std::clamp(g.diplomacy.trust[Key(a, b)] + delta, -100, 100);
}

int alliedTargetCity(const Game& g, int side) {
    if (side < 0 || side >= 8) return NONE;
    return g.diplomacy.alliedTargetCity[side];
}

void setAlliedTargetCity(Game& g, int side, int city_index) {
    if (side < 0 || side >= 8) return;
    g.diplomacy.alliedTargetCity[side] = city_index;
}

namespace {
void SettleDealTransfers(Game& g, Side& side, Side& other, std::vector<std::string>& messages) {
    int a = side.index, b = other.index;
    int offer_a = goldOffer(g, a, b);
    int offer_b = goldOffer(g, b, a);
    if (offer_a > 0 && side.gold > 0) {
        int paid = std::min(side.gold, offer_a);
        side.gold -= paid;
        other.gold += paid;
        addTrust(g, b, a, std::min(30, paid / 10 + 5));
        messages.push_back(fmt("%s paid %d gold in tribute to %s.", side.name.c_str(), paid,
                               other.name.c_str()));
    } else if (offer_b > 0 && other.gold > 0) {
        int paid = std::min(other.gold, offer_b);
        other.gold -= paid;
        side.gold += paid;
        addTrust(g, a, b, std::min(30, paid / 10 + 5));
        messages.push_back(fmt("%s paid %d gold in tribute to %s.", other.name.c_str(), paid,
                               side.name.c_str()));
    }
    setGoldOffer(g, a, b, 0);
    setGoldOffer(g, b, a, 0);

    int wt = warTarget(g, a, b);
    if (wt == NONE) wt = warTarget(g, b, a);
    if (wt >= 0 && wt < 8 && wt != a && wt != b && g.map->sides[wt].alive) {
        for (int participant : {a, b}) {
            if (state(g, participant, wt) != WAR) {
                g.diplomacy.state[Key(participant, wt)] = WAR;
                g.diplomacy.state[Key(wt, participant)] = WAR;
                g.diplomacy.proposal[Key(participant, wt)] = WAR;
                g.diplomacy.proposal[Key(wt, participant)] = WAR;
                setAllied(g, participant, wt, false);
                messages.push_back(fmt("%s joined the war against %s!",
                                       g.map->sides[participant].name.c_str(),
                                       g.map->sides[wt].name.c_str()));
            }
        }
    }
    setWarTarget(g, a, b, NONE);
    setWarTarget(g, b, a, NONE);
}
}  // namespace

std::vector<std::string> apply(Game& g, Side& side) {
    std::vector<std::string> messages;
    int a = side.index;
    for (int b = 0; b < 8; b++) {
        Side* other = g.map->side(b);
        if (b != a && other) {
            if (!side.classicAi && truceTurns(g, a, b) > 0 && state(g, a, b) == PEACE) {
                setTruce(g, a, b, truceTurns(g, a, b) - 1);
            }
            int now = state(g, a, b);
            int want = proposal(g, a, b);
            if (want != now) {
                if (want > now) {  // escalation: at once
                    bool was_allied = isAllied(g, a, b);
                    int truce_left = truceTurns(g, a, b);
                    setAllied(g, a, b, false);
                    setTruce(g, a, b, 0);
                    g.diplomacy.state[Key(a, b)] = want;
                    g.diplomacy.state[Key(b, a)] = want;
                    if (proposal(g, b, a) < want) g.diplomacy.proposal[Key(b, a)] = want;
                    if (!side.classicAi && (was_allied || (truce_left > 0 && want == WAR))) {
                        addTrust(g, b, a, -50);
                        for (int c = 0; c < 8; c++) {
                            if (c != a && c != b) addTrust(g, c, a, -20);
                        }
                    }
                    if (want == WAR) {
                        history::deed(g, &side, DeedType::AT_WAR, a, b, "");  // 484e:0f15
                        messages.push_back("War declared with " + other->name + "!");
                    }
                } else if (proposal(g, b, a) <= want) {  // de-escalation: mutual
                    g.diplomacy.state[Key(a, b)] = want;
                    g.diplomacy.state[Key(b, a)] = want;
                    if (want == PEACE) {
                        history::deed(g, &side, DeedType::AT_PEACE, a, b, "");  // 484e:1027
                        messages.push_back("Peace negotiated with " + other->name + "!");
                        if (!side.classicAi || !other->classicAi) {
                            setTruce(g, a, b, 10);
                            addTrust(g, a, b, 15);
                            addTrust(g, b, a, 15);
                            SettleDealTransfers(g, side, *other, messages);
                        }
                    }
                }
            }
            // Check mutual Alliance upgrade when at Peace
            if (state(g, a, b) == PEACE && !isAllied(g, a, b) && proposalAllied(g, a, b) &&
                proposalAllied(g, b, a)) {
                setAllied(g, a, b, true);
                setTruce(g, a, b, 15);
                addTrust(g, a, b, 25);
                addTrust(g, b, a, 25);
                SettleDealTransfers(g, side, *other, messages);
                messages.push_back("Alliance forged with " + other->name + "!");
            }
        }
    }
    return messages;
}

bool mustDeclare(const Game& g, int a, int b, const City* city) {
    if (g.map->options.diplomacy == 0 || a == NONE || b == NONE) return false;
    int st = state(g, a, b);
    return st == PEACE || (st == INTERMEDIATE && city && city->index > 0);
}

void attackAnyway(Game& g, int a, int b) {
    if (a == NONE || b == NONE) return;
    Side* side = g.map->side(a);
    if (!side) return;
    int st = state(g, a, b);
    if (st == PEACE)
        game::addDiploScore(g, *side, g.rng.dice(1, 100, 100));
    else if (st == INTERMEDIATE)
        game::addDiploScore(g, *side, g.rng.dice(1, 15, 10));
    setAllied(g, a, b, false);
    setTruce(g, a, b, 0);
    addTrust(g, b, a, -60);
    for (int c = 0; c < 8; c++) {
        if (c != a && c != b) addTrust(g, c, a, -25);
    }
    for (int k : {Key(a, b), Key(b, a)}) {
        g.diplomacy.state[k] = WAR;
        g.diplomacy.proposal[k] = WAR;
    }
    history::deed(g, side, DeedType::TREACHERY, a, b, "");
}

void scoreUpdate(Game& g, Side& side) {
    int a = side.index;
    for (int b = 0; b < 8; b++) {
        if (b != a) {
            int p = proposal(g, a, b), st = state(g, a, b);
            if (p < st && p < proposal(g, b, a)) addScore(g, side, st, p);
        }
    }
}

int addScore(Game& g, Side& side, int from, int to) {
    int gain = NONE;
    if (from == WAR && to == PEACE)
        gain = g.rng.dice(1, 10, 10);
    else if (to < from)
        gain = g.rng.dice(1, 2, 1);
    if (gain != NONE) game::addDiploScore(g, side, gain);
    return gain;
}

std::map<int, std::string> ratings(const Game& g) {
    struct E {
        Side* side;
        int score;
        int seq;
    };
    std::vector<E> order;
    // only the sides still in the game are ranked (484e:0aed)
    for (size_t i = 0; i < g.sides.size(); i++) {
        if (g.sides[i]->alive)
            order.push_back(E{g.sides[i], g.sides[i]->diploScore, static_cast<int>(i)});
    }
    luaSort(order, [](const E& p, const E& q) {
        return p.score != q.score ? p.score < q.score : p.seq < q.seq;
    });
    const std::vector<int>& ranks = ratingRanks(static_cast<int>(order.size()));
    std::map<int, std::string> out;
    for (size_t i = 0; i < order.size(); i++) {
        int r = i < ranks.size() ? ranks[i] : 8;
        out[order[i].side->index] = TITLES[r - 1];
    }
    return out;
}

AttitudeBreakdown computeAttitude(const Game& g, int ai_side, int target_side) {
    AttitudeBreakdown out;
    out.fromSide = ai_side;
    out.toSide = target_side;
    if (!ValidPair(ai_side, target_side)) return out;

    const Side& me = g.map->sides[ai_side];
    const Side& them = g.map->sides[target_side];
    const auto& prof = ai::personality::forSide(me);
    out.personalityName = prof.name;
    out.personalityArchetype = prof.archetype;
    out.motto = prof.motto;
    out.trustScore = trust(g, ai_side, target_side);
    out.truceRemaining = truceTurns(g, ai_side, target_side);

    // Only cities the AI side can actually see count, so the attitude score (which
    // feeds the diplomacy policy) never leaks the true map under Hidden Map.
    int total_standing_cities = 0;
    int their_cities = 0, leader_cities = 0, leader_side = NONE;
    int cities_taken_from_me = 0;
    std::array<int, 8> side_cities{};
    for (const auto& c : g.map->cities) {
        if (c.razed || !ai::belief::isCityVisible(g, ai_side, c)) continue;
        total_standing_cities++;
        if (c.ownerIndex >= 0 && c.ownerIndex < 8) {
            side_cities[c.ownerIndex]++;
            if (side_cities[c.ownerIndex] > leader_cities) {
                leader_cities = side_cities[c.ownerIndex];
                leader_side = c.ownerIndex;
            }
        }
        if (c.ownerIndex == target_side) {
            their_cities++;
            if (c.claim == ai_side) cities_taken_from_me++;
        }
    }

    auto push_factor = [&](const std::string& label, int delta) {
        if (delta == 0) return;
        out.factors.push_back(AttitudeFactor{label, delta});
        out.totalScore += delta;
    };

    // Personality disposition baseline
    int base_disp = static_cast<int>(std::round((prof.traits[ai::personality::HONOUR] -
                                                 prof.traits[ai::personality::AGGRESSION] * 0.6F) *
                                                20.0F));
    push_factor("Personality (" + prof.archetype + ")", base_disp);

    // Relational trust & treaty history
    if (out.trustScore != 0) {
        push_factor("Treaty and trust track record", out.trustScore / 2);
    }
    if (isAllied(g, ai_side, target_side)) {
        push_factor("Sworn military alliance", 35);
    } else if (out.truceRemaining > 0) {
        push_factor(fmt("Active peace truce (%d turns left)", out.truceRemaining), 15);
    }

    // Shared enemies vs opposing wars
    int shared_wars = 0;
    for (int s = 0; s < 8; s++) {
        if (s != ai_side && s != target_side && g.map->sides[s].alive) {
            if (state(g, ai_side, s) == WAR && state(g, target_side, s) == WAR) shared_wars++;
        }
    }
    if (shared_wars > 0) {
        int bonus = shared_wars *
                    static_cast<int>(std::round(
                            18.0F + 10.0F * prof.traits[ai::personality::COALITION_DRIVE]));
        push_factor(
                fmt("Fighting %d common %s", shared_wars, shared_wars == 1 ? "enemy" : "enemies"),
                bonus);
    }

    // Border military pressure (from fair-info belief if available)
    float their_border_str = 0.0F, my_border_str = 0.0F;
    if (me.belief) {
        their_border_str = me.belief->theirBorderStrength[target_side];
        my_border_str = me.belief->myBorderStrength[target_side];
    }
    if (their_border_str > 4.0F) {
        int border_pen = -std::min(40, static_cast<int>(std::round(their_border_str * 1.2F)));
        push_factor("Your armies massed near our borders", border_pen);
    } else if (my_border_str > their_border_str + 12.0F &&
               prof.traits[ai::personality::AGGRESSION] > 0.2F) {
        push_factor("Our strike forces outmatch your border garrison", -18);
    }

    // Past battle casualties & lost cities
    if (me.ai) {
        int grudge = me.ai->citiesLost[target_side] * 10 + me.ai->heroesKilled[target_side] * 8 +
                     me.ai->lost[target_side] * 3;
        if (grudge > 0) {
            push_factor("Blood feud (cities and heroes lost)", -std::min(50, grudge));
        }
    }
    if (cities_taken_from_me > 0) {
        push_factor(fmt("You hold %d of our claimed cities", cities_taken_from_me),
                    -cities_taken_from_me * 8);
    }
    if (me.capital && me.capital->ownerIndex == target_side) {
        push_factor("You occupy our ancestral Capital!", -60);
    }

    // Balance of power / anti-runaway coalition logic
    float their_share = static_cast<float>(their_cities) /
                        static_cast<float>(std::max(1, total_standing_cities));
    if (leader_side == target_side && their_share >= 0.30F) {
        int hegemony_pen = -static_cast<int>(
                std::round((their_share - 0.25F) * 120.0F *
                           (1.0F + 0.5F * prof.traits[ai::personality::COALITION_DRIVE])));
        push_factor(fmt("Fear of your growing hegemony (%d%% of cities)",
                        static_cast<int>(std::round(their_share * 100.0F))),
                    hegemony_pen);
    } else if (leader_side != NONE && leader_side != ai_side && leader_side != target_side &&
               static_cast<float>(leader_cities) /
                               static_cast<float>(std::max(1, total_standing_cities)) >=
                       0.32F) {
        push_factor("Need for coalition against the runaway leader", 22);
    }

    // Diplomatic reputation / atrocities
    if (them.diploScore > 60) {
        int atrocity_pen = -std::min(35, (them.diploScore - 40) / 4);
        if (prof.traits[ai::personality::HONOUR] > 0.0F) {
            push_factor("Your reputation for razing and treachery", atrocity_pen);
        }
    }

    // Active tribute offer on the table
    int offered_to_ai = goldOffer(g, target_side, ai_side);
    if (offered_to_ai > 0) {
        int gold_bonus =
                static_cast<int>(std::round(static_cast<float>(offered_to_ai) / 8.0F *
                                            (1.0F + 0.6F * prof.traits[ai::personality::AVARICE])));
        push_factor(fmt("Gold tribute offered (%d gold)", offered_to_ai), gold_bonus);
    }

    out.totalScore = std::clamp(out.totalScore, -100, 100);

    // Compute clear stance summary and "What It Takes" forecast
    int cur_state = state(g, ai_side, target_side);
    if (isAllied(g, ai_side, target_side)) {
        out.stanceSummary = "Allied Partner";
        out.whatItTakes = "Alliance is strong. Coordinate strikes against shared foes!";
        out.peaceGoldNeeded = 0;
        out.allyGoldNeeded = 0;
    } else if (me.capital && me.capital->ownerIndex == target_side) {
        out.stanceSummary = "Mortal Enemy (Capital Held)";
        out.whatItTakes = "Refuses Peace while you hold " + me.capital->name + "!";
        out.peaceGoldNeeded = NONE;
        out.allyGoldNeeded = NONE;
    } else {
        float greed_mult = std::max(0.5F, 1.2F - 0.5F * prof.traits[ai::personality::AVARICE]);
        if (out.totalScore >= 35) {
            out.stanceSummary = "Friendly, Cooperative";
            out.peaceGoldNeeded = 0;
            out.allyGoldNeeded = 0;
            out.whatItTakes = "Willing to accept Peace or forge a full Alliance!";
        } else if (out.totalScore >= 0) {
            out.stanceSummary = (cur_state == WAR) ? "Weary of War" : "Cautious, Pragmatic";
            out.peaceGoldNeeded = 0;
            out.allyGoldNeeded = std::max(
                    50, static_cast<int>(std::round((35 - out.totalScore) * 8.0F / greed_mult)) /
                                25 * 25);
            out.whatItTakes = fmt("Accepts Peace freely; Alliance for ~%d gold (or shared war).",
                                  out.allyGoldNeeded);
        } else if (out.totalScore >= -45) {
            out.stanceSummary = "Hostile, Distrustful";
            out.peaceGoldNeeded = std::max(
                    50, static_cast<int>(std::round((-out.totalScore + 10) * 6.0F / greed_mult)) /
                                25 * 25);
            out.allyGoldNeeded = NONE;
            out.whatItTakes = fmt("Demands %d gold tribute (or border withdrawal) for Peace.",
                                  out.peaceGoldNeeded);
        } else {
            out.stanceSummary = "Implacable Warpath";
            if (prof.traits[ai::personality::AVARICE] > 0.5F && out.totalScore > -75) {
                out.peaceGoldNeeded = std::max(
                        150, static_cast<int>(std::round((-out.totalScore) * 8.0F)) / 50 * 50);
                out.whatItTakes = fmt("Only a heavy tribute of %d gold will stay their hand!",
                                      out.peaceGoldNeeded);
            } else {
                out.peaceGoldNeeded = NONE;
                out.whatItTakes =
                        "Refuses Peace: determined to settle grievances on the battlefield!";
            }
            out.allyGoldNeeded = NONE;
        }
    }

    return out;
}

}  // namespace w2::diplomacy
