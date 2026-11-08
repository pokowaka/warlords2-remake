/**
 * @file
 * @brief Ruins, temples and sages: what they hold, and what searching them does.
 *
 * See docs/rules.md > Ruins, temples and sages. The contents are rolled once
 * at game start (the original's setup_random_sites); searching is the
 * original's site_search.
 */
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct QuestResult;

/** @brief What searching a site found (site.search in the reference). */
struct SearchResult {
    Site* site = nullptr;  ///< The site searched.
    /// "temple", "no hero", "sage", "killed", "item", "gold", "allies" or "empty".
    std::string kind;
    Army* hero = nullptr;                ///< The hero who searched, if the stack had one.
    int blessed = 0;                     ///< How many armies a temple blessed.
    std::shared_ptr<Quest> assigned;     ///< A temple's new quest.
    std::shared_ptr<QuestResult> quest;  ///< What finding an item did for a quest.
    const Monster* monster = nullptr;    ///< What killed the hero.
    const Monster* guardian = nullptr;   ///< What the hero beat.
    Item* item = nullptr;                ///< The item found.
    int gold = 0;                        ///< The gold found.
    std::vector<Army*> armies;           ///< The allies that joined, as many as had room.
    const ArmyType* type = nullptr;      ///< The allies' army type.
    int count = 0;                       ///< How many allies came, room or not.
};

namespace site {

/// What a site holds (Site::content).
constexpr int kEmpty = 0, kTemple = 1, kItem = 2, kSage = 3, kGold = 4, kAllies = 5;
/// Display names of the contents, by content value.
extern const char* const kContentNames[6];

constexpr int kCapitalRange = 15;    ///< "A capital within 15 tiles" makes a site near.
constexpr int kRichShare = 3;        ///< sites * 3 / 10 are rich.
constexpr int kBlessingTemples = 4;  ///< Only the first four temples can bless.
constexpr int kSageRange = 35;       ///< How far from the hero a sage can tell of a site.

/**
 * @brief Whether an item may only be hidden in a rich ruin (the original's
 * item_reserved): flight, double move, the standards, and command 2 or more.
 */
bool ItemReserved(int type, int value);
/** @brief ItemReserved() for an item record. */
inline bool ItemReserved(const Item& item) {
    return ItemReserved(item.type, item.value);
}
/** @brief ItemReserved() for an entry of the scenario's .ITM pool. */
inline bool ItemReserved(const PoolItem& item) {
    return ItemReserved(item.type, item.value);
}

/**
 * @brief Refills item records 8..21 from the scenario's .ITM pool (see
 * docs/formats/itm.md).
 * @param reserved How many of the records, from 8 up, get reserved items.
 */
void FillItemPool(Game& game, int reserved);
/**
 * @brief Marks `sites * 3 / 10` non-temple sites rich (the original's
 * mark_rich_sites); with quests on, rich sites start hidden from every side.
 */
void MarkRich(Game& game);
/** @brief Rolls what every site holds (the original's setup_random_sites). */
void Setup(Game& game);
/**
 * @brief Whether the hero survives the ruin's guardian: 3 more in 100 for
 * every army on its tile, whoever's.
 */
bool SurvivesGuardian(Game& game, const Army& hero, int monster_strength);
/** @brief The strength of a site's guardian, from the scenario's monster table. */
int GuardianStrength(const Game& game, const Site& site);
/**
 * @brief Blesses a stack at a temple: +1 strength (max 9), once per temple
 * per army; only the first four temples can bless.
 * @return How many armies were blessed.
 */
int Bless(Game& game, const Site& site, const std::vector<Army*>& stack);
/**
 * @brief Searches the site under a stack (the original's site_search).
 *
 * The stack's first army needs a move left. With no hero only a temple does
 * anything: it blesses at once. With one, a human chooses at a temple; a
 * found item is left on the ground for Take to pick up.
 * @return What was found, or nullptr when there is nothing to search.
 */
std::shared_ptr<SearchResult> Search(Game& game, const std::vector<Army*>& stack, int x, int y,
                                     bool human = false);

/** @brief Whether a side has been shown the site (its bit in Site::revealed). */
bool ShownTo(const Site& site, const Side& side);
/** @brief One thing a sage can tell a side about. */
struct SageEntry {
    std::string kind;      ///< "gold", "allies", "item".
    std::string name;      ///< What the sage's list shows.
    Item* item = nullptr;  ///< The item, for an "item" entry.
};
/**
 * @brief What a sage can tell a side, for its hero at (hero_x, hero_y):
 * unshown, unsearched rich sites within kSageRange; gold and allies once
 * each, every item.
 */
std::vector<SageEntry> SageList(Game& game, const Side& side, int hero_x, int hero_y);
/**
 * @brief Shows the side where an entry of SageList() lies: the item's site,
 * or the nearest such gold or allies site.
 * @return The site shown, or nullptr when there is none.
 */
Site* SageShow(Game& game, const Side& side, const SageEntry& entry, int hero_x, int hero_y);
/** @brief The sage's gem: 3d500 + 500 gold for the side. @return The gold given. */
int SageGem(Game& game, Side& side);
/**
 * @brief Uncovers a patch of the map round the tile pointed at.
 * @return {x, y, w, h} of the patch, in tiles.
 */
std::array<int, 4> SageMap(Game& game, const Side& side, int target_x, int target_y);

}  // namespace site
}  // namespace w2
