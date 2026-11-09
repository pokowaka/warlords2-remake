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

void ModernMlPlayer::PlayTurn(Game& game, Side& side, const PauseCallback& pause) {
    ai::modern::PlayTurn(game, side, pause, custom_policy_, sampler_);
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
    if (game.map->options.quests == 0 || !side_quest || side_quest->TargetCityId() != city.Id()) {
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
            if (ai::hooks.on_spoils) ai::hooks.on_spoils(game, side, city, "razed");
        }
        return true;
    }
    return false;
}

void ModernMlPlayer::RecordBattle(Game&, SideId, SideId attacker, TilePos, int heroes_lost,
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
}

void ModernMlPlayer::RecordBattleObservation(Game& game, SideId observer, TilePos pos) {
    ai::belief::RecordBattleObservation(game, observer.Value(), pos.x, pos.y);
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
