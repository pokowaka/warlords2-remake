/**
 * @file
 * @brief `ModernMlPlayer` strategy encapsulating fair-information `SideBelief`, neural policy
 * thoughts (`AIThought`), board-value telemetry, and optional policy/sampler overrides.
 */
#pragma once

#include <array>
#include <utility>
#include <vector>

#include "absl/status/status.h"

#include "nlohmann/json.hpp"
#include "warlords/ai/attitude.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/model/computer_player.hpp"

namespace w2 {

class ModernMlPlayer;

/**
 * @brief Extension interface implemented by `ModernMlPlayer` and modern-backed decorators
 * (such as `FrozenPlayer`) so modern AI helpers can access the underlying `ModernMlPlayer`.
 */
class IModernBackedPlayer : public IComputerPlayer {
  public:
    virtual ModernMlPlayer& ModernImpl() = 0;
    virtual const ModernMlPlayer& ModernImpl() const = 0;
};

/**
 * @brief Concrete `IComputerPlayer` strategy executing the fair-information neural-policy AI.
 *
 * Owns the side's `SideBelief` world model, turn-by-turn `AIThought` telemetry for the
 * F10 Brain Inspector, latest critic board value, battle casualty grudge counters, and
 * optional custom `PolicyNetwork` / `RolloutSampler` pointers used during PPO training.
 *
 * Not thread-safe without external synchronization.
 */
class ModernMlPlayer final : public IModernBackedPlayer {
  public:
    /** @brief List of scored macro-actions recorded during the most recent turn. */
    using ThoughtList = std::vector<AIThought>;
    /** @brief Per-opponent integer counter array. */
    using StatArray = std::array<int, 8>;

    ModernMlPlayer() = default;
    explicit ModernMlPlayer(const ai::ml::PolicyNetwork* custom_policy,
                            ai::ml::RolloutSampler* sampler = nullptr);

    ModernMlPlayer& ModernImpl() override { return *this; }
    const ModernMlPlayer& ModernImpl() const override { return *this; }

    void PlayTurn(Game& game, Side& side, IAiTurnEvents* events = nullptr) override;
    AttitudeBreakdown EvaluateProposal(const Game& game, SideId ai_side,
                                       SideId proposer_side) const override;
    City* ChooseHeroCity(Game& game, Side& side, City* fallback) override;
    bool OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) override;
    void RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos, int heroes_lost,
                      int armies_lost, bool all_lost, bool city_tile) override;
    void RecordBattleObservation(Game& game, SideId observer, TilePos pos) override;
    nlohmann::ordered_json SaveState() const override;
    absl::Status LoadState(const nlohmann::ordered_json& state, const Game& game) override;
    ThoughtSpan Thoughts() const override { return thoughts_; }
    float LastBoardValue() const override { return last_board_value_; }
    bool IsClassic() const override { return false; }
    const SideBelief* Belief() const override { return &belief_; }

    SideBelief& MutableBelief() { return belief_; }
    const SideBelief& GetBelief() const { return belief_; }
    void ClearThoughts() { thoughts_.clear(); }
    void PushThought(AIThought thought) { thoughts_.push_back(std::move(thought)); }
    void SetLastBoardValue(float value) { last_board_value_ = value; }
    void SetCustomPolicy(const ai::ml::PolicyNetwork* policy) { custom_policy_ = policy; }
    void SetRolloutSampler(ai::ml::RolloutSampler* sampler) { sampler_ = sampler; }
    const ai::BattleGrudge& Grudge() const { return grudge_; }

    /** @brief Returns (installing if absent) the `ModernMlPlayer` on `side`. */
    static ModernMlPlayer& EnsureOn(Side& side);
    /** @brief Returns the `ModernMlPlayer` backing `side` if present, or `nullptr`. */
    static const ModernMlPlayer* FindOn(const Side& side);
    /** @brief Returns the mutable `ModernMlPlayer` backing `side` if present, or `nullptr`. */
    static ModernMlPlayer* FindMutableOn(Side& side);

  private:
    SideBelief belief_;                                     ///< Fair-information world belief.
    ThoughtList thoughts_;                                  ///< Recent neural policy decisions.
    ai::BattleGrudge grudge_;                               ///< Battle grudge casualty counters.
    StatArray armies_killed_{};                             ///< Defending armies slain by rival.
    StatArray battles_{};                                   ///< Defending battles fought by rival.
    StatArray city_battles_{};                              ///< Defending city sieges by rival.
    float last_board_value_ = 0.0F;                         ///< Latest critic value prediction.
    const ai::ml::PolicyNetwork* custom_policy_ = nullptr;  ///< Optional policy override.
    ai::ml::RolloutSampler* sampler_ = nullptr;             ///< Optional rollout sampler.
};

}  // namespace w2
