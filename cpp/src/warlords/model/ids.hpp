/**
 * @file
 * @brief Lightweight constexpr strong identifier value types for Warlords II domain entities.
 */
#pragma once

#include <compare>
#include <utility>
#include <variant>

namespace w2 {

/// Sentinel index meaning "none / unset" across domain records and coordinates.
constexpr int kNone = -1;

/**
 * @brief Strongly typed value wrapper around an integer entity identifier.
 *
 * Prevents accidental mixing of indices from distinct entity spaces (`SideId`,
 * `CityId`, `ArmyId`, `ArmyTypeId`, `SiteId`, `ItemId`) at compile time with
 * zero runtime overhead. An underlying value `< 0` (`kNone`) denotes an unset
 * or invalid identifier.
 *
 * @tparam Tag Phantom tag type distinguishing each entity space.
 */
template <typename Tag>
class StrongId {
  public:
    /** @brief Constructs an unset (`kNone`) identifier. */
    constexpr StrongId() = default;

    /**
     * @brief Wraps raw integer index `raw_value`.
     * @param raw_value Non-negative index or `kNone` (`-1`).
     */
    explicit constexpr StrongId(int raw_value) : value_(raw_value) {}

    /** @brief Returns the canonical invalid (`kNone`) identifier. */
    static constexpr StrongId None() { return StrongId(kNone); }

    /** @brief Returns the underlying integer index (`kNone` when unset). */
    constexpr int Value() const { return value_; }

    /** @brief True when this identifier holds a valid non-negative index. */
    constexpr bool IsValid() const { return value_ >= 0; }

    /** @brief Equality comparison on the underlying index. */
    constexpr bool operator==(const StrongId& other) const = default;

    /** @brief Three-way comparison ordering identifiers by underlying index. */
    constexpr auto operator<=>(const StrongId& other) const = default;

    /** @brief Abseil hash integration for `absl::flat_hash_map` and `absl::flat_hash_set`. */
    template <typename HashState>
    friend HashState AbslHashValue(HashState hash_state, StrongId identifier) {
        return HashState::combine(std::move(hash_state), identifier.value_);
    }

  private:
    int value_ = kNone;  ///< Underlying integer index, or `kNone` when unset.
};

/** @brief Tag distinguishing `SideId` (`0..7`). */
struct SideIdTag {};
/** @brief Tag distinguishing `CityId` (`0..cities.size()-1`). */
struct CityIdTag {};
/** @brief Tag distinguishing `ArmyId` (unique monotonic handle in `ArmyRegistry`). */
struct ArmyIdTag {};
/** @brief Tag distinguishing `ArmyTypeId` (`0..63` in `ARMYTYPE.DAT`). */
struct ArmyTypeIdTag {};
/** @brief Tag distinguishing `SiteId` (`0..sites.size()-1`). */
struct SiteIdTag {};
/** @brief Tag distinguishing `ItemId` (`0..items.size()-1`). */
struct ItemIdTag {};

/** @brief Strongly typed side / faction identifier (`0..7`, or `SideId::None()`). */
using SideId = StrongId<SideIdTag>;
/** @brief Strongly typed city identifier (`0..cities.size()-1`, or `CityId::None()`). */
using CityId = StrongId<CityIdTag>;
/** @brief Strongly typed stable army handle (`ArmyRegistry` identifier, or `ArmyId::None()`). */
using ArmyId = StrongId<ArmyIdTag>;
/** @brief Strongly typed army catalog type identifier (`0..63`, or `ArmyTypeId::None()`). */
using ArmyTypeId = StrongId<ArmyTypeIdTag>;
/** @brief Strongly typed temple or ruin site identifier (`0..sites.size()-1`, or `SiteId::None()`).
 */
using SiteId = StrongId<SiteIdTag>;
/** @brief Strongly typed magic item or standard identifier (`0..items.size()-1`, or
 * `ItemId::None()`). */
using ItemId = StrongId<ItemIdTag>;

/** @brief Tag value representing a side's planted standard as a city vectoring destination. */
struct PlantedStandard {
    /** @brief Trivial equality for `VectorTarget` comparisons. */
    constexpr bool operator==(const PlantedStandard&) const = default;
    /** @brief Trivial ordering for `VectorTarget` comparisons. */
    constexpr auto operator<=>(const PlantedStandard&) const = default;
};

/** @brief Destination where a city sends newly produced armies: none, a city, or a standard. */
using VectorTarget = std::variant<std::monostate, CityId, PlantedStandard>;

}  // namespace w2
