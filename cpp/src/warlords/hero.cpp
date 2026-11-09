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
    for (Army* army : game.Armies()) {
        if (army->Hero()) {
            all++;
            if (side && army->OwnedBy(side->Id())) mine++;
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
    ally.PlaceAt(TilePos(x, y));
    ally.SetOwner(SideId(owner));
    ally.SetType(ArmyTypeId(type->id));
    ally.SetName(type->name);
    ally.SetStrength(type->strength);
    ally.SetMaxMoves(type->move);
    ally.SetMoves(type->move);
    ally.SetUpkeep(0);
    ally.SetHomeCity(CityId(0));
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
    if (const auto* cached = game_.CachedHeroNames(side.Index())) return *cached;
    CandidateList names;
    auto text = ReadFile(game_.DataDir() / "TERRAIN0" /
                         ("HERONAM" + std::to_string(side.Index()) + ".DAT"));
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
    return game_.CacheHeroNames(side.Index(), std::move(names));
}

HeroSystem::CandidateName HeroSystem::RollName(const Side& side) {
    const auto& names = Names(side);
    int roll = game_.GetRng().Dice(1, hero::kNameRoll, 0);
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
    if (game_.SurrenderOffered()) return nullptr;
    if (game_.Turn() == 1)
        return named(HeroOffer{0, side.CapitalCity(game_.GetMap()), true, "", false});

    auto [all, mine] = CountHeroes(game_, &side);
    if (all >= hero::kMaxInGame) return nullptr;
    auto cities = game::SideCities(game_, side);
    int limit = static_cast<int>(cities.size()) >= hero::kBigSideCities ? hero::kMaxPerBigSide
                                                                        : hero::kMaxPerSide;
    if (mine >= limit) return nullptr;

    int price = mine == 0 ? game_.GetRng().Dice(1, 400, 300) : game_.GetRng().Dice(1, 600, 1000);
    if (price > side.Gold()) return nullptr;
    if (game_.GetRng().Dice(1, 30, 0) >= 7) return nullptr;

    City* city = game_.GetRng().Pick(cities, static_cast<City*>(nullptr));
    if (!city) return nullptr;
    if (side.IsComputerControlled() && side.ComputerPlayer()) {
        city = side.ComputerPlayer()->ChooseHeroCity(game_, side, city);
    }
    return named(HeroOffer{price, city, false, "", false});
}

const ArmyType* HeroSystem::MagicalType() {
    std::vector<int> ids;
    for (int id = 0; id <= 27; id++) {
        const ArmyType* type = game_.ArmyTypes().ById(id);
        if (type && type->IsMagical()) ids.push_back(id);
    }
    int pick = game_.GetRng().Dice(1, static_cast<int>(ids.size()), -1);
    return game_.ArmyTypes().ById(pick >= 0 && pick < static_cast<int>(ids.size()) ? ids[pick]
                                                                                   : 25);
}

HeroSystem::AllyRoll HeroSystem::Allies() {
    const ArmyType* type = MagicalType();
    int roll = game_.GetRng().Dice(1, 100, 0);
    int count = roll < 70 ? 1 : roll < 95 ? 2 : 3;
    return {type, count};
}

Army* HeroSystem::PlaceAlly(const ArmyType* type, int x, int y, const Side& side) {
    const Map& map = game_.GetMap();
    int try_x = x, try_y = y;
    for (int tries = 10; tries > 0; tries--) {
        if (map.InBounds(try_x, try_y)) {
            auto armies_here = game::ArmiesAt(game_, try_x, try_y);
            if (armies_here.size() < 8 &&
                (armies_here.empty() || armies_here[0]->OwnedBy(side.Id()))) {
                return game_.Add(hero::NewAlly(type, try_x, try_y, side.Index()));
            }
        }
        try_x += game_.GetRng().Dice(1, 3, -2);
        try_y += game_.GetRng().Dice(1, 3, -2);
    }
    return nullptr;
}

hero::Recruited HeroSystem::Recruit(Side& side, const HeroOffer& offer) {
    City* city = offer.city;
    side.AddGold(-offer.price);
    std::pair<int, int> last{kNone, kNone};
    auto [hero_x, hero_y] = game::FreeTileIn(game_, *city, true, &last);
    if (hero_x == kNone) {
        hero_x = last.first != kNone ? last.first : city->X();
        hero_y = last.second != kNone ? last.second : city->Y();
    }

    Army hero_proto;
    hero_proto.PlaceAt(TilePos(hero_x, hero_y));
    hero_proto.SetOwner(side.Id());
    hero_proto.SetType(ArmyTypeId(armytype::kHero));
    hero_proto.SetName(offer.name.empty() ? "Hero" : offer.name);
    hero_proto.SetFemale(offer.female);
    hero_proto.SetStrength(hero::kStartStrength);
    hero_proto.SetMaxMoves(hero::kStartMoves);
    hero_proto.SetMoves(hero::kStartMoves);
    hero_proto.SetUpkeep(0);
    hero_proto.SetHomeCity(city->Id());
    hero_proto.SetLevel(1);
    hero_proto.SetExperience(0);
    Map& map = game_.GetMap();
    if (offer.first && side.Index() < map.ItemCount()) {
        Item* standard = &map.Items()[side.Index()];
        standard->status = 3;
        standard->x = kNone;
        standard->y = kNone;
        standard->standard_of = side.Index();
        hero_proto.AddItem(standard);
    }
    hero::Recruited recruited;
    recruited.hero = game_.Add(hero_proto);
    history::RecordDeed(game_, &side, DeedType::kHeroEmerges, city->Index(), 0,
                        recruited.hero->Name());

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
    hero->SetExperience(std::min(rules::kMaxHeroXp, hero->Experience() + amount));
}

std::vector<Promotion> HeroSystem::CheckPromotions(const Side& side) {
    std::vector<Promotion> promoted;
    for (int i = static_cast<int>(game_.Armies().size()) - 1; i >= 0; i--) {
        Army* army = game_.Armies()[i];
        if (army->Hero() && army->OwnedBy(side.Id())) {
            int level = army->Level() ? army->Level() : 1;
            int needed = hero::PromotionAt(level);
            if (needed != kNone && army->Experience() >= needed) {
                int before = army->Strength();
                army->SetLevel(level + 1);
                army->SetStrength(std::min(hero::kMaxStrength, before + 1));
                army->AddMaxMoves(hero::kPromotionMoves);
                army->SetTitle(hero::LevelName(army->Level()));
                promoted.push_back(Promotion{army, army->Strength() - before});
            }
        }
    }
    return promoted;
}

void HeroSystem::BattleExperience(const ArmyStack& attackers, const ArmyStack& defenders,
                                  const Battle& result, bool was_city) {
    for (Army* attacker : attackers) {
        if (attacker->Hero() && !result.dead_by_army.count(attacker))
            AddExperience(attacker, was_city ? 2 : 1);
    }
    for (size_t i = 0; i < defenders.size(); i++) {
        Army* defender = defenders[i];
        if (!result.dead_by_army.count(defender)) {
            bool counts;
            if (game_.RulesBugs().hero_experience_reads_attacker_types) {
                Army* same_slot = i < attackers.size() ? attackers[i] : nullptr;
                counts = same_slot && same_slot->Hero();
            } else {
                counts = true;
            }
            if (counts && defender->Hero()) AddExperience(defender, 1);
        }
    }
}

std::vector<Item*> HeroSystem::ItemsHere(const Army* hero) {
    std::vector<Item*> carried = hero->Items();
    std::vector<Item*> ground;
    for (auto& item : game_.GetMap().Items()) {
        if (item.status == 1 && item.x == hero->X() && item.y == hero->Y()) ground.push_back(&item);
    }
    auto by_index = [](Item* left, Item* right) { return left->index < right->index; };
    LuaSort(carried, by_index);
    LuaSort(ground, by_index);
    carried.insert(carried.end(), ground.begin(), ground.end());
    return carried;
}

void HeroSystem::DropItem(Army* hero, Item* item) {
    hero->RemoveItem(item);
    if (scn::TerrainAt(game_.GetMap(), hero->X(), hero->Y()) == move::kWater) {
        item->status = 0;
        item->x = kNone;
        item->y = kNone;
    } else {
        item->status = 1;
        item->x = hero->X();
        item->y = hero->Y();
        item->planted = false;
    }
}

void HeroSystem::TakeItem(Army* hero, Item* item) {
    item->status = 3;
    item->x = kNone;
    item->y = kNone;
    item->planted = false;
    hero->AddItem(item);
}

std::vector<Item*> HeroSystem::DropItems(Army* hero, int x, int y, int current_side) {
    std::vector<Item*> dropped;
    if (hero->Items().empty()) return dropped;
    if (current_side == kNone && game_.CurrentSide()) current_side = game_.CurrentSide()->Index();
    Map& map = game_.GetMap();
    bool drowned = scn::TerrainAt(map, x, y) == move::kWater;
    for (Item* item : hero->TakeAllItems()) {
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
            for (auto& city : map.Cities()) {
                if (city.IsVectoredToStandard() && city.OwnedBy(current_side)) city.ClearVector();
            }
        }
    }
    return dropped;
}

}  // namespace w2
