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

const std::filesystem::path kData = "original";

const std::vector<std::string> kScenarios = {"ERYTHEA", "ILLURIA", "DRAGON", "HADESHA",
                                             "ISLADIA", "SORCERY", "TUTORIA"};

bool ScenarioExists(std::string_view scenario) {
    std::filesystem::path scn_path = kData / scenario / (std::string(scenario) + ".SCN");
    return FileExists(scn_path);
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
    return *game::NewGame(kData, scenario, new_game_options);
}

Side* EndTurn(Game& game) {
    Side* side = game::EndTurn(game);
    if (side != nullptr && !side->IsComputerControlled()) {
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
    auto& offer = game.TurnNews(side.Id()).hero_offer;
    if (!side.IsComputerControlled() && offer) {
        hero::Recruit(game, side, *offer);
        offer.reset();
    }
    ai::PlayTurn(game, side);
}

Army* AddArmy(Game& game, int x, int y, int owner, int type, int strength, int moves,
              int max_moves) {
    Army army;
    army.PlaceAt(TilePos(x, y));
    army.SetOwner(SideId(owner));
    army.SetType(ArmyTypeId(type));
    army.SetStrength(strength);
    army.SetMoves(moves);
    army.SetMaxMoves(max_moves);
    const ArmyType* army_type = game.types.ById(type);
    army.SetName(army_type != nullptr ? army_type->name : "");
    return game.Add(army);
}

Army* AddTypedArmy(Game& game, int x, int y, int owner, int type, int strength) {
    const ArmyType* army_type = game.types.ById(type);
    Army army;
    army.PlaceAt(TilePos(x, y));
    army.SetOwner(SideId(owner));
    army.SetType(ArmyTypeId(type));
    army.SetName(army_type->name);
    army.SetStrength(strength != kNone ? strength : army_type->strength);
    army.SetMaxMoves(army_type->move);
    army.SetMoves(army_type->move);
    army.SetUpkeep(1);
    return game.Add(army);
}

City* FindNeutralCity(Game& game, size_t min_slots, const City* exclude) {
    for (auto& city : game.map->cities) {
        if (&city != exclude && city.IsNeutral() && !city.IsRazed() &&
            city.Slots().size() >= min_slots) {
            return &city;
        }
    }
    return nullptr;
}

City* FindEnemyCity(Game& game, int side_index) {
    for (auto& city : game.map->cities) {
        if (!city.OwnedBy(side_index)) {
            return &city;
        }
    }
    return nullptr;
}

City* FindCityByName(Game& game, std::string_view name) {
    for (auto& city : game.map->cities) {
        if (city.Name() == name) {
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
            while (game::ArmiesAt(game, city.X() + dx, city.Y() + dy).size() <
                   static_cast<size_t>(rules::kMaxStack)) {
                AddTypedArmy(game, city.X() + dx, city.Y() + dy, owner, type);
            }
        }
    }
}

std::shared_ptr<Quest> MakeCityQuest(int type, Army* hero, City* target) {
    ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
    CityId city_id = target ? target->Id() : CityId::None();
    return std::make_shared<Quest>(type, hero_id, city_id);
}

std::shared_ptr<Quest> MakeSlaughterQuest(Army* hero, Side* target_side, int required) {
    ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
    SideId side_id = target_side ? target_side->Id() : SideId::None();
    return std::make_shared<Quest>(quest::kSlaughter, hero_id, side_id, required, 0);
}

std::shared_ptr<Quest> MakePillageQuest(Army* hero, int required) {
    ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
    return std::make_shared<Quest>(quest::kPillageGold, hero_id, std::monostate{}, required, 0);
}

std::shared_ptr<Quest> MakeItemQuest(Army* hero, Item* item) {
    ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
    ItemId item_id = item ? ItemId(item->index) : ItemId::None();
    return std::make_shared<Quest>(quest::kRetrieveItem, hero_id, item_id);
}

std::shared_ptr<Quest> MakeSlayHeroQuest(Army* hero, Army* prey) {
    ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
    ArmyId prey_id = prey ? prey->Id() : ArmyId::None();
    return std::make_shared<Quest>(quest::kSlayHero, hero_id, prey_id);
}

ScopedBugFlags::ScopedBugFlags(Game& game) : game_(game), saved_bugs_(game.RulesBugs()) {}

ScopedBugFlags::~ScopedBugFlags() {
    game_.RulesBugs() = saved_bugs_;
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
