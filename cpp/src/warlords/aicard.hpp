// The computer players' character cards: CARDS/K|L|W nnn.CRD and .DSC.
//
// docs/formats/crd.md. A computer side's level picks the deck (K Knight,
// L Lord, W Warlord) and .SCN 0x00e0 + 2*side the card; at game start the
// card overwrites the level's built-in AI settings (59bf:0d7b).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace w2::aicard {

constexpr int kRecord = 0x67;  // 103 bytes

struct Card {
    int groups = 0, bold = 0;
    std::vector<int> fight_order;
    int cautious = 0, rebuild_type = 0, rebuild_type_rich = 0, rebuild_limit = 0;
    int raze = 0, sack = 0, pillage = 0, per_city = 0;
    int bonus_human = 0, bonus_knight = 0, bonus_lord = 0, bonus_warlord = 0;
    int die_human = 0, die_knight = 0, die_lord = 0, die_warlord = 0;
    int poor = 0, early = 0, human_share = 0, solidarity = 0;
};

/** The file name of a card: CARDS\%c%03d.%s. */
std::string Path(const std::string& data_dir, int level, int number,
                 const std::string& ext = "CRD");
/** Read a card, or none when the file is missing or short. */
std::optional<Card> Load(const std::string& data_dir, int level, int number);
/** A card's name and description lines, from its .DSC, or none. */
std::optional<std::pair<std::string, std::vector<std::string>>> Describe(
        const std::string& data_dir, int level, int number);
/** A level's deck as the setup screen lists it (list_carried_items mode 7,
 *  796c:03d9): the name of each card from 000 up to the first missing .DSC,
 *  at most 30 -- the first 40 bytes of the file, cut at a carriage return. */
std::vector<std::string> Deck(const std::string& data_dir, int level);
/** How many cards a level has on disk, 000 up to the first missing one. */
int Count(const std::string& data_dir, int level);

}  // namespace w2::aicard
