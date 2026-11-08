#include "opponents.hpp"

#include <algorithm>
#include <vector>

#include "warlords/ai.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/move.hpp"

namespace w2::opponents {

namespace {

// Armies of `owner` standing on the city's 2x2 tiles.
int Defenders(Game& g, const City& c) {
    int n = 0;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            for (Army* a : game::armiesAt(g, c.x + dx, c.y + dy)) {
                if (a->owner == c.ownerIndex) n++;
            }
        }
    }
    return n;
}

// May `me` walk into `c` right now without breaking a treaty?
bool Raidable(Game& g, int me, const City& c) {
    if (c.razed || c.ownerIndex < 0 || c.ownerIndex >= 8 || c.ownerIndex == me) return false;
    if (!g.map->sides[c.ownerIndex].alive) return false;
    if (diplomacy::isAllied(g, me, c.ownerIndex)) return false;
    if (diplomacy::mustDeclare(g, me, c.ownerIndex, &c)) return false;
    return Defenders(g, c) == 0;
}

// Would moving `a` away leave one of `me`'s cities without defenders?
bool LastDefender(Game& g, int me, Army* a) {
    City* home = game::cityAt(g, a->x, a->y);
    if (!home || home->ownerIndex != me) return false;
    return Defenders(g, *home) <= 1;
}

}  // namespace

int raidEmptyCities(Game& g, Side& side) {
    const int me = side.index;
    constexpr int kMaxTriesPerCity = 4;
    constexpr int kMoveCostPerTile = 2;  // cheapest open-ground step

    std::vector<City*> targets;
    for (auto& c : g.map->cities) {
        if (Raidable(g, me, c)) targets.push_back(&c);
    }
    // The learner's (non-Classic) cities first, then the richest
    auto learner_owned = [&](const City* c) { return !g.map->sides[c->ownerIndex].classicAi; };
    std::sort(targets.begin(), targets.end(), [&](City* a, City* b) {
        if (learner_owned(a) != learner_owned(b)) return learner_owned(a);
        return a->income > b->income;
    });

    int taken = 0;
    for (City* c : targets) {
        if (!Raidable(g, me, *c)) continue;
        std::vector<Army*> raiders;
        for (Army* a : g.armies) {
            if (a->owner != me || a->x == NONE || a->transit || a->moves <= 0 || a->hero())
                continue;
            int d = move::distance(a->x, a->y, c->x, c->y);
            if (d * kMoveCostPerTile > a->moves + 2) continue;
            if (LastDefender(g, me, a)) continue;
            raiders.push_back(a);
        }
        // weakest first: a raid needs a body, not strength
        std::sort(raiders.begin(), raiders.end(), [](Army* a, Army* b) {
            if (a->strength != b->strength) return a->strength < b->strength;
            return a->moves > b->moves;
        });
        int tries = 0;
        for (Army* a : raiders) {
            if (tries++ >= kMaxTriesPerCity || c->ownerIndex == me) break;
            if (!g.alive(a) || a->moves <= 0) continue;
            int dest_x = c->x + (c->x < a->x ? 1 : 0), dest_y = c->y + (c->y < a->y ? 1 : 0);
            auto path = move::findPath(g, {a}, a->x, a->y, dest_x, dest_y);
            if (!path || path->empty()) continue;
            move::WalkResult r = move::walk(g, {a}, *path);
            if (r.steps > 0) ai::walked(g, {a}, r);
            if (r.stopped == "attack" && r.attack && r.attack->city == c && g.alive(a)) {
                ai::core::Sel sel = ai::core::select(g, {a});
                if (!sel.armies.empty()) ai::core::fight(g, sel, r.attack->x, r.attack->y);
            }
            if (c->ownerIndex == me) taken++;
        }
    }
    return taken;
}

void playTurn(Game& g, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen) {
    if (kind == Kind::Frozen && frozen) {
        ai::modern::playTurn(g, side, nullptr, frozen, nullptr);
        return;
    }
    if (kind == Kind::Raider) raidEmptyCities(g, side);
    ai::playTurn(g, side, nullptr);
}

}  // namespace w2::opponents
