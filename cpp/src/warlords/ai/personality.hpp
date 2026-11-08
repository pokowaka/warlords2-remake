/**
 * @file
 * @brief Personality vectors and archetypes for the 27 computer character cards.
 *
 * Maps each (level, card) pair of CARDS/<name>.DSC (K000-K008, L000-L008,
 * W000-W008) to a 10-dimensional PersonalityVector in [-1, 1]^10 that
 * conditions the neural policy's four heads and shapes the self-play
 * rewards.
 */
#pragma once

#include <array>
#include <string>

#include "warlords/types.hpp"

namespace w2::ai::personality {

constexpr int kTraitDim = 10;  ///< Number of traits in a PersonalityVector.

/** @brief The position of each trait in a PersonalityVector. */
enum TraitIndex : int {
    kAggression = 0,       ///< Eagerness to attack.
    kHonour = 1,           ///< Keeping treaties.
    kAvarice = 2,          ///< Love of gold: sacking and pillaging.
    kScorchedEarth = 3,    ///< Readiness to raze.
    kHeroFocus = 4,        ///< Weight given to heroes, ruins and items.
    kCoalitionDrive = 5,   ///< Readiness to gang up on a leader.
    kCavalryAffinity = 6,  ///< Preference for fast land armies.
    kFlierAffinity = 7,    ///< Preference for flying armies.
    kSiegeAffinity = 8,    ///< Preference for siege and heavy armies.
    kCaution = 9,          ///< Care for its own cities and armies.
};

/** @brief One value in [-1, 1] per trait, indexed by TraitIndex. */
using PersonalityVector = std::array<float, kTraitDim>;

/** @brief A computer character: who it is and how it plays. */
struct Profile {
    int level = 0;               ///< 0 Knight, 1 Lord, 2 Warlord.
    int card = 0;                ///< Card number within the level's deck, 0..8.
    std::string name;            ///< The character's name, as on its card.
    std::string archetype;       ///< A short label for its play style.
    std::string motto;           ///< A line in the character's voice.
    PersonalityVector traits{};  ///< Its personality, by TraitIndex.
};

/**
 * @brief The personality profile of a level and character card.
 *
 * Out-of-range values are clamped.
 * @param level Difficulty tier: 0 Knight, 1 Lord, 2 Warlord.
 * @param card Character card number, 0..8.
 * @return A reference to the static profile.
 */
const Profile& ProfileFor(int level, int card);

/**
 * @brief The personality profile of a side in the game.
 * @param side The side whose level and card pick the profile.
 * @return A reference to the static profile.
 */
const Profile& ForSide(const Side& side);

}  // namespace w2::ai::personality
