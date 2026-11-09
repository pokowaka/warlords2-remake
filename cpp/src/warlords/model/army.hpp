/**
 * @file
 * @brief Encapsulated `w2::Army` unit/hero domain entity and `w2::Transit` value struct.
 */
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "warlords/model/army_type.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

class ArmyRegistry;
struct Item;

/** @brief Road-transit descriptor for an army marching off-map toward a city or standard. */
struct Transit {
    int turns = 0;        ///< Turns remaining until arrival.
    VectorTarget dest{};  ///< Destination city (`CityId`) or `PlantedStandard`.

    /** @brief Equality comparison across turns and destination. */
    constexpr bool operator==(const Transit& other) const = default;
};

/**
 * @brief A combat unit, hero, or summoned ally participating in a `Game`, encapsulating movement,
 * fighting strength, sea embarkation, road transit, hero progression, carried items, and blessings.
 *
 * When owned inside an `ArmyRegistry` (`registry_ != nullptr`), every position or transit mutation
 * (`PlaceAt`, `SendIntoTransit`, `ArriveAt`, `ClearTransit`) automatically updates the registry's
 * per-tile spatial index (`by_tile_`) so spatial lookups never drift from unit state.
 *
 * Invariants:
 * - `OnMap()` holds iff `!transit_.has_value() && pos_.x != kNone && pos_.y != kNone`.
 * - `SendIntoTransit` clears `pos_` to `(kNone, kNone)`; `ArriveAt` clears `transit_` and
 *   `returning_` before placing the unit at its destination tile.
 * - Copying or moving a value `Army` sets `registry_ = nullptr` on the copy unless rebound by
 *   `ArmyRegistry`.
 *
 * Not thread-safe without external synchronization; pooled and indexed by `ArmyRegistry`.
 */
class Army {
  public:
    /** @brief Optional destination coordinate for multi-turn stack walking. */
    using OptionalTilePos = std::optional<TilePos>;
    /** @brief Optional off-map road-transit state. */
    using OptionalTransit = std::optional<Transit>;
    /** @brief Ordered list of carried magic items and standards. */
    using ItemList = std::vector<Item*>;
    /** @brief Sorted set of temple indices (`0..n`) that have blessed this unit. */
    using BlessingSet = std::set<int>;

    /** @brief Constructs an unregistered neutral unit off-map at `(kNone, kNone)`. */
    Army() = default;
    /** @brief Copies unit attributes into an unregistered value `Army` (`registry_ == nullptr`). */
    Army(const Army& other);
    /** @brief Copies unit attributes while preserving `this`'s `ArmyRegistry` binding and `Id()`.
     */
    Army& operator=(const Army& other);
    /** @brief Moves unit attributes into an unregistered value `Army` (`registry_ == nullptr`). */
    Army(Army&& other) noexcept;
    /** @brief Moves unit attributes while preserving `this`'s `ArmyRegistry` binding and `Id()`. */
    Army& operator=(Army&& other) noexcept;
    ~Army() = default;

    /** @brief Returns the stable monotonic identifier assigned by `ArmyRegistry` (`None` if
     * unbound). */
    ArmyId Id() const { return id_; }
    /** @brief Returns this unit's slot index in the active army table (`>= 0`). */
    int Slot() const { return slot_; }
    /** @brief Sets this unit's slot index prior to `ArmyRegistry` insertion or restore. */
    void SetSlot(int slot) { slot_ = slot; }

