/**
 * @file
 * @brief `TurnEngine` domain service orchestrating turn progression, round endings,
 * eliminations, surrender, army disembarkation/reset, and `TurnReport` population.
 */
#pragma once

#include <vector>

#include "warlords/types.hpp"

namespace w2 {

/**
 * @brief Domain service coordinating turn start, movement reset, encampments, capital
 * tracking, round-end elimination, victory checks, and `TurnReport` updates on `Game`.
 */
class TurnEngine {
  public:
    /** @brief Ordered list of army pointers. */
    using ArmyStack = std::vector<Army*>;

    /** @brief Binds a turn engine to `game` (must outlive this service instance). */
    explicit TurnEngine(Game& game) : game_(game) {}

    /** @brief Starts the opening turn of a newly created game. */
    Side* Begin();

    /**
     * @brief Runs turn-opening steps for `side` and populates `Game::TurnNews(SideId(side.index))`.
     * @return False if a computer `side` holds zero cities and skips its turn.
     */
    bool StartTurn(Side& side);

    /** @brief Advances deferred city production for a human `side` once hero offers settle. */
    void HumanProduction(Side& side);

    /** @brief Finishes the current side's turn and advances to the next active side (or `nullptr`).
     */
    Side* EndTurn();

    /** @brief Returns the next living human side in the round schedule, or `nullptr`. */
    Side* NextHuman() const;

    /** @brief Updates `side`'s current capital coordinates before its turn begins. */
    void FollowCapital(Side& side);

    /** @brief Eliminates all active sides left with zero cities, returning their `Fallen` records.
     */
    std::vector<Fallen> EliminateFallen();

    /** @brief Runs end-of-round elimination, victory checks, and neutral city production. */
    Ending EndRound(Side* last = nullptr);

    /** @brief Evaluates victory, triumph, surrender, and total-elimination conditions. */
    Ending CheckEnd();

    /** @brief Accepts an offered computer surrender on behalf of `side`. */
    void AcceptSurrender(Side& side);

    /** @brief True when a computer surrender has been offered and not yet shown to the human. */
    bool SurrenderPending() const;

    /** @brief Razes `side`'s cities, disbands its armies, and reveals the map on resignation. */
    void Resign(Side& side);

    /** @brief Disbands `armies` belonging to `side`, dropping hero items and cancelling lost
     * quests. */
    void Disband(Side& side, const ArmyStack& armies);

    /** @brief Drowns non-flying armies of `side` left stranded on water without a ship. */
    int SinkStranded(Side& side, int x = kNone, int y = kNone);

  private:
    Side* AdvanceToNextSide(Side* leaving);
    Fallen EliminateSide(Side& side, bool boxed);
    void ResetMovement(Side& side);
    void Encamp(Side& side);
    void ComeAshore(Side& side);
    void NotifyTurnQuest(Side& side);

    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
