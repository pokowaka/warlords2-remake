/**
 * @file
 * @brief Implementation of `ModernMlPlayer`.
 */
#include "warlords/ai/modern_ml_player.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

#include "absl/base/no_destructor.h"

#include "warlords/ai.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"

namespace w2 {

namespace {

using Json = nlohmann::ordered_json;

template <typename T, size_t N>
Json IntArray(const std::array<T, N>& arr) {
    Json out = Json::array();
    for (T val : arr) out.push_back(static_cast<int>(val));
    return out;
}

template <typename T, size_t N>
void ReadIntArray(const Json& json_arr, std::array<T, N>& arr) {
    if (!json_arr.is_array()) return;
    for (size_t i = 0; i < N && i < json_arr.size(); i++) {
        if (json_arr[i].is_number()) arr[i] = static_cast<T>(json_arr[i].get<int>());
    }
}

}  // namespace

ModernMlPlayer::ModernMlPlayer(const ai::ml::PolicyNetwork* custom_policy,
                               ai::ml::RolloutSampler* sampler)
        : custom_policy_(custom_policy), sampler_(sampler) {}

void IModernBackedPlayer::RecordCityStolen(SideId attacker, int turn) {
    ModernImpl().RecordCityStolen(attacker, turn);
}

void IModernBackedPlayer::RecordCityRazed(SideId razer, int turn) {
    ModernImpl().RecordCityRazed(razer, turn);
}

void IModernBackedPlayer::RecordBetrayal(SideId betrayer, bool broke_alliance, int turn) {
    ModernImpl().RecordBetrayal(betrayer, broke_alliance, turn);
}

void IModernBackedPlayer::RecordTributeReceived(SideId payer, int gold) {
    ModernImpl().RecordTributeReceived(payer, gold);
}

void ModernMlPlayer::PlayTurn(Game& game, Side& side, IAiTurnEvents* events) {
    ai::modern::PlayTurn(game, side, events, custom_policy_, sampler_);
}

AttitudeBreakdown ModernMlPlayer::EvaluateProposal(const Game& game, SideId ai_side,
                                                   SideId proposer_side) const {
    return ai::EvaluateSideProposal(game, ai_side, proposer_side, &belief_, &grudge_);
}

City* ModernMlPlayer::ChooseHeroCity(Game& game, Side& side, City* fallback) {
    return ai::modern::SelectHeroCity(game, side, fallback, custom_policy_);
}

bool ModernMlPlayer::OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) {
    auto side_quest = side.QuestPtr();
    if (game.GetMap().GameOptions().quests == 0 || !side_quest ||
        side_quest->TargetCityId() != city.Id()) {
        return false;
    }
    if (side_quest->Type() != quest::kOccupy && side_quest->Type() != quest::kRaze) return false;
    bool hero_in_stack = side_quest->HeroId().IsValid() &&
                         std::any_of(stack.begin(), stack.end(), [&side_quest](const Army* member) {
                             return member && member->Id() == side_quest->HeroId();
                         });
    if (!hero_in_stack) return false;
    if (side_quest->Type() == quest::kRaze) {
        if (ai::groups::SackValue(game, city) >= 400) {
            ai::groups::Sack(game, side, city);
        } else {
            game::Raze(game, side, city, stack);
            if (IAiTurnEvents* events = game.ActiveAiEvents()) {
                (void)events->OnSpoils(game, side, city, "razed");
            }
        }
        return true;
    }
    return false;
}

void ModernMlPlayer::RecordBattle(Game& game, SideId, SideId attacker, TilePos, int heroes_lost,
                                  int armies_lost, bool all_lost, bool city_tile) {
    int atk = attacker.Value();
    if (atk < 0 || atk >= 8) return;
    grudge_.heroes_killed[atk] += heroes_lost;
    armies_killed_[atk] += armies_lost;
    battles_[atk]++;
    if (all_lost) grudge_.lost[atk]++;
    if (city_tile) {
        city_battles_[atk]++;
        if (all_lost) grudge_.cities_lost[atk]++;
    }
    if (heroes_lost > 0 || (city_tile && all_lost)) {
        grudge_.last_grudge_turn[atk] = game.Turn();
    }
}

