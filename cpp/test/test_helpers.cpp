/**
 * @file
 * @brief Implementation of shared GoogleTest scenario helpers and RAII state guards.
 */
#include "test/test_helpers.hpp"

#include <system_error>

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/armytype.hpp"
#include "warlords/hero.hpp"
#include "warlords/quest.hpp"
#include "warlords/scn.hpp"

namespace w2::test {

const std::string kData = "original";

const std::vector<std::string> kScenarios = {"ERYTHEA", "ILLURIA", "DRAGON", "HADESHA",
                                             "ISLADIA", "SORCERY", "TUTORIA"};

bool ScenarioExists(std::string_view scenario) {
    std::filesystem::path scn_path =
            std::filesystem::path(kData) / scenario / (std::string(scenario) + ".SCN");
    return FileExists(scn_path.string());
}

std::string ScenarioParamName(const ::testing::TestParamInfo<std::string>& info) {
    return info.param;
}

GameOpts Seed(double seed) {
    GameOpts game_options;
    game_options.seed = seed;
    return game_options;
}

GameOpts SeedWith(double seed, std::vector<std::pair<std::string, int>> options) {
    GameOpts game_options;
    game_options.seed = seed;
    game_options.options = std::move(options);
    return game_options;
}

game::SideSetup MakeSideSetup(bool computer, int level, bool off) {
    game::SideSetup setup;
    setup.computer = computer;
    setup.level = level;
    setup.off = off;
    return setup;
}

std::unique_ptr<Game> SetupGame(const std::string& scenario, const GameOpts& game_options) {
    game::NewGameOptions new_game_options;
    new_game_options.seed = game_options.seed;
    new_game_options.options = game_options.options;
    new_game_options.sides = game_options.sides;
    new_game_options.greatest = game_options.greatest;
    return game::NewGame(kData, scenario, new_game_options);
}

Side* EndTurn(Game& game) {
    Side* side = game::EndTurn(game);
    if (side != nullptr && !side->computer) {
        game::HumanProduction(game, *side);
    }
    return side;
}

void AdvanceRounds(Game& game, size_t rounds) {
    size_t total_turns = rounds * game.sides.size();
    for (size_t i = 0; i < total_turns; ++i) {
        EndTurn(game);
    }
}

void PlayAi(Game& game, Side& side) {
    if (!side.computer && side.hero_offer) {
        hero::Recruit(game, side, *side.hero_offer);
        side.hero_offer.reset();
    }
    ai::PlayTurn(game, side);
}

Army* AddArmy(Game& game, int x, int y, int owner, int type, int strength, int moves,
              int max_moves) {
    Army army;
    army.x = x;
    army.y = y;
    army.owner = owner;
    army.type = type;
    army.strength = strength;
    army.moves = moves;
    army.max_moves = max_moves;
    const ArmyType* army_type = game.types.ById(type);
    army.name = army_type != nullptr ? army_type->name : "";
    return game.Add(army);
}

Army* AddTypedArmy(Game& game, int x, int y, int owner, int type, int strength) {
    const ArmyType* army_type = game.types.ById(type);
    Army army;
    army.x = x;
    army.y = y;
    army.owner = owner;
    army.type = type;
    army.name = army_type->name;
    army.strength = strength != kNone ? strength : army_type->strength;
    army.max_moves = army_type->move;
    army.moves = army_type->move;
    army.upkeep = 1;
    return game.Add(army);
}

City* FindNeutralCity(Game& game, size_t min_slots, const City* exclude) {
    for (auto& city : game.map->cities) {
        if (&city != exclude && city.owner_index == kNone && !city.razed &&
            city.slots.size() >= min_slots) {
            return &city;
        }
    }
    return nullptr;
}

City* FindEnemyCity(Game& game, int side_index) {
    for (auto& city : game.map->cities) {
        if (city.owner_index != side_index) {
            return &city;
        }
    }
    return nullptr;
}

City* FindCityByName(Game& game, std::string_view name) {
    for (auto& city : game.map->cities) {
        if (city.name == name) {
            return &city;
        }
    }
    return nullptr;
}

Site* FindSiteByContent(Game& game, int content) {
    for (auto& map_site : game.map->sites) {
        if (map_site.content == content) {
            return &map_site;
        }
    }
    return nullptr;
}

TilePos FindTerrainTile(const Game& game, int terrain, bool empty_non_crossing) {
    for (int y = 0; y < game.map->height; ++y) {
        for (int x = 0; x < game.map->width; ++x) {
            if (scn::TerrainAt(*game.map, x, y) != terrain) {
                continue;
            }
            if (empty_non_crossing &&
                (scn::IsCrossing(*game.map, x, y) || !game.ArmiesOnTile(x, y).empty())) {
                continue;
            }
            return TilePos(x, y);
        }
    }
    return TilePos();
}

void FillCityFootprint(Game& game, const City& city, int owner, int type) {
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            while (game::ArmiesAt(game, city.x + dx, city.y + dy).size() <
                   static_cast<size_t>(rules::kMaxStack)) {
                AddTypedArmy(game, city.x + dx, city.y + dy, owner, type);
            }
        }
    }
}

