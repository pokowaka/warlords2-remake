#include "warlords/site.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2::site {

const char* const kContentNames[6] = {"empty",  "temple", "a magic item",
                                      "a sage", "gold",   "allies"};

/**
 * @brief The contents an empty site may roll, by band ("rich", "near",
 * "far"): kSage, kGold or kAllies.
 */
static const std::vector<int>& Content(const std::string& band) {
    static const std::vector<int> kRich = {5, 5, 4}, kFar = {3, 4, 5, 3, 4}, kNear = {3, 4, 5};
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}
/** @brief The army type ids an allies site may hold, by band. */
static const std::vector<int>& AllyTypes(const std::string& band) {
    static const std::vector<int> kRich = {25, 23, 27, 19},  // Dragons, Wizards, Devils, Archons
            kFar = {24, 20, 26},                             // Ghosts, Giant Worms, Demons
            kNear = {22, 20};                                // Elementals, Giant Worms
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}

bool ItemReserved(int type, int value) {
    return type == rules::kItemFlight || type == rules::kItemDoubleMove ||
           type == rules::kItemStandard || (type == rules::kItemCommand && value >= 2);
}

void FillItemPool(Game& game, int reserved) {
    if (!game.map->item_pool) return;
    const auto& pool = *game.map->item_pool;
    std::map<int, Item*> by_index;
    std::map<int, bool> used;
    for (auto& item : game.map->items) by_index[item.index] = &item;
    for (int idx = 8; idx <= 21; idx++) {
        auto entry = by_index.find(idx);
        if (entry == by_index.end()) continue;
        Item* item = entry->second;
        bool want = idx < 8 + reserved;
        std::vector<int> choices;
        for (size_t i = 0; i < pool.size(); i++) {
            if (!used[static_cast<int>(i)] && ItemReserved(pool[i]) == want)
                choices.push_back(static_cast<int>(i));
        }
        int choice = game.rng.PickIndex(choices.size());
        if (choice >= 0) {
            int pick = choices[choice];
            used[pick] = true;
            item->name = pool[pick].name;
            item->type = pool[pick].type;
            item->value = pool[pick].value;
        }
    }
}

void MarkRich(Game& game) {
    std::vector<Site*> ruins;
    for (auto& site : game.map->sites) {
        site.rich = false;
        site.revealed = 0xff;
        if (site.type != kTemple) ruins.push_back(&site);
    }
    int want = static_cast<int>(game.map->sites.size()) * kRichShare / 10;
    game.rng.Shuffle(ruins);
    for (int i = 0; i < std::min(want, static_cast<int>(ruins.size())); i++) {
        ruins[i]->rich = true;
        if (game.map->options.quests != 0) ruins[i]->revealed = 0;
    }
}

/**
 * @brief A site's band: "rich", "near" (an in-play side's capital less than
 * kCapitalRange away, by straight-line map distance) or "far".
 */
static std::string Band(const Site& site, const std::vector<std::pair<int, int>>& capitals) {
    if (site.rich) return "rich";
    for (auto& capital : capitals) {
        double dx = site.x - capital.first, dy = site.y - capital.second;
        if (static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy))) < kCapitalRange)
            return "near";
    }
    return "far";
}

void Setup(Game& game) {
    // the in-play sides' capitals where they stand, city or not
    std::vector<std::pair<int, int>> capitals;
    for (Side* side : game.sides) capitals.emplace_back(side->cap_x, side->cap_y);

    MarkRich(game);
    int temples = 0;
    for (auto& site : game.map->sites) {
        site.content = site.type == kTemple ? kTemple : kEmpty;
        if (site.content == kTemple) {
            site.temple_index = temples;
            temples++;
        }
        site.item = kNone;
        site.guardian = kNone;
        site.ally_type = kNone;
        site.searched = false;
        site.band = Band(site, capitals);
    }

    int n_sites = static_cast<int>(game.map->sites.size());
    int reserved = n_sites * 2 / 10;
    FillItemPool(game, reserved);

    int band_hi = reserved;
    int band_lo = std::min(game.rng.Dice(2, 3, 1), band_hi);
    int last = std::min(22, n_sites / 3 + game.rng.Dice(1, 5, -3) + 8);

    std::vector<Site*> free;
    for (auto& site : game.map->sites)
        if (site.content == kEmpty) free.push_back(&site);
    game.rng.Shuffle(free);

    std::map<int, Item*> by_index;
    for (auto& item : game.map->items) {
        by_index[item.index] = &item;
        item.status = 0;
    }

    for (int idx = 8; idx <= last - 1; idx++) {
        auto entry = by_index.find(idx);
        // the band between band_lo and band_hi is held back, probably for quests
        if (entry != by_index.end() && !(idx >= 8 + band_lo && idx < 8 + band_hi)) {
            Item* item = entry->second;
            bool want = ItemReserved(*item);
            for (size_t i = 0; i < free.size(); i++) {
                Site* site = free[i];
                if (site->rich == want) {
                    site->content = kItem;
                    site->item = idx;
                    item->status = 2;  // hidden in a ruin
                    free.erase(free.begin() + i);
                    break;
                }
            }
        }
    }

    for (auto& site : game.map->sites) {
        if (site.content == kEmpty) site.content = game.rng.Pick(Content(site.band), 0);
        if (site.content == kTemple) {
            site.guardian = kNone;
        } else if (site.content == kAllies) {
            // the allies' type takes the guardian's place, as in the original
            site.ally_type = game.rng.Pick(AllyTypes(site.band), 0);
            site.guardian = kNone;
        } else {
            site.guardian = game.rng.Dice(1, 9, 0);
        }
    }
    game.map->site_at.assign(static_cast<size_t>(game.map->width) * game.map->height, nullptr);
    for (auto& site : game.map->sites) game.map->site_at[site.y * game.map->width + site.x] = &site;
}

