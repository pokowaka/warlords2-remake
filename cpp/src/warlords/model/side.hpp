/**
 * @file
 * @brief Encapsulated `w2::Side` faction entity and `Advisor`, `AIThought`, and
 * `DiplomaticMissive` value structs.
 */
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "warlords/model/computer_player.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/quest.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

class City;
class Map;

/** @brief City-count threshold markers governing voiced advisor remarks (`cues.cpp`). */
struct Advisor {
    int dir = 0;   ///< `1` after rising past a multiple of 5 cities, `2` after falling below one.
    int mark = 0;  ///< City count threshold (`0, 5, 10, ...`) last remarked on.
};

/**
 * @brief A scored macro-action recorded by the modern ML AI for the in-game F10 Brain Inspector.
 */
struct AIThought {
    std::string category;     ///< Action category label.
    std::string actor_name;   ///< Acting stack or side name.
    std::string target_name;  ///< Target entity name.
    int sx = kNone;           ///< Actor origin column.
    int sy = kNone;           ///< Actor origin row.
    int tx = kNone;           ///< Target column.
    int ty = kNone;           ///< Target row.
    float score = 0.0F;       ///< Policy logit/score.
    float win_prob = 0.0F;    ///< Estimated combat win probability.
    bool stage_near = false;  ///< True when staging adjacent rather than striking directly.
    std::string detail;       ///< Diagnostic summary string for the inspector UI.
};

/** @brief A turn-start diplomatic parley missive sent from an AI side to a human side. */
struct DiplomaticMissive {
    int from_side = kNone;          ///< Sending faction index (`0..7`).
    int to_side = kNone;            ///< Receiving faction index (`0..7`).
    int proposed_state = kNone;     ///< Proposed diplomatic state (`kPeace`, `kWar`, ...).
    bool propose_alliance = false;  ///< True when a formal alliance is proposed.
    int gold_demand = 0;            ///< `> 0`: tribute demanded; `< 0`: gift offered.
    int joint_war_target = kNone;   ///< Target side index for a joint-war pact, or `kNone`.
    std::string headline;           ///< Dialog title text.
    std::string body;               ///< Main parley message body.
    std::string reason_summary;     ///< Attitude summary shown beneath the offer.
};

/**
 * @brief One of the eight scenario factions (`0..7`), encapsulating treasury, capital identity,
 * human/computer control, active temple quest, diplomatic missives, and AI strategy ownership.
 *
 * Tracks the faction's scenario capital via `origin_capital_pos_` (`TilePos`) and `capital_city_`
 * (`CityId`), and tracks any relocated wartime seat of power via `current_capital_pos_`
 * (`std::optional<TilePos>`), resolving `City*` pointers on demand through `Map` so `Side` holds
 * zero raw `City*` pointers across map cropping or vector reallocation.
 *
 * Invariants:
 * - `gold_ >= 0` across all treasury mutations (`SetGold`, `AddGold`, `SpendGold`).
 * - `current_capital_pos_`, when engaged, always satisfies `TilePos::IsValid()`.
 * - `IsActive()` holds iff `in_use_ && alive_`; `IsHuman()` and `IsComputer()` partition active
 *   factions by `!computer_` vs `computer_`.
 *
 * Not thread-safe without external synchronization; owned in `Map::sides`.
 */
class Side {
  public:
    /** @brief Optional map coordinate for a relocated capital seat. */
    using OptionalTilePos = std::optional<TilePos>;
    /** @brief Shared handle to an active temple `Quest`. */
    using SharedQuest = std::shared_ptr<Quest>;
    /** @brief Shared handle to a polymorphic `IComputerPlayer` strategy. */
    using SharedComputerPlayer = std::shared_ptr<IComputerPlayer>;
    /** @brief Queue of unread diplomatic parley missives. */
    using MissiveList = std::vector<DiplomaticMissive>;
    /** @brief Per-recipient round number of the most recent missive sent (`0..7`). */
    using MissiveTurnTable = std::array<int, 8>;

