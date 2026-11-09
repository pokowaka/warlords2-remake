/**
 * @file
 * @brief Implementation of `w2::Side`.
 */
#include "warlords/model/side.hpp"

#include <algorithm>
#include <utility>

#include "warlords/model/map.hpp"

namespace w2 {

void Side::SetName(std::string name) {
    name_ = std::move(name);
}

void Side::SetGold(int gold) {
    gold_ = std::max(0, gold);
}

void Side::AddGold(int delta) {
    gold_ = std::max(0, gold_ + delta);
}

bool Side::SpendGold(int amount) {
    if (amount < 0 || gold_ < amount) {
        return false;
    }
    gold_ -= amount;
    return true;
}

City* Side::CapitalCity(Map& map) const {
    return map.CityById(capital_city_);
}

const City* Side::CapitalCity(const Map& map) const {
    return map.CityById(capital_city_);
}

void Side::SetCurrentCapitalPos(TilePos pos) {
    if (pos.IsValid()) {
        current_capital_pos_ = pos;
    } else {
        current_capital_pos_.reset();
    }
}

void Side::SetComputerPlayer(SharedComputerPlayer player) {
    computer_player_ = std::move(player);
}

bool Side::IsClassicAi() const {
    return !computer_player_ || computer_player_->IsClassic();
}

void Side::SetQuest(SharedQuest quest) {
    quest_ = std::move(quest);
}

void Side::AddMissive(DiplomaticMissive missive) {
    missives_.push_back(std::move(missive));
}

DiplomaticMissive Side::PopFrontMissive() {
    DiplomaticMissive front = std::move(missives_.front());
    missives_.erase(missives_.begin());
    return front;
}

int Side::LastMissiveTurn(SideId target) const {
    int idx = target.Value();
    return (idx >= 0 && idx < static_cast<int>(last_missive_turn_.size()))
                   ? last_missive_turn_[static_cast<size_t>(idx)]
                   : 0;
}

void Side::SetLastMissiveTurn(SideId target, int turn) {
    int idx = target.Value();
    if (idx >= 0 && idx < static_cast<int>(last_missive_turn_.size())) {
        last_missive_turn_[static_cast<size_t>(idx)] = turn;
    }
}

}  // namespace w2
