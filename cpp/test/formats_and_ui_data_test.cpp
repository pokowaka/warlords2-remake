/**
 * @file
 * @brief GoogleTest suite for RNG dice, ARMYTYPE.DAT, selection slots, UI layout, castles, and
 * sound cues.
 */
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/pck.hpp"
#include "warlords/rng.hpp"
#include "warlords/scn.hpp"
#include "warlords/slots.hpp"
#include "warlords/uidata.hpp"

namespace w2 {
namespace {

using test::kData;
using test::Seed;
using test::SetupGame;

TEST(DiceTest, BoundsReachabilityAndSeedDeterminism) {
    Rng rng(1234);
    int lowest = 1 << 30;
    int highest = -(1 << 30);
    for (int i = 0; i < 20000; ++i) {
        int roll = rng.Dice(1, 100, 0);
        lowest = std::min(lowest, roll);
        highest = std::max(highest, roll);
    }
    EXPECT_EQ(lowest, 1) << "1d100 lower bound";
    EXPECT_EQ(highest, 100) << "1d100 upper bound";

    lowest = 1 << 30;
    highest = -(1 << 30);
    for (int i = 0; i < 20000; ++i) {
        int roll = rng.Dice(3, 500, 500);
        lowest = std::min(lowest, roll);
        highest = std::max(highest, roll);
    }
    EXPECT_GE(lowest, 503) << "3d500+500 lower bound";
    EXPECT_LE(highest, 2000) << "3d500+500 upper bound";

    std::set<int> seen;
    for (int i = 0; i < 2000; ++i) {
        seen.insert(rng.Dice(1, 5, -1));
    }
    for (int i = 0; i <= 4; ++i) {
        EXPECT_EQ(seen.count(i), 1U) << "dice(1,5,-1) reaches " << i;
    }
    EXPECT_EQ(seen.count(5), 0U) << "dice(1,5,-1) stays below 5";

    Rng first_rng(7);
    Rng second_rng(7);
    bool same = true;
    for (int i = 0; i < 100; ++i) {
        if (first_rng.Dice(2, 6, 0) != second_rng.Dice(2, 6, 0)) {
            same = false;
        }
    }
    EXPECT_TRUE(same) << "a seed reproduces the sequence";
}

TEST(ArmyTypesTest, LoadsDocumentedTypesAndSignedModifiers) {
    Types types;
    ASSERT_TRUE(armytype::Load(kData / "TERRAIN0/ARMYTYPE.DAT", types).ok());
    EXPECT_EQ(types.all.size(), 29U);
    ASSERT_NE(types.ById(armytype::kHero), nullptr);
    EXPECT_EQ(types.ById(armytype::kHero)->name, "Hero");
    ASSERT_NE(types.ById(armytype::kScouts), nullptr);
    EXPECT_EQ(types.ById(armytype::kScouts)->name, "Scouts");

    const ArmyType* heavy_cavalry = nullptr;
    const ArmyType* elephants = nullptr;
    bool any_fly = false;
    for (const auto& army_type : types.all) {
        if (army_type.name == "Heavy Cav.") {
            heavy_cavalry = &army_type;
        }
        if (army_type.name == "Elephants") {
            elephants = &army_type;
        }
        if (army_type.flies) {
            any_fly = true;
        }
    }
    ASSERT_NE(heavy_cavalry, nullptr);
    EXPECT_EQ(heavy_cavalry->strength, 4);
    EXPECT_EQ(heavy_cavalry->time, 3);
    EXPECT_EQ(heavy_cavalry->cost, 8);
    EXPECT_EQ(heavy_cavalry->move, 16);
    EXPECT_TRUE(any_fly);
    ASSERT_NE(elephants, nullptr);
    EXPECT_EQ(elephants->bonus[50], -1) << "Elephants take 1 off the enemy stack (signed field)";
}

/**
 * @brief Minimal game and tile-stack fixture for `slots::` selection bar tests.
 */
class SlotsTest : public ::testing::Test {
  protected:
    /** @brief Populates `game_` with `n` ungrouped armies ranked highest-first in fight order. */
    void BuildStandInStack(int n) {
        game_ = std::make_unique<Game>();
        game_->map = std::make_unique<Map>();
        game_->map->fight_order.assign(9, std::vector<int>(29, 0));
        armies_.clear();
        for (int i = 1; i <= n; ++i) {
            Army army;
            army.SetType(ArmyTypeId(i));
            army.SetMoves(10 + i);
            army.SetGroup(0);
            game_->map->fight_order[0][i] = n - i;
            armies_.push_back(game_->Add(army));
        }
    }

