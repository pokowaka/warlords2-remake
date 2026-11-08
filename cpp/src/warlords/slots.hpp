// The eight army slots under the map, and which of their armies move.
//
// Clicking a tile does not select the whole stack: it selects one army, and
// the bottom bar is where the player builds up the group that will move. The
// original keeps three parallel arrays over the tile's armies -- the group
// each belongs to, whether that group is the one moving, and the mark drawn
// under it. 89e0:0d30 builds them, 89e0:17e7 sorts and marks them, 89e0:000a
// writes the grouping back into the armies. docs/re/ui.md > The army slots.
//
// Slots are numbered from 0 here.
#pragma once

#include <array>
#include <optional>
#include <set>
#include <vector>

#include "warlords/types.hpp"

namespace w2::slots {

constexpr int kMax = 8;

// The mark under a slot (89e0:17e7): the head of the group that moves takes
// the tick, the head of every other group the cross.
constexpr int kNomark = -1, kCross = 0, kTick = 1;

// Group 0 means "not grouped": each such army is a group of its own.
constexpr int kUngrouped = 0;

struct Slots {
    int n = 0;
    int side = kNone;
    std::array<Army*, kMax> army{};
    std::array<int, kMax> group{};
    std::array<bool, kMax> in_group{};
    std::array<int, kMax> mark{};
    std::optional<int> active;
};

/** Which armies a click on the tile picks up (8c07:06eb). */
std::set<Army*> Clicked(const Game& g, const std::vector<Army*>& armies, int side,
                        Army* anchor = nullptr);
/** The slots for the armies standing on one tile, at most eight of them.
 *  `selected` is the set of armies that should be moving (none: a click's). */
Slots Build(const Game& g, const std::vector<Army*>& armies, int side,
            const std::set<Army*>* selected = nullptr);
/** Click a slot (controls 224-231, 89e0:0963): add that army to the moving
 *  group, or drop it out into a group of its own. */
void Toggle(Slots& s, const Game& g, int i);
/** Click the mark under a slot (controls 232-239, 89e0:0910). */
void PickGroup(Slots& s, const Game& g, int i);
/** The Grp button while it is red, and the space bar (control 240). */
void All(Slots& s, const Game& g);
/** The Grp button while it is green (control 241). */
void Single(Slots& s, const Game& g);
/** The armies that move. */
std::vector<Army*> Selected(const Slots& s);
/** Group Move: the pace of the slowest army in the group (1c8c:0912). */
int Moves(const Slots& s);
/** Whether the Grp button shows green (89e0:0567). */
bool Grouped(const Slots& s);
/** Rebuild the slots over a fresh list of armies, keeping whichever of them
 *  were moving. None when none are left. */
std::optional<Slots> Keep(const Slots& s, const Game& g, const std::vector<Army*>& armies);
/** Write the grouping back into the armies (89e0:000a's tail). */
void Commit(Slots& s, Game& g);

}  // namespace w2::slots
