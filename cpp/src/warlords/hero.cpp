#include "warlords/hero.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2::hero {

const char* LevelName(int level) {
    switch (level) {
    case 1:
        return "Hero";
    case 2:
        return "Cavalier";
    case 3:
        return "Champion";
    case 4:
        return "Paladin";
    }
    return "";
}

int PromotionAt(int level) {
    switch (level) {
    case 1:
        return 15;
    case 2:
        return 30;
    case 3:
        return 60;
    }
    return kNone;
}

const std::vector<std::pair<std::string, bool>>& Names(Game& game, const Side& side) {
    auto cached = game.hero_names.find(side.index);
    if (cached != game.hero_names.end()) return cached->second;
    std::vector<std::pair<std::string, bool>> names;
    auto text = ReadFile(game.data_dir + "/TERRAIN0/HERONAM" + std::to_string(side.index) + ".DAT");
    for (auto& line : SplitLines(text ? *text : "")) {
        // ^#(\d)\s+(.+)$
        if (line.size() >= 3 && line[0] == '#' && line[1] >= '0' && line[1] <= '9' &&
            (line[2] == ' ' || line[2] == '\t')) {
            size_t name_start = 2;
            while (name_start < line.size() &&
                   (line[name_start] == ' ' || line[name_start] == '\t'))
                name_start++;
            if (name_start < line.size())
                names.emplace_back(line.substr(name_start), line[1] == '1');
        }
    }
    return game.hero_names[side.index] = names;
}

std::pair<std::string, bool> RollName(Game& game, const Side& side) {
    const auto& names = Names(game, side);
    int roll = game.rng.Dice(1, kNameRoll, 0);
    if (roll - 1 < static_cast<int>(names.size())) return names[roll - 1];
    if (names.empty()) return {"Hero", false};
    return names.back();
}

/**
 * @brief Counts the heroes in the game.
 * @param side The side whose own heroes to count too; may be null.
 * @return {heroes in the whole game, heroes of `side`}.
 */
static std::pair<int, int> CountHeroes(const Game& game, const Side* side) {
    int all = 0, mine = 0;
    for (Army* army : game.armies) {
        if (army->type == armytype::kHero) {
            all++;
            if (side && army->owner == side->index) mine++;
        }
    }
    return {all, mine};
}

std::shared_ptr<HeroOffer> Offer(Game& game, Side& side) {
    auto named = [&](HeroOffer offer) {
        auto [name, female] = RollName(game, side);
        offer.name = name;
        offer.female = female;
        return std::make_shared<HeroOffer>(offer);
    };
    // none at all once the surrender has been offered
    if (game.surrender_offered) return nullptr;
    if (game.turn == 1) return named(HeroOffer{0, side.capital, true, "", false});

    auto [all, mine] = CountHeroes(game, &side);
    if (all >= kMaxInGame) return nullptr;
    auto cities = game::SideCities(game, side);
    int limit = static_cast<int>(cities.size()) >= kBigSideCities ? kMaxPerBigSide : kMaxPerSide;
    if (mine >= limit) return nullptr;

    int price = mine == 0 ? game.rng.Dice(1, 400, 300) : game.rng.Dice(1, 600, 1000);
    if (price > side.gold) return nullptr;
    if (game.rng.Dice(1, 30, 0) >= 7) return nullptr;  // a 20% chance

    City* city = game.rng.Pick(cities, static_cast<City*>(nullptr));
    if (!city) return nullptr;
    // a computer's hero appears where its AI wants one
    if (side.computer) city = ai::HeroCity(game, side, city);
    return named(HeroOffer{price, city, false, "", false});
}

const ArmyType* MagicalType(Game& game) {
    std::vector<int> ids;
    for (int id = 0; id <= 27; id++) {
        const ArmyType* type = game.types.ById(id);
        // bonus field 48 is the magical flag (see docs/formats/armytype.md)
        if (type && type->bonus[48] != 0) ids.push_back(id);
    }
    int pick = game.rng.Dice(1, static_cast<int>(ids.size()), -1);
    return game.types.ById(pick >= 0 && pick < static_cast<int>(ids.size()) ? ids[pick] : 25);
}

std::pair<const ArmyType*, int> Allies(Game& game) {
    const ArmyType* type = MagicalType(game);
    int roll = game.rng.Dice(1, 100, 0);
    int count = roll < 70 ? 1 : roll < 95 ? 2 : 3;
    return {type, count};
}

Army NewAlly(const ArmyType* type, int x, int y, int owner) {
    Army ally;
    ally.x = x;
    ally.y = y;
    ally.owner = owner;
    ally.type = type->id;
    ally.name = type->name;
    ally.strength = type->strength;
    ally.max_moves = type->move;
    ally.moves = type->move;
    ally.upkeep = 0;
    ally.home_city = 0;
    return ally;
}

Army* PlaceAlly(Game& game, const ArmyType* type, int x, int y, const Side& side) {
    int try_x = x, try_y = y;
    for (int tries = 10; tries > 0; tries--) {
        if (try_x >= 0 && try_y >= 0 && try_x < game.map->width && try_y < game.map->height) {
            auto armies_here = game::ArmiesAt(game, try_x, try_y);
            if (armies_here.size() < 8 &&
                (armies_here.empty() || armies_here[0]->owner == side.index)) {
                return game.Add(NewAlly(type, try_x, try_y, side.index));
            }
        }
        try_x += game.rng.Dice(1, 3, -2);
        try_y += game.rng.Dice(1, 3, -2);
    }
    return nullptr;
}

