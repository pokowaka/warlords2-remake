/**
 * @file
 * @brief GoogleTest suite for heroes, items, sages, sites, quests, tutorial invulnerability, and
 * bug-compatibility flags.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using namespace w2::test;

TEST(SageTest, RichSiteDiscoveryGemPayoutAndHiddenMapPatch) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(31));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    int hero_x = side->capital->x;
    int hero_y = side->capital->y;
    for (auto& map_site : game.map->sites) {
        map_site.rich = false;
        map_site.revealed = 0xff;
        map_site.searched = false;
    }
    EXPECT_TRUE(site::SageList(game, *side, hero_x, hero_y).empty());

    auto dist_sq = [&](const Site* target) {
        return (target->x - hero_x) * (target->x - hero_x) +
               (target->y - hero_y) * (target->y - hero_y);
    };
    std::vector<Site*> nearby;
    for (auto& map_site : game.map->sites) {
        if (std::sqrt(static_cast<double>(dist_sq(&map_site))) < site::kSageRange) {
            nearby.push_back(&map_site);
        }
    }
    std::stable_sort(nearby.begin(), nearby.end(),
                     [&](Site* left, Site* right) { return dist_sq(left) < dist_sq(right); });
    ASSERT_GE(nearby.size(), 3U);

    Site* near_gold = nearby[0];
    Site* item_site = nearby[1];
    Site* far_gold = nearby[2];
    for (Site* rich_site : {far_gold, near_gold, item_site}) {
        rich_site->rich = true;
        rich_site->revealed = 0;
    }
    far_gold->content = site::kGold;
    near_gold->content = site::kGold;
    item_site->content = site::kItem;
    item_site->item = game.map->items[9].index;

    auto list = site::SageList(game, *side, hero_x, hero_y);
    ASSERT_EQ(list.size(), 2U);
    const site::SageEntry* gold_ptr = nullptr;
    for (const auto& entry : list) {
        if (entry.kind == "gold") gold_ptr = &entry;
    }
    ASSERT_NE(gold_ptr, nullptr);
    site::SageEntry gold_entry = *gold_ptr;
    EXPECT_EQ(site::SageShow(game, *side, gold_entry, hero_x, hero_y), near_gold);
    EXPECT_EQ((near_gold->revealed >> side->index) & 1, 1);
    EXPECT_EQ(site::SageShow(game, *side, gold_entry, hero_x, hero_y), far_gold);

    auto remaining = site::SageList(game, *side, hero_x, hero_y);
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining[0].item, &game.map->items[9]);

    int gold_before = side->gold;
    int gem = site::SageGem(game, *side);
    EXPECT_TRUE(gem >= 503 && gem <= 2000);
    EXPECT_EQ(side->gold, gold_before + gem);

    game.map->options.hidden_map = 1;
    game.explored.clear();
    auto patch = site::SageMap(game, *side, 3, 3);
    EXPECT_EQ(patch[0], 0);
    EXPECT_TRUE(patch[2] >= 16 && patch[2] <= 25 && patch[3] >= 16 && patch[3] <= 25);
    EXPECT_TRUE(game::Seen(game, side->index, patch[0] + patch[2] - 1, patch[1] + patch[3] - 1));
}

TEST(HeroItemsTest, StandardTakeDropPlantFlagAndVectorToStandard) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(29));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* hero = hero::Recruit(game, *side, *side->hero_offer).hero;
    EXPECT_EQ(hero->items[0], &game.map->items[side->index]);
    EXPECT_EQ(hero->items[0]->status, 3);
    EXPECT_EQ(hero::ItemsHere(game, hero).size(), 1U);

    Item* item = &game.map->items[8];
    item->status = 1;
    item->x = hero->x;
    item->y = hero->y;
    auto here = hero::ItemsHere(game, hero);
    ASSERT_EQ(here.size(), 2U);
    EXPECT_EQ(here[1], item);

    hero::TakeItem(game, hero, item);
    EXPECT_TRUE(item->status == 3 && hero->items.size() == 2U);

    hero::DropItem(game, hero, item);
    EXPECT_TRUE(item->status == 1 && item->x == hero->x && hero->items.size() == 1U);

    EXPECT_FALSE(game::PlantFlag(game, *side, hero));
    int flag_x = kNone;
    int flag_y = kNone;
    for (int dy = -3; dy <= 4 && flag_x == kNone; ++dy) {
        for (int dx = -3; dx <= 4 && flag_x == kNone; ++dx) {
            int x = hero->x + dx;
            int y = hero->y + dy;
            int terrain = scn::TerrainAt(*game.map, x, y);
            if ((terrain == movement::kPlain || terrain == movement::kRoad) &&
                !game::CityAt(game, x, y)) {
                flag_x = x;
                flag_y = y;
            }
        }
    }
    ASSERT_NE(flag_x, kNone);
    hero->x = flag_x;
    hero->y = flag_y;
    EXPECT_TRUE(game::PlantFlag(game, *side, hero));
    EXPECT_EQ(game::StandardAt(game, *side).first, flag_x);
    EXPECT_FALSE(game::PlantFlag(game, *side, hero));

    City* city = side->capital;
    game::SetProduction(game, *city, 0);
    city->countdown = 1;
    EXPECT_TRUE(game::VectorToStandard(game, *city, *side));
    size_t count_before = game::ArmiesAt(game, flag_x, flag_y).size();
    for (size_t i = 0; i < 3 * game.sides.size() + 1; ++i) EndTurn(game);
    EXPECT_GT(game::ArmiesAt(game, flag_x, flag_y).size(), count_before);

    hero::TakeItem(game, hero, &game.map->items[side->index]);
    EXPECT_EQ(game::StandardAt(game, *side).first, kNone);
}

TEST(TutorialHeroTest, HeroCannotDieAttackingNeutralsWhenTutorialEnabled) {
    auto game_ptr = SetupGame("TUTORIA", Seed(31));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    EXPECT_TRUE(game.map->options.tutorial == 1 && !side->computer);

    Army hero;
    hero.type = armytype::kHero;
    hero.strength = 1;
    hero.owner = side->index;
    std::vector<Army> defender_pool(8);
    std::vector<Army*> defenders;
    for (auto& defender : defender_pool) {
        defender.type = 11;
        defender.strength = 9;
        defender.owner = kNone;
        defenders.push_back(&defender);
    }
    int deaths = 0;
    for (int i = 0; i < 50; ++i) {
        deaths += static_cast<int>(
                combat::Resolve(game, {&hero}, defenders, 0, 0).dead_attackers.size());
    }
    EXPECT_EQ(deaths, 0);

    game.map->options.tutorial = 0;
    for (int i = 0; i < 50; ++i) {
        deaths += static_cast<int>(
                combat::Resolve(game, {&hero}, defenders, 0, 0).dead_attackers.size());
    }
    EXPECT_GT(deaths, 0);
}

TEST(HeroesTest, RecruitmentPromotionsAlliesCapsAndItemDrops) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(37));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    auto offer = side->hero_offer;
    ASSERT_NE(offer, nullptr);
    EXPECT_TRUE(offer->price == 0 && offer->city == side->capital);

    const auto& names = hero::Names(game, *side);
    EXPECT_EQ(names.size(), 100U);
    EXPECT_FALSE(offer->name.empty());
    bool known_name = false;
    for (const auto& entry : names) {
        if (entry.first == offer->name) known_name = true;
    }
    EXPECT_TRUE(known_name);
    for (const auto& entry : hero::Names(game, game.map->sides[0])) EXPECT_FALSE(entry.second);

    auto hired = hero::Recruit(game, *side, *offer);
    Army* hero = hired.hero;
    EXPECT_TRUE(hero->type == armytype::kHero && hero->name == offer->name);
    EXPECT_TRUE(hero->strength == 5 && hero->max_moves == 14 && hero->moves == 14);
    EXPECT_TRUE(hired.allies.empty() && hero->items.size() == 1U);
    EXPECT_EQ(hero->items[0]->type, rules::kItemStandard);

    hero->experience = 15;
    EXPECT_EQ(hero::CheckPromotions(game, *side).size(), 1U);
    EXPECT_TRUE(hero->level == 2 && hero->title == "Cavalier" && hero->strength == 6 &&
                hero->max_moves == 16);
    EXPECT_TRUE(hero::CheckPromotions(game, *side).empty());

    hero->experience = 60;
    hero::CheckPromotions(game, *side);
    EXPECT_EQ(hero->level, 3);
    hero::CheckPromotions(game, *side);
    EXPECT_EQ(hero->title, "Paladin");
    EXPECT_TRUE(hero::CheckPromotions(game, *side).empty());
    hero::AddExperience(game, hero, 99);
    EXPECT_EQ(hero->experience, rules::kMaxHeroXp);

    auto [ally_type, ally_count] = hero::Allies(game);
    EXPECT_TRUE(ally_type != nullptr && ally_count >= 1 && ally_count <= 3);

    game.turn = 4;
    HeroOffer later_offer;
    later_offer.city = side->capital;
    auto escort = hero::Recruit(game, *side, later_offer).allies;
    EXPECT_FALSE(escort.empty());
    for (const Army* ally : escort)
        EXPECT_TRUE(ally->moves == ally->max_moves && ally->upkeep == 0);
    game.turn = 1;
    EXPECT_TRUE(hero::kMaxPerSide == 5 && hero::kMaxInGame == 40);
    for (int i = 0; i < 5; ++i) AddArmy(game, 0, 0, side->index, armytype::kHero, 5);
    game.turn = 2;
    for (int i = 0; i < 50; ++i) EXPECT_EQ(hero::Offer(game, *side), nullptr);

    TilePos plain = FindTerrainTile(game, movement::kPlain);
    TilePos water = FindTerrainTile(game, movement::kWater);
    ASSERT_TRUE(plain.IsValid() && water.IsValid());

    Item fire;
    fire.name = "Firesword";
    fire.type = rules::kItemBattle;
    fire.value = 1;
    Army carrier;
    carrier.type = armytype::kHero;
    carrier.owner = side->index;
    carrier.items = {&fire};
    auto dropped = hero::DropItems(game, &carrier, plain.x, plain.y);
    ASSERT_EQ(dropped.size(), 1U);
    EXPECT_TRUE(dropped[0]->status == 1 && dropped[0]->x == plain.x && carrier.items.empty());

    Item ice;
    ice.name = "Icesword";
    ice.type = rules::kItemBattle;
    ice.value = 1;
    ice.status = 3;
    Army drowner;
    drowner.type = armytype::kHero;
    drowner.owner = side->index;
    drowner.items = {&ice};
    EXPECT_TRUE(hero::DropItems(game, &drowner, water.x, water.y).empty());
    EXPECT_EQ(ice.status, 0);
}

TEST(HeroExperienceTest, BugCompatibilityFlagAndBattleAwards) {
    ScopedBugFlags bug_guard;
    auto game_ptr = SetupGame("ERYTHEA", Seed(41));
    Game& game = *game_ptr;
    auto defender_xp = [&]() {
        Army def_hero;
        Army attacker;
        def_hero.type = armytype::kHero;
        def_hero.strength = 5;
        def_hero.owner = 1;
        attacker.type = 11;
        attacker.strength = 3;
        attacker.owner = 0;
        Battle battle;
        hero::BattleExperience(game, {&attacker}, {&def_hero}, battle, false);
        return def_hero.experience;
    };
    rules::bugs.hero_experience_reads_attacker_types = true;
    EXPECT_EQ(defender_xp(), 0);
    rules::bugs.hero_experience_reads_attacker_types = false;
    EXPECT_EQ(defender_xp(), 1);
    rules::bugs.hero_experience_reads_attacker_types = true;

    Army hero;
    hero.type = armytype::kHero;
    hero.strength = 5;
    hero.owner = 0;
    Battle clean_battle;
    hero::BattleExperience(game, {&hero}, {}, clean_battle, false);
    EXPECT_EQ(hero.experience, 1);
    hero::BattleExperience(game, {&hero}, {}, clean_battle, true);
    EXPECT_EQ(hero.experience, 3);

    Army dead_hero;
    dead_hero.type = armytype::kHero;
    dead_hero.strength = 5;
    dead_hero.owner = 0;
    Battle fatal_battle;
    fatal_battle.dead_by_army.insert(&dead_hero);
    hero::BattleExperience(game, {&dead_hero}, {}, fatal_battle, true);
    EXPECT_EQ(dead_hero.experience, 0);
}

/** @brief Parameterized suite testing site contents, ruins, temples, and guardians. */
class SitesScenarioTest : public ScenarioTest {};

