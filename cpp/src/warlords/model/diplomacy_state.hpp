/**
 * @file
 * @brief Encapsulated 8x8 diplomatic relationship matrix (`w2::DiplomacyMatrix`).
 */
#pragma once

#include <array>

#include "warlords/model/ids.hpp"

namespace w2 {

/**
 * @brief Pairwise diplomatic relations, treaty offers, alliances, truces, and trust across 8 sides.
 *
 * Maintains an $8 \times 8$ relational matrix indexed by `(from_side, to_side)`
 * for asymmetric proposals, gold tributes, joint-war requests, and directional
 * trust, alongside symmetric active treaties (`state_`, `allied_`, `truce_turns_`)
 * and per-side allied target broadcasts.
 *
 * Invariants:
 * - Active alliances (`IsAllied(a, b)`) hold only when `a != b` and both
 *   directions are at peace (`0`).
 * - `TruceTurns` is always `>= 0`.
 * - `Trust` is clamped to `[-100, +100]`.
 *
 * Not thread-safe without external synchronization; owned by a single `Game`.
 */
class DiplomacyMatrix {
  public:
    /** @brief Constructs a default-initialized matrix (all states 0, targets `kNone`). */
    DiplomacyMatrix();

    /**
     * @brief Resets all pairwise states and proposals to `initial_state` and clears all deals.
     * @param initial_state Base diplomatic state (`0` peace, `1` uneasy, `2` war).
     */
    void Reset(int initial_state);

    /**
     * @brief Returns the active diplomatic state from `from_side` toward `to_side` (`2` if
     * invalid).
     */
    int State(SideId from_side, SideId to_side) const;

    /** @brief Sets the directional diplomatic state slot `(from_side, to_side)` directly. */
    void SetState(SideId from_side, SideId to_side, int state);

    /** @brief Sets the active diplomatic state symmetrically in both directions. */
    void SetSymmetricState(SideId first_side, SideId second_side, int state);

    /**
     * @brief Returns `from_side`'s pending proposal toward `to_side` (falls back to `State`).
     */
    int Proposal(SideId from_side, SideId to_side) const;

    /** @brief Sets the directional proposal slot `(from_side, to_side)` directly (`kNone` clears).
     */
    void SetProposal(SideId from_side, SideId to_side, int proposed_state);

    /** @brief Sets the pending proposal symmetrically in both directions. */
    void SetSymmetricProposal(SideId first_side, SideId second_side, int proposed_state);

    /**
     * @brief Records `from_side` proposing `proposed_state` to `to_side`.
     *
     * Any proposal other than peace (`0`) automatically withdraws an alliance offer.
     */
    void Propose(SideId from_side, SideId to_side, int proposed_state);

    /** @brief True when `from_side` and `to_side` are distinct sides at peace and mutually allied.
     */
    bool IsAllied(SideId from_side, SideId to_side) const;

    /** @brief Returns the raw alliance flag stored for `(from_side, to_side)`. */
    bool AlliedFlag(SideId from_side, SideId to_side) const;

    /**
     * @brief Forges or breaks a mutual alliance between `first_side` and `second_side`.
     *
     * Forging an alliance also sets both directions' state and proposal to peace (`0`).
     */
    void SetAllied(SideId first_side, SideId second_side, bool allied);

    /** @brief True when `from_side` currently offers an alliance to `to_side`. */
    bool ProposalAllied(SideId from_side, SideId to_side) const;

    /** @brief Sets `from_side`'s alliance offer to `to_side` (offering also proposes peace). */
    void ProposeAlliance(SideId from_side, SideId to_side, bool propose);

    /** @brief Remaining turns of active truce between `from_side` and `to_side`. */
    int TruceTurns(SideId from_side, SideId to_side) const;

    /** @brief Sets symmetric truce duration between `first_side` and `second_side` (`>= 0`). */
    void SetTruce(SideId first_side, SideId second_side, int turns);

    /** @brief Gold tribute attached to `from_side`'s proposal toward `to_side`. */
    int GoldOffer(SideId from_side, SideId to_side) const;

    /** @brief Sets gold tribute attached to `from_side`'s proposal toward `to_side`. */
    void SetGoldOffer(SideId from_side, SideId to_side, int gold);

    /** @brief Joint-war target side requested by `from_side` from `to_side`, or `SideId::None()`.
     */
    SideId WarTarget(SideId from_side, SideId to_side) const;

    /** @brief Sets the joint-war target side requested by `from_side` from `to_side`. */
    void SetWarTarget(SideId from_side, SideId to_side, SideId target_side);