std::shared_ptr<Quest> MakeCityQuest(int type, Army* hero, City* target) {
    auto city_quest = std::make_shared<Quest>();
    city_quest->type = type;
    city_quest->hero = hero;
    city_quest->target_kind = "city";
    city_quest->city = target;
    city_quest->done = 0;
    return city_quest;
}

std::shared_ptr<Quest> MakeSlaughterQuest(Army* hero, Side* target_side, int required) {
    auto slaughter_quest = std::make_shared<Quest>();
    slaughter_quest->type = quest::kSlaughter;
    slaughter_quest->hero = hero;
    slaughter_quest->target_kind = "side";
    slaughter_quest->side = target_side;
    slaughter_quest->required = required;
    return slaughter_quest;
}

std::shared_ptr<Quest> MakePillageQuest(Army* hero, int required) {
    auto pillage_quest = std::make_shared<Quest>();
    pillage_quest->type = quest::kPillageGold;
    pillage_quest->hero = hero;
    pillage_quest->target_kind = "none";
    pillage_quest->required = required;
    return pillage_quest;
}

std::shared_ptr<Quest> MakeItemQuest(Army* hero, Item* item) {
    auto item_quest = std::make_shared<Quest>();
    item_quest->type = quest::kRetrieveItem;
    item_quest->hero = hero;
    item_quest->target_kind = "item";
    item_quest->item = item;
    return item_quest;
}

std::shared_ptr<Quest> MakeSlayHeroQuest(Army* hero, Army* prey) {
    auto slay_quest = std::make_shared<Quest>();
    slay_quest->type = quest::kSlayHero;
    slay_quest->hero = hero;
    slay_quest->target_kind = "army";
    slay_quest->army = prey;
    return slay_quest;
}

ScopedBugFlags::ScopedBugFlags() : saved_bugs_(rules::bugs) {}

ScopedBugFlags::~ScopedBugFlags() {
    rules::bugs = saved_bugs_;
}

ScopedDefaultAi::ScopedDefaultAi() : saved_classic_ai_(ai::DefaultClassicAi()) {}

ScopedDefaultAi::ScopedDefaultAi(bool classic_ai) : saved_classic_ai_(ai::DefaultClassicAi()) {
    ai::SetDefaultClassicAi(classic_ai);
}

ScopedDefaultAi::~ScopedDefaultAi() {
    ai::SetDefaultClassicAi(saved_classic_ai_);
}

ScopedTempPath::ScopedTempPath(std::string_view leaf_name)
        : path_(std::filesystem::temp_directory_path() / leaf_name) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

ScopedTempPath::~ScopedTempPath() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

void ScenarioTest::SetUp() {
    if (!ScenarioExists(GetParam())) {
        GTEST_SKIP() << "Scenario " << GetParam() << " is not installed under " << kData;
    }
}

}  // namespace w2::test
