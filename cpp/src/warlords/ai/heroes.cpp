#include "warlords/ai/heroes.hpp"

#include <vector>

#include "warlords/ai/cities.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/quest.hpp"
#include "warlords/site.hpp"

namespace w2::ai::heroes {

using namespace core;

namespace {
/** @brief Has the hero already been blessed at this site's temple? */
bool Blessed(const Army* hero, const Site& site) {
    return site.temple_index != kNone && hero->HasBlessing(site.temple_index);
}

/**
 * @brief Is a site still worth the hero's while: open, explored, and within
 *        a reach that grows with the turn (further for a rich site)?
 */
bool Worth(const Game& game, const Side& side, const Site& site, const Army* hero) {
    int distance = Dist(site.x, site.y, hero->X(), hero->Y());
    int reach = game.turn * 2 + (site.rich ? 15 : 3);
    return distance < reach && Explored(game, side.Index(), site.x, site.y) && SiteOpen(site);
}

/**
 * @brief Is an item lying where a hero may take it: on the map, explored,
 *        and not inside a standing city of another side?
 */
bool Lying(const Game& game, const Side& side, const Item& item) {
    if (item.status != 1 || item.x == kNone || item.planted) return false;
    if (!Explored(game, side.Index(), item.x, item.y)) return false;
    City* city = CityAt(game, item.x, item.y);
    return !city || !Standing(*city) || city->OwnerIndex() == side.Index();
}

/** @brief The side's heroes and, for the hero being moved, its candidate goals. */
struct Info {
    std::vector<Army*> heroes;  ///< Six at most.
    std::vector<City*> city;    ///< By hero: the standing city it is in, or null.
    std::vector<Site*> site;    ///< By hero: the site it stands on, or null.
    Site* quest = nullptr;      ///< A temple to get a quest at.
    int quest_d = 100;          ///< Its distance.
    Site* target = nullptr;     ///< A site to visit.
    int site_d = 100;           ///< Its distance.
    Item* item = nullptr;       ///< An item to pick up.
    int item_d = 100;           ///< Its distance.
    City* enemy = nullptr;      ///< An enemy city the hero's stack beats.
    int enemy_d = 20;           ///< Its distance.
};

/** @brief The side's heroes, six at most, with the city or site each stands on. */
Info Survey(Game& game, Side& side) {
    AIData& data = Data(game, side);
    Info info;
    data.heroes = 0;
    for (Army* army : Armies(game, side.Index())) {
        if (IsHero(army) && !army->InTransit() && info.heroes.size() < 6) {
            info.heroes.push_back(army);
            City* city = CityAt(game, army->X(), army->Y());
            info.city.push_back(city && Standing(*city) ? city : nullptr);
            info.site.push_back(SiteAt(game, army->X(), army->Y()));
            data.heroes++;
        }
    }
    return info;
}

/**
 * @brief Finds the temples and ruins in reach of a hero: within 5 when it is
 *        sent at a city, else 25. A temple is a quest temple when the side
 *        can take a quest; otherwise one that has blessed the hero is
 *        skipped, and so is a ruin another hero is going for.
 */
void Sites(Game& game, Side& side, size_t hero_index, Info& info) {
    Army* hero = info.heroes[hero_index];
    int range = OrderOf(game, hero).order == kOrderCity ? 5 : 25;
    int quest_d = 100, site_d = 100;
    Site* quest_site = nullptr;
    Site* best = nullptr;
    for (auto& candidate : game.map->sites) {
        if (Worth(game, side, candidate, hero)) {
            int distance = Dist(candidate.x, candidate.y, hero->X(), hero->Y());
            bool worth = true;
            if (candidate.content == site::kTemple) {
                bool quest_free = game.map->options.quests != 0 && !side.HasQuest() &&
                                  distance < range && distance < quest_d;
                if (quest_free) {
                    quest_d = distance;
                    quest_site = &candidate;
                } else if (Blessed(hero, candidate))
                    worth = false;
            } else {
                for (size_t j = 0; j < info.heroes.size(); j++) {
                    Army* other = info.heroes[j];
                    const ClassicArmyOrder& other_ord = OrderOf(game, other);
                    if (j != hero_index && other_ord.order == kOrderSite &&
                        other_ord.dest == candidate.index)
                        worth = false;
                }
            }
            if (worth && distance <= range && distance < site_d) {
                site_d = distance;
                best = &candidate;
            }
        }
    }
    info.quest = quest_site;
    info.quest_d = quest_d;
    info.target = best;
    info.site_d = site_d;
}

/**
 * @brief Finds the nearest item lying about within 5 (when the hero is sent
 *        at a city) or 15 that no other hero is going for.
 */
void Items(Game& game, Side& side, size_t hero_index, Info& info) {
    Army* hero = info.heroes[hero_index];
    int range = OrderOf(game, hero).order == kOrderCity ? 5 : 15;
    Item* best = nullptr;
    int best_distance = 100;
    for (int i = static_cast<int>(game.map->items.size()) - 1; i >= 0; i--) {
        Item& item = game.map->items[i];
        if (Lying(game, side, item)) {
            bool taken = false;
            for (size_t j = 0; j < info.heroes.size(); j++) {
                Army* other = info.heroes[j];
                const ClassicArmyOrder& other_ord = OrderOf(game, other);
                if (j != hero_index && other_ord.order == kOrderItem &&
                    other_ord.dest == item.index)
                    taken = true;
            }
            if (!taken) {
                int distance = Dist(item.x, item.y, hero->X(), hero->Y());
                if (distance <= range && distance < best_distance) {
                    best = &item;
                    best_distance = distance;
                }
            }
        }
    }
    info.item = best;
    info.item_d = best_distance;
}

/**
 * @brief Finds an enemy city within 20 the hero's stack beats: 95% odds for
 *        a stack under three, else 75%, with 20 extra for the city it is
 *        already going for.
 */
void Enemy(Game& game, Side& side, size_t hero_index, Info& info) {
    AIData& data = Data(game, side);
    Army* hero = info.heroes[hero_index];
    info.enemy = nullptr;
    info.enemy_d = 20;
    const ClassicArmyOrder& hero_ord = OrderOf(game, hero);
    int dest = hero_ord.order == kOrderCity ? hero_ord.dest : kNone;
    auto list = CollectOrdered(game, side.Index(), hero->X(), hero->Y(), hero_ord.group,
                               hero_ord.order, 12);
    if (list.empty()) return;
    Sel sel = Select(game, list);
    int need = list.size() < 3 ? 95 : 75;
    int best_odds = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (Standing(city) && !Cflag(data, city, kCfUnseen) && !city.IsNeutral() &&
            State(game, side.Index(), city.OwnerIndex()) == 2) {
            int distance = Dist(city.X(), city.Y(), hero->X(), hero->Y());
            if (distance <= info.enemy_d || (dest != kNone && dest == city.Index())) {
                int odds = Odds(game, sel, city.X(), city.Y()) +
                           (dest != kNone && dest == city.Index() ? 20 : 0);
                if (odds >= need &&
                    (best_odds < odds || (odds == best_odds && distance < info.enemy_d))) {
                    info.enemy = &city;
                    info.enemy_d = distance;
                    best_odds = odds;
                }
            }
        }
    }
}

/**
 * @brief Picks the hero's goal, each scored 100 - distance + 1d20. A hero in
 *        a city with a garrison of four or fewer considers no enemy city or
 *        item.
 * @return 1 the quest temple, 3 the site, 4 the enemy city, 2 the item,
 *         0 nothing.
 */
int Choose(Game& game, Side& side, size_t hero_index, Info& info) {
    AIData& data = Data(game, side);
    int roll = game.rng.Dice(1, 20, 0);
    int pick = info.quest ? 1 : 0;
    int best = 0;
    if (info.quest) best = 100 - info.quest_d + roll;
    roll = game.rng.Dice(1, 20, 0);
    int score = 100 - info.site_d + roll;
    if (info.target && best < score) {
        pick = 3;
        best = score;
    }
    City* city = info.city[hero_index];
    if (!city || Get(data.garrison, city->Index()) > 4) {
        roll = game.rng.Dice(1, 20, 0);
        score = 100 - info.enemy_d + roll;
        if (info.enemy && best < score) {
            pick = 4;
            best = score;
        }
        roll = game.rng.Dice(1, 20, 0);
        if (info.item && best < 100 - info.item_d + roll) pick = 2;
    }
    return pick;
}

/**
 * @brief Goes for the site (kind 3) or the item (kind 2). A hero leaving a
 *        city takes a flier along, or goes alone when no enemy is within 15;
 *        it stays when it is the city's last defence and an enemy is near.
 * @return 1 when it got there, else 0.
 */
int Go(Game& game, Side& side, size_t hero_index, int kind, Info& info) {
    AIData& data = Data(game, side);
    Army* hero = info.heroes[hero_index];
    int target_x, target_y, index;
    if (kind == 3) {
        if (!info.target) return 0;
        target_x = info.target->x;
        target_y = info.target->y;
        index = info.target->index;
    } else {
        if (!info.item) return 0;
        target_x = info.item->x;
        target_y = info.item->y;
        index = info.item->index;
    }
    const ClassicArmyOrder& hero_ord = OrderOf(game, hero);
    auto list = CollectOrdered(game, side.Index(), hero->X(), hero->Y(), hero_ord.group,
                               hero_ord.order, 12);
    int stack_size = static_cast<int>(list.size());
    if (stack_size == 0) return 0;
    City* city = info.city[hero_index];
    int enemy_distance = 100;
    if (city) {
        enemy_distance = cities::NearestArmy(game, side, hero->X(), hero->Y());
        Army* flier = nullptr;
        for (int i = static_cast<int>(list.size()) - 1; i >= 0; i--)
            if (Flies(game, list[i])) flier = list[i];
        if (!flier) {
            if (enemy_distance >= 15) {
                list = {hero};
                stack_size = 1;
            }
        } else {
            list = {hero, flier};
            stack_size = 2;
        }
    }
    if (!city || Get(data.garrison, city->Index()) != stack_size || stack_size > 2 ||
        enemy_distance > 14) {
        std::optional<Sel> sel;
        if (kind == 3) {
            sel = Order(game, list, kOrderSite, index, 0);
        } else {
            sel = Order(game, list, kOrderRoam, 0, 0);
            if (sel) sel->leader->SetTarget(TilePos(target_x, target_y));
        }
        if (sel && MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y) == 4)
            return 1;
    }
    return 0;
}

