/**
 * @file
 * @brief GoogleTest suite for the map evaluator, modern scenario generator, and classic random map
 * generator.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "test/test_helpers.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/mapeval.hpp"
#include "warlords/move.hpp"
#include "warlords/pck.hpp"
#include "warlords/preview.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rng.hpp"
#include "warlords/save.hpp"
#include "warlords/scenarios.hpp"
#include "warlords/scengen.hpp"
#include "warlords/scn.hpp"

namespace w2 {
namespace {

namespace movement = w2::move;
using test::EndTurn;
using test::kData;
using test::PlayAi;
using test::ScenarioParamName;
using test::ScenarioTest;
using test::ScopedTempPath;

/** @brief Parameterized fixture verifying shipped designer scenarios against `mapeval`. */
class ShippedMapEvalTest : public ScenarioTest {};

TEST_P(ShippedMapEvalTest, DesignerMapBreaksNoHardRuleAndSpreadsCities) {
    const std::string& scenario = GetParam();
    auto map = *scn::Load(kData / scenario, scenario);
    auto evaluation = mapeval::Evaluate(*map, scenario);
    std::string first_issue;
    for (const auto& issue : evaluation.issues) {
        if (mapeval::IsHard(issue.check)) {
            first_issue = issue.check + ": " + issue.detail;
            break;
        }
    }
    EXPECT_TRUE(evaluation.Passed()) << scenario << " " << first_issue;
    EXPECT_GT(evaluation.Metric("clarkEvansR"), 1.3) << scenario;
}

// NOLINTNEXTLINE(readability-identifier-length,cppcoreguidelines-avoid-non-const-global-variables)
INSTANTIATE_TEST_SUITE_P(DesignerMaps, ShippedMapEvalTest,
                         ::testing::Values("ERYTHEA", "HADESHA", "ISLADIA", "DRAGON", "SORCERY"),
                         ScenarioParamName);

TEST(MapEvalTest, BrokenMapsTriggerHardChecksFairCapitalsAndReferenceScores) {
    auto map = *scn::Load(kData / "ERYTHEA", "ERYTHEA");
    Map& broken = *map;
    auto tile_of = [&](int type) {
        for (int tile = 0; tile < 256; ++tile) {
            if (broken.TerrainTypeOf(tile) == type) {
                return tile;
            }
        }
        return -1;
    };

    int lone_x = -1;
    int lone_y = -1;
    for (int y = 3; y < kMapH - 3 && lone_x < 0; ++y) {
        for (int x = 3; x < kMapW - 3 && lone_x < 0; ++x) {
            bool clear = true;
            for (int dy = -2; dy <= 2 && clear; ++dy) {
                for (int dx = -2; dx <= 2 && clear; ++dx) {
                    clear = scn::TerrainAt(broken, x + dx, y + dy) == movement::kPlain &&
                            scn::RoadAt(broken, x + dx, y + dy) == 0;
                }
            }
            if (clear) {
                lone_x = x;
                lone_y = y;
            }
        }
    }
    ASSERT_GE(lone_x, 0);
    broken.SetRoadByte(lone_x, lone_y, 1);
    broken.Cities()[1].SetPosition(TilePos(broken.Cities()[0].X() + 1, broken.Cities()[0].Y()));
    broken.RebuildSpatialIndices();
    broken.Cities()[3].Rename(broken.Cities()[2].Name());
    broken.Signs()[0].lines = {broken.Cities()[5].Name(), "999 leagues north"};
    const Site& walled = broken.Sites()[0];
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int x = walled.x + dx;
            int y = walled.y + dy;
            if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= kMapW || y >= kMapH) {
                continue;
            }
            broken.SetTileWord(x, y, tile_of(movement::kMountains));
            broken.SetRoadByte(x, y, 0);
        }
    }
    auto broken_eval = mapeval::Evaluate(broken, "broken");
    EXPECT_FALSE(broken_eval.Passed());
    for (const char* check : {"road-to-nowhere", "dead-end", "city-gap", "city-terrain",
                              "city-name", "signpost", "unreachable"}) {
        EXPECT_GT(broken_eval.Count(check), 0) << check;
    }

    auto near_map = *scn::Load(kData / "ERYTHEA", "ERYTHEA");
    City* home = near_map->Sides()[0].CapitalCity(*near_map);
    City* next = nullptr;
    for (auto& candidate : near_map->Cities()) {
        if (&candidate != home &&
            (!next || std::hypot(candidate.X() - home->X(), candidate.Y() - home->Y()) <
                              std::hypot(next->X() - home->X(), next->Y() - home->Y()))) {
            next = &candidate;
        }
    }
    near_map->Sides()[1].SetCapitalCityId(next->Id());
    EXPECT_GT(mapeval::Evaluate(*near_map, "near").Count("capital-gap"), 0);

    Rng classic_rng(11);
    Rng fair_rng(11);
    randommap::Options fair_options;
    fair_options.data_dir = kData;
    fair_options.rng = &classic_rng;
    auto classic_files = *randommap::GenerateNow(fair_options);
    fair_options.rng = &fair_rng;
    fair_options.fair_capitals = true;
    auto fair_files = *randommap::GenerateNow(fair_options);
    EXPECT_NE(classic_files["SCN"], fair_files["SCN"]);

    auto fair = *scn::LoadFromFiles(kData / "RANDOM", "RANDOM", fair_files);
    std::set<City*> capitals;
    for (auto& fair_side : fair->Sides()) {
        capitals.insert(fair_side.CapitalCity(*fair));
    }
    EXPECT_EQ(capitals.size(), 8U);
    auto fair_eval = mapeval::Evaluate(*fair, "fair#11");
    EXPECT_EQ(fair_eval.Count("capital-gap"), 0);
    EXPECT_LE(fair_eval.Metric("remoteCityFrac"), 0.05);
    EXPECT_GE(fair_eval.Metric("capitalGapMinTurns"), 2.5);

    std::vector<mapeval::Evaluation> refs;
    for (const char* scenario : {"ERYTHEA", "HADESHA"}) {
        refs.push_back(mapeval::Evaluate(**scn::Load(kData / scenario, scenario), scenario));
    }
    auto ref = mapeval::MakeReference(refs);
    for (const auto& designed : refs) {
        EXPECT_DOUBLE_EQ(mapeval::ScoreAgainst(designed, ref).penalty, 0.0) << designed.name;
    }

    Rng rng(1);
    randommap::Options gen_options;
    gen_options.data_dir = kData;
    gen_options.rng = &rng;
    auto files = *randommap::GenerateNow(gen_options);
    auto made = *scn::LoadFromFiles(kData / "RANDOM", "RANDOM", files);
    auto made_eval = mapeval::Evaluate(*made, "classic#1");
    EXPECT_EQ(static_cast<int>(made_eval.Metric("cities")), 80);
    EXPECT_NE(mapeval::ToJson(made_eval).dump().find("\"metrics\""), std::string::npos);
}