/** @brief The first hero in a stack, or nullptr. */
static Army* HeroIn(const std::vector<Army*>& stack) {
    for (Army* army : stack)
        if (army->type == armytype::kHero) return army;
    return nullptr;
}

bool SurvivesGuardian(Game& game, const Army& hero, int monster_strength) {
    int margin = 90 + 5 * (hero.strength + combat::BattleItems(hero) - monster_strength) +
                 3 * static_cast<int>(game::ArmiesAt(game, hero.x, hero.y).size());
    return game.rng.Dice(1, 100, 0) <= margin;
}

int GuardianStrength(const Game& game, const Site& site) {
    if (site.guardian < 0 || site.guardian > 9) return 0;
    const auto& monster = game.map->monsters[site.guardian];
    return monster ? monster->strength : 0;
}

int Bless(Game& game, const Site& site, const std::vector<Army*>& stack) {
    if ((site.temple_index == kNone ? 0 : site.temple_index) >= kBlessingTemples) return 0;
    int bit = site.temple_index;
    int blessed = 0;
    for (Army* army : stack) {
        if (!army->blessings.count(bit)) {
            army->blessings.insert(bit);
            army->strength = std::min(9, army->strength + 1);
            blessed++;
            if (army->type == armytype::kHero) hero::AddExperience(game, army, 1);
        }
    }
    return blessed;
}

/** @brief The scenario's monster with this index (0..9), or nullptr. */
static const Monster* MonsterOf(const Game& game, int index) {
    if (index < 0 || index > 9 || !game.map->monsters[index]) return nullptr;
    return &*game.map->monsters[index];
}

std::shared_ptr<SearchResult> Search(Game& game, const std::vector<Army*>& stack, int x, int y,
                                     bool human) {
    if (game.map->site_at.empty() || x < 0 || y < 0) return nullptr;
    Site* site = game.map->site_at[y * game.map->width + x];
    if (!site || site->searched) return nullptr;
    if (stack.empty() || stack[0]->moves < 1) return nullptr;
    Army* hero = HeroIn(stack);
    auto out = std::make_shared<SearchResult>();
    out->site = site;

    if (site->content == kTemple) {
        out->kind = "temple";
        out->hero = hero;
        if (human && hero) return out;
        out->blessed = Bless(game, *site, stack);
        return out;
    }

    if (!hero) {
        out->kind = "no hero";
        return out;
    }
    Side* side = game.map->Side(hero->owner);
    out->hero = hero;

    if (site->content == kSage) {
        hero::AddExperience(game, hero, 3);
        history::RecordDeed(game, side, DeedType::kFound, history::kFoundSage, 0, hero->name);
        site->searched = true;
        out->kind = "sage";
        return out;
    }

    // a ruin: the original's ruin_search
    hero::AddExperience(game, hero, 3);

    // an allies site holds its allies' type where the guardian would be, and
    // nothing guards it
    const Monster* beaten = nullptr;
    if (site->content != kAllies && site->guardian != kNone && site->guardian > 0) {
        int strength = GuardianStrength(game, *site);
        const Monster* monster = MonsterOf(game, site->guardian);
        if (!SurvivesGuardian(game, *hero, strength)) {
            history::RecordDeed(game, side, DeedType::kHeroKilled, history::kKilledSearching, 0,
                                hero->name);
            history::Tally(game, hero, kNone, false);  // the side's own losses
            hero::DropItems(game, hero, hero->x, hero->y, hero->owner);
            game.Remove(hero);
            out->kind = "killed";
            out->monster = monster;
            return out;
        }
        beaten = monster;
        site->guardian = 0;
    }
    out->guardian = beaten;

    if (site->content == kItem) {
        Item* found = nullptr;
        for (auto& item : game.map->items)
            if (item.index == site->item) found = &item;
        if (found && human) {
            found->status = 1;
            found->x = x;
            found->y = y;  // on the ground, to take
        } else if (found) {
            found->status = 3;  // carried
            hero->items.push_back(found);
        }
        if (found) history::RecordDeed(game, side, DeedType::kFound, found->index, 0, hero->name);
        site->searched = true;
        site->content = kEmpty;
        out->kind = "item";
        out->item = found;
        return out;
    } else if (site->content == kGold) {
        int gold = site->rich ? game.rng.Dice(3, 1000, 1000) : game.rng.Dice(3, 500, 500);
        if (side) side->gold += gold;
        site->searched = true;
        history::RecordDeed(game, side, DeedType::kFound, history::kFoundGold, 0, hero->name);
        out->kind = "gold";
        out->gold = gold;
        return out;
    } else if (site->content == kAllies) {
        site->searched = true;
        const ArmyType* type = game.types.ById(site->ally_type);
        if (!type) type = game.types.ById(armytype::kScouts);
        int count = game.rng.Dice(1, 2, 0);
        if (site->rich) count += 2;
        for (int i = 0; i < count; i++) {
            Army* ally = hero::PlaceAlly(game, type, x, y, *side);
            if (ally) out->armies.push_back(ally);
        }
        history::RecordDeed(game, side, DeedType::kFound, history::kFoundAllies, 0, hero->name);
        out->kind = "allies";
        out->type = type;
        out->count = count;
        return out;
    }
    out->kind = "empty";
    return out;
}

