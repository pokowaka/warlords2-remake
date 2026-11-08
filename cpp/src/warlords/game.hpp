/**
 * @file
 * @brief Game state and the turn loop.
 *
 * Headless on purpose: this never touches SDL, so the rules can be run and
 * checked by the tests. Rendering and input sit on top.
 */
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

#include "warlords/combat.hpp"
#include "warlords/scenario_source.hpp"
#include "warlords/types.hpp"

namespace w2 {
struct SearchResult;
struct QuestResult;
}  // namespace w2

namespace w2::game {

constexpr int kStandard = -2;      ///< Vector destination meaning "the side's standard".
constexpr int kMaxVectoredTo = 4;  ///< No more than four cities may send to one place.

/** @brief How one side is set up for a new game. */
struct SideSetup {
    bool off = false;       ///< Left out of the game.
    bool computer = false;  ///< Played by the computer.
    int level = kNone;      ///< Computer skill level; kNone keeps the scenario's.
    int card = 0;           ///< Computer character card (CARDS/*.CRD); 0 is the level's own.
    std::string name;       ///< Retyped on the setup screen; empty keeps the scenario's.
};

/** @brief Everything NewGame() needs besides the scenario itself. */
struct NewGameOptions {
    double seed = 0;                                   ///< Seed for the game's dice.
    std::vector<std::pair<std::string, int>> options;  ///< Option name, value.
    std::map<int, SideSetup> sides;                    ///< Setup by side index.
    bool greatest = false;  ///< The setup screen's Greatest mode: every side a level 2 computer.
    const ScenarioFiles* memory_files = nullptr;  ///< Scenario files held in memory, by name;
                                                  ///< null reads them from disk.
    int active_sides = 0;  ///< 0 keeps all scenario sides; 2..8 keeps the K nearest capitals.
    int crop_margin = 0;   ///< Above 0, crops the map to the active capitals +/- this many tiles.
};

/** @brief Builds a fresh game from the original data files. */
absl::StatusOr<std::unique_ptr<Game>> NewGame(const std::filesystem::path& data_dir,
                                              const std::string& scenario,
                                              const NewGameOptions& opts = NewGameOptions());

/**
 * @brief The side whose turn it is while the game is set up.
 *
 * The scenario's own current player (see docs/formats/scenario.md), 0 in
 * every shipped scenario. Its Enhanced flag is the one best_production_for
 * and city_produce_army read.
 */
constexpr int kSetupPlayer = 0;

/**
 * @brief Every army standing on a tile (armies in transit are nowhere).
 *
 * The army table is walked from the top down, as the original's
 * armies_at_tile walks it.
 */
std::vector<Army*> ArmiesAt(const Game& game, int x, int y);
/** @brief The city standing on a tile, anywhere in its 2x2 footprint. */
City* CityAt(const Game& game, int x, int y);
/** @brief Gives a city to a side (kNone for neutral) and drops the cached cost grids. */
void SetCityOwner(Game& game, City& city, int side_index);
/** @brief The cities a side holds, in city order. */
std::vector<City*> SideCities(const Game& game, const Side& side);
/** @brief The armies a side owns, in army table order, those in transit too. */
std::vector<Army*> SideArmies(const Game& game, const Side& side);
/** @brief Items carried by a side's heroes, summed by effect type. */
int ItemBonus(const Game& game, const Side& side, int item_type);
/**
 * @brief A tile in the city with room for another army.
 *
 * Tries the four tiles of its footprint in the order (x,y), (x+1,y),
 * (x,y+1), (x+1,y+1); then -- if `spill` -- up to 20 random tries within one
 * step for a tile no army stands on.
 * @return The tile, or {kNone, kNone} when there is none; then the last tile
 *         tried is put in `last`, where hero_recruit puts its hero regardless.
 */
std::pair<int, int> FreeTileIn(Game& game, const City& city, bool spill,
                               std::pair<int, int>* last = nullptr);
/** @brief A side's gross income; see docs/rules.md > Start of a side's turn, step 4. */
int Income(const Game& game, const Side& side);
/** @brief What a side's armies cost each turn; armies at sea cost a minimum. */
int Upkeep(const Game& game, const Side& side);

/** @brief Is (x, y) an encampment? */
bool TowerAt(const Game& game, int x, int y);
/** @brief Takes the tower flag off every tile nobody stands on any more. */
void TidyTowers(Game& game);
/** @brief Is one of the side's heroes carrying a double-movement item on this tile? */
bool HeroWithDoubleMoveAt(const Game& game, const Side& side, int x, int y);

/**
 * @brief Puts the side's capital where it now is, before every side's turn.
 *
 * While more than one side plays and the side holds cities but not its own
 * capital, the capital is the side's own city nearest the capital it had;
 * else its own capital again. The turn opens centred there.
 */
void FollowCapital(Game& game, Side& side);
/**
 * @brief Runs the start of `side`'s turn.
 *
 * The prelude every side gets: its armies in a city come ashore, its capital
 * follows it. Then a computer's turn: diplomacy; a side left with no city
 * does nothing more until the round's end puts it out; the hero, hired at
 * once; promotions, gold, production, the movement reset, the encampments,
 * the quest. A human's goes on regardless: the movement reset, the
 * encampments and the gold come first, then the quest; promotions and the
 * hero follow, and the cities build only once the hero offer is settled
 * (HumanProduction()).
 * @return False if the side has no turn to play.
 */
bool StartTurn(Game& game, Side& side);
/**
 * @brief A human's cities build, once the turn's hero offer is taken or turned down.
 *
 * On turn 1 they do not: the capital's production opens instead. Each city
 * that built starts its next army.
 */
void HumanProduction(Game& game, Side& side);
/**
 * @brief Is the city building?
 *
 * Its countdown is above 0; the original's city screen shows "none" otherwise.
 */
bool Producing(const City& city);
/**
 * @brief Hands the turn to the next side in the turn order.
 *
 * Ends the round when the order runs out.
 * @return The side now to play, or null if the game is over.
 */
Side* EndTurn(Game& game);
/** @brief Starts the very first turn of a new game. */
Side* Begin(Game& game);
/**
 * @brief The human who plays next in the round, the side to play included.
 *
 * While the computers before it play, the screen is this side's: the
 * original keeps one seen bit per tile, set for its human only, so a
 * computer's turn shows the human's hidden map.
 * @return The side, or null with no human left.
 */
Side* NextHuman(Game& game);

/** @brief The signpost on a tile, if there is one. */
Sign* SignAt(Game& game, int x, int y);
/** @brief Order > Disband: heroes drop what they carry, then the armies are gone. */
void Disband(Game& game, Side& side, const std::vector<Army*>& armies);
/** @brief Gold taken with a city won from another side. */
int Loot(const Game& game, const Side* loser);
/**
 * @brief Fights for a tile, deciding it and nothing more.
 *
 * ApplyAttack() makes it take effect. A city attacked is marked with the
 * attacker's bit: a neutral one that holds out builds from then on
 * (NeutralProduction()).
 */
Battle DecideAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
/**
 * @brief Charges a stack what an attack costs.
 *
 * A stack that is not flying and not at sea, attacking onto water or shore,
 * puts to sea first -- every army that cannot fly, and has no flight item,
 * at sea with no moves, boats too. Then every army loses the tile's movement
 * cost, 2 at least, down to 0, and forgets its goal.
 */
void ChargeAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
/**
 * @brief Armies left on water without a boat drown.
 *
 * On a water tile, or a crossing (see docs/formats/map.md), every army of
 * the side's that is not at sea, not a hero and cannot fly is lost -- unless
 * the armies there could fly off together. With x, y only that tile is
 * looked at, with kNone every tile the side stands on. The original asks it
 * for a human player only: as the turn opens and as a stack is picked up.
 * @return How many sank.
 */
int SinkStranded(Game& game, Side& side, int x = kNone, int y = kNone);
/** @brief Fights for a tile and applies the outcome at once. */
Battle ResolveAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
/**
 * @brief Lets a decided fight take effect.
 *
 * Once only; sets `captured` to the city taken, and `medal` to the army that
 * won one.
 */
Battle& ApplyAttack(Game& game, Battle& result);

/** @brief Raises a side's diplomatic score. */
void AddDiploScore(Game& game, Side& side, int points);
/** @brief Keeps a captured city as it is, and asks the quest about it. */
std::shared_ptr<QuestResult> Occupy(Game& game, Side& side, City& city,
                                    const std::vector<Army*>& stack);
/** @brief What pillaging or sacking took: the gold and the types lost. */
struct Spoils {
    int gold = 0;                           ///< Gold taken in all.
    std::vector<std::pair<int, int>> lost;  ///< Each type lost: type, gold.
};
/** @brief Strips the most expensive production type for gold. */
Spoils Pillage(Game& game, Side& side, City& city, const std::vector<Army*>& stack);
/** @brief Strips every production type but the cheapest for gold. */
Spoils Sack(Game& game, Side& side, City& city, const std::vector<Army*>& stack);
/** @brief Burns the city to the ground. */
void Raze(Game& game, Side& side, City& city, const std::vector<Army*>& stack,
          bool own_city = false);
/** @brief Order > Resign: the side's cities become ruins and its armies are gone. */
void Resign(Game& game, Side& side);

/** @brief Has this side seen the tile? Always true with Hidden Map off or for a spectator
 *  (side == kNone). */
bool Seen(const Game& game, int side, int x, int y);
/** @brief Returns true if at least one living human side still holds a city in `game`. */
bool HasActiveHuman(const Game& game);
/** @brief Returns `player->index` while a human is in the game, or `kNone` (full-map
 *  spectator) in AI-vs-AI. */
int ViewerSide(const Game& game, const Side* player);
/**
 * @brief Uncovers the tiles around (x, y): 2 out for a flier or on a city, else 1.
 * @return How many were new.
 */
int Reveal(Game& game, int side, int x, int y, bool flying);
extern const int kFogDx[8];      ///< Column step to each of the eight neighbours, from north.
extern const int kFogDy[8];      ///< Row step to each of the eight neighbours, from north.
extern const int kFogCell[256];  ///< HIDDEN.PCK cell by unseen-neighbour bits, or kFogNone.
constexpr int kFogNone = 255;    ///< kFogCell entry for "no fog cell".
constexpr int kFogBlack = 14;    ///< The all-black HIDDEN.PCK cell.
/**
 * @brief The HIDDEN.PCK cell over (x, y) for this side.
 * @return The cell, or kNone for a tile it has seen (or one whose edge has no cell).
 */
int FogCell(const Game& game, int side, int x, int y);
/** @brief The start of a turn tidies the hidden map: unseen tiles with no fog cell are
 *  uncovered. */
void TidyExplored(Game& game, int side);
/** @brief Uncovers everything a side can already see: its cities and its armies. */
void RevealStart(Game& game, const Side& side);

/** @brief The surrender offer taken: the computers are out and the side has won. */
void AcceptSurrender(Game& game, Side& side);
/**
 * @brief Is a surrender offered and not yet put to the player?
 *
 * Asked as the human's turn opens, not kept on the round's Ending, which the
 * computer turns in between would clear.
 */
bool SurrenderPending(const Game& game);
/** @brief STRING.DAT group 11 holds five ways of saying a side is gone. */
constexpr int kFallenLines = 5;
/**
 * @brief Puts every side in the game left without a city out of it, in side order.
 *
 * Told in a box while a human plays or when the side was a human's,
 * otherwise in the status bar.
 */
std::vector<Fallen> EliminateFallen(Game& game);
/**
 * @brief The round's end.
 *
 * The sides left without a city are put out of the game, then the end is
 * looked for, then the neutral cities build. `last` is the side that played
 * last: its gold and Enhanced flag are the ones the neutral cities'
 * production reads. Leaves the finding in game.ending.
 */
Ending EndRound(Game& game, Side* last = nullptr);
/**
 * @brief Sets a city's production for side `who` (the original's set_city_production).
 *
 * Refused after turn 5 when that side has less than the type's cost + 30 in
 * gold.
 * @return True if set.
 */
bool SetCityProduction(Game& game, const Side* who, City& city, int slot);
/**
 * @brief The neutral cities' production, with Neutral Cities at 2 or more.
 *
 * A neutral city that has beaten off an attack (City::attacked) counts its
 * build down and, while fewer than 4 armies stand on its top-left tile,
 * starts the best balanced type (purpose 2). The last city first. See
 * docs/rules.md > Neutral production during play.
 */
void NeutralProduction(Game& game, const Side* who);
/**
 * @brief Is the game over, or nearly?
 *
 * Run at the round's end, once the fallen are out.
 */
Ending CheckEnd(Game& game);

/** @brief Searches whatever the stack is standing on, if anything. */
std::shared_ptr<SearchResult> SearchHere(Game& game, const std::vector<Army*>& stack, bool human);
/** @brief A line of prose for a search result. */
std::string DescribeSearch(const SearchResult* result);

/** @brief Chooses what a city builds: a slot index from 0, or kNone to stop. */
void SetProduction(Game& game, City& city, int slot_index);
/**
 * @brief The cities sending what they build to `dest`.
 *
 * `dest` is a city, or (when null) the cities of `side` sending theirs to
 * its standard.
 */
std::vector<City*> VectoredTo(Game& game, const City* dest, const Side* side = nullptr);
/** @brief Where the side's standard is planted, or {kNone, kNone}. */
std::pair<int, int> StandardAt(const Game& game, const Side& side);
/**
 * @brief Hero > Plant Flag.
 * @return True if it was planted.
 */
bool PlantFlag(Game& game, Side& side, Army* hero);
/** @brief Sends what a city builds to its side's planted standard. */
bool VectorToStandard(Game& game, City& city, const Side& side);
/** @brief Sends what a city builds to another city; null (or itself) stops it. */
bool Vector(Game& game, City& city, City* dest_city);
/** @brief The city nearest a tile by map distance, the first of equals. */
City* NearestCity(Game& game, int x, int y, const Side* side = nullptr, const Side* seer = nullptr);
/** @brief The army types a side may buy for a city, in ARMYTYPE.DAT's own order. */
std::vector<const ArmyType*> BuyableTypes(const Game& game);
/** @brief Why a type cannot be bought for this city right now, or "". */
std::string CannotBuy(const Game& game, const Side& side, const City& city,
                      const ArmyType& army_type);
/** @brief The slot the Build Production screen starts on, from 0. */
int BuySlot(const City& city);
/** @brief Buys an army type into a city's production slot `slot_index`. */
void BuyProduction(Game& game, Side& side, City& city, int slot_index, int type_id);
/** @brief Puts a city's production back in price order. */
void SortProduction(City& city);
/** @brief Gives a city a new name (City > Rename). */
void RenameCity(Game& game, City& city, const std::string& name);

}  // namespace w2::game