/**
 * @brief Goes for the enemy city with the hero's stack, unless the stack is
 *        the whole garrison of its city and an enemy is near.
 * @return Always 0: the hero moves no further this turn.
 */
int Attack(Game& game, Side& side, size_t hero_index, Info& info) {
    AIData& data = Data(game, side);
    Army* hero = info.heroes[hero_index];
    City* target = info.enemy;
    if (!target) return 0;
    const ClassicArmyOrder& hero_ord = OrderOf(game, hero);
    auto list = CollectOrdered(game, side.Index(), hero->X(), hero->Y(), hero_ord.group,
                               hero_ord.order, 12);
    if (list.empty()) return 0;
    City* city = info.city[hero_index];
    if (city && Get(data.garrison, city->Index()) == static_cast<int>(list.size())) {
        int enemy_distance = cities::NearestArmy(game, side, hero->X(), hero->Y());
        if (enemy_distance < (game.turn < 6 ? 10 : 15)) return 0;
    }
    auto sel = Order(game, list, kOrderCity, target->Index(), 0);
    if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
    return 0;
}

/**
 * @brief The quest hero goes for the city its quest wants taken or razed,
 *        with one army along (two for an owned city from turn 8) and good
 *        odds unless it is already on its way.
 * @return Whether it went.
 */
