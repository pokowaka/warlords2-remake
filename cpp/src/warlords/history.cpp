#include "warlords/history.hpp"

#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/report.hpp"

namespace w2::history {

void RecordDeed(Game& g, const Side* side, DeedType type, int v1, int v2, const std::string& name) {
    if (!side) return;
    auto& d = g.deeds[side->index];
    Deed e{side->index, type, v1, v2, name.substr(0, 15)};
    if (d.size() < 2) {
        d.push_back(e);
        return;
    }
    int worse = d[1].type >= d[0].type ? 1 : 0;
    if (d[worse].type > type) d[worse] = e;
}

static void Bump(Game& g, int side, int opp, Loss kind, int n = 1) {
    if (side < 0 || opp < 0 || side > 7 || opp > 7) return;
    auto& row = g.triumphs[side].try_emplace(opp, std::array<int, 5>{0, 0, 0, 0, 0}).first->second;
    row[static_cast<int>(kind)] += n;
}

void Tally(Game& g, const Army* army, int killer, bool boat) {
    int loser = army->owner;
    Loss kind;
    if (army->type == armytype::kHero)
        kind = Loss::kHeroes;
    else if (g.types.ById(army->type)->bonus[48] != 0)
        kind = Loss::kCreatures;
    else
        kind = Loss::kArmies;
    int std = 0;
    for (Item* it : army->items)
        if (it->index < 8) std++;
    int rows[2][2] = {{loser, loser}, {killer, loser}};
    for (auto& row : rows) {
        Bump(g, row[0], row[1], kind);
        if (boat) Bump(g, row[0], row[1], Loss::kNavies);
        if (std > 0) Bump(g, row[0], row[1], Loss::kStandards, std);
    }
}

int Triumph(const Game& g, int me, int opp, Loss kind) {
    auto a = g.triumphs.find(me);
    if (a == g.triumphs.end()) return 0;
    auto b = a->second.find(opp);
    if (b == a->second.end()) return 0;
    return b->second[static_cast<int>(kind)];
}

void Record(Game& g) {
    if (g.turn > kLastRecordedTurn) return;
    HistoryRecord r;
    auto win = report::ComputeFigures(g, *g.sides[0], report::kWinning);
    for (int i = 0; i < 8; i++) {
        Side* s = g.map->Side(i);
        r.gold[i] = (s && s->in_use) ? s->gold : 0;
        r.score[i] = win.value[i];
        r.cities[i] = s ? static_cast<int>(game::SideCities(g, *s).size()) : 0;
    }
    for (auto& c : g.map->cities)
        r.owners.push_back(c.razed ? 0xff : (c.owner_index == kNone ? 8 : c.owner_index));
    int n = 0;
    int take[8] = {0};
    for (int i = 0; i < 8; i++) {
        auto it = g.deeds.find(i);
        if (it != g.deeds.end() && it->second.size() >= 1) {
            take[i] = 1;
            n++;
        }
    }
    for (int i = 0; i < 8; i++) {
        auto it = g.deeds.find(i);
        if (it != g.deeds.end() && it->second.size() >= 2 && n < kMaxDeedsPerRound) {
            take[i] = 2;
            n++;
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < take[i]; k++) r.events.push_back(g.deeds[i][k]);
    }
    g.deeds.clear();
    g.history.push_back(r);
}

}  // namespace w2::history
