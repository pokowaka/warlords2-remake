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

const char* const kStateNames[3] = {"peace", "uneasy", "war"};
const char* const kTitles[8] = {"Statesman", "Diplomat",  "Pragmatist", "Politician",
                                "Deceiver",  "Scoundrel", "Turncoat",   "Running Dog"};

const std::vector<int>& RatingRanks(int n) {
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

void Init(Game& g) {
    int start = g.map->options.diplomacy != 0 ? kPeace : kWar;
    g.diplomacy.allied.fill(false);
    g.diplomacy.propose_allied.fill(false);
    g.diplomacy.truce_turns.fill(0);
    g.diplomacy.gold_offer.fill(0);
    g.diplomacy.war_target.fill(kNone);
    g.diplomacy.trust.fill(0);
    g.diplomacy.allied_target_city.fill(kNone);
    for (int a = 0; a < 8; a++) {
        for (int b = 0; b < 8; b++) {
            int v = start;
            g.diplomacy.state[Key(a, b)] = v;
            g.diplomacy.proposal[Key(a, b)] = v;
        }
    }
}

int State(const Game& g, int a, int b) {
    if (a < 0 || b < 0 || a > 7 || b > 7) return kWar;
    int v = g.diplomacy.state[Key(a, b)];
    return v == kNone ? kWar : v;
}

bool MayAttack(const Game& g, int a, int b) {
    if (a == kNone || b == kNone) return true;  // neutrals are fair game
    return State(g, a, b) != kPeace;
}

void Propose(Game& g, int a, int b, int st) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.proposal[Key(a, b)] = st;
    if (st != kPeace) {
        g.diplomacy.propose_allied[Key(a, b)] = false;
    }
}

int Proposal(const Game& g, int a, int b) {
    if (a < 0 || b < 0 || a > 7 || b > 7) return State(g, a, b);
    int p = g.diplomacy.proposal[Key(a, b)];
    if (p == kNone) p = State(g, a, b);
    return p;
}

bool IsAllied(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return false;
    return State(g, a, b) == kPeace && g.diplomacy.allied[Key(a, b)];
}

void SetAllied(Game& g, int a, int b, bool v) {
    if (!ValidPair(a, b)) return;
    if (v) {
        g.diplomacy.state[Key(a, b)] = kPeace;
        g.diplomacy.state[Key(b, a)] = kPeace;
        g.diplomacy.proposal[Key(a, b)] = kPeace;
        g.diplomacy.proposal[Key(b, a)] = kPeace;
    }
    g.diplomacy.allied[Key(a, b)] = v;
    g.diplomacy.allied[Key(b, a)] = v;
    g.diplomacy.propose_allied[Key(a, b)] = v;
    g.diplomacy.propose_allied[Key(b, a)] = v;
}

bool ProposalAllied(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return false;
    return g.diplomacy.propose_allied[Key(a, b)];
}

void ProposeAlliance(Game& g, int a, int b, bool v) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.propose_allied[Key(a, b)] = v;
    if (v) g.diplomacy.proposal[Key(a, b)] = kPeace;
}

int TruceTurns(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.truce_turns[Key(a, b)];
}

void SetTruce(Game& g, int a, int b, int turns) {
    if (!ValidPair(a, b)) return;
    int t = std::max(0, turns);
    g.diplomacy.truce_turns[Key(a, b)] = t;
    g.diplomacy.truce_turns[Key(b, a)] = t;
}

int GoldOffer(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.gold_offer[Key(a, b)];
}

void SetGoldOffer(Game& g, int a, int b, int gold) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.gold_offer[Key(a, b)] = gold;
}

int WarTarget(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return kNone;
    return g.diplomacy.war_target[Key(a, b)];
}

void SetWarTarget(Game& g, int a, int b, int target_side) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.war_target[Key(a, b)] = target_side;
}

int Trust(const Game& g, int a, int b) {
    if (!ValidPair(a, b)) return 0;
    return g.diplomacy.trust[Key(a, b)];
}

void AddTrust(Game& g, int a, int b, int delta) {
    if (!ValidPair(a, b)) return;
    g.diplomacy.trust[Key(a, b)] = std::clamp(g.diplomacy.trust[Key(a, b)] + delta, -100, 100);
}

