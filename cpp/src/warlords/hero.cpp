/**
 * @file
 * @brief Implementation of `HeroSystem` and `w2::hero` wrappers.
 */
#include "warlords/hero.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

std::pair<int, int> CountHeroes(const Game& game, const Side* side) {
    int all = 0, mine = 0;
    for (Army* army : game.armies) {
        if (army->type == armytype::kHero) {
            all++;
            if (side && army->owner == side->index) mine++;
        }
    }
    return {all, mine};
}

}  // namespace

namespace hero {

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

const std::vector<std::pair<std::string, bool>>& Names(Game& game, const Side& side) {
    return HeroSystem(game).Names(side);
}

std::pair<std::string, bool> RollName(Game& game, const Side& side) {
    return HeroSystem(game).RollName(side);
}

std::shared_ptr<HeroOffer> Offer(Game& game, Side& side) {
    return HeroSystem(game).Offer(side);
}

const ArmyType* MagicalType(Game& game) {
    return HeroSystem(game).MagicalType();
}

std::pair<const ArmyType*, int> Allies(Game& game) {
    return HeroSystem(game).Allies();
}

Army* PlaceAlly(Game& game, const ArmyType* type, int x, int y, const Side& side) {
    return HeroSystem(game).PlaceAlly(type, x, y, side);
}

Recruited Recruit(Game& game, Side& side, const HeroOffer& offer) {
    return HeroSystem(game).Recruit(side, offer);
}

void AddExperience(Game& game, Army* hero, int amount) {
    HeroSystem(game).AddExperience(hero, amount);
}

std::vector<Promotion> CheckPromotions(Game& game, const Side& side) {
    return HeroSystem(game).CheckPromotions(side);
}

void BattleExperience(Game& game, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city) {
    HeroSystem(game).BattleExperience(attackers, defenders, result, was_city);
}

std::vector<Item*> ItemsHere(Game& game, const Army* hero) {
    return HeroSystem(game).ItemsHere(hero);
}

void DropItem(Game& game, Army* hero, Item* item) {
    HeroSystem(game).DropItem(hero, item);
}

void TakeItem(Game& game, Army* hero, Item* item) {
    HeroSystem(game).TakeItem(hero, item);
}

std::vector<Item*> DropItems(Game& game, Army* hero, int x, int y, int current_side) {
    return HeroSystem(game).DropItems(hero, x, y, current_side);
}

}  // namespace hero

const HeroSystem::CandidateList& HeroSystem::Names(const Side& side) {
    auto cached = game_.hero_names.find(side.index);
    if (cached != game_.hero_names.end()) return cached->second;
    CandidateList names;
    auto text = ReadFile(game_.data_dir / "TERRAIN0" /
                         ("HERONAM" + std::to_string(side.index) + ".DAT"));
    for (auto& line : SplitLines(text.ok() ? *text : "")) {
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
    return game_.hero_names[side.index] = names;
}

HeroSystem::CandidateName HeroSystem::RollName(const Side& side) {
    const auto& names = Names(side);
    int roll = game_.rng.Dice(1, hero::kNameRoll, 0);
    if (roll - 1 < static_cast<int>(names.size())) return names[roll - 1];
    if (names.empty()) return {"Hero", false};
    return names.back();
}

std::shared_ptr<HeroOffer> HeroSystem::Offer(Side& side) {
    auto named = [&](HeroOffer offer) {
        auto [name, female] = RollName(side);
        offer.name = name;
        offer.female = female;
        return std::make_shared<HeroOffer>(offer);
    };
    if (game_.surrender_offered) return nullptr;
    if (game_.turn == 1) return named(HeroOffer{0, side.capital, true, "", false});

    auto [all, mine] = CountHeroes(game_, &side);
    if (all >= hero::kMaxInGame) return nullptr;
    auto cities = game::SideCities(game_, side);
    int limit = static_cast<int>(cities.size()) >= hero::kBigSideCities ? hero::kMaxPerBigSide
                                                                        : hero::kMaxPerSide;
    if (mine >= limit) return nullptr;

    int price = mine == 0 ? game_.rng.Dice(1, 400, 300) : game_.rng.Dice(1, 600, 1000);
    if (price > side.gold) return nullptr;
    if (game_.rng.Dice(1, 30, 0) >= 7) return nullptr;

    City* city = game_.rng.Pick(cities, static_cast<City*>(nullptr));
    if (!city) return nullptr;
    if (side.computer && side.computer_player) {
        city = side.computer_player->ChooseHeroCity(game_, side, city);
    }
    return named(HeroOffer{price, city, false, "", false});
}

const ArmyType* HeroSystem::MagicalType() {
    std::vector<int> ids;
    for (int id = 0; id <= 27; id++) {
        const ArmyType* type = game_.types.ById(id);
        if (type && type->IsMagical()) ids.push_back(id);
    }
    int pick = game_.rng.Dice(1, static_cast<int>(ids.size()), -1);
    return game_.types.ById(pick >= 0 && pick < static_cast<int>(ids.size()) ? ids[pick] : 25);
}

HeroSystem::AllyRoll HeroSystem::Allies() {
    const ArmyType* type = MagicalType();
    int roll = game_.rng.Dice(1, 100, 0);
    int count = roll < 70 ? 1 : roll < 95 ? 2 : 3;
    return {type, count};
}

Army* HeroSystem::PlaceAlly(const ArmyType* type, int x, int y, const Side& side) {
    int try_x = x, try_y = y;
    for (int tries = 10; tries > 0; tries--) {
        if (try_x >= 0 && try_y >= 0 && try_x < game_.map->width && try_y < game_.map->height) {
            auto armies_here = game::ArmiesAt(game_, try_x, try_y);
            if (armies_here.size() < 8 &&
                (armies_here.empty() || armies_here[0]->owner == side.index)) {
                return game_.Add(hero::NewAlly(type, try_x, try_y, side.index));
            }
        }
        try_x += game_.rng.Dice(1, 3, -2);
        try_y += game_.rng.Dice(1, 3, -2);
    }
    return nullptr;
}

hero::Recruited HeroSystem::Recruit(Side& side, const HeroOffer& offer) {
    City* city = offer.city;
    side.gold = std::max(0, side.gold - offer.price);
    std::pair<int, int> last{kNone, kNone};
    auto [hero_x, hero_y] = game::FreeTileIn(game_, *city, true, &last);
    if (hero_x == kNone) {
        hero_x = last.first != kNone ? last.first : city->x;
        hero_y = last.second != kNone ? last.second : city->y;
    }

    Army hero_proto;
    hero_proto.x = hero_x;
    hero_proto.y = hero_y;
    hero_proto.owner = side.index;
    hero_proto.type = armytype::kHero;
    hero_proto.name = offer.name.empty() ? "Hero" : offer.name;
    hero_proto.female = offer.female;
    hero_proto.strength = hero::kStartStrength;
    hero_proto.max_moves = hero::kStartMoves;
    hero_proto.moves = hero::kStartMoves;
    hero_proto.upkeep = 0;
    hero_proto.home_city = city->index;
    hero_proto.level = 1;
    hero_proto.experience = 0;
    if (offer.first && side.index < static_cast<int>(game_.map->items.size())) {
        Item* standard = &game_.map->items[side.index];
        standard->status = 3;
        standard->x = kNone;
        standard->y = kNone;
        standard->standard_of = side.index;
        hero_proto.items.push_back(standard);
    }
    hero::Recruited recruited;
    recruited.hero = game_.Add(hero_proto);
    history::RecordDeed(game_, &side, DeedType::kHeroEmerges, city->index, 0, recruited.hero->name);

    if (!offer.first) {
        auto [type, ally_count] = Allies();
        recruited.type = type;
        recruited.count = ally_count;
        for (int i = 0; i < ally_count; i++) {
            Army* ally = PlaceAlly(type, hero_x, hero_y, side);
            if (ally) recruited.allies.push_back(ally);
        }
    }
    return recruited;
}

void HeroSystem::AddExperience(Army* hero, int amount) {
    hero->experience = std::min(rules::kMaxHeroXp, hero->experience + amount);
}

std::vector<Promotion> HeroSystem::CheckPromotions(const Side& side) {
    std::vector<Promotion> promoted;
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        Army* army = game_.armies[i];
        if (army->type == armytype::kHero && army->owner == side.index) {
            int level = army->level ? army->level : 1;
            int needed = hero::PromotionAt(level);
            if (needed != kNone && army->experience >= needed) {
                int before = army->strength;
                army->level = level + 1;
                army->strength = std::min(hero::kMaxStrength, before + 1);
                army->max_moves += hero::kPromotionMoves;
                army->title = hero::LevelName(army->level);
                promoted.push_back(Promotion{army, army->strength - before});
            }
        }
    }
    return promoted;
}

