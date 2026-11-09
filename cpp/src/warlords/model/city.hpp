/**
 * @file
 * @brief Encapsulated `w2::City` domain entity and production `w2::Slot` value struct.
 */
#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

/**
 * @brief One unit-production slot inside a city, storing the localized unit statistics.
 */
struct Slot {
    int type = 0;      ///< Catalog army type index (`0..63`).
    std::string name;  ///< Localized unit type display name.
    int strength = 0;  ///< Fighting strength of units built from this slot.
    int time = 0;      ///< Turns required for this city to build one unit.
    int cost = 0;      ///< Per-turn gold upkeep basis of units built from this slot.
    int move = 0;      ///< Base movement points of units built from this slot.
    int price = 0;     ///< Gold purchase value of this slot (used for sorting and spoils).

    /** @brief Value equality across all slot statistics and name. */
    bool operator==(const Slot& other) const {
        return type == other.type && name == other.name && strength == other.strength &&
               time == other.time && cost == other.cost && move == other.move &&
               price == other.price;
    }
};

/**
 * @brief A 2x2 fortified settlement on the scenario map that generates treasury income, builds
 * army units from up to four `Slot`s, and optionally vectors newly built units elsewhere.
 *
 * Models ownership, territorial AI claims, and razing attribution with strong `SideId` values and
 * models production vectoring with `VectorTarget` (`std::monostate`, `CityId`, or
 * `PlantedStandard`).
 *
 * Invariants:
 * - When `razed_ == true`: `owner_ == SideId::None()`, `claim_ == SideId::None()`,
 *   `slots_.empty()`, `producing_ == kNone`, `countdown_ == 0`, `defence_ == 0`,
 *   `income_ == 0`, and `IsVectored() == false`.
 * - `producing_` is either `kNone` or a valid index in `[0, slots_.size())` whenever slots are
 *   replaced, truncated, popped, or re-sorted.
 * - A `CityId` stored inside `vector_target_` always satisfies `IsValid()`.
 *
 * Not thread-safe without external synchronization; owned contiguously by `Map`.
 */
class City {
  public:
    /** @brief Ordered list of unit production slots available in this city. */
    using SlotList = std::vector<Slot>;
    /** @brief Raw scenario army type indices (`0..63`) configured before slot randomization. */
    using ProduceList = std::vector<int>;

    /** @brief Constructs a neutral, unrazed city at `(0, 0)` with `CityId(0)`. */
    City() = default;

    /** @brief Returns this city's strong identifier (`0..cities.size()-1`). */
    CityId Id() const { return id_; }
    /** @brief Returns this city's integer index in `Map::cities`. */
    int Index() const { return id_.Value(); }
    /** @brief Assigns this city's strong identifier. */
    void SetId(CityId city_id) { id_ = city_id; }

    /** @brief Returns the top-left tile coordinate `(x, y)` of the 2x2 city footprint. */
    TilePos Position() const { return pos_; }
    /** @brief Returns the left column index of the 2x2 city footprint. */
    int X() const { return pos_.x; }
    /** @brief Returns the top row index of the 2x2 city footprint. */
    int Y() const { return pos_.y; }
    /** @brief Sets the top-left tile coordinate of the 2x2 city footprint. */
    void SetPosition(TilePos pos) { pos_ = pos; }
    /** @brief True when `(tile_x, tile_y)` lies inside this city's 2x2 tile footprint. */
    bool ContainsTile(int tile_x, int tile_y) const;

    /** @brief Returns the city's display name. */
    const std::string& Name() const { return name_; }
    /** @brief Updates the city's display name to `name`. */
    void Rename(std::string name);

    /** @brief Returns the gold income earned per turn while held by a faction. */
    int Income() const { return income_; }
    /** @brief Sets the per-turn gold income of this city. */
    void SetIncome(int income) { income_ = income; }

    /** @brief Returns the scenario's initial army type indices before slot generation. */
    const ProduceList& Produces() const { return produces_; }
    /** @brief Appends `army_type` to the scenario's initial production type list. */
    void AddProducedType(int army_type);
    /** @brief Replaces the scenario's initial production type list. */
    void SetProduces(ProduceList produces);

