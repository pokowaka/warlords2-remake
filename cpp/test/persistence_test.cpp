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
        if (army->TypeIndex() == armytype::kHero) {
            hero = army;
        }
    }
    ASSERT_NE(hero, nullptr) << "the game has a hero to save";

    Side* owner = game.map->Side(hero->Owner());
    ASSERT_NE(owner, nullptr);
    owner->SetQuest(MakeCityQuest(quest::kOccupy, hero, &game.map->cities[4]));
    hero->SetItems({&game.map->items[0]});
    game.map->items[0].status = 3;
    game.armies[0]->SetGroup(4);
    game.armies[1]->SetGroup(4);
    game.armies[0]->SetFortified(true);

    std::string text = save::Encode(game);
    EXPECT_GT(text.size(), 1000U);

    auto loaded_ptr = *save::Decode(text, kData);
    ASSERT_NE(loaded_ptr, nullptr);
    Game& loaded = *loaded_ptr;
    EXPECT_EQ(loaded.turn, game.turn);
    EXPECT_EQ(loaded.current, game.current);
    ASSERT_EQ(loaded.armies.size(), game.armies.size());
    EXPECT_EQ(loaded.rng.State(), game.rng.State());

    for (size_t i = 0; i < game.armies.size(); ++i) {
        const Army* saved = game.armies[i];
        const Army* restored = loaded.armies[i];
        EXPECT_EQ(restored->X(), saved->X()) << "army " << i;
        EXPECT_EQ(restored->Type(), saved->Type()) << "army " << i;
        EXPECT_EQ(restored->Strength(), saved->Strength()) << "army " << i;
        EXPECT_EQ(restored->Moves(), saved->Moves()) << "army " << i;
        EXPECT_EQ(restored->Owner(), saved->Owner()) << "army " << i;
        EXPECT_EQ(restored->Group(), saved->Group()) << "army " << i;
        EXPECT_EQ(restored->IsFortified(), saved->IsFortified()) << "army " << i;
    }

    ASSERT_EQ(loaded.map->cities.size(), game.map->cities.size());
    for (size_t i = 0; i < game.map->cities.size(); ++i) {
        const City& saved = game.map->cities[i];
        const City& restored = loaded.map->cities[i];
        EXPECT_EQ(restored.OwnerIndex(), saved.OwnerIndex()) << "city " << i;
        EXPECT_EQ(restored.ProducingSlot(), saved.ProducingSlot()) << "city " << i;
        EXPECT_EQ(restored.Slots().size(), saved.Slots().size()) << "city " << i;
    }

    ASSERT_EQ(loaded.sides.size(), game.sides.size());
    for (size_t i = 0; i < game.sides.size(); ++i) {
        const Side* saved = game.sides[i];
        EXPECT_EQ(loaded.sides[i]->Gold(), saved->Gold()) << saved->Name();
        EXPECT_EQ(loaded.sides[i]->IsAlive(), saved->IsAlive()) << saved->Name();
        EXPECT_EQ(loaded.sides[i]->DiploScore(), saved->DiploScore()) << saved->Name();
    }

    ASSERT_EQ(loaded.map->sites.size(), game.map->sites.size());
    for (size_t i = 0; i < game.map->sites.size(); ++i) {
        const Site& saved = game.map->sites[i];
        const Site& restored = loaded.map->sites[i];
        EXPECT_EQ(restored.content, saved.content) << "site " << i;
        EXPECT_EQ(restored.searched, saved.searched) << "site " << i;
        EXPECT_EQ(restored.rich, saved.rich) << "site " << i;
    }

    Side* restored_owner = loaded.map->Side(owner->Id());
    ASSERT_NE(restored_owner, nullptr);
    ASSERT_NE(restored_owner->ActiveQuest(), nullptr);
    EXPECT_EQ(restored_owner->ActiveQuest()->Type(), quest::kOccupy);
    EXPECT_EQ(restored_owner->ActiveQuest()->TargetCityId(), game.map->cities[4].Id());
    const Army* restored_hero = restored_owner->ActiveQuest()->ResolveHero(loaded);
    ASSERT_NE(restored_hero, nullptr);
    EXPECT_EQ(restored_hero->TypeIndex(), armytype::kHero);

    bool any_carried = false;
    for (const Army* army : loaded.armies) {
        if (!army->Items().empty()) {
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
