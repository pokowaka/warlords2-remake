/**
 * @file
 * @brief The classic computer players' shared machinery.
 *
 * Their per-side data, the city neighbour table, the stack they have
 * "selected", the flood fill they measure distances with, the battles they
 * simulate, and the walk that moves a stack and fights what it meets.
 *
 * A port of the original game's computer player, by way of the reference
 * (the Lua remake); docs/re/ai.md describes what is known of it.
 */
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

/**
 * @brief An assault group: a strike force gathering at one of the side's
 *        cities to take a list of an enemy's cities.
 *
 * Its lists are keyed 1-6 (members and staged 1-4) with holes, as the
 * original's fixed slots are. See docs/re/ai.md > Assault groups.
 */
struct Group {
    int active = 0;      ///< Non-zero while running; counts up each turn it goes on.
    int target = kNone;  ///< The side it goes for.
    int rally = kNone;   ///< The city it gathers at, one of the side's own.
    std::map<int, int> members, cities, dist, bonus,
            taken;                ///< By slot: member cities (1-4), target cities (1-6), their
                                  ///< distances and bonuses, and cities taken (1-6); city indices.
    std::map<int, Army*> staged;  ///< By slot 1-4: the lead army of each stack on its way.
    int size = 0,
        flags = 0;  ///< The stack size it wants, and its kGf* flags (spoils, speed, fliers).
};

/**
 * @brief A side's block of computer-player data.
 *
 * Filled at game start from the level's defaults and then the side's
 * character card (docs/formats/crd.md); see docs/re/ai.md > AI data.
 */
struct AIData {
    int turns = 0;  ///< Turns the evaluate phase has run for the side; no assaults before one.
    int own = 0, enemy = 0, neutral = 0,
        unseen = 0;          ///< Cities owned by the side, by enemies, neutral, and not yet seen.
    int rebuild_limit = 10;  ///< Rebuilding's city limit (scaled by the number of cities).
    bool bold = false;       ///< May attack a side it is not at war with when at war with nobody.
    int heroes = 0;          ///< Heroes the side has.
    int rebuild_type = 0,
        rebuild_type_rich = 0;  ///< Army type rebuilding buys below and from 2000 gold.
    int bought = 0;             ///< Production types bought.
    int die_human = 0, die_lord = 0, die_knight = 0,
        die_warlord = 0;               ///< The enemy-picking constant for each kind of side.
    int sims = 10;                     ///< Battles simulated to estimate the odds of an attack.
    int searchers = 0, explorers = 0;  ///< Heroes searching sites, armies exploring.
    int raze = 0, sack = 0, pillage = 0,
        per_city = 0;  ///< Per-mille spoils chances, and the bonus per city owned.
    int bonus_human = 0, bonus_warlord = 0, bonus_lord = 0,
        bonus_knight = 0;  ///< Spoils bonus by the kind of side.
    int poor = 0;          ///< Added to sack and pillage chances under 100 gold.
    int solidarity = 0;    ///< Won't make war on a computer at war with a human.
    int min_strength = 0, fly_cities = 0, strong_cities = 0,
        fast_cities = 0;     ///< What the side's cities build well for the groups.
    int early = 0;           ///< Early vengeance: sacks or razes a human's city before turn 10.
    int human_share = 80;    ///< Percent of all cities a human must hold to be declared war on.
    int quest_city = kNone;  ///< The city a quest wants taken, left to the hero.
    int cautious = 0;        ///< No quick attacks; one army more before taking a neutral.
    int quests_done = 0, items_passed = 0;  ///< Quests finished and items handed on.
    int max_groups = 1;                     ///< Assault groups allowed at once.
    std::map<int, int> roles, held, flags, garrison,
            keep;  ///< By city index: role, turns held, kCf* flags, garrison size, and
                   ///< armies kept on the city's first tile.
    /// The assault groups, 1-based as the original numbers them: an army's
    /// ai_group is the number, and 0 means none. Held by pointer as the
    /// reference holds tables: cancelling a group puts a fresh one in its
    /// slot, and whoever still holds the old one goes on with it, as the
    /// original's code does.
    std::array<std::shared_ptr<Group>, 5> groups;
    std::array<int, 8> heroes_killed{}, armies_killed{}, battles{}, lost{}, city_battles{},
            cities_lost{};  ///< Battle statistics, by the other side (docs/re/ai.md).
    std::optional<std::pair<int, int>> cursor;  ///< Where the side's last move was made.
};

