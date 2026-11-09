/**
 * @file
 * @brief `SearchResult`, `SiteExplorer` domain service, and `w2::site` wrappers.
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

constexpr int kEmpty = 0, kTemple = 1, kItem = 2, kSage = 3, kGold = 4, kAllies = 5;
extern const char* const kContentNames[6];

constexpr int kCapitalRange = 15;
constexpr int kRichShare = 3;
constexpr int kBlessingTemples = 4;
constexpr int kSageRange = 35;

bool ItemReserved(int type, int value);
inline bool ItemReserved(const Item& item) {
    return ItemReserved(item.type, item.value);
}
inline bool ItemReserved(const PoolItem& item) {
    return ItemReserved(item.type, item.value);
}

bool ShownTo(const Site& site, const Side& side);

/** @brief One thing a sage can tell a side about. */
struct SageEntry {
    std::string kind;      ///< "gold", "allies", "item".
    std::string name;      ///< What the sage's list shows.
    Item* item = nullptr;  ///< The item, for an "item" entry.
};

void FillItemPool(Game& game, int reserved);
void MarkRich(Game& game);
void Setup(Game& game);
bool SurvivesGuardian(Game& game, const Army& hero, int monster_strength);
int GuardianStrength(const Game& game, const Site& site);
int Bless(Game& game, const Site& site, const std::vector<Army*>& stack);
std::shared_ptr<SearchResult> Search(Game& game, const std::vector<Army*>& stack, int x, int y,
                                     bool human = false);
std::vector<SageEntry> SageList(Game& game, const Side& side, int hero_x, int hero_y);
Site* SageShow(Game& game, const Side& side, const SageEntry& entry, int hero_x, int hero_y);
int SageGem(Game& game, Side& side);
std::array<int, 4> SageMap(Game& game, const Side& side, int target_x, int target_y);

}  // namespace site

/**
 * @brief Domain service governing site content generation, temple blessings, ruin guardian
 * duels, treasure/ally rewards, and sage lore revelations.
 */
class SiteExplorer {
  public:
    /** @brief Ordered list of army pointers forming a searching stack. */
    using ArmyStack = std::vector<Army*>;
    /** @brief Rectangular map region `{x, y, width, height}` revealed by a sage map. */
    using MapPatch = std::array<int, 4>;

    /** @brief Binds a site-exploration service to `game` (must outlive this service instance). */
    explicit SiteExplorer(Game& game) : game_(game) {}

    void FillItemPool(int reserved);
    void MarkRich();
    void Setup();
    bool SurvivesGuardian(const Army& hero, int monster_strength);
    int GuardianStrength(const Site& site) const;
    int Bless(const Site& site, const ArmyStack& stack);
    std::shared_ptr<SearchResult> Search(const ArmyStack& stack, int x, int y, bool human = false);
    std::vector<site::SageEntry> SageList(const Side& side, int hero_x, int hero_y) const;
    Site* SageShow(const Side& side, const site::SageEntry& entry, int hero_x, int hero_y);
    int SageGem(Side& side);
    MapPatch SageMap(const Side& side, int target_x, int target_y);

    /** @brief Formats a single-line prose summary of `result`. */
    static std::string DescribeSearch(const SearchResult* result);

  private:
    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