    /** @brief Constructs an inactive faction at slot `0`. */
    Side() = default;

    /** @brief Returns this faction's strong identifier (`SideId(0)..SideId(7)`). */
    SideId Id() const { return id_; }
    /** @brief Returns this faction's integer slot index (`0..7`). */
    int Index() const { return id_.Value(); }
    /** @brief Sets this faction's strong identifier. */
    void SetId(SideId side_id) { id_ = side_id; }

    /** @brief Returns the faction's display name. */
    const std::string& Name() const { return name_; }
    /** @brief Sets the faction's display name. */
    void SetName(std::string name);

    /** @brief Returns the faction's primary palette colour index. */
    int Colour() const { return colour_; }
    /** @brief Sets the faction's primary palette colour index. */
    void SetColour(int colour) { colour_ = colour; }

    /** @brief Returns the faction's banner edging colour index. */
    int Edge() const { return edge_; }
    /** @brief Sets the faction's banner edging colour index. */
    void SetEdge(int edge) { edge_ = edge; }

    /** @brief True when this faction slot participates in the scenario at all. */
    bool InUse() const { return in_use_; }
    /** @brief Marks whether this faction slot participates in the scenario. */
    void SetInUse(bool in_use) { in_use_ = in_use; }

    /** @brief True when this faction has not yet been vanquished. */
    bool IsAlive() const { return alive_; }
    /** @brief Sets whether this faction remains alive in play. */
    void SetAlive(bool alive) { alive_ = alive; }

    /** @brief True when this faction is configured under computer control (alive or fallen). */
    bool IsComputerControlled() const { return computer_; }
    /** @brief Sets whether this faction is under computer control. */
    void SetComputerControlled(bool computer) { computer_ = computer; }

    /** @brief True when participating in the scenario and still alive (`InUse() && IsAlive()`). */
    bool IsActive() const { return in_use_ && alive_; }
    /** @brief True when this faction is an active human player. */
    bool IsHuman() const { return in_use_ && alive_ && !computer_; }
    /** @brief True when this faction is an active computer player. */
    bool IsComputer() const { return in_use_ && alive_ && computer_; }

    /** @brief Returns the AI difficulty level (`0..3`: Knight, Baron, Lord, Warlord). */
    int Level() const { return level_; }
    /** @brief Sets the AI difficulty level (`0..3`). */
    void SetLevel(int level) { level_ = level; }

    /** @brief True when Enhanced mode strengthens units built by this faction. */
    bool IsEnhanced() const { return enhanced_; }
    /** @brief Sets the Enhanced unit-strength flag. */
    void SetEnhanced(bool enhanced) { enhanced_ = enhanced; }

    /** @brief True when human players watch this side's movements and battles. */
    bool IsObserved() const { return observe_; }
    /** @brief Sets whether this side's turns are observed on screen. */
    void SetObserved(bool observe) { observe_ = observe; }

    /** @brief Returns the ruler personality card index (`AICARD.DAT`). */
    int Card() const { return card_; }
    /** @brief Sets the ruler personality card index (`AICARD.DAT`). */
    void SetCard(int card) { card_ = card; }

    /** @brief True while a human side's turn-opening city production awaits hero-offer settlement.
     */
    bool IsProductionDue() const { return production_due_; }
    /** @brief Sets whether human turn-opening city production is pending. */
    void SetProductionDue(bool due) { production_due_ = due; }

    /** @brief Returns gold currently held in the faction's treasury (`>= 0`). */
    int Gold() const { return gold_; }
    /** @brief Sets treasury gold, clamped to `>= 0`. */
    void SetGold(int gold);
    /** @brief Adds signed `delta` to treasury gold, clamping the resulting balance to `>= 0`. */
    void AddGold(int delta);
    /**
     * @brief Deducts `amount` gold if the treasury holds at least `amount`.
     * @param amount Non-negative gold cost to spend.
     * @return True if `amount` was deducted; false if `amount < 0` or funds were insufficient.
     */
    bool SpendGold(int amount);

