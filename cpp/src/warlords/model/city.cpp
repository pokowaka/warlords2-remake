/**
 * @file
 * @brief Implementation of `w2::City`.
 */
#include "warlords/model/city.hpp"

#include <utility>

namespace w2 {

bool City::ContainsTile(int tile_x, int tile_y) const {
    return tile_x >= pos_.x && tile_x <= pos_.x + 1 && tile_y >= pos_.y && tile_y <= pos_.y + 1;
}

void City::Rename(std::string name) {
    name_ = std::move(name);
}

void City::AddProducedType(int army_type) {
    produces_.push_back(army_type);
}

void City::SetProduces(ProduceList produces) {
    produces_ = std::move(produces);
}

void City::SetOwner(SideId owner) {
    owner_ = razed_ ? SideId::None() : owner;
}

void City::SetRazed(bool razed) {
    razed_ = razed;
    if (razed_) {
        owner_ = SideId::None();
    }
}

void City::Raze(SideId razed_by) {
    razed_ = true;
    razed_by_ = razed_by.IsValid() ? razed_by : SideId(0);
    owner_ = SideId::None();
    claim_ = SideId::None();
    slots_.clear();
    producing_ = kNone;
    countdown_ = 0;
    vector_target_ = std::monostate{};
    defence_ = 0;
    income_ = 0;
}

void City::RecordAttackBy(SideId attacker) {
    if (attacker.IsValid() && attacker.Value() < 31) {
        attacked_mask_ |= (1 << attacker.Value());
    }
}

bool City::HasBeenAttackedBy(SideId attacker) const {
    return attacker.IsValid() && attacker.Value() < 31 &&
           (attacked_mask_ & (1 << attacker.Value())) != 0;
}

void City::ReplaceSlots(SlotList slots) {
    slots_ = std::move(slots);
    if (producing_ < 0 || producing_ >= static_cast<int>(slots_.size())) {
        producing_ = kNone;
        countdown_ = 0;
    }
}

void City::SetSlot(size_t index, Slot slot) {
    if (index >= slots_.size()) {
        slots_.resize(index + 1);
    }
    slots_[index] = std::move(slot);
}

Slot City::PopLastSlot() {
    Slot removed = std::move(slots_.back());
    slots_.pop_back();
    if (producing_ != kNone && producing_ >= static_cast<int>(slots_.size())) {
        producing_ = kNone;
        countdown_ = 0;
    }
    return removed;
}

void City::TruncateSlots(size_t count) {
    if (count < slots_.size()) {
        slots_.resize(count);
    }
    if (producing_ != kNone && producing_ >= static_cast<int>(slots_.size())) {
        producing_ = kNone;
        countdown_ = 0;
    }
}

void City::SortSlotsByPrice() {
    int building = producing_;
    for (int i = 1; i < static_cast<int>(slots_.size()); ++i) {
        int pos = i;
        while (pos > 0 && slots_[pos].price < slots_[pos - 1].price) {
            std::swap(slots_[pos], slots_[pos - 1]);
            if (building == pos) {
                building = pos - 1;
            } else if (building == pos - 1) {
                building = pos;
            }
            --pos;
        }
    }
    if (producing_ != kNone) {
        producing_ = building;
    }
}

void City::SetProducingSlot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= static_cast<int>(slots_.size())) {
        producing_ = kNone;
    } else {
        producing_ = slot_idx;
    }
}

void City::SelectProduction(int slot_idx) {
    if (slot_idx >= 0 && slot_idx < static_cast<int>(slots_.size())) {
        producing_ = slot_idx;
        countdown_ = slots_[slot_idx].time;
    } else {
        producing_ = kNone;
        countdown_ = 0;
        vector_target_ = std::monostate{};
    }
}

void City::StopProduction() {
    producing_ = kNone;
    countdown_ = 0;
    vector_target_ = std::monostate{};
}

bool City::TickCountdown() {
    if (countdown_ <= 0) {
        return false;
    }
    --countdown_;
    return countdown_ == 0;
}

bool City::IsProducing() const {
    return !razed_ && producing_ >= 0 && producing_ < static_cast<int>(slots_.size()) &&
           countdown_ > 0;
}

const Slot* City::ActiveSlot() const {
    return !razed_ && producing_ >= 0 && producing_ < static_cast<int>(slots_.size())
                   ? &slots_[producing_]
                   : nullptr;
}

void City::SetVectorDestination(VectorTarget destination) {
    if (const auto* city_id = std::get_if<CityId>(&destination); city_id && !city_id->IsValid()) {
        vector_target_ = std::monostate{};
        return;
    }
    vector_target_ = destination;
}

void City::VectorToCity(CityId target_city) {
    if (!target_city.IsValid()) {
        vector_target_ = std::monostate{};
    } else {
        vector_target_ = target_city;
    }
}

CityId City::VectoredCityId() const {
    const auto* city_id = std::get_if<CityId>(&vector_target_);
    return city_id != nullptr ? *city_id : CityId::None();
}

}  // namespace w2
