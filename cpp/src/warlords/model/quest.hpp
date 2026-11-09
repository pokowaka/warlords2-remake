/**
 * @file
 * @brief Encapsulated hero temple quest value/entity (`w2::Quest`) with type-safe `std::variant`
 * targets.
 */
#pragma once

#include <variant>

#include "warlords/model/ids.hpp"

namespace w2 {

class Army;
struct ArmyType;
class City;
class Game;
struct Item;
class Side;

/**
 * @brief Active hero quest assigned at a temple, tracking the questing `ArmyId`, typed `Target`,
 * and completion progress.
 *
 * Encodes the quest objective in a `Target` `std::variant` over strong entity
 * identifiers (`CityId`, `SideId`, `ItemId`, `ArmyTypeId`, `ArmyId`, or
 * `std::monostate` for count-only pillage quests) and resolves live domain
 * entity pointers on demand against `Game`.
 *
 * Invariants:
 * - `HasTarget()` holds iff `type_ == kPillageGold` with `std::monostate`, or
 *   `target_` holds a strong ID satisfying `IsValid()`.
 */
class Quest {
  public:
    /** @brief Discriminated union of possible quest target identifiers. */
    using Target = std::variant<std::monostate, CityId, SideId, ItemId, ArmyTypeId, ArmyId>;

    static constexpr int kSlayHero = 0;      ///< Slay a specific enemy hero (`ArmyId`).
    static constexpr int kRetrieveItem = 1;  ///< Retrieve a hidden magic item (`ItemId`).
    static constexpr int kSlayType = 2;      ///< Slay an army of a magical type (`ArmyTypeId`).
    static constexpr int kSlaughter = 3;     ///< Slay `required_` armies of a side (`SideId`).
    static constexpr int kOccupy = 4;        ///< Capture and occupy a target city (`CityId`).
    static constexpr int kRaze = 5;          ///< Capture and raze a target city (`CityId`).
    static constexpr int kPillageGold = 6;   ///< Pillage/sack `required_` gold (`std::monostate`).

    /** @brief Constructs an empty/unset quest. */
    Quest() = default;

    /**
     * @brief Constructs a quest assigned to `hero` with `target` and optional counting thresholds.
     * @param type Quest kind (`0..6`).
     * @param hero Stable `ArmyId` of the questing hero.
     * @param target Typed target entity identifier (or `std::monostate{}` for `kPillageGold`).
     * @param required Target count/gold for counting quests, or `kNone`.
     * @param done Initial progress toward `required`.
     */
    Quest(int type, ArmyId hero, Target target, int required = kNone, int done = 0);

    /** @brief Quest type code (`0..6`). */
    int Type() const { return type_; }
    /** @brief Sets the quest type code (`0..6`). */
    void SetType(int type) { type_ = type; }

    /** @brief Stable `ArmyId` of the hero assigned to this quest. */
    ArmyId HeroId() const { return hero_; }
    /** @brief Sets the stable `ArmyId` of the hero assigned to this quest. */
    void SetHeroId(ArmyId hero_id) { hero_ = hero_id; }

    /** @brief Returns the `std::variant` target value. */
    const Target& GetTarget() const { return target_; }
    /** @brief Sets the `std::variant` target value. */
    void SetTarget(Target target) { target_ = target; }

    /** @brief Required count/gold for counting quests (`kSlaughter`, `kPillageGold`), or `kNone`.
     */
    int Required() const { return required_; }
    /** @brief Sets the required count/gold threshold. */
    void SetRequired(int required) { required_ = required; }

    /** @brief Accumulated count/gold progress toward `Required()`. */
    int Done() const { return done_; }
    /** @brief Sets accumulated progress directly. */
    void SetDone(int done) { done_ = done; }
    /** @brief Increments `done_` by `delta`. */
    void AddProgress(int delta);

    /** @brief True when this quest has a valid target (or is a targetless `kPillageGold` quest). */
    bool HasTarget() const;

    /** @brief Returns the target `CityId` if held, else `CityId::None()`. */
    CityId TargetCityId() const;
    /** @brief Returns the target `SideId` if held, else `SideId::None()`. */
    SideId TargetSideId() const;
    /** @brief Returns the target `ItemId` if held, else `ItemId::None()`. */
    ItemId TargetItemId() const;
    /** @brief Returns the target `ArmyTypeId` if held, else `ArmyTypeId::None()`. */
    ArmyTypeId TargetArmyTypeId() const;
    /** @brief Returns the target `ArmyId` if held, else `ArmyId::None()`. */
    ArmyId TargetArmyId() const;

    /** @brief Resolves `HeroId()` in `game`'s army pool (alive or fallen), or `nullptr`. */
    Army* ResolveHero(Game& game) const;
    /** @brief Resolves `HeroId()` in `game`'s army pool (const), or `nullptr`. */
    const Army* ResolveHero(const Game& game) const;

    /** @brief Resolves a `CityId` target on `game.map`, or `nullptr`. */
    City* ResolveCity(Game& game) const;
    /** @brief Resolves a `CityId` target on `game.map` (const), or `nullptr`. */
    const City* ResolveCity(const Game& game) const;

    /** @brief Resolves a `SideId` target on `game.map`, or `nullptr`. */
    Side* ResolveSide(Game& game) const;
    /** @brief Resolves a `SideId` target on `game.map` (const), or `nullptr`. */
    const Side* ResolveSide(const Game& game) const;

    /** @brief Resolves an `ItemId` target on `game.map`, or `nullptr`. */
    Item* ResolveItem(Game& game) const;
    /** @brief Resolves an `ItemId` target on `game.map` (const), or `nullptr`. */
    const Item* ResolveItem(const Game& game) const;

    /** @brief Resolves an `ArmyTypeId` target in `game.types`, or `nullptr`. */
    const ArmyType* ResolveArmyType(const Game& game) const;

    /** @brief Resolves an `ArmyId` target in `game`'s army pool, or `nullptr`. */
    Army* ResolveTargetArmy(Game& game) const;
    /** @brief Resolves an `ArmyId` target in `game`'s army pool (const), or `nullptr`. */
    const Army* ResolveTargetArmy(const Game& game) const;

  private:
    int type_ = 0;                  ///< Quest type (`0..6`).
    ArmyId hero_ = ArmyId::None();  ///< Hero undertaking the quest.
    Target target_{};               ///< Discriminated strong-ID target.
    int required_ = kNone;          ///< Count/gold threshold, or `kNone`.
    int done_ = 0;                  ///< Progress accumulated so far.
};

}  // namespace w2
