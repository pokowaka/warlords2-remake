/**
 * @file
 * @brief Training opponent `IComputerPlayer` strategies (`RaiderPlayer`, `FrozenPlayer`) for
 * reinforcement learning and diagnostic evaluation (`w2arena`, `w2aidiag`).
 *
 * `RaiderPlayer` decorates a `ClassicAiPlayer` with an opening raid pass that sends cheap units
 * to capture reachable undefended enemy cities before executing the nineteen-phase 1993 Classic
 * AI. `FrozenPlayer` runs a fixed, non-learning `PolicyNetwork` checkpoint through
 * `ModernMlPlayer` so a training policy cannot co-adapt with a single rule-based adversary.
 */
#pragma once

#include <memory>

#include "absl/status/status.h"

#include "nlohmann/json.hpp"
#include "warlords/ai/classic_ai_player.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/game.hpp"

namespace w2 {

/**
 * @brief Classic-backed `IComputerPlayer` decorator that raids reachable undefended enemy cities
 * at the start of its turn before delegating to an underlying `ClassicAiPlayer`.
 *
 * Not thread-safe without external synchronization.
 */
class RaiderPlayer final : public IClassicBackedPlayer {
  public:
    /** @brief Shared handle to the decorated `ClassicAiPlayer`. */
    using SharedClassicPlayer = std::shared_ptr<ClassicAiPlayer>;

    explicit RaiderPlayer(SharedClassicPlayer base);
    RaiderPlayer(Game& game, Side& side);

    ClassicAiPlayer& ClassicImpl() override { return *base_; }
    const ClassicAiPlayer& ClassicImpl() const override { return *base_; }

    void PlayTurn(Game& game, Side& side, const PauseCallback& pause = nullptr) override;
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

  private:
    SharedClassicPlayer base_;  ///< Underlying 1993 Classic AI strategy.
};

/**
 * @brief Modern-backed `IComputerPlayer` pinned to an immutable `PolicyNetwork` checkpoint.
 *
 * Maintains its own `ModernMlPlayer` belief and grudge memory while always evaluating decisions
 * with `frozen_policy_`. `frozen_policy_` must outlive this `FrozenPlayer`.
 *
 * Not thread-safe without external synchronization.
 */
class FrozenPlayer final : public IModernBackedPlayer {
  public:
    explicit FrozenPlayer(const ai::ml::PolicyNetwork* frozen_policy);

    ModernMlPlayer& ModernImpl() override { return *inner_; }
    const ModernMlPlayer& ModernImpl() const override { return *inner_; }

    void PlayTurn(Game& game, Side& side, const PauseCallback& pause = nullptr) override;
    AttitudeBreakdown EvaluateProposal(const Game& game, SideId ai_side,
                                       SideId proposer_side) const override;
    City* ChooseHeroCity(Game& game, Side& side, City* fallback) override;
    bool OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) override;
    void RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos, int heroes_lost,
                      int armies_lost, bool all_lost, bool city_tile) override;
    void RecordBattleObservation(Game& game, SideId observer, TilePos pos) override;
    nlohmann::ordered_json SaveState() const override;
    absl::Status LoadState(const nlohmann::ordered_json& state, const Game& game) override;
    ThoughtSpan Thoughts() const override { return inner_->Thoughts(); }
    float LastBoardValue() const override { return inner_->LastBoardValue(); }
    bool IsClassic() const override { return false; }
    const SideBelief* Belief() const override { return inner_->Belief(); }

    const ai::ml::PolicyNetwork* Policy() const { return frozen_policy_; }

  private:
    const ai::ml::PolicyNetwork* frozen_policy_ = nullptr;  ///< Non-owning frozen network.
    std::unique_ptr<ModernMlPlayer> inner_;                 ///< Delegated modern AI state.
};

namespace opponents {

/** @brief Opponent personality selector used by training and evaluation harnesses. */
enum class Kind {
    kClassic,  ///< Unmodified 1993 `ClassicAiPlayer`.
    kRaider,   ///< `RaiderPlayer` pre-phase over `ClassicAiPlayer`.
    kFrozen,   ///< `FrozenPlayer` running a fixed `PolicyNetwork` checkpoint.
};

/**
 * @brief Installs the `IComputerPlayer` corresponding to `kind` onto `side` if not already active.
 */
void InstallOpponent(Game& game, Side& side, Kind kind,
                     const ai::ml::PolicyNetwork* frozen = nullptr);

/**
 * @brief Executes one opponent turn for `side` using `kind` (and `frozen` when `Kind::kFrozen`).
 */
void PlayTurn(Game& game, Side& side, Kind kind, const ai::ml::PolicyNetwork* frozen = nullptr);

/**
 * @brief Executes only the Raider's pre-phase, marching cheap units into reachable undefended
 * enemy cities.
 * @return Number of enemy cities captured during the raid pass.
 */
int RaidEmptyCities(Game& game, Side& side);

}  // namespace opponents
}  // namespace w2
