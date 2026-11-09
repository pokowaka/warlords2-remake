/**
 * @file
 * @brief GoogleTest suite for combat resolution, city capture, pillage/sack/raze, and binary-
 * verified edge cases.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rng.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::AddArmy;
using test::AddTypedArmy;
using test::AdvanceRounds;
using test::FillCityFootprint;
using test::FindEnemyCity;
using test::FindNeutralCity;
using test::FindSiteByContent;
using test::Seed;
using test::SeedWith;
using test::SetupGame;

TEST(CombatTest, TerrainClassesModifiersBattleLinesOddsAndAdvisor) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(17));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    EXPECT_EQ(combat::kHeroTable[0], 0);
    EXPECT_EQ(combat::kHeroTable[4], 1);
    EXPECT_EQ(combat::kHeroTable[9], 3);
    EXPECT_EQ(combat::kCity, 0);
    EXPECT_EQ(combat::kOpen, 1);
    EXPECT_EQ(combat::kWoods, 2);
    EXPECT_EQ(combat::kHills, 3);

    std::set<int> seen_classes;
    for (int y = 0; y < game.map->height; y += 7) {
        for (int x = 0; x < game.map->width; x += 7) {
            int cls = combat::TerrainClass(game, x, y);
            int terrain = scn::TerrainAt(*game.map, x, y);
            seen_classes.insert(cls);
            if (terrain == movement::kForest) EXPECT_EQ(cls, combat::kWoods);
            if (terrain == movement::kHills || terrain == movement::kMountains) {
                EXPECT_EQ(cls, combat::kHills);
            }
            if (terrain == movement::kCity || terrain == movement::kSite) {
                EXPECT_EQ(cls, combat::kCity);
            }
            if (terrain == movement::kPlain || terrain == movement::kMarsh ||
                terrain == movement::kWater) {
                EXPECT_EQ(cls, combat::kOpen);
            }
        }
    }
    EXPECT_EQ(seen_classes.count(combat::kOpen), 1U);
    EXPECT_EQ(game.map->combat_cap, 5);

    std::set<int> used_ranks(game.map->fight_order[0].begin(),
                             game.map->fight_order[0].begin() + 29);
    EXPECT_EQ(used_ranks.size(), 29U);

    City* neutral = FindNeutralCity(game, 3);
    if (neutral != nullptr) {
        int cls = combat::TerrainClass(game, neutral->x, neutral->y);
        EXPECT_EQ(combat::Fortify(game, {}, neutral->x, neutral->y, cls), 1);
        game::SetCityOwner(game, *neutral, game.sides.back()->index);
        EXPECT_EQ(combat::Fortify(game, {}, neutral->x, neutral->y, cls), 2);
        game::SetCityOwner(game, *neutral, kNone);
        for (const auto& army_type : game.types.all) {
            if (army_type.bonus[52] == combat::kSiege) {
                Army siege_army;
                siege_army.type = army_type.id;
                EXPECT_EQ(combat::Fortify(game, {&siege_army}, neutral->x, neutral->y, cls), 0);
                break;
            }
        }
    }

    Army boat;
    boat.type = 11;
    boat.strength = 9;
    boat.at_sea = true;
    EXPECT_EQ(combat::Strength(game, boat, 5, combat::kOpen, movement::kWater), 4);
    EXPECT_EQ(combat::Strength(game, boat, 5, combat::kOpen, movement::kShore), 4);
    boat.at_sea = false;
    EXPECT_GT(combat::Strength(game, boat, 5, combat::kOpen, movement::kPlain), 4);

    Army strong;
    strong.type = 11;
    strong.strength = 9;
    EXPECT_EQ(combat::Strength(game, strong, 99, combat::kOpen, movement::kPlain), 15);

    Army* lone_attacker = game::SideArmies(game, *side)[0];
    City* enemy = FindEnemyCity(game, side->index);
    ASSERT_NE(enemy, nullptr);
    auto lines = combat::BattleLines(game, {lone_attacker}, enemy->x, enemy->y);
    EXPECT_EQ(lines.attackers.size(), 1U);
    EXPECT_FALSE(lines.defenders.empty());
    EXPECT_EQ(lines.city, enemy);
    for (const Army* defender : lines.defenders) EXPECT_NE(defender->owner, side->index);
    const auto& owner_row = game.map->fight_order[lines.def_owner == kNone ? 8 : lines.def_owner];
    for (size_t i = 1; i < lines.defenders.size(); ++i) {
        EXPECT_GE(owner_row[lines.defenders[i]->type], owner_row[lines.defenders[i - 1]->type]);
    }

    auto battle = combat::Resolve(game, lines.attackers, lines.defenders, enemy->x, enemy->y);
    EXPECT_FALSE(battle.log.empty());
    EXPECT_EQ(battle.log.size(), battle.dead_attackers.size() + battle.dead_defenders.size());
    EXPECT_EQ(battle.won, battle.defenders.empty());

    auto fight_odds = [&](int atk_str, int n_atk, int def_str, int n_def) {
        std::vector<Army> attackers(n_atk);
        std::vector<Army> defenders(n_def);
        std::vector<Army*> attacker_ptrs;
        std::vector<Army*> defender_ptrs;
        for (auto& attacker : attackers) {
            attacker.type = 11;
            attacker.strength = atk_str;
            attacker.owner = side->index;
            attacker_ptrs.push_back(&attacker);
        }
        for (auto& defender : defenders) {
            defender.type = 11;
            defender.strength = def_str;
            defender.owner = kNone;
            defender_ptrs.push_back(&defender);
        }
        int wins = 0;
        for (int i = 0; i < 200; ++i) {
            if (combat::Resolve(game, attacker_ptrs, defender_ptrs, 0, 0).won) ++wins;
        }
        return wins;
    };
    EXPECT_GT(fight_odds(9, 8, 1, 1), 190);
    EXPECT_LT(fight_odds(1, 1, 9, 8), 10);
    EXPECT_GT(fight_odds(3, 8, 9, 1), 150);

    EXPECT_EQ(combat::kAdviceBattles, 19);
    auto [verdict, wins] =
            combat::Advise(game, lines.attackers, lines.defenders, enemy->x, enemy->y);
    EXPECT_TRUE(!verdict.empty() && wins >= 0 && wins <= 19);
    EXPECT_EQ(verdict, std::string(combat::kAdvice[wins / 2]));

    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            auto sub_lines =
                    combat::BattleLines(game, {lone_attacker}, enemy->x + dx, enemy->y + dy);
            EXPECT_EQ(sub_lines.city, enemy);
            EXPECT_EQ(sub_lines.defenders.size(), lines.defenders.size());
        }
    }
    EXPECT_EQ(game.map->options.military_advisor, 1);
}

TEST(CaptureTest, WalkAssaultDecideAndIdempotentApplyPlusLootShares) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(23));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    City* target = FindNeutralCity(game);
    ASSERT_NE(target, nullptr);
    std::vector<Army*> stack;
    for (int i = 0; i < 8; ++i) {
        Army* attacker = AddArmy(game, target->x, target->y - 1, side->index, 11, 9, 20, 20);
        attacker->upkeep = 1;
        stack.push_back(attacker);
    }

    Army* far = AddArmy(game, 0, 0, side->index, 11, 9, 20, 20);
    far->upkeep = 1;
    movement::WalkResult walk;
    for (auto [off_x, off_y] : std::vector<std::pair<int, int>>{
             {-3, -3}, {3, -3}, {-3, 3}, {3, 3}, {0, -3}, {0, 3}, {-3, 0}, {3, 0}}) {
        far->x = target->x + off_x;
        far->y = target->y + off_y;
        far->moves = 20;
        walk = movement::MoveTo(game, {far}, target->x, target->y);
        if (walk.stopped == movement::WalkStop::kAttack) break;
    }
    EXPECT_EQ(walk.stopped, movement::WalkStop::kAttack);
    ASSERT_TRUE(walk.attack.has_value());
    EXPECT_TRUE(walk.attack->x == target->x && walk.attack->y == target->y &&
                walk.attack->city == target);
    EXPECT_EQ(std::max(std::abs(far->x - target->x), std::abs(far->y - target->y)), 1);
    EXPECT_TRUE(far->x != target->x || far->y != target->y);
    EXPECT_EQ(target->owner_index, kNone);
    game.Remove(far);

    int cities_before = static_cast<int>(game::SideCities(game, *side).size());
    City* sender = FindNeutralCity(game, 1, target);
    ASSERT_NE(sender, nullptr);
    sender->producing = 0;
    sender->countdown = 2;
    sender->vector_to = target->index;

    int tile_index = target->y * kMapW + target->x;
    int old_tile = game.map->tiles[tile_index];
    size_t old_armies = game.armies.size();
    int start_x = stack[0]->x;
    Battle battle = game::DecideAttack(game, stack, target->x, target->y);
    EXPECT_TRUE(battle.won && target->owner_index == kNone && battle.captured == nullptr);
    EXPECT_EQ(game.map->tiles[tile_index], old_tile);
    EXPECT_EQ(game.armies.size(), old_armies);
    EXPECT_EQ(stack[0]->x, start_x);

    game::ApplyAttack(game, battle);
    EXPECT_NE(game.map->tiles[tile_index], old_tile);
    size_t post_armies = game.armies.size();
    game::ApplyAttack(game, battle);
    EXPECT_EQ(game.armies.size(), post_armies);
    EXPECT_EQ(target->owner_index, side->index);
    EXPECT_TRUE(sender->vector_to == kNone && sender->producing == kNone);
    EXPECT_EQ(battle.lines.attackers.size(), stack.size());
    EXPECT_EQ(battle.lines.city, target);
    for (int loser : battle.log) EXPECT_TRUE(loser == 0 || loser == 1);
    EXPECT_EQ(battle.log.size(), battle.dead_attackers.size() + battle.dead_defenders.size());
    EXPECT_EQ(static_cast<int>(game::SideCities(game, *side).size()), cities_before + 1);
    EXPECT_EQ(target->producing, kNone);
    for (const Army* survivor : battle.attackers) EXPECT_EQ(survivor->x, target->x);
    for (Army* dead : battle.dead_defenders) EXPECT_FALSE(game.Alive(dead));

    Side* victim = game.sides.back();
    victim->gold = 400;
    EXPECT_EQ(game::Loot(game, victim), 200);
    City* extra = FindNeutralCity(game, 0, target);
    ASSERT_NE(extra, nullptr);
    game::SetCityOwner(game, *extra, victim->index);
    EXPECT_EQ(game::Loot(game, victim), (400 / 2) / 2);
}

TEST(CityChoicesTest, PillageSackAndRazeRewardsAndDiplomaticPenalties) {
    auto game_ptr = SetupGame("ERYTHEA", Seed(29));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    auto claim_neutral = [&](size_t min_slots) -> City* {
        City* cand = FindNeutralCity(game, min_slots);
        if (cand != nullptr) game::SetCityOwner(game, *cand, side->index);
        return cand;
    };

    City* pillage_city = claim_neutral(2);
    ASSERT_NE(pillage_city, nullptr);
    size_t slot_count = pillage_city->slots.size();
    int expected_gold = std::abs(game.types.ById(pillage_city->slots.back().type)->price) / 2;
    int gold_before = side->gold;
    int score_before = side->diplo_score;
    auto pillage_res = game::Pillage(game, *side, *pillage_city, {});
    EXPECT_EQ(pillage_res.gold, expected_gold);
    EXPECT_EQ(side->gold, gold_before + expected_gold);
    EXPECT_EQ(pillage_city->slots.size(), slot_count - 1);
    EXPECT_EQ(pillage_city->defence,
              rules::CityDefence(static_cast<int>(pillage_city->slots.size())));
    EXPECT_TRUE(side->diplo_score - score_before >= 1 && side->diplo_score - score_before <= 5);

    City* sack_city = claim_neutral(3);
    ASSERT_NE(sack_city, nullptr);
    Slot cheapest = sack_city->slots[0];
    int sack_want = 0;
    for (size_t i = 1; i < sack_city->slots.size(); ++i) {
        sack_want += std::abs(game.types.ById(sack_city->slots[i].type)->price) / 2;
    }
    gold_before = side->gold;
    score_before = side->diplo_score;
    slot_count = sack_city->slots.size();
    auto sack_res = game::Sack(game, *side, *sack_city, {});
    EXPECT_EQ(sack_res.gold, sack_want);
    EXPECT_EQ(sack_res.lost.size(), slot_count - 1);
    int lost_sum = 0;
    for (const auto& entry : sack_res.lost) lost_sum += entry.second;
    EXPECT_EQ(lost_sum, sack_want);
    EXPECT_EQ(side->gold, gold_before + sack_want);
    ASSERT_EQ(sack_city->slots.size(), 1U);
    EXPECT_EQ(sack_city->slots[0], cheapest);
    EXPECT_EQ(sack_city->defence, 1);
    EXPECT_TRUE(side->diplo_score - score_before >= 6 && side->diplo_score - score_before <= 15);

    City* raze_city = claim_neutral(1);
    ASSERT_NE(raze_city, nullptr);
    int owned_before = static_cast<int>(game::SideCities(game, *side).size());
    City* other_owned = side->capital != raze_city ? side->capital : pillage_city;
    game::Vector(game, *other_owned, raze_city);
    score_before = side->diplo_score;
    game::Raze(game, *side, *raze_city, {});
    EXPECT_TRUE(raze_city->owner_index == kNone && raze_city->slots.empty() &&
                raze_city->income == 0);
    EXPECT_EQ(static_cast<int>(game::SideCities(game, *side).size()), owned_before - 1);
    EXPECT_EQ(other_owned->vector_to, kNone);
    EXPECT_TRUE(side->diplo_score - score_before >= 11 && side->diplo_score - score_before <= 25);
    int ruin_byte =
            movement::Grid(game, side->index)[raze_city->y * game.map->width + raze_city->x];
    EXPECT_TRUE(!movement::Has(ruin_byte, movement::kCityF) && ruin_byte % 8 != 0);

    City empty_city;
    empty_city.index = -1;
    EXPECT_EQ(game::Pillage(game, *side, empty_city, {}).gold, 0);
    City single_slot_city;
    single_slot_city.index = -1;
    single_slot_city.slots.push_back(Slot{11, "", 0, 0, 0, 0, 0});
    EXPECT_EQ(game::Sack(game, *side, single_slot_city, {}).gold, 0);
}

TEST(VerifiedRulesTest, ArmySlotsProductionRandomTurnsCombatMedalsAndPathCache) {
    {
        auto game = SetupGame("ERYTHEA", Seed(1));
        size_t count_before = game->armies.size();
        game->Remove(game->armies[2]);
        Army* first_added = AddTypedArmy(*game, 0, 0, 0, 11);
        EXPECT_EQ(first_added->slot, 2);
        EXPECT_EQ(std::find(game->armies.begin(), game->armies.end(), first_added) -
                          game->armies.begin(),
                  2);
        EXPECT_EQ(game->armies.size(), count_before);
        Army* second_added = AddTypedArmy(*game, 0, 0, 0, 11);
        EXPECT_EQ(game::ArmiesAt(*game, 0, 0)[0], second_added);
    }
    {
        Rng rng3(3);
        int hits = 0;
        for (int i = 0; i < 20000; ++i) {
            if (rng3.Chance(10)) ++hits;
        }
        EXPECT_TRUE(hits > 1600 && hits < 2000);
        Rng rng1(1);
        for (int i = 0; i < 5000; ++i) EXPECT_FALSE(rng1.Chance(1));
    }
    {
        auto game = SetupGame("ERYTHEA", Seed(2));
        Side* side = game::Begin(*game);
        City* city = side->capital;
        game::SetProduction(*game, *city, 0);
        city->countdown = 1;
        FillCityFootprint(*game, *city, side->index, 11);
        size_t count_before = game->armies.size();
        side->production_due = true;
        game->turn = 2;
        game::HumanProduction(*game, *side);
        EXPECT_EQ(game->armies.size(), count_before);
        EXPECT_TRUE(city->countdown == 0 && !game::Producing(*city));

        for (Army* occupant : game::ArmiesAt(*game, city->x + 1, city->y + 1)) {
            game->Remove(occupant);
        }
        side->production_due = true;
        game::HumanProduction(*game, *side);
        EXPECT_EQ(game->armies.size(), count_before - 8);

        game::SetProduction(*game, *city, 0);
        city->countdown = 1;
        side->production_due = true;
        game::HumanProduction(*game, *side);
        EXPECT_EQ(game->armies.size(), count_before - 7);
        ASSERT_EQ(game::ArmiesAt(*game, city->x + 1, city->y + 1).size(), 1U);
        EXPECT_EQ(city->countdown, city->slots[0].time);
        Army* built = game::ArmiesAt(*game, city->x + 1, city->y + 1)[0];
        EXPECT_EQ(built->moves, built->max_moves);
    }
    {
        auto game = SetupGame("ERYTHEA", Seed(2));
        City& city = game->map->cities[0];
        for (Army* army : std::vector<Army*>(game->armies.begin(), game->armies.end())) {
            if (city.ContainsTile(army->x, army->y)) game->Remove(army);
        }
        for (int i = 0; i < 8; ++i) AddTypedArmy(*game, city.x, city.y, 0, 11);
        EXPECT_EQ(game::FreeTileIn(*game, city, false), std::make_pair(city.x + 1, city.y));
    }
    {
        auto game = SetupGame("ERYTHEA", SeedWith(4, {{"randomTurns", 1}}));
        for (Side* player : game->sides) player->computer = true;
        Side* side = game::Begin(*game);
        constexpr std::array<int, 8> kPlainOrder{0, 1, 2, 3, 4, 5, 6, 7};
        bool shuffled = false;
        while (side != nullptr && game->turn <= 6) {
            if (game->turn_schedule.Order() != kPlainOrder) shuffled = true;
            EXPECT_EQ(SideId(side->index), game->turn_schedule.CurrentSideId());
            side = game::EndTurn(*game);
        }
        EXPECT_TRUE(shuffled);
        auto sorted = game->turn_schedule.Order();
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(sorted, kPlainOrder);

        auto fixed = SetupGame("ERYTHEA", Seed(4));
        game::Begin(*fixed);
        for (int i = 0; i < 30; ++i) game::EndTurn(*fixed);
        EXPECT_EQ(fixed->turn_schedule.Order(), kPlainOrder);
    }
    {
        auto game = SetupGame("ERYTHEA", SeedWith(5, {{"neutralCities", 2}}));
        Side* side = game::Begin(*game);
        City* city = FindNeutralCity(*game, 1);
        for (Army* defender : game::ArmiesAt(*game, city->x, city->y)) defender->strength = 9;
        Army* weak = AddTypedArmy(*game, city->x - 1, city->y, side->index, 11, 1);
        Battle battle = game::DecideAttack(*game, {weak}, city->x, city->y);
        EXPECT_NE(city->attacked & (1 << side->index), 0);
        if (!battle.won) {
            game::ApplyAttack(*game, battle);
            size_t before = game::ArmiesAt(*game, city->x, city->y).size();
            AdvanceRounds(*game, 12);
            EXPECT_GT(game::ArmiesAt(*game, city->x, city->y).size(), before);
            EXPECT_TRUE(game::ArmiesAt(*game, city->x, city->y).size() <= 4 || before >= 4);
        }
    }
    {
        int medals = 0;
        for (int seed = 1; seed <= 60 && medals == 0; ++seed) {
            auto game = SetupGame("ERYTHEA", Seed(seed));
            Side* side = game::Begin(*game);
            City* city = FindNeutralCity(*game);
            for (Army* defender : game::ArmiesAt(*game, city->x, city->y)) defender->strength = 1;
            while (game::ArmiesAt(*game, city->x, city->y).size() < 2) {
                AddTypedArmy(*game, city->x, city->y, kNone, 11, 1);
            }
            std::vector<Army*> stack = {
                AddTypedArmy(*game, city->x - 1, city->y, side->index, armytype::kHero, 9),
                AddTypedArmy(*game, city->x - 1, city->y, side->index, 11, 9),
                AddTypedArmy(*game, city->x - 1, city->y, side->index, 11, 9)};
            Battle decided = game::DecideAttack(*game, stack, city->x, city->y);
            Battle& applied = game::ApplyAttack(*game, decided);
            if (applied.medal != nullptr) {
                ++medals;
                EXPECT_EQ(applied.medal->medals, 1);
                EXPECT_NE(applied.medal->type, armytype::kHero);
            }
        }
        EXPECT_GT(medals, 0);
    }
}

TEST(VerifiedRulesTest, AlliesRuinQuestsSurrenderCacheFloodAndMovementFlags) {
    bool tried_ally_ruin = false;
    for (int seed = 1; seed <= 20 && !tried_ally_ruin; ++seed) {
        auto game = SetupGame("ERYTHEA", Seed(seed));
        Side* side = game::Begin(*game);
        Site* ally_site = FindSiteByContent(*game, site::kAllies);
        if (ally_site == nullptr) continue;
        tried_ally_ruin = true;
        Army* hero =
                AddTypedArmy(*game, ally_site->x, ally_site->y, side->index, armytype::kHero, 1);
        auto search_res = site::Search(*game, {hero}, ally_site->x, ally_site->y, false);
        EXPECT_EQ(search_res->kind, "allies");
        EXPECT_TRUE(game->Alive(hero));
    }
    EXPECT_TRUE(tried_ally_ruin);

    {
        auto game = SetupGame("ERYTHEA", SeedWith(8, {{"quests", 1}}));
        Side* side = game::Begin(*game);
        Army* hero = AddTypedArmy(*game, side->capital->x, side->capital->y, side->index,
                                  armytype::kHero);
        for (int i = 0; i < 40; ++i) {
            side->quest.reset();
            int quest_type = quest::Assign(*game, *side, hero, true)->Type();
            EXPECT_TRUE(quest_type == quest::kOccupy || quest_type == quest::kRaze ||
                        quest_type == quest::kSlaughter || quest_type == quest::kPillageGold);
        }
        bool archons = false;
        Quest quest_obj(quest::kPillageGold, hero->id, std::monostate{}, 500, 500);
        for (int i = 0; i < 60 && !archons; ++i) {
            side->gold = 500;
            auto reward = quest::ChooseReward(*game, *side, &quest_obj);
            if (reward->kind == "allies") {
                archons = true;
                EXPECT_EQ(reward->type->id, quest::kRewardAlly);
            }
        }
        EXPECT_TRUE(archons);
    }
    {
        auto game = SetupGame("ERYTHEA", Seed(9));
        Side* side = game::Begin(*game);
        game->surrender_offered = true;
        side->gold = 100000;
        for (int i = 0; i < 50; ++i) EXPECT_EQ(hero::Offer(*game, *side), nullptr);
    }
    {
        auto game = SetupGame("ERYTHEA", Seed(1));
        Side* side = game::Begin(*game);
        Army* army = game::SideArmies(*game, *side)[0];
        City* far = nullptr;
        for (auto& city : game->map->cities) {
            if (!far && movement::Distance(city.x, city.y, army->x, army->y) > 20) far = &city;
        }
        auto first_path = movement::FindPath(*game, {army}, army->x, army->y, far->x, far->y);
        ASSERT_TRUE(first_path && first_path->size() > 3);
        bool cached = false;
        for (const auto& entry : game->path_cache->slots) {
            if (entry && entry->dx == far->x && entry->dy == far->y) cached = true;
        }
        EXPECT_TRUE(cached);
        auto mid = (*first_path)[2];
        auto second_path = movement::FindPath(*game, {army}, mid.x, mid.y, far->x, far->y);
        ASSERT_EQ(second_path->size(), first_path->size() - 3);
        for (size_t i = 0; i < second_path->size(); ++i) {
            EXPECT_EQ((*second_path)[i].x, (*first_path)[i + 3].x);
            EXPECT_EQ((*second_path)[i].y, (*first_path)[i + 3].y);
        }

        City& origin_city = game->map->cities[0];
        auto flood = ai::core::FloodFrom(*game, 0, origin_city.x, origin_city.y, 5, nullptr, kNone);
        for (int y = 0; y < game->map->height; ++y) {
            for (int x = 0; x < game->map->width; ++x) {
                if (TilePos(x, y).ChebyshevDistance(origin_city.Position()) > 5) {
                    EXPECT_EQ(ai::core::FloodAt(flood, x, y), ai::core::kUnreached);
                }
            }
        }

        auto is_free = [&](int x, int y) {
            return scn::TerrainAt(*game->map, x, y) == movement::kPlain &&
                   !game::CityAt(*game, x, y) && game::ArmiesAt(*game, x, y).empty() &&
                   scn::RoadAt(*game->map, x, y) % 32 == 0;
        };
        TilePos free_tile;
        for (int y = 1; y < game->map->height - 1 && !free_tile.IsValid(); ++y) {
            for (int x = 1; x < game->map->width - 1 && !free_tile.IsValid(); ++x) {
                if (is_free(x, y) && is_free(x + 1, y)) free_tile = TilePos(x, y);
            }
        }
        Army* walker = AddTypedArmy(*game, free_tile.x, free_tile.y, side->index, 11);
        int step_cost = (*movement::FindPath(*game, {walker}, free_tile.x, free_tile.y,
                                             free_tile.x + 1, free_tile.y))[0]
                                .cost;
        walker->moves = step_cost + 1;
        EXPECT_TRUE(movement::MoveTo(*game, {walker}, free_tile.x + 1, free_tile.y).out_of_moves);
        walker->moves = step_cost + 2;
        EXPECT_FALSE(movement::MoveTo(*game, {walker}, free_tile.x, free_tile.y).out_of_moves);

        Army* bridge_army = AddTypedArmy(*game, 71, 11, side->index, 4);
        bridge_army->moves = 40;
        auto to_sea = movement::MoveTo(*game, {bridge_army}, 70, 14);
        EXPECT_TRUE(bridge_army->x == 70 && bridge_army->y == 14 && !to_sea.sea_before &&
                    bridge_army->at_sea);
        bridge_army->moves = 40;
        EXPECT_TRUE(movement::MoveTo(*game, {bridge_army}, 71, 11).sea_before);
    }

    for (bool flying : {false, true}) {
        auto game = SetupGame("ERYTHEA", SeedWith(2, {{"quests", 1}, {"hiddenMap", 1}}));
        Side* side = game::Begin(*game);
        Site* item_site = nullptr;
        for (auto& map_site : game->map->sites) {
            if (!item_site && map_site.content == site::kItem &&
                !site::ItemReserved(game->map->items[map_site.item]))
                item_site = &map_site;
        }
        Army* hero = AddTypedArmy(*game, item_site->x, item_site->y, side->index, armytype::kHero);
        std::shared_ptr<Quest> assigned;
        for (int i = 0; i < 200 && (!assigned || assigned->Type() != quest::kRetrieveItem); ++i) {
            side->quest.reset();
            assigned = quest::Assign(*game, *side, hero, false, flying);
        }
        ASSERT_TRUE(assigned && assigned->Type() == quest::kRetrieveItem);
        Site* target_site = nullptr;
        for (auto& map_site : game->map->sites) {
            if (map_site.content == site::kItem &&
                map_site.item == assigned->TargetItemId().Value())
                target_site = &map_site;
        }
        ASSERT_NE(target_site, nullptr);
        if (flying) {
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy)
                    if (std::max(std::abs(dx), std::abs(dy)) == 2)
                        EXPECT_TRUE(game::Seen(*game, side->index, target_site->x + dx,
                                               target_site->y + dy));
        } else {
            EXPECT_TRUE(game::Seen(*game, side->index, target_site->x + 1, target_site->y + 1));
        }
    }
}

}  // namespace
}  // namespace w2
