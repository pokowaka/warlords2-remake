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

constexpr int PEACE = 0, INTERMEDIATE = 1, WAR = 2;
extern const char* const STATE_NAMES[3];

// STRING.DAT group 106, best first.
extern const char* const TITLES[8];
/** Which titles are used, by how many sides are in play (1-based ranks). */
const std::vector<int>& ratingRanks(int n);

/** War everywhere if Diplomacy is off, peace if it is on (484e:11bd). */
void init(Game& g);
/** The state between two sides. */
int state(const Game& g, int a, int b);
inline bool atWar(const Game& g, int a, int b) {
    return state(g, a, b) != PEACE;
}
/** May `a` attack `b`? A side at peace may not. */
bool mayAttack(const Game& g, int a, int b);
/** Propose a state to another side. */
void propose(Game& g, int a, int b, int st);
/** The state a side proposes to another. */
int proposal(const Game& g, int a, int b);
/** Apply one side's proposals. Returns a list of messages. */
std::vector<std::string> apply(Game& g, Side& side);
/** Must `a` be asked before it attacks `b`'s tile -- "Thou art attacking
 *  without first having declared war!" (attack_tile, 67cc:00b2)? Yes at
 *  peace, and when uneasy if the tile is a city. */
bool mustDeclare(const Game& g, int a, int b, const City* city);
/** "Shall we attack anyway?" -- yes (67cc:20a8): war at once, both ways,
 *  proposals too, and it costs the attacker 1d100+100 diplomatic score from
 *  peace, 1d15+10 from uneasy. History notes the treachery. */
void attackAnyway(Game& g, int a, int b);
/** At the end of a side's turn, its peace overtures count (484e:1063). */
void scoreUpdate(Game& g, Side& side);
/** What a peaceful move adds to the diplomatic score (484e:1063); NONE for nothing. */
int addScore(Game& g, Side& side, int from, int to);
/** Every side's diplomatic title, by side index (diplomatic_rating, 484e:0aed). */
std::map<int, std::string> ratings(const Game& g);

// ------------------------------------------------ Upgraded Diplomacy & Parley

/** Are sides `a` and `b` mutually Allied (sharing fog-of-war vision and targets)? */
bool isAllied(const Game& g, int a, int b);
void setAllied(Game& g, int a, int b, bool v);

/** Does `a` propose an Alliance with `b`? */
bool proposalAllied(const Game& g, int a, int b);
void proposeAlliance(Game& g, int a, int b, bool v);

/** Remaining turns of active peace/alliance truce between `a` and `b`. */
int truceTurns(const Game& g, int a, int b);
void setTruce(Game& g, int a, int b, int turns);

/** Gold tribute attached to `a`'s proposal toward `b` (>0 offered, <0 demanded). */
int goldOffer(const Game& g, int a, int b);
void setGoldOffer(Game& g, int a, int b, int gold);

/** Joint war target side index (0..7, or NONE) requested by `a` from `b`. */
int warTarget(const Game& g, int a, int b);
void setWarTarget(Game& g, int a, int b, int targetSide);

/** Relational trust score (-100 to +100) held by `a` toward `b`. */
int trust(const Game& g, int a, int b);
void addTrust(Game& g, int a, int b, int delta);

/** Primary target city index broadcast by `side` to its allies (or NONE). */
int alliedTargetCity(const Game& g, int side);
void setAlliedTargetCity(Game& g, int side, int cityIndex);

struct AttitudeFactor {
    std::string label;
    int delta = 0;
};

struct AttitudeBreakdown {
    int fromSide = NONE;
    int toSide = NONE;
    std::string personalityName;
    std::string personalityArchetype;
    std::string motto;
    int totalScore = 0;
    int trustScore = 0;
    int truceRemaining = 0;
    std::vector<AttitudeFactor> factors;
    std::string stanceSummary;
    std::string whatItTakes;
    int peaceGoldNeeded = 0;  // 0 = free, >0 = gold tribute required, NONE = will not accept
    int allyGoldNeeded = 0;   // 0 = free, >0 = gold tribute required, NONE = will not accept
};

/**
 * Computes a transparent, human-readable breakdown of why `aiSide` holds its
 * current diplomatic disposition toward `targetSide`, and what deal it will accept.
 */
AttitudeBreakdown computeAttitude(const Game& g, int aiSide, int targetSide);

}  // namespace w2::diplomacy
