/**
 * @file
 * @brief Encapsulated `w2::Game` aggregate root and `w2::Bugs` compatibility value struct.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/types/span.h"

#include "warlords/model/army.hpp"
#include "warlords/model/army_registry.hpp"
#include "warlords/model/army_type.hpp"
#include "warlords/model/computer_player.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/fog_of_war.hpp"
#include "warlords/model/history_types.hpp"
#include "warlords/model/map.hpp"
#include "warlords/model/turn_report.hpp"
#include "warlords/model/turn_schedule.hpp"
#include "warlords/rng.hpp"

namespace w2 {

class Pathfinder;

/** @brief Deliberate faults in the 1993 original reproduced per `Game` instance by default. */
struct Bugs {
    /// Post-battle hero experience checks the *attacker's* type array when
    /// deciding whether a surviving defender was a hero (`hero::BattleExperience`).
    bool hero_experience_reads_attacker_types = true;
};

/**
 * @brief Aggregate root for an active Warlords II session, owning the scenario `Map`, army pool
 * and spatial registry, participating factions, turn schedule, diplomacy matrix, fog of war,
 * encampment towers, chronicle history, and movement `Pathfinder`.
 *
 * Models the active faction turn cursor through a single authoritative `current_side_index_` into
 * `sides_` (`0..sides_.size()-1`, or `kNone` when no side holds the turn), deriving `CurrentSide()`
 * on demand so index and pointer views never diverge.
 *
 * Invariants:
 * - `current_side_index_` is either `kNone` or a valid index in `[0, sides_.size())`;
 *   `CurrentSide()` is non-null iff `current_side_index_ != kNone`.
 * - Every `Side*` in `sides_` is non-null and points into `map_->Sides()` while `map_` is set.
 * - `SetCityOwner(city, owner)` updates `city` and invalidates `pathfinder_` atomically.
 * - `RecordDeed` retains at most the two highest-priority (`DeedType` lowest-ordinal) deeds per
 *   faction in `deeds_` during a round, with names truncated to 15 characters.
 *
 * Thread safety: Not thread-safe without external synchronization.
 *
 * Ownership & lifetimes: Exclusively owns `map_`, `armies_`, and `pathfinder_`. Non-copyable;
 * movable.
 */
class Game {
  public:
    /** @brief Owned `Map` handle. */
    using MapHandle = std::unique_ptr<Map>;
    /** @brief Owned `Pathfinder` handle. */
    using PathfinderHandle = std::unique_ptr<Pathfinder>;
    /** @brief Participating scenario factions in setup order. */
    using SideList = std::vector<Side*>;
    /** @brief Read-only span over participating `Side*` pointers. */
    using SideSpan = absl::Span<Side* const>;
    /** @brief Ordered list of army pointers on a tile or in a stack. */
    using ArmyStack = std::vector<Army*>;
    /** @brief Chronological game log entries. */
    using LogList = std::vector<std::string>;
    /** @brief Flat tile indices (`y * map_width + x`) holding fortified encampment towers. */
    using TowerSet = std::set<int>;
    /** @brief Per-round historical snapshots (`1..kLastRecordedTurn`). */
    using HistoryList = std::vector<HistoryRecord>;
    /** @brief Up to two notable deeds recorded for one side in the current round. */
    using DeedList = std::vector<Deed>;
    /** @brief Current round's notable deeds keyed by side index (`0..7`). */
    using DeedMap = std::map<int, DeedList>;
    /** @brief Five loss-category counters (`Loss::kArmies..Loss::kStandards`). */
    using TriumphCounts = std::array<int, 5>;
    /** @brief Per-opponent triumph/loss row for one faction. */
    using TriumphRow = std::map<int, TriumphCounts>;
    /** @brief Two-dimensional triumph/loss tally matrix (`side -> opponent -> counts`). */
    using TriumphMatrix = std::map<int, TriumphRow>;
    /** @brief Identifiers of tutorial moments already shown in this game. */
    using TutorialSet = std::set<std::string>;
    /** @brief Hero candidate name paired with its heroine (`true` = female) flag. */
    using HeroNameEntry = std::pair<std::string, bool>;
    /** @brief Ordered hero candidate names loaded from `HERONAM*.DAT`. */
    using HeroNameList = std::vector<HeroNameEntry>;
    /** @brief Cached `HERONAM*.DAT` name lists keyed by side index (`0..7`). */
    using HeroNamePool = std::map<int, HeroNameList>;
    /** @brief Transient turn-opening news reports indexed by faction (`0..7`). */
    using TurnReportTable = std::array<TurnReport, 8>;

