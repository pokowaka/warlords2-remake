/**
 * @file
 * @brief Shared fixtures, scenario helpers, and RAII state guards for headless GoogleTest suites.
 */
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/types.hpp"

namespace w2::test {

/// Repository-relative path to the shipped Warlords II game assets.
extern const std::string kData;

/// Canonical scenario directory names exercised across scenario-parameterized tests.
extern const std::vector<std::string> kScenarios;

/**
 * @brief Checks whether a scenario's `.SCN` file is installed under `kData`.
 * @param scenario Scenario directory and stem name (e.g. `"ERYTHEA"`).
 * @return True when `<kData>/<scenario>/<scenario>.SCN` exists on disk.
 */
bool ScenarioExists(std::string_view scenario);

/**
 * @brief Formats a scenario name as a valid GoogleTest parameter suffix.
 * @param info Parameter metadata supplied by `INSTANTIATE_TEST_SUITE_P`.
 * @return The scenario identifier string.
 */
std::string ScenarioParamName(const ::testing::TestParamInfo<std::string>& info);

/**
 * @brief Test-side configuration wrapper for `game::NewGameOptions` with concise defaults.
 */
struct GameOpts {
    double seed = 0.0;                                 ///< Deterministic RNG seed for setup.
    std::vector<std::pair<std::string, int>> options;  ///< Scenario option overrides by name.
    std::map<int, game::SideSetup> sides;              ///< Per-side setup overrides by index.
    bool greatest = false;                             ///< Enable "I am the Greatest" mode.
};

/** @brief Builds `GameOpts` with only the random seed set. */
GameOpts Seed(double seed);

/** @brief Builds `GameOpts` with a random seed and named scenario option overrides. */
GameOpts SeedWith(double seed, std::vector<std::pair<std::string, int>> options);

/** @brief Builds a `game::SideSetup` descriptor for human/computer status, AI level, or off. */
game::SideSetup MakeSideSetup(bool computer, int level = kNone, bool off = false);

/**
 * @brief Creates a new headless game instance of `scenario` from `kData`.
 * @param scenario Scenario directory/stem name under `kData`.
 * @param game_options Seed, rule flags, and faction overrides.
 * @return Owning pointer to the initialized `Game`.
 */
std::unique_ptr<Game> SetupGame(const std::string& scenario,
                                const GameOpts& game_options = GameOpts{});

/**
 * @brief Advances the active turn as the UI does, running `HumanProduction` when a human opens.
 * @return Pointer to the newly active `Side`, or `nullptr` if the game ended.
 */
Side* EndTurn(Game& game);

/** @brief Advances `rounds` full rounds across all sides currently in play. */
void AdvanceRounds(Game& game, size_t rounds);

/**
 * @brief Plays one turn using the AI controller, recruiting any standing human hero offer first.
 */
void PlayAi(Game& game, Side& side);

/**
 * @brief Adds an army with explicit strength and movement values onto `game`.
 * @return Pointer to the stored `Army` inside `game`.
 */
Army* AddArmy(Game& game, int x, int y, int owner, int type, int strength, int moves = 0,
              int max_moves = 0);

/**
 * @brief Adds an army initialized from its `ArmyType` catalog defaults (with optional strength).
 * @return Pointer to the stored `Army` inside `game`.
 */
Army* AddTypedArmy(Game& game, int x, int y, int owner, int type, int strength = kNone);

/** @brief Finds the first neutral city on `game` with at least `min_slots` production slots. */
City* FindNeutralCity(Game& game, size_t min_slots = 0, const City* exclude = nullptr);

/** @brief Finds the first city on `game` not owned by `side_index`. */
City* FindEnemyCity(Game& game, int side_index);

/** @brief Finds a city on `game` by its exact name, or returns `nullptr`. */
City* FindCityByName(Game& game, std::string_view name);

/** @brief Finds the first map site on `game` matching `content`, or returns `nullptr`. */
Site* FindSiteByContent(Game& game, int content);

/** @brief Finds the first map tile matching `terrain` (optionally unoccupied and non-crossing). */
TilePos FindTerrainTile(const Game& game, int terrain, bool empty_non_crossing = false);

/** @brief Fills all four 2x2 footprint tiles of `city` up to `rules::kMaxStack` armies. */
void FillCityFootprint(Game& game, const City& city, int owner, int type);

/** @brief Creates an occupy/raze city `Quest` record bound to `hero` and `target`. */
std::shared_ptr<Quest> MakeCityQuest(int type, Army* hero, City* target);

/** @brief Creates a `quest::kSlaughter` quest targeting `target_side` with `required` kills. */
std::shared_ptr<Quest> MakeSlaughterQuest(Army* hero, Side* target_side, int required);

/** @brief Creates a `quest::kPillageGold` quest requiring `required` gold pillaged. */
std::shared_ptr<Quest> MakePillageQuest(Army* hero, int required);

/** @brief Creates a `quest::kRetrieveItem` quest targeting `item`. */
std::shared_ptr<Quest> MakeItemQuest(Army* hero, Item* item);

/** @brief Creates a `quest::kSlayHero` quest targeting `prey`. */
std::shared_ptr<Quest> MakeSlayHeroQuest(Army* hero, Army* prey);

/**
 * @brief RAII guard that snapshots `rules::bugs` on construction and restores it on scope exit.
 */
class ScopedBugFlags {
  public:
    ScopedBugFlags();
    ~ScopedBugFlags();

    ScopedBugFlags(const ScopedBugFlags&) = delete;
    ScopedBugFlags& operator=(const ScopedBugFlags&) = delete;

  private:
    rules::Bugs saved_bugs_;  ///< Snapshot of `rules::bugs` prior to test mutation.
};

/**
 * @brief RAII guard that snapshots `ai::DefaultClassicAi()` and restores it on scope exit.
 */
class ScopedDefaultAi {
  public:
    ScopedDefaultAi();
    explicit ScopedDefaultAi(bool classic_ai);
    ~ScopedDefaultAi();

    ScopedDefaultAi(const ScopedDefaultAi&) = delete;
    ScopedDefaultAi& operator=(const ScopedDefaultAi&) = delete;

  private:
    bool saved_classic_ai_;  ///< Previous `ai::DefaultClassicAi()` setting.
};

/**
 * @brief RAII temporary filesystem path under the system temp directory, cleaned up on destruction.
 */
class ScopedTempPath {
  public:
    explicit ScopedTempPath(std::string_view leaf_name);
    ~ScopedTempPath();

    ScopedTempPath(const ScopedTempPath&) = delete;
    ScopedTempPath& operator=(const ScopedTempPath&) = delete;

    /** @brief Returns the managed temporary file or directory path. */
    const std::filesystem::path& Path() const { return path_; }

  private:
    std::filesystem::path path_;  ///< Managed path removed on construction and destruction.
};

/**
 * @brief Base fixture for scenario-parameterized GoogleTest suites (`TEST_P`).
 *
 * Automatically skips scenarios whose `.SCN` file is absent from `kData` (such as `"ILLURIA"`).
 */
class ScenarioTest : public ::testing::TestWithParam<std::string> {
  protected:
    void SetUp() override;
};

}  // namespace w2::test