    /** @brief Relational trust score (`[-100, +100]`) held by `from_side` toward `to_side`. */
    int Trust(SideId from_side, SideId to_side) const;

    /** @brief Adds `delta` to `from_side`'s trust toward `to_side`, clamped to `[-100, +100]`. */
    void AddTrust(SideId from_side, SideId to_side, int delta);

    /** @brief Primary target city broadcast by `side` to its allies, or `CityId::None()`. */
    CityId AlliedTargetCity(SideId side) const;

    /** @brief Sets the primary target city broadcast by `side` to its allies. */
    void SetAlliedTargetCity(SideId side, CityId city_id);

    /**
     * @brief Resets every state and proposal pair involving `fallen_side` (including self) to
     * `neutral_state`.
     */
    void ResetFallenSide(SideId fallen_side, int neutral_state);

    /** @brief Read-only view of the 64-entry pairwise state table for serialization. */
    const std::array<int, 64>& StateTable() const { return state_; }
    /** @brief Restores the 64-entry pairwise state table from serialization. */
    void SetStateTable(const std::array<int, 64>& table) { state_ = table; }

    /** @brief Read-only view of the 64-entry pairwise proposal table for serialization. */
    const std::array<int, 64>& ProposalTable() const { return proposed_; }
    /** @brief Restores the 64-entry pairwise proposal table from serialization. */
    void SetProposalTable(const std::array<int, 64>& table) { proposed_ = table; }

    /** @brief Read-only view of the 64-entry alliance table for serialization. */
    const std::array<bool, 64>& AlliedTable() const { return allied_; }
    /** @brief Restores the 64-entry alliance table from serialization. */
    void SetAlliedTable(const std::array<bool, 64>& table) { allied_ = table; }

    /** @brief Read-only view of the 64-entry proposed-alliance table for serialization. */
    const std::array<bool, 64>& ProposeAlliedTable() const { return propose_allied_; }
    /** @brief Restores the 64-entry proposed-alliance table from serialization. */
    void SetProposeAlliedTable(const std::array<bool, 64>& table) { propose_allied_ = table; }

    /** @brief Read-only view of the 64-entry truce-turns table for serialization. */
    const std::array<int, 64>& TruceTurnsTable() const { return truce_turns_; }
    /** @brief Restores the 64-entry truce-turns table from serialization. */
    void SetTruceTurnsTable(const std::array<int, 64>& table) { truce_turns_ = table; }

    /** @brief Read-only view of the 64-entry gold-offer table for serialization. */
    const std::array<int, 64>& GoldOfferTable() const { return gold_offer_; }
    /** @brief Restores the 64-entry gold-offer table from serialization. */
    void SetGoldOfferTable(const std::array<int, 64>& table) { gold_offer_ = table; }

    /** @brief Read-only view of the 64-entry joint-war target table for serialization. */
    const std::array<int, 64>& WarTargetTable() const { return war_target_; }
    /** @brief Restores the 64-entry joint-war target table from serialization. */
    void SetWarTargetTable(const std::array<int, 64>& table) { war_target_ = table; }

    /** @brief Read-only view of the 64-entry trust table for serialization. */
    const std::array<int, 64>& TrustTable() const { return trust_; }
    /** @brief Restores the 64-entry trust table from serialization. */
    void SetTrustTable(const std::array<int, 64>& table) { trust_ = table; }

    /** @brief Read-only view of the 8-entry allied target city table for serialization. */
    const std::array<int, 8>& AlliedTargetCityTable() const { return allied_target_city_; }
    /** @brief Restores the 8-entry allied target city table from serialization. */
    void SetAlliedTargetCityTable(const std::array<int, 8>& table) { allied_target_city_ = table; }

  private:
    std::array<int, 64> state_{};            ///< Active diplomatic state (`0`..`2`).
    std::array<int, 64> proposed_{};         ///< Proposed diplomatic state, or `kNone`.
    std::array<bool, 64> allied_{};          ///< Mutual military alliance flags.
    std::array<bool, 64> propose_allied_{};  ///< Directional alliance offer flags.
    std::array<int, 64> truce_turns_{};      ///< Remaining turns of active truce (`>= 0`).
    std::array<int, 64> gold_offer_{};       ///< Gold tribute offered (`> 0`) or demanded (`< 0`).
    std::array<int, 64> war_target_{};       ///< Joint-war target side index, or `kNone`.
    std::array<int, 64> trust_{};            ///< Directional trust score (`[-100, +100]`).
    std::array<int, 8> allied_target_city_;  ///< Broadcast allied target city per side, or `kNone`.
};

}  // namespace w2