TEST_P(SitesScenarioTest, RuinsItemsTemplesAndGuardianSurvival) {
    auto game_ptr = SetupGame(GetParam(), Seed(53));
    Game& game = *game_ptr;
    int rich_count = 0;
    for (const auto& map_site : game.map->sites) {
        EXPECT_NE(map_site.content, site::kEmpty);
        if (map_site.rich) {
            ++rich_count;
            EXPECT_NE(map_site.type, site::kTemple);
        }
        if (map_site.content == site::kAllies) {
            EXPECT_TRUE(map_site.ally_type != kNone &&
                        game.types.ById(map_site.ally_type) != nullptr &&
                        map_site.guardian == kNone);
        } else if (map_site.content != site::kTemple) {
            EXPECT_TRUE(map_site.guardian >= 1 && map_site.guardian <= 9);
        }
    }
    EXPECT_EQ(rich_count, static_cast<int>(game.map->sites.size()) * 3 / 10);

    std::set<int> placed_items;
    for (const auto& map_site : game.map->sites) {
        if (map_site.content == site::kItem) EXPECT_TRUE(placed_items.insert(map_site.item).second);
    }
    std::map<int, Item*> by_index;
    for (auto& item : game.map->items) {
        by_index[item.index] = &item;
        if (placed_items.count(item.index)) EXPECT_EQ(item.status, 2);
    }
    for (const auto& map_site : game.map->sites) {
        if (map_site.content == site::kItem) {
            EXPECT_EQ(site::ItemReserved(*by_index[map_site.item]), map_site.rich);
        }
    }
    if (game.map->item_pool) {
        std::set<std::string> pool_names;
        for (const auto& pool_item : *game.map->item_pool) pool_names.insert(pool_item.name);
        for (int i = 8; i <= 21; ++i) {
            if (by_index.count(i)) EXPECT_EQ(pool_names.count(by_index[i]->name), 1U);
        }
        for (int i = 0; i <= 7; ++i) {
            if (by_index.count(i)) EXPECT_EQ(by_index[i]->type, rules::kItemStandard);
        }
    }

    Site* gold_ruin = FindSiteByContent(game, site::kGold);
    if (gold_ruin != nullptr) {
        Side* side = game.sides[0];
        Army* grunt = AddArmy(game, gold_ruin->x, gold_ruin->y, side->index, 11, 3, 10, 10);
        EXPECT_EQ(site::Search(game, {grunt}, gold_ruin->x, gold_ruin->y)->kind, "no hero");
        EXPECT_FALSE(gold_ruin->searched);

        Army* hero =
                AddArmy(game, gold_ruin->x, gold_ruin->y, side->index, armytype::kHero, 9, 14, 14);
        int gold_before = side->gold;
        auto result = site::Search(game, {hero, grunt}, gold_ruin->x, gold_ruin->y);
        EXPECT_TRUE(result->kind == "gold" || result->kind == "killed");
        if (result->kind == "gold") {
            EXPECT_TRUE(result->gold >= 503 && result->gold <= 4000);
            EXPECT_EQ(side->gold, gold_before + result->gold);
            EXPECT_EQ(hero->experience, 3);
            EXPECT_TRUE(gold_ruin->searched);
            EXPECT_EQ(site::Search(game, {hero}, gold_ruin->x, gold_ruin->y), nullptr);
        } else {
            EXPECT_FALSE(gold_ruin->searched);
        }
    }

    for (auto& map_site : game.map->sites) {
        if (map_site.content == site::kTemple && map_site.temple_index == 0) {
            Army visitor;
            visitor.x = map_site.x;
            visitor.y = map_site.y;
            visitor.owner = game.sides[0]->index;
            visitor.type = 11;
            visitor.strength = 3;
            visitor.moves = 0;
            EXPECT_EQ(site::Search(game, {&visitor}, map_site.x, map_site.y), nullptr);
            visitor.moves = 10;
            auto blessed = site::Search(game, {&visitor}, map_site.x, map_site.y, true);
            EXPECT_TRUE(blessed->kind == "temple" && blessed->blessed == 1 &&
                        visitor.strength == 4);
            EXPECT_EQ(site::Search(game, {&visitor}, map_site.x, map_site.y)->blessed, 0);
            visitor.strength = 9;
            visitor.blessings.clear();
            site::Search(game, {&visitor}, map_site.x, map_site.y);
            EXPECT_EQ(visitor.strength, 9);
            break;
        }
    }

    Army strong_hero;
    Army weak_hero;
    strong_hero.strength = 9;
    weak_hero.strength = 1;
    int survived = 0;
    int died = 0;
    for (int i = 0; i < 400; ++i) {
        if (site::SurvivesGuardian(game, strong_hero, 3)) ++survived;
        if (!site::SurvivesGuardian(game, weak_hero, 8)) ++died;
    }
    EXPECT_EQ(survived, 400);
    EXPECT_GT(died, 0);
}

