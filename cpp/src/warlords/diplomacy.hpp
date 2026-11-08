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

constexpr int kPeace = 0, kIntermediate = 1, kWar = 2;
extern const char* const kStateNames[3];

// STRING.DAT group 106, best first.
extern const char* const kTitles[8];
/** Which titles are used, by how many sides are in play (1-based ranks). */
const std::vector<int>& RatingRanks(int n);

/** War everywhere if Diplomacy is off, peace if it is on (484e:11bd). */
void Init(Game& g);
/** The state between two sides. */
int State(const Game& g, int a, int b);
inline bool AtWar(const Game& g, int a, int b) {
    return State(g, a, b) != kPeace;
}
/** May `a` attack `b`? A side at peace may not. */
bool MayAttack(const Game& g, int a, int b);
/** Propose a state to another side. */
void Propose(Game& g, int a, int b, int st);
/** The state a side proposes to another. */
int Proposal(const Game& g, int a, int b);
/** Apply one side's proposals. Returns a list of messages. */
std::vector<std::string> Apply(Game& g, Side& side);
/** Must `a` be asked before it attacks `b`'s tile -- "Thou art attacking
 *  without first having declared war!" (attack_tile, 67cc:00b2)? Yes at
 *  peace, and when uneasy if the tile is a city. */
bool MustDeclare(const Game& g, int a, int b, const City* city);
/** "Shall we attack anyway?" -- yes (67cc:20a8): war at once, both ways,
 *  proposals too, and it costs the attacker 1d100+100 diplomatic score from
 *  peace, 1d15+10 from uneasy. History notes the treachery. */
void AttackAnyway(Game& g, int a, int b);
/** At the end of a side's turn, its peace overtures count (484e:1063). */
void ScoreUpdate(Game& g, Side& side);
/** What a peaceful move adds to the diplomatic score (484e:1063); NONE for nothing. */
int AddScore(Game& g, Side& side, int from, int to);
/** Every side's diplomatic title, by side index (diplomatic_rating, 484e:0aed). */
std::map<int, std::string> Ratings(const Game& g);

// ------------------------------------------------ Upgraded Diplomacy & Parley

/** Are sides `a` and `b` mutually Allied (sharing fog-of-war vision and targets)? */
bool IsAllied(const Game& g, int a, int b);
void SetAllied(Game& g, int a, int b, bool v);

/** Does `a` propose an Alliance with `b`? */
bool ProposalAllied(const Game& g, int a, int b);
void ProposeAlliance(Game& g, int a, int b, bool v);

/** Remaining turns of active peace/alliance truce between `a` and `b`. */
int TruceTurns(const Game& g, int a, int b);
void SetTruce(Game& g, int a, int b, int turns);

/** Gold tribute attached to `a`'s proposal toward `b` (>0 offered, <0 demanded). */
int GoldOffer(const Game& g, int a, int b);
void SetGoldOffer(Game& g, int a, int b, int gold);

/** Joint war target side index (0..7, or NONE) requested by `a` from `b`. */
int WarTarget(const Game& g, int a, int b);
void SetWarTarget(Game& g, int a, int b, int target_side);

/** Relational trust score (-100 to +100) held by `a` toward `b`. */
int Trust(const Game& g, int a, int b);
void AddTrust(Game& g, int a, int b, int delta);

/** Primary target city index broadcast by `side` to its allies (or NONE). */
int AlliedTargetCity(const Game& g, int side);
void SetAlliedTargetCity(Game& g, int side, int city_index);

struct AttitudeFactor {
    std::string label;
    int delta = 0;
};

struct AttitudeBreakdown {
    int from_side = kNone;
    int to_side = kNone;
    std::string personality_name;
    std::string personality_archetype;
    std::string motto;
    int total_score = 0;
    int trust_score = 0;
    int truce_remaining = 0;
    std::vector<AttitudeFactor> factors;
    std::string stance_summary;
    std::string what_it_takes;
    int peace_gold_needed = 0;  // 0 = free, >0 = gold tribute required, NONE = will not accept
    int ally_gold_needed = 0;   // 0 = free, >0 = gold tribute required, NONE = will not accept
};

/**
 * Computes a transparent, human-readable breakdown of why `aiSide` holds its
 * current diplomatic disposition toward `targetSide`, and what deal it will accept.
 */
AttitudeBreakdown ComputeAttitude(const Game& g, int ai_side, int target_side);

}  // namespace w2::diplomacy