/** @brief Each city's six nearest cities by land path, by city index. */
struct Neighbours {
    /** @brief One city's neighbours. */
    struct Entry {
        std::vector<int> cities;  ///< City indices, nearest picked first.
        std::vector<int> dist;    ///< The path distance to each.
    };
    std::vector<Entry> by_city;  ///< By city index.
};

namespace ai::core {

constexpr int kNeutral = rules::kNeutral;  ///< The neutral side's index.

// City roles (docs/re/ai.md > City roles).
constexpr int kJustTaken = 1;      ///< Just captured.
constexpr int kTakingNeutral = 2;  ///< Armies are on their way to a neutral city.
constexpr int kNearNeutral = 3;    ///< Has a neutral city among its neighbours.
constexpr int kWeak = 4;           ///< Garrison too small, or no neutral neighbours left.
constexpr int kBuilding = 5;       ///< Garrison below the wanted size.
constexpr int kMember = 6;         ///< Member of an assault group; vectors to it.
constexpr int kRally = 7;          ///< An assault group's rally city.
constexpr int kStop = 8;           ///< Stop producing here.
constexpr int kExploreR2 = 11;     ///< Explorer, set on turn 2.
constexpr int kExplorer = 13;      ///< Explorer, set on turn 1.
constexpr int kNowhere = 14;       ///< Nothing left to vector to.

// An army's standing order.
constexpr int kOrderNone = 0;  ///< No order.
constexpr int kOrderCity = 1;  ///< Go for a city.
constexpr int kOrderItem = 2;  ///< Go for an item.
constexpr int kOrderSite = 3;  ///< Go for a site (a ruin or temple).
constexpr int kOrderRoam = 4;  ///< Roam: no fixed destination.

// Per-city AI flags.
constexpr int kCfUnseen = 0x01;    ///< Not seen yet (only with a hidden map).
constexpr int kCfFlier = 0x02;     ///< Produces a flier.
constexpr int kCfCleaned = 0x04;   ///< The garrison has been checked this turn.
constexpr int kCfMovE12 = 0x08;    ///< Builds a strong army with more than 11 moves.
constexpr int kCfMagic = 0x10;     ///< The garrison has a magical army.
constexpr int kCfHero = 0x20;      ///< The garrison has a hero.
constexpr int kCfFlygroup = 0x40;  ///< Builds fliers for the flying group.
constexpr int kCfMovE16 = 0x80;    ///< Builds a strong army with more than 15 moves.

// An assault group's flags.
constexpr int kGfRaze = 0x01;     ///< Razes what it takes.
constexpr int kGfSack = 0x02;     ///< Sacks what it takes.
constexpr int kGfPillage = 0x04;  ///< Pillages what it takes.
constexpr int kGfMovE16 = 0x08;   ///< Wants armies with more than 15 moves.
constexpr int kGfMovE12 = 0x10;   ///< Wants armies with more than 11 moves.
constexpr int kGfFly = 0x20;      ///< Wants fliers.

constexpr int kMaxGroups = 4;      ///< Assault groups a side can run at once.
constexpr int kUnreached = 30001;  ///< FloodAt's answer for a tile the flood did not reach.

/** @brief Is `bit` set in `value`? */
inline bool Has(int value, int bit) {
    return (value & bit) != 0;
}
/** @brief `value` with `bit` set. */
inline int Set(int value, int bit) {
    return value | bit;
}
/** @brief `value` with `bit` cleared. */
inline int Clear(int value, int bit) {
    return value & ~bit;
}
/** @brief The value under `key`, or 0 when there is none. */
inline int Get(const std::map<int, int>& values, int key) {
    auto entry = values.find(key);
    return entry == values.end() ? 0 : entry->second;
}
/** @brief Is there a value under `key`? */
inline bool HasKey(const std::map<int, int>& values, int key) {
    return values.count(key) > 0;
}

/** @brief The straight-line distance between two tiles, truncated. */
int Dist(int x1, int y1, int x2, int y2);
/** @brief The city's owner, with the neutral side for an unowned city. */
inline int Owner(const City& city) {
    return city.owner_index == kNone ? kNeutral : city.owner_index;
}
/** @brief Is the city still a city (not ruins)? */
inline bool Standing(const City& city) {
    return !city.razed;
}
/** @brief Is a side in the game? */
bool InPlay(const Game& game, int index);
/** @brief Is a side played by the computer? */
bool IsComputer(const Game& game, int index);
/**
 * @brief The diplomatic state between two sides.
 *
 * Always war (2) with the neutral side or no side, and peace (0) with
 * oneself.
 */
int State(const Game& game, int side_index, int other_index);
/** @brief A side's proposal to another; none (0) to itself. */
int Proposal(const Game& game, int side_index, int other_index);
/** @brief Make a side's proposal to another; nothing to itself. */
void Propose(Game& game, int side_index, int other_index, int proposal);
/** @brief The city whose footprint covers (x, y). */
City* CityAt(const Game& game, int x, int y);
/** @brief The site on (x, y), or null. */
Site* SiteAt(const Game& game, int x, int y);
/** @brief The terrain of (x, y). */
int Terrain(const Game& game, int x, int y);
/** @brief Is the site worth a visit: a temple, or a ruin nobody has searched? */
bool SiteOpen(const Site& site);
/** @brief Has the side seen (x, y), or any tile next to it? */
bool Explored(const Game& game, int side_index, int x, int y);
/** @brief Does the army's type fly? */
bool Flies(const Game& game, const Army* army);
/** @brief Is the army's type magical? */
bool Magical(const Game& game, const Army* army);
/** @brief The army type's special ability value. */
int Ability(const Game& game, const Army* army);
/** @brief Does the army's type move well in woods? */
bool Woods(const Game& game, const Army* army);
/** @brief Does the army's type move well in hills? */
bool Hills(const Game& game, const Army* army);
/** @brief Is the army a hero? */
inline bool IsHero(const Army* army) {
    return army->Hero();
}
/** @brief The side's armies, last record first. */
std::vector<Army*> Armies(const Game& game, int side_index);
/** @brief Every placed army of a side on (x, y), last record first. */
std::vector<Army*> OnTile(const Game& game, int side_index, int x, int y);
/** @brief Is an army still in the game? */
inline bool Alive(const Game& game, const Army* army) {
    return game.Alive(army);
}

/** @brief A fresh block of AI data, with empty assault groups. */
AIData NewData();
/** @brief An empty assault group. */
inline std::shared_ptr<Group> EmptyGroup() {
    return std::make_shared<Group>();
}
/** @brief The AI data of a side, made on first use. */
AIData& Data(Game& game, Side& side);
/** @brief The AI data of a side by index, made on first use. */
AIData& Data(Game& game, int side_index);
/** @brief The city's role for the side (0 when it has none). */
inline int Role(const AIData& data, const City& city) {
    return Get(data.roles, city.index);
}
/** @brief Give the city a role for the side. */
inline void SetRole(AIData& data, const City& city, int role) {
    data.roles[city.index] = role;
}
/** @brief Is the per-city flag `bit` (kCf*) set? */
inline bool Cflag(const AIData& data, const City& city, int bit) {
    return Has(Get(data.flags, city.index), bit);
}
/** @brief Set the per-city flag `bit` (kCf*). */
inline void SetCflag(AIData& data, const City& city, int bit) {
    data.flags[city.index] = Set(Get(data.flags, city.index), bit);
}
/** @brief Clear the per-city flag `bit` (kCf*). */
inline void ClearCflag(AIData& data, const City& city, int bit) {
    data.flags[city.index] = Clear(Get(data.flags, city.index), bit);
}

/**
 * @brief A city's neighbours: {cities, distances}.
 *
 * Builds the shared neighbour table on first use.
 */
std::pair<std::vector<City*>, std::vector<int>> FindNeighbours(Game& game, const City& city);
/**
 * @brief Builds the neighbour table.
 *
 * For every city: flood by land from it (45 rounds, or 60 when that finds no
 * city), as side 0 with every tile seen, and pick six.
 */
std::shared_ptr<Neighbours> BuildNeighbours(Game& game);

/** @brief The stack a computer has "selected". */
struct Sel {
    std::vector<Army*> armies;  ///< The armies in the stack.
    Army* leader = nullptr;     ///< The army whose position and target the stack follows.
    int min_moves = 0;          ///< The movement points the armies share.
    int mode = move::kLand;     ///< How the stack moves (land, water, flying).
    Army* hero = nullptr;       ///< The first hero in the stack, if any.
};
/** @brief Recomputes the movement points a selection shares, how it moves, and its hero. */
Sel& Refresh(Game& game, Sel& sel);
/** @brief Selects a list of armies as one stack. The leader is the first. */
Sel Select(Game& game, const std::vector<Army*>& list);
/** @brief The lowest group number from 2 up that none of the side's armies uses. */
int NewGroup(const Game& game, int side_index);
/**
 * @brief Selects a list and gives it a standing order; none for an empty list.
 * @param order The standing order (kOrder*).
 * @param dest The order's city, item or site index.
 * @param flags 0x80 marks the armies as going for a neutral, 0x20 as
 *        exploring, 0x100 as a party.
 */
std::optional<Sel> Order(Game& game, const std::vector<Army*>& list, int order, int dest,
                         int flags);
/** @brief Points an army at the tile its order leads to. */
void SetTarget(Game& game, Army* army, int order, int dest);
/** @brief Forgets an army's order. */
inline void ClearOrder(Army* army) {
    army->ai_order = kOrderNone;
    army->ai_dest = kNone;
}
/** @brief Selects the stack an army moves with; they are marked moved. */
Sel SelectStackOf(Game& game, Army* army);
/** @brief The side's armies on a tile with at least `min_moves` moves left, up to eight. */
std::vector<Army*> Collect(const Game& game, int side_index, int x, int y, int min_moves);
/**
 * @brief The side's armies on a tile in group `group` with standing order
 *        `order`, and a maximum move of at least `min_max`, up to eight.
 */
std::vector<Army*> CollectOrdered(const Game& game, int side_index, int x, int y, int group,
                                  int order, int min_max);

/** @brief A flood's grid of movement costs (move::Flood). */
struct Flood {
    std::vector<int> dist;  ///< Cost per tile, row by row.
    int w = 0, h = 0;       ///< The grid's width and height.
};
/** @brief FloodFrom's `fog_side` for the flooding side's own hidden map. */
constexpr int kSameSide = -3;
/**
 * @brief Floods the map from (x, y) the way the selection moves.
 *
 * The original's bounded sweep (move::Flood) over `side_index`'s cost grid,
 * at most `range` rounds. `fog_side` is whose hidden map counts, the
 * flooding side's by default (a computer's: none).
 */
Flood FloodFrom(Game& game, int side_index, int x, int y, int range, const Sel* sel,
                int fog_side = kSameSide);
/** @brief Floods the map from (x, y) in a movement mode, without woods or hills moves. */
Flood FloodFrom(Game& game, int side_index, int x, int y, int range, int mode,
                int fog_side = kSameSide);
/** @brief The flood's cost to (x, y), plus 1, or kUnreached. */
int FloodAt(const Flood& flood, int x, int y);
/**
 * @brief The flood's distance to a city: {best, best_x, best_y}.
 *
 * The least over the twelve tiles round it, or 100 (1000 with `far`) when
 * none is closer; best_x is kNone then.
 */
std::tuple<int, int, int> CityDistance(const Game& game, const Flood& flood, const City& city,
                                       bool far = false);
/** @brief The chance, in percent, that the selection takes (x, y), by simulated battles. */
int Odds(Game& game, const Sel& sel, int x, int y);
/** @brief The selection's hero picks up whatever lies where it stands. */
void PickUp(Game& game, const Sel& sel);
/** @brief Fights for (x, y) with the selection, for real. */
Battle Fight(Game& game, Sel& sel, int x, int y);
/** @brief Marks the selection done for the turn. */
void Done(const Sel& sel);
/**
 * @brief Moves a computer's stack: walks to (target_x, target_y), fighting
 *        what the walk runs into, until the walk stops.
 * @param keep Leave the stack not done, so it can move on.
 * @return The last walk code: 1 no path, 2 stopped short, 3 an enemy army
 *         is next, 4 the path is walked, 5 an enemy city is next.
 */
int MoveTo(Game& game, Sel& sel, int target_x, int target_y, bool keep = false);
/**
 * @brief Attacks a city the walk has run into at (attack_x, attack_y).
 * @return {again, x, y}: whether to walk on, and to where.
 */
std::tuple<bool, int, int> AttackCity(Game& game, Sel& sel, City* dest, int attack_x, int attack_y);
/** @brief Is the city worth attacking with this stack, or is there better nearby? */
City* Retarget(Game& game, Sel& sel, City* city, int x, int y);
/** @brief The best city for the selection to go for, by the flood. */
City* BestCity(Game& game, const Sel& sel, const Flood& flood, bool skip_own);

}  // namespace ai::core
}  // namespace w2
