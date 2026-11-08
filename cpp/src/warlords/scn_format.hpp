/**
 * @file
 * @brief Typed record views and layout constants for the binary .SCN format.
 *
 * See docs/formats/scenario.md. Both the scenario loader (`scn.cpp`) and the
 * random world generator (`randommap.cpp`) read and write `.SCN` buffers
 * through these typed views instead of raw byte offsets.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "warlords/bytes.hpp"

namespace w2::scn_format {

constexpr int kScnSize = 12001;       ///< Exact byte length of a Warlords II `.SCN` file.
constexpr int kSideNames = 0;         ///< Offset of the 8x20-byte side name table.
constexpr int kSideNameStride = 20;   ///< Bytes per side name entry.
constexpr int kSideColours = 0xa0;    ///< Offset of the 8 uint16 side fill colours.
constexpr int kSideEdges = 0xb0;      ///< Offset of the 8 uint16 side outline colours.
constexpr int kLevels = 0xc0;         ///< Offset of the 8 uint16 AI difficulty levels.
constexpr int kControllers = 0xd0;    ///< Offset of the 8 uint16 controller flags (0=human).
constexpr int kCards = 0xe0;          ///< Offset of the 8 uint16 AI personality card indices.
constexpr int kEnhanced = 0xf0;       ///< Offset of the 8 uint16 enhanced-AI flags.
constexpr int kCombatCap = 0x112;     ///< Offset of the uint16 maximum stack combat modifier.
constexpr int kObserve = 0x147;       ///< Offset of the 8 uint16 observe-AI flags.
constexpr int kSideRecs = 0x183;      ///< Offset of the 8x20-byte side records (387).
constexpr int kSideRecStride = 20;    ///< Bytes per side record.
constexpr int kFightOrder = 0x60b;    ///< Offset of the 9x29-byte fight order table.
constexpr int kFightRows = 9;         ///< Rows in the fight order table (0..7 sides, 8 neutral).
constexpr int kFightTypes = 29;       ///< Columns per row in the fight order table.
constexpr int kTerrainTable = 0x710;  ///< Offset of the 255-byte tile->terrain lookup table.
constexpr int kTerrainCount = 255;    ///< Entries in the tile->terrain lookup table.
constexpr int kSitesCount = 0x80f;    ///< Offset of the uint16 site count.
constexpr int kSites = 0x811;         ///< Offset of the 40x31-byte site records.
constexpr int kSiteStride = 31;       ///< Bytes per site record.
constexpr int kNSites = 40;           ///< Maximum site records in a `.SCN` file.
constexpr int kItems = 3305;          ///< Offset of the 22x29-byte item records.
constexpr int kItemStride = 29;       ///< Bytes per item record.
constexpr int kNItems = 22;           ///< Number of item records in a `.SCN` file.
constexpr int kMonsters = 3943;       ///< Offset of the 10x16-byte monster name records.
constexpr int kMonsterStride = 16;    ///< Bytes per monster name record.
constexpr int kNMonsters = 10;        ///< Number of monster records in a `.SCN` file.
constexpr int kMonsterStrength = 0x1007;  ///< Offset of the 10 uint16 monster strength words.
constexpr int kDiploScore = 0x10e3;       ///< Offset of the 8 uint16 diplomatic score words.
constexpr int kCitiesCount = 0x157b;      ///< Offset of the uint16 city count (5499).
constexpr int kCities = 0x157d;           ///< Offset of the 65-byte city records (5501).
constexpr int kCityStride = 65;           ///< Bytes per city record (0x41).
constexpr uint8_t kNeutralOwner = 15;     ///< Owner byte value for a neutral city in `.SCN`.
constexpr uint8_t kEmptySlot = 255;       ///< Army type byte value for an unused production slot.

/** @brief Read-only view over a 20-byte side record in a `.SCN` buffer. */
class SideRecordView {
  public:
    /** @brief Constructs a view for side `side_idx` (0..7) in `scn_bytes`. */
    SideRecordView(const std::string& scn_bytes, int side_idx)
            : bytes_(scn_bytes), idx_(side_idx), offset_(kSideRecs + kSideRecStride * side_idx) {}