bool QuestGo(Game& game, Side& side, size_t hero_index, Info& info) {
    AIData& data = Data(game, side);
    Army* hero = info.heroes[hero_index];
    auto side_quest = side.QuestPtr();
    if (!(side_quest &&
          (side_quest->Type() == quest::kOccupy || side_quest->Type() == quest::kRaze)))
        return false;
    City* city = side_quest->ResolveCity(game);
    if (!city || Cflag(data, *city, kCfUnseen)) return false;
    int need = (city->IsNeutral() || game.turn < 8) ? 1 : 2;
    auto list = Collect(game, side.Index(), hero->X(), hero->Y(), 8);
    if (static_cast<int>(list.size()) < need) return false;
    const ClassicArmyOrder& hero_ord = OrderOf(game, hero);
    if (!(hero_ord.order == kOrderCity && hero_ord.dest == city->Index())) {
        Sel sel = Select(game, list);
        if (Odds(game, sel, city->X(), city->Y()) < 75) return false;
    }
    auto sel = Order(game, list, kOrderCity, city->Index(), 0);
    if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
    return true;
}
}  // namespace

void Phase(Game& game, Side& side) {
    AIData& data = Data(game, side);
    data.quest_city = kNone;
    auto side_quest = side.QuestPtr();
    if (game.map->options.quests != 0 && side_quest &&
        (side_quest->Type() == quest::kOccupy || side_quest->Type() == quest::kRaze)) {
        City* quest_city = side_quest->ResolveCity(game);
        if (quest_city) data.quest_city = quest_city->Index();
    }
    Info info = Survey(game, side);
    for (int hero_index = static_cast<int>(info.heroes.size()) - 1; hero_index >= 0; hero_index--) {
        Army* hero = info.heroes[hero_index];
        if (hero && Alive(game, hero) && hero->OwnerIndex() == side.Index() && IsHero(hero)) {
            if (info.city[hero_index]) cities::Garrison(game, side, *info.city[hero_index], true);
            bool skip = game.map->options.quests != 0 && side.ActiveQuest() &&
                        side.ActiveQuest()->HeroId() == hero->Id() &&
                        QuestGo(game, side, hero_index, info);
            if (!skip && Alive(game, hero)) {
                // A hero sent at a site goes on there while it is still worth
                // it; otherwise its stack forgets the order.
                if (OrderOf(game, hero).order == kOrderSite) {
                    int hero_dest = OrderOf(game, hero).dest;
                    Site* site =
                            hero_dest >= 0 && hero_dest < static_cast<int>(game.map->sites.size())
                                    ? &game.map->sites[hero_dest]
                                    : nullptr;
                    if (!site || !Worth(game, side, *site, hero)) {
                        for (Army* army : OnTile(game, side.Index(), hero->X(), hero->Y())) {
                            if (OrderOf(game, army).order == OrderOf(game, hero).order) {
                                ClearOrder(game, army);
                                army->ClearTarget();
                            }
                        }
                    } else {
                        info.target = site;
                        Go(game, side, hero_index, 3, info);
                    }
                }
                int step = 1, tries = 0;
                bool moved = false;
                while (step != 0 && tries < 2 && Alive(game, hero) && hero->Moves() > 3) {
                    tries++;
                    Sites(game, side, hero_index, info);
                    Items(game, side, hero_index, info);
                    Enemy(game, side, hero_index, info);
                    int pick = Choose(game, side, hero_index, info);
                    if (pick == 1) {
                        step = 0;
                    } else if (pick == 2 || pick == 3) {
                        moved = true;
                        step = Go(game, side, hero_index, pick, info);
                    } else if (pick == 4) {
                        moved = true;
                        step = Attack(game, side, hero_index, info);
                    } else {
                        // Nothing to go for: early on a hidden map, a lone
                        // hero that has moved explores.
                        if (moved && game.map->options.hidden_map != 0 && game.turn < 10 &&
                            Collect(game, side.Index(), hero->X(), hero->Y(), 8).size() == 1) {
                            moves::Explore(game, side, hero, nullptr);
                        }
                        step = 0;
                    }
                    if (!Alive(game, hero) || hero->OwnerIndex() != side.Index()) step = 0;
                }
            }
        }
    }
}

}  // namespace w2::ai::heroes
