#include "warlords/ai/diplomacy.hpp"

#include <algorithm>

#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"

namespace w2::ai::diplo {

using namespace core;

bool CanAttack(Game& g, Side& side, const City& c) {
    int own = Owner(c);
    if (own == kNeutral) return true;
    AIData& d = Data(g, side);
    int wars = 0, last = kNone;
    for (int s = 7; s >= 0; s--) {
        if (InPlay(g, s) && State(g, side.index, s) == 2) {
            wars++;
            last = s;
        }
    }
    int st = State(g, side.index, own);
    if (st == 2) return true;
    if (st == 0 && wars != 0 && (wars != 1 || last != own)) return false;
    if (st > 2) return false;
    return d.bold;
}

bool Spared(Game& g, Side& side, int other) {
    AIData& d = Data(g, side);
    if (g.map->options.diplomacy == 0 || d.solidarity == 0 || !side.computer) return false;
    for (int h = 7; h >= 0; h--) {
        const Side& hs = g.map->sides[h];
        if (InPlay(g, h) && !hs.computer && State(g, other, h) == 2 && hs.ai_solidarity != 0)
            return true;
    }
    return false;
}

// Claim one more city as the side's own ground (558d:0917).
static void Claim(Game& g, Side& side) {
    City* cap = side.capital;
    if (!cap) return;
    City* best = nullptr;
    int best_d = kNone;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        int cl = c.claim != kNone ? c.claim : kNeutral;
        if (Standing(c) && cl != side.index && (cl == kNeutral || !InPlay(g, cl))) {
            int dd = Dist(c.x, c.y, cap->x, cap->y);
            if (dd < 40 && (best_d == kNone || dd < best_d)) {
                best = &c;
                best_d = dd;
            }
        }
    }
    if (best) best->claim = side.index;
}

void Phase(Game& g, Side& side) {
    AIData& d = Data(g, side);
    int me = side.index;
    bool human = !side.computer;
    Claim(g, side);
    if (g.map->options.diplomacy == 0) return;
    int enemy = groups::PickEnemy(g, side);

    int swapped[16] = {0}, total[16] = {0}, seen[16] = {0}, they_war[8] = {0}, they_peace[8] = {0};
    for (int b = 7; b >= 0; b--) {
        Propose(g, me, b, 0);
        int p = Proposal(g, b, me);
        if (p == 2)
            they_war[b] = 1;
        else if (p == 0)
            they_peace[b] = 1;
    }
    int tiles = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (Standing(c)) tiles++;
        int o = c.owner_index;
        if (o != kNone) {
            total[o]++;
            if (!Cflag(d, c, kCfUnseen)) seen[o]++;
            int cl = c.claim != kNone ? c.claim : kNeutral;
            if (o == me) {
                if (cl != kNeutral && cl >= 0 && cl < 16) swapped[cl]++;
            } else if (cl == me) {
                swapped[o]++;
            }
        }
    }
    // the grudge tests; each has a war branch that can never run (docs/re/ai.md)
    for (int b = 7; b >= 0; b--) {
        if (b != me && InPlay(g, b)) {
            int grudge = (they_peace[b] - they_war[b]) * 2;
            if (swapped[b] >= grudge + 4) Propose(g, me, b, 1);
        }
    }
    for (int b = 7; b >= 0; b--) {
        if (b != me && InPlay(g, b)) {
            int grudge = (they_peace[b] - they_war[b]) * 2;
            int threat = d.lost[b] + d.cities_lost[b] * 4 + d.heroes_killed[b] * 2;
            if (threat >= grudge + 5) Propose(g, me, b, std::max(1, Proposal(g, me, b)));
        }
    }
    if (enemy != kNone) Propose(g, me, enemy, 2);
    for (int gi = kMaxGroups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && grp->target != kNone) Propose(g, me, grp->target, 2);
    }
    // whoever holds too much of the world
    int leader = kNone, most = 0;
    for (int s = 7; s >= 0; s--) {
        if (InPlay(g, s)) {
            int share = total[s] * 100 / std::max(1, tiles);
            int limit = IsComputer(g, s) ? 50 : d.human_share;
            if (most < total[s] && limit < share) {
                leader = s;
                most = total[s];
            }
        }
    }
    if (leader != kNone && leader != me) {
        for (int s = 7; s >= 0; s--) {
            if (InPlay(g, s)) Propose(g, me, s, (s == leader || s == enemy) ? 2 : 0);
        }
    }
    if (!human && d.solidarity != 0) {
        for (int s = 7; s >= 0; s--) {
            if (InPlay(g, s) && IsComputer(g, s) && s != me && Proposal(g, me, s) != 0 &&
                Spared(g, side, s)) {
                Propose(g, me, s, 0);
            }
        }
        bool at_war_with_human = false;
        for (int s = 7; s >= 0; s--) {
            if (InPlay(g, s) && !IsComputer(g, s) && State(g, me, s) == 2) at_war_with_human = true;
        }
        if (at_war_with_human) {
            for (int s = 7; s >= 0; s--) {
                if (InPlay(g, s) && IsComputer(g, s) && g.map->sides[s].ai_solidarity != 0)
                    Propose(g, me, s, 0);
            }
        }
    }
    if (!human && g.greatest) {
        auto fights = groups::FightingHumans(g);
        for (int s = 7; s >= 0; s--) {
            if (InPlay(g, s) && IsComputer(g, s) && (fights[me] || fights[s])) Propose(g, me, s, 0);
        }
    }
    for (int b = 7; b >= 0; b--)
        if (seen[b] == 0) Propose(g, me, b, 0);
    for (int gi = kMaxGroups; gi >= 1; gi--) {
        auto grp = d.groups[gi];
        if (grp->active != 0 && grp->target != kNone && Proposal(g, me, grp->target) == 0)
            groups::Cancel(g, side, gi);
    }
}

}  // namespace w2::ai::diplo
