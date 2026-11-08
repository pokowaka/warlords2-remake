#include "warlords/ai/diplomacy.hpp"

#include <algorithm>

#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"

namespace w2::ai::diplo {

using namespace core;

bool CanAttack(Game& game, Side& side, const City& city) {
    int owner = Owner(city);
    if (owner == kNeutral) return true;
    AIData& data = Data(game, side);
    int wars = 0, last = kNone;
    for (int other = 7; other >= 0; other--) {
        if (InPlay(game, other) && State(game, side.index, other) == 2) {
            wars++;
            last = other;
        }
    }
    int state = State(game, side.index, owner);
    if (state == 2) return true;
    if (state == 0 && wars != 0 && (wars != 1 || last != owner)) return false;
    if (state > 2) return false;
    return data.bold;
}

bool Spared(Game& game, Side& side, int other) {
    AIData& data = Data(game, side);
    if (game.map->options.diplomacy == 0 || data.solidarity == 0 || !side.computer) return false;
    for (int human = 7; human >= 0; human--) {
        const Side& human_side = game.map->sides[human];
        if (InPlay(game, human) && !human_side.computer && State(game, other, human) == 2 &&
            human_side.ai_solidarity != 0)
            return true;
    }
    return false;
}

/**
 * @brief Claims one more city as the side's own ground: the nearest standing
 *        city within 40 of its capital that no side in play claims.
 */
static void Claim(Game& game, Side& side) {
    City* capital = side.capital;
    if (!capital) return;
    City* best = nullptr;
    int best_distance = kNone;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        int claim = city.claim != kNone ? city.claim : kNeutral;
        if (Standing(city) && claim != side.index && (claim == kNeutral || !InPlay(game, claim))) {
            int distance = Dist(city.x, city.y, capital->x, capital->y);
            if (distance < 40 && (best_distance == kNone || distance < best_distance)) {
                best = &city;
                best_distance = distance;
            }
        }
    }
    if (best) best->claim = side.index;
}

void Phase(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int side_index = side.index;
    bool human = !side.computer;
    Claim(game, side);
    if (game.map->options.diplomacy == 0) return;
    int enemy = groups::PickEnemy(game, side);

    // Per side: cities claimed by one and held by the other, cities owned,
    // cities seen, and what it proposes to us.
    int swapped[16] = {0}, total[16] = {0}, seen[16] = {0}, they_war[8] = {0}, they_peace[8] = {0};
    for (int other = 7; other >= 0; other--) {
        Propose(game, side_index, other, 0);
        int proposal = Proposal(game, other, side_index);
        if (proposal == 2)
            they_war[other] = 1;
        else if (proposal == 0)
            they_peace[other] = 1;
    }
    int tiles = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (Standing(city)) tiles++;
        int owner = city.owner_index;
        if (owner != kNone) {
            total[owner]++;
            if (!Cflag(data, city, kCfUnseen)) seen[owner]++;
            int claim = city.claim != kNone ? city.claim : kNeutral;
            if (owner == side_index) {
                if (claim != kNeutral && claim >= 0 && claim < 16) swapped[claim]++;
            } else if (claim == side_index) {
                swapped[owner]++;
            }
        }
    }
    // The grudge tests; each has a war branch that can never run (docs/re/ai.md).
    for (int other = 7; other >= 0; other--) {
        if (other != side_index && InPlay(game, other)) {
            int grudge = (they_peace[other] - they_war[other]) * 2;
            if (swapped[other] >= grudge + 4) Propose(game, side_index, other, 1);
        }
    }
    for (int other = 7; other >= 0; other--) {
        if (other != side_index && InPlay(game, other)) {
            int grudge = (they_peace[other] - they_war[other]) * 2;
            int threat =
                    data.lost[other] + data.cities_lost[other] * 4 + data.heroes_killed[other] * 2;
            if (threat >= grudge + 5)
                Propose(game, side_index, other, std::max(1, Proposal(game, side_index, other)));
        }
    }
    if (enemy != kNone) Propose(game, side_index, enemy, 2);
    for (int group_index = kMaxGroups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group->target != kNone)
            Propose(game, side_index, group->target, 2);
    }
    // Whoever holds too much of the world.
    int leader = kNone, most = 0;
    for (int other = 7; other >= 0; other--) {
        if (InPlay(game, other)) {
            int share = total[other] * 100 / std::max(1, tiles);
            int limit = IsComputer(game, other) ? 50 : data.human_share;
            if (most < total[other] && limit < share) {
                leader = other;
                most = total[other];
            }
        }
    }
    if (leader != kNone && leader != side_index) {
        for (int other = 7; other >= 0; other--) {
            if (InPlay(game, other))
                Propose(game, side_index, other, (other == leader || other == enemy) ? 2 : 0);
        }
    }
    if (!human && data.solidarity != 0) {
        for (int other = 7; other >= 0; other--) {
            if (InPlay(game, other) && IsComputer(game, other) && other != side_index &&
                Proposal(game, side_index, other) != 0 && Spared(game, side, other)) {
                Propose(game, side_index, other, 0);
            }
        }
        bool at_war_with_human = false;
        for (int other = 7; other >= 0; other--) {
            if (InPlay(game, other) && !IsComputer(game, other) &&
                State(game, side_index, other) == 2)
                at_war_with_human = true;
        }
        if (at_war_with_human) {
            for (int other = 7; other >= 0; other--) {
                if (InPlay(game, other) && IsComputer(game, other) &&
                    game.map->sides[other].ai_solidarity != 0)
                    Propose(game, side_index, other, 0);
            }
        }
    }
    if (!human && game.greatest) {
        auto fights = groups::FightingHumans(game);
        for (int other = 7; other >= 0; other--) {
            if (InPlay(game, other) && IsComputer(game, other) &&
                (fights[side_index] || fights[other]))
                Propose(game, side_index, other, 0);
        }
    }
    for (int other = 7; other >= 0; other--)
        if (seen[other] == 0) Propose(game, side_index, other, 0);
    for (int group_index = kMaxGroups; group_index >= 1; group_index--) {
        auto group = data.groups[group_index];
        if (group->active != 0 && group->target != kNone &&
            Proposal(game, side_index, group->target) == 0)
            groups::Cancel(game, side, group_index);
    }
}

}  // namespace w2::ai::diplo
