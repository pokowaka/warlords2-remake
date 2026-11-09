/**
 * @file
 * @brief Implementation of `w2::Quest`.
 */
#include "warlords/model/quest.hpp"

#include <type_traits>

#include "warlords/model/game_state.hpp"

namespace w2 {

Quest::Quest(int type, ArmyId hero, Target target, int required, int done)
        : type_(type), hero_(hero), target_(target), required_(required), done_(done) {}

void Quest::AddProgress(int delta) {
    done_ += delta;
}

bool Quest::HasTarget() const {
    return std::visit(
            [this](const auto& held) -> bool {
                using HeldType = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<HeldType, std::monostate>) {
                    return type_ == kPillageGold;
                } else {
                    return held.IsValid();
                }
            },
            target_);
}

CityId Quest::TargetCityId() const {
    const auto* ptr = std::get_if<CityId>(&target_);
    return ptr != nullptr ? *ptr : CityId::None();
}

SideId Quest::TargetSideId() const {
    const auto* ptr = std::get_if<SideId>(&target_);
    return ptr != nullptr ? *ptr : SideId::None();
}

ItemId Quest::TargetItemId() const {
    const auto* ptr = std::get_if<ItemId>(&target_);
    return ptr != nullptr ? *ptr : ItemId::None();
}

ArmyTypeId Quest::TargetArmyTypeId() const {
    const auto* ptr = std::get_if<ArmyTypeId>(&target_);
    return ptr != nullptr ? *ptr : ArmyTypeId::None();
}

ArmyId Quest::TargetArmyId() const {
    const auto* ptr = std::get_if<ArmyId>(&target_);
    return ptr != nullptr ? *ptr : ArmyId::None();
}

Army* Quest::ResolveHero(Game& game) const {
    return game.Armies().FindInPool(hero_);
}

const Army* Quest::ResolveHero(const Game& game) const {
    return game.Armies().FindInPool(hero_);
}

City* Quest::ResolveCity(Game& game) const {
    return game.HasMap() ? game.GetMap().CityById(TargetCityId()) : nullptr;
}

const City* Quest::ResolveCity(const Game& game) const {
    return game.HasMap() ? game.GetMap().CityById(TargetCityId()) : nullptr;
}

Side* Quest::ResolveSide(Game& game) const {
    return game.HasMap() ? game.GetMap().SideById(TargetSideId()) : nullptr;
}

const Side* Quest::ResolveSide(const Game& game) const {
    return game.HasMap() ? game.GetMap().SideById(TargetSideId()) : nullptr;
}

Item* Quest::ResolveItem(Game& game) const {
    return game.HasMap() ? game.GetMap().ItemById(TargetItemId()) : nullptr;
}

const Item* Quest::ResolveItem(const Game& game) const {
    return game.HasMap() ? game.GetMap().ItemById(TargetItemId()) : nullptr;
}

const ArmyType* Quest::ResolveArmyType(const Game& game) const {
    return game.ArmyTypes().ById(TargetArmyTypeId());
}

Army* Quest::ResolveTargetArmy(Game& game) const {
    return game.Armies().FindInPool(TargetArmyId());
}

const Army* Quest::ResolveTargetArmy(const Game& game) const {
    return game.Armies().FindInPool(TargetArmyId());
}

}  // namespace w2
