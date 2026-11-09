/**
 * @file
 * @brief Implementation of `w2::ArmyRegistry`.
 */
#include "warlords/model/army_registry.hpp"

#include <algorithm>
#include <utility>

#include "absl/log/absl_check.h"

namespace w2 {

ArmyRegistry::ArmyRegistry(ArmyRegistry&& other) noexcept
        : pool_(std::move(other.pool_))
        , active_(std::move(other.active_))
        , alive_ids_(std::move(other.alive_ids_))
        , by_tile_(std::move(other.by_tile_))
        , indexed_pos_(std::move(other.indexed_pos_)) {
    RebindPool();
}

ArmyRegistry& ArmyRegistry::operator=(ArmyRegistry&& other) noexcept {
    if (this != &other) {
        pool_ = std::move(other.pool_);
        active_ = std::move(other.active_);
        alive_ids_ = std::move(other.alive_ids_);
        by_tile_ = std::move(other.by_tile_);
        indexed_pos_ = std::move(other.indexed_pos_);
        RebindPool();
    }
    return *this;
}

void ArmyRegistry::RebindPool() {
    for (auto& owned : pool_) {
        if (owned) {
            owned->RebindRegistry(this);
        }
    }
}

Army* ArmyRegistry::Make(const Army& prototype) {
    ArmyId assigned_id(static_cast<int>(pool_.size()));
    auto owned = std::make_unique<Army>(prototype);
    owned->BindRegistry(this, assigned_id);
    Army* raw_ptr = owned.get();
    pool_.push_back(std::move(owned));
    return raw_ptr;
}

Army* ArmyRegistry::Add(const Army& prototype) {
    int free_slot = 0;
    size_t insert_at = 0;
    while (insert_at < active_.size() && active_[insert_at]->Slot() == free_slot) {
        ++free_slot;
        ++insert_at;
    }
    // Handle any duplicate or sparse slot numbers defensively (e.g. after save import).
    if (insert_at < active_.size() && active_[insert_at]->Slot() < free_slot) {
        absl::flat_hash_set<int> used_slots;
        used_slots.reserve(active_.size());
        for (const Army* existing : active_) used_slots.insert(existing->Slot());
        free_slot = 0;
        while (used_slots.contains(free_slot)) ++free_slot;
        insert_at = active_.size();
        while (insert_at > 0 && active_[insert_at - 1]->Slot() > free_slot) --insert_at;
    }

    Army* added = Make(prototype);
    added->SetSlot(free_slot);
    active_.insert(active_.begin() + static_cast<std::ptrdiff_t>(insert_at), added);
    alive_ids_.insert(added->Id());
    IndexInsert(added);
    return added;
}

bool ArmyRegistry::Alive(ArmyId army_id) const {
    return army_id.IsValid() && alive_ids_.contains(army_id);
}

bool ArmyRegistry::Alive(const Army* army) const {
    return army != nullptr && Alive(army->Id()) && FindInPool(army->Id()) == army;
}

Army* ArmyRegistry::FindAlive(ArmyId army_id) const {
    return Alive(army_id) ? FindInPool(army_id) : nullptr;
}

Army* ArmyRegistry::FindInPool(ArmyId army_id) const {
    int raw = army_id.Value();
    if (raw < 0 || static_cast<size_t>(raw) >= pool_.size()) return nullptr;
    return pool_[static_cast<size_t>(raw)].get();
}

void ArmyRegistry::Remove(const Army* army) {
    if (army == nullptr) return;
    auto found = std::find(active_.begin(), active_.end(), army);
    if (found == active_.end()) return;
    IndexRemove(army);
    alive_ids_.erase(army->Id());
    active_.erase(found);
}

void ArmyRegistry::Remove(ArmyId army_id) {
    Remove(FindAlive(army_id));
}

void ArmyRegistry::RemoveAll(absl::Span<Army* const> dead) {
    if (dead.empty()) return;
    absl::flat_hash_set<const Army*> dead_set(dead.begin(), dead.end());
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* candidate = active_[static_cast<size_t>(i)];
        if (dead_set.contains(candidate)) {
            IndexRemove(candidate);
            alive_ids_.erase(candidate->Id());
            active_.erase(active_.begin() + i);
        }
    }
}

void ArmyRegistry::RemoveDisbanded() {
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* candidate = active_[static_cast<size_t>(i)];
        if (candidate->IsDisbanded()) {
            IndexRemove(candidate);
            alive_ids_.erase(candidate->Id());
            active_.erase(active_.begin() + i);
        }
    }
}

void ArmyRegistry::ClearActive() {
    active_.clear();
    alive_ids_.clear();
    by_tile_.clear();
    indexed_pos_.clear();
}

void ArmyRegistry::RestoreActive(absl::Span<Army* const> restored) {
    ClearActive();
    active_.assign(restored.begin(), restored.end());
    std::stable_sort(active_.begin(), active_.end(),
                     [](const Army* lhs, const Army* rhs) { return lhs->Slot() < rhs->Slot(); });
    alive_ids_.reserve(active_.size());
    for (Army* army : active_) {
        ABSL_DCHECK(army != nullptr);
        ABSL_DCHECK(army->Id().IsValid());
        alive_ids_.insert(army->Id());
    }
    RebuildTileIndex();
}

void ArmyRegistry::Relocate(Army* army, TilePos destination) {
    if (army != nullptr) {
        army->PlaceAt(destination);
    }
}

void ArmyRegistry::RebuildTileIndex() {
    by_tile_.clear();
    indexed_pos_.clear();
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* army = active_[static_cast<size_t>(i)];
        if (army->OnMap()) {
            TilePos pos = army->Position();
            by_tile_[pos].push_back(army);
            indexed_pos_[army->Id()] = pos;
        }
    }
}

absl::Span<Army* const> ArmyRegistry::IndexedOnTile(TilePos tile_pos) const {
    auto found = by_tile_.find(tile_pos);
    if (found == by_tile_.end()) return {};
    return found->second;
}

std::vector<Army*> ArmyRegistry::ArmiesOnTile(int x, int y) const {
    absl::Span<Army* const> span = IndexedOnTile(TilePos(x, y));
    return std::vector<Army*>(span.begin(), span.end());
}

void ArmyRegistry::IndexInsert(Army* army) {
    if (army == nullptr || !army->OnMap()) return;
    TilePos pos = army->Position();
    auto& bucket = by_tile_[pos];
    auto insert_it = std::find_if(bucket.begin(), bucket.end(), [army](const Army* existing) {
        return existing->Slot() < army->Slot();
    });
    bucket.insert(insert_it, army);
    indexed_pos_[army->Id()] = pos;
}

void ArmyRegistry::IndexRemove(const Army* army) {
    if (army == nullptr || !army->Id().IsValid()) return;
    auto pos_it = indexed_pos_.find(army->Id());
    if (pos_it == indexed_pos_.end()) return;
    auto bucket_it = by_tile_.find(pos_it->second);
    if (bucket_it != by_tile_.end()) {
        auto& bucket = bucket_it->second;
        bucket.erase(std::remove(bucket.begin(), bucket.end(), army), bucket.end());
        if (bucket.empty()) by_tile_.erase(bucket_it);
    }
    indexed_pos_.erase(pos_it);
}

}  // namespace w2
