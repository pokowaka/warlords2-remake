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
    while (side != nullptr && game.Turn() <= 8) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }

    Army* hero = nullptr;
    for (Army* army : game.Armies()) {
        if (army->TypeIndex() == armytype::kHero) {
            hero = army;
        }
    }
    ASSERT_NE(hero, nullptr) << "the game has a hero to save";

    Side* owner = game.GetMap().SideById(hero->Owner());
    ASSERT_NE(owner, nullptr);
    owner->SetQuest(MakeCityQuest(quest::kOccupy, hero, game.GetMap().CityAt(4)));
    hero->SetItems({&game.GetMap().Items()[0]});
    game.GetMap().Items()[0].status = 3;
    game.Armies()[0]->SetGroup(4);
    game.Armies()[1]->SetGroup(4);
    game.Armies()[0]->SetFortified(true);

    std::string text = save::Encode(game);
    EXPECT_GT(text.size(), 1000U);

    auto loaded_ptr = *save::Decode(text, kData);
    ASSERT_NE(loaded_ptr, nullptr);
    Game& loaded = *loaded_ptr;
    EXPECT_EQ(loaded.Turn(), game.Turn());
    EXPECT_EQ(loaded.CurrentSideIndex(), game.CurrentSideIndex());
    ASSERT_EQ(loaded.Armies().size(), game.Armies().size());
    EXPECT_EQ(loaded.GetRng().State(), game.GetRng().State());

    for (size_t i = 0; i < game.Armies().size(); ++i) {
        const Army* saved = game.Armies()[i];
        const Army* restored = loaded.Armies()[i];
        EXPECT_EQ(restored->X(), saved->X()) << "army " << i;
        EXPECT_EQ(restored->Type(), saved->Type()) << "army " << i;
        EXPECT_EQ(restored->Strength(), saved->Strength()) << "army " << i;
        EXPECT_EQ(restored->Moves(), saved->Moves()) << "army " << i;
        EXPECT_EQ(restored->Owner(), saved->Owner()) << "army " << i;
        EXPECT_EQ(restored->Group(), saved->Group()) << "army " << i;
        EXPECT_EQ(restored->IsFortified(), saved->IsFortified()) << "army " << i;
    }

    ASSERT_EQ(loaded.GetMap().Cities().size(), game.GetMap().Cities().size());
    for (size_t i = 0; i < game.GetMap().Cities().size(); ++i) {
        const City& saved = game.GetMap().Cities()[i];
        const City& restored = loaded.GetMap().Cities()[i];
        EXPECT_EQ(restored.OwnerIndex(), saved.OwnerIndex()) << "city " << i;
        EXPECT_EQ(restored.ProducingSlot(), saved.ProducingSlot()) << "city " << i;
        EXPECT_EQ(restored.Slots().size(), saved.Slots().size()) << "city " << i;
    }

    ASSERT_EQ(loaded.Sides().size(), game.Sides().size());
    for (size_t i = 0; i < game.Sides().size(); ++i) {
        const Side* saved = game.Sides()[i];
        EXPECT_EQ(loaded.Sides()[i]->Gold(), saved->Gold()) << saved->Name();
        EXPECT_EQ(loaded.Sides()[i]->IsAlive(), saved->IsAlive()) << saved->Name();
        EXPECT_EQ(loaded.Sides()[i]->DiploScore(), saved->DiploScore()) << saved->Name();
    }

    ASSERT_EQ(loaded.GetMap().Sites().size(), game.GetMap().Sites().size());
    for (size_t i = 0; i < game.GetMap().Sites().size(); ++i) {
        const Site& saved = game.GetMap().Sites()[i];
        const Site& restored = loaded.GetMap().Sites()[i];
        EXPECT_EQ(restored.content, saved.content) << "site " << i;
        EXPECT_EQ(restored.searched, saved.searched) << "site " << i;
        EXPECT_EQ(restored.rich, saved.rich) << "site " << i;
    }

    Side* restored_owner = loaded.GetMap().SideById(owner->Id());
    ASSERT_NE(restored_owner, nullptr);
    ASSERT_NE(restored_owner->ActiveQuest(), nullptr);
    EXPECT_EQ(restored_owner->ActiveQuest()->Type(), quest::kOccupy);
    EXPECT_EQ(restored_owner->ActiveQuest()->TargetCityId(), game.GetMap().Cities()[4].Id());
    const Army* restored_hero = restored_owner->ActiveQuest()->ResolveHero(loaded);
    ASSERT_NE(restored_hero, nullptr);
    EXPECT_EQ(restored_hero->TypeIndex(), armytype::kHero);

    bool any_carried = false;
    for (const Army* army : loaded.Armies()) {
        if (!army->Items().empty()) {
            any_carried = true;
        }
    }
    EXPECT_TRUE(any_carried);
    EXPECT_EQ(diplomacy::State(loaded, 0, 1), diplomacy::State(game, 0, 1));

    Side* active_side = loaded.Sides()[loaded.CurrentSideIndex()];
    int turn_before = loaded.Turn();
    for (size_t i = 0; i < loaded.Sides().size() * 2 && active_side != nullptr; ++i) {
        PlayAi(loaded, *active_side);
        active_side = EndTurn(loaded);
    }
    EXPECT_GT(loaded.Turn(), turn_before);

    ScopedTempPath temp_save("w2-test-save.json");
    ASSERT_TRUE(WriteFile(temp_save.Path(), text).ok());
    auto disk_text = ReadFile(temp_save.Path());
    ASSERT_TRUE(disk_text.ok());
    auto from_file = *save::Decode(*disk_text, kData);
    ASSERT_NE(from_file, nullptr);
    EXPECT_EQ(from_file->Armies().size(), game.Armies().size());
}

}  // namespace
}  // namespace w2
