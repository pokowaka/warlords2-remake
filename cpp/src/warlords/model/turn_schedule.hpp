/**
 * @file
 * @brief Encapsulated round turn-order permutation and cursor (`w2::TurnSchedule`).
 */
#pragma once

#include <array>

#include "warlords/model/ids.hpp"
#include "warlords/rng.hpp"

namespace w2 {

/**
 * @brief Permutation of the 8 faction slots (`0..7`) and the active turn cursor within a round.
 *
 * Starts each game in canonical order `{0, 1, 2, 3, 4, 5, 6, 7}` with `cursor_ == -1`
 * (before the first faction acts). At the end of each round (`AtRoundEnd()`),
 * `StartNewRound` resets the order and optionally performs the 20 random pair
 * swaps specified by the Random Turns rule.
 *
 * Invariants:
 * - `cursor_` lies in `[-1, 7]`.
 * - `order_` contains side indices in `[0, 7]`.
 */
class TurnSchedule {
  public:
    /** @brief Constructs the canonical `0..7` turn schedule positioned before the first turn. */
    TurnSchedule() = default;

    /** @brief Resets `order_` to `{0..7}` and rewinds `cursor_` to `-1`. */
    void Reset();

    /** @brief Rewinds `cursor_` to `-1` without altering `order_`. */
    void RewindCursor() { cursor_ = -1; }

    /**
     * @brief Rebuilds `order_` for a new round, shuffling with 20 swaps when `random_turns` holds.
     * @param random_turns Whether the scenario's Random Turns option is enabled.
     * @param rng Game RNG used for the 20 pair swaps.
     */
    void StartNewRound(bool random_turns, Rng& rng);

    /** @brief True when all 8 slots in the current round have already been visited (`cursor_ >=
     * 7`).
     */
    bool AtRoundEnd() const { return cursor_ >= 7; }

    /**
     * @brief Advances `cursor_` by one slot and returns the `SideId` scheduled at that slot.
     *
     * Caller must check `!AtRoundEnd()` (calling `StartNewRound` first when at the end).
     */
    SideId Advance();

    /** @brief Current slot cursor (`-1` before first turn of round, else `0..7`). */
    int Cursor() const { return cursor_; }

    /** @brief Sets the slot cursor directly (clamped to `[-1, 7]`). */
    void SetCursor(int cursor);

    /** @brief Places `cursor_` on the first slot matching `side`, or `-1` if not found. */
    void SyncCursorToSide(SideId side);

    /** @brief Returns the `SideId` at `slot_index` (`0..7`), or `SideId::None()` if out of bounds.
     */
    SideId SideAt(int slot_index) const;

    /** @brief Returns the `SideId` at `Cursor()`, or `SideId::None()` when `Cursor() < 0`. */
    SideId CurrentSideId() const { return SideAt(cursor_); }

    /** @brief Read-only view of the 8-slot side permutation. */
    const std::array<int, 8>& Order() const { return order_; }

    /** @brief Restores the 8-slot side permutation from a saved game. */
    void SetOrder(const std::array<int, 8>& order);

  private:
    std::array<int, 8> order_{0, 1, 2, 3, 4, 5, 6, 7};  ///< Scheduled side indices for the round.
    int cursor_ = -1;  ///< Active position in `order_`, or `-1` before slot 0.
};

}  // namespace w2