void HeroSystem::BattleExperience(const ArmyStack& attackers, const ArmyStack& defenders,
                                  const Battle& result, bool was_city) {
    for (Army* attacker : attackers) {
        if (attacker->type == armytype::kHero && !result.dead_by_army.count(attacker))
            AddExperience(attacker, was_city ? 2 : 1);
    }
    for (size_t i = 0; i < defenders.size(); i++) {
        Army* defender = defenders[i];
        if (!result.dead_by_army.count(defender)) {
            bool counts;
            if (game_.RulesBugs().hero_experience_reads_attacker_types) {
                Army* same_slot = i < attackers.size() ? attackers[i] : nullptr;
                counts = same_slot && same_slot->type == armytype::kHero;
            } else {
                counts = true;
            }
            if (counts && defender->type == armytype::kHero) AddExperience(defender, 1);
        }
    }
}

std::vector<Item*> HeroSystem::ItemsHere(const Army* hero) {
    std::vector<Item*> carried = hero->items;
    std::vector<Item*> ground;
    for (auto& item : game_.map->items) {
        if (item.status == 1 && item.x == hero->x && item.y == hero->y) ground.push_back(&item);
    }
    auto by_index = [](Item* left, Item* right) { return left->index < right->index; };
    LuaSort(carried, by_index);
    LuaSort(ground, by_index);
    carried.insert(carried.end(), ground.begin(), ground.end());
    return carried;
}

void HeroSystem::DropItem(Army* hero, Item* item) {
    auto carried = std::find(hero->items.begin(), hero->items.end(), item);
    if (carried != hero->items.end()) hero->items.erase(carried);
    if (scn::TerrainAt(*game_.map, hero->x, hero->y) == move::kWater) {
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

void HeroSystem::TakeItem(Army* hero, Item* item) {
    item->status = 3;
    item->x = kNone;
    item->y = kNone;
    item->planted = false;
    hero->items.push_back(item);
}

std::vector<Item*> HeroSystem::DropItems(Army* hero, int x, int y, int current_side) {
    std::vector<Item*> dropped;
    if (hero->items.empty()) return dropped;
    if (current_side == kNone && game_.side) current_side = game_.side->index;
    bool drowned = scn::TerrainAt(*game_.map, x, y) == move::kWater;
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
            for (auto& city : game_.map->cities) {
                if (city.vector_to == -2 && city.owner_index == current_side)
                    city.vector_to = kNone;
            }
        }
    }
    hero->items.clear();
    return dropped;
}

}  // namespace w2