    /** @brief Constructs an empty game aggregate with an initialized `Pathfinder`. */
    Game();
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    Game(Game&&) noexcept;
    Game& operator=(Game&&) noexcept;

    /** @brief Returns mutable access to the game's deterministic PRNG. */
    Rng& GetRng() { return rng_; }
    /** @brief Returns read-only access to the game's deterministic PRNG. */
    const Rng& GetRng() const { return rng_; }
    /** @brief Replaces the game's deterministic PRNG state. */
    void SetRng(const Rng& rng) { rng_ = rng; }

    /** @brief Returns read-only per-game 1993 bug-compatibility flags. */
    const Bugs& RulesBugs() const { return bugs_; }
    /** @brief Returns mutable per-game 1993 bug-compatibility flags. */
    Bugs& RulesBugs() { return bugs_; }

    /** @brief Returns the root asset data directory path. */
    const std::filesystem::path& DataDir() const { return data_dir_; }
    /** @brief Sets the root asset data directory path. */
    void SetDataDir(std::filesystem::path data_dir);

    /** @brief Returns read-only access to the `ARMYTYPE.DAT` unit catalog. */
    const Types& ArmyTypes() const { return types_; }
    /** @brief Returns mutable access to the `ARMYTYPE.DAT` unit catalog. */
    Types& MutableArmyTypes() { return types_; }

    /** @brief True when a scenario `Map` is installed on this game. */
    bool HasMap() const { return map_ != nullptr; }
    /** @brief Returns a non-owning pointer to the scenario `Map`, or `nullptr`. */
    Map* MapPtr() { return map_.get(); }
    /** @brief Returns a read-only pointer to the scenario `Map`, or `nullptr`. */
    const Map* MapPtr() const { return map_.get(); }
    /** @brief Returns mutable access to the installed scenario `Map` (`HasMap()` must hold). */
    Map& GetMap() {
        ABSL_DCHECK(map_ != nullptr);
        return *map_;
    }
    /** @brief Returns read-only access to the installed scenario `Map` (`HasMap()` must hold). */
    const Map& GetMap() const {
        ABSL_DCHECK(map_ != nullptr);
        return *map_;
    }
    /** @brief Installs `map` on this game and resets `sides_` and `current_side_index_`. */
    void SetMap(MapHandle map);

    /** @brief Returns mutable access to the army pool, active slot table, and spatial index. */
    ArmyRegistry& Armies() { return armies_; }
    /** @brief Returns read-only access to the army pool, active slot table, and spatial index. */
    const ArmyRegistry& Armies() const { return armies_; }

    /** @brief Allocates `army` in the pool without placing it in the active slot table. */
    Army* Make(const Army& army);
    /** @brief Allocates `army` and inserts it into the lowest free active army slot. */
    Army* Add(const Army& army);
    /** @brief True when `army` is currently active on the board or in road transit. */
    bool Alive(const Army* army) const;
    /** @brief Removes `army` from the active table while keeping its pool record alive. */
    void Remove(const Army* army);
    /** @brief Returns all active on-map armies standing on `(x, y)` in descending slot order. */
    ArmyStack ArmiesOnTile(int x, int y) const;

    /** @brief Returns the 1-based round number currently in play. */
    int Turn() const { return turn_; }
    /** @brief Sets the 1-based round number. */
    void SetTurn(int turn) { turn_ = turn; }
    /** @brief Advances the round counter by one. */
    void AdvanceTurn() { ++turn_; }

