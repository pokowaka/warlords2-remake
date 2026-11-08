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
#include <fstream>
#include <iterator>
#include <set>
#include <string>
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
    auto map = scn::Load(kData + "/" + scenario, scenario);
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

// NOLINTNEXTLINE(readability-identifier-length)
INSTANTIATE_TEST_SUITE_P(DesignerMaps, ShippedMapEvalTest,
                         ::testing::Values("ERYTHEA", "HADESHA", "ISLADIA", "DRAGON", "SORCERY"),
                         ScenarioParamName);

TEST(MapEvalTest, BrokenMapsTriggerHardChecksFairCapitalsAndReferenceScores) {
    auto map = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
    Map& broken = *map;
    auto index_of = [](int x, int y) { return y * kMapW + x; };
    auto tile_of = [&](int type) {
        for (int tile = 0; tile < 256; ++tile) {
            if (broken.terrain_type[tile] == type) {
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
    broken.roads[index_of(lone_x, lone_y)] = 1;
    broken.cities[1].x = broken.cities[0].x + 1;
    broken.cities[1].y = broken.cities[0].y;
    broken.cities[3].name = broken.cities[2].name;
    broken.signs[0].lines = {broken.cities[5].name, "999 leagues north"};
    const Site& walled = broken.sites[0];
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int x = walled.x + dx;
            int y = walled.y + dy;
            if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= kMapW || y >= kMapH) {
                continue;
            }
            broken.tiles[index_of(x, y)] = tile_of(movement::kMountains);
            broken.roads[index_of(x, y)] = 0;
        }
    }
    auto broken_eval = mapeval::Evaluate(broken, "broken");
    EXPECT_FALSE(broken_eval.Passed());
    for (const char* check : {"road-to-nowhere", "dead-end", "city-gap", "city-terrain",
                              "city-name", "signpost", "unreachable"}) {
        EXPECT_GT(broken_eval.Count(check), 0) << check;
    }

    auto near_map = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
    City* home = near_map->sides[0].capital;
    City* next = nullptr;
    for (auto& candidate : near_map->cities) {
        if (&candidate != home &&
            (!next || std::hypot(candidate.x - home->x, candidate.y - home->y) <
                              std::hypot(next->x - home->x, next->y - home->y))) {
            next = &candidate;
        }
    }
    near_map->sides[1].capital = next;
    EXPECT_GT(mapeval::Evaluate(*near_map, "near").Count("capital-gap"), 0);

    Rng classic_rng(11);
    Rng fair_rng(11);
    randommap::Options fair_options;
    fair_options.data_dir = kData;
    fair_options.rng = &classic_rng;
    auto classic_files = randommap::GenerateNow(fair_options);
    fair_options.rng = &fair_rng;
    fair_options.fair_capitals = true;
    auto fair_files = randommap::GenerateNow(fair_options);
    EXPECT_NE(classic_files["SCN"], fair_files["SCN"]);

    auto fair = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", fair_files);
    std::set<City*> capitals;
    for (auto& fair_side : fair->sides) {
        capitals.insert(fair_side.capital);
    }
    EXPECT_EQ(capitals.size(), 8U);
    auto fair_eval = mapeval::Evaluate(*fair, "fair#11");
    EXPECT_EQ(fair_eval.Count("capital-gap"), 0);
    EXPECT_LE(fair_eval.Metric("remoteCityFrac"), 0.05);
    EXPECT_GE(fair_eval.Metric("capitalGapMinTurns"), 2.5);

    std::vector<mapeval::Evaluation> refs;
    for (const char* scenario : {"ERYTHEA", "HADESHA"}) {
        refs.push_back(mapeval::Evaluate(*scn::Load(kData + "/" + scenario, scenario), scenario));
    }
    auto ref = mapeval::MakeReference(refs);
    for (const auto& designed : refs) {
        EXPECT_DOUBLE_EQ(mapeval::ScoreAgainst(designed, ref).penalty, 0.0) << designed.name;
    }

    Rng rng(1);
    randommap::Options gen_options;
    gen_options.data_dir = kData;
    gen_options.rng = &rng;
    auto files = randommap::GenerateNow(gen_options);
    auto made = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", files);
    auto made_eval = mapeval::Evaluate(*made, "classic#1");
    EXPECT_EQ(static_cast<int>(made_eval.Metric("cities")), 80);
    EXPECT_NE(mapeval::ToJson(made_eval).Dump().find("\"metrics\""), std::string::npos);
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
            auto scengen_files = randommap::GenerateNow(scengen_options);
            scengen_options.rng = &again;
            EXPECT_EQ(scengen_files, randommap::GenerateNow(scengen_options))
                    << scengen::ShapeName(shape);

            auto world = scn::LoadFromFiles(kData + "/RANDOM", "RANDOM", scengen_files);
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
            EXPECT_EQ(world->options.military_advisor, 1);
        }
    }

    auto fin_data = ReadFile(kData + "/CHANCE17.FIN");
    ASSERT_TRUE(fin_data.has_value());
    int glyph_count = static_cast<uint8_t>((*fin_data)[0]);
    int first_char = static_cast<uint8_t>((*fin_data)[1]);
    auto measure_width = [&](const std::string& text) {
        int pixels = 0;
        for (unsigned char glyph : text) {
            if (glyph >= first_char && glyph < first_char + glyph_count) {
                pixels +=
                        static_cast<uint8_t>((*fin_data)[11 + glyph_count + (glyph - first_char)]);
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
    std::filesystem::copy_file(kData + "/DATA/SCENARIO.DAT", root / "DATA" / "SCENARIO.DAT");
    scenarios::Entry made_entry{"Continents 7", "GEN7", "A new world, seed 7", 80, 40, 8, true};
    WriteFile((root / "GEN7" / "GEN7.INF").string(), scenarios::Info(made_entry));
    WriteFile((root / "GEN7" / "GEN7.SCN").string(), "");
    WriteFile((root / "NOINFO" / "NOINFO.SCN").string(), "");
    EXPECT_TRUE(scenarios::Remember(root.string(), "NOINFO") &&
                scenarios::Remember(root.string(), "GEN7") &&
                scenarios::Remember(root.string(), "GEN7"));
    auto scengen_lst = ReadFile((root / "DATA" / "SCENGEN.LST").string());
    ASSERT_TRUE(scengen_lst.has_value());
    EXPECT_EQ(*scengen_lst, "GEN7\nNOINFO\n");

    auto list = scenarios::List(root.string());
    ASSERT_EQ(list.size(), 7U);
    EXPECT_EQ(list[3].dir, "Erythea");
    EXPECT_EQ(list.back().name, "Continents 7");
    EXPECT_EQ(list.back().cities, 80);
    EXPECT_TRUE(list.back().generated && !list[0].generated);
    auto parsed = scenarios::ParseInfo(scenarios::Info(made_entry), "GEN7");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->text, made_entry.text);
    EXPECT_EQ(parsed->players, 8);

    auto menu_map = scn::Load(kData + "/ERYTHEA", "ERYTHEA");
    auto pic = preview::MenuPicture(*menu_map, kData);
    EXPECT_EQ(pic.w, 264);
    EXPECT_EQ(pic.h, 225);
    ScopedTempPath pck_file("w2test-SCENARIO.PCK");
    WriteFile(pck_file.Path().string(), pck::Encode(pic));
    auto read_pic = pck::Decode(pck_file.Path().string());
    ASSERT_NE(read_pic, nullptr);
    EXPECT_TRUE(read_pic->w == pic.w && read_pic->h == pic.h && read_pic->px == pic.px);
    std::string png = preview::Png(*menu_map, kData);
    EXPECT_EQ(png.compare(0, 8, "\x89PNG\r\n\x1a\n"), 0);
}

TEST(RandomMapTest, ClassicGeneratorDeterminismSlidersAndSaveRoundTrip) {
    auto make_world = [](int seed, std::array<int, 4> sliders, bool allies) {
        Rng rng(seed);
        randommap::Options options;
        options.data_dir = kData;
        options.rng = &rng;
        options.sliders = sliders;
        options.allies = allies;
        return randommap::GenerateNow(options);
    };
    auto made = make_world(7, {{3, 3, 2, 3}}, false);
    auto remade = make_world(7, {{3, 3, 2, 3}}, false);
    for (const char* ext : randommap::kFiles) {
        EXPECT_TRUE(made.count(ext) && !made[ext].empty()) << ext;
        EXPECT_EQ(made[ext], remade[ext]) << ext;
    }
    EXPECT_EQ(made["SCN"].size(), 12001U);
    EXPECT_EQ(made["MAP"].size(), static_cast<size_t>(2 * 112 * 156));

    randommap::Install(kData, made);
    auto map = scn::Load(kData + "/RANDOM", "RANDOM");
    EXPECT_EQ(map->cities.size(), 80U);
    EXPECT_EQ(map->sites.size(), 40U);
    EXPECT_GE(map->signs.size(), 41U);
    EXPECT_LE(map->signs.size(), 70U);

    std::set<City*> capitals;
    for (auto& side : map->sides) {
        EXPECT_TRUE(side.capital != nullptr && side.capital->owner == &side) << side.name;
        capitals.insert(side.capital);
    }
    EXPECT_EQ(capitals.size(), 8U);

    for (const auto& city : map->cities) {
        EXPECT_EQ(map->terrain_type[scn::TileAt(*map, city.x, city.y) % 256], 10) << city.name;
        EXPECT_FALSE(city.produces.empty()) << city.name;
        bool is_capital = city.owner != nullptr;
        EXPECT_GE(city.income, is_capital ? 33 : 15) << city.name;
        EXPECT_LE(city.income, is_capital ? 40 : 28) << city.name;
        auto description = map->city_text.find(city.index);
        ASSERT_NE(description, map->city_text.end());
        EXPECT_TRUE(description->second[0].starts_with(city.name + " is a")) << city.name;
    }

    Types types;
    armytype::Load(kData + "/TERRAIN0/ARMYTYPE.DAT", types);
    auto is_magical = [&](int type) {
        return types.ById(type) != nullptr && types.ById(type)->bonus[48] != 0;
    };
    bool no_magical = true;
    for (const auto& city : map->cities) {
        for (int produced : city.produces) {
            if (is_magical(produced)) {
                no_magical = false;
            }
        }
    }
    EXPECT_TRUE(no_magical);

    int magical_count = 0;
    for (int seed = 1; seed <= 10; ++seed) {
        randommap::Install(kData, make_world(seed, {{3, 3, 2, 3}}, true));
        auto allied = scn::Load(kData + "/RANDOM", "RANDOM");
        for (const auto& city : allied->cities) {
            for (int produced : city.produces) {
                if (is_magical(produced)) {
                    ++magical_count;
                }
            }
        }
    }
    EXPECT_GT(magical_count, 0);

    randommap::Install(kData, make_world(3, {{0, 0, 6, 0}}, false));
    EXPECT_EQ(scn::Load(kData + "/RANDOM", "RANDOM")->cities.size(), 100U);
    randommap::Install(kData, make_world(3, {{6, 6, 0, 6}}, false));
    EXPECT_EQ(scn::Load(kData + "/RANDOM", "RANDOM")->cities.size(), 70U);

    Rng slider_rng(5);
    auto rolled = randommap::Settle({{7, 7, 7, 7}}, slider_rng);
    for (int slider : rolled) {
        EXPECT_GE(slider, 0);
        EXPECT_LE(slider, 6);
    }

    std::ifstream disk(kData + "/RANDOM/RANDOM.SCN", std::ios::binary);
    if (disk) {
        std::string on_disk((std::istreambuf_iterator<char>(disk)),
                            std::istreambuf_iterator<char>());
        EXPECT_NE(on_disk, made["SCN"]);
    }

    randommap::Install(kData, made);
    game::NewGameOptions options;
    options.seed = 4;
    auto game_ptr = game::NewGame(kData, "RANDOM", options);
    Game& game = *game_ptr;
    for (Side* player : game.sides) {
        player->computer = true;
    }
    Side* side = game::Begin(game);
    while (side != nullptr && game.turn <= 6) {
        PlayAi(game, *side);
        side = EndTurn(game);
    }
    EXPECT_EQ(game.turn, 7);

    std::string save_text = save::Encode(game);
    randommap::Install(kData, make_world(8, {{3, 3, 2, 3}}, false));
    auto reloaded = save::Decode(save_text, kData);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->map->tiles, game.map->tiles);
    ASSERT_EQ(reloaded->map->cities.size(), game.map->cities.size());
    for (size_t i = 0; i < game.map->cities.size(); ++i) {
        EXPECT_EQ(reloaded->map->cities[i].name, game.map->cities[i].name);
    }
    EXPECT_EQ(reloaded->map->signs.size(), game.map->signs.size());
}

}  // namespace
}  // namespace w2
