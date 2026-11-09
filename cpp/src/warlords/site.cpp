/**
 * @file
 * @brief Implementation of `SiteExplorer` and `w2::site` wrappers.
 */
#include "warlords/site.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

const std::vector<int>& Content(const std::string& band) {
    static const std::vector<int> kRich = {5, 5, 4}, kFar = {3, 4, 5, 3, 4}, kNear = {3, 4, 5};
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}

const std::vector<int>& AllyTypes(const std::string& band) {
    static const std::vector<int> kRich = {25, 23, 27, 19}, kFar = {24, 20, 26}, kNear = {22, 20};
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}

std::string Band(const Site& target_site, const std::vector<std::pair<int, int>>& capitals) {
    if (target_site.rich) return "rich";
    for (const auto& capital : capitals) {
        double dx = target_site.x - capital.first, dy = target_site.y - capital.second;
        if (static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy))) < site::kCapitalRange)
            return "near";
    }
    return "far";
}

Army* HeroIn(const std::vector<Army*>& stack) {
    for (Army* army : stack) {
        if (army->Hero()) return army;
    }
    return nullptr;
}

const Monster* MonsterOf(const Game& game, int index) {
    return game.GetMap().MonsterAt(index);
}

int SageDistance(const Site& target_site, int hero_x, int hero_y) {
    double dx = target_site.x - hero_x, dy = target_site.y - hero_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

bool Unshown(const Side& side, const Site& target_site, int hero_x, int hero_y) {
    return target_site.rich && !target_site.searched && !site::ShownTo(target_site, side) &&
           SageDistance(target_site, hero_x, hero_y) < site::kSageRange;
}

}  // namespace

namespace site {

const char* const kContentNames[6] = {"empty",  "temple", "a magic item",
                                      "a sage", "gold",   "allies"};

bool ItemReserved(int type, int value) {
    return type == rules::kItemFlight || type == rules::kItemDoubleMove ||
           type == rules::kItemStandard || (type == rules::kItemCommand && value >= 2);
}

bool ShownTo(const Site& target_site, const Side& side) {
    return ((target_site.revealed >> side.Index()) & 1) == 1;
}

void FillItemPool(Game& game, int reserved) {
    SiteExplorer(game).FillItemPool(reserved);
}

void MarkRich(Game& game) {
    SiteExplorer(game).MarkRich();
}

void Setup(Game& game) {
    SiteExplorer(game).Setup();
}

bool SurvivesGuardian(Game& game, const Army& hero, int monster_strength) {
    return SiteExplorer(game).SurvivesGuardian(hero, monster_strength);
}

int GuardianStrength(const Game& game, const Site& target_site) {
    return SiteExplorer(const_cast<Game&>(game)).GuardianStrength(target_site);
}

int Bless(Game& game, const Site& target_site, const std::vector<Army*>& stack) {
    return SiteExplorer(game).Bless(target_site, stack);
}

std::shared_ptr<SearchResult> Search(Game& game, const std::vector<Army*>& stack, int x, int y,
                                     bool human) {
    return SiteExplorer(game).Search(stack, x, y, human);
}

std::vector<SageEntry> SageList(Game& game, const Side& side, int hero_x, int hero_y) {
    return SiteExplorer(game).SageList(side, hero_x, hero_y);
}

Site* SageShow(Game& game, const Side& side, const SageEntry& entry, int hero_x, int hero_y) {
    return SiteExplorer(game).SageShow(side, entry, hero_x, hero_y);
}

int SageGem(Game& game, Side& side) {
    return SiteExplorer(game).SageGem(side);
}

std::array<int, 4> SageMap(Game& game, const Side& side, int target_x, int target_y) {
    return SiteExplorer(game).SageMap(side, target_x, target_y);
}

}  // namespace site