    /** @brief Returns the city's fortification defence level (`0` when razed, `1..2` standing). */
    int Defence() const { return defence_; }
    /** @brief Sets the city's fortification defence level. */
    void SetDefence(int defence) { defence_ = defence; }

    /** @brief Returns the owning faction (`SideId::None()` when neutral or razed). */
    SideId Owner() const { return owner_; }
    /** @brief Returns the owning faction's raw index (`kNone` when neutral or razed). */
    int OwnerIndex() const { return owner_.Value(); }
    /** @brief Sets the owning faction (ignored if `razed_ == true`, keeping `SideId::None()`). */
    void SetOwner(SideId owner);
    /** @brief True when no faction currently holds this city (`Owner() == SideId::None()`). */
    bool IsNeutral() const { return !owner_.IsValid(); }
    /** @brief True when this city is currently held by `side_id`. */
    bool OwnedBy(SideId side_id) const { return owner_ == side_id; }
    /** @brief True when this city is currently held by raw side index `side_index`. */
    bool OwnedBy(int side_index) const { return owner_.Value() == side_index; }

    /** @brief True when this city has been burnt to ruins and removed from play. */
    bool IsRazed() const { return razed_; }
    /** @brief Sets the razed state flag (clears ownership when `razed` is true). */
    void SetRazed(bool razed);
    /** @brief Returns the faction credited with razing this city (`SideId::None()` if standing). */
    SideId RazedBy() const { return razed_by_; }
    /** @brief Sets the faction attributed on the razed ruin graphic. */
    void SetRazedBy(SideId razed_by) { razed_by_ = razed_by; }
    /**
     * @brief Burns this city to permanent ruins, clearing owner, claim, slots, production,
     * vectoring, defence, and income.
     * @param razed_by Faction whose ruin banner is placed (`SideId::None()` defaults to `0`).
     */
    void Raze(SideId razed_by);

    /** @brief Returns the faction claiming this city as its rightful territory (`SideId::None()` if
     * unclaimed). */
    SideId Claim() const { return claim_; }
    /** @brief Sets the faction claiming this city (`SideId::None()` to clear). */
    void SetClaim(SideId claim) { claim_ = claim; }

    /** @brief Returns the bitmask of sides (`1 << side_index`) that have attacked this city. */
    int AttackedMask() const { return attacked_mask_; }
    /** @brief Restores the raw attacked-by-side bitmask. */
    void SetAttackedMask(int mask) { attacked_mask_ = mask; }
    /** @brief Clears all recorded attacker bits (e.g. upon city capture). */
    void ClearAttackedMask() { attacked_mask_ = 0; }
    /** @brief True when at least one faction has attacked this city since its last capture. */
    bool HasBeenAttacked() const { return attacked_mask_ != 0; }
    /** @brief Records in `attacked_mask_` that `attacker` assaulted this city. */
    void RecordAttackBy(SideId attacker);
    /** @brief True when `attacker` is recorded in `attacked_mask_`. */
    bool HasBeenAttackedBy(SideId attacker) const;

    /** @brief Returns read-only access to the city's unit production slots. */
    const SlotList& Slots() const { return slots_; }
    /** @brief Returns the number of unit production slots (`0..4`). */
    int SlotCount() const { return static_cast<int>(slots_.size()); }
    /** @brief Returns the production slot at `index`. */
    const Slot& SlotAt(size_t index) const { return slots_[index]; }
    /** @brief Replaces all production slots and stops production if `producing_` is out of range.
     */
    void ReplaceSlots(SlotList slots);
    /** @brief Writes `slot` at `index` (growing `slots_` if `index >= slots_.size()`). */
    void SetSlot(size_t index, Slot slot);
    /** @brief Removes and returns the final slot in `slots_`, clamping `producing_` if needed. */
    Slot PopLastSlot();
    /** @brief Shrinks `slots_` to at most `count` entries, clamping `producing_` if needed. */
    void TruncateSlots(size_t count);
    /** @brief Sorts `slots_` ascending by `Slot::price`, keeping `producing_` on the same slot. */
    void SortSlotsByPrice();

