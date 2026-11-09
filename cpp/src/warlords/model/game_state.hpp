/**
 * @file
 * @brief Game aggregate root state: map, sides, armies, turn schedule, diplomacy, fog, and history.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "warlords/model/army.hpp"
#include "warlords/model/army_registry.hpp"
#include "warlords/model/army_type.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/fog_of_war.hpp"
#include "warlords/model/history_types.hpp"
#include "warlords/model/map.hpp"
#include "warlords/model/turn_schedule.hpp"
#include "warlords/rng.hpp"

namespace w2 {

struct Grids;      // move.cpp's cached cost grids
struct PathCache;  // move.cpp's route cache
struct Neighbours;

/** @brief Deliberate faults in the 1993 original reproduced per `Game` instance by default. */
struct Bugs {
    /// Post-battle hero experience checks the *attacker's* type array when
    /// deciding whether a surviving defender was a hero (`hero::BattleExperience`).
    bool hero_experience_reads_attacker_types = true;
};

/** @brief A game in play: the map, the armies, the sides and whose turn it is. */
struct Game {
    Game();
    ~Game();
    Rng rng;                         ///< The game's dice.
    Bugs bugs;                       ///< Per-game bug-compatibility flags.
    std::filesystem::path data_dir;  ///< Where the original's data files are.
    Types types;                     ///< The army types.
    std::unique_ptr<Map> map;        ///< The map and what is on it.
    ArmyRegistry armies;             ///< Pool, slot-ordered table, and spatial tile index.
    int turn = 1;                    ///< The round being played.
    std::vector<std::string> log;    ///< The game's log.
    bool greatest = false;           ///< A game of the Greatest Warlord campaign.
    std::vector<Side*> sides;        ///< The sides in play.
    int current = 0;                 ///< The index of the side to play.
    Side* side = nullptr;            ///< The side to play.
    DiplomacyMatrix diplomacy;       ///< The relations between the sides.
    FogOfWar explored;               ///< Per-side tile exploration masks.
    std::set<int> towers;            ///< Tiles with an encampment.
    bool won = false;                ///< The game is won.
    bool over = false;               ///< The game is over.
    bool no_humans_said = false;     ///< "No humans left" has been told.
    bool surrender_offered = false;  ///< The surrender offer was made.
    /// The offer is put to the player: it waits for the human's turn,
    /// however many computer turns come between.
    bool surrender_shown = false;
    Ending ending;                           ///< How the game is ending.
    std::vector<HistoryRecord> history;      ///< Every round's record.
    std::map<int, std::vector<Deed>> deeds;  ///< By side: this round's deeds.
    /// By side, then by opponent: what History > Triumphs counts (history::Loss).
    std::map<int, std::map<int, std::array<int, 5>>> triumphs;
    std::set<std::string> tutorial_seen;  ///< The tutorial's moments, as shown.
    /// By side: the hero names, and whether each is a heroine's.
    std::map<int, std::vector<std::pair<std::string, bool>>> hero_names;
    std::shared_ptr<Grids> grids;               ///< Cached movement cost grids.
    std::shared_ptr<Neighbours> ai_neighbours;  ///< The computer's view of who borders whom.
    std::shared_ptr<PathCache> path_cache;      ///< Cached routes.
    TurnSchedule turn_schedule;                 ///< Round turn-order permutation and cursor.

    /** @brief Returns the per-game bug-compatibility configuration (const). */
    const Bugs& RulesBugs() const { return bugs; }
    /** @brief Returns a mutable reference to the per-game bug-compatibility configuration. */
    Bugs& RulesBugs() { return bugs; }

    /** @brief A new army, owned by the game but not yet in it. */
    Army* Make(const Army& army);
    /**
     * @brief A new army, in the game, in the army table's lowest free slot.
     *
     * `armies` stays in slot order, so a scan from its end is the original's
     * from the top down.
     */
    Army* Add(const Army& army);
    /** @brief Is the army in the game? */
    bool Alive(const Army* army) const;
    /** @brief Takes the army out of the game (it stays in the pool). */
    void Remove(const Army* army);
    /** @brief True if an encampment (tower) stands on tile `(x, y)`. */
    bool HasTower(int x, int y) const {
        return map && map->InBounds(x, y) && towers.count(y * map->width + x) != 0;
    }
    /** @brief Returns all armies standing on tile `(x, y)` in top-down slot order. */
    std::vector<Army*> ArmiesOnTile(int x, int y) const;
    /** @brief Returns the number of sides still alive in the game. */
    int LivingSideCount() const;
};

}  // namespace w2
