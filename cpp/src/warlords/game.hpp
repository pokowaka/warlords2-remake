/**
 * @file
 * @brief Scenario setup (`NewGame`), map/fog queries, and forwarding wrappers over `TurnEngine`,
 * `CityEconomy`, `CombatResolver`, and `SiteExplorer`.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"

#include "warlords/city_economy.hpp"
#include "warlords/combat.hpp"
#include "warlords/scenario_source.hpp"
#include "warlords/turn_engine.hpp"
#include "warlords/types.hpp"

namespace w2 {
struct SearchResult;
struct QuestResult;
}  // namespace w2

namespace w2::game {

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
    /** @brief Key-value list of scenario rule options. */
    using OptionPairList = std::vector<std::pair<std::string, int>>;
    /** @brief Per-side setup overrides keyed by side index `0..7`. */
    using SideSetupMap = std::map<int, SideSetup>;

    double seed = 0;                              ///< Seed for the game's dice.
    OptionPairList options;                       ///< Option name, value.
    SideSetupMap sides;                           ///< Setup by side index.
    bool greatest = false;                        ///< Greatest Warlord campaign flag.
    const ScenarioFiles* memory_files = nullptr;  ///< In-memory scenario files, or null for disk.
    int active_sides = 0;  ///< 0 keeps all scenario sides; 2..8 keeps the K nearest capitals.
    int crop_margin = 0;   ///< Above 0, crops the map to the active capitals +/- this many tiles.
    GameComputerSetupFn computer_setup = nullptr;  ///< Optional override for AI strategy setup.
};

/** @brief Builds a fresh game from the original data files. */
absl::StatusOr<std::unique_ptr<Game>> NewGame(const std::filesystem::path& data_dir,
                                              const std::string& scenario,
                                              const NewGameOptions& opts = NewGameOptions());

constexpr int kSetupPlayer = 0;
constexpr int kFallenLines = 5;

std::vector<Army*> ArmiesAt(const Game& game, int x, int y);
absl::Span<Army* const> ArmiesOnTileSpan(const Game& game, int x, int y);
City* CityAt(const Game& game, int x, int y);
void SetCityOwner(Game& game, City& city, int side_index);
std::vector<City*> SideCities(const Game& game, const Side& side);
std::vector<Army*> SideArmies(const Game& game, const Side& side);
int ItemBonus(const Game& game, const Side& side, int item_type);
std::pair<int, int> FreeTileIn(Game& game, const City& city, bool spill,
                               std::pair<int, int>* last = nullptr);
int Income(const Game& game, const Side& side);
int Upkeep(const Game& game, const Side& side);

bool TowerAt(const Game& game, int x, int y);
void TidyTowers(Game& game);
bool HeroWithDoubleMoveAt(const Game& game, const Side& side, int x, int y);

void FollowCapital(Game& game, Side& side);
bool StartTurn(Game& game, Side& side);
void HumanProduction(Game& game, Side& side);
bool Producing(const City& city);
Side* EndTurn(Game& game);
Side* Begin(Game& game);
Side* NextHuman(Game& game);

Sign* SignAt(Game& game, int x, int y);
void Disband(Game& game, Side& side, const std::vector<Army*>& armies);
int Loot(const Game& game, const Side* loser);
Battle DecideAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
void ChargeAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
int SinkStranded(Game& game, Side& side, int x = kNone, int y = kNone);
Battle ResolveAttack(Game& game, const std::vector<Army*>& stack, int x, int y);
Battle& ApplyAttack(Game& game, Battle& result);

void AddDiploScore(Game& game, Side& side, int points);
std::shared_ptr<QuestResult> Occupy(Game& game, Side& side, City& city,
                                    const std::vector<Army*>& stack);
Spoils Pillage(Game& game, Side& side, City& city, const std::vector<Army*>& stack);
Spoils Sack(Game& game, Side& side, City& city, const std::vector<Army*>& stack);
void Raze(Game& game, Side& side, City& city, const std::vector<Army*>& stack,
          bool own_city = false);
void Resign(Game& game, Side& side);

/**
 * @brief Pre-resolved exploration mask pointers for `side` and its active allies, allowing tight
 * full-map loops (`Prepare`, `belief::Update`, `ComputeEmpireScore`) to test tile visibility with
 * direct byte loads instead of repeated diplomacy and container lookups.
 */
struct SeenMasks {
    std::array<const uint8_t*, 8> masks{};  ///< Non-null mask pointers for `side` and allies.
    int count = 0;                          ///< Number of active pointers in `masks`.
    int width = 0;                          ///< Map width in tiles.
    int height = 0;                         ///< Map height in tiles.
    size_t total_tiles = 0;                 ///< `width * height`.
    bool all_visible = false;               ///< True when Hidden Map is off or `side == kNone`.
};

SeenMasks BuildSeenMasks(const Game& game, int side);

inline bool IsSeenIndex(const SeenMasks& seen, size_t tile_index) {
    if (seen.all_visible) return true;
    for (int i = 0; i < seen.count; ++i) {
        if (seen.masks[static_cast<size_t>(i)][tile_index] != 0) return true;
    }
    return false;
}

inline bool IsSeenAt(const SeenMasks& seen, int x, int y) {
    if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(seen.width) ||
        static_cast<unsigned int>(y) >= static_cast<unsigned int>(seen.height)) {
        return false;
    }
    if (seen.all_visible) return true;
    return IsSeenIndex(seen,
                       static_cast<size_t>(y) * static_cast<size_t>(seen.width) +
                               static_cast<size_t>(x));
}

bool Seen(const Game& game, int side, int x, int y);
bool HasActiveHuman(const Game& game);
int ViewerSide(const Game& game, const Side* player);
int Reveal(Game& game, int side, int x, int y, bool flying);
extern const int kFogDx[8];
extern const int kFogDy[8];
extern const int kFogCell[256];
constexpr int kFogNone = 255;
constexpr int kFogBlack = 14;
int FogCell(const Game& game, int side, int x, int y);
void TidyExplored(Game& game, int side);
void RevealStart(Game& game, const Side& side);

void AcceptSurrender(Game& game, Side& side);
bool SurrenderPending(const Game& game);
std::vector<Fallen> EliminateFallen(Game& game);
Ending EndRound(Game& game, Side* last = nullptr);
bool SetCityProduction(Game& game, const Side* who, City& city, int slot);
void NeutralProduction(Game& game, const Side* who);
Ending CheckEnd(Game& game);

std::shared_ptr<SearchResult> SearchHere(Game& game, const std::vector<Army*>& stack, bool human);
std::string DescribeSearch(const SearchResult* result);

void SetProduction(Game& game, City& city, int slot_index);
std::vector<City*> VectoredTo(Game& game, const City* dest, const Side* side = nullptr);
std::pair<int, int> StandardAt(const Game& game, const Side& side);
bool PlantFlag(Game& game, Side& side, Army* hero);
bool VectorToStandard(Game& game, City& city, const Side& side);
bool Vector(Game& game, City& city, City* dest_city);
City* NearestCity(Game& game, int x, int y, const Side* side = nullptr, const Side* seer = nullptr);
std::vector<const ArmyType*> BuyableTypes(const Game& game);
std::string CannotBuy(const Game& game, const Side& side, const City& city,
                      const ArmyType& army_type);
int BuySlot(const City& city);
void BuyProduction(Game& game, Side& side, City& city, int slot_index, int type_id);
void SortProduction(City& city);
void RenameCity(Game& game, City& city, const std::string& name);

}  // namespace w2::game
