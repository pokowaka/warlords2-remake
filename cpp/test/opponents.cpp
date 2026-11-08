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

/**
 * @brief Counts the armies of the city's owner standing on its 2x2 tiles.
 */
int Defenders(Game& game, const City& city) {
    int count = 0;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            for (Army* army : game::ArmiesAt(game, city.x + dx, city.y + dy)) {
                if (army->owner == city.owner_index) count++;
            }
        }
    }
    return count;
}

/**
 * @brief Whether side `raider_index` may walk into `city` right now: an enemy city of a
 *        living side, without defenders, that it can enter without breaking a
 *        treaty.
 */
bool Raidable(Game& game, int raider_index, const City& city) {
    if (city.razed || city.owner_index < 0 || city.owner_index >= 8 ||
        city.owner_index == raider_index)
        return false;
    if (!game.map->sides[city.owner_index].alive) return false;
    if (diplomacy::IsAllied(game, raider_index, city.owner_index)) return false;
    if (diplomacy::MustDeclare(game, raider_index, city.owner_index, &city)) return false;
    return Defenders(game, city) == 0;
}

/**
 * @brief Whether moving `army` away would leave one of side `raider_index`'s cities
 *        without defenders.
 */
bool LastDefender(Game& game, int raider_index, Army* army) {
    City* home = game::CityAt(game, army->x, army->y);
    if (!home || home->owner_index != raider_index) return false;
    return Defenders(game, *home) <= 1;
}

}  // namespace

int RaidEmptyCities(Game& game, Side& side) {
    const int raider_index = side.index;
    constexpr int kMaxTriesPerCity = 4;
    constexpr int kMoveCostPerTile = 2;  // cheapest open-ground step

    std::vector<City*> targets;
    for (auto& city : game.map->cities) {
        if (Raidable(game, raider_index, city)) targets.push_back(&city);
    }
    // The learner's (non-Classic) cities first, then the richest
    auto learner_owned = [&](const City* city) {
        return !game.map->sides[city->owner_index].classic_ai;
    };
    std::sort(targets.begin(), targets.end(), [&](City* lhs, City* rhs) {
        if (learner_owned(lhs) != learner_owned(rhs)) return learner_owned(lhs);
        return lhs->income > rhs->income;
    });

    int taken = 0;
    for (City* target : targets) {
        if (!Raidable(game, raider_index, *target)) continue;
        std::vector<Army*> raiders;
        for (Army* army : game.armies) {
            if (army->owner != raider_index || army->x == kNone || army->transit ||
                army->moves <= 0 || army->Hero())
                continue;
            int distance = move::Distance(army->x, army->y, target->x, target->y);
            if (distance * kMoveCostPerTile > army->moves + 2) continue;
            if (LastDefender(game, raider_index, army)) continue;
            raiders.push_back(army);
        }
        // weakest first: a raid needs a body, not strength
        std::sort(raiders.begin(), raiders.end(), [](Army* lhs, Army* rhs) {
            if (lhs->strength != rhs->strength) return lhs->strength < rhs->strength;
            return lhs->moves > rhs->moves;
        });
        int tries = 0;
        for (Army* raider : raiders) {
            if (tries++ >= kMaxTriesPerCity || target->owner_index == raider_index) break;
            if (!game.Alive(raider) || raider->moves <= 0) continue;
            // the corner of the city's 2x2 footprint nearest the raider
            int dest_x = target->x + (target->x < raider->x ? 1 : 0),
                dest_y = target->y + (target->y < raider->y ? 1 : 0);
            auto path = move::FindPath(game, {raider}, raider->x, raider->y, dest_x, dest_y);
            if (!path || path->empty()) continue;
            move::WalkResult walk = move::Walk(game, {raider}, *path);
            if (walk.steps > 0) ai::Walked(game, {raider}, walk);
            if (walk.stopped == "attack" && walk.attack && walk.attack->city == target &&
                game.Alive(raider)) {
                ai::core::Sel sel = ai::core::Select(game, {raider});
                if (!sel.armies.empty()) ai::core::Fight(game, sel, walk.attack->x, walk.attack->y);
            }
            if (target->owner_index == raider_index) taken++;
        }
    }
    return taken;
}

/**
 * @brief Plays one turn for a training opponent: the frozen network alone for
 *        kFrozen, otherwise the Classic AI, preceded by the raid for kRaider.
 */
void PlayTurn(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen) {
    if (kind == Kind::kFrozen && frozen) {
        ai::modern::PlayTurn(game, side, nullptr, frozen, nullptr);
        return;
    }
    if (kind == Kind::kRaider) RaidEmptyCities(game, side);
    ai::PlayTurn(game, side, nullptr);
}

}  // namespace w2::opponents
