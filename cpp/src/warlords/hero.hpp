/**
 * @file
 * @brief `HeroOffer` model, `HeroSystem` domain service, and `w2::hero` wrappers.
 */
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct Battle;

/** @brief A hero offering itself to a side, at a price, in one of its cities. */
struct HeroOffer {
    int price = 0;         ///< Gold the hero asks; 0 for the first.
    City* city = nullptr;  ///< The city the hero appears in.
    bool first = false;    ///< The free turn-1 hero, who carries the side's standard.
    std::string name;      ///< The hero's name, drawn from the side's candidates.
    bool female = false;   ///< Whether the hero is a heroine.
};

namespace hero {

constexpr int kMaxInGame = 40;      ///< Heroes in the whole game.
constexpr int kMaxPerSide = 5;      ///< Heroes per side; kMaxPerBigSide for a big side.
constexpr int kMaxPerBigSide = 6;   ///< Heroes per side owning kBigSideCities cities.
constexpr int kBigSideCities = 40;  ///< Cities that make a side big.
constexpr int kStartStrength = 5;   ///< A new hero's strength.
constexpr int kStartMoves = 14;     ///< A new hero's moves.
constexpr int kMaxStrength = 9;     ///< The strength promotion stops at.
constexpr int kPromotionMoves = 2;  ///< Added to the maximum move on each promotion.
constexpr int kNameRoll = 100;      ///< Candidate hero names rolled per `HERONAM<side>.DAT`.

/** @brief What the levels are called, by level, 1-based as the game numbers them. */
const char* LevelName(int level);
/** @brief Experience needed to leave a level, or kNone at the top. */
int PromotionAt(int level);

/** @brief One ally army, as the original builds it: moves full, no upkeep, home city 0. */
Army NewAlly(const ArmyType* type, int x, int y, int owner);

/** @brief A hired hero, and the allies it brought. */
struct Recruited {
    Army* hero = nullptr;            ///< The hired hero.
    std::vector<Army*> allies;       ///< The allies that found room.
    const ArmyType* type = nullptr;  ///< The allies' type, or null with none.
    int count = 0;                   ///< How many allies came, room or not.
};

const std::vector<std::pair<std::string, bool>>& Names(Game& game, const Side& side);
std::pair<std::string, bool> RollName(Game& game, const Side& side);
std::shared_ptr<HeroOffer> Offer(Game& game, Side& side);
const ArmyType* MagicalType(Game& game);
std::pair<const ArmyType*, int> Allies(Game& game);
Army* PlaceAlly(Game& game, const ArmyType* type, int x, int y, const Side& side);
Recruited Recruit(Game& game, Side& side, const HeroOffer& offer);
void AddExperience(Game& game, Army* hero, int amount);
std::vector<Promotion> CheckPromotions(Game& game, const Side& side);
void BattleExperience(Game& game, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city);
std::vector<Item*> ItemsHere(Game& game, const Army* hero);
void DropItem(Game& game, Army* hero, Item* item);
void TakeItem(Game& game, Army* hero, Item* item);
std::vector<Item*> DropItems(Game& game, Army* hero, int x, int y, int current_side = kNone);

}  // namespace hero

/**
 * @brief Domain service managing hero candidate lists, turn-start offers, recruitment,
 * magical escorts, experience, level promotions, and item inventory transfers.
 *
 * Consults `side.ComputerPlayer()->ChooseHeroCity(...)` when placing computer heroes
 * so hero rules remain independent of `//cpp:ai`.
 */
class HeroSystem {
  public:
    /** @brief Candidate hero name and heroine flag `{name, female}`. */
    using CandidateName = std::pair<std::string, bool>;
    /** @brief Per-side list of candidate hero names loaded from `HERONAM<side>.DAT`. */
    using CandidateList = std::vector<CandidateName>;
    /** @brief Magical ally army type and rolled escort count. */
    using AllyRoll = std::pair<const ArmyType*, int>;
    /** @brief Ordered list of army pointers. */
    using ArmyStack = std::vector<Army*>;

    /** @brief Binds a hero service to `game` (must outlive this service instance). */
    explicit HeroSystem(Game& game) : game_(game) {}

    const CandidateList& Names(const Side& side);
    CandidateName RollName(const Side& side);
    std::shared_ptr<HeroOffer> Offer(Side& side);
    const ArmyType* MagicalType();
    AllyRoll Allies();
    Army* PlaceAlly(const ArmyType* type, int x, int y, const Side& side);
    hero::Recruited Recruit(Side& side, const HeroOffer& offer);
    void AddExperience(Army* hero, int amount);
    std::vector<Promotion> CheckPromotions(const Side& side);
    void BattleExperience(const ArmyStack& attackers, const ArmyStack& defenders,
                          const Battle& result, bool was_city);
    std::vector<Item*> ItemsHere(const Army* hero);
    void DropItem(Army* hero, Item* item);
    void TakeItem(Army* hero, Item* item);
    std::vector<Item*> DropItems(Army* hero, int x, int y, int current_side = kNone);

  private:
    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
