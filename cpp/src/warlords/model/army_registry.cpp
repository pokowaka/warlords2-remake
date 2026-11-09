/**
 * @file
 * @brief Implementation of `w2::ArmyRegistry`.
 */
#include "warlords/model/army_registry.hpp"

#include <algorithm>

#include "absl/log/absl_check.h"

namespace w2 {

Army* ArmyRegistry::Make(const Army& prototype) {
    ArmyId assigned_id(static_cast<int>(pool_.size()));
    auto owned = std::make_unique<Army>(prototype);
    owned->id = assigned_id;
    Army* raw_ptr = owned.get();
    pool_.push_back(std::move(owned));
    return raw_ptr;
}

Army* ArmyRegistry::Add(const Army& prototype) {
    int free_slot = 0;
    size_t insert_at = 0;
    while (insert_at < active_.size() && active_[insert_at]->slot == free_slot) {
        ++free_slot;
        ++insert_at;
    }
    // Handle any duplicate or sparse slot numbers defensively (e.g. after save import).
    if (insert_at < active_.size() && active_[insert_at]->slot < free_slot) {
        absl::flat_hash_set<int> used_slots;
        used_slots.reserve(active_.size());
        for (const Army* existing : active_) used_slots.insert(existing->slot);
        free_slot = 0;
        while (used_slots.contains(free_slot)) ++free_slot;
        insert_at = active_.size();
        while (insert_at > 0 && active_[insert_at - 1]->slot > free_slot) --insert_at;
    }

    Army* added = Make(prototype);
    added->slot = free_slot;
    active_.insert(active_.begin() + static_cast<std::ptrdiff_t>(insert_at), added);
    alive_ids_.insert(added->id);
    IndexInsert(added);
    return added;
}

bool ArmyRegistry::Alive(ArmyId army_id) const {
    return army_id.IsValid() && alive_ids_.contains(army_id);
}

bool ArmyRegistry::Alive(const Army* army) const {
    return army != nullptr && Alive(army->id) && FindInPool(army->id) == army;
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
    alive_ids_.erase(army->id);
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
            alive_ids_.erase(candidate->id);
            active_.erase(active_.begin() + i);
        }
    }
}

void ArmyRegistry::RemoveDisbanded() {
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* candidate = active_[static_cast<size_t>(i)];
        if (candidate->disbanded) {
            IndexRemove(candidate);
            alive_ids_.erase(candidate->id);
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
                     [](const Army* lhs, const Army* rhs) { return lhs->slot < rhs->slot; });
    alive_ids_.reserve(active_.size());
    for (Army* army : active_) {
        ABSL_DCHECK(army != nullptr);
        ABSL_DCHECK(army->id.IsValid());
        alive_ids_.insert(army->id);
    }
    RebuildTileIndex();
}

void ArmyRegistry::Relocate(Army* army, TilePos destination) {
    if (army == nullptr) return;
    bool is_alive = Alive(army);
    if (is_alive) IndexRemove(army);
    army->x = destination.x;
    army->y = destination.y;
    if (is_alive) IndexInsert(army);
}

void ArmyRegistry::RebuildTileIndex() {
    by_tile_.clear();
    indexed_pos_.clear();
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* army = active_[static_cast<size_t>(i)];
        if (army->OnMap()) {
            TilePos pos = army->Position();
            by_tile_[pos].push_back(army);
            indexed_pos_[army->id] = pos;
        }
    }
}

absl::Span<Army* const> ArmyRegistry::IndexedOnTile(TilePos tile_pos) const {
    auto found = by_tile_.find(tile_pos);
    if (found == by_tile_.end()) return {};
    return found->second;
}

std::vector<Army*> ArmyRegistry::ArmiesOnTile(int x, int y) const {
    std::vector<Army*> stack;
    for (int i = static_cast<int>(active_.size()) - 1; i >= 0; --i) {
        Army* army = active_[static_cast<size_t>(i)];
        if (!army->transit && army->x == x && army->y == y) {
            stack.push_back(army);
        }
    }
    return stack;
}

void ArmyRegistry::IndexInsert(Army* army) {
    if (army == nullptr || !army->OnMap()) return;
    TilePos pos = army->Position();
    auto& bucket = by_tile_[pos];
    auto insert_it = std::find_if(bucket.begin(), bucket.end(), [army](const Army* existing) {
        return existing->slot < army->slot;
    });
    bucket.insert(insert_it, army);
    indexed_pos_[army->id] = pos;
}

void ArmyRegistry::IndexRemove(const Army* army) {
    if (army == nullptr || !army->id.IsValid()) return;
    auto pos_it = indexed_pos_.find(army->id);
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
