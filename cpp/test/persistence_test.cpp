/**
 * @file
 * @brief GoogleTest suite for full-state game save encoding, decoding, and file round-tripping.
 */
#include <string>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"
#include "warlords/save.hpp"

namespace w2 {
namespace {

using test::EndTurn;
using test::kData;
using test::MakeCityQuest;
using test::PlayAi;
using test::ScopedTempPath;
using test::SeedWith;
using test::SetupGame;

TEST(PersistenceTest, SaveAndLoadPreservesFullGameStateAndPlaysOn) {
    auto game_ptr = SetupGame("ERYTHEA", SeedWith(91, {{"quests", 1}, {"diplomacy", 1}}));
    Game& game = *game_ptr;
    Side* side = game::Begin(game);
    while (side != nullptr && game.turn <= 8) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }

    Army* hero = nullptr;
    for (Army* army : game.armies) {
        if (army->type == armytype::kHero) {
            hero = army;
        }
    }
    ASSERT_NE(hero, nullptr) << "the game has a hero to save";

    Side* owner = game.map->Side(hero->owner);
    ASSERT_NE(owner, nullptr);
    owner->quest = MakeCityQuest(quest::kOccupy, hero, &game.map->cities[4]);
    hero->items = {&game.map->items[0]};
    game.map->items[0].status = 3;
    game.armies[0]->group = 4;
    game.armies[1]->group = 4;
    game.armies[0]->fortified = true;

    std::string text = save::Encode(game);
    EXPECT_GT(text.size(), 1000U);

    auto loaded_ptr = *save::Decode(text, kData);
    ASSERT_NE(loaded_ptr, nullptr);
    Game& loaded = *loaded_ptr;
    EXPECT_EQ(loaded.turn, game.turn);
    EXPECT_EQ(loaded.current, game.current);
    ASSERT_EQ(loaded.armies.size(), game.armies.size());
    EXPECT_EQ(loaded.rng.state, game.rng.state);

    for (size_t i = 0; i < game.armies.size(); ++i) {
        const Army* saved = game.armies[i];
        const Army* restored = loaded.armies[i];
        EXPECT_EQ(restored->x, saved->x) << "army " << i;
        EXPECT_EQ(restored->type, saved->type) << "army " << i;
        EXPECT_EQ(restored->strength, saved->strength) << "army " << i;
        EXPECT_EQ(restored->moves, saved->moves) << "army " << i;
        EXPECT_EQ(restored->owner, saved->owner) << "army " << i;
        EXPECT_EQ(restored->group, saved->group) << "army " << i;
        EXPECT_EQ(restored->fortified, saved->fortified) << "army " << i;
    }

    ASSERT_EQ(loaded.map->cities.size(), game.map->cities.size());
    for (size_t i = 0; i < game.map->cities.size(); ++i) {
        const City& saved = game.map->cities[i];
        const City& restored = loaded.map->cities[i];
        EXPECT_EQ(restored.owner_index, saved.owner_index) << "city " << i;
        EXPECT_EQ(restored.producing, saved.producing) << "city " << i;
        EXPECT_EQ(restored.slots.size(), saved.slots.size()) << "city " << i;
    }

    ASSERT_EQ(loaded.sides.size(), game.sides.size());
    for (size_t i = 0; i < game.sides.size(); ++i) {
        const Side* saved = game.sides[i];
        EXPECT_EQ(loaded.sides[i]->gold, saved->gold) << saved->name;
        EXPECT_EQ(loaded.sides[i]->alive, saved->alive) << saved->name;
        EXPECT_EQ(loaded.sides[i]->diplo_score, saved->diplo_score) << saved->name;
    }

    ASSERT_EQ(loaded.map->sites.size(), game.map->sites.size());
    for (size_t i = 0; i < game.map->sites.size(); ++i) {
        const Site& saved = game.map->sites[i];
        const Site& restored = loaded.map->sites[i];
        EXPECT_EQ(restored.content, saved.content) << "site " << i;
        EXPECT_EQ(restored.searched, saved.searched) << "site " << i;
        EXPECT_EQ(restored.rich, saved.rich) << "site " << i;
    }

    Side* restored_owner = loaded.map->Side(owner->index);
    ASSERT_NE(restored_owner, nullptr);
    ASSERT_NE(restored_owner->quest, nullptr);
    EXPECT_EQ(restored_owner->quest->type, quest::kOccupy);
    EXPECT_EQ(restored_owner->quest->city != nullptr ? restored_owner->quest->city->index : kNone,
              game.map->cities[4].index);
    ASSERT_NE(restored_owner->quest->hero, nullptr);
    EXPECT_EQ(restored_owner->quest->hero->type, armytype::kHero);

    bool any_carried = false;
    for (const Army* army : loaded.armies) {
        if (!army->items.empty()) {
            any_carried = true;
        }
    }
    EXPECT_TRUE(any_carried);
    EXPECT_EQ(diplomacy::State(loaded, 0, 1), diplomacy::State(game, 0, 1));

    Side* active_side = loaded.sides[loaded.current];
    int turn_before = loaded.turn;
    for (size_t i = 0; i < loaded.sides.size() * 2 && active_side != nullptr; ++i) {
        PlayAi(loaded, *active_side);
        active_side = EndTurn(loaded);
    }
    EXPECT_GT(loaded.turn, turn_before);

    ScopedTempPath temp_save("w2-test-save.json");
    ASSERT_TRUE(WriteFile(temp_save.Path(), text).ok());
    auto disk_text = ReadFile(temp_save.Path());
    ASSERT_TRUE(disk_text.ok());
    auto from_file = *save::Decode(*disk_text, kData);
    ASSERT_NE(from_file, nullptr);
    EXPECT_EQ(from_file->armies.size(), game.armies.size());
}

}  // namespace
}  // namespace w2
