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
#include "warlords/ai/modern_ml_player.hpp"
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

void PlayTurn(Game& game, Side& side, IAiTurnEvents* events, const ml::PolicyNetwork* custom_policy,
              ml::RolloutSampler* sampler) {
    ScopedAiTurnEvents turn_scope(game, events);
    const ml::PolicyNetwork& policy = custom_policy ? *custom_policy : ml::ActivePolicy();
    // Outside training the policy is still sampled, as in training, but at the
    // deployment temperature and from a generator seeded by the game state (never
    // advancing the game's own dice), so play stays reproducible. Taking the single
    // best-scoring option instead would follow a different policy from the one
    // trained: for a spread-out distribution its mode is not its typical behaviour.
    ml::RolloutSampler deploy_sampler;
    if (!sampler) {
        deploy_sampler.temperature = ml::DeployTemperature();
        deploy_sampler.rng_state = (game.GetRng().State() * 2654435761U) ^
                                   static_cast<uint32_t>(game.Turn() * 40503U) ^
                                   static_cast<uint32_t>((side.Index() + 1) * 2246822519U);
        sampler = &deploy_sampler;
    }
    MovedArmySet moved_armies;
    ModernMlPlayer& modern_player = ModernMlPlayer::EnsureOn(side);
    modern_player.ClearThoughts();

    std::array<bool, 8> diplo_decided{};
    for (int wave = 0; wave < 6; wave++) {
        belief::Update(game, side);
        ml::ContextVec wave_ctx = ml::BuildContext(game, side);
        if (wave == 0) {
            ml::CriticVec critic = ml::BuildCriticFeatures(game, side);
            float board_value = policy.EvaluateValue(wave_ctx, critic);
            modern_player.SetLastBoardValue(board_value);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->start_ctx = wave_ctx;
                sampler->current_turn->start_critic = critic;
                sampler->current_turn->value_pred = board_value;
                sampler->current_turn->start_score = ml::ComputeEmpireScore(game, side.Index());
            }
        }
        RunDiplomacyPhase(game, side, policy, wave_ctx, sampler, diplo_decided);
        if (events && events->IsAborted()) return;
        if (wave == 0 && events && (!events->OnPhase("diplomacy") || events->IsAborted())) return;
        RunMilitaryAndExplorationPhase(game, side, policy, wave_ctx, wave == 0 ? events : nullptr,
                                       sampler, moved_armies);
        if (events && events->IsAborted()) return;
        bool any_left = false;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() == side.Index() && army->X() != kNone && !army->InTransit() &&
                !moved_armies.contains(army->Id()) && army->Moves() >= 2) {
                any_left = true;
                break;
            }
        }
        if (!any_left) break;
    }

    // A city emptied by one move but re-garrisoned by a later one was not left open
    if (sampler && sampler->current_turn) {
        const Map& map = game.GetMap();
        for (auto& mission_step : sampler->current_turn->mission_steps) {
            if (mission_step.vacated_city == kNone || mission_step.vacated_city >= map.CityCount())
                continue;
            const City& vacated_city = map.Cities()[mission_step.vacated_city];
            for (int dy = 0; dy <= 1 && mission_step.vacated_city != kNone; dy++) {
                for (int dx = 0; dx <= 1 && mission_step.vacated_city != kNone; dx++) {
                    for (Army* army :
                         game::ArmiesAt(game, vacated_city.X() + dx, vacated_city.Y() + dy)) {
                        if (army->OwnerIndex() == side.Index()) {
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
    modern_player.SetLastBoardValue(policy.EvaluateValue(ctx, ml::BuildCriticFeatures(game, side)));

    RunEconomyPhase(game, side, policy, ctx, sampler);
    if (events && !events->IsAborted()) (void)events->OnPhase("economy");
}

std::vector<MissionSnapshot> SnapshotMissions(Game& game, Side& side,
                                              const ml::PolicyNetwork& policy) {
    std::vector<MissionSnapshot> out;
    MovedArmySet moved_armies;
    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side);
    RunMilitaryAndExplorationPhase(game, side, policy, ctx, nullptr, nullptr, moved_armies, &out);
    return out;
}

}  // namespace w2::ai::modern
