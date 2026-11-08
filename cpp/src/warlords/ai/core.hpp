// The computer players' shared machinery: their per-side data, the city
// neighbour table, the stack they have "selected", the flood fill they
// measure distances with, the battles they simulate, and the walk that moves
// a stack and fights what it meets.
//
// A port of WARLORD2.EXE; the addresses are Ghidra's (docs/re/ai.md).
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "warlords/combat.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/types.hpp"

namespace w2 {

/** An assault group (5f19:08ba). Its lists are keyed 1-6 (members and staged
 *  1-4) with holes, as the original's fixed slots are. */
struct Group {
    int active = 0;
    int target = kNone;                                      // the side it goes for
    int rally = kNone;                                       // the city it gathers at
    std::map<int, int> members, cities, dist, bonus, taken;  // city indices, distances
    std::map<int, Army*> staged;
    int size = 0, flags = 0;
};

/** A side's block of AI data (ai_init_side, 59bf:084d). */
struct AIData {
    int turns = 0;
    int own = 0, enemy = 0, neutral = 0, unseen = 0;
    int rebuild_limit = 10;
    bool bold = false;
    int heroes = 0;
    int rebuild_type = 0, rebuild_type_rich = 0;
    int bought = 0;
    int die_human = 0, die_lord = 0, die_knight = 0, die_warlord = 0;
    int sims = 10;
    int searchers = 0, explorers = 0;
    int raze = 0, sack = 0, pillage = 0, per_city = 0;
    int bonus_human = 0, bonus_warlord = 0, bonus_lord = 0, bonus_knight = 0;
    int poor = 0;
    int solidarity = 0;
    int min_strength = 0, fly_cities = 0, strong_cities = 0, fast_cities = 0;
    int early = 0;
    int human_share = 80;
    int quest_city = kNone;
    int cautious = 0;
    int quests_done = 0, items_passed = 0;
    int max_groups = 1;
    std::map<int, int> roles, held, flags, garrison, keep;  // by city index
    // 1-based, as the original numbers them: an army's aiGroup is the number,
    // and 0 means none. Held by pointer as the Lua holds tables: cancelling a
    // group puts a fresh one in its slot, and whoever still holds the old one
    // goes on with it, as the original's code does.
    std::array<std::shared_ptr<Group>, 5> groups;
    std::array<int, 8> heroes_killed{}, armies_killed{}, battles{}, lost{}, city_battles{},
            cities_lost{};
    std::optional<std::pair<int, int>> cursor;
};

/** Each city's six nearest cities by land path, by city index. */
struct Neighbours {
    struct Entry {
        std::vector<int> cities;
        std::vector<int> dist;
    };
    std::vector<Entry> by_city;
};

namespace ai::core {

constexpr int kNeutral = rules::kNeutral;

// city roles, AI data +0x56 + city (docs/re/ai.md > City roles)
constexpr int kJustTaken = 1, kTakingNeutral = 2, kNearNeutral = 3;
constexpr int kWeak = 4, kBuilding = 5, kMember = 6, kRally = 7;
constexpr int kStop = 8, kExploreR2 = 11, kExplorer = 13, kNowhere = 14;

// an army's standing order, the low nibble of record +14
constexpr int kOrderNone = 0, kOrderCity = 1, kOrderItem = 2;
constexpr int kOrderSite = 3, kOrderRoam = 4;

// per-city AI flags, +0x11e + city
constexpr int kCfUnseen = 0x01, kCfFlier = 0x02, kCfCleaned = 0x04;
constexpr int kCfMovE12 = 0x08, kCfMagic = 0x10, kCfHero = 0x20;
constexpr int kCfFlygroup = 0x40, kCfMovE16 = 0x80;

// an assault group's flags, entry +0x5a
constexpr int kGfRaze = 0x01, kGfSack = 0x02, kGfPillage = 0x04;
constexpr int kGfMovE16 = 0x08, kGfMovE12 = 0x10, kGfFly = 0x20;

constexpr int kMaxGroups = 4;
constexpr int kUnreached = 30001;

inline bool Has(int v, int bit) {
    return (v & bit) != 0;
}
inline int Set(int v, int bit) {
    return v | bit;
}
inline int Clear(int v, int bit) {
    return v & ~bit;
}
inline int Get(const std::map<int, int>& m, int k) {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
}
inline bool HasKey(const std::map<int, int>& m, int k) {
    return m.count(k) > 0;
}

/** map_distance (2012:1199): the straight-line distance, truncated. */
int Dist(int x1, int y1, int x2, int y2);
inline int Owner(const City& c) {
    return c.owner_index == kNone ? kNeutral : c.owner_index;
}
/** Is the city still a city (not ruins)? */
inline bool Standing(const City& c) {
    return !c.razed;
}
/** Is a side in the game? */
bool InPlay(const Game& g, int index);
bool IsComputer(const Game& g, int index);
/** The diplomatic state between two sides. */
int State(const Game& g, int a, int b);
/** A side's proposal to another. */
int Proposal(const Game& g, int a, int b);
void Propose(Game& g, int a, int b, int v);
/** The city whose footprint covers (x, y). */
City* CityAt(const Game& g, int x, int y);
/** The site on (x, y) (828e:04b5). */
Site* SiteAt(const Game& g, int x, int y);
int Terrain(const Game& g, int x, int y);
/** A temple, or a ruin nobody has searched. */
bool SiteOpen(const Site& s);
/** Has the side seen (x, y), or any tile next to it (623c:16ae)? */
bool Explored(const Game& g, int sd, int x, int y);
bool Flies(const Game& g, const Army* a);
bool Magical(const Game& g, const Army* a);
int Ability(const Game& g, const Army* a);
bool Woods(const Game& g, const Army* a);
bool Hills(const Game& g, const Army* a);
inline bool IsHero(const Army* a) {
    return a->Hero();
}
/** The side's armies, last record first. */
std::vector<Army*> Armies(const Game& g, int side_index);
/** Every placed army of a side on (x, y), last record first. */
std::vector<Army*> OnTile(const Game& g, int side_index, int x, int y);
/** Is an army still in the game? */
inline bool Alive(const Game& g, const Army* a) {
    return g.Alive(a);
}

/** A fresh block of AI data (ai_init_side, 59bf:084d). */
AIData NewData();
/** An empty assault group (5f19:08ba). */
inline std::shared_ptr<Group> EmptyGroup() {
    return std::make_shared<Group>();
}
/** The AI data of a side, made on first use. */
AIData& Data(Game& g, Side& sd);
AIData& Data(Game& g, int side_index);
inline int Role(const AIData& d, const City& c) {
    return Get(d.roles, c.index);
}
inline void SetRole(AIData& d, const City& c, int r) {
    d.roles[c.index] = r;
}
inline bool Cflag(const AIData& d, const City& c, int bit) {
    return Has(Get(d.flags, c.index), bit);
}
inline void SetCflag(AIData& d, const City& c, int bit) {
    d.flags[c.index] = Set(Get(d.flags, c.index), bit);
}
inline void ClearCflag(AIData& d, const City& c, int bit) {
    d.flags[c.index] = Clear(Get(d.flags, c.index), bit);
}

/** A city's neighbours: {cities, dists}. */
std::pair<std::vector<City*>, std::vector<int>> FindNeighbours(Game& g, const City& c);
/** For every city: flood by land from it (45 rounds, or 60 when that finds
 *  no city), as side 0 with every tile seen, and pick six (623c:0749). */
std::shared_ptr<Neighbours> BuildNeighbours(Game& g);

/** The stack a computer has "selected". */
struct Sel {
    std::vector<Army*> armies;
    Army* leader = nullptr;
    int min_moves = 0;
    int mode = move::kLand;
    Army* hero = nullptr;
};
/** The movement points a selection shares, and how it moves. */
Sel& Refresh(Game& g, Sel& sel);
/** Select a list of armies as one stack (623c:14d3). The leader is the first. */
Sel Select(Game& g, const std::vector<Army*>& list);
/** The lowest group number from 2 up that none of the side's armies uses
 *  (1b62:0cde). */
int NewGroup(const Game& g, int side_index);
/** Select a list and give it an order (623c:0ae7); none for an empty one. */
std::optional<Sel> Order(Game& g, const std::vector<Army*>& list, int ord, int dest, int flags);
/** Where an order points an army (623c:0c7e). */
void SetTarget(Game& g, Army* a, int ord, int dest);
/** Forget an army's order. */
inline void ClearOrder(Army* a) {
    a->ai_order = kOrderNone;
    a->ai_dest = kNone;
}
/** Select the stack an army moves with (8c07:06eb); they are marked moved. */
Sel SelectStackOf(Game& g, Army* a);
/** The side's armies on a tile with at least `minMoves` moves left, up to
 *  eight (623c:12c9). */
std::vector<Army*> Collect(const Game& g, int side_index, int x, int y, int min_moves);
/** The side's armies on a tile in group nibble `grp` with standing order
 *  `ord`, and a maximum move of at least `minMax` (623c:13b2). */
std::vector<Army*> CollectOrdered(const Game& g, int side_index, int x, int y, int grp, int ord,
                                  int min_max);

/** A flood's grid (move::flood). */
struct Flood {
    std::vector<int> dist;
    int w = 0, h = 0;
};
/** floodFrom's `fogSide` for the flooding side's own hidden map. */
constexpr int kSameSide = -3;
/** Flood the map from (x, y) the way the selection moves (1555:1ab9): the
 *  original's bounded sweep (move::flood) over `sideIndex`'s cost grid, at
 *  most `range` rounds. `fogSide` is whose hidden map counts, the flooding
 *  side's by default (a computer's: none). */
Flood FloodFrom(Game& g, int side_index, int x, int y, int range, const Sel* sel,
                int fog_side = kSameSide);
Flood FloodFrom(Game& g, int side_index, int x, int y, int range, int mode,
                int fog_side = kSameSide);
/** The flood's cost to (x, y), plus 1, or UNREACHED. */
int FloodAt(const Flood& flood, int x, int y);
/** The flood's distance to a city: {best, bx, by}, the least over the twelve
 *  tiles round it, or 100 (1000 with `far`) when none is closer, bx NONE
 *  then (59bf:0a85). */
std::tuple<int, int, int> CityDistance(const Game& g, const Flood& flood, const City& c,
                                       bool far = false);
/** The chance, in percent, that the selection takes (x, y) (623c:15c5). */
int Odds(Game& g, const Sel& sel, int x, int y);
/** A hero picks up whatever lies where it stands (6087:0427). */
void PickUp(Game& g, const Sel& sel);
/** Fight for (x, y) with the selection, for real. */
Battle Fight(Game& g, Sel& sel, int x, int y);
/** Mark the selection done for the turn (8c07:08fb). */
void Done(const Sel& sel);
/** move_stack_to (1a8b:0001) for a computer: walk to (tx, ty), fighting what
 *  the walk runs into, until the walk stops. Returns the last walk code. */
int MoveTo(Game& g, Sel& sel, int tx, int ty, bool keep = false);
/** Attack a city the walk has run into (1a8b:0254): {again, x, y}. */
std::tuple<bool, int, int> AttackCity(Game& g, Sel& sel, City* dest, int ax, int ay);
/** Is the city worth attacking with this stack, or is there better nearby
 *  (623c:1771)? */
City* Retarget(Game& g, Sel& sel, City* c, int x, int y);
/** The best city for the selection to go for, by the flood (623c:1885). */
City* BestCity(Game& g, const Sel& sel, const Flood& flood, bool skip_own);

}  // namespace ai::core
}  // namespace w2
