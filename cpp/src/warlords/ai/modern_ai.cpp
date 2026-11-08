/**
 * @file
 * @brief Modern AI turn orchestrator (PlayTurn) and mission snapshot inspector.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_internal.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"

namespace w2::ai::modern {

void PlayTurn(Game& game, Side& side, const PauseCallback& pause,
              const ml::PolicyNetwork* custom_policy, ml::RolloutSampler* sampler) {
    const ml::PolicyNetwork& policy = custom_policy ? *custom_policy : ml::ActivePolicy();
    // Outside training the policy is still sampled, as in training, but at the
    // deployment temperature and from a generator seeded by the game state (never
    // advancing the game's own dice), so play stays reproducible. Taking the single
    // best-scoring option instead would follow a different policy from the one
    // trained: for a spread-out distribution its mode is not its typical behaviour.
    ml::RolloutSampler deploy_sampler;
    if (!sampler) {
        deploy_sampler.temperature = ml::DeployTemperature();
        deploy_sampler.rng_state = (game.rng.state * 2654435761U) ^
                                   static_cast<uint32_t>(game.turn * 40503U) ^
                                   static_cast<uint32_t>((side.index + 1) * 2246822519U);
        sampler = &deploy_sampler;
    }
    for (Army* army : game.armies) {
        if (army->owner == side.index) army->ai_moved = false;
    }
    side.ai_thoughts.clear();

    std::array<bool, 8> diplo_decided{};
    for (int wave = 0; wave < 6; wave++) {
        belief::Update(game, side);
        ml::ContextVec wave_ctx = ml::BuildContext(game, side);
        if (wave == 0) {
            ml::CriticVec critic = ml::BuildCriticFeatures(game, side);
            float board_value = policy.EvaluateValue(wave_ctx, critic);
            side.last_board_value = board_value;
            if (sampler && sampler->current_turn) {
                sampler->current_turn->start_ctx = wave_ctx;
                sampler->current_turn->start_critic = critic;
                sampler->current_turn->value_pred = board_value;
                sampler->current_turn->start_score = ml::ComputeEmpireScore(game, side.index);
            }
        }
        RunDiplomacyPhase(game, side, policy, wave_ctx, sampler, diplo_decided);
        if (wave == 0 && pause) pause("diplomacy");
        RunMilitaryAndExplorationPhase(game, side, policy, wave_ctx, wave == 0 ? pause : nullptr,
                                       sampler);
        bool any_left = false;
        for (Army* army : game.armies) {
            if (army->owner == side.index && army->x != kNone && !army->transit &&
                !army->ai_moved && army->moves >= 2) {
                any_left = true;
                break;
            }
        }
        if (!any_left) break;
    }

    // A city emptied by one move but re-garrisoned by a later one was not left open
    if (sampler && sampler->current_turn) {
        for (auto& mission_step : sampler->current_turn->mission_steps) {
            if (mission_step.vacated_city == kNone ||
                mission_step.vacated_city >= static_cast<int>(game.map->cities.size()))
                continue;
            const City& vacated_city = game.map->cities[mission_step.vacated_city];
            for (int dy = 0; dy <= 1 && mission_step.vacated_city != kNone; dy++) {
                for (int dx = 0; dx <= 1 && mission_step.vacated_city != kNone; dx++) {
                    for (Army* army :
                         game::ArmiesAt(game, vacated_city.x + dx, vacated_city.y + dy)) {
                        if (army->owner == side.index) {
                            mission_step.vacated_city = kNone;
                            break;
                        }
                    }
                }
            }
        }
    }

    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side);
    side.last_board_value = policy.EvaluateValue(ctx, ml::BuildCriticFeatures(game, side));

    RunEconomyPhase(game, side, policy, ctx, sampler);
    if (pause) pause("economy");
}

std::vector<MissionSnapshot> SnapshotMissions(Game& game, Side& side,
                                              const ml::PolicyNetwork& policy) {
    std::vector<MissionSnapshot> out;
    std::vector<bool> moved_before;
    for (Army* army : game.armies) moved_before.push_back(army->ai_moved);
    for (Army* army : game.armies) {
        if (army->owner == side.index) army->ai_moved = false;
    }
    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side);
    RunMilitaryAndExplorationPhase(game, side, policy, ctx, nullptr, nullptr, &out);
    for (size_t i = 0; i < game.armies.size() && i < moved_before.size(); i++)
        game.armies[i]->ai_moved = moved_before[i];
    return out;
}

}  // namespace w2::ai::modern
