/**
 * @file
 * @brief Modern AI turn orchestrator (PlayTurn) and mission snapshot inspector.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"

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

namespace {

class MissionFightStore {
  public:
    void Set(MissionFightCallback callback) ABSL_LOCKS_EXCLUDED(mutex_) {
        absl::MutexLock lock(&mutex_);
        const bool has_cb = static_cast<bool>(callback);
        callback_ = std::move(callback);
        active_.store(has_cb, std::memory_order_release);
    }

    void Notify(Game& game, const MissionFight& fight, const Battle& battle) const
            ABSL_LOCKS_EXCLUDED(mutex_) {
        if (!active_.load(std::memory_order_acquire)) return;
        MissionFightCallback cb_copy;
        {
            absl::MutexLock lock(&mutex_);
            cb_copy = callback_;
        }
        if (cb_copy) cb_copy(game, fight, battle);
    }

  private:
    std::atomic<bool> active_{false};
    mutable absl::Mutex mutex_;
    MissionFightCallback callback_ ABSL_GUARDED_BY(mutex_);
};

MissionFightStore& GlobalMissionFightStore() {
    static absl::NoDestructor<MissionFightStore> store;
    return *store;
}

void RestrictToFairView(Game& game, int our_side) {
    std::array<std::vector<Army*>, 8> unseen;
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() < 0 || army->OwnerIndex() >= 8 || army->OwnerIndex() == our_side ||
            army->IsDisbanded()) {
            continue;
        }
        bool hidden = army->InTransit() || army->X() == kNone;
        if (!hidden) {
            if (const City* city = game::CityAt(game, army->X(), army->Y())) {
                hidden = !belief::IsCityVisible(game, our_side, *city);
            } else {
                hidden = !belief::IsTileVisible(game, our_side, army->X(), army->Y());
            }
        }
        if (hidden) unseen[army->OwnerIndex()].push_back(army);
    }
    Map& map = game.GetMap();
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        if (!unseen[side_idx].empty()) game::Disband(game, map.Sides()[side_idx], unseen[side_idx]);
    }
}

bool ShouldSwitchToImperialLookahead(const Game& game, const Side& side,
                                     const ml::PolicyNetwork& card_policy,
                                     const ml::PolicyNetwork& imperial_policy,
                                     const ml::PolicyNetwork& reply_base_policy) {
    if (game.Turn() < 5 || game.DataDir().empty()) return false;
    std::string snap = EncodeDefaultGameSnapshot(game);
    if (snap.empty()) return false;
    constexpr int kRollouts = 2;
    float diff_sum = 0.0F;
    float diff_sq_sum = 0.0F;
    for (int rollout = 0; rollout < kRollouts; rollout++) {
        std::array<float, 2> scores = {0.0F, 0.0F};
        for (int cand_idx = 0; cand_idx < 2; cand_idx++) {
            std::unique_ptr<Game> copy = DecodeDefaultGameSnapshot(snap, game.DataDir());
            if (!copy) return false;
            RestrictToFairView(*copy, side.Index());
            // Fresh deterministic rollout dice disjoint from `game.GetRng().State()`
            // (zero combat-dice peeking).
            uint32_t seed_hash = static_cast<uint32_t>(game.Turn() * 2654435761U) ^
                                 static_cast<uint32_t>((side.Index() + 1) * 2246822519U) ^
                                 static_cast<uint32_t>((rollout + 1) * 3266489917U);
            copy->SetRng(Rng(static_cast<double>(1U + (seed_hash & 0x7fffffffU))));
            const ml::PolicyNetwork& cand = (cand_idx == 0) ? card_policy : imperial_policy;
            PlayTurn(*copy, *copy->GetMap().SideAt(side.Index()), nullptr, &cand, nullptr);
            const int stop_turn = copy->Turn() + 1;
            Side* cur = game::EndTurn(*copy);
            int safety = 0;
            while (cur && !copy->IsOver() && !copy->IsWon() && copy->Turn() < stop_turn &&
                   safety++ < 16) {
                if (cur->IsComputerControlled() && cur->IsAlive()) {
                    if (cur->IsClassicAi()) {
                        ai::PlayTurn(*copy, *cur, nullptr);
                    } else {
                        ml::PolicyNetwork opp_pol = reply_base_policy.MaterializeForSide(*cur);
                        opp_pol.SetWarlordLookahead(false);
                        PlayTurn(*copy, *cur, nullptr, &opp_pol, nullptr);
                    }
                }
                cur = game::EndTurn(*copy);
            }
            scores[cand_idx] = ml::ComputeEmpireScore(*copy, side.Index());
        }
        float delta = scores[1] - scores[0];
        diff_sum += delta;
        diff_sq_sum += delta * delta;
    }
    float mean = diff_sum / static_cast<float>(kRollouts);
    float var = std::max(0.0F, diff_sq_sum / static_cast<float>(kRollouts) - mean * mean);
    float std_err = std::sqrt(var / static_cast<float>(kRollouts)) + 1e-4F;
    return mean > 1.5F * std_err && mean > 0.02F;
}

}  // namespace

void SetMissionFightCallback(MissionFightCallback callback) {
    GlobalMissionFightStore().Set(std::move(callback));
}

void NotifyMissionFight(Game& game, const MissionFight& fight, const Battle& battle) {
    GlobalMissionFightStore().Notify(game, fight, battle);
}

void PlayTurn(Game& game, Side& side, IAiTurnEvents* events, const ml::PolicyNetwork* custom_policy,
              ml::RolloutSampler* sampler) {
    ScopedAiTurnEvents turn_scope(game, events);
    const ml::PolicyNetwork& base_policy = custom_policy ? *custom_policy : ml::ActivePolicy();
    const bool caller_provided_sampler = (sampler != nullptr);
    const bool apply_persona = base_policy.UsePersonalities() && base_policy.HasArchetypes() &&
                               !caller_provided_sampler;
    ml::PolicyNetwork materialized;
    bool lookahead_switched = false;
    if (apply_persona) {
        materialized = base_policy.MaterializeForSide(side);
        materialized.SetWarlordLookahead(false);
        if (base_policy.WarlordLookahead() && side.Level() >= 2 && side.Card() != 0) {
            personality::ArchetypeWeightArray imp_w{1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
            ml::PolicyNetwork imperial_pol = base_policy.MaterializeForWeights(imp_w);
            imperial_pol.SetWarlordLookahead(false);
            if (ShouldSwitchToImperialLookahead(game, side, materialized, imperial_pol,
                                                base_policy)) {
                materialized = std::move(imperial_pol);
                materialized.SetPersonaActive(false);
                lookahead_switched = true;
            }
        }
    }
    const ml::PolicyNetwork& policy = apply_persona ? materialized : base_policy;

    // Outside training the policy is still sampled, as in training, but at the
    // deployment temperature (scaled by difficulty tier when personalities are active)
    // and from a generator seeded by the game state (never advancing the game's own dice).
    ml::RolloutSampler deploy_sampler;
    if (!sampler) {
        float base_temp = ml::DeployTemperature();
        deploy_sampler.temperature =
                base_policy.UsePersonalities()
                        ? personality::DifficultyTemperature(side.Level(), base_temp)
                        : base_temp;
        deploy_sampler.rng_state = (game.GetRng().State() * 2654435761U) ^
                                   static_cast<uint32_t>(game.Turn() * 40503U) ^
                                   static_cast<uint32_t>((side.Index() + 1) * 2246822519U);
        sampler = &deploy_sampler;
    }
    MovedArmySet moved_armies;
    const Map& map = game.GetMap();
    for (Army* army : game.Armies()) {
        if (army->OwnerIndex() != side.Index()) continue;
        if (policy.Version() >= 7 && army->AiOrder() >= 100 && army->AiDest() >= 0 &&
            army->AiDest() < map.CityCount()) {
            const City& dest_city = map.Cities()[army->AiDest()];
            int base_kind = (army->AiOrder() - 100) % 20;
            bool want_enemy = (base_kind == static_cast<int>(TargetKind::kNeutralCity) ||
                               base_kind == static_cast<int>(TargetKind::kEnemyCity));
            if (dest_city.IsRazed() || (want_enemy && dest_city.OwnerIndex() == side.Index())) {
                army->ClearAiOrder();
            }
        }
    }
    ModernMlPlayer& modern_player = ModernMlPlayer::EnsureOn(side);
    modern_player.ClearThoughts();
    if (apply_persona && ml::RecordThoughts()) {
        const auto& prof = personality::ForSide(side);
        auto weights = personality::ArchetypeWeights(prof);
        std::string mix;
        for (int arch_idx = 0; arch_idx < personality::kNumArchetypes; arch_idx++) {
            if (weights[arch_idx] < 0.08F) continue;
            if (!mix.empty()) mix += " + ";
            mix += absl::StrFormat("%s %.0f%%", personality::ArchetypeName(arch_idx),
                                   100.0F * weights[arch_idx]);
        }
        if (lookahead_switched) mix += " [Lookahead->Imperial]";
        TilePos cap_pos = side.CapitalPosition();
        modern_player.PushThought(AIThought{
            "Persona", prof.name, prof.archetype, cap_pos.x, cap_pos.y, cap_pos.x, cap_pos.y, 1.0F,
            1.0F, false, absl::StrFormat("%s (T=%.2f)", mix, sampler->temperature)});
    }

    std::array<bool, 8> diplo_decided{};
    for (int wave = 0; wave < 6; wave++) {
        belief::Update(game, side);
        ml::ContextVec wave_ctx = ml::BuildContext(game, side, &policy);
        if (wave == 0) {
            ml::CriticVec critic = ml::BuildCriticFeatures(game, side);
            float board_value = policy.EvaluateValue(wave_ctx, critic);
            modern_player.SetLastBoardValue(board_value);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->start_ctx = wave_ctx;
                sampler->current_turn->start_critic = critic;
                sampler->current_turn->value_pred = board_value;
                sampler->current_turn->start_score = ml::ComputeEmpireScore(game, side.Index());
                for (const auto& prod_ev : game.TurnNews(side.Id()).produced) {
                    if ((prod_ev.kind == ProducedKind::kBuilt ||
                         prod_ev.kind == ProducedKind::kSent) &&
                        prod_ev.unit_id != 0 && prod_ev.city.IsValid()) {
                        sampler->current_turn->produced_units.push_back(ml::ProducedUnitRecord{
                            prod_ev.unit_id, prod_ev.city.Value(), prod_ev.type,
                            prod_ev.kind == ProducedKind::kSent});
                    }
                }
            }
        }
        RunDiplomacyPhase(game, side, policy, wave_ctx, sampler, diplo_decided);
        if (events && events->IsAborted()) return;
        if (wave == 0 && events && (!events->OnPhase("diplomacy") || events->IsAborted())) return;
        if (wave == 0) RunMusterSelectionPhase(game, side, policy, wave_ctx, sampler);
        if (wave == 0) RunCityGuardPhase(game, side, policy, wave_ctx, sampler, moved_armies);
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
        for (auto& mission_step : sampler->current_turn->mission_steps) {
            if (mission_step.vacated_city == kNone || mission_step.vacated_city >= map.CityCount())
                continue;
            const City& vacated_city = map.Cities()[mission_step.vacated_city];
            for (int dy = 0; dy <= 1 && mission_step.vacated_city != kNone; dy++) {
                for (int dx = 0; dx <= 1 && mission_step.vacated_city != kNone; dx++) {
                    for (Army* army :
                         game::ArmiesOnTileSpan(game, vacated_city.X() + dx,
                                                vacated_city.Y() + dy)) {
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
    ml::ContextVec ctx = ml::BuildContext(game, side, &policy);
    modern_player.SetLastBoardValue(policy.EvaluateValue(ctx, ml::BuildCriticFeatures(game, side)));

    RunStandardPhase(game, side, policy, ctx, sampler);
    RunEconomyPhase(game, side, policy, ctx, sampler);
    if (events && !events->IsAborted()) (void)events->OnPhase("economy");
}

std::vector<MissionSnapshot> SnapshotMissions(Game& game, Side& side,
                                              const ml::PolicyNetwork& base_policy) {
    ml::PolicyNetwork materialized;
    const ml::PolicyNetwork& policy =
            (base_policy.UsePersonalities() && base_policy.HasArchetypes())
                    ? (materialized = base_policy.MaterializeForSide(side))
                    : base_policy;
    std::vector<MissionSnapshot> out;
    MovedArmySet moved_armies;
    belief::Update(game, side);
    ml::ContextVec ctx = ml::BuildContext(game, side, &policy);
    RunMilitaryAndExplorationPhase(game, side, policy, ctx, nullptr, nullptr, moved_armies, &out);
    return out;
}

}  // namespace w2::ai::modern
