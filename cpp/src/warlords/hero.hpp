// Heroes: offers, recruitment, experience, promotion and death.
//
// See docs/rules.md > Heroes. This is where the first of the original's bugs
// shows up: see BattleExperience() and rules::bugs.
#pragma once

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

/** @brief What the levels are called, by level, 1-based as the game numbers them. */
const char* LevelName(int level);
/** @brief Experience needed to leave a level, or kNone at the top. */
int PromotionAt(int level);

/// Each side has its own hundred candidate heroes in TERRAIN0/HERONAM<side>.DAT,
/// "#0 Sir Nick" a line. The name is rolled with dice(1, 100, 0); the count is
/// hard-coded, so HERONAM4.DAT's 101st hero is never drawn.
constexpr int kNameRoll = 100;

/** @brief The candidate list for a side, as {name, female}. Cached. */
const std::vector<std::pair<std::string, bool>>& Names(Game& game, const Side& side);
/** @brief Rolls the name and sex of a hero offering itself to `side`. */
std::pair<std::string, bool> RollName(Game& game, const Side& side);
/**
 * @brief Does a hero offer itself to `side` this turn, and at what price?
 * @return The offer, or null when none does.
 */
std::shared_ptr<HeroOffer> Offer(Game& game, Side& side);
/**
 * @brief A random magical army type.
 *
 * The magical types are counted in id order, 0-27; Dragons (25) when there
 * is none.
 */
const ArmyType* MagicalType(Game& game);
/** @brief The allies a hired hero brings: 1-3 of one random magical type. */
std::pair<const ArmyType*, int> Allies(Game& game);
/** @brief One ally army, as the original builds it: moves full, no upkeep, home city 0. */
Army NewAlly(const ArmyType* type, int x, int y, int owner);
/**
 * @brief Puts an ally of the side's near (x, y).
 *
 * On the tile if it has room and no other side's army, else a random step
 * from there and try again, ten tries in all.
 * @return The ally, or null when none of the tries had room -- the ally is
 *         lost.
 */
Army* PlaceAlly(Game& game, const ArmyType* type, int x, int y, const Side& side);
/**
 * @brief A hired hero, and the allies it brought: their type, how many came,
 *        and those that found room.
 */
struct Recruited {
    Army* hero = nullptr;            ///< The hired hero.
    std::vector<Army*> allies;       ///< The allies that found room.
    const ArmyType* type = nullptr;  ///< The allies' type, or null with none.
    int count = 0;                   ///< How many allies came, room or not.
};
/**
 * @brief Hires the offered hero.
 *
 * The hero stands on the city's first tile with room, or on a free tile round
 * it -- or, with none, on the last one tried. Its allies stand with it.
 */
Recruited Recruit(Game& game, Side& side, const HeroOffer& offer);
/** @brief Gives a hero experience, capped at rules::kMaxHeroXp. */
void AddExperience(Game& game, Army* hero, int amount);
/**
 * @brief Promotes a side's heroes, one step each.
 *
 * Each promotion is +1 strength (kMaxStrength at most) and +2 to the maximum
 * move.
 * @return Each hero and the strength it really gained.
 */
std::vector<Promotion> CheckPromotions(Game& game, const Side& side);
/**
 * @brief Awards experience after a battle.
 *
 * **Original bug**: a surviving defending hero is credited only when the
 * attacker at the same place in the line is a hero
 * (rules::bugs.hero_experience_reads_attacker_types).
 */
void BattleExperience(Game& game, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city);
/**
 * @brief What the hero info dialog lists for a hero: the items it carries,
 *        then those lying on its tile, each in item order.
 */
std::vector<Item*> ItemsHere(Game& game, const Army* hero);
/** @brief Puts an item down on the hero's tile -- or loses it at sea. */
void DropItem(Game& game, Army* hero, Item* item);
/** @brief Picks an item up off the hero's tile. */
void TakeItem(Game& game, Army* hero, Item* item);
/**
 * @brief A dead hero drops everything it carried where it fell -- lost on
 *        water.
 *
 * When an item is the current player's standard (item number = side), that
 * side's cities stop vectoring to it.
 * @param current_side The current player, game.side's when kNone.
 * @return The items left lying on the tile.
 */
std::vector<Item*> DropItems(Game& game, Army* hero, int x, int y, int current_side = kNone);

}  // namespace hero
}  // namespace w2