    /** @brief Returns the unit's current tile coordinate (`(kNone, kNone)` while in transit). */
    TilePos Position() const { return pos_; }
    /** @brief Returns the unit's current column (`kNone` while in transit). */
    int X() const { return pos_.x; }
    /** @brief Returns the unit's current row (`kNone` while in transit). */
    int Y() const { return pos_.y; }
    /** @brief Places the unit at `pos` (`TilePos()` for off-map) and synchronizes `ArmyRegistry`.
     */
    void PlaceAt(TilePos pos);
    /**
     * @brief Removes the unit from the map (`pos_ = TilePos()`) into `transit` and synchronizes
     * `ArmyRegistry`.
     * @param transit Turns and `VectorTarget` destination.
     * @param returning True when bouncing back toward `HomeCity()`.
     */
    void SendIntoTransit(Transit transit, bool returning = false);
    /** @brief Completes road transit at `pos`, clearing `transit_` and `returning_`, and
     * re-indexes. */
    void ArriveAt(TilePos pos);
    /** @brief Clears `transit_` without altering `pos_` and synchronizes `ArmyRegistry`. */
    void ClearTransit();
    /**
     * @brief Decrements `transit_->turns` by one turn when in transit.
     * @return True when `transit_->turns <= 0` (ready to deliver).
     */
    bool TickTransit();
    /** @brief Returns the active `Transit` state if off-map in road transit, else `std::nullopt`.
     */
    const OptionalTransit& TransitState() const { return transit_; }
    /** @brief True while marching off-map in road transit. */
    bool InTransit() const { return transit_.has_value(); }
    /** @brief True when road transit is bouncing back toward `HomeCity()`. */
    bool IsReturning() const { return returning_; }
    /** @brief Sets whether road transit is returning toward `HomeCity()`. */
    void SetReturning(bool returning) { returning_ = returning; }
    /** @brief True when standing on a valid map tile (not in transit and not `kNone`). */
    bool OnMap() const { return !transit_.has_value() && pos_.x != kNone && pos_.y != kNone; }

    /** @brief Returns the owning faction (`SideId::None()` for neutral garrisons). */
    SideId Owner() const { return owner_; }
    /** @brief Returns the owning faction's raw index (`kNone` for neutral garrisons). */
    int OwnerIndex() const { return owner_.Value(); }
    /** @brief Sets the owning faction. */
    void SetOwner(SideId owner) { owner_ = owner; }
    /** @brief True when this unit belongs to no faction (`Owner() == SideId::None()`). */
    bool IsNeutral() const { return !owner_.IsValid(); }
    /** @brief True when this unit belongs to `side_id`. */
    bool OwnedBy(SideId side_id) const { return owner_ == side_id; }
    /** @brief True when this unit belongs to raw side index `side_index`. */
    bool OwnedBy(int side_index) const { return owner_.Value() == side_index; }

    /** @brief Returns the catalog `ArmyTypeId`. */
    ArmyTypeId Type() const { return type_; }
    /** @brief Returns the integer catalog type index (`0..63`). */
    int TypeIndex() const { return type_.Value(); }
    /** @brief Sets the catalog `ArmyTypeId`. */
    void SetType(ArmyTypeId type) { type_ = type; }
    /** @brief True when `type_` is the Hero unit type (`kHeroTypeIndex`). */
    bool Hero() const;

    /** @brief Returns the unit's or hero's display name. */
    const std::string& Name() const { return name_; }
    /** @brief Sets the unit's or hero's display name. */
    void SetName(std::string name);
    /** @brief True when this hero uses heroine portraits and pronouns. */
    bool IsFemale() const { return female_; }
    /** @brief Sets whether this hero uses heroine portraits and pronouns. */
    void SetFemale(bool female) { female_ = female; }

    /** @brief Returns the `CityId` of the city that produced this unit (`CityId::None()` if none).
     */
    CityId HomeCity() const { return home_city_; }
    /** @brief Returns the raw index of `HomeCity()` (`kNone` if none). */
    int HomeCityIndex() const { return home_city_.Value(); }
    /** @brief Sets the `CityId` of the city that produced this unit. */
    void SetHomeCity(CityId city_id) { home_city_ = city_id; }

    /** @brief Returns base fighting strength (`1..9` in combat, higher before clamping). */
    int Strength() const { return strength_; }
    /** @brief Sets base fighting strength. */
    void SetStrength(int strength) { strength_ = strength; }

    /** @brief Returns movement points remaining this turn. */
    int Moves() const { return moves_; }
    /** @brief Sets movement points remaining this turn. */
    void SetMoves(int moves) { moves_ = moves; }
    /** @brief Deducts `cost` movement points, clamping remaining moves to `>= 0`. */
    void SpendMoves(int cost);
    /** @brief Adds signed `delta` to movement points remaining. */
    void AddMoves(int delta) { moves_ += delta; }

    /** @brief Returns base movement points per turn on land. */
    int MaxMoves() const { return max_moves_; }
    /** @brief Sets base movement points per turn on land. */
    void SetMaxMoves(int max_moves) { max_moves_ = max_moves; }
    /** @brief Adds `delta` to base movement points per turn. */
    void AddMaxMoves(int delta) { max_moves_ += delta; }