void SiteExplorer::FillItemPool(int reserved) {
    Map& map = game_.GetMap();
    if (!map.ItemPool()) return;
    const auto& pool = *map.ItemPool();
    std::map<int, Item*> by_index;
    std::map<int, bool> used;
    for (auto& item : map.Items()) by_index[item.index] = &item;
    for (int idx = 8; idx <= 21; idx++) {
        auto entry = by_index.find(idx);
        if (entry == by_index.end()) continue;
        Item* item = entry->second;
        bool want = idx < 8 + reserved;
        std::vector<int> choices;
        for (size_t i = 0; i < pool.size(); i++) {
            if (!used[static_cast<int>(i)] && site::ItemReserved(pool[i]) == want)
                choices.push_back(static_cast<int>(i));
        }
        int choice = game_.GetRng().PickIndex(choices.size());
        if (choice >= 0) {
            int pick = choices[choice];
            used[pick] = true;
            item->name = pool[pick].name;
            item->type = pool[pick].type;
            item->value = pool[pick].value;
        }
    }
}

void SiteExplorer::MarkRich() {
    Map& map = game_.GetMap();
    std::vector<Site*> ruins;
    for (auto& map_site : map.Sites()) {
        map_site.rich = false;
        map_site.revealed = 0xff;
        if (map_site.type != site::kTemple) ruins.push_back(&map_site);
    }
    int want = map.SiteCount() * site::kRichShare / 10;
    game_.GetRng().Shuffle(ruins);
    for (int i = 0; i < std::min(want, static_cast<int>(ruins.size())); i++) {
        ruins[i]->rich = true;
        if (map.GameOptions().quests != 0) ruins[i]->revealed = 0;
    }
}

