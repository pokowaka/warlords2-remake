/**
 * @file
 * @brief Side (player/faction), Quest, Advisor, Promotion, AIThought, and DiplomaticMissive
 * structs.
 */
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "warlords/model/army_type.hpp"

namespace w2 {

struct City;
struct Army;
struct Item;
struct Side;
struct Reward;
struct QuestResult;
struct HeroOffer;
struct AIData;
struct SideBelief;

/** @brief What a side's turn start produced, for the report (side.produced). */
struct Produced {
    std::string kind;       ///< "built", "sent" or "arrived".
    int type = 0;           ///< The army type.
    int city = kNone;       ///< The city it concerns.
    bool standard = false;  ///< It went to the side's standard.
};

/** @brief A quest (quest.lua). The target is one of the pointers, as target_kind says. */
struct Quest {
    int type = 0;                         ///< The kind of quest.
    Army* hero = nullptr;                 ///< The hero on the quest.
    std::string target_kind;              ///< "city", "side", "item", "armytype", "army", "none".
    City* city = nullptr;                 ///< The target city.
    Side* side = nullptr;                 ///< The target side.
    Item* item = nullptr;                 ///< The target item.
    const ArmyType* army_type = nullptr;  ///< The target army type.
    Army* army = nullptr;                 ///< The target army.
    int required = kNone;                 ///< How many are needed, for a counting quest.
    int done = 0;                         ///< How many are done so far.
    /** @brief Does the quest still have its target? */
    bool HasTarget() const {
        return target_kind == "none" || city || side || item || army_type || army;
    }
};

/** @brief The advisor's marks (cues.lua). */
struct Advisor {
    int dir = 0;   ///< 1 after a rise in cities, 2 after a fall.
    int mark = 0;  ///< The city count last remarked on, a multiple of 5.
};

/** @brief A hero promoted as the turn opened, and the strength it gained. */
struct Promotion {
    Army* hero = nullptr;  ///< The hero.
    int gain = 0;          ///< The strength it gained.
};

/**
 * @brief A scored macro-action recorded by the modern ML AI for the in-game F10 Brain Inspector.
 */
struct AIThought {
    std::string category;     ///< What kind of action.
    std::string actor_name;   ///< Who would act.
    std::string target_name;  ///< On what.
    int sx = kNone;           ///< Column the actor stands on.
    int sy = kNone;           ///< Row the actor stands on.
    int tx = kNone;           ///< Column of the target.
    int ty = kNone;           ///< Row of the target.
    float score = 0.0F;       ///< The action's score.
    float win_prob = 0.0F;    ///< The predicted chance of winning the fight.
    bool stage_near = false;  ///< Stage near the target rather than strike it.
    std::string detail;       ///< Free text for the inspector.
};

/** @brief A turn-start diplomatic parley missive sent from an AI side to a player side. */
struct DiplomaticMissive {
    int from_side = kNone;          ///< The sender.
    int to_side = kNone;            ///< The receiver.
    int proposed_state = kNone;     ///< The diplomatic state proposed.
    bool propose_alliance = false;  ///< An alliance is proposed.
    int gold_demand = 0;            ///< > 0: from_side demands gold; < 0: from_side offers gold.
    int joint_war_target = kNone;   ///< The side proposed for a joint war.
    std::string headline;           ///< The missive's title.
    std::string body;               ///< Its text.
    std::string reason_summary;     ///< Why it was sent.
};

/** @brief A player: human or computer, with its gold, its capital and its turn's news. */
struct Side {
    int index = 0;                            ///< 0..7.
    std::string name;                         ///< The side's name.
    int colour = 0;                           ///< Its colour.
    int edge = 0;                             ///< The colour of its edging.
    int gold = 0;                             ///< Gold in the treasury.
    int cap_x = 0;                            ///< Column of its capital, as the scenario gives it.
    int cap_y = 0;                            ///< Row of its capital, as the scenario gives it.
    bool computer = false;                    ///< Played by the computer.
    bool classic_ai = true;                   ///< A computer playing the original's AI.
    int level = 0;                            ///< The computer's level.
    bool enhanced = false;                    ///< Enhanced: its armies are built stronger.
    bool observe = false;                     ///< The human watches this computer's turns.
    int diplo_score = 0;                      ///< Its diplomatic score.
    int card = 0;                             ///< The computer's personality card (aicard).
    bool in_use = false;                      ///< In this game at all.
    City* capital = nullptr;                  ///< Its capital city.
    bool alive = false;                       ///< Still in the game.
    int owner_index = 0;                      ///< The side's own index, as an owner.
    int income = 0;                           ///< The turn's income.
    int upkeep_total = 0;                     ///< The turn's upkeep.
    std::vector<Produced> produced;           ///< What the turn start produced.
    std::vector<std::string> diplo_news;      ///< Diplomatic news for the turn.
    std::shared_ptr<HeroOffer> hero_offer;    ///< A hero offering to join, if any.
    std::shared_ptr<Quest> quest;             ///< Its quest, if any.
    std::shared_ptr<QuestResult> quest_news;  ///< How its quest went, to tell.
    std::shared_ptr<AIData> ai;               ///< The computer player's memory.
    std::shared_ptr<SideBelief> belief;       ///< What the modern AI believes of the others.
    std::vector<AIThought> ai_thoughts;       ///< For the Brain Inspector.
    std::vector<DiplomaticMissive> missives;  ///< Missives waiting to be read.
    std::array<int, 8> last_missive_turn{};   ///< The turn it last sent each side a missive.
    float last_board_value = 0.0F;            ///< The modern AI's last view of its position.
    int ai_solidarity = 0;                    ///< The computer's solidarity with its allies.
    Advisor advisor;                          ///< The advisor's marks.
    int cap_now_x = kNone;  ///< Column of where the capital now stands; kNone until the first turn.
    int cap_now_y = kNone;  ///< Row of where the capital now stands.
    bool production_due = false;      ///< A human's cities build once the offer is settled.
    std::vector<Promotion> promoted;  ///< For the interface to show.
};

}  // namespace w2