void ModernMlPlayer::RecordDefenseOutcome(const Game& game, SideId defender, const Battle& battle) {
    if (pending_off_turn_ == nullptr || battle.lines.defenders.empty()) return;
    const int def_idx = defender.Value();
    if (def_idx < 0 || def_idx >= 8) return;

    float post_my_str = 0.0F;
    float post_all_str = 0.0F;
    for (const Army* army : game.Armies()) {
        if (army->IsDisbanded() || (army->X() == kNone && !army->InTransit()) ||
            army->OwnerIndex() < 0 || army->OwnerIndex() >= 8) {
            continue;
        }
        float power = static_cast<float>(army->Strength() * army->Strength());
        post_all_str += power;
        if (army->OwnerIndex() == def_idx) post_my_str += power;
    }
    float dead_my_str = 0.0F;
    float dead_all_str = 0.0F;
    for (const Army* dead_atk : battle.dead_attackers) {
        if (dead_atk->OwnerIndex() >= 0 && dead_atk->OwnerIndex() < 8) {
            float power = static_cast<float>(dead_atk->Strength() * dead_atk->Strength());
            dead_all_str += power;
            if (dead_atk->OwnerIndex() == def_idx) dead_my_str += power;
        }
    }
    for (const Army* dead_def : battle.dead_defenders) {
        if (dead_def->OwnerIndex() >= 0 && dead_def->OwnerIndex() < 8) {
            float power = static_cast<float>(dead_def->Strength() * dead_def->Strength());
            dead_all_str += power;
            if (dead_def->OwnerIndex() == def_idx) dead_my_str += power;
        }
    }
    const float pre_my_str = post_my_str + dead_my_str;
    const float pre_all_str = post_all_str + dead_all_str;
    const float post_share = post_all_str > 0.0F ? (post_my_str / post_all_str) : 0.0F;
    const float pre_share = pre_all_str > 0.0F ? (pre_my_str / pre_all_str) : 0.0F;
    const float delta_army = ai::ml::ArmyScoreWeight() * (post_share - pre_share);

    float delta_city = 0.0F;
    const City* defended_city = battle.lines.city;
    if (defended_city != nullptr && !defended_city->IsRazed() &&
        defended_city->OwnerIndex() == def_idx) {
        int standing = 0;
        int total_income = 0;
        for (const auto& city : game.GetMap().Cities()) {
            if (city.IsRazed()) continue;
            standing++;
            total_income += std::max(1, city.Income());
        }
        const float city_weight =
                2.0F / static_cast<float>(std::max(1, standing)) +
                0.5F * static_cast<float>(std::max(1, defended_city->Income())) /
                        static_cast<float>(std::max(1, total_income));
        delta_city = battle.won ? -city_weight : (off_turn_repel_weight_ * city_weight);
    }

    const float per_defender =
            (delta_army + delta_city) / static_cast<float>(battle.lines.defenders.size());
    for (const Army* def_army : battle.lines.defenders) {
        pending_off_turn_->off_turn_unit_deltas.push_back(ai::ml::OffTurnUnitDelta{
            reinterpret_cast<std::uintptr_t>(def_army), per_defender});
    }
}

void ModernMlPlayer::RecordBattleObservation(Game& game, SideId observer, TilePos pos) {
    ai::belief::RecordBattleObservation(game, observer.Value(), pos.x, pos.y);
}

void ModernMlPlayer::RecordCityStolen(SideId attacker, int turn) {
    int atk = attacker.Value();
    if (atk < 0 || atk >= 8) return;
    grudge_.cities_stolen[atk]++;
    grudge_.last_grudge_turn[atk] = turn;
}

void ModernMlPlayer::RecordCityRazed(SideId razer, int turn) {
    int atk = razer.Value();
    if (atk < 0 || atk >= 8) return;
    grudge_.cities_razed_by[atk]++;
    grudge_.last_grudge_turn[atk] = turn;
}