void SiteExplorer::Setup() {
    Map& map = game_.GetMap();
    std::vector<std::pair<int, int>> capitals;
    for (Side* side : game_.Sides()) capitals.push_back(side->OriginCapitalPos().ToPair());

    MarkRich();
    int temples = 0;
    for (auto& map_site : map.Sites()) {
        map_site.content = map_site.type == site::kTemple ? site::kTemple : site::kEmpty;
        if (map_site.content == site::kTemple) {
            map_site.temple_index = temples;
            temples++;
        }
        map_site.item = kNone;
        map_site.guardian = kNone;
        map_site.ally_type = kNone;
        map_site.searched = false;
        map_site.band = Band(map_site, capitals);
    }

    int n_sites = map.SiteCount();
    int reserved = n_sites * 2 / 10;
    FillItemPool(reserved);

    int band_hi = reserved;
    int band_lo = std::min(game_.GetRng().Dice(2, 3, 1), band_hi);
    int last = std::min(22, n_sites / 3 + game_.GetRng().Dice(1, 5, -3) + 8);

    std::vector<Site*> free_sites;
    for (auto& map_site : map.Sites()) {
        if (map_site.content == site::kEmpty) free_sites.push_back(&map_site);
    }
    game_.GetRng().Shuffle(free_sites);

    std::map<int, Item*> by_index;
    for (auto& item : map.Items()) {
        by_index[item.index] = &item;
        item.status = 0;
    }

    for (int idx = 8; idx <= last - 1; idx++) {
        auto entry = by_index.find(idx);
        if (entry != by_index.end() && !(idx >= 8 + band_lo && idx < 8 + band_hi)) {
            Item* item = entry->second;
            bool want = site::ItemReserved(*item);
            for (size_t i = 0; i < free_sites.size(); i++) {
                Site* chosen = free_sites[i];
                if (chosen->rich == want) {
                    chosen->content = site::kItem;
                    chosen->item = idx;
                    item->status = 2;
                    free_sites.erase(free_sites.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
    }

    for (auto& map_site : map.Sites()) {
        if (map_site.content == site::kEmpty)
            map_site.content = game_.GetRng().Pick(Content(map_site.band), 0);
        if (map_site.content == site::kTemple) {
            map_site.guardian = kNone;
        } else if (map_site.content == site::kAllies) {
            map_site.ally_type = game_.GetRng().Pick(AllyTypes(map_site.band), 0);
            map_site.guardian = kNone;
        } else {
            map_site.guardian = game_.GetRng().Dice(1, 9, 0);
        }
    }
    map.RebuildSpatialIndices();
}

bool SiteExplorer::SurvivesGuardian(const Army& hero, int monster_strength) {
    int margin = 90 + 5 * (hero.Strength() + combat::BattleItems(hero) - monster_strength) +
                 3 * static_cast<int>(game::ArmiesAt(game_, hero.X(), hero.Y()).size());
    return game_.GetRng().Dice(1, 100, 0) <= margin;
}

int SiteExplorer::GuardianStrength(const Site& target_site) const {
    const Monster* monster = game_.GetMap().MonsterAt(target_site.guardian);
    return monster ? monster->strength : 0;
}

int SiteExplorer::Bless(const Site& target_site, const ArmyStack& stack) {
    if ((target_site.temple_index == kNone ? 0 : target_site.temple_index) >=
        site::kBlessingTemples)
        return 0;
    int bit = target_site.temple_index;
    int blessed = 0;
    for (Army* army : stack) {
        if (!army->HasBlessing(bit)) {
            army->AddBlessing(bit);
            army->SetStrength(std::min(9, army->Strength() + 1));
            blessed++;
            if (army->Hero()) hero::AddExperience(game_, army, 1);
        }
    }
    return blessed;
}

std::shared_ptr<SearchResult> SiteExplorer::Search(const ArmyStack& stack, int x, int y,
                                                   bool human) {
    Map& map = game_.GetMap();
    Site* target_site = map.SiteAt(x, y);
    if (!target_site || target_site->searched) return nullptr;
    if (stack.empty() || stack[0]->Moves() < 1) return nullptr;
    Army* hero = HeroIn(stack);
    auto out = std::make_shared<SearchResult>();
    out->site = target_site;

    if (target_site->content == site::kTemple) {
        out->kind = "temple";
        out->hero = hero;
        if (human && hero) return out;
        out->blessed = Bless(*target_site, stack);
        return out;
    }

    if (!hero) {
        out->kind = "no hero";
        return out;
    }
    Side* side = map.SideById(hero->Owner());
    out->hero = hero;

    if (target_site->content == site::kSage) {
        hero::AddExperience(game_, hero, 3);
        history::RecordDeed(game_, side, DeedType::kFound, history::kFoundSage, 0, hero->Name());
        target_site->searched = true;
        out->kind = "sage";
        return out;
    }

    hero::AddExperience(game_, hero, 3);
    const Monster* beaten = nullptr;
    if (target_site->content != site::kAllies && target_site->guardian != kNone &&
        target_site->guardian > 0) {
        int strength = GuardianStrength(*target_site);
        const Monster* monster = MonsterOf(game_, target_site->guardian);
        if (!SurvivesGuardian(*hero, strength)) {
            history::RecordDeed(game_, side, DeedType::kHeroKilled, history::kKilledSearching, 0,
                                hero->Name());
            history::Tally(game_, hero, kNone, false);
            hero::DropItems(game_, hero, hero->X(), hero->Y(), hero->OwnerIndex());
            game_.Remove(hero);
            out->kind = "killed";
            out->monster = monster;
            return out;
        }
        beaten = monster;
        target_site->guardian = 0;
    }
    out->guardian = beaten;

    if (target_site->content == site::kItem) {
        Item* found = nullptr;
        for (auto& item : map.Items()) {
            if (item.index == target_site->item) found = &item;
        }
        if (found && human) {
            found->status = 1;
            found->x = x;
            found->y = y;
        } else if (found) {
            found->status = 3;
            hero->AddItem(found);
        }
        if (found)
            history::RecordDeed(game_, side, DeedType::kFound, found->index, 0, hero->Name());
        target_site->searched = true;
        target_site->content = site::kEmpty;
        out->kind = "item";
        out->item = found;
        return out;
    }
    if (target_site->content == site::kGold) {
        int gold = target_site->rich ? game_.GetRng().Dice(3, 1000, 1000)
                                     : game_.GetRng().Dice(3, 500, 500);
        if (side) side->AddGold(gold);
        target_site->searched = true;
        history::RecordDeed(game_, side, DeedType::kFound, history::kFoundGold, 0, hero->Name());
        out->kind = "gold";
        out->gold = gold;
        return out;
    }
    if (target_site->content == site::kAllies) {
        target_site->searched = true;
        const ArmyType* type = game_.ArmyTypes().ById(target_site->ally_type);
        if (!type) type = game_.ArmyTypes().ById(armytype::kScouts);
        int count = game_.GetRng().Dice(1, 2, 0);
        if (target_site->rich) count += 2;
        for (int i = 0; i < count; i++) {
            Army* ally = hero::PlaceAlly(game_, type, x, y, *side);
            if (ally) out->armies.push_back(ally);
        }
        history::RecordDeed(game_, side, DeedType::kFound, history::kFoundAllies, 0, hero->Name());
        out->kind = "allies";
        out->type = type;
        out->count = count;
        return out;
    }
    out->kind = "empty";
    return out;
}

std::vector<site::SageEntry> SiteExplorer::SageList(const Side& side, int hero_x,
                                                    int hero_y) const {
    Map& map = game_.GetMap();
    std::vector<site::SageEntry> list;
    bool gold = false, allies = false;
    for (auto& map_site : map.Sites()) {
        if (Unshown(side, map_site, hero_x, hero_y)) {
            if (map_site.content == site::kGold && !gold) {
                gold = true;
                list.push_back(site::SageEntry{"gold", "Gold", nullptr});
            } else if (map_site.content == site::kAllies && !allies) {
                allies = true;
                list.push_back(site::SageEntry{"allies", "Allies", nullptr});
            } else if (map_site.content == site::kItem) {
                for (auto& item : map.Items()) {
                    if (item.index == map_site.item)
                        list.push_back(site::SageEntry{"item", item.name, &item});
                }
            }
        }
    }
    return list;
}

Site* SiteExplorer::SageShow(const Side& side, const site::SageEntry& entry, int hero_x,
                             int hero_y) {
    Site* found = nullptr;
    int best = kNone;
    for (auto& map_site : game_.GetMap().Sites()) {
        if (entry.kind == "item") {
            if (map_site.content == site::kItem && entry.item &&
                map_site.item == entry.item->index) {
                found = &map_site;
                break;
            }
        } else if (Unshown(side, map_site, hero_x, hero_y) &&
                   map_site.content == (entry.kind == "gold" ? site::kGold : site::kAllies)) {
            int distance = SageDistance(map_site, hero_x, hero_y);
            if (best == kNone || distance < best) {
                found = &map_site;
                best = distance;
            }
        }
    }
    if (!found) return nullptr;
    if (!site::ShownTo(*found, side)) found->revealed = found->revealed + (1 << side.Index());
    game::Reveal(game_, side.Index(), found->x, found->y, false);
    return found;
}

int SiteExplorer::SageGem(Side& side) {
    int gold = game_.GetRng().Dice(3, 500, 500);
    side.AddGold(gold);
    return gold;
}

SiteExplorer::MapPatch SiteExplorer::SageMap(const Side& side, int target_x, int target_y) {
    const Map& map = game_.GetMap();
    int left = std::max(0, target_x - game_.GetRng().Dice(1, 5, 8));
    int top = std::max(0, target_y - game_.GetRng().Dice(1, 5, 8));
    int width = game_.GetRng().Dice(1, 10, 15), height = game_.GetRng().Dice(1, 10, 15);
    if (left + width >= map.Width()) width = map.Width() - left - 1;
    if (top + height >= map.Height()) height = map.Height() - top - 1;
    for (int x = left; x < left + width; x++) {
        for (int y = top; y < top + height; y++) game::Reveal(game_, side.Index(), x, y, false);
    }
    return {left, top, width, height};
}

std::string SiteExplorer::DescribeSearch(const SearchResult* result) {
    if (!result) return "";
    const std::string& name = result->site->name;
    if (result->kind == "temple") {
        return result->blessed > 0
                       ? name + " blesses " + std::to_string(result->blessed) + " of your armies."
                       : name + " has blessed them already.";
    }
    if (result->kind == "no hero") return name + " can only be searched by a hero.";
    if (result->kind == "killed") {
        return "Your hero is slain in " + name + " by a " +
               (result->monster ? result->monster->name : "guardian") + "!";
    }
    if (result->kind == "gold") return name + " yields " + std::to_string(result->gold) + " gold!";
    if (result->kind == "item") {
        return "Your hero finds the " +
               (result->item ? result->item->name : std::string("treasure")) + " in " + name + "!";
    }
    if (result->kind == "allies") {
        return std::to_string(result->armies.size()) + " " + result->type->name + " join you at " +
               name + "!";
    }
    if (result->kind == "sage") return "A sage dwells in " + name + ".";
    return name + " holds nothing.";
}

}  // namespace w2
