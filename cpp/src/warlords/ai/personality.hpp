// Character personality vectors and archetypes for the 27 Warlords II computer cards.
//
// Architectural Role: Maps each (level, card) pair from CARDS/*.DSC (K000-K008,
// L000-L008, W000-W008) to a 10-dimensional normalized PersonalityVector in [-1, 1]^10
// that conditions the 4-head neural policy and shapes multi-objective self-play rewards.
#pragma once

#include <array>
#include <string>

#include "warlords/types.hpp"

namespace w2::ai::personality {

constexpr int TRAIT_DIM = 10;

enum TraitIndex : int {
    AGGRESSION = 0,
    HONOUR = 1,
    AVARICE = 2,
    SCORCHED_EARTH = 3,
    HERO_FOCUS = 4,
    COALITION_DRIVE = 5,
    CAVALRY_AFFINITY = 6,
    FLIER_AFFINITY = 7,
    SIEGE_AFFINITY = 8,
    CAUTION = 9,
};

using PersonalityVector = std::array<float, TRAIT_DIM>;

struct Profile {
    int level = 0;
    int card = 0;
    std::string name;
    std::string archetype;
    std::string motto;
    PersonalityVector traits{};
};

/**
 * Returns the personality profile for a given difficulty level (0=Knight, 1=Lord, 2=Warlord)
 * and character card index (0..8).
 *
 * @param level Difficulty tier in [0, 2].
 * @param card Character card index in [0, 8].
 * @return Reference to the static Profile descriptor.
 */
const Profile& profileFor(int level, int card);

/**
 * Returns the personality profile for a specific side in the game.
 *
 * @param side The side whose level and card determine the profile.
 * @return Reference to the static Profile descriptor.
 */
const Profile& forSide(const Side& side);

}  // namespace w2::ai::personality