    /** @brief Returns gross city/item income recorded at the start of the side's turn. */
    int Income() const { return income_; }
    /** @brief Sets gross turn income. */
    void SetIncome(int income) { income_ = income; }

    /** @brief Returns total army upkeep recorded at the start of the side's turn. */
    int UpkeepTotal() const { return upkeep_total_; }
    /** @brief Sets total turn army upkeep. */
    void SetUpkeepTotal(int upkeep) { upkeep_total_ = upkeep; }

    /** @brief Returns net gold per turn (`Income() - UpkeepTotal()`). */
    int NetIncome() const { return income_ - upkeep_total_; }

    /** @brief Returns the faction's cumulative diplomatic reputation score. */
    int DiploScore() const { return diplo_score_; }
    /** @brief Sets the cumulative diplomatic reputation score. */
    void SetDiploScore(int score) { diplo_score_ = score; }
    /** @brief Adds `delta` points to the diplomatic reputation score. */
    void AddDiploScore(int delta) { diplo_score_ += delta; }

    /** @brief Returns the AI ally-solidarity score. */
    int AiSolidarity() const { return ai_solidarity_; }
    /** @brief Sets the AI ally-solidarity score. */
    void SetAiSolidarity(int solidarity) { ai_solidarity_ = solidarity; }

    /** @brief Returns the scenario's original capital coordinates `(cap_x, cap_y)`. */
    TilePos OriginCapitalPos() const { return origin_capital_pos_; }
    /** @brief Sets the scenario's original capital coordinates. */
    void SetOriginCapitalPos(TilePos pos) { origin_capital_pos_ = pos; }

    /** @brief Returns the strong `CityId` of this faction's home capital (`CityId::None()` if
     * none). */
    CityId CapitalCityId() const { return capital_city_; }
    /** @brief Sets the strong `CityId` of this faction's home capital. */
    void SetCapitalCityId(CityId city_id) { capital_city_ = city_id; }
    /** @brief True when a valid home capital `CityId` is assigned. */
    bool HasCapitalCity() const { return capital_city_.IsValid(); }
    /** @brief Resolves `CapitalCityId()` on `map`, or returns `nullptr` if unset/invalid. */
    City* CapitalCity(Map& map) const;
    /** @brief Resolves `CapitalCityId()` on `map` (const), or returns `nullptr` if unset/invalid.
     */
    const City* CapitalCity(const Map& map) const;

    /** @brief Returns the relocated active capital coordinate if initialized, else `std::nullopt`.
     */
    const OptionalTilePos& CurrentCapitalPos() const { return current_capital_pos_; }
    /** @brief Sets the relocated active capital coordinate (invalid `pos` resets to `nullopt`). */
    void SetCurrentCapitalPos(TilePos pos);
    /** @brief Clears the relocated active capital coordinate back to `std::nullopt`. */
    void ClearCurrentCapitalPos() { current_capital_pos_.reset(); }
    /** @brief Returns `CurrentCapitalPos()` when set, otherwise `OriginCapitalPos()`. */
    TilePos CapitalPosition() const { return current_capital_pos_.value_or(origin_capital_pos_); }

    /** @brief Returns a non-owning pointer to this side's `IComputerPlayer`, or `nullptr`. */
    IComputerPlayer* ComputerPlayer() const { return computer_player_.get(); }
    /** @brief Returns the shared `IComputerPlayer` handle. */
    const SharedComputerPlayer& ComputerPlayerShared() const { return computer_player_; }
    /** @brief Installs `player` as this faction's computer strategy. */
    void SetComputerPlayer(SharedComputerPlayer player);
    /** @brief True when this faction's computer strategy follows the 1993 Classic AI ruleset. */
    bool IsClassicAi() const;

