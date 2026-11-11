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
#include <vector>

#include "warlords/types.hpp"

namespace w2::ai::personality {

constexpr int kTraitDim = 10;      ///< Number of traits in a PersonalityVector.
constexpr int kNumArchetypes = 5;  ///< Number of evolved specialist archetypes.

/** @brief Identifier for each evolved archetype specialist (`0..4`). */
enum ArchetypeId : int {
    kArchImperial = 0,   ///< Balanced Hegemon (`damped_v7_soup` generalist anchor).
    kArchBerserker = 1,  ///< Blitzkrieg Raider & Scorched-Earth Conqueror.
    kArchGuardian = 2,   ///< Chivalrous Defender, Garrison Turtle & Builder.
    kArchSchemer = 3,    ///< Machiavellian Coalitionist, Extortionist & Flanker.
    kArchQuestor = 4,    ///< Hero Champion, Ruin Hunter, Banner-Lord & Heavy Siege.
};

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
/** @briefConvex combination weights across the 5 evolved specialist archetypes. */
using ArchetypeWeightArray = std::array<float, kNumArchetypes>;

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

/** @brief Short human-readable label for each evolved archetype specialist (`0..4`). */
const char* ArchetypeName(int arch_id);

/**
 * @brief Projects a character card's 10D trait vector smoothly onto the 5-simplex of evolved
 * specialist brains (`Imperial`, `Berserker`, `Guardian`, `Schemer`, `Questor`).
 */
ArchetypeWeightArray ArchetypeWeights(const Profile& profile);

/**
 * @brief Maps a side's difficulty tier (`0` = Knight, `1` = Lord, `2` = Warlord) to a policy
 * sampling temperature without hardcoded tactical handicaps.
 */
float DifficultyTemperature(int level, float warlord_temp);

/**
 * @brief Returns the bounded character-doctrine scaling factor for `level` (`Knight > Lord >
 * Warlord`), calibrated against `DifficultyTemperature` so Warlords express distinct personas
 * without overriding tactical soundness.
 */
float DoctrineScale(int level);

/** @brief Input signals for trait-conditioned mission utility modulation. */
struct MissionDoctrineContext {
    float vendetta_score = 0.0F;      ///< Blood-feud intensity against target owner in `[0, 1]`.
    bool is_enemy_target = false;     ///< True for enemy cities and enemy field stacks.
    bool is_enemy_capital = false;    ///< True when targeting an enemy faction's home capital.
    bool is_hold_garrison = false;    ///< True when holding garrison in an owned city.
    bool leaves_city_sparse = false;  ///< True when departing leaves `rem_guard < 5` strength.
    bool is_ruin_or_temple = false;   ///< True when targeting an open ruin, sage, temple, or item.
    bool stack_has_hero = false;      ///< True when the candidate coalition includes a Hero.
    float win_converged = 1.0F;       ///< Converged win probability in `[0, 1]`.
};

/** @brief Computes a bounded logit utility adjustment for a mission candidate under `profile`. */
float ScoreMissionCharacterPrior(const Profile& profile, const MissionDoctrineContext& doc_ctx);

/** @brief Input signals for trait-conditioned diplomacy proposal modulation. */
struct DiplomacyDoctrineContext {
    bool cur_allied = false;           ///< Currently in a sworn alliance with this rival.
    bool cur_at_peace = false;         ///< Currently at peace (`State == kPeace`).
    bool cur_at_war = false;           ///< Currently at war (`State == kWar`).
    int truce_turns = 0;               ///< Remaining turns on active truce.
    int truces_broken_by_rival = 0;    ///< Betrayal count recorded against this rival.
    int cities_razed_by_rival = 0;     ///< Our cities razed by this rival.
    float vendetta_score = 0.0F;       ///< Blood-feud intensity in `[0, 1]`.
    float border_advantage = 0.0F;     ///< `tanh((my_str - their_str) / 20)` in `[-1, 1]`.
    bool rival_is_runaway = false;     ///< Rival is the `#1` leader with `>= 28%` city share.
    float runaway_share = 0.0F;        ///< City share of the `#1` runaway leader (`0` if none).
    bool third_party_runaway = false;  ///< Another faction (neither us nor rival) is runaway `#1`.
    int active_wars = 0;               ///< Number of active wars we are already fighting.
};

/**
 * @brief Adjusts diplomatic `stance_logits` (`Allied`, `Peace`, `Uneasy`, `War`), `tribute_logit`,
 * and `joint_war_logit` in-place according to `profile` and relational memory.
 */
void ApplyDiplomacyCharacterPrior(const Profile& profile, const DiplomacyDoctrineContext& doc_ctx,
                                  std::array<float, 4>& stance_logits, float& tribute_logit,
                                  float& joint_war_logit);

/** @brief Unit-archetype attributes evaluated during slot purchase and city production. */
struct UnitDoctrineContext {
    bool is_cavalry = false;         ///< Fast land army (`move >= 14 && !flies`).
    bool is_flier = false;           ///< Flying army (`flies == true`).
    bool is_heavy_or_siege = false;  ///< Heavy monster, siege engine, magical, or `strength >= 7`.
    int strength = 1;                ///< Unit fighting strength (`1..9`).
    int build_time = 1;              ///< Turns required to produce (`1..6`).
};

/** @brief Computes a bounded logit adjustment for purchasing or producing a unit under `profile`. */
float ScoreUnitCharacterPrior(const Profile& profile, const UnitDoctrineContext& unit_ctx);

/** @brief Computes a bounded `HoldGold` logit adjustment when treasury `gold` is below reserve. */
float ScoreHoldGoldCharacterPrior(const Profile& profile, int gold);

/**
 * @brief Adjusts capture-spoils candidate `scores` (`0` = Occupy, `1` = Pillage, `2` = Sack,
 * `3` = Raze) in-place according to `profile`, local threat, city income, and plunder value.
 */
void ApplySpoilsCharacterPrior(const Profile& profile, float local_threat, int city_income,
                               int pillage_gold, int sack_gold, const std::vector<int>& modes,
                               std::vector<float>& scores);

/** @brief Dramatic in-game trigger for an in-character herald or parley dispatch. */
enum class CharacterEvent : int {
    kVendettaWar = 0,        ///< Declaring war over slain Heroes or stolen/razed cities.
    kBetrayalWar = 1,        ///< Breaking peace or alliance for opportunistic conquest.
    kCoalitionAlliance = 2,  ///< Proposing alliance + joint war against a runaway leader.
    kAllianceOffer = 3,      ///< Proposing a formal military alliance in character voice.
    kTributeDemand = 4,      ///< Demanding gold tribute from a weaker neighbor.
    kPeacePlea = 5,          ///< Offering gold or suing for peace when hard-pressed.
    kCityRazed = 6,          ///< Burning a captured city to permanent ruins.
    kCityPillaged = 7,       ///< Pillaging or sacking a captured city for gold.
    kCapitalCaptured = 8,    ///< Storming and seizing an enemy faction's home capital.
};

/** @brief Concrete relational facts interpolated into `FormatCharacterDispatch`. */
struct CharacterDispatchContext {
    std::string actor_faction;   ///< Acting AI faction name.
    std::string target_faction;  ///< Rival or conquered faction name.
    std::string third_faction;   ///< Runaway leader / joint-war target faction name, if any.
    std::string city_name;       ///< Relevant captured or razed city name, if any.
    int gold_amount = 0;         ///< Tribute demanded/offered or gold plundered.
    int cities_lost = 0;         ///< Cities this rival took or razed from us.
    int heroes_slain = 0;        ///< Our Heroes slain by this rival.
};

/**
 * @brief Formats a trait- and card-authentic herald/parley message grounded in the concrete
 * relational facts in `ctx`.
 */
std::string FormatCharacterDispatch(const Profile& profile, CharacterEvent event,
                                    const CharacterDispatchContext& ctx);

}  // namespace w2::ai::personality