Recruited Recruit(Game& game, Side& side, const HeroOffer& offer) {
    City* city = offer.city;
    side.gold = std::max(0, side.gold - offer.price);
    std::pair<int, int> last{kNone, kNone};
    auto [hero_x, hero_y] = game::FreeTileIn(game, *city, true, &last);
    if (hero_x == kNone) {
        hero_x = last.first != kNone ? last.first : city->x;
        hero_y = last.second != kNone ? last.second : city->y;
    }

    Army hero;
    hero.x = hero_x;
    hero.y = hero_y;
    hero.owner = side.index;
    hero.type = armytype::kHero;
    hero.name = offer.name.empty() ? "Hero" : offer.name;
    hero.female = offer.female;
    hero.strength = kStartStrength;
    hero.max_moves = kStartMoves;
    hero.moves = kStartMoves;
    hero.upkeep = 0;
    hero.home_city = city->index;
    hero.level = 1;
    hero.experience = 0;
    if (offer.first) {
        // Turn 1: the hero carries its side's standard, item number = side
        if (side.index < static_cast<int>(game.map->items.size())) {
            Item* standard = &game.map->items[side.index];
            standard->status = 3;
            standard->x = kNone;
            standard->y = kNone;
            standard->standard_of = side.index;
            hero.items.push_back(standard);
        }
    }
    Recruited recruited;
    recruited.hero = game.Add(hero);
    history::RecordDeed(game, &side, DeedType::kHeroEmerges, city->index, 0, recruited.hero->name);

    if (!offer.first) {
        auto [type, ally_count] = Allies(game);
        recruited.type = type;
        recruited.count = ally_count;
        for (int i = 0; i < ally_count; i++) {
            Army* ally = PlaceAlly(game, type, hero_x, hero_y, side);
            if (ally) recruited.allies.push_back(ally);
        }
    }
    return recruited;
}

void AddExperience(Game& game, Army* hero, int amount) {
    hero->experience = std::min(rules::kMaxHeroXp, hero->experience + amount);
}

std::vector<Promotion> CheckPromotions(Game& game, const Side& side) {
    std::vector<Promotion> promoted;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->type == armytype::kHero && army->owner == side.index) {
            int level = army->level ? army->level : 1;
            int needed = PromotionAt(level);
            if (needed != kNone && army->experience >= needed) {
                int before = army->strength;
                army->level = level + 1;
                army->strength = std::min(kMaxStrength, before + 1);
                army->max_moves += kPromotionMoves;
                army->title = LevelName(army->level);
                promoted.push_back(Promotion{army, army->strength - before});
            }
        }
    }
    return promoted;
}

void BattleExperience(Game& game, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city) {
    for (Army* attacker : attackers) {
        if (attacker->type == armytype::kHero && !result.dead_by_army.count(attacker))
            AddExperience(game, attacker, was_city ? 2 : 1);
    }
    for (size_t i = 0; i < defenders.size(); i++) {
        Army* defender = defenders[i];
        if (!result.dead_by_army.count(defender)) {
            bool counts;
            if (rules::bugs.hero_experience_reads_attacker_types) {
                Army* same_slot = i < attackers.size() ? attackers[i]
                                                       : nullptr;  // the wrong line, on purpose
                counts = same_slot && same_slot->type == armytype::kHero;
            } else {
                counts = true;
            }
            if (counts && defender->type == armytype::kHero) AddExperience(game, defender, 1);
        }
    }
}

std::vector<Item*> ItemsHere(Game& game, const Army* hero) {
    std::vector<Item*> carried = hero->items;
    std::vector<Item*> ground;
    for (auto& item : game.map->items)
        if (item.status == 1 && item.x == hero->x && item.y == hero->y) ground.push_back(&item);
    auto by_index = [](Item* left, Item* right) { return left->index < right->index; };
    LuaSort(carried, by_index);
    LuaSort(ground, by_index);
    carried.insert(carried.end(), ground.begin(), ground.end());
    return carried;
}

void DropItem(Game& game, Army* hero, Item* item) {
    auto carried = std::find(hero->items.begin(), hero->items.end(), item);
    if (carried != hero->items.end()) hero->items.erase(carried);
    if (scn::TerrainAt(*game.map, hero->x, hero->y) == move::kWater) {
        item->status = 0;
        item->x = kNone;
        item->y = kNone;
    } else {
        item->status = 1;
        item->x = hero->x;
        item->y = hero->y;
        item->planted = false;
    }
}

void TakeItem(Game& game, Army* hero, Item* item) {
    item->status = 3;
    item->x = kNone;
    item->y = kNone;
    item->planted = false;
    hero->items.push_back(item);
}

std::vector<Item*> DropItems(Game& game, Army* hero, int x, int y, int current_side) {
    std::vector<Item*> dropped;
    if (hero->items.empty()) return dropped;
    if (current_side == kNone && game.side) current_side = game.side->index;
    bool drowned = scn::TerrainAt(*game.map, x, y) == move::kWater;
    for (Item* item : hero->items) {
        item->planted = false;
        if (drowned) {
            item->status = 0;
            item->x = kNone;
            item->y = kNone;
        } else {
            item->status = 1;
            item->x = x;
            item->y = y;
            dropped.push_back(item);
        }
        if (current_side != kNone && item->index == current_side) {
            for (auto& city : game.map->cities) {
                if (city.vector_to == -2 && city.owner_index == current_side)
                    city.vector_to = kNone;
            }
        }
    }
    hero->items.clear();
    return dropped;
}

}  // namespace w2::hero
