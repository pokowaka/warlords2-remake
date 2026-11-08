/**
 * @file
 * @brief Shared types and phase function declarations for the modern ML-driven AI.
 */
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "warlords/ai/core.hpp"
#include "warlords/ai/modern_ai.hpp"

namespace w2::ai::modern {

/** @brief Category of mission objective scored by MissionNet. */
enum class TargetKind : int {
    kHoldGarrison = 0,   ///< Stay in our current city as garrison.
    kNeutralCity = 1,    ///< Capture a neutral city.
    kEnemyCity = 2,      ///< Siege an enemy city.
    kEnemyStack = 3,     ///< Intercept a visible enemy stack in the field.
    kRuinOrSage = 4,     ///< Search an open ruin, library, or sage site.
    kTempleOrItem = 5,   ///< Visit a temple for blessing or pick up a ground item.
    kFogFrontier = 6,    ///< Explore unseen tiles along the fog frontier.
    kReinforceCity = 7,  ///< March to reinforce another owned city.
};

/** @brief A candidate mission target considered during the military and exploration phase. */
struct CandidateTarget {
    TargetKind kind = TargetKind::kHoldGarrison;  ///< Objective category.
    int x = 0, y = 0;                             ///< Target tile coordinates.
    City* city = nullptr;                         ///< Target city, if any.
    Site* site = nullptr;                         ///< Target site, if any.
    Item* item = nullptr;                         ///< Target ground item, if any.
    int owner = kNone;                            ///< Owning side index, or kNone.
    float value = 0.0F;                           ///< Base objective value.
    bool is_quest_or_own_cap = false;             ///< Whether this is our quest target or capital.
    bool is_enemy_capital = false;                ///< Whether this is an enemy side's capital.
    bool is_water = false;                        ///< Whether the target tile is on water.
    float enemy_threat = 0.0F;         ///< Nearby hostile strength threatening the target.
    float nearby_friendly_str = 0.0F;  ///< Friendly mobile strength within 14 tiles.
    int assigned_stacks = 0;           ///< Stacks already assigned to this target.
    float assigned_strength = 0.0F;    ///< Combined strength already assigned here.
    std::string label;                 ///< Human-readable description for AIThoughts.
};

/** @brief Returns whether `our_side` may choose an attack on `owner` (at `city`, or a field stack).
 */
bool Attackable(const Game& game, int our_side, int owner, const City* city);

/** @brief Builds the 16-dimensional feature vector describing `stack` for MissionNet. */
ml::StackVec BuildStackFeatures(const Game& game, const std::vector<Army*>& stack,
                                int prev_min_move, float origin_rem_guard_str = 10.0F,
                                float origin_threat = 0.0F);

/** @brief Returns the minimum movement-point cost in `flood` to reach `tgt`. */
int TargetPathCost(const Game& game, const core::Flood& flood, const CandidateTarget& tgt);

/** @brief Builds the 21-dimensional feature vector describing `tgt` for `stack` in MissionNet. */
ml::TargetVec BuildTargetFeatures(Game& game, const Side& side, const std::vector<Army*>& stack,
                                  const CandidateTarget& tgt, float nearby_friendly_str,
                                  float caution_bias, int path_cost);

/** @brief Evaluates diplomatic proposals and turn-start missives using DiploNet. */
void RunDiplomacyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       std::array<bool, 8>& diplo_decided);

/** @brief Uses EconNet to choose whether to occupy, pillage, sack, or raze a newly captured `city`.
 */
void DecideCaptureSpoils(Game& game, Side& side, City& city, const std::vector<Army*>& attackers,
                         const ml::PolicyNetwork& policy, const ml::ContextVec& ctx,
                         ml::RolloutSampler* sampler);

/** @brief Moves `stack` along its path toward `tgt`, fighting blockers or the target. */
bool ExecuteStackMission(Game& game, Side& side, std::vector<Army*>& stack, CandidateTarget& tgt,
                         bool stage_near, const ml::PolicyNetwork& policy,
                         const ml::ContextVec& ctx, ml::RolloutSampler* sampler);

/** @brief Returns the distinct candidate stack sizes to consider for a group led by `seed`. */
std::vector<size_t> BuildCoalitionSizes(const Game& game, Army* seed,
                                        const std::vector<Army*>& escorts);

/** @brief Forms army coalitions, scores mission options with MissionNet, and executes moves. */
void RunMilitaryAndExplorationPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                                    const ml::ContextVec& ctx, const PauseCallback& pause,
                                    ml::RolloutSampler* sampler,
                                    std::vector<MissionSnapshot>* snapshots = nullptr);

/** @brief Uses EconNet to decide unit retention, empire-wide slot purchases, production, and
 * vectoring. */
void RunEconomyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                     const ml::ContextVec& ctx, ml::RolloutSampler* sampler);

}  // namespace w2::ai::modern