TEST(ScengenTest, ShapesTitlesScenarioCatalogAndMenuPreviews) {
    for (auto shape :
         {scengen::Shape::kContinents, scengen::Shape::kPangaea, scengen::Shape::kIslands}) {
        for (int seed = 1; seed <= 3; ++seed) {
            Rng seeded_rng(seed);
            Rng again(seed);
            randommap::Options scengen_options;
            scengen_options.data_dir = kData;
            scengen_options.rng = &seeded_rng;
            scengen_options.scenario = true;
            scengen_options.world.shape = shape;
            auto scengen_files = *randommap::GenerateNow(scengen_options);
            scengen_options.rng = &again;
            EXPECT_EQ(scengen_files, *randommap::GenerateNow(scengen_options))
                    << scengen::ShapeName(shape);

            auto world = *scn::LoadFromFiles(kData / "RANDOM", "RANDOM", scengen_files);
            auto evaluation = mapeval::Evaluate(
                    *world, std::string(scengen::ShapeName(shape)) + "#" + std::to_string(seed));
            std::string first_issue;
            for (const auto& issue : evaluation.issues) {
                if (mapeval::IsHard(issue.check)) {
                    first_issue = ": " + issue.check + " " + issue.detail;
                    break;
                }
            }
            EXPECT_TRUE(evaluation.Passed()) << evaluation.name << first_issue;
            EXPECT_EQ(static_cast<int>(evaluation.Metric("cities")), 80);
            EXPECT_GE(evaluation.Metric("cityRoadCoverage"), 0.74);
            EXPECT_EQ(world->GameOptions().military_advisor, 1);
        }
    }

    auto fin_data = ReadBytes(kData / "CHANCE17.FIN");
    ASSERT_TRUE(fin_data.ok());
    int glyph_count = (*fin_data)[0];
    int first_char = (*fin_data)[1];
    auto measure_width = [&](const std::string& text) {
        int pixels = 0;
        for (unsigned char glyph : text) {
            if (glyph >= first_char && glyph < first_char + glyph_count) {
                pixels += (*fin_data)[11 + glyph_count + (glyph - first_char)];
            }
        }
        return pixels;
    };
    int widest = 0;
    bool all_fit = true;
    for (auto shape :
         {scengen::Shape::kContinents, scengen::Shape::kPangaea, scengen::Shape::kIslands}) {
        for (long seed = 1; seed <= 600; ++seed) {
            auto title = scengen::MakeTitle(shape, seed);
            widest = std::max(widest, measure_width(title.name));
            if (title.name.empty() || title.name.size() > 20 || title.text.empty() ||
                title.text.size() > 30) {
                all_fit = false;
            }
        }
    }
    EXPECT_LE(widest, measure_width("The Frozen Steppe"));
    EXPECT_TRUE(all_fit);

    ScopedTempPath scenarios_dir("w2test-scenarios");
    const std::filesystem::path& root = scenarios_dir.Path();
    std::filesystem::create_directories(root / "DATA");
    std::filesystem::create_directories(root / "GEN7");
    std::filesystem::create_directories(root / "NOINFO");
    std::filesystem::copy_file(kData / "DATA/SCENARIO.DAT", root / "DATA" / "SCENARIO.DAT");
    scenarios::Entry made_entry{"Continents 7", "GEN7", "A new world, seed 7", 80, 40, 8, true};
    ASSERT_TRUE(WriteFile(root / "GEN7" / "GEN7.INF", scenarios::Info(made_entry)).ok());
    ASSERT_TRUE(WriteFile(root / "GEN7" / "GEN7.SCN", "").ok());
    ASSERT_TRUE(WriteFile(root / "NOINFO" / "NOINFO.SCN", "").ok());
    EXPECT_TRUE(scenarios::Remember(root, "NOINFO") && scenarios::Remember(root, "GEN7") &&
                scenarios::Remember(root, "GEN7"));
    auto scengen_lst = ReadFile(root / "DATA" / "SCENGEN.LST");
    ASSERT_TRUE(scengen_lst.ok());
    EXPECT_EQ(*scengen_lst, "GEN7\nNOINFO\n");

    auto list = scenarios::List(root);
    ASSERT_EQ(list.size(), 7U);
    EXPECT_EQ(list[3].dir, "Erythea");
    EXPECT_EQ(list.back().name, "Continents 7");
    EXPECT_EQ(list.back().cities, 80);
    EXPECT_TRUE(list.back().generated && !list[0].generated);
    auto parsed = scenarios::ParseInfo(scenarios::Info(made_entry), "GEN7");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->text, made_entry.text);
    EXPECT_EQ(parsed->players, 8);

    auto menu_map = *scn::Load(kData / "ERYTHEA", "ERYTHEA");
    auto pic = *preview::MenuPicture(*menu_map, kData);
    EXPECT_EQ(pic.w, 264);
    EXPECT_EQ(pic.h, 225);
    ScopedTempPath pck_file("w2test-SCENARIO.PCK");
    ASSERT_TRUE(WriteBytes(pck_file.Path(), *pck::Encode(pic)).ok());
    auto read_pic = *pck::Decode(pck_file.Path());
    ASSERT_NE(read_pic, nullptr);
    EXPECT_TRUE(read_pic->w == pic.w && read_pic->h == pic.h && read_pic->px == pic.px);
    auto png = *preview::Png(*menu_map, kData);
    ASSERT_GE(png.size(), 8U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(png.data()), 8), "\x89PNG\r\n\x1a\n");
}

