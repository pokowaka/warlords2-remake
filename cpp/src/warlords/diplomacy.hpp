// Diplomacy: the state between every pair of sides, proposals, alliances,
// truces, tribute deals, joint-war pacts, and transparent attitude breakdowns.
//
// docs/rules.md > Diplomacy. Base states are PEACE (0), INTERMEDIATE/UNEASY (1),
// and WAR (2), with an optional mutual ALLIED tier on top of PEACE.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2::diplomacy {

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
 *
 * Escalation takes effect at once; de-escalation only when the other side
 * proposes the same or milder. Mutual alliance proposals at peace forge an
 * alliance. Truces count down here too.
 * @return Messages for the player, in the order things happened.
 */
std::vector<std::string> Apply(Game& game, Side& side);
/**
 * @brief Must `from_side` be asked before it attacks `to_side`'s tile?
 *
 * The "Thou art attacking without first having declared war!" question
 * (attack_tile in the reference). Yes at peace, and when uneasy if the tile
 * is a city.
 */
bool MustDeclare(const Game& game, int from_side, int to_side, const City* city);
/**
 * @brief The answer yes to "Shall we attack anyway?": war at once, both ways.
 *
 * Proposals turn to war too, and it costs the attacker 1d100+100 diplomatic
 * score from peace, 1d15+10 from uneasy. History notes the treachery.
 */
void AttackAnyway(Game& game, int from_side, int to_side);
/** @brief At the end of a side's turn, its peace overtures count toward its diplomatic score. */
void ScoreUpdate(Game& game, Side& side);
/**
 * @brief Adds what a peaceful move from `from_state` to `to_state` is worth to the diplomatic
 * score.
 * @return The amount added, or kNone when the move is worth nothing.
 */
int AddScore(Game& game, Side& side, int from_state, int to_state);
/** @brief Every living side's diplomatic title, by side index (diplomatic_rating in the reference).
 */
std::map<int, std::string> Ratings(const Game& game);

// ------------------------------------------------ Upgraded Diplomacy & Parley

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

/** @brief One labelled contribution to an attitude score. */
struct AttitudeFactor {
    std::string label;  ///< What the factor is, as shown to the player.
    int delta = 0;      ///< How much it adds to (or takes from) the score.
};

/**
 * @brief Why one side feels the way it does about another, and what deal it will take.
 *
 * Shown to the player in the parley screen; also feeds the diplomacy policy.
 */
struct AttitudeBreakdown {
    int from_side = kNone;                ///< The side holding the attitude.
    int to_side = kNone;                  ///< The side the attitude is toward.
    std::string personality_name;         ///< Name of `from_side`'s AI personality.
    std::string personality_archetype;    ///< Archetype of that personality.
    std::string motto;                    ///< The personality's motto.
    int total_score = 0;                  ///< Sum of the factors, clamped to -100..+100.
    int trust_score = 0;                  ///< `from_side`'s trust in `to_side`.
    int truce_remaining = 0;              ///< Turns of truce left between them.
    std::vector<AttitudeFactor> factors;  ///< The contributions to the score, in order.
    std::string stance_summary;           ///< A short label for the stance.
    std::string what_it_takes;            ///< What the player must do to get a deal.
    int peace_gold_needed = 0;  ///< 0 = free, >0 = gold tribute required, kNone = will not accept.
    int ally_gold_needed = 0;   ///< 0 = free, >0 = gold tribute required, kNone = will not accept.
};

/**
 * @brief Computes a transparent, human-readable breakdown of why `ai_side` holds its
 * current diplomatic disposition toward `target_side`, and what deal it will accept.
 *
 * Only cities `ai_side` can see count, so it never leaks the hidden map.
 */
AttitudeBreakdown ComputeAttitude(const Game& game, int ai_side, int target_side);

}  // namespace w2::diplomacy