void ModernMlPlayer::RecordBetrayal(SideId betrayer, bool broke_alliance, int turn) {
    int atk = betrayer.Value();
    if (atk < 0 || atk >= 8) return;
    grudge_.truces_broken_by[atk] += broke_alliance ? 2 : 1;
    grudge_.last_grudge_turn[atk] = turn;
}

void ModernMlPlayer::RecordTributeReceived(SideId payer, int gold) {
    int pay_idx = payer.Value();
    if (pay_idx < 0 || pay_idx >= 8 || gold <= 0) return;
    grudge_.tribute_received[pay_idx] += gold;
}

nlohmann::ordered_json ModernMlPlayer::SaveState() const {
    Json obj = Json::object();
    obj["kind"] = "modern";
    obj["lastBoardValue"] = last_board_value_;
    obj["heroesKilled"] = IntArray(grudge_.heroes_killed);
    obj["armiesKilled"] = IntArray(armies_killed_);
    obj["battles"] = IntArray(battles_);
    obj["lost"] = IntArray(grudge_.lost);
    obj["cityBattles"] = IntArray(city_battles_);
    obj["citiesLost"] = IntArray(grudge_.cities_lost);
    obj["citiesStolen"] = IntArray(grudge_.cities_stolen);
    obj["citiesRazedBy"] = IntArray(grudge_.cities_razed_by);
    obj["trucesBrokenBy"] = IntArray(grudge_.truces_broken_by);
    obj["tributeReceived"] = IntArray(grudge_.tribute_received);
    obj["lastGrudgeTurn"] = IntArray(grudge_.last_grudge_turn);
    return obj;
}

absl::Status ModernMlPlayer::LoadState(const nlohmann::ordered_json& state, const Game&) {
    if (!state.is_object()) return absl::OkStatus();
    auto field = [&](std::string_view key) -> const Json& {
        static const absl::NoDestructor<Json> kNull(nullptr);
        auto found = state.find(key);
        return found == state.end() ? *kNull : *found;
    };
    const Json& val = field("lastBoardValue");
    if (val.is_number()) last_board_value_ = val.get<float>();
    ReadIntArray(field("heroesKilled"), grudge_.heroes_killed);
    ReadIntArray(field("armiesKilled"), armies_killed_);
    ReadIntArray(field("battles"), battles_);
    ReadIntArray(field("lost"), grudge_.lost);
    ReadIntArray(field("cityBattles"), city_battles_);
    ReadIntArray(field("citiesLost"), grudge_.cities_lost);
    ReadIntArray(field("citiesStolen"), grudge_.cities_stolen);
    ReadIntArray(field("citiesRazedBy"), grudge_.cities_razed_by);
    ReadIntArray(field("trucesBrokenBy"), grudge_.truces_broken_by);
    ReadIntArray(field("tributeReceived"), grudge_.tribute_received);
    ReadIntArray(field("lastGrudgeTurn"), grudge_.last_grudge_turn);
    return absl::OkStatus();
}

const ModernMlPlayer* ModernMlPlayer::FindOn(const Side& side) {
    const auto* backed = dynamic_cast<const IModernBackedPlayer*>(side.ComputerPlayer());
    return backed ? &backed->ModernImpl() : nullptr;
}

ModernMlPlayer* ModernMlPlayer::FindMutableOn(Side& side) {
    auto* backed = dynamic_cast<IModernBackedPlayer*>(side.ComputerPlayer());
    return backed ? &backed->ModernImpl() : nullptr;
}

ModernMlPlayer& ModernMlPlayer::EnsureOn(Side& side) {
    if (ModernMlPlayer* existing = FindMutableOn(side)) return *existing;
    auto created = std::make_shared<ModernMlPlayer>();
    ModernMlPlayer& ref = *created;
    side.SetComputerPlayer(std::move(created));
    return ref;
}

}  // namespace w2
