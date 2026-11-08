/**
 * @file
 * @brief The computer players' character cards: CARDS/K|L|W nnn.CRD and .DSC.
 *
 * See docs/formats/crd.md. A computer side's level picks the deck (K Knight,
 * L Lord, W Warlord) and the scenario's per-side card number picks the card;
 * at game start the card overwrites the level's built-in AI settings.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w2::aicard {

constexpr int kRecord = 0x67;  ///< Size of a .CRD file's record: 103 bytes.

/**
 * @brief The AI settings one character card holds (docs/formats/crd.md).
 *
 * Most fields are copied straight into the side's AI data; the die_* fields
 * are dice sizes that are rolled when the card is applied.
 */
struct Card {
    int groups = 0;                ///< Assault groups the side runs at once.
    int bold = 0;                  ///< Non-zero: may attack a side it is at peace with.
    std::vector<int> fight_order;  ///< The side's fight order, 29 army types.
    int cautious = 0;              ///< Non-zero: careful with neutrals, no quick attacks.
    int rebuild_type = 0;          ///< Army type rebuilding buys below 2000 gold.
    int rebuild_type_rich = 0;     ///< Army type rebuilding buys from 2000 gold.
    int rebuild_limit = 0;         ///< Rebuilding's city limit factor.
    int raze = 0;                  ///< Per-mille chance an assault group razes.
    int sack = 0;                  ///< Per-mille chance an assault group sacks.
    int pillage = 0;               ///< Per-mille chance an assault group pillages.
    int per_city = 0;              ///< Added to all three chances per city owned.
    int bonus_human = 0;           ///< Added to all three chances for a human side.
    int bonus_knight = 0;          ///< Added to all three chances for a Knight.
    int bonus_lord = 0;            ///< Added to all three chances for a Lord.
    int bonus_warlord = 0;         ///< Added to all three chances for a Warlord.
    int die_human = 0;             ///< Dice size of the enemy-pick constant, human.
    int die_knight = 0;            ///< Dice size of the enemy-pick constant, Knight.
    int die_lord = 0;              ///< Dice size of the enemy-pick constant, Lord.
    int die_warlord = 0;           ///< Dice size of the enemy-pick constant, Warlord.
    int poor = 0;                  ///< Added to sack and pillage under 100 gold.
    int early = 0;                 ///< Non-zero: early vengeance on humans.
    int human_share = 0;           ///< Tens of % of cities a human may hold before war.
    int solidarity = 0;            ///< Non-zero: computers stand together.
};

/** @brief The file path of a card: `<data_dir>/CARDS/<letter><nnn>.<ext>`. */
std::filesystem::path Path(const std::filesystem::path& data_dir, int level, int number,
                           std::string_view ext = "CRD");
/** @brief Reads a card, or std::nullopt when the file is missing or short. */
std::optional<Card> Load(const std::filesystem::path& data_dir, int level, int number);
/** @brief A card's name and description lines, from its .DSC, or std::nullopt. */
std::optional<std::pair<std::string, std::vector<std::string>>> Describe(
        const std::filesystem::path& data_dir, int level, int number);
/**
 * @brief A level's deck as the setup screen lists it.
 *
 * The name of each card from 000 up to the first missing .DSC, at most 30:
 * the first 40 bytes of the file, cut at a carriage return.
 */
std::vector<std::string> Deck(const std::filesystem::path& data_dir, int level);
/** @brief How many cards a level has on disk, 000 up to the first missing one. */
int Count(const std::filesystem::path& data_dir, int level);

}  // namespace w2::aicard
