/**
 * @file
 * @brief Implementation of `w2::Army` and its spatial synchronization with `w2::ArmyRegistry`.
 */
#include "warlords/model/army.hpp"

#include <algorithm>
#include <utility>

#include "warlords/model/army_registry.hpp"

namespace w2 {

Army::Army(const Army& other) {
    CopyAttributesFrom(other);
}

Army& Army::operator=(const Army& other) {
    if (this == &other) {
        return *this;
    }
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    ArmyRegistry* saved_registry = registry_;
    ArmyId saved_id = id_;
    CopyAttributesFrom(other);
    registry_ = saved_registry;
    if (saved_registry != nullptr) {
        id_ = saved_id;
    }
    if (notify) {
        registry_->IndexInsert(this);
    }
    return *this;
}

Army::Army(Army&& other) noexcept {
    MoveAttributesFrom(std::move(other));
}

Army& Army::operator=(Army&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    ArmyRegistry* saved_registry = registry_;
    ArmyId saved_id = id_;
    MoveAttributesFrom(std::move(other));
    registry_ = saved_registry;
    if (saved_registry != nullptr) {
        id_ = saved_id;
    }
    if (notify) {
        registry_->IndexInsert(this);
    }
    return *this;
}

void Army::BindRegistry(ArmyRegistry* registry, ArmyId assigned_id) {
    registry_ = registry;
    id_ = assigned_id;
}

void Army::CopyAttributesFrom(const Army& other) {
    registry_ = nullptr;
    id_ = other.id_;
    slot_ = other.slot_;
    pos_ = other.pos_;
    owner_ = other.owner_;
    type_ = other.type_;
    name_ = other.name_;
    female_ = other.female_;
    strength_ = other.strength_;
    moves_ = other.moves_;
    max_moves_ = other.max_moves_;
    upkeep_ = other.upkeep_;
    home_city_ = other.home_city_;
    medals_ = other.medals_;
    at_sea_ = other.at_sea_;
    group_ = other.group_;
    target_ = other.target_;
    fortified_ = other.fortified_;
    level_ = other.level_;
    experience_ = other.experience_;
    title_ = other.title_;
    items_ = other.items_;
    blessings_ = other.blessings_;
    transit_ = other.transit_;
    returning_ = other.returning_;
    done_ = other.done_;
    offered_ = other.offered_;
    disbanded_ = other.disbanded_;
}

void Army::MoveAttributesFrom(Army&& other) noexcept {
    registry_ = nullptr;
    id_ = other.id_;
    slot_ = other.slot_;
    pos_ = other.pos_;
    owner_ = other.owner_;
    type_ = other.type_;
    name_ = std::move(other.name_);
    female_ = other.female_;
    strength_ = other.strength_;
    moves_ = other.moves_;
    max_moves_ = other.max_moves_;
    upkeep_ = other.upkeep_;
    home_city_ = other.home_city_;
    medals_ = other.medals_;
    at_sea_ = other.at_sea_;
    group_ = other.group_;
    target_ = other.target_;
    fortified_ = other.fortified_;
    level_ = other.level_;
    experience_ = other.experience_;
    title_ = std::move(other.title_);
    items_ = std::move(other.items_);
    blessings_ = std::move(other.blessings_);
    transit_ = std::move(other.transit_);
    returning_ = other.returning_;
    done_ = other.done_;
    offered_ = other.offered_;
    disbanded_ = other.disbanded_;
}

void Army::PlaceAt(TilePos pos) {
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    pos_ = pos;
    if (notify) {
        registry_->IndexInsert(this);
    }
}

void Army::SendIntoTransit(Transit transit, bool returning) {
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    transit_ = std::move(transit);
    pos_ = TilePos();
    returning_ = returning;
}

void Army::ArriveAt(TilePos pos) {
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    transit_.reset();
    returning_ = false;
    pos_ = pos;
    if (notify) {
        registry_->IndexInsert(this);
    }
}

void Army::ClearTransit() {
    bool notify = registry_ != nullptr && registry_->Alive(this);
    if (notify) {
        registry_->IndexRemove(this);
    }
    transit_.reset();
    if (notify) {
        registry_->IndexInsert(this);
    }
}

bool Army::TickTransit() {
    if (!transit_.has_value()) {
        return false;
    }
    --transit_->turns;
    return transit_->turns <= 0;
}

bool Army::Hero() const {
    return type_ == ArmyTypeId(kHeroTypeIndex);
}

void Army::SetName(std::string name) {
    name_ = std::move(name);
}

void Army::SpendMoves(int cost) {
    moves_ = std::max(0, moves_ - cost);
}

void Army::SetTitle(std::string title) {
    title_ = std::move(title);
}

void Army::SetItems(ItemList items) {
    items_ = std::move(items);
}

bool Army::HasItem(const Item* item) const {
    return std::find(items_.begin(), items_.end(), item) != items_.end();
}

void Army::AddItem(Item* item) {
    if (item != nullptr) {
        items_.push_back(item);
    }
}

bool Army::RemoveItem(const Item* item) {
    auto found = std::find(items_.begin(), items_.end(), item);
    if (found == items_.end()) {
        return false;
    }
    items_.erase(found);
    return true;
}

Army::ItemList Army::TakeAllItems() {
    ItemList taken = std::move(items_);
    items_.clear();
    return taken;
}

}  // namespace w2