/** @brief Straight-line distance from a site to the hero, rounded down. */
static int SageDistance(const Site& site, int hero_x, int hero_y) {
    double dx = site.x - hero_x, dy = site.y - hero_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

bool ShownTo(const Site& site, const Side& side) {
    return ((site.revealed >> side.index) & 1) == 1;
}

/**
 * @brief Whether a sage may tell the side of this site: rich, unsearched,
 * not yet shown to the side, and within kSageRange of the hero.
 */
static bool Unshown(const Side& side, const Site& site, int hero_x, int hero_y) {
    return site.rich && !site.searched && !ShownTo(site, side) &&
           SageDistance(site, hero_x, hero_y) < kSageRange;
}

std::vector<SageEntry> SageList(Game& game, const Side& side, int hero_x, int hero_y) {
    std::vector<SageEntry> list;
    bool gold = false, allies = false;
    for (auto& site : game.map->sites) {
        if (Unshown(side, site, hero_x, hero_y)) {
            if (site.content == kGold && !gold) {
                gold = true;
                list.push_back(SageEntry{"gold", "Gold", nullptr});
            } else if (site.content == kAllies && !allies) {
                allies = true;
                list.push_back(SageEntry{"allies", "Allies", nullptr});
            } else if (site.content == kItem) {
                for (auto& item : game.map->items) {
                    if (item.index == site.item)
                        list.push_back(SageEntry{"item", item.name, &item});
                }
            }
        }
    }
    return list;
}

Site* SageShow(Game& game, const Side& side, const SageEntry& entry, int hero_x, int hero_y) {
    Site* found = nullptr;
    int best = kNone;
    for (auto& site : game.map->sites) {
        if (entry.kind == "item") {
            if (site.content == kItem && entry.item && site.item == entry.item->index) {
                found = &site;
                break;
            }
        } else if (Unshown(side, site, hero_x, hero_y) &&
                   site.content == (entry.kind == "gold" ? kGold : kAllies)) {
            int distance = SageDistance(site, hero_x, hero_y);
            if (best == kNone || distance < best) {
                found = &site;
                best = distance;
            }
        }
    }
    if (!found) return nullptr;
    if (!ShownTo(*found, side)) found->revealed = found->revealed + (1 << side.index);
    game::Reveal(game, side.index, found->x, found->y, false);
    return found;
}

int SageGem(Game& game, Side& side) {
    int gold = game.rng.Dice(3, 500, 500);
    side.gold += gold;
    return gold;
}

std::array<int, 4> SageMap(Game& game, const Side& side, int target_x, int target_y) {
    int left = std::max(0, target_x - game.rng.Dice(1, 5, 8));
    int top = std::max(0, target_y - game.rng.Dice(1, 5, 8));
    int width = game.rng.Dice(1, 10, 15), height = game.rng.Dice(1, 10, 15);
    if (left + width >= game.map->width) width = game.map->width - left - 1;
    if (top + height >= game.map->height) height = game.map->height - top - 1;
    for (int x = left; x < left + width; x++)
        for (int y = top; y < top + height; y++) game::Reveal(game, side.index, x, y, false);
    return {left, top, width, height};
}

}  // namespace w2::site