    /** @brief Returns all Participating scenario factions in setup order. */
    SideSpan Sides() const { return sides_; }
    /** @brief Replaces the participating faction list and re-validates `current_side_index_`. */
    void SetActiveSides(SideList sides);
    /** @brief Appends `side` (when non-null) to the participating faction list. */
    void AddActiveSide(Side* side);
    /** @brief Returns the number of participating factions still alive. */
    int LivingSideCount() const;

    /**
     * @brief Returns the index into `Sides()` of the faction currently taking its turn, or `kNone`
     * when no faction is active.
     */
    int CurrentSideIndex() const { return current_side_index_; }
    /** @brief Returns the faction currently taking its turn, or `nullptr` when none is active. */
    Side* CurrentSide();
    /** @brief Returns the faction currently taking its turn (const), or `nullptr`. */
    const Side* CurrentSide() const;
    /** @brief Sets `current_side_index_` to `index` if in `[0, Sides().size())`, else `kNone`. */
    void SetCurrentSideIndex(int index);
    /**
     * @brief Sets the active faction to `side` (locating its index in `Sides()`, or `kNone` when
     * `side == nullptr`).
     */
    void SetCurrentSide(Side* side);
    /** @brief Clears the active faction (`CurrentSideIndex() == kNone`, `CurrentSide() ==
     * nullptr`).
     */
    void ClearCurrentSide() { current_side_index_ = kNone; }

    /** @brief Assigns `city` to `owner` and invalidates cached `Pathfinder` cost grids. */
    void SetCityOwner(City& city, SideId owner);

    /** @brief Returns mutable access to the inter-faction `DiplomacyMatrix`. */
    DiplomacyMatrix& Diplomacy() { return diplomacy_; }
    /** @brief Returns read-only access to the inter-faction `DiplomacyMatrix`. */
    const DiplomacyMatrix& Diplomacy() const { return diplomacy_; }

    /** @brief Returns mutable access to per-side `FogOfWar` exploration masks. */
    FogOfWar& Explored() { return explored_; }
    /** @brief Returns read-only access to per-side `FogOfWar` exploration masks. */
    const FogOfWar& Explored() const { return explored_; }

    /** @brief Returns mutable access to the round `TurnSchedule`. */
    TurnSchedule& Schedule() { return turn_schedule_; }
    /** @brief Returns read-only access to the round `TurnSchedule`. */
    const TurnSchedule& Schedule() const { return turn_schedule_; }

    /** @brief Returns the owned `Pathfinder` movement cost-grid and route-cache service. */
    Pathfinder& GetPathfinder();
    /** @brief Returns the owned `Pathfinder` movement service (const). */
    const Pathfinder& GetPathfinder() const;

    /** @brief Returns mutable transient turn-opening news for `side_id` (`0..7`). */
    TurnReport& TurnNews(SideId side_id);
    /** @brief Returns read-only transient turn-opening news for `side_id` (`0..7`). */
    const TurnReport& TurnNews(SideId side_id) const;

    /** @brief True when an encampment tower stands on `(x, y)`. */
    bool HasTower(int x, int y) const;
    /** @brief True when an encampment tower stands on `pos`. */
    bool HasTower(TilePos pos) const { return HasTower(pos.x, pos.y); }
    /** @brief Places an encampment tower on `pos` when inside `map_` bounds. */
    void PlaceTower(TilePos pos);
    /** @brief Inserts a serialized flat tower key (`y * width + x`) when inside `map_` bounds. */
    void PlaceTowerKey(int tile_key);
    /** @brief Removes any encampment tower on `pos`. */
    void RemoveTower(TilePos pos);
    /** @brief Removes all encampment towers from the board. */
    void ClearTowers() { towers_.clear(); }
    /** @brief Removes any encampment tower whose tile no longer holds an on-map army. */
    void ClearAbandonedTowers();
    /** @brief Returns read-only flat tile keys (`y * width + x`) of all standing towers. */
    const TowerSet& TowerKeys() const { return towers_; }

    /** @brief True when playing a Greatest Warlord campaign scenario. */
    bool IsGreatestCampaign() const { return greatest_; }
    /** @brief Sets the Greatest Warlord campaign flag. */
    void SetGreatestCampaign(bool greatest) { greatest_ = greatest; }

