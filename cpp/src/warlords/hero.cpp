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

const std::vector<std::pair<std::string, bool>>& Names(Game& g, const Side& side) {
    auto it = g.hero_names.find(side.index);
    if (it != g.hero_names.end()) return it->second;
    std::vector<std::pair<std::string, bool>> list;
    auto text = ReadFile(g.data_dir + "/TERRAIN0/HERONAM" + std::to_string(side.index) + ".DAT");
    for (auto& line : SplitLines(text ? *text : "")) {
        // ^#(\d)\s+(.+)$
        if (line.size() >= 3 && line[0] == '#' && line[1] >= '0' && line[1] <= '9' &&
            (line[2] == ' ' || line[2] == '\t')) {
            size_t i = 2;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (i < line.size()) list.emplace_back(line.substr(i), line[1] == '1');
        }
    }
    return g.hero_names[side.index] = list;
}

std::pair<std::string, bool> RollName(Game& g, const Side& side) {
    const auto& list = Names(g, side);
    int n = g.rng.Dice(1, kNameRoll, 0);
    if (n - 1 < static_cast<int>(list.size())) return list[n - 1];
    if (list.empty()) return {"Hero", false};
    return list.back();
}

static std::pair<int, int> CountHeroes(const Game& g, const Side* side) {
    int all = 0, mine = 0;
    for (Army* a : g.armies) {
        if (a->type == armytype::kHero) {
            all++;
            if (side && a->owner == side->index) mine++;
        }
    }
    return {all, mine};
}

std::shared_ptr<HeroOffer> Offer(Game& g, Side& side) {
    auto named = [&](HeroOffer o) {
        auto [n, f] = RollName(g, side);
        o.name = n;
        o.female = f;
        return std::make_shared<HeroOffer>(o);
    };
    // none at all once the surrender has been offered (7563:0000)
    if (g.surrender_offered) return nullptr;
    if (g.turn == 1) return named(HeroOffer{0, side.capital, true, "", false});

    auto [all, mine] = CountHeroes(g, &side);
    if (all >= kMaxInGame) return nullptr;
    auto cities = game::SideCities(g, side);
    int limit = static_cast<int>(cities.size()) >= kBigSideCities ? kMaxPerBigSide : kMaxPerSide;
    if (mine >= limit) return nullptr;

    int price = mine == 0 ? g.rng.Dice(1, 400, 300) : g.rng.Dice(1, 600, 1000);
    if (price > side.gold) return nullptr;
    if (g.rng.Dice(1, 30, 0) >= 7) return nullptr;  // a 20% chance

    City* city = g.rng.Pick(cities, static_cast<City*>(nullptr));
    if (!city) return nullptr;
    // a computer's hero appears where its AI wants one (ai_hero_city, 5db9:0919)
    if (side.computer) city = ai::HeroCity(g, side, city);
    return named(HeroOffer{price, city, false, "", false});
}

const ArmyType* MagicalType(Game& g) {
    std::vector<int> ids;
    for (int id = 0; id <= 27; id++) {
        const ArmyType* t = g.types.ById(id);
        if (t && t->bonus[48] != 0) ids.push_back(id);
    }
    int n = g.rng.Dice(1, static_cast<int>(ids.size()), -1);
    return g.types.ById(n >= 0 && n < static_cast<int>(ids.size()) ? ids[n] : 25);
}

std::pair<const ArmyType*, int> Allies(Game& g) {
    const ArmyType* type = MagicalType(g);
    int roll = g.rng.Dice(1, 100, 0);
    int n = roll < 70 ? 1 : roll < 95 ? 2 : 3;
    return {type, n};
}

Army NewAlly(const ArmyType* type, int x, int y, int owner) {
    Army a;
    a.x = x;
    a.y = y;
    a.owner = owner;
    a.type = type->id;
    a.name = type->name;
    a.strength = type->strength;
    a.max_moves = type->move;
    a.moves = type->move;
    a.upkeep = 0;
    a.home_city = 0;
    return a;
}

Army* PlaceAlly(Game& g, const ArmyType* type, int x, int y, const Side& side) {
    int px = x, py = y;
    for (int tries = 10; tries > 0; tries--) {
        if (px >= 0 && py >= 0 && px < g.map->width && py < g.map->height) {
            auto here = game::ArmiesAt(g, px, py);
            if (here.size() < 8 && (here.empty() || here[0]->owner == side.index)) {
                return g.Add(NewAlly(type, px, py, side.index));
            }
        }
        px += g.rng.Dice(1, 3, -2);
        py += g.rng.Dice(1, 3, -2);
    }
    return nullptr;
}

