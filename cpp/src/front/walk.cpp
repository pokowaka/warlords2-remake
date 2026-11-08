/**
 * @file
 * @brief Stack route planning, movement preview, and step-by-step walk animation.
 */
#include <cmath>

#include "front/internal.hpp"
#include "platform/display.hpp"
#include "platform/sound.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

// ------------------------------------------------------------------ walking

// A stack walks one tile at a time, and the map shows where it is going. The
// destination is kept in the army record itself, so it outlives a walk that
// could not finish. The original marks the route: a ring on every tile but
// the last, plain while the stack can still reach it this turn and crossed
// once it cannot, and a ghost of the leading army on the last (docs/re/ui.md).
//
// In ASHADOW.PCK: the plain ring, and the crossed one for out of reach this
// turn; then where in its tile a ring, and the ghost, are drawn.
static const int kWalkRing[2] = {496, 31}, kWalkCrossed[2] = {496, 48};
static const int kWalkRingW = 16, kWalkRingH = 14;
static const int kWalkRingAt[2] = {16, 13}, kWalkGhostAt[2] = {4, 4};
static const double kWalkStep = 0.1;  ///< Seconds the walk spends on one tile.

/**
 * @brief Works out the route to the selection's destination, if it has one.
 *
 * A stack already standing on its destination forgets it.
 */
void RefreshRoute() {
    g_app.route.reset();
    if (!g_app.selection || g_app.selection->stack.empty()) return;
    auto& sel = *g_app.selection;
    auto target = sel.stack[0]->target;
    if (!target) return;
    if (sel.stack[0]->x == target->first && sel.stack[0]->y == target->second) {
        for (Army* army : sel.stack) army->target.reset();
        return;
    }
    g_app.route = move::PreviewRoute(*g_app.g, sel.stack, target->first, target->second);
}

/** @brief Gives every army in the stack the destination (x, y); kNone clears it. */
void OrderTo(const std::vector<Army*>& stack, int x, int y) {
    for (Army* army : stack) {
        if (x == kNone)
            army->target.reset();
        else
            army->target = std::make_pair(x, y);
    }
}

/**
 * @brief Alt and a click: the route to (x, y) is drawn but not walked.
 *
 * A click on the stack's own tile clears its destination.
 */
void PlanRoute(int x, int y) {
    if (!g_app.selection || g_app.selection->stack.empty()) return;
    auto& sel = *g_app.selection;
    if (!game::Seen(*g_app.g, g_app.player->index, x, y)) return;
    if (sel.stack[0]->x == x && sel.stack[0]->y == y) {
        OrderTo(sel.stack, kNone, kNone);
        g_app.route.reset();
        return;
    }
    OrderTo(sel.stack, x, y);
    RefreshRoute();
    if (!g_app.route) sound::Effect("chord");
}

/**
 * @brief Plays the walk back a tile at a time, centring on the stack as it goes.
 * @param sea_before The stack was at sea as it set off, and is shown so to
 *        the end of the walk: the original puts it to sea or ashore only
 *        after its last step.
 */
void StartWalk(const std::vector<Army*>& armies, const std::vector<std::pair<int, int>>& tiles,
               bool sea_before) {
    if (tiles.empty()) return;
    Walk walk;
    walk.sea_before = sea_before;
    walk.armies = std::set<Army*>(armies.begin(), armies.end());
    walk.tiles = tiles;
    walk.i = 0;
    walk.at = w2::Now();
    walk.top = armies.empty() ? nullptr : armies[0];
    g_app.walk = walk;
    CentreOn(tiles[0].first, tiles[0].second);
}

void MoveAllStep();
void FlushKeys();

/**
 * @brief Puts down a stack out of moves after its walk.
 *
 * It is done for the turn and put down, with nothing selected. A goal it has
 * reached is forgotten.
 */
void PutDown(const std::vector<Army*>& stack) {
    Army* lead = stack.empty() ? nullptr : stack[0];
    bool arrived = lead && lead->target && lead->x == lead->target->first &&
                   lead->y == lead->target->second;
    for (Army* army : stack) {
        army->done = true;
        if (arrived) army->target.reset();
    }
    g_app.selection.reset();
    g_app.route.reset();
}

/**
 * @brief Moves the walk on by the time that has passed, and finishes it once
 *        its last tile has had its turn.
 *
 * A finished walk puts down a stack out of moves, and hands on to whoever
 * waits for it: the computer's turn, Move All, or the keys typed meanwhile.
 */
void AdvanceWalk() {
    if (!g_app.walk) return;
    Walk& walk = *g_app.walk;
    double now = w2::Now();
    while (walk.i < walk.tiles.size() - 1 && now - walk.at >= kWalkStep) {
        walk.i++;
        walk.at += kWalkStep;
        CentreOn(walk.tiles[walk.i].first, walk.tiles[walk.i].second);
    }
    if (walk.i >= walk.tiles.size() - 1 && now - walk.at >= kWalkStep) {
        bool computer = walk.computer;
        auto down = walk.put_down;
        g_app.walk.reset();
        if (!down.empty()) PutDown(down);
        RefreshRoute();
        if (computer && g_app.ai_run) ResumeComputer();
        if (g_app.move_all) MoveAllStep();
        if (!g_app.walk && !g_app.move_all) FlushKeys();
    }
}

/** @brief The tile the walk has got to, or nullptr with no walk. */
const std::pair<int, int>* WalkingAt() {
    return g_app.walk ? &g_app.walk->tiles[g_app.walk->i] : nullptr;
}

/** @brief Draws the selection's route: rings to the last tile, the lead's ghost on it. */
void DrawRoute() {
    if (!g_app.route) return;
    const auto& route = *g_app.route;
    Army* ghost = g_app.selection && !g_app.selection->stack.empty() ? g_app.selection->stack[0]
                                                                     : nullptr;
    // while the walk plays out the route is drawn from where it has got to
    size_t first = g_app.walk ? g_app.walk->i + 1 : 1;
    for (size_t i = first; i <= route.path.size(); i++) {
        const auto& step = route.path[i - 1];
        if (!Shown(step.x, step.y)) continue;
        int pixel_x = step.x * kTile, pixel_y = step.y * kTile;
        gfx::SetColor(1, 1, 1);
        if (i == route.path.size()) {
            if (ghost)
                gfx::Draw(g_app.shadow_img, kit::ArmyQuad(ghost->type), pixel_x + kWalkGhostAt[0],
                          pixel_y + kWalkGhostAt[1]);
        } else {
            const int* src = static_cast<int>(i) <= route.reach ? kWalkRing : kWalkCrossed;
            gfx::Draw(g_app.shadow_img, gfx::NewQuad(src[0], src[1], kWalkRingW, kWalkRingH),
                      pixel_x + kWalkRingAt[0], pixel_y + kWalkRingAt[1]);
        }
    }
}

/** @brief A UI point to the map tile under it, or none outside the viewport. */
std::optional<std::pair<int, int>> TileAtPoint(int point_x, int point_y) {
    const auto& rect = g_app.map_rect;
    if (point_x < rect.x || point_y < rect.y || point_x >= rect.x + rect.w ||
        point_y >= rect.y + rect.h)
        return std::nullopt;
    auto [tile_x, tile_y] = UiToMap(point_x, point_y);
    return std::make_pair(static_cast<int>(std::floor(tile_x)),
                          static_cast<int>(std::floor(tile_y)));
}

}  // namespace front
