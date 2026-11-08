// Heroes: offers, recruitment, experience, promotion and death.
//
// docs/rules.md > Heroes. This is where the first of the original's bugs
// shows up: see battleExperience and rules::bugs.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct Battle;

/** A hero offering itself to a side (hero_offer_check). */
struct HeroOffer {
    int price = 0;
    City* city = nullptr;
    bool first = false;
    std::string name;
    bool female = false;
};

namespace hero {

constexpr int kMaxInGame = 40;  // heroes in the whole game
constexpr int kMaxPerSide = 5;  // 6 once the side owns this many cities
constexpr int kMaxPerBigSide = 6, kBigSideCities = 40;
constexpr int kStartStrength = 5, kStartMoves = 14;
constexpr int kMaxStrength = 9;
constexpr int kPromotionMoves = 2;

/** What the levels are called, by level, 1-based as the game numbers them. */
const char* LevelName(int level);
/** Experience needed to leave a level, or NONE at the top. */
int PromotionAt(int level);

// Each side has its own hundred candidate heroes in TERRAIN0/HERONAM<side>.DAT,
// "#0 Sir Nick" a line. load_hero_name (6563:0c67) rolls dice(1, 100, 0); the
// count is hard-coded, so HERONAM4.DAT's 101st hero is never drawn.
constexpr int kNameRoll = 100;

/** The candidate list for a side, as {name, female}. Cached. */
const std::vector<std::pair<std::string, bool>>& Names(Game& g, const Side& side);
/** Roll the name and sex of a hero offering itself to `side`. */
std::pair<std::string, bool> RollName(Game& g, const Side& side);
/** Does a hero offer itself to `side` this turn, and at what price? None when
 *  none does (hero_offer_check, 7563:0000). */
std::shared_ptr<HeroOffer> Offer(Game& g, Side& side);
/** A random magical army type (random_magical_type, 7563:1a9b): the types
 *  are counted in id order, 0-27; Dragons (25) when there is none. */
const ArmyType* MagicalType(Game& g);
/** The allies a hired hero brings (hero_brings_allies, 7563:01fc): 1-3 of one
 *  random magical type. */
std::pair<const ArmyType*, int> Allies(Game& g);
/** One ally army, as create_ally_army (6536:12e6) builds it: moves full, no
 *  upkeep, home city 0. */
Army NewAlly(const ArmyType* type, int x, int y, int owner);
/** Put an ally of the side's near (x, y) (create_ally_army, 6536:12e6): on
 *  the tile if it has room and no other side's army, else a random step
 *  from there and try again, ten tries in all. Null when none of the tries
 *  had room -- the ally is lost. */
Army* PlaceAlly(Game& g, const ArmyType* type, int x, int y, const Side& side);
/** A hired hero, and the allies it brought: their type, how many came, and
 *  those that found room. */
struct Recruited {
    Army* hero = nullptr;
    std::vector<Army*> allies;
    const ArmyType* type = nullptr;
    int count = 0;
};
/** Hire the offered hero (hero_recruit, 7563:031b). The hero stands on the
 *  city's first tile with room, or on a free tile round it -- or, with none,
 *  on the last one tried. Its allies stand with it. */
Recruited Recruit(Game& g, Side& side, const HeroOffer& off);
/** Give a hero experience, capped at 60. */
void AddExperience(Game& g, Army* h, int n);
/** Promote a side's heroes, one step each (hero_check_promotions, 7563:0579;
 *  hero_promote, 7563:0672): +1 strength (9 at most) and +2 to the maximum
 *  move. Returns each hero and the strength it really gained. */
std::vector<Promotion> CheckPromotions(Game& g, const Side& side);
/** Award experience after a battle. **Original bug**: a surviving defending
 *  hero is credited only when the attacker at the same place in the line is
 *  a hero (67cc:0c8e). */
void BattleExperience(Game& g, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city);
/** What the hero info dialog lists for a hero (796c:03d9): the items it
 *  carries, then those lying on its tile, each in item order. */
std::vector<Item*> ItemsHere(Game& g, const Army* h);
/** Put an item down on the hero's tile (7563:0943) -- or lose it at sea. */
void DropItem(Game& g, Army* h, Item* it);
/** Pick an item up off the hero's tile (7563:08c7). */
void TakeItem(Game& g, Army* h, Item* it);
/** A dead hero drops everything it carried where it fell -- lost on water
 *  (hero_drop_items, 67cc:16cd). When the item is the current player's
 *  standard (item number = side), that side's cities stop vectoring to it.
 *  `cur` is the current player, g.side's when NONE. */
std::vector<Item*> DropItems(Game& g, Army* h, int x, int y, int cur = kNone);

}  // namespace hero
}  // namespace w2