    /** @brief True once a faction has achieved victory. */
    bool IsWon() const { return won_; }
    /** @brief Sets whether a faction has achieved victory. */
    void SetWon(bool won) { won_ = won; }

    /** @brief True once all factions are eliminated and the game has ended. */
    bool IsOver() const { return over_; }
    /** @brief Sets whether the game has ended with no remaining players. */
    void SetOver(bool over) { over_ = over; }

    /** @brief True once the "no humans left" announcement has been triggered. */
    bool NoHumansSaid() const { return no_humans_said_; }
    /** @brief Sets whether the "no humans left" announcement has been triggered. */
    void SetNoHumansSaid(bool said) { no_humans_said_ = said; }

    /** @brief True once computer opponents have offered their surrender. */
    bool SurrenderOffered() const { return surrender_offered_; }
    /** @brief Sets whether computer opponents have offered their surrender. */
    void SetSurrenderOffered(bool offered) { surrender_offered_ = offered; }

    /** @brief True once the pending surrender offer has been presented to the human player. */
    bool SurrenderShown() const { return surrender_shown_; }
    /** @brief Sets whether the surrender offer has been presented to the human player. */
    void SetSurrenderShown(bool shown) { surrender_shown_ = shown; }

    /** @brief Returns read-only end-of-round victory/defeat/fallen notification state. */
    const Ending& EndingState() const { return ending_; }
    /** @brief Returns mutable end-of-round victory/defeat/fallen notification state. */
    Ending& MutableEndingState() { return ending_; }
    /** @brief Replaces end-of-round victory/defeat/fallen notification state. */
    void SetEndingState(Ending ending);

    /** @brief Returns chronological game log messages. */
    const LogList& Log() const { return log_; }
    /** @brief Appends `entry` to the game log. */
    void AppendLog(std::string entry);
    /** @brief Clears all game log messages. */
    void ClearLog() { log_.clear(); }

    /** @brief Returns all recorded round history snapshots. */
    const HistoryList& History() const { return history_; }
    /** @brief Appends `record` to the round history list. */
    void AppendHistoryRecord(HistoryRecord record);
    /** @brief Clears all recorded round history snapshots. */
    void ClearHistory() { history_.clear(); }

    /** @brief Returns the map of current-round notable deeds per side. */
    const DeedMap& Deeds() const { return deeds_; }
    /** @brief Returns current-round notable deeds for `side_index` (empty when none). */
    const DeedList& DeedsForSide(int side_index) const;
    /**
     * @brief Records a notable deed for `side_id`, keeping at most the two highest-priority deeds
     * (`DeedType` lowest ordinal) and truncating `actor_name` to 15 characters.
     */
    void RecordDeed(SideId side_id, DeedType type, int subject, int object,
                    std::string_view actor_name);
    /** @brief Restores serialized current-round deeds for `side_index`. */
    void SetSideDeeds(int side_index, DeedList deeds);
    /** @brief Clears all current-round deeds across every side. */
    void ClearDeeds() { deeds_.clear(); }

    /** @brief Returns read-only access to the full triumph/loss matrix. */
    const TriumphMatrix& Triumphs() const { return triumphs_; }
    /**
     * @brief Adds `count` losses of category `kind_index` (`0..4`) to `side_index`'s row against
     * `opponent_index` (`0..7`).
     */
    void RecordTriumphLoss(int side_index, int opponent_index, int kind_index, int count = 1);
    /** @brief Returns the recorded loss count for `(side_index, opponent_index, kind_index)`. */
    int TriumphCount(int side_index, int opponent_index, int kind_index) const;
    /** @brief Restores a serialized 5-count triumph row for `(side_index, opponent_index)`. */
    void SetTriumphRow(int side_index, int opponent_index, const TriumphCounts& counts);
    /** @brief Clears all recorded triumph/loss counts. */
    void ClearTriumphs() { triumphs_.clear(); }

