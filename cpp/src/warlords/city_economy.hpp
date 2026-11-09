/**
 * @file
 * @brief `CityEconomy` domain service managing city production, road transit, vectoring,
 * slot purchases, treasury upkeep/income, and post-capture spoils.
 */
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct QuestResult;

namespace game {

constexpr int kMaxVectoredTo = 4;  ///< No more than four cities may send to one place.

/** @brief What pillaging or sacking took: the gold and the types lost. */
struct Spoils {
    int gold = 0;                           ///< Gold taken in all.
    std::vector<std::pair<int, int>> lost;  ///< Each type lost: type, gold.
};

}  // namespace game

/**
 * @brief Domain service encapsulating treasury income and army upkeep, road-transit delivery,
 * city unit production, standard planting, vectoring, slot purchasing, and city spoils.
 */
class CityEconomy {
  public:
    /** @brief Ordered list of army pointers forming a stack. */
    using ArmyStack = std::vector<Army*>;
    /** @brief Tile coordinate pair `{x, y}` (`{kNone, kNone}` when absent). */
    using CoordPair = std::pair<int, int>;
    /** @brief List of buyable army type descriptors. */
    using BuyableTypeList = std::vector<const ArmyType*>;

    /** @brief Binds a city-economy service to `game` (must outlive this service instance). */
    explicit CityEconomy(Game& game) : game_(game) {}

    int Income(const Side& side) const;
    int Upkeep(const Side& side) const;
    void ApplyIncome(Side& side);

    /**
     * @brief Delivers road-transit armies and ticks city build countdowns for `side`,
     * recording `TurnNews(SideId(side.index)).produced`.
     * @return Newly built entries (excluding road arrivals) so human turns can restart slots.
     */
    std::vector<Produced> RunProduction(Side& side);

    bool Producing(const City& city) const;
    void SetProduction(City& city, int slot_index);
    bool SetCityProduction(const Side* who, City& city, int slot);
    void NeutralProduction(const Side* who);

    std::vector<City*> VectoredTo(const City* dest, const Side* side = nullptr) const;
    CoordPair StandardAt(const Side& side) const;
    bool PlantFlag(Side& side, Army* hero);
    bool VectorToStandard(City& city, const Side& side);
    bool Vector(City& city, City* dest_city);

    City* NearestCity(int x, int y, const Side* side = nullptr, const Side* seer = nullptr) const;

    BuyableTypeList BuyableTypes() const;
    std::string CannotBuy(const Side& side, const City& city, const ArmyType& army_type) const;
    int BuySlot(const City& city) const;
    void BuyProduction(Side& side, City& city, int slot_index, int type_id);
    static void SortProduction(City& city);
    static void RenameCity(City& city, const std::string& name);

    std::shared_ptr<QuestResult> Occupy(Side& side, City& city, const ArmyStack& stack);
    game::Spoils Pillage(Side& side, City& city, const ArmyStack& stack);
    game::Spoils Sack(Side& side, City& city, const ArmyStack& stack);
    void Raze(Side& side, City& city, const ArmyStack& stack, bool own_city = false);
    void MakeRuins(City& city);

  private:
    bool Deliver(Army* army, City& city);
    void ProduceNeutralGarrison(City& city, const Side* who);
    int SlotPillageValue(const Slot& slot) const;
    static void RecomputeCityAfterSpoils(City& city);

    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
