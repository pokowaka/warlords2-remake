/**
 * @file
 * @brief Encapsulated army lifetime pool, slot-ordered table, and spatial tile index
 * (`w2::ArmyRegistry`).
 */
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"

#include "warlords/model/army.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

/**
 * @brief Owning registry for every `Army` created in a `Game`, maintaining stable `ArmyId`
 * handles, ascending `slot` order, and a per-tile spatial index.
 *
 * Every army created via `Make` or `Add` receives a unique, never-reused
 * `ArmyId` backed by `pool_`, so pointers and `ArmyId` handles remain valid for
 * the lifetime of the `Game` even after an army is slain or disbanded. Active
 * armies (`active_`) occupy the lowest free table `slot` (`>= 0`) and are kept
 * sorted by ascending `slot`, matching the 1993 engine's fixed army table.
 *
 * Invariants:
 * - For all `k` in `[0, pool_.size())`, `pool_[k]->id == ArmyId(k)`.
 * - `active_` is strictly sorted by `army->slot` ascending, and every element's
 *   `id` belongs to `alive_ids_`.
 * - `by_tile_[pos]` lists active on-map armies at `pos` in descending `slot`
 *   order (newest/highest slot first) as updated by `Add`, `Remove`,
 *   `RestoreActive`, `RebuildTileIndex`, and `Relocate`.
 *
 * Not thread-safe without external synchronization; owned exclusively by `Game`.
 */
class ArmyRegistry {
  public:
    /** @brief Iterator type over active `Army*` pointers in ascending slot order. */
    using const_iterator =
            std::vector<Army*>::const_iterator;  // NOLINT(readability-identifier-naming)

    /** @brief Constructs an empty army registry. */
    ArmyRegistry() = default;
    ~ArmyRegistry() = default;

    ArmyRegistry(const ArmyRegistry&) = delete;
    ArmyRegistry& operator=(const ArmyRegistry&) = delete;
    ArmyRegistry(ArmyRegistry&&) noexcept = default;
    ArmyRegistry& operator=(ArmyRegistry&&) noexcept = default;

    /**
     * @brief Allocates a new `Army` in the lifetime pool with a unique `ArmyId`, without placing it
     * in active play.
     * @param prototype Initial field values copied into the pooled `Army`.
     * @return Stable non-null pointer owned by this registry.
     */
    Army* Make(const Army& prototype);

    /**
     * @brief Allocates `prototype` in the lowest free `slot` and inserts it into active play.
     * @param prototype Initial field values for the new army.
     * @return Stable non-null pointer to the active `Army`.
     */
    Army* Add(const Army& prototype);

    /** @brief True when `army_id` names an army currently in active play. */
    bool Alive(ArmyId army_id) const;

    /** @brief True when `army` belongs to this registry and is currently in active play. */
    bool Alive(const Army* army) const;

    /** @brief Resolves `army_id` to an active `Army*`, or `nullptr` if dead or unknown. */
    Army* FindAlive(ArmyId army_id) const;

    /**
     * @brief Resolves `army_id` to its pooled `Army*` (whether alive or removed), or `nullptr` if
     * out of range.
     */
    Army* FindInPool(ArmyId army_id) const;

    /** @brief Removes `army` from active play while keeping its allocation in `pool_`. */
    void Remove(const Army* army);

    /** @brief Removes the army with `army_id` from active play while keeping it in `pool_`. */
    void Remove(ArmyId army_id);

    /** @brief Removes every army listed in `dead` from active play. */
    void RemoveAll(absl::Span<Army* const> dead);

    /** @brief Removes every active army marked with `disbanded == true`. */
    void RemoveDisbanded();

    /** @brief Clears all active armies and spatial entries while retaining `pool_`. */
    void ClearActive();

    /**
     * @brief Replaces the active army table with `restored`, sorted by ascending `slot`.
     * @param restored Pooled armies belonging to this registry.
     */
    void RestoreActive(absl::Span<Army* const> restored);

    /**
     * @brief Moves `army` to `destination` (`TilePos()` for off-map) and updates the spatial tile
     * index.
     */
    void Relocate(Army* army, TilePos destination);

    /** @brief Rebuilds the per-tile spatial index from current active army coordinates. */
    void RebuildTileIndex();

    /** @brief Direct $O(1)$ spatial index lookup at `tile_pos` in top-down slot order. */
    absl::Span<Army* const> IndexedOnTile(TilePos tile_pos) const;

    /** @brief Returns all active armies standing on `(x, y)` (not in transit) in top-down slot
     * order. */
    std::vector<Army*> ArmiesOnTile(int x, int y) const;

    /** @brief Read-only span over all active armies in ascending slot order. */
    absl::Span<Army* const> AllActive() const { return active_; }

    /** @brief Number of armies currently in active play. */
    size_t size() const { return active_.size(); }

    /** @brief True when no armies are currently in active play. */
    bool empty() const { return active_.empty(); }

    /** @brief Iterator to the first active army (lowest slot). */
    const_iterator begin() const { return active_.begin(); }

    /** @brief Past-the-end iterator over active armies. */
    const_iterator end() const { return active_.end(); }

    /** @brief Pointer to the contiguous active army array. */
    Army* const* data() const { return active_.data(); }

    /** @brief Returns the active army at `index` in slot order. */
    Army* operator[](size_t index) const { return active_[index]; }

  private:
    using ArmyPool = std::vector<std::unique_ptr<Army>>;
    using ArmyList = std::vector<Army*>;
    using TileArmyMap = absl::flat_hash_map<TilePos, ArmyList>;
    using ArmyPosMap = absl::flat_hash_map<ArmyId, TilePos>;

    void IndexInsert(Army* army);
    void IndexRemove(const Army* army);

    ArmyPool pool_;                          ///< Lifetime-owning pool indexed by `ArmyId`.
    ArmyList active_;                        ///< Active armies sorted by ascending `slot`.
    absl::flat_hash_set<ArmyId> alive_ids_;  ///< Set of `ArmyId`s currently in `active_`.
    TileArmyMap by_tile_;                    ///< Spatial stacks (top-down slot).
    ArmyPosMap indexed_pos_;                 ///< Last indexed tile per `ArmyId`.
};

}  // namespace w2