// NOLINTNEXTLINE(readability-identifier-length)
INSTANTIATE_TEST_SUITE_P(ErytheaAndDragon, SitesScenarioTest,
                         ::testing::Values("ERYTHEA", "DRAGON"), ScenarioParamName);

TEST(QuestsTest, AssignmentCompletionFailureModesAndRewards) {
    auto off_game = SetupGame("ERYTHEA", Seed(71));
    Side* side_off = game::Begin(*off_game);
    Army hero_off;
    hero_off.owner = side_off->index;
    hero_off.type = armytype::kHero;
    hero_off.strength = 5;
    EXPECT_EQ(quest::Assign(*off_game, *side_off, &hero_off), nullptr);

    auto game_ptr = SetupGame("ERYTHEA", SeedWith(71, {{"quests", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    Army* hero = AddArmy(game, side->capital->x, side->capital->y, side->index, armytype::kHero, 5);
    auto assigned = quest::Assign(game, *side, hero);
    ASSERT_NE(assigned, nullptr);
    EXPECT_TRUE(assigned->type >= 0 && assigned->type <= 6 && assigned->HasTarget());
    EXPECT_FALSE(quest::Describe(*assigned).empty());
    EXPECT_EQ(quest::Assign(game, *side, hero), nullptr);

    std::map<int, int> table_counts;
    for (int q_type : quest::kTypeTable) ++table_counts[q_type];
    EXPECT_TRUE(table_counts[0] == 1 && table_counts[4] == 2 && table_counts[6] == 2);
    EXPECT_EQ(quest::kTypeTable.size(), 10U);

    auto city_by_one = [&](int idx) { return &game.map->cities[idx - 1]; };
    side->quest = MakeCityQuest(quest::kOccupy, hero, city_by_one(2));
    side->gold = 5000;
    quest::Event occupy_ev;
    occupy_ev.city = city_by_one(2);
    occupy_ev.stack = {hero};
    auto result = quest::Notify(game, *side, "occupy", occupy_ev);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->failed.empty() && side->quest == nullptr && result->reward != nullptr);
    EXPECT_EQ(hero->experience, quest::kExperience);

    bool was_computer = side->computer;
    side->computer = false;
    side->quest_news.reset();
    side->quest = MakeCityQuest(quest::kOccupy, hero, city_by_one(2));
    result = quest::Notify(game, *side, "occupy", occupy_ev);
    EXPECT_EQ(side->quest_news, result);

    side->computer = true;
    side->quest_news.reset();
    side->quest = MakeCityQuest(quest::kOccupy, hero, city_by_one(2));
    quest::Notify(game, *side, "occupy", occupy_ev);
    EXPECT_EQ(side->quest_news, nullptr);
    side->computer = was_computer;
    side->quest_news.reset();

    hero->experience = 0;
    side->quest = MakeCityQuest(quest::kOccupy, hero, city_by_one(3));
    quest::Event no_hero_ev;
    no_hero_ev.city = city_by_one(3);
    result = quest::Notify(game, *side, "occupy", no_hero_ev);
    ASSERT_TRUE(result && !result->failed.empty());
    EXPECT_TRUE(result->why == 0x22 && hero->experience == 0);

    City* raze_target = city_by_one(5);
    int orig_owner = raze_target->owner_index;
    bool orig_razed = raze_target->razed;
    side->quest = MakeCityQuest(quest::kRaze, hero, raze_target);
    raze_target->razed = true;
    result = quest::Notify(game, *side, "turn");
    EXPECT_TRUE(result && !result->failed.empty() && result->why == 0x21);
    raze_target->razed = false;
    raze_target->owner_index = side->index;
    side->quest = MakeCityQuest(quest::kRaze, hero, raze_target);
    result = quest::Notify(game, *side, "turn");
    EXPECT_TRUE(result && !result->failed.empty() && result->why == 0x2a);
    raze_target->razed = orig_razed;
    raze_target->owner_index = orig_owner;

    side->quest = MakeCityQuest(quest::kOccupy, hero, city_by_one(4));
    quest::Event raze_ev;
    raze_ev.city = city_by_one(4);
    raze_ev.stack = {hero};
    result = quest::Notify(game, *side, "raze", raze_ev);
    EXPECT_TRUE(result && !result->failed.empty());

    Side* victim = game.sides.back();
    side->quest = MakeSlaughterQuest(hero, victim, 3);
    Army slain_a;
    Army slain_b;
    slain_a.owner = victim->index;
    slain_b.owner = victim->index;
    quest::Event battle_ev;
    battle_ev.stack = {hero};
    battle_ev.killed = {&slain_a, &slain_b};
    EXPECT_EQ(quest::Notify(game, *side, "battle", battle_ev), nullptr);
    EXPECT_EQ(side->quest->done, 2);
    result = quest::Notify(game, *side, "battle", battle_ev);
    EXPECT_TRUE(result && result->failed.empty());

    side->quest = MakeSlaughterQuest(hero, victim, 2);
    battle_ev.stack.clear();
    quest::Notify(game, *side, "battle", battle_ev);
    EXPECT_EQ(side->quest->done, 0);

    side->quest = MakePillageQuest(hero, 100);
    quest::Event pillage_ev{.stack = {hero}, .gold = 60};
    quest::Notify(game, *side, "pillage", pillage_ev);
    EXPECT_EQ(side->quest->done, 60);
    result = quest::Notify(game, *side, "pillage", pillage_ev);
    EXPECT_TRUE(result && result->failed.empty());

    Item quest_item{.index = 99, .type = rules::kItemBattle, .status = 3};
    hero->items = {&quest_item};
    side->quest = MakeItemQuest(hero, &quest_item);
    quest::Event item_ev{.hero = hero};
    result = quest::Notify(game, *side, "item", item_ev);
    EXPECT_TRUE(result && result->failed.empty());
    EXPECT_EQ(std::find(hero->items.begin(), hero->items.end(), &quest_item), hero->items.end());
    EXPECT_EQ(quest_item.status, 0);

    Army prey;
    prey.type = armytype::kHero;
    prey.owner = victim->index;
    side->quest = MakeSlayHeroQuest(hero, &prey);
    quest::Event slay_ev{.killed = {&prey}};
    EXPECT_EQ(quest::Notify(game, *side, "battle", slay_ev), nullptr);
    slay_ev.stack = {hero};
    result = quest::Notify(game, *side, "battle", slay_ev);
    EXPECT_TRUE(result && result->failed.empty());

    Army ghost;
    ghost.type = armytype::kHero;
    side->quest = MakePillageQuest(&ghost, 1);
    result = quest::Notify(game, *side, "turn");
    EXPECT_TRUE(result && !result->failed.empty());

    side->gold = 50;
    Quest with_hero;
    with_hero.hero = hero;
    auto gold_reward = quest::ChooseReward(game, *side, &with_hero);
    EXPECT_TRUE(gold_reward->kind == "gold" && gold_reward->gold >= 1002 &&
                gold_reward->gold <= 3000);
    game.turn = 20;
    side->gold = 5000;
    auto ally_reward = quest::ChooseReward(game, *side, &with_hero);
    EXPECT_TRUE(ally_reward->kind == "allies" && !ally_reward->armies.empty() &&
                ally_reward->armies.size() <= 8);
}

TEST(BugFlagsTest, DefaultFlagsAreEnabledAndReadByEngineSources) {
    EXPECT_TRUE(rules::bugs.hero_experience_reads_attacker_types);
    std::set<std::string> used;
    std::regex flag_read("rules::bugs\\.(\\w+)");
    for (const char* module : {"hero", "combat", "game", "move", "ai", "site", "quest"}) {
        auto text = ReadFile(std::string("cpp/src/warlords/") + module + ".cpp");
        if (!text) continue;
        for (auto iter = std::sregex_iterator(text->begin(), text->end(), flag_read);
             iter != std::sregex_iterator(); ++iter) {
            used.insert((*iter)[1]);
        }
    }
    EXPECT_EQ(used.count("hero_experience_reads_attacker_types"), 1U);
}

}  // namespace
}  // namespace w2
