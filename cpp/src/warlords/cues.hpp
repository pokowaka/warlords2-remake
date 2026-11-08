/**
 * @file
 * @brief What the game plays, and when.
 *
 * The choices behind the music and the advisor's voice, as the original makes
 * them (docs/re/sound.md).
 *
 * Headless. The file names come from DATA/FILE.DAT (uidata::Strings). A game
 * should not play out differently because the music is on, so the caller
 * passes a roll of its own: roll(n) -> 1..n.
 */
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "warlords/types.hpp"
#include "warlords/uidata.hpp"

namespace w2::cues {

/// A die of the caller's: roll(n) returns 1..n.
using Roll = std::function<int(int)>;

/// The music cues: what the original's music selector is asked for.
constexpr int kTitle = 0, kPlay = 1, kComputer = 2, kTriumph = 3, kHero = 4, kTemple = 5;
constexpr int kSage = 6, kPromotion = 7, kMedal = 8, kSurrender = 9, kDefiance = 10, kBegin = 11;

/** @brief The song for a cue: {FILE.DAT name, loops}; "" for none. */
std::pair<std::string, bool> Song(const uidata::Strings& files, int cue,
                                  const std::vector<Side*>& sides, const Roll& roll);

/// The advisor's clips are FILE.DAT groups 40-62, each with its subtitle 30
/// groups on -- or, for the four set phrases, at index 1 of its own.
constexpr int kGreet = 59, kMoment = 60, kBeginWar = 61, kQuit = 62;

/**
 * @brief What the advisor says as `side`'s turn opens: a FILE.DAT group, or kNone.
 *
 * Updates side.advisor as the original does.
 */
int AdvisorCue(const Game& game, Side& side, const Roll& roll);
/** @brief The clip's sample and subtitle names for an advisor group. */
std::pair<std::string, std::string> Clip(const uidata::Strings& files, int group, const Roll& roll);

}  // namespace w2::cues
