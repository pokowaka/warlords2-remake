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
    return site.temple_index != kNone && hero->blessings.count(site.temple_index);
}

/**
 * @brief Is a site still worth the hero's while: open, explored, and within
 *        a reach that grows with the turn (further for a rich site)?
 */
bool Worth(const Game& game, const Side& side, const Site& site, const Army* hero) {
    int distance = Dist(site.x, site.y, hero->x, hero->y);
    int reach = game.turn * 2 + (site.rich ? 15 : 3);
    return distance < reach && Explored(game, side.index, site.x, site.y) && SiteOpen(site);
}

/**
 * @brief Is an item lying where a hero may take it: on the map, explored,
 *        and not inside a standing city of another side?
 */
bool Lying(const Game& game, const Side& side, const Item& item) {
    if (item.status != 1 || item.x == kNone || item.planted) return false;
    if (!Explored(game, side.index, item.x, item.y)) return false;
    City* city = CityAt(game, item.x, item.y);
    return !city || !Standing(*city) || city->owner_index == side.index;
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
    for (Army* army : Armies(game, side.index)) {
        if (IsHero(army) && !army->transit && info.heroes.size() < 6) {
            info.heroes.push_back(army);
            City* city = CityAt(game, army->x, army->y);
            info.city.push_back(city && Standing(*city) ? city : nullptr);
            info.site.push_back(SiteAt(game, army->x, army->y));
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
    int range = hero->ai_order == kOrderCity ? 5 : 25;
    int quest_d = 100, site_d = 100;
    Site* quest_site = nullptr;
    Site* best = nullptr;
    for (auto& candidate : game.map->sites) {
        if (Worth(game, side, candidate, hero)) {
            int distance = Dist(candidate.x, candidate.y, hero->x, hero->y);
            bool worth = true;
            if (candidate.content == site::kTemple) {
                bool quest_free = game.map->options.quests != 0 && !side.quest &&
                                  distance < range && distance < quest_d;
                if (quest_free) {
                    quest_d = distance;
                    quest_site = &candidate;
                } else if (Blessed(hero, candidate))
                    worth = false;
            } else {
                for (size_t j = 0; j < info.heroes.size(); j++) {
                    Army* other = info.heroes[j];
                    if (j != hero_index && other->ai_order == kOrderSite &&
                        other->ai_dest == candidate.index)
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
    int range = hero->ai_order == kOrderCity ? 5 : 15;
    Item* best = nullptr;
    int best_distance = 100;
    for (int i = static_cast<int>(game.map->items.size()) - 1; i >= 0; i--) {
        Item& item = game.map->items[i];
        if (Lying(game, side, item)) {
            bool taken = false;
            for (size_t j = 0; j < info.heroes.size(); j++) {
                Army* other = info.heroes[j];
                if (j != hero_index && other->ai_order == kOrderItem &&
                    other->ai_dest == item.index)
                    taken = true;
            }
            if (!taken) {
                int distance = Dist(item.x, item.y, hero->x, hero->y);
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
    int dest = hero->ai_order == kOrderCity ? hero->ai_dest : kNone;
    auto list =
            CollectOrdered(game, side.index, hero->x, hero->y, hero->ai_group, hero->ai_order, 12);
    if (list.empty()) return;
    Sel sel = Select(game, list);
    int need = list.size() < 3 ? 95 : 75;
    int best_odds = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (Standing(city) && !Cflag(data, city, kCfUnseen) && city.owner_index != kNone &&
            State(game, side.index, city.owner_index) == 2) {
            int distance = Dist(city.x, city.y, hero->x, hero->y);
            if (distance <= info.enemy_d || (dest != kNone && dest == city.index)) {
                int odds = Odds(game, sel, city.x, city.y) +
                           (dest != kNone && dest == city.index ? 20 : 0);
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
    if (!city || Get(data.garrison, city->index) > 4) {
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
    auto list =
            CollectOrdered(game, side.index, hero->x, hero->y, hero->ai_group, hero->ai_order, 12);
    int stack_size = static_cast<int>(list.size());
    if (stack_size == 0) return 0;
    City* city = info.city[hero_index];
    int enemy_distance = 100;
    if (city) {
        enemy_distance = cities::NearestArmy(game, side, hero->x, hero->y);
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
    if (!city || Get(data.garrison, city->index) != stack_size || stack_size > 2 ||
        enemy_distance > 14) {
        std::optional<Sel> sel;
        if (kind == 3) {
            sel = Order(game, list, kOrderSite, index, 0);
        } else {
            sel = Order(game, list, kOrderRoam, 0, 0);
            if (sel) sel->leader->target = std::make_pair(target_x, target_y);
        }
        if (sel && MoveTo(game, *sel, sel->leader->target->first, sel->leader->target->second) == 4)
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
    auto list =
            CollectOrdered(game, side.index, hero->x, hero->y, hero->ai_group, hero->ai_order, 12);
    if (list.empty()) return 0;
    City* city = info.city[hero_index];
    if (city && Get(data.garrison, city->index) == static_cast<int>(list.size())) {
        int enemy_distance = cities::NearestArmy(game, side, hero->x, hero->y);
        if (enemy_distance < (game.turn < 6 ? 10 : 15)) return 0;
    }
    auto sel = Order(game, list, kOrderCity, target->index, 0);
    if (sel) MoveTo(game, *sel, sel->leader->target->first, sel->leader->target->second);
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
    auto side_quest = side.quest;
    if (!(side_quest && (side_quest->type == quest::kOccupy || side_quest->type == quest::kRaze)))
        return false;
    City* city = side_quest->city;
    if (!city || Cflag(data, *city, kCfUnseen)) return false;
    int need = (city->owner_index == kNone || game.turn < 8) ? 1 : 2;
    auto list = Collect(game, side.index, hero->x, hero->y, 8);
    if (static_cast<int>(list.size()) < need) return false;
    if (!(hero->ai_order == kOrderCity && hero->ai_dest == city->index)) {
        Sel sel = Select(game, list);
        if (Odds(game, sel, city->x, city->y) < 75) return false;
    }
    auto sel = Order(game, list, kOrderCity, city->index, 0);
    if (sel) MoveTo(game, *sel, sel->leader->target->first, sel->leader->target->second);
    return true;
}
}  // namespace

void Phase(Game& game, Side& side) {
    AIData& data = Data(game, side);
    data.quest_city = kNone;
    auto side_quest = side.quest;
    if (game.map->options.quests != 0 && side_quest &&
        (side_quest->type == quest::kOccupy || side_quest->type == quest::kRaze) &&
        side_quest->city) {
        data.quest_city = side_quest->city->index;
    }
    Info info = Survey(game, side);
    for (int hero_index = static_cast<int>(info.heroes.size()) - 1; hero_index >= 0; hero_index--) {
        Army* hero = info.heroes[hero_index];
        if (hero && Alive(game, hero) && hero->owner == side.index && IsHero(hero)) {
            if (info.city[hero_index]) cities::Garrison(game, side, *info.city[hero_index], true);
            bool skip = game.map->options.quests != 0 && side.quest && side.quest->hero == hero &&
                        QuestGo(game, side, hero_index, info);
            if (!skip && Alive(game, hero)) {
                // A hero sent at a site goes on there while it is still worth
                // it; otherwise its stack forgets the order.
                if (hero->ai_order == kOrderSite) {
                    Site* site =
                            hero->ai_dest >= 0 &&
                                            hero->ai_dest < static_cast<int>(game.map->sites.size())
                                    ? &game.map->sites[hero->ai_dest]
                                    : nullptr;
                    if (!site || !Worth(game, side, *site, hero)) {
                        for (Army* army : OnTile(game, side.index, hero->x, hero->y)) {
                            if (army->ai_order == hero->ai_order) {
                                ClearOrder(army);
                                army->target.reset();
                            }
                        }
                    } else {
                        info.target = site;
                        Go(game, side, hero_index, 3, info);
                    }
                }
                int step = 1, tries = 0;
                bool moved = false;
                while (step != 0 && tries < 2 && Alive(game, hero) && hero->moves > 3) {
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
                            Collect(game, side.index, hero->x, hero->y, 8).size() == 1) {
                            moves::Explore(game, side, hero, nullptr);
                        }
                        step = 0;
                    }
                    if (!Alive(game, hero) || hero->owner != side.index) step = 0;
                }
            }
        }
    }
}

}  // namespace w2::ai::heroes