TEST(RandomMapTest, ClassicGeneratorDeterminismSlidersAndSaveRoundTrip) {
    auto make_world = [](int seed, std::array<int, 4> sliders, bool allies) {
        Rng rng(seed);
        randommap::Options options;
        options.data_dir = kData;
        options.rng = &rng;
        options.sliders = sliders;
        options.allies = allies;
        return *randommap::GenerateNow(options);
    };
    auto made = make_world(7, {{3, 3, 2, 3}}, false);
    auto remade = make_world(7, {{3, 3, 2, 3}}, false);
    for (const char* ext : randommap::kFiles) {
        EXPECT_TRUE(made.count(ext) && !made[ext].empty()) << ext;
        EXPECT_EQ(made[ext], remade[ext]) << ext;
    }
    EXPECT_EQ(made["SCN"].size(), 12001U);
    EXPECT_EQ(made["MAP"].size(), static_cast<size_t>(2 * 112 * 156));

    auto map = *scn::LoadFromFiles(kData / "RANDOM", "RANDOM", made);
    EXPECT_EQ(map->Cities().size(), 80U);
    EXPECT_EQ(map->Sites().size(), 40U);
    EXPECT_GE(map->Signs().size(), 41U);
    EXPECT_LE(map->Signs().size(), 70U);

    std::set<City*> capitals;
    for (auto& side : map->Sides()) {
        City* cap = side.CapitalCity(*map);
        EXPECT_TRUE(cap != nullptr && cap->Owner() == side.Id()) << side.Name();
        capitals.insert(cap);
    }
    EXPECT_EQ(capitals.size(), 8U);

    for (const auto& city : map->Cities()) {
        EXPECT_EQ(map->TerrainTypeOf(scn::TileAt(*map, city.X(), city.Y()) % 256), 10)
                << city.Name();
        EXPECT_FALSE(city.Produces().empty()) << city.Name();
        bool is_capital = !city.IsNeutral();
        EXPECT_GE(city.Income(), is_capital ? 33 : 15) << city.Name();
        EXPECT_LE(city.Income(), is_capital ? 40 : 28) << city.Name();
        const auto* description = map->CityDescription(city.Index());
        ASSERT_NE(description, nullptr);
        EXPECT_TRUE((*description)[0].starts_with(city.Name() + " is a")) << city.Name();
    }

    Types types;
    ASSERT_TRUE(armytype::Load(kData / "TERRAIN0/ARMYTYPE.DAT", types).ok());
    auto is_magical = [&](int type) {
        return types.ById(type) != nullptr && types.ById(type)->bonus[48] != 0;
    };
    bool no_magical = true;
    for (const auto& city : map->Cities()) {
        for (int produced : city.Produces()) {
            if (is_magical(produced)) {
                no_magical = false;
            }
        }
    }
    EXPECT_TRUE(no_magical);

    int magical_count = 0;
    for (int seed = 1; seed <= 10; ++seed) {
        auto allied_files = make_world(seed, {{3, 3, 2, 3}}, true);
        auto allied = *scn::LoadFromFiles(kData / "RANDOM", "RANDOM", allied_files);
        for (const auto& city : allied->Cities()) {
            for (int produced : city.Produces()) {
                if (is_magical(produced)) {
                    ++magical_count;
                }
            }
        }
    }
    EXPECT_GT(magical_count, 0);

    EXPECT_EQ(
            (*scn::LoadFromFiles(kData / "RANDOM", "RANDOM", make_world(3, {{0, 0, 6, 0}}, false)))
                    ->Cities()
                    .size(),
            100U);
    EXPECT_EQ(
            (*scn::LoadFromFiles(kData / "RANDOM", "RANDOM", make_world(3, {{6, 6, 0, 6}}, false)))
                    ->Cities()
                    .size(),
            70U);

    Rng slider_rng(5);
    auto rolled = randommap::Settle({{7, 7, 7, 7}}, slider_rng);
    for (int slider : rolled) {
        EXPECT_GE(slider, 0);
        EXPECT_LE(slider, 6);
    }

    auto on_disk = ReadBytes(kData / "RANDOM/RANDOM.SCN");
    if (on_disk.ok()) {
        EXPECT_NE(*on_disk, made["SCN"]);
    }

    game::NewGameOptions options;
    options.seed = 4;
    options.memory_files = &made;
    auto game_ptr = *game::NewGame(kData, "RANDOM", options);
    Game& game = *game_ptr;
    for (Side* player : game.Sides()) {
        player->SetComputerControlled(true);
    }
    Side* side = game::Begin(game);
    while (side != nullptr && game.Turn() <= 6) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    EXPECT_EQ(game.Turn(), 7);

    std::string save_text = save::Encode(game);
    auto reloaded = *save::Decode(save_text, kData);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->GetMap().Tiles(), game.GetMap().Tiles());
    ASSERT_EQ(reloaded->GetMap().Cities().size(), game.GetMap().Cities().size());
    for (size_t i = 0; i < game.GetMap().Cities().size(); ++i) {
        EXPECT_EQ(reloaded->GetMap().Cities()[i].Name(), game.GetMap().Cities()[i].Name());
    }
    EXPECT_EQ(reloaded->GetMap().Signs().size(), game.GetMap().Signs().size());
}

}  // namespace
}  // namespace w2