Recruited Recruit(Game& g, Side& side, const HeroOffer& off) {
    City* city = off.city;
    side.gold = std::max(0, side.gold - off.price);
    std::pair<int, int> last{kNone, kNone};
    auto [hx, hy] = game::FreeTileIn(g, *city, true, &last);
    if (hx == kNone) {
        hx = last.first != kNone ? last.first : city->x;
        hy = last.second != kNone ? last.second : city->y;
    }

    Army h;
    h.x = hx;
    h.y = hy;
    h.owner = side.index;
    h.type = armytype::kHero;
    h.name = off.name.empty() ? "Hero" : off.name;
    h.female = off.female;
    h.strength = kStartStrength;
    h.max_moves = kStartMoves;
    h.moves = kStartMoves;
    h.upkeep = 0;
    h.home_city = city->index;
    h.level = 1;
    h.experience = 0;
    if (off.first) {
        // Turn 1: the hero carries its side's standard, item number = side
        if (side.index < static_cast<int>(g.map->items.size())) {
            Item* std = &g.map->items[side.index];
            std->status = 3;
            std->x = kNone;
            std->y = kNone;
            std->standard_of = side.index;
            h.items.push_back(std);
        }
    }
    Recruited out;
    out.hero = g.Add(h);
    history::RecordDeed(g, &side, DeedType::kHeroEmerges, city->index, 0,
                        out.hero->name);  // 7563:04e7

    if (!off.first) {
        auto [type, n] = Allies(g);
        out.type = type;
        out.count = n;
        for (int i = 0; i < n; i++) {
            Army* a = PlaceAlly(g, type, hx, hy, side);
            if (a) out.allies.push_back(a);
        }
    }
    return out;
}

void AddExperience(Game& g, Army* h, int n) {
    h->experience = std::min(rules::kMaxHeroXp, h->experience + n);
}

std::vector<Promotion> CheckPromotions(Game& g, const Side& side) {
    std::vector<Promotion> promoted;
    for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
        Army* a = g.armies[i];
        if (a->type == armytype::kHero && a->owner == side.index) {
            int level = a->level ? a->level : 1;
            int need = PromotionAt(level);
            if (need != kNone && a->experience >= need) {
                int before = a->strength;
                a->level = level + 1;
                a->strength = std::min(kMaxStrength, before + 1);
                a->max_moves += kPromotionMoves;
                a->title = LevelName(a->level);
                promoted.push_back(Promotion{a, a->strength - before});
            }
        }
    }
    return promoted;
}

void BattleExperience(Game& g, const std::vector<Army*>& attackers,
                      const std::vector<Army*>& defenders, const Battle& result, bool was_city) {
    for (Army* a : attackers) {
        if (a->type == armytype::kHero && !result.dead_by_army.count(a))
            AddExperience(g, a, was_city ? 2 : 1);
    }
    for (size_t i = 0; i < defenders.size(); i++) {
        Army* d = defenders[i];
        if (!result.dead_by_army.count(d)) {
            bool counts;
            if (rules::bugs.hero_experience_reads_attacker_types) {
                Army* same_slot = i < attackers.size() ? attackers[i]
                                                       : nullptr;  // the wrong line, on purpose
                counts = same_slot && same_slot->type == armytype::kHero;
            } else {
                counts = true;
            }
            if (counts && d->type == armytype::kHero) AddExperience(g, d, 1);
        }
    }
}

std::vector<Item*> ItemsHere(Game& g, const Army* h) {
    std::vector<Item*> carried = h->items;
    std::vector<Item*> ground;
    for (auto& it : g.map->items)
        if (it.status == 1 && it.x == h->x && it.y == h->y) ground.push_back(&it);
    auto by_index = [](Item* a, Item* b) { return a->index < b->index; };
    LuaSort(carried, by_index);
    LuaSort(ground, by_index);
    carried.insert(carried.end(), ground.begin(), ground.end());
    return carried;
}

void DropItem(Game& g, Army* h, Item* it) {
    auto i = std::find(h->items.begin(), h->items.end(), it);
    if (i != h->items.end()) h->items.erase(i);
    if (scn::TerrainAt(*g.map, h->x, h->y) == move::kWater) {
        it->status = 0;
        it->x = kNone;
        it->y = kNone;
    } else {
        it->status = 1;
        it->x = h->x;
        it->y = h->y;
        it->planted = false;
    }
}

void TakeItem(Game& g, Army* h, Item* it) {
    it->status = 3;
    it->x = kNone;
    it->y = kNone;
    it->planted = false;
    h->items.push_back(it);
}

std::vector<Item*> DropItems(Game& g, Army* h, int x, int y, int cur) {
    std::vector<Item*> dropped;
    if (h->items.empty()) return dropped;
    if (cur == kNone && g.side) cur = g.side->index;
    bool drowned = scn::TerrainAt(*g.map, x, y) == move::kWater;
    for (Item* it : h->items) {
        it->planted = false;
        if (drowned) {
            it->status = 0;
            it->x = kNone;
            it->y = kNone;
        } else {
            it->status = 1;
            it->x = x;
            it->y = y;
            dropped.push_back(it);
        }
        if (cur != kNone && it->index == cur) {
            for (auto& c : g.map->cities) {
                if (c.vector_to == -2 && c.owner_index == cur) c.vector_to = kNone;
            }
        }
    }
    h->items.clear();
    return dropped;
}

}  // namespace w2::hero