    /** @brief The side's display name (20-byte C string at offset 0). */
    std::string Name() const {
        return Cstr(bytes_, kSideNames + kSideNameStride * idx_, kSideNameStride);
    }
    /** @brief The side's fill palette colour index. */
    int Colour() const { return U16(bytes_, kSideColours + 2 * idx_); }
    /** @brief The side's outline palette colour index. */
    int Edge() const { return U16(bytes_, kSideEdges + 2 * idx_); }
    /** @brief Starting gold in the treasury. */
    int Gold() const { return U16(bytes_, offset_ + 2); }
    /** @brief Capital column coordinate. */
    int CapX() const { return U16(bytes_, offset_ + 6); }
    /** @brief Capital row coordinate. */
    int CapY() const { return U16(bytes_, offset_ + 8); }
    /** @brief True if controlled by the computer. */
    bool Computer() const { return U16(bytes_, kControllers + 2 * idx_) != 0; }
    /** @brief Computer AI difficulty level. */
    int Level() const { return U16(bytes_, kLevels + 2 * idx_); }
    /** @brief True if Enhanced AI is enabled for this side. */
    bool Enhanced() const { return U16(bytes_, kEnhanced + 2 * idx_) != 0; }
    /** @brief True if human observes this computer side's moves. */
    bool Observe() const { return U16(bytes_, kObserve + 2 * idx_) != 0; }
    /** @brief Diplomatic reputation score. */
    int DiploScore() const { return U16(bytes_, kDiploScore + 2 * idx_); }
    /** @brief AI personality card index. */
    int Card() const { return U16(bytes_, kCards + 2 * idx_); }

  private:
    const std::string& bytes_;  ///< Underlying `.SCN` byte string.
    int idx_;                   ///< Side index (0..7).
    int offset_;                ///< Byte offset of this side's 20-byte record.
};

/** @brief Read-only view over a 65-byte city record in a `.SCN` buffer. */
class CityRecordView {
  public:
    /** @brief Constructs a view for city `city_idx` in `scn_bytes`. */
    CityRecordView(const std::string& scn_bytes, int city_idx)
            : bytes_(scn_bytes), offset_(kCities + kCityStride * city_idx) {}

    /** @brief Column of the city's top-left tile. */
    int X() const { return U16(bytes_, offset_); }
    /** @brief Row of the city's top-left tile. */
    int Y() const { return U16(bytes_, offset_ + 2); }
    /** @brief The city's name (16-byte C string at +4). */
    std::string Name() const { return Cstr(bytes_, offset_ + 4, 16); }
    /** @brief Initial fortification / defence level (+20). */
    int Defence() const { return U8(bytes_, offset_ + 20); }
    /** @brief Owner byte (+21; 0..7 or 15 for neutral). */
    int OwnerByte() const { return U8(bytes_, offset_ + 21); }
    /** @brief Army type id in production slot `slot_idx` (0..3), or 255 if empty (+22). */
    int SlotType(int slot_idx) const { return U8(bytes_, offset_ + 22 + slot_idx); }
    /** @brief Gold income per turn (+42). */
    int Income() const { return U8(bytes_, offset_ + 42); }

  private:
    const std::string& bytes_;  ///< Underlying `.SCN` byte string.
    int offset_;                ///< Byte offset of this city's 65-byte record.
};

/** @brief Read-only view over a 31-byte site record in a `.SCN` buffer. */
class SiteRecordView {
  public:
    /** @brief Constructs a view for site `site_idx` in `scn_bytes`. */
    SiteRecordView(const std::string& scn_bytes, int site_idx)
            : bytes_(scn_bytes), offset_(kSites + kSiteStride * site_idx) {}

    /** @brief Column coordinate of the site. */
    int X() const { return U16(bytes_, offset_); }
    /** @brief Row coordinate of the site. */
    int Y() const { return U16(bytes_, offset_ + 2); }
    /** @brief The site's name (20-byte C string at +4). */
    std::string Name() const { return Cstr(bytes_, offset_ + 4, 20); }
    /** @brief Site kind (+24: 1 = temple, 2 = ruin). */
    int Type() const { return U16(bytes_, offset_ + 24); }

  private:
    const std::string& bytes_;  ///< Underlying `.SCN` byte string.
    int offset_;                ///< Byte offset of this site's 31-byte record.
};

/** @brief Read-only view over a 29-byte item record in a `.SCN` buffer. */
class ItemRecordView {
  public:
    /** @brief Constructs a view for item `item_idx` in `scn_bytes`. */
    ItemRecordView(const std::string& scn_bytes, int item_idx)
            : bytes_(scn_bytes), offset_(kItems + kItemStride * item_idx) {}

    /** @brief The item's name (20-byte C string at +0). */
    std::string Name() const { return Cstr(bytes_, offset_, 20); }
    /** @brief Item bonus category (+20). */
    int Type() const { return U8(bytes_, offset_ + 20); }
    /** @brief Item bonus magnitude (+21). */
    int Value() const { return U8(bytes_, offset_ + 21); }

  private:
    const std::string& bytes_;  ///< Underlying `.SCN` byte string.
    int offset_;                ///< Byte offset of this item's 29-byte record.
};

}  // namespace w2::scn_format