    /** @brief Returns gold upkeep per turn. */
    int Upkeep() const { return upkeep_; }
    /** @brief Sets gold upkeep per turn. */
    void SetUpkeep(int upkeep) { upkeep_ = upkeep; }

    /** @brief Returns combat medals earned (`0..4`). */
    int Medals() const { return medals_; }
    /** @brief Sets combat medals earned. */
    void SetMedals(int medals) { medals_ = medals; }
    /** @brief Increments combat medals by one. */
    void AddMedal() { ++medals_; }

    /** @brief True while embarked aboard a transport ship at sea. */
    bool IsAtSea() const { return at_sea_; }
    /** @brief Sets sea-embarkation status. */
    void SetAtSea(bool at_sea) { at_sea_ = at_sea; }
    /** @brief Embarks the unit aboard a ship (`at_sea_ = true`). */
    void BoardShip() { at_sea_ = true; }
    /** @brief Disembarks the unit onto land (`at_sea_ = false`). */
    void ComeAshore() { at_sea_ = false; }

    /** @brief Returns the selection-bar group identifier (`0` when ungrouped). */
    int Group() const { return group_; }
    /** @brief Sets the selection-bar group identifier (`0` for ungrouped). */
    void SetGroup(int group) { group_ = group; }

    /** @brief Returns the active multi-turn walk destination, if any. */
    const OptionalTilePos& Target() const { return target_; }
    /** @brief True when a multi-turn walk destination is set. */
    bool HasTarget() const { return target_.has_value(); }
    /** @brief Assigns `target_pos` as the unit's multi-turn walk destination. */
    void SetTarget(TilePos target_pos) { target_ = target_pos; }
    /** @brief Clears any active multi-turn walk destination. */
    void ClearTarget() { target_.reset(); }

    /** @brief True when fortified/defending (skipped by the turn army cycle). */
    bool IsFortified() const { return fortified_; }
    /** @brief Sets fortified/defending status. */
    void SetFortified(bool fortified) { fortified_ = fortified; }

    /** @brief True when on the map, unfortified, not marked done, and holding `Moves() > 0`. */
    bool CanMove() const { return moves_ > 0 && !done_ && !fortified_ && OnMap(); }

    /** @brief Returns a hero's current level. */
    int Level() const { return level_; }
    /** @brief Sets a hero's current level. */
    void SetLevel(int level) { level_ = level; }

    /** @brief Returns a hero's accumulated experience points. */
    int Experience() const { return experience_; }
    /** @brief Sets a hero's accumulated experience points. */
    void SetExperience(int experience) { experience_ = experience; }

    /** @brief Returns a hero's honorific title. */
    const std::string& Title() const { return title_; }
    /** @brief Sets a hero's honorific title. */
    void SetTitle(std::string title);

    /** @brief Returns read-only access to the items carried by this unit. */
    const ItemList& Items() const { return items_; }
    /** @brief Replaces the carried item list. */
    void SetItems(ItemList items);
    /** @brief True when `item` is currently carried by this unit. */
    bool HasItem(const Item* item) const;
    /** @brief Appends `item` to this unit's carried inventory (if non-null). */
    void AddItem(Item* item);
    /** @brief Removes `item` from this unit's carried inventory; returns true if found. */
    bool RemoveItem(const Item* item);
    /** @brief Empties and returns all items currently carried by this unit. */
    ItemList TakeAllItems();
    /** @brief Clears all carried items without returning them. */
    void ClearItems() { items_.clear(); }

    /** @brief Returns the sorted set of temple indices that have blessed this unit. */
    const BlessingSet& Blessings() const { return blessings_; }
    /** @brief True when temple `temple_id` has already blessed this unit. */
    bool HasBlessing(int temple_id) const { return blessings_.count(temple_id) != 0; }
    /** @brief Records a temple blessing from `temple_id`. */
    void AddBlessing(int temple_id) { blessings_.insert(temple_id); }
    /** @brief Clears all recorded temple blessings. */
    void ClearBlessings() { blessings_.clear(); }

    /** @brief True when marked done for the current turn. */
    bool IsDone() const { return done_; }
    /** @brief Marks or clears the per-turn `done` flag. */
    void SetDone(bool done) { done_ = done; }

