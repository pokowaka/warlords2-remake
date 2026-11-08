/**
 * @file
 * @brief Quests: taking one at a temple, checking it off, and the reward.
 *
 * See docs/rules.md > Quests. The original's routines are quest_assign,
 * quest_check and quest_choose_reward. One quest per side at a time.
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

/// The quest types (Quest::type).
constexpr int kSlayHero = 0, kRetrieveItem = 1, kSlayType = 2;
constexpr int kSlaughter = 3, kOccupy = 4, kRaze = 5, kPillageGold = 6;

/// The original's quest type table, rolled with 1d10: types 4, 5 and 6 come
/// up twice as often.
extern const std::vector<int> kTypeTable;
/// A line of prose per quest type; some take the required count.
extern const char* const kDescriptions[7];
/// How far a target may be from the hero: a human's, a computer's.
constexpr int kItemRange = 50, kCityRange = 60;
constexpr int kAiItemRange = 30, kAiCityRange = 40, kRazeRange = 60;
/// What a quest's target *is*, by quest type, so it can be saved and restored
/// by reference.
extern const char* const kTargetKind[7];
constexpr int kExperience = 10;  ///< Experience a hero gains for finishing a quest.
/// The allies a quest gives are always Archons (army type 19), as in the
/// original.
constexpr int kRewardAlly = 19;

/**
 * @brief Takes a quest at a temple, or none (the original's quest_assign).
 *
 * Tries until a quest finds a target; pillage gold always does.
 * @param hero The hero taking the quest.
 * @param computer For a computer player's quest: other types first and
 *     shorter ranges.
 * @param flying Whether the searching stack flies: a retrieve-item quest
 *     uncovers more round the item's site.
 * @return The quest, or nullptr when the side has one or quests are off.
 */
std::shared_ptr<Quest> Assign(Game& game, Side& side, Army* hero, bool computer = false,
                              bool flying = false);
/** @brief A line of prose for a quest. */
std::string Describe(const Quest& quest);

/** @brief What just happened, for Notify(). */
struct Event {
    std::vector<Army*> stack;   ///< The stack that acted (the quest hero must be in it).
    std::vector<Army*> killed;  ///< The armies killed in a battle.
    City* city = nullptr;       ///< The city pillaged, occupied or razed.
    int gold = 0;               ///< The gold pillaged.
    Army* hero = nullptr;       ///< The hero concerned.
};
/**
 * @brief Tells the quest what just happened (the original's quest_check).
 * @param event "battle", "item", "pillage", "occupy", "raze" or "turn".
 * @return How the quest ended, or nullptr while it goes on.
 */
std::shared_ptr<QuestResult> Notify(Game& game, Side& side, const std::string& event,
                                    const Event& data = Event());
/**
 * @brief Chooses and gives the reward (the original's quest_choose_reward
 * and quest_give_reward).
 */
std::shared_ptr<Reward> ChooseReward(Game& game, Side& side, const Quest* quest);

}  // namespace quest
}  // namespace w2
