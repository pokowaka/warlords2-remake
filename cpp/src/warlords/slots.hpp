// The eight army slots under the map, and which of their armies move.
//
// Clicking a tile does not select the whole stack: it selects one army, and
// the bottom bar is where the player builds up the group that will move. The
// original keeps three parallel arrays over the tile's armies -- the group
// each belongs to, whether that group is the one moving, and the mark drawn
// under it. Build() builds them, the sorting and marking happens on every
// change, and Commit() writes the grouping back into the armies.
// docs/re/ui.md > The army slots.
//
// Slots are numbered from 0 here.
#pragma once

#include <array>
#include <optional>
#include <set>
#include <vector>

#include "warlords/types.hpp"

namespace w2::slots {

/** @brief How many slots the bar has; armies past the eighth are not shown. */
constexpr int kMax = 8;

/**
 * @brief The mark under a slot: the head of the group that moves takes the
 * tick, the head of every other group the cross, every other slot nothing.
 */
constexpr int kNomark = -1, kCross = 0, kTick = 1;

/** @brief Group 0 means "not grouped": each such army is a group of its own. */
constexpr int kUngrouped = 0;

/**
 * @brief The army slots for one tile: which army sits where, the group each
 * belongs to, and which of them move.
 *
 * Slots of the same group are kept next to each other, ordered by fight order.
 */
struct Slots {
    int n = 0;                          ///< How many slots are filled (at most kMax).
    int side = kNone;                   ///< The side owning the armies.
    std::array<Army*, kMax> army{};     ///< The army in each slot.
    std::array<int, kMax> group{};      ///< Each slot's group number, counted from 0.
    std::array<bool, kMax> in_group{};  ///< Whether the slot's army is among those that move.
    std::array<int, kMax> mark{};  ///< The mark drawn under each slot (kNomark, kCross, kTick).
    std::optional<int> active;     ///< The group that moves, when exactly one whole group does.
};

/**
 * @brief Which armies a click on the tile picks up.
 *
 * The clicked army (`anchor`, or else the one highest in the fight order) and
 * every army that shares its remembered group.
 */
std::set<Army*> Clicked(const Game& game, const std::vector<Army*>& armies, int side,
                        Army* anchor = nullptr);
/**
 * @brief The slots for the armies standing on one tile, at most eight of them.
 * @param selected The set of armies that should be moving (null: what a click picks up).
 */
Slots Build(const Game& game, const std::vector<Army*>& armies, int side,
            const std::set<Army*>* selected = nullptr);
/**
 * @brief Click a slot (controls 224-231): add that army to the moving group,
 * or drop it out into a group of its own.
 */
void Toggle(Slots& slots, const Game& game, int slot);
/** @brief Click the mark under a slot (controls 232-239): that slot's group moves. */
void PickGroup(Slots& slots, const Game& game, int slot);
/** @brief The Grp button while it is red, and the space bar (control 240): everyone moves. */
void All(Slots& slots, const Game& game);
/** @brief The Grp button while it is green (control 241): only the first army moves. */
void Single(Slots& slots, const Game& game);
/** @brief The armies that move. */
std::vector<Army*> Selected(const Slots& slots);
/** @brief Group Move: the pace of the slowest army in the group. */
int Moves(const Slots& slots);
/** @brief Whether the Grp button shows green: every army moves (or there is only one). */
bool Grouped(const Slots& slots);
/**
 * @brief Rebuild the slots over a fresh list of armies, keeping whichever of
 * them were moving.
 * @return std::nullopt when no armies are left.
 */
std::optional<Slots> Keep(const Slots& slots, const Game& game, const std::vector<Army*>& armies);
/**
 * @brief Write the grouping back into the armies, the way the original does
 * when the selection is done.
 *
 * Lone armies become ungrouped, the moving group gets a group id of its own
 * (reusing one of its old ids where it can), and every other group gets 1.
 */
void Commit(Slots& slots, Game& game);

}  // namespace w2::slots
