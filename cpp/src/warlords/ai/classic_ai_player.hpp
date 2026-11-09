/**
 * @file
 * @brief `ClassicAiPlayer` strategy encapsulating 1993 Warlords II `AIData`, per-`ArmyId`
 * standing orders (`ClassicArmyOrder`), and shared city neighbour tables.
 */
#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "nlohmann/json.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/model/computer_player.hpp"

namespace w2 {

class ClassicAiPlayer;

/**
 * @brief Extension interface implemented by `ClassicAiPlayer` and classic-backed decorators
 * (such as `RaiderPlayer`) so classic AI phase helpers can access the underlying `ClassicAiPlayer`.
 */
class IClassicBackedPlayer : public IComputerPlayer {
  public:
    virtual ClassicAiPlayer& ClassicImpl() = 0;
    virtual const ClassicAiPlayer& ClassicImpl() const = 0;
};

/**
 * @brief Concrete `IComputerPlayer` strategy implementing the 1993 Warlords II nineteen-phase AI.
 *
 * Owns the side's `AIData` block, a private `ArmyOrderMap` mapping `ArmyId` handles to
 * `ClassicArmyOrder` records, and a handle to the game's initial land-distance city
 * `Neighbours` table.
 *
 * Invariants:
 * - `data_.groups[1..4]` are always non-null `Group` pointers.
 * - `orders_` stores standing orders keyed by `ArmyId`; absent entries read as default idle
 *   `ClassicArmyOrder` values.
 *
 * Not thread-safe without external synchronization.
 */
class ClassicAiPlayer final : public IClassicBackedPlayer {
  public:
    /** @brief Per-army standing-order map keyed by `ArmyId`. */
    using ArmyOrderMap = absl::flat_hash_map<ArmyId, ClassicArmyOrder>;
    /** @brief Shared city-neighbour table computed once from turn-1 land paths. */
    using SharedNeighbours = std::shared_ptr<Neighbours>;

    ClassicAiPlayer();
    ClassicAiPlayer(AIData initial_data, SharedNeighbours neighbours, const ArmyRegistry* registry);

    ClassicAiPlayer& ClassicImpl() override { return *this; }
    const ClassicAiPlayer& ClassicImpl() const override { return *this; }

    void PlayTurn(Game& game, Side& side, IAiTurnEvents* events = nullptr) override;
    void OnTurnStarted(Side& side) override;
    AttitudeBreakdown EvaluateProposal(const Game& game, SideId ai_side,
                                       SideId proposer_side) const override;
    City* ChooseHeroCity(Game& game, Side& side, City* fallback) override;
    bool OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) override;
    void RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos, int heroes_lost,
                      int armies_lost, bool all_lost, bool city_tile) override;
    void RecordBattleObservation(Game& game, SideId observer, TilePos pos) override;
    nlohmann::ordered_json SaveState() const override;
    absl::Status LoadState(const nlohmann::ordered_json& state, const Game& game) override;
    ThoughtSpan Thoughts() const override { return {}; }
    float LastBoardValue() const override { return 0.0F; }
    bool IsClassic() const override { return true; }
    const SideBelief* Belief() const override {
        return inspection_belief_ ? &*inspection_belief_ : nullptr;
    }

    const AIData& Data() const { return data_; }
    AIData& MutableData() { return data_; }

    /** @brief Lazily allocates and returns a fair-information `SideBelief` for UI inspection. */
    SideBelief& EnsureInspectionBelief() {
        if (!inspection_belief_) inspection_belief_.emplace();
        return *inspection_belief_;
    }

    const ClassicArmyOrder& OrderFor(ArmyId army_id) const;
    ClassicArmyOrder& MutableOrderFor(ArmyId army_id);
    void ClearOrder(ArmyId army_id);
    void ResetMovedFlags();

    SharedNeighbours NeighbourTable() const { return neighbours_; }
    void SetNeighbourTable(SharedNeighbours neighbours) { neighbours_ = std::move(neighbours); }
    void BindRegistry(const ArmyRegistry* registry) { registry_ = registry; }

    /** @brief Returns (installing if absent) the `ClassicAiPlayer` on `side`. */
    static ClassicAiPlayer& EnsureOn(Game& game, Side& side);
    /** @brief Returns the `ClassicAiPlayer` backing `side` if present, or `nullptr`. */
    static const ClassicAiPlayer* FindOn(const Side& side);
    /** @brief Returns the mutable `ClassicAiPlayer` backing `side` if present, or `nullptr`. */
    static ClassicAiPlayer* FindMutableOn(Side& side);

  private:
    AIData data_;                             ///< 1993 per-side AI state and assault groups.
    ArmyOrderMap orders_;                     ///< Per-`ArmyId` standing orders and flags.
    ClassicArmyOrder invalid_order_scratch_;  ///< Instance-local scratch for invalid `ArmyId`s.
    std::optional<SideBelief> inspection_belief_;  ///< Optional belief for F10 Brain Inspector.
    SharedNeighbours neighbours_;                  ///< Shared initial 6-nearest-city table.
    const ArmyRegistry* registry_ = nullptr;  ///< Non-owning active army registry for save slots.
};

}  // namespace w2