    /** @brief Returns the currently selected slot index (`0..3`), or `kNone` when idle. */
    int ProducingSlot() const { return producing_; }
    /** @brief Directly sets the selected production slot index (`kNone` or `0..SlotCount()-1`). */
    void SetProducingSlot(int slot_idx);
    /** @brief Returns turns remaining until the active unit finishes building. */
    int Countdown() const { return countdown_; }
    /** @brief Directly sets the production countdown turns remaining. */
    void SetCountdown(int countdown) { countdown_ = countdown; }
    /**
     * @brief Selects `slot_idx` (`kNone` to halt), resetting `countdown_` to the slot's build time
     * and clearing vectoring when halted.
     */
    void SelectProduction(int slot_idx);
    /** @brief Halts active production (`producing_ = kNone`, `countdown_ = 0`) and clears vector.
     */
    void StopProduction();
    /**
     * @brief Decrements `countdown_` by one turn when positive.
     * @return True when `countdown_` transitions from `1` to `0` (unit ready to spawn).
     */
    bool TickCountdown();
    /** @brief True when intact, a valid slot is selected, and `countdown_ > 0`. */
    bool IsProducing() const;
    /** @brief Returns a pointer to the selected `Slot` when intact, or `nullptr` if none. */
    const Slot* ActiveSlot() const;

    /** @brief Returns the current vector destination (`std::monostate`, `CityId`, or
     * `PlantedStandard`). */
    const VectorTarget& VectorDestination() const { return vector_target_; }
    /** @brief Sets the vector destination directly (normalizing invalid `CityId` to none). */
    void SetVectorDestination(VectorTarget destination);
    /** @brief Vectors newly produced armies to `target_city` (`CityId::None()` clears vector). */
    void VectorToCity(CityId target_city);
    /** @brief Vectors newly produced armies to the owning side's planted standard. */
    void VectorToStandard() { vector_target_ = PlantedStandard{}; }
    /** @brief Cancels any active production vectoring. */
    void ClearVector() { vector_target_ = std::monostate{}; }
    /** @brief True when vectoring to either another city or a planted standard. */
    bool IsVectored() const { return !std::holds_alternative<std::monostate>(vector_target_); }
    /** @brief Returns the destination `CityId` when vectored to a city, else `CityId::None()`. */
    CityId VectoredCityId() const;
    /** @brief True when vectoring to the owning side's `PlantedStandard`. */
    bool IsVectoredToStandard() const {
        return std::holds_alternative<PlantedStandard>(vector_target_);
    }

  private:
    CityId id_ = CityId(0);          ///< Position in `Map::cities`.
    TilePos pos_{0, 0};              ///< Top-left tile coordinate of the 2x2 footprint.
    std::string name_;               ///< Display name.
    int income_ = 0;                 ///< Gold earned per turn for its owner.
    ProduceList produces_;           ///< Scenario army type indices prior to slot generation.
    int defence_ = 0;                ///< Fortification defence bonus (`0..2`).
    SideId owner_ = SideId::None();  ///< Holding faction, or `SideId::None()` when neutral/razed.
    SlotList slots_;                 ///< Available unit production slots (`0..4`).
    int producing_ = kNone;          ///< Active slot index in `slots_`, or `kNone`.
    int countdown_ = 0;              ///< Turns left until the active unit completes.
    VectorTarget vector_target_{};   ///< Destination for newly built units.
    bool razed_ = false;             ///< True once permanently burnt to ruins.
    SideId razed_by_ = SideId::None();  ///< Faction whose ruin graphic is shown.
    SideId claim_ = SideId::None();     ///< AI territorial claim or prior owner before capture.
    int attacked_mask_ = 0;             ///< Bitmask (`1 << side`) of factions that assaulted it.
};

}  // namespace w2