    std::unique_ptr<Game> game_;  ///< Stand-in game state owning the armies and fight order.
    std::vector<Army*> armies_;   ///< Pointers to armies placed into the selection bar.
};

TEST_F(SlotsTest, InitialClickSelectsHighestRankedSingleArmy) {
    BuildStandInStack(3);
    auto bar = slots::Build(*game_, armies_, 0);
    EXPECT_EQ(bar.n, 3);
    EXPECT_EQ(bar.army[0], armies_[0]);
    EXPECT_TRUE(bar.in_group[0] && !bar.in_group[1] && !bar.in_group[2]);
    EXPECT_EQ(bar.group[0], 0);
    EXPECT_EQ(bar.group[2], 2);
    EXPECT_EQ(bar.mark[0], slots::kTick);
    EXPECT_EQ(bar.mark[1], slots::kCross);
    EXPECT_EQ(slots::Moves(bar), 11);
    EXPECT_FALSE(slots::Grouped(bar));
    EXPECT_EQ(slots::Selected(bar).size(), 1U);
}

TEST_F(SlotsTest, ToggleAddsAndDropsMembersWhileKeepingLastArmy) {
    BuildStandInStack(3);
    auto bar = slots::Build(*game_, armies_, 0);
    slots::Toggle(bar, *game_, 1);
    EXPECT_TRUE(bar.in_group[1] && bar.group[1] == bar.group[0]);
    EXPECT_EQ(bar.mark[1], slots::kNomark);
    EXPECT_EQ(slots::Moves(bar), 11);

    slots::Toggle(bar, *game_, 1);
    EXPECT_FALSE(bar.in_group[1]);
    EXPECT_NE(bar.group[1], bar.group[0]);

    slots::Toggle(bar, *game_, 0);
    EXPECT_TRUE(bar.in_group[0]) << "the last army of a group cannot be dropped";
}

TEST_F(SlotsTest, GroupAllSingleAndPickGroupSwitchSelection) {
    BuildStandInStack(3);
    auto bar = slots::Build(*game_, armies_, 0);
    slots::All(bar, *game_);
    EXPECT_EQ(slots::Selected(bar).size(), 3U);
    EXPECT_TRUE(slots::Grouped(bar));
    EXPECT_EQ(bar.mark[1], slots::kNomark);

    slots::Single(bar, *game_);
    EXPECT_EQ(slots::Selected(bar).size(), 1U);
    EXPECT_FALSE(slots::Grouped(bar));

    slots::PickGroup(bar, *game_, 2);
    EXPECT_TRUE(bar.in_group[2] && !bar.in_group[0]);
    EXPECT_EQ(bar.mark[2], slots::kTick);

    BuildStandInStack(3);
    auto full_bar = slots::Build(*game_, armies_, 0);
    slots::All(full_bar, *game_);
    full_bar.in_group[2] = false;
    slots::PickGroup(full_bar, *game_, 0);
    EXPECT_TRUE(full_bar.in_group[2]) << "picking a group takes all of it";
}

TEST_F(SlotsTest, CommitPersistsGroupIdsAndKeepRetainsSurvivors) {
    BuildStandInStack(3);
    auto bar = slots::Build(*game_, armies_, 0);
    slots::All(bar, *game_);
    slots::Commit(bar, *game_);
    int group_id = armies_[0]->Group();
    EXPECT_NE(group_id, slots::kUngrouped);
    EXPECT_EQ(armies_[1]->Group(), group_id);
    EXPECT_EQ(armies_[2]->Group(), group_id);

    auto again = slots::Build(*game_, armies_, 0);
    EXPECT_EQ(slots::Selected(again).size(), 3U);
    EXPECT_TRUE(slots::Grouped(again));
    EXPECT_EQ(again.group[0], again.group[2]);
    slots::Commit(again, *game_);
    EXPECT_EQ(armies_[0]->Group(), group_id);

    slots::Toggle(again, *game_, 2);
    slots::Commit(again, *game_);
    auto third = slots::Build(*game_, armies_, 0);
    EXPECT_EQ(slots::Selected(third).size(), 2U);
    EXPECT_EQ(armies_[2]->Group(), slots::kUngrouped);
    EXPECT_EQ(armies_[0]->Group(), group_id);

    slots::Single(third, *game_);
    slots::Commit(third, *game_);
    EXPECT_EQ(armies_[0]->Group(), slots::kUngrouped);
    EXPECT_EQ(slots::Selected(slots::Build(*game_, armies_, 0)).size(), 1U);

    BuildStandInStack(3);
    auto combat_bar = slots::Build(*game_, armies_, 0);
    slots::All(combat_bar, *game_);
    auto left = slots::Keep(combat_bar, *game_, {armies_[0], armies_[2]});
    ASSERT_TRUE(left.has_value());
    EXPECT_EQ(left->n, 2);
    EXPECT_TRUE(left->in_group[0] && left->in_group[1]);
    EXPECT_FALSE(slots::Keep(combat_bar, *game_, {}).has_value());
}

TEST(ScreenLayoutTest, UiDataDialogsShortcutsAndSpriteSheets) {
    auto ui_data = *uidata::Load(kData);
    EXPECT_EQ(ui_data.bitmaps[0], "startup.pck");
    EXPECT_EQ(ui_data.bitmaps[4], "button.pck");
    EXPECT_EQ(ui_data.bitmaps[78], "movebar7.pck");
    EXPECT_EQ(ui_data.joins.size(), 36U);
    EXPECT_EQ(ui_data.joins[0].button, 0);
    EXPECT_EQ(ui_data.joins[0].area, 2);
    EXPECT_EQ(ui_data.joins[9].button, 11);
    EXPECT_EQ(ui_data.joins[12].button, 9);

    auto dialog = uidata::FindDialog(ui_data, uidata::kMainScreen);
    EXPECT_EQ(dialog.controls.size(), 43U);
    EXPECT_EQ(dialog.regions.size(), 6U);
    const std::map<int, std::array<int, 4>> expected_regions = {{1, {400, 30, 224, 312}},
                                                                {2, {16, 30, 360, 360}},
                                                                {3, {0, 0, 640, 18}},
                                                                {9, {16, 408, 360, 56}}};
    for (const auto& region : dialog.regions) {
        auto found = expected_regions.find(region.id);
        if (found != expected_regions.end()) {
            EXPECT_EQ(region.x, found->second[0]) << "region " << region.id;
            EXPECT_EQ(region.y, found->second[1]) << "region " << region.id;
            EXPECT_EQ(region.w, found->second[2]) << "region " << region.id;
            EXPECT_EQ(region.h, found->second[3]) << "region " << region.id;
        }
    }
    EXPECT_EQ(ui_data.shortcut_names[507], "Move All");
    EXPECT_EQ(ui_data.shortcut_names[535], "End Turn");
    constexpr std::array<const char*, 4> kExpectedShortcuts = {"Search", "Move All", "Heroes",
                                                               "End Turn"};
    for (size_t i = 0; i < kExpectedShortcuts.size(); ++i) {
        EXPECT_EQ(ui_data.shortcut_names[ui_data.shortcuts[i]], kExpectedShortcuts[i]);
    }

    auto menu_img = *pck::Decode(kData / "PICS/MENUBUTT.PCK");
    ASSERT_NE(menu_img, nullptr);
    EXPECT_EQ(menu_img->w, 320);
    EXPECT_EQ(menu_img->h, 197);
    std::set<std::pair<int, int>> cells;
    int cell_count = 0;
    for (const auto& [item_id, item] : ui_data.shortcut_items) {
        EXPECT_EQ(item.w, 32) << "item " << item_id;
        EXPECT_EQ(item.h, 29) << "item " << item_id;
        for (int state = 0; state <= 2; ++state) {
            auto src = item.src[state];
            EXPECT_EQ(src.x % 32, 0) << "item " << item_id << " state " << state;
            EXPECT_EQ(src.y % 28, 0) << "item " << item_id << " state " << state;
            EXPECT_LE(src.x + 32, menu_img->w);
            EXPECT_LE(src.y + 29, menu_img->h);
            EXPECT_TRUE(cells.insert({src.x, src.y}).second)
                    << "cell " << src.x << "," << src.y << " reused";
            ++cell_count;
        }
    }
    EXPECT_EQ(cell_count, 63);
    const auto& end_turn = ui_data.shortcut_items[535];
    EXPECT_EQ(end_turn.src[uidata::kNormal].x, 0);
    EXPECT_EQ(end_turn.src[uidata::kNormal].y, 0);
    EXPECT_EQ(end_turn.src[uidata::kActive].x, 32);

    auto abits_img = *pck::Decode(kData / "PICS/ABITS.PCK");
    ASSERT_NE(abits_img, nullptr);
    EXPECT_EQ(abits_img->w, 480);
    EXPECT_EQ(abits_img->h, 40);
    constexpr int kBgColor = 3;
    std::set<int> seen_colors;
    for (int cell = 0; cell <= 8; ++cell) {
        std::map<int, int> count;
        for (int y = 0; y <= 29; ++y) {
            for (int x = cell * 32; x <= cell * 32 + 31; ++x) {
                int pixel = abits_img->px[y * abits_img->w + x];
                if (pixel != kBgColor) {
                    ++count[pixel];
                }
            }
        }
        int best = kNone;
        int best_count = 0;
        for (const auto& [color, color_count] : count) {
            if (color_count > best_count) {
                best = color;
                best_count = color_count;
            }
        }
        EXPECT_GT(best_count, 40) << "ring " << cell;
        if (cell > 0) {
            EXPECT_TRUE(seen_colors.insert(best).second) << "ring " << cell;
        }
    }
    int ring_row = 0;
    int strip_row = 0;
    for (int x = 0; x <= 287; ++x) {
        if (abits_img->px[15 * abits_img->w + x] != kBgColor) {
            ++ring_row;
        }
        if (abits_img->px[38 * abits_img->w + x] != kBgColor) {
            ++strip_row;
        }
    }
    EXPECT_GT(strip_row, ring_row * 2);

    /** @brief Status-strip icon rectangle in ABITS.PCK. */
    struct StatusIcon {
        int sheet_x;       ///< Left coordinate in the sprite sheet.
        int sheet_y;       ///< Top coordinate in the sprite sheet.
        const char* name;  ///< Diagnostic label.
    };
    for (StatusIcon icon : {StatusIcon{344, 0, "cities"}, StatusIcon{344, 20, "treasury"},
                            StatusIcon{384, 0, "income"}, StatusIcon{384, 20, "upkeep"}}) {
        EXPECT_GE(icon.sheet_x, 288) << icon.name;
        EXPECT_LE(icon.sheet_x + 40, abits_img->w) << icon.name;
        EXPECT_LE(icon.sheet_y + 20, abits_img->h) << icon.name;
        int ink = 0;
        int edge = 0;
        for (int y = icon.sheet_y; y <= icon.sheet_y + 19; ++y) {
            if (abits_img->px[y * abits_img->w + icon.sheet_x - 1] != kBgColor) {
                ++edge;
            }
            for (int x = icon.sheet_x; x <= icon.sheet_x + 39; ++x) {
                if (abits_img->px[y * abits_img->w + x] != kBgColor) {
                    ++ink;
                }
            }
        }
        EXPECT_GT(ink, 40) << icon.name;
        EXPECT_EQ(edge, 0) << icon.name;
    }

    std::map<std::string, std::pair<int, int>> sizes;
    std::set<std::string> missing;
    int checked = 0;
    for (const auto& [group_id, group] : ui_data.buttons) {
        (void)group_id;
        for (const auto& control : group.controls) {
            if (control.bitmap == 0 || control.w <= 0) {
                continue;
            }
            const std::string& name = ui_data.bitmaps[control.bitmap];
            if (!sizes.count(name) && !missing.count(name)) {
                bool found = false;
                for (const char* dir : {"PICS", "TERRAIN0", ""}) {
                    std::filesystem::path path =
                            dir[0] != '\0' ? (kData / dir / Upper(name)) : (kData / Upper(name));
                    if (FileExists(path)) {
                        auto image = *pck::Decode(path);
                        sizes[name] = {image->w, image->h};
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    missing.insert(name);
                }
            }
            EXPECT_EQ(sizes.count(name), 1U) << "bitmap resolves to a file: " << name;
            if (sizes.count(name)) {
                auto [sheet_w, sheet_h] = sizes[name];
                for (int state = 0; state <= 2; ++state) {
                    EXPECT_LE(control.src[state].x + control.w, sheet_w)
                            << "control " << control.id << " in " << name;
                    EXPECT_LE(control.src[state].y + control.h, sheet_h)
                            << "control " << control.id << " in " << name;
                    ++checked;
                }
            }
        }
    }
    EXPECT_GE(checked, 400);
}

TEST(CityCastlesTest, CastleAndRuinTileBlocksMatchOwnerAndRazer) {
    auto game_ptr = SetupGame("TUTORIA", Seed(11));
    Map& map = *game_ptr->map;
    const std::map<int, int> expected_by_owner = {{-1, 96}, {0, 98},  {1, 100}, {2, 102}, {3, 104},
                                                  {4, 106}, {5, 108}, {6, 128}, {7, 130}};
    City& city = map.cities[0];
    for (auto [owner, base] : expected_by_owner) {
        city.SetRazed(false);
        city.SetOwner(owner >= 0 ? SideId(owner) : SideId::None());
        EXPECT_EQ(scn::CityTileBase(city), base) << "owner " << owner;
        scn::SetCityTiles(map, city);
        EXPECT_EQ(scn::TileAt(map, city.X(), city.Y()), base);
        EXPECT_EQ(scn::TileAt(map, city.X() + 1, city.Y()), base + 1);
        EXPECT_EQ(scn::TileAt(map, city.X(), city.Y() + 1), base + 16);
        EXPECT_EQ(scn::TileAt(map, city.X() + 1, city.Y() + 1), base + 17);
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dy = 0; dy <= 1; ++dy) {
                EXPECT_EQ(scn::TerrainAt(map, city.X() + dx, city.Y() + dy), 10);
            }
        }
    }
    for (int owner = 0; owner <= 7; ++owner) {
        city.SetRazed(true);
        city.SetRazedBy(SideId(owner));
        EXPECT_EQ(scn::CityTileBase(city), 0xa0 + 2 * owner);
        scn::SetCityTiles(map, city);
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dy = 0; dy <= 1; ++dy) {
                EXPECT_EQ(scn::TerrainAt(map, city.X() + dx, city.Y() + dy), 11);
            }
        }
    }

    auto game2 = SetupGame("TUTORIA", Seed(12));
    City& city2 = game2->map->cities[0];
    for (Side* side : game2->sides) {
        game::SetCityOwner(*game2, city2, side->Index());
        scn::SetCityTiles(*game2->map, city2);
    }
    auto mine_list = game::SideCities(*game2, *game2->sides[0]);
    ASSERT_FALSE(mine_list.empty());
    City* mine = mine_list[0];
    int previous_owner = mine->OwnerIndex();
    game::Raze(*game2, *game2->sides[0], *mine, {});
    EXPECT_EQ(mine->RazedBy(), SideId(previous_owner));
    EXPECT_EQ(scn::TileAt(*game2->map, mine->X(), mine->Y()), 0xa0 + 2 * previous_owner);
}

