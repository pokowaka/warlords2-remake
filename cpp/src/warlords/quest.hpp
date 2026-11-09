/**
 * @file
 * @brief `Reward`, `QuestResult`, `QuestSystem` domain service, and `w2::quest` wrappers.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

/** @brief What a finished quest paid (the original's quest_choose_reward). */
struct Reward {
    std::string kind;                ///< "allies", "gold", "item", "revealed".
    std::vector<Army*> armies;       ///< The allies given, as many as had room.
    const ArmyType* type = nullptr;  ///< The allies' army type.
    int count = 0;                   ///< Allies given, room or not.
    int gold = 0;                    ///< The gold given.
    Item* item = nullptr;            ///< The item given.
    Site* site = nullptr;            ///< The rich site revealed.
};

/** @brief A quest's end: done with a reward, or failed for a reason. */
struct QuestResult {
    std::shared_ptr<Quest> quest;    ///< The quest that ended.
    std::shared_ptr<Reward> reward;  ///< The reward, when it was done.
    std::string failed;              ///< Why it failed; "" when it was done.
    int why = 0;  ///< The STRING.DAT group quest_check tells a human the failure with.
};

namespace quest {

constexpr int kSlayHero = Quest::kSlayHero;
constexpr int kRetrieveItem = Quest::kRetrieveItem;
constexpr int kSlayType = Quest::kSlayType;
constexpr int kSlaughter = Quest::kSlaughter;
constexpr int kOccupy = Quest::kOccupy;
constexpr int kRaze = Quest::kRaze;
constexpr int kPillageGold = Quest::kPillageGold;

extern const std::vector<int> kTypeTable;
extern const char* const kDescriptions[7];
constexpr int kItemRange = 50, kCityRange = 60;
constexpr int kAiItemRange = 30, kAiCityRange = 40, kRazeRange = 60;
constexpr int kExperience = 10;
constexpr int kRewardAlly = 19;

/** @brief What just happened, for Notify(). */
struct Event {
    std::vector<Army*> stack;   ///< The stack that acted (the quest hero must be in it).
    std::vector<Army*> killed;  ///< The armies killed in a battle.
    City* city = nullptr;       ///< The city pillaged, occupied or razed.
    int gold = 0;               ///< The gold pillaged.
    Army* hero = nullptr;       ///< The hero concerned.
};

std::shared_ptr<Quest> Assign(Game& game, Side& side, Army* hero, bool computer = false,
                              bool flying = false);
std::string Describe(const Game& game, const Quest& quest);
std::shared_ptr<QuestResult> Notify(Game& game, Side& side, const std::string& event,
                                    const Event& data = Event());
std::shared_ptr<Reward> ChooseReward(Game& game, Side& side, const Quest* quest);

}  // namespace quest

/**
 * @brief Domain service governing temple quest assignment, progress/failure tracking,
 * reward selection, and human quest-news reporting on `Game::TurnNews`.
 */
class QuestSystem {
  public:
    /** @brief Binds a quest service to `game` (must outlive this service instance). */
    explicit QuestSystem(Game& game) : game_(game) {}

    /** @brief Assigns a new quest to `hero` at a temple if `side` has none active. */
    std::shared_ptr<Quest> Assign(Side& side, Army* hero, bool computer = false,
                                  bool flying = false);

    /** @brief Processes a turn or action event (`"turn"`, `"battle"`, `"item"`, etc.) for `side`.
     */
    std::shared_ptr<QuestResult> Notify(Side& side, const std::string& event,
                                        const quest::Event& data = quest::Event());

    /** @brief Chooses and grants a completion reward for `quest` to `side`. */
    std::shared_ptr<Reward> ChooseReward(Side& side, const Quest* quest);

    /** @brief Formats a human-readable objective description for `quest`. */
    static std::string Describe(const Game& game, const Quest& quest);

  private:
    std::shared_ptr<QuestResult> Finish(Side& side, const std::string& reason, int why = 0);

    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