    /** @brief True when already visited during the current army-cycle pass. */
    bool IsOffered() const { return offered_; }
    /** @brief Marks or clears the army-cycle `offered` flag. */
    void SetOffered(bool offered) { offered_ = offered; }

    /** @brief True when slated for disbandment at the end of road-transit processing. */
    bool IsDisbanded() const { return disbanded_; }
    /** @brief Marks or clears the `disbanded` flag. */
    void SetDisbanded(bool disbanded) { disbanded_ = disbanded; }

    /** @brief Clears per-turn cycle flags (`done_ = false`, `offered_ = false`). */
    void ResetTurnMarks() {
        done_ = false;
        offered_ = false;
    }

    /** @brief Returns the persisted Modern AI cross-turn order (`0` = none). */
    int AiOrder() const { return ai_order_; }
    /** @brief Sets the persisted Modern AI cross-turn order. */
    void SetAiOrder(int order) { ai_order_ = order; }
    /** @brief Returns the persisted Modern AI destination index (`kNone` if unset). */
    int AiDest() const { return ai_dest_; }
    /** @brief Sets the persisted Modern AI destination index. */
    void SetAiDest(int dest) { ai_dest_ = dest; }
    /** @brief Returns the persisted Modern AI group identifier (`0` if ungrouped). */
    int AiGroup() const { return ai_group_; }
    /** @brief Sets the persisted Modern AI group identifier. */
    void SetAiGroup(int group) { ai_group_ = group; }
    /** @brief Resets persisted Modern AI order memory (`ai_order_`, `ai_dest_`, `ai_group_`). */
    void ClearAiOrder() {
        ai_order_ = 0;
        ai_dest_ = kNone;
        ai_group_ = 0;
    }

  private:
    friend class ArmyRegistry;

    void BindRegistry(ArmyRegistry* registry, ArmyId assigned_id);
    void RebindRegistry(ArmyRegistry* registry) { registry_ = registry; }
    void CopyAttributesFrom(const Army& other);
    void MoveAttributesFrom(Army&& other) noexcept;

    ArmyRegistry* registry_ = nullptr;   ///< Owning registry for automatic spatial re-indexing.
    ArmyId id_ = ArmyId::None();         ///< Stable monotonic pool handle.
    int slot_ = 0;                       ///< Ascending army-table slot index.
    TilePos pos_;                        ///< Current tile coordinate (`kNone` in transit).
    SideId owner_ = SideId::None();      ///< Owning faction (`SideId::None()` for neutral).
    ArmyTypeId type_ = ArmyTypeId(0);    ///< Catalog unit type identifier.
    std::string name_;                   ///< Unit or hero display name.
    bool female_ = false;                ///< Heroine portrait/pronoun flag.
    int strength_ = 0;                   ///< Base fighting strength.
    int moves_ = 0;                      ///< Movement points remaining this turn.
    int max_moves_ = 0;                  ///< Base movement points per turn.
    int upkeep_ = 0;                     ///< Per-turn gold upkeep.
    CityId home_city_ = CityId::None();  ///< Producing home city identifier.
    int medals_ = 0;                     ///< Combat medals won (`0..4`).
    bool at_sea_ = false;                ///< True when embarked on water.
    int group_ = 0;                      ///< Selection-bar group tag (`0` ungrouped).
    OptionalTilePos target_;             ///< Active multi-turn walk destination.
    bool fortified_ = false;             ///< Fortified/skipped in the turn cycle.
    int level_ = 0;                      ///< Hero level.
    int experience_ = 0;                 ///< Hero experience points.
    std::string title_;                  ///< Hero honorific title.
    ItemList items_;                     ///< Carried magic items and standards.
    BlessingSet blessings_;              ///< Temple indices that have blessed this unit.
    OptionalTransit transit_;            ///< Active road-transit state when off-map.
    bool returning_ = false;             ///< True when bouncing back to `home_city_`.
    bool done_ = false;                  ///< Done for the current turn.
    bool offered_ = false;               ///< Already offered in this cycle pass.
    bool disbanded_ = false;             ///< Marked disbanded during transit delivery.
    int ai_order_ = 0;                   ///< Persisted Modern AI cross-turn order.
    int ai_dest_ = kNone;                ///< Persisted Modern AI destination index.
    int ai_group_ = 0;                   ///< Persisted Modern AI group identifier.
};

}  // namespace w2
