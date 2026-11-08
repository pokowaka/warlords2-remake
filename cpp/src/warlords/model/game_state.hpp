/**
 * @file
 * @brief Game aggregate root state: map, sides, armies, turn order, diplomacy, and history.
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "warlords/model/army.hpp"
#include "warlords/model/army_type.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/history_types.hpp"
#include "warlords/model/map.hpp"
#include "warlords/rng.hpp"

namespace w2 {

struct Grids;      // move.cpp's cached cost grids
struct PathCache;  // move.cpp's route cache
struct Neighbours;

/** @brief A game in play: the map, the armies, the sides and whose turn it is. */
struct Game {
    Game();
    ~Game();
    Rng rng;                                       ///< The game's dice.
    std::string data_dir;                          ///< Where the original's data files are.
    Types types;                                   ///< The army types.
    std::unique_ptr<Map> map;                      ///< The map and what is on it.
    std::vector<Army*> armies;                     ///< The armies in the game, in slot order.
    std::vector<std::unique_ptr<Army>> pool;       ///< Every army ever made.
    int turn = 1;                                  ///< The round being played.
    std::vector<std::string> log;                  ///< The game's log.
    bool greatest = false;                         ///< A game of the Greatest Warlord campaign.
    std::vector<Side*> sides;                      ///< The sides in play.
    int current = 0;                               ///< The index of the side to play.
    Side* side = nullptr;                          ///< The side to play.
    Diplomacy diplomacy;                           ///< The relations between the sides.
    std::map<int, std::vector<uint8_t>> explored;  ///< By side: the tiles it has seen.
    std::set<int> towers;                          ///< Tiles with an encampment.
    bool won = false;                              ///< The game is won.
    bool over = false;                             ///< The game is over.
    bool no_humans_said = false;                   ///< "No humans left" has been told.
    bool surrender_offered = false;                ///< The surrender offer was made.
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
    /// The turn order (see docs/formats/scenario.md), shuffled each round
    /// with Random Turns.
    std::array<int, 8> turn_order{0, 1, 2, 3, 4, 5, 6, 7};
    int order_at = -1;  ///< The place in turn_order of the side to play.

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
};

}  // namespace w2