TEST(SoundCuesTest, MusicSelectionAndVoiceAdvisorThresholds) {
    auto files = *uidata::LoadStrings(kData / "DATA/FILE.DAT");
    auto first = [](int) { return 1; };
    auto [title_song, title_loop] = cues::Song(files, cues::kTitle, {}, first);
    EXPECT_EQ(title_song, "STARTUP.XMI");
    EXPECT_TRUE(title_loop);

    Side human;
    human.SetComputerControlled(false);
    human.SetAlive(true);
    auto [computer_song, computer_loop] = cues::Song(files, cues::kComputer, {&human}, first);
    EXPECT_EQ(computer_song, "INT2.XMI");
    EXPECT_FALSE(computer_loop);
    EXPECT_EQ(cues::Song(files, cues::kBegin, {}, first).first, "INT22.XMI");

    Game game;
    game.map = std::make_unique<Map>();
    game.turn = 1;
    Side side;
    side.SetId(SideId(0));
    side.SetGold(500);
    game.map->cities.resize(6);
    for (auto& city : game.map->cities) {
        city.SetOwner(SideId(0));
    }
    EXPECT_EQ(cues::AdvisorCue(game, side, first), kNone);
    EXPECT_EQ(side.AdvisorState().mark, 5);
    game.turn = 2;
    EXPECT_EQ(cues::AdvisorCue(game, side, first), kNone);
    for (int i = 0; i < 5; ++i) {
        game.map->cities[i].SetOwner(SideId(1));
    }
    EXPECT_EQ(cues::AdvisorCue(game, side, first), 40);
    EXPECT_EQ(side.AdvisorState().dir, 2);
    for (int i = 0; i < 5; ++i) {
        game.map->cities[i].SetOwner(SideId(0));
    }
    EXPECT_EQ(cues::AdvisorCue(game, side, first), 47);
    game.turn = 7;
    side.SetGold(50);
    EXPECT_EQ(cues::AdvisorCue(game, side, first), 54);
}

}  // namespace
}  // namespace w2
