/**
 * @file
 * @brief Diplomacy rules: pairwise states, proposals, alliances, truces, tribute deals, and
 * ratings.
 */
#pragma once

#include <map>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2::diplomacy {

using w2::AttitudeBreakdown;
using w2::AttitudeFactor;

/**
 * @brief The three base diplomatic states between two sides, mildest first.
 *
 * kPeace forbids attacks, kIntermediate ("uneasy") forbids attacking cities
 * without a declaration, kWar allows everything. Higher means more hostile.
 */
constexpr int kPeace = 0, kIntermediate = 1, kWar = 2;
/** @brief Display names of the base states, indexed by kPeace..kWar. */
extern const char* const kStateNames[3];

/** @brief The diplomatic titles (STRING.DAT group 106), best first. */
extern const char* const kTitles[8];
/**
 * @brief Which titles are used, by how many sides are in play.
 * @param n The number of sides still in the game (1-8; others count as 8).
 * @return 1-based indices into kTitles, one per rank, best first.
 */
const std::vector<int>& RatingRanks(int n);

/** @brief Sets every pair to war if Diplomacy is off, peace if it is on, and clears all deals. */
void Init(Game& game);
/** @brief The state between two sides; war for anything out of range. */
int State(const Game& game, int from_side, int to_side);
/** @brief Whether the two sides are anything but at peace (uneasy counts as war). */
inline bool AtWar(const Game& game, int from_side, int to_side) {
    return State(game, from_side, to_side) != kPeace;
}
/** @brief May `from_side` attack `to_side`? A side at peace may not; neutrals are fair game. */
bool MayAttack(const Game& game, int from_side, int to_side);
/** @brief `from_side` proposes a state to `to_side`; anything but peace withdraws an alliance
 * offer. */
void Propose(Game& game, int from_side, int to_side, int proposed_state);
/** @brief The state `from_side` proposes to `to_side`, or the current state if none. */
int Proposal(const Game& game, int from_side, int to_side);
/**
 * @brief Applies one side's proposals to every other side.
 * @return Messages for the player, in the order things happened.
 */
std::vector<std::string> Apply(Game& game, Side& side);
/**
 * @brief Must `from_side` be asked before it attacks `to_side`'s tile?
 */
bool MustDeclare(const Game& game, int from_side, int to_side, const City* city);
/**
 * @brief The answer yes to "Shall we attack anyway?": war at once, both ways.
 */
void AttackAnyway(Game& game, int from_side, int to_side);
/** @brief At the end of a side's turn, its peace overtures count toward its diplomatic score. */
void ScoreUpdate(Game& game, Side& side);
/**
 * @brief Adds what a peaceful move from `from_state` to `to_state` is worth to the diplomatic
 * score.
 */
int AddScore(Game& game, Side& side, int from_state, int to_state);
/** @brief Every living side's diplomatic title, by side index. */
std::map<int, std::string> Ratings(const Game& game);

/** @brief Are the two sides mutually Allied (sharing fog-of-war vision and targets)? */
bool IsAllied(const Game& game, int from_side, int to_side);
/** @brief Makes or breaks an alliance, both ways; making one also makes peace. */
void SetAllied(Game& game, int from_side, int to_side, bool allied);

/** @brief Does `from_side` propose an Alliance with `to_side`? */
bool ProposalAllied(const Game& game, int from_side, int to_side);
/** @brief Sets `from_side`'s alliance offer to `to_side`; offering also proposes peace. */
void ProposeAlliance(Game& game, int from_side, int to_side, bool propose);

/** @brief Remaining turns of active peace/alliance truce between the two sides. */
int TruceTurns(const Game& game, int from_side, int to_side);
/** @brief Sets the truce between the two sides, both ways, never below zero. */
void SetTruce(Game& game, int from_side, int to_side, int turns);

/** @brief Gold tribute attached to `from_side`'s proposal toward `to_side` (>0 offered, <0
 * demanded). */
int GoldOffer(const Game& game, int from_side, int to_side);
/** @brief Sets the gold tribute attached to `from_side`'s proposal toward `to_side`. */
void SetGoldOffer(Game& game, int from_side, int to_side, int gold);

/** @brief Joint war target side index (0..7, or kNone) requested by `from_side` from `to_side`. */
int WarTarget(const Game& game, int from_side, int to_side);
/** @brief Sets the joint war target `from_side` requests from `to_side`. */
void SetWarTarget(Game& game, int from_side, int to_side, int target_side);

/** @brief Relational trust score (-100 to +100) held by `from_side` toward `to_side`. */
int Trust(const Game& game, int from_side, int to_side);
/** @brief Adds `delta` to `from_side`'s trust in `to_side`, clamped to -100..+100. */
void AddTrust(Game& game, int from_side, int to_side, int delta);

/** @brief Primary target city index broadcast by `side` to its allies (or kNone). */
int AlliedTargetCity(const Game& game, int side);
/** @brief Sets the target city `side` broadcasts to its allies. */
void SetAlliedTargetCity(Game& game, int side, int city_index);

/**
 * @brief Computes a transparent breakdown of why `ai_side` holds its current diplomatic
 * disposition toward `target_side` by delegating to `ai_side`'s `IComputerPlayer` strategy.
 */
AttitudeBreakdown ComputeAttitude(const Game& game, int ai_side, int target_side);

}  // namespace w2::diplomacy