    /** @brief Returns the shared `Quest` handle (`nullptr` when no quest is active). */
    const SharedQuest& QuestPtr() const { return quest_; }
    /** @brief Returns a non-owning pointer to the active `Quest`, or `nullptr`. */
    Quest* ActiveQuest() const { return quest_.get(); }
    /** @brief True when this faction currently has an active hero quest. */
    bool HasQuest() const { return quest_ != nullptr; }
    /** @brief Assigns `quest` as this faction's active temple quest. */
    void SetQuest(SharedQuest quest);
    /** @brief Abandons or clears any active temple quest. */
    void ClearQuest() { quest_.reset(); }

    /** @brief Returns unread diplomatic missives queued for this faction. */
    const MissiveList& Missives() const { return missives_; }
    /** @brief Enqueues a diplomatic missive at the back of `missives_`. */
    void AddMissive(DiplomaticMissive missive);
    /** @brief Pops and returns the oldest queued diplomatic missive. */
    DiplomaticMissive PopFrontMissive();
    /** @brief Drops all queued diplomatic missives. */
    void ClearMissives() { missives_.clear(); }

    /** @brief Returns the round when this side last sent a missive to `target` (`0` if never). */
    int LastMissiveTurn(SideId target) const;
    /** @brief Records that this side sent a missive to `target` on `turn`. */
    void SetLastMissiveTurn(SideId target, int turn);
    /** @brief Returns the full per-side missive cooldown table. */
    const MissiveTurnTable& LastMissiveTurns() const { return last_missive_turn_; }

    /** @brief Returns read-only advisor city-milestone state. */
    const Advisor& AdvisorState() const { return advisor_; }
    /** @brief Returns mutable advisor city-milestone state. */
    Advisor& MutableAdvisorState() { return advisor_; }
    /** @brief Replaces the advisor city-milestone state. */
    void SetAdvisorState(Advisor advisor) { advisor_ = advisor; }

  private:
    SideId id_ = SideId(0);                 ///< Faction slot identifier (`0..7`).
    std::string name_;                      ///< Faction display name.
    int colour_ = 0;                        ///< Primary palette colour.
    int edge_ = 0;                          ///< Banner edging palette colour.
    int gold_ = 0;                          ///< Gold coins in the treasury (`>= 0`).
    TilePos origin_capital_pos_{0, 0};      ///< Scenario capital coordinates `(cap_x, cap_y)`.
    CityId capital_city_ = CityId::None();  ///< Scenario home capital `CityId`.
    OptionalTilePos current_capital_pos_;   ///< Relocated capital coordinates once turns begin.
    bool computer_ = false;                 ///< True when controlled by an AI strategy.
    int level_ = 0;                         ///< AI difficulty level (`0..3`).
    bool enhanced_ = false;                 ///< True when built units receive Enhanced bonuses.
    bool observe_ = false;                  ///< True when computer turns are shown on screen.
    int diplo_score_ = 0;                   ///< Diplomatic reputation score.
    int card_ = 0;                          ///< Ruler personality card index (`AICARD.DAT`).
    bool in_use_ = false;                   ///< True when active in this scenario setup.
    bool alive_ = false;                    ///< True while still holding cities in play.
    int income_ = 0;                        ///< Turn's gross income.
    int upkeep_total_ = 0;                  ///< Turn's total army upkeep.
    SharedQuest quest_;                     ///< Active hero temple quest, if any.
    SharedComputerPlayer computer_player_;  ///< Polymorphic AI strategy.
    MissiveList missives_;                  ///< Queued diplomatic parley missives.
    MissiveTurnTable last_missive_turn_{};  ///< Round of most recent missive per recipient.
    int ai_solidarity_ = 0;                 ///< AI ally-solidarity score.
    Advisor advisor_;                       ///< Voiced advisor city-count marks.
    bool production_due_ = false;           ///< True while human turn production awaits hero offer.
};

}  // namespace w2