int AlliedTargetCity(const Game& g, int side) {
    if (side < 0 || side >= 8) return kNone;
    return g.diplomacy.allied_target_city[side];
}

void SetAlliedTargetCity(Game& g, int side, int city_index) {
    if (side < 0 || side >= 8) return;
    g.diplomacy.allied_target_city[side] = city_index;
}

namespace {
void SettleDealTransfers(Game& g, Side& side, Side& other, std::vector<std::string>& messages) {
    int a = side.index, b = other.index;
    int offer_a = GoldOffer(g, a, b);
    int offer_b = GoldOffer(g, b, a);
    if (offer_a > 0 && side.gold > 0) {
        int paid = std::min(side.gold, offer_a);
        side.gold -= paid;
        other.gold += paid;
        AddTrust(g, b, a, std::min(30, paid / 10 + 5));
        messages.push_back(Fmt("%s paid %d gold in tribute to %s.", side.name.c_str(), paid,
                               other.name.c_str()));
    } else if (offer_b > 0 && other.gold > 0) {
        int paid = std::min(other.gold, offer_b);
        other.gold -= paid;
        side.gold += paid;
        AddTrust(g, a, b, std::min(30, paid / 10 + 5));
        messages.push_back(Fmt("%s paid %d gold in tribute to %s.", other.name.c_str(), paid,
                               side.name.c_str()));
    }
    SetGoldOffer(g, a, b, 0);
    SetGoldOffer(g, b, a, 0);

    int wt = WarTarget(g, a, b);
    if (wt == kNone) wt = WarTarget(g, b, a);
    if (wt >= 0 && wt < 8 && wt != a && wt != b && g.map->sides[wt].alive) {
        for (int participant : {a, b}) {
            if (State(g, participant, wt) != kWar) {
                g.diplomacy.state[Key(participant, wt)] = kWar;
                g.diplomacy.state[Key(wt, participant)] = kWar;
                g.diplomacy.proposal[Key(participant, wt)] = kWar;
                g.diplomacy.proposal[Key(wt, participant)] = kWar;
                SetAllied(g, participant, wt, false);
                messages.push_back(Fmt("%s joined the war against %s!",
                                       g.map->sides[participant].name.c_str(),
                                       g.map->sides[wt].name.c_str()));
            }
        }
    }
    SetWarTarget(g, a, b, kNone);
    SetWarTarget(g, b, a, kNone);
}
}  // namespace

std::vector<std::string> Apply(Game& g, Side& side) {
    std::vector<std::string> messages;
    int a = side.index;
    for (int b = 0; b < 8; b++) {
        Side* other = g.map->Side(b);
        if (b != a && other) {
            if (!side.classic_ai && TruceTurns(g, a, b) > 0 && State(g, a, b) == kPeace) {
                SetTruce(g, a, b, TruceTurns(g, a, b) - 1);
            }
            int now = State(g, a, b);
            int want = Proposal(g, a, b);
            if (want != now) {
                if (want > now) {  // escalation: at once
                    bool was_allied = IsAllied(g, a, b);
                    int truce_left = TruceTurns(g, a, b);
                    SetAllied(g, a, b, false);
                    SetTruce(g, a, b, 0);
                    g.diplomacy.state[Key(a, b)] = want;
                    g.diplomacy.state[Key(b, a)] = want;
                    if (Proposal(g, b, a) < want) g.diplomacy.proposal[Key(b, a)] = want;
                    if (!side.classic_ai && (was_allied || (truce_left > 0 && want == kWar))) {
                        AddTrust(g, b, a, -50);
                        for (int c = 0; c < 8; c++) {
                            if (c != a && c != b) AddTrust(g, c, a, -20);
                        }
                    }
                    if (want == kWar) {
                        history::RecordDeed(g, &side, DeedType::kAtWar, a, b, "");  // 484e:0f15
                        messages.push_back("War declared with " + other->name + "!");
                    }
                } else if (Proposal(g, b, a) <= want) {  // de-escalation: mutual
                    g.diplomacy.state[Key(a, b)] = want;
                    g.diplomacy.state[Key(b, a)] = want;
                    if (want == kPeace) {
                        history::RecordDeed(g, &side, DeedType::kAtPeace, a, b, "");  // 484e:1027
                        messages.push_back("Peace negotiated with " + other->name + "!");
                        if (!side.classic_ai || !other->classic_ai) {
                            SetTruce(g, a, b, 10);
                            AddTrust(g, a, b, 15);
                            AddTrust(g, b, a, 15);
                            SettleDealTransfers(g, side, *other, messages);
                        }
                    }
                }
            }
            // Check mutual Alliance upgrade when at Peace
            if (State(g, a, b) == kPeace && !IsAllied(g, a, b) && ProposalAllied(g, a, b) &&
                ProposalAllied(g, b, a)) {
                SetAllied(g, a, b, true);
                SetTruce(g, a, b, 15);
                AddTrust(g, a, b, 25);
                AddTrust(g, b, a, 25);
                SettleDealTransfers(g, side, *other, messages);
                messages.push_back("Alliance forged with " + other->name + "!");
            }
        }
    }
    return messages;
}