    /** @brief Returns all tutorial moment identifiers already shown. */
    const TutorialSet& TutorialSeen() const { return tutorial_seen_; }
    /** @brief True when tutorial moment `moment` has already been shown. */
    bool HasSeenTutorial(const std::string& moment) const {
        return tutorial_seen_.count(moment) != 0;
    }
    /**
     * @brief Marks tutorial moment `moment` as shown.
     * @return True if `moment` was newly inserted; false if already present.
     */
    bool MarkTutorialSeen(std::string moment);
    /** @brief Clears all shown tutorial moment markers. */
    void ClearTutorialSeen() { tutorial_seen_.clear(); }

    /** @brief Returns cached hero candidate names for `side_index`, or `nullptr` if uncached. */
    const HeroNameList* CachedHeroNames(int side_index) const;
    /** @brief Stores and returns `names` in the hero-name cache for `side_index`. */
    const HeroNameList& CacheHeroNames(int side_index, HeroNameList names);

    /** @brief Returns the active `IAiTurnEvents` observer during a computer turn, or `nullptr`. */
    IAiTurnEvents* ActiveAiEvents() const { return active_ai_events_; }

  private:
    friend class ScopedAiTurnEvents;

    Rng rng_;                         ///< Deterministic per-game dice sequence.
    Bugs bugs_;                       ///< Per-game 1993 bug-compatibility flags.
    std::filesystem::path data_dir_;  ///< Root directory of original Warlords II assets.
    Types types_;                     ///< Loaded `ARMYTYPE.DAT` unit definitions.
    MapHandle map_;                   ///< Owned scenario `Map`.
    ArmyRegistry armies_;             ///< Army pool, active slot table, and spatial index.
    int turn_ = 1;                    ///< Current 1-based round number.
    LogList log_;                     ///< Chronological game event log.
    bool greatest_ = false;           ///< Greatest Warlord campaign mode flag.
    SideList sides_;                  ///< Participating scenario factions in setup order.
    int current_side_index_ = kNone;  ///< Authoritative index into `sides_`, or `kNone`.
    DiplomacyMatrix diplomacy_;       ///< Inter-faction diplomatic stances, treaties, and trust.
    FogOfWar explored_;               ///< Per-side tile exploration masks.
    TowerSet towers_;                 ///< Flat tile indices (`y * width + x`) with towers.
    bool won_ = false;                ///< True once victory has been achieved.
    bool over_ = false;               ///< True once all factions have fallen.
    bool no_humans_said_ = false;     ///< True once "no humans left" has been announced.
    bool surrender_offered_ = false;  ///< True once AI opponents offered surrender.
    bool surrender_shown_ = false;    ///< True once the surrender dialog was shown to a human.
    Ending ending_;                   ///< Latest round-end victory/defeat/fallen report.
    HistoryList history_;             ///< Recorded per-round history snapshots.
    DeedMap deeds_;                   ///< Current round's notable deeds per side (`<= 2`).
    TriumphMatrix triumphs_;          ///< Cumulative casualties slain/lost per side pair.
    TutorialSet tutorial_seen_;       ///< Shown tutorial moment identifiers.
    HeroNamePool hero_names_;         ///< Lazy `HERONAM*.DAT` cache per side index.
    TurnReportTable turn_news_{};     ///< Transient turn-opening news reports (`0..7`).
    PathfinderHandle pathfinder_;     ///< Owned movement cost-grid and route-cache service.
    TurnSchedule turn_schedule_;      ///< Round turn-order permutation and cursor.
    IAiTurnEvents* active_ai_events_ = nullptr;  ///< Non-owning AI turn observer while active.
};

/**
 * @brief RAII scope guard binding an `IAiTurnEvents*` observer onto a `Game` for the duration of a
 * computer turn and restoring the previous pointer on exit.
 */
class ScopedAiTurnEvents {
  public:
    ScopedAiTurnEvents(Game& game, IAiTurnEvents* events);
    ~ScopedAiTurnEvents();

    ScopedAiTurnEvents(const ScopedAiTurnEvents&) = delete;
    ScopedAiTurnEvents& operator=(const ScopedAiTurnEvents&) = delete;

  private:
    Game& game_;                         ///< Game aggregate whose observer slot is bound.
    IAiTurnEvents* previous_ = nullptr;  ///< Previous observer restored on scope exit.
};

}  // namespace w2
