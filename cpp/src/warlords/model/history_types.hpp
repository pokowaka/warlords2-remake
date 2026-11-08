/**
 * @file
 * @brief History and end-of-game domain model structs (DeedType, Deed, HistoryRecord, Fallen,
 * Ending).
 */
#pragma once

#include <array>
#include <string>
#include <vector>

namespace w2 {

struct Side;

/**
 * @brief What a deed was: one of the events History plays back.
 *
 * STRING.DAT group 94, docs/formats/history.md. The order is also each
 * deed's rank: a side keeps only its two most important deeds of a round,
 * and a lower value counts for more. The comments say what a deed's two
 * values hold.
 */
enum class DeedType : int {
    kHeroEmerges = 0,     ///< v1: the city the hero appeared in.
    kHeroKilled = 1,      ///< v1: the city, or history::kKilledInBattle / kKilledSearching.
    kQuestCompleted = 2,  ///< A hero completed its quest.
    kQuestReceived = 3,   ///< A hero was given a quest.
    kSideVanquished = 4,  ///< v1: the side.
    kCityWon = 5,         ///< v1: the side, or history::kWonByName; v2: the city.
    kFound = 6,           ///< v1: the item, or history::kFoundAllies / kFoundSage / kFoundGold.
    kSideVictorious = 7,  ///< v1: the side.
    kTreachery = 8,       ///< v1: the side that attacked at peace, v2: its victim.
    kAtWar = 9,           ///< v1, v2: the two sides.
    kAtPeace = 10,        ///< v1, v2: the two sides.
};

/**
 * @brief A deed of the round, for the history.
 *
 * Who did it, what, two values whose meaning depends on the type, and the
 * hero's or the side's name.
 */
struct Deed {
    int side = 0;                            ///< The side whose deed it is.
    DeedType type = DeedType::kHeroEmerges;  ///< What it was.
    int v1 = 0;                              ///< The first value (see DeedType).
    int v2 = 0;                              ///< The second value (see DeedType).
    std::string name;                        ///< The hero's or the side's name.
};

/** @brief A round's record: what History plays back. */
struct HistoryRecord {
    std::array<int, 8> gold{};    ///< Each side's gold.
    std::array<int, 8> score{};   ///< Each side's score.
    std::array<int, 8> cities{};  ///< Each side's city count.
    std::vector<int> owners;      ///< Each city's owner: 8 neutral, 0xff razed.
    std::vector<Deed> events;     ///< The round's deeds.
};

/**
 * @brief A side put out at the round's end, and how it is told.
 *
 * A line of STRING.DAT group 11, in a box or in the status bar.
 */
struct Fallen {
    Side* side = nullptr;  ///< The side put out.
    int line = 0;          ///< The line of STRING.DAT group 11 that tells it.
    bool boxed = false;    ///< Told in a box rather than in the status bar.
};

/** @brief How the game ended, or is ending, as the round's end finds it. */
struct Ending {
    bool over = false;           ///< The game is over.
    bool won = false;            ///< A side has won.
    bool triumph = false;        ///< A triumph: the winner holds every city.
    bool surrender = false;      ///< The others offer to surrender.
    bool no_humans = false;      ///< No human is left.
    bool shown = false;          ///< The front end has told it.
    bool told = false;           ///< ... and the fallen and the last human's fall.
    Side* winner = nullptr;      ///< The side that won.
    std::string message;         ///< What to say.
    std::vector<Fallen> fallen;  ///< The sides put out this round.
};

}  // namespace w2