bool MustDeclare(const Game& g, int a, int b, const City* city) {
    if (g.map->options.diplomacy == 0 || a == kNone || b == kNone) return false;
    int st = State(g, a, b);
    return st == kPeace || (st == kIntermediate && city && city->index > 0);
}

void AttackAnyway(Game& g, int a, int b) {
    if (a == kNone || b == kNone) return;
    Side* side = g.map->Side(a);
    if (!side) return;
    int st = State(g, a, b);
    if (st == kPeace)
        game::AddDiploScore(g, *side, g.rng.Dice(1, 100, 100));
    else if (st == kIntermediate)
        game::AddDiploScore(g, *side, g.rng.Dice(1, 15, 10));
    SetAllied(g, a, b, false);
    SetTruce(g, a, b, 0);
    AddTrust(g, b, a, -60);
    for (int c = 0; c < 8; c++) {
        if (c != a && c != b) AddTrust(g, c, a, -25);
    }
    for (int k : {Key(a, b), Key(b, a)}) {
        g.diplomacy.state[k] = kWar;
        g.diplomacy.proposal[k] = kWar;
    }
    history::RecordDeed(g, side, DeedType::kTreachery, a, b, "");
}

void ScoreUpdate(Game& g, Side& side) {
    int a = side.index;
    for (int b = 0; b < 8; b++) {
        if (b != a) {
            int p = Proposal(g, a, b), st = State(g, a, b);
            if (p < st && p < Proposal(g, b, a)) AddScore(g, side, st, p);
        }
    }
}

int AddScore(Game& g, Side& side, int from, int to) {
    int gain = kNone;
    if (from == kWar && to == kPeace)
        gain = g.rng.Dice(1, 10, 10);
    else if (to < from)
        gain = g.rng.Dice(1, 2, 1);
    if (gain != kNone) game::AddDiploScore(g, side, gain);
    return gain;
}

std::map<int, std::string> Ratings(const Game& g) {
    struct E {
        Side* side;
        int score;
        int seq;
    };
    std::vector<E> order;
    // only the sides still in the game are ranked (484e:0aed)
    for (size_t i = 0; i < g.sides.size(); i++) {
        if (g.sides[i]->alive)
            order.push_back(E{g.sides[i], g.sides[i]->diplo_score, static_cast<int>(i)});
    }
    LuaSort(order, [](const E& p, const E& q) {
        return p.score != q.score ? p.score < q.score : p.seq < q.seq;
    });
    const std::vector<int>& ranks = RatingRanks(static_cast<int>(order.size()));
    std::map<int, std::string> out;
    for (size_t i = 0; i < order.size(); i++) {
        int r = i < ranks.size() ? ranks[i] : 8;
        out[order[i].side->index] = kTitles[r - 1];
    }
    return out;
}

