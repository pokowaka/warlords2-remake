/**
 * @file
 * @brief Side (player/faction), Advisor, AIThought, and DiplomaticMissive domain structs.
 */
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "warlords/model/army_type.hpp"
#include "warlords/model/computer_player.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/quest.hpp"
#include "warlords/model/tile_pos.hpp"
#include "warlords/model/turn_report.hpp"

namespace w2 {

struct City;
struct Army;
struct Item;
struct Side;

/** @brief The advisor's marks (cues.lua). */
struct Advisor {
    int dir = 0;   ///< 1 after a rise in cities, 2 after a fall.
    int mark = 0;  ///< The city count last remarked on, a multiple of 5.
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

/** @brief A faction in the scenario: human or computer, with its treasury, capital, and quest. */
struct Side {
    int index = 0;                 ///< 0..7.
    std::string name;              ///< The side's name.
    int colour = 0;                ///< Its colour.
    int edge = 0;                  ///< The colour of its edging.
    int gold = 0;                  ///< Gold in the treasury.
    int cap_x = 0;                 ///< Column of its capital, as the scenario gives it.
    int cap_y = 0;                 ///< Row of its capital, as the scenario gives it.
    bool computer = false;         ///< Played by the computer.
    int level = 0;                 ///< The computer's level.
    bool enhanced = false;         ///< Enhanced: its armies are built stronger.
    bool observe = false;          ///< The human watches this computer's turns.
    int diplo_score = 0;           ///< Its diplomatic score.
    int card = 0;                  ///< The computer's personality card (aicard).
    bool in_use = false;           ///< In this game at all.
    City* capital = nullptr;       ///< Its capital city.
    bool alive = false;            ///< Still in the game.
    int owner_index = 0;           ///< The side's own index, as an owner.
    int income = 0;                ///< The turn's income.
    int upkeep_total = 0;          ///< The turn's upkeep.
    std::shared_ptr<Quest> quest;  ///< Its quest, if any.
    std::shared_ptr<IComputerPlayer> computer_player;  ///< Polymorphic computer player strategy.
    std::vector<DiplomaticMissive> missives;           ///< Missives waiting to be read.
    std::array<int, 8> last_missive_turn{};  ///< The turn it last sent each side a missive.
    int ai_solidarity = 0;                   ///< The computer's solidarity with its allies.
    Advisor advisor;                         ///< The advisor's marks.
    int cap_now_x = kNone;  ///< Column of where the capital now stands; kNone until the first turn.
    int cap_now_y = kNone;  ///< Row of where the capital now stands.
    bool production_due = false;  ///< A human's cities build once the offer is settled.

    /** @brief True if this side is participating in the scenario and still alive. */
    bool IsActive() const { return in_use && alive; }
    /** @brief True if this side is an active human player. */
    bool IsHuman() const { return in_use && alive && !computer; }
    /** @brief True if this side is an active computer player. */
    bool IsComputer() const { return in_use && alive && computer; }
    /** @brief True when this side's computer strategy follows the 1993 Classic AI ruleset. */
    bool IsClassicAi() const { return !computer_player || computer_player->IsClassic(); }
    /** @brief Returns a non-owning pointer to this side's `IComputerPlayer`, or `nullptr`. */
    IComputerPlayer* ComputerPlayer() const { return computer_player.get(); }
    /** @brief Net gold per turn (`income - upkeep_total`). */
    int NetIncome() const { return income - upkeep_total; }
    /** @brief Current capital position (`cap_now_x, cap_now_y` if set, else `cap_x, cap_y`). */
    TilePos CapitalPosition() const {
        return cap_now_x != kNone ? TilePos{cap_now_x, cap_now_y} : TilePos{cap_x, cap_y};
    }
};

}  // namespace w2