AttitudeBreakdown ComputeAttitude(const Game& g, int ai_side, int target_side) {
    AttitudeBreakdown out;
    out.from_side = ai_side;
    out.to_side = target_side;
    if (!ValidPair(ai_side, target_side)) return out;

    const Side& me = g.map->sides[ai_side];
    const Side& them = g.map->sides[target_side];
    const auto& prof = ai::personality::ForSide(me);
    out.personality_name = prof.name;
    out.personality_archetype = prof.archetype;
    out.motto = prof.motto;
    out.trust_score = Trust(g, ai_side, target_side);
    out.truce_remaining = TruceTurns(g, ai_side, target_side);

    // Only cities the AI side can actually see count, so the attitude score (which
    // feeds the diplomacy policy) never leaks the true map under Hidden Map.
    int total_standing_cities = 0;
    int their_cities = 0, leader_cities = 0, leader_side = kNone;
    int cities_taken_from_me = 0;
    std::array<int, 8> side_cities{};
    for (const auto& c : g.map->cities) {
        if (c.razed || !ai::belief::IsCityVisible(g, ai_side, c)) continue;
        total_standing_cities++;
        if (c.owner_index >= 0 && c.owner_index < 8) {
            side_cities[c.owner_index]++;
            if (side_cities[c.owner_index] > leader_cities) {
                leader_cities = side_cities[c.owner_index];
                leader_side = c.owner_index;
            }
        }
        if (c.owner_index == target_side) {
            their_cities++;
            if (c.claim == ai_side) cities_taken_from_me++;
        }
    }

    auto push_factor = [&](const std::string& label, int delta) {
        if (delta == 0) return;
        out.factors.push_back(AttitudeFactor{label, delta});
        out.total_score += delta;
    };

    // Personality disposition baseline
    int base_disp = static_cast<int>(std::round((prof.traits[ai::personality::kHonour] -
                                                 prof.traits[ai::personality::kAggression] * 0.6F) *
                                                20.0F));
    push_factor("Personality (" + prof.archetype + ")", base_disp);

    // Relational trust & treaty history
    if (out.trust_score != 0) {
        push_factor("Treaty and trust track record", out.trust_score / 2);
    }
    if (IsAllied(g, ai_side, target_side)) {
        push_factor("Sworn military alliance", 35);
    } else if (out.truce_remaining > 0) {
        push_factor(Fmt("Active peace truce (%d turns left)", out.truce_remaining), 15);
    }

    // Shared enemies vs opposing wars
    int shared_wars = 0;
    for (int s = 0; s < 8; s++) {
        if (s != ai_side && s != target_side && g.map->sides[s].alive) {
            if (State(g, ai_side, s) == kWar && State(g, target_side, s) == kWar) shared_wars++;
        }
    }
    if (shared_wars > 0) {
        int bonus = shared_wars *
                    static_cast<int>(std::round(
                            18.0F + 10.0F * prof.traits[ai::personality::kCoalitionDrive]));
        push_factor(
                Fmt("Fighting %d common %s", shared_wars, shared_wars == 1 ? "enemy" : "enemies"),
                bonus);
    }

    // Border military pressure (from fair-info belief if available)
    float their_border_str = 0.0F, my_border_str = 0.0F;
    if (me.belief) {
        their_border_str = me.belief->their_border_strength[target_side];
        my_border_str = me.belief->my_border_strength[target_side];
    }
    if (their_border_str > 4.0F) {
        int border_pen = -std::min(40, static_cast<int>(std::round(their_border_str * 1.2F)));
        push_factor("Your armies massed near our borders", border_pen);
    } else if (my_border_str > their_border_str + 12.0F &&
               prof.traits[ai::personality::kAggression] > 0.2F) {
        push_factor("Our strike forces outmatch your border garrison", -18);
    }

    // Past battle casualties & lost cities
    if (me.ai) {
        int grudge = me.ai->cities_lost[target_side] * 10 + me.ai->heroes_killed[target_side] * 8 +
                     me.ai->lost[target_side] * 3;
        if (grudge > 0) {
            push_factor("Blood feud (cities and heroes lost)", -std::min(50, grudge));
        }
    }
    if (cities_taken_from_me > 0) {
        push_factor(Fmt("You hold %d of our claimed cities", cities_taken_from_me),
                    -cities_taken_from_me * 8);
    }
    if (me.capital && me.capital->owner_index == target_side) {
        push_factor("You occupy our ancestral Capital!", -60);
    }

    // Balance of power / anti-runaway coalition logic
    float their_share = static_cast<float>(their_cities) /
                        static_cast<float>(std::max(1, total_standing_cities));
    if (leader_side == target_side && their_share >= 0.30F) {
        int hegemony_pen = -static_cast<int>(
                std::round((their_share - 0.25F) * 120.0F *
                           (1.0F + 0.5F * prof.traits[ai::personality::kCoalitionDrive])));
        push_factor(Fmt("Fear of your growing hegemony (%d%% of cities)",
                        static_cast<int>(std::round(their_share * 100.0F))),
                    hegemony_pen);
    } else if (leader_side != kNone && leader_side != ai_side && leader_side != target_side &&
               static_cast<float>(leader_cities) /
                               static_cast<float>(std::max(1, total_standing_cities)) >=
                       0.32F) {
        push_factor("Need for coalition against the runaway leader", 22);
    }

    // Diplomatic reputation / atrocities
    if (them.diplo_score > 60) {
        int atrocity_pen = -std::min(35, (them.diplo_score - 40) / 4);
        if (prof.traits[ai::personality::kHonour] > 0.0F) {
            push_factor("Your reputation for razing and treachery", atrocity_pen);
        }
    }

    // Active tribute offer on the table
    int offered_to_ai = GoldOffer(g, target_side, ai_side);
    if (offered_to_ai > 0) {
        int gold_bonus = static_cast<int>(
                std::round(static_cast<float>(offered_to_ai) / 8.0F *
                           (1.0F + 0.6F * prof.traits[ai::personality::kAvarice])));
        push_factor(Fmt("Gold tribute offered (%d gold)", offered_to_ai), gold_bonus);
    }

    out.total_score = std::clamp(out.total_score, -100, 100);

    // Compute clear stance summary and "What It Takes" forecast
    int cur_state = State(g, ai_side, target_side);
    if (IsAllied(g, ai_side, target_side)) {
        out.stance_summary = "Allied Partner";
        out.what_it_takes = "Alliance is strong. Coordinate strikes against shared foes!";
        out.peace_gold_needed = 0;
        out.ally_gold_needed = 0;
    } else if (me.capital && me.capital->owner_index == target_side) {
        out.stance_summary = "Mortal Enemy (Capital Held)";
        out.what_it_takes = "Refuses Peace while you hold " + me.capital->name + "!";
        out.peace_gold_needed = kNone;
        out.ally_gold_needed = kNone;
    } else {
        float greed_mult = std::max(0.5F, 1.2F - 0.5F * prof.traits[ai::personality::kAvarice]);
        if (out.total_score >= 35) {
            out.stance_summary = "Friendly, Cooperative";
            out.peace_gold_needed = 0;
            out.ally_gold_needed = 0;
            out.what_it_takes = "Willing to accept Peace or forge a full Alliance!";
        } else if (out.total_score >= 0) {
            out.stance_summary = (cur_state == kWar) ? "Weary of War" : "Cautious, Pragmatic";
            out.peace_gold_needed = 0;
            out.ally_gold_needed = std::max(
                    50, static_cast<int>(std::round((35 - out.total_score) * 8.0F / greed_mult)) /
                                25 * 25);
            out.what_it_takes = Fmt("Accepts Peace freely; Alliance for ~%d gold (or shared war).",
                                    out.ally_gold_needed);
        } else if (out.total_score >= -45) {
            out.stance_summary = "Hostile, Distrustful";
            out.peace_gold_needed = std::max(
                    50, static_cast<int>(std::round((-out.total_score + 10) * 6.0F / greed_mult)) /
                                25 * 25);
            out.ally_gold_needed = kNone;
            out.what_it_takes = Fmt("Demands %d gold tribute (or border withdrawal) for Peace.",
                                    out.peace_gold_needed);
        } else {
            out.stance_summary = "Implacable Warpath";
            if (prof.traits[ai::personality::kAvarice] > 0.5F && out.total_score > -75) {
                out.peace_gold_needed = std::max(
                        150, static_cast<int>(std::round((-out.total_score) * 8.0F)) / 50 * 50);
                out.what_it_takes = Fmt("Only a heavy tribute of %d gold will stay their hand!",
                                        out.peace_gold_needed);
            } else {
                out.peace_gold_needed = kNone;
                out.what_it_takes =
                        "Refuses Peace: determined to settle grievances on the battlefield!";
            }
            out.ally_gold_needed = kNone;
        }
    }

    return out;
}

}  // namespace w2::diplomacy
