#include "warlords/scn.hpp"

#include <algorithm>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "util/util.hpp"
#include "warlords/bytes.hpp"
#include "warlords/scn_format.hpp"

namespace w2 {

const std::vector<std::string>& Options::Names() {
    static const std::vector<std::string> kNames = {
        "neutralCities",   "diplomacy",     "quests",        "randomTurns",
        "hiddenMap",       "intenseCombat", "quickStart",    "viewEnemies",
        "militaryAdvisor", "tutorial",      "viewProduction"};
    return kNames;
}

int* Options::Field(const std::string& name) {
    if (name == "neutralCities") return &neutral_cities;
    if (name == "diplomacy") return &diplomacy;
    if (name == "quests") return &quests;
    if (name == "randomTurns") return &random_turns;
    if (name == "hiddenMap") return &hidden_map;
    if (name == "intenseCombat") return &intense_combat;
    if (name == "quickStart") return &quick_start;
    if (name == "viewEnemies") return &view_enemies;
    if (name == "militaryAdvisor") return &military_advisor;
    if (name == "tutorial") return &tutorial;
    if (name == "viewProduction") return &view_production;
    return nullptr;
}

namespace scn {

namespace {
using namespace scn_format;

/// The option words. They are NOT in menu order; see docs/rules.md > Game setup.
const std::pair<const char*, int> kOptions[] = {
    {"neutralCities", 0x11a}, {"diplomacy", 0x11c},      {"quests", 0x11e},
    {"randomTurns", 0x122},   {"hiddenMap", 0x124},      {"intenseCombat", 0x126},
    {"quickStart", 0x128},    {"viewEnemies", 0x12a},    {"militaryAdvisor", 0x12c},
    {"tutorial", 0x12e},      {"viewProduction", 0x132},
};

/** @brief Converts a raw byte buffer into non-empty text lines. */
std::vector<std::string> BytesToLines(absl::Span<const uint8_t> bytes) {
    if (bytes.empty()) return {};
    std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return SplitLines(view);
}

/**
 * @brief The whole number a string starts with, after blanks and a sign,
 *        as C's atol reads it; 0 when there is none.
 */
long LeadingInt(std::string_view text) {
    size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) pos++;
    bool neg = false;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) neg = text[pos++] == '-';
    long value = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos++] - '0');
        any = true;
    }
    return any ? (neg ? -value : value) : 0;
}

std::optional<std::vector<PoolItem>> ParseItemPool(const std::vector<std::string>& lines) {
    if (lines.empty()) return std::nullopt;
    long count = LeadingInt(lines[0]);
    std::vector<PoolItem> pool;
    for (size_t i = 1; i < std::min(lines.size(), static_cast<size_t>(count) + 1); i++) {
        const std::string& line = lines[i];
        PoolItem item;
        std::string name = line.substr(0, std::min<size_t>(20, line.size()));
        for (auto& letter : name) {
            if (letter == '_') letter = ' ';
        }
        item.name = TrimRight(name);
        item.type = line.size() > 21 ? static_cast<int>(LeadingInt(line.substr(21, 1))) : 0;
        item.value = line.size() > 23 ? static_cast<int>(LeadingInt(line.substr(23, 1))) : 0;
        pool.push_back(item);
    }
    return pool;
}

/**
 * @brief Parses the lines of a .CTY or .SPC file: "#number|one|two|three|"
 *        gives entry `number` its three lines; other lines are skipped.
 */
std::map<int, std::array<std::string, 3>> ParseDescriptions(const std::vector<std::string>& lines) {
    std::map<int, std::array<std::string, 3>> out;
    for (const auto& line : lines) {
        if (line.size() < 2 || line[0] != '#') continue;
        size_t pos = 1;
        long number = 0;
        bool any = false;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            number = number * 10 + (line[pos++] - '0');
            any = true;
        }
        if (!any || pos >= line.size() || line[pos] != '|') continue;
        std::string rest = line.substr(pos + 1);
        std::vector<std::string> parts;
        std::string cur;
        for (char letter : rest) {
            if (letter == '|') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += letter;
            }
        }
        std::array<std::string, 3> description;
        for (int part = 0; part < 3; part++) {
            description[part] = part < static_cast<int>(parts.size()) ? parts[part] : "";
        }
        out[static_cast<int>(number)] = description;
    }
    return out;
}

/**
 * @brief Parses an .SGN file: a count, then 104-byte records of a position
 *        and two 50-byte lines. A record cut short ends the list.
 */
std::vector<Sign> ParseSigns(absl::Span<const uint8_t> bytes) {
    std::vector<Sign> out;
    if (bytes.size() < 2) return out;
    int count = U16(bytes, 0);
    for (int i = 0; i < count; i++) {
        size_t offset = 2 + 104UZ * static_cast<size_t>(i);
        if (offset + 104 > bytes.size()) break;
        Sign sign;
        sign.index = i;
        sign.x = U16(bytes, offset);
        sign.y = U16(bytes, offset + 2);
        sign.lines = {Cstr(bytes, offset + 4, 50), Cstr(bytes, offset + 54, 50)};
        out.push_back(sign);
    }
    return out;
}

}  // namespace

std::optional<std::vector<PoolItem>> LoadItemPool(const std::filesystem::path& path) {
    auto text = ReadFile(path);
    if (!text.ok()) return std::nullopt;
    return ParseItemPool(SplitLines(*text));
}

std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::filesystem::path& path) {
    auto text = ReadFile(path);
    if (!text.ok()) return {};
    return ParseDescriptions(SplitLines(*text));
}

std::vector<Sign> LoadSigns(const std::filesystem::path& path) {
    auto bytes = ReadBytes(path);
    if (!bytes.ok()) return {};
    return ParseSigns(*bytes);
}

absl::StatusOr<std::unique_ptr<Map>> LoadFromSource(const IScenarioSource& source,
                                                    std::string_view name) {
    auto scn_file = source.ReadExtension("SCN");
    if (!scn_file.ok()) return scn_file.status();
    absl::Span<const uint8_t> scn_bytes(*scn_file);
    if (scn_bytes.size() != static_cast<size_t>(kScnSize)) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load scenario '%s' from '%s': expected %d bytes in .SCN file, got %zu "
                "bytes; verify the scenario file is an intact Warlords II v1.10 .SCN asset.",
                name, source.DescribeSource(), kScnSize, scn_bytes.size()));
    }
    auto map_file = source.ReadExtension("MAP");
    if (!map_file.ok()) return map_file.status();
    absl::Span<const uint8_t> map_bytes(*map_file);

    auto rd_file = source.ReadExtension("RD");
    if (!rd_file.ok()) return rd_file.status();

    auto map = std::make_unique<Map>();
    map->name = std::string(name);

    for (const auto& [option, offset] : kOptions) {
        uint16_t value = static_cast<uint16_t>(U16(scn_bytes, offset));
        if (std::string_view(option) == "militaryAdvisor" && value == 0) value = 1;
        *map->options.Field(option) = value;
    }

    for (int i = 0; i < 8; i++) {
        SideRecordView rec(scn_bytes, i);
        Side& side = map->sides[i];
        side.index = i;
        side.name = rec.Name();
        side.colour = rec.Colour();
        side.edge = rec.Edge();
        side.gold = rec.Gold();
        side.cap_x = rec.CapX();
        side.cap_y = rec.CapY();
        side.computer = rec.Computer();
        side.level = rec.Level();
        side.enhanced = rec.Enhanced();
        side.observe = rec.Observe();
        side.diplo_score = rec.DiploScore();
        side.card = rec.Card();
    }

    int n_cities = U16(scn_bytes, kCitiesCount);
    map->cities.resize(n_cities);
    map->city_at.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    map->city_tile.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    for (int i = 0; i < n_cities; i++) {
        CityRecordView rec(scn_bytes, i);
        City& city = map->cities[i];
        city.index = i;
        city.x = rec.X();
        city.y = rec.Y();
        city.name = rec.Name();
        city.income = rec.Income();
        for (int slot = 0; slot < 4; slot++) {
            int army_type = rec.SlotType(slot);
            // the game removes Navy (type 5) from every city at start
            if (army_type != kEmptySlot && army_type != 5) city.produces.push_back(army_type);
        }
        city.defence = city.produces.size() >= 3 ? 2 : 1;
        map->city_at[city.y * kMapW + city.x] = &city;
    }
    // A city covers a 2x2 footprint; cityAt keys the top-left, cityTile every tile.
    for (auto& city : map->cities) {
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int tile = (city.y + dy) * kMapW + (city.x + dx);
                if (tile >= 0 && tile < kMapW * kMapH) map->city_tile[tile] = &city;
            }
        }
    }
    // Ownership is derived: at scenario start a side owns only its capital.
    for (auto& side : map->sides) {
        int tile = side.cap_y * kMapW + side.cap_x;
        City* capital = tile >= 0 && tile < kMapW * kMapH ? map->city_at[tile] : nullptr;
        side.in_use = capital != nullptr;
        side.capital = capital;
        if (capital) capital->owner = &side;
    }

    // fight order: 29 bytes per player, row 8 for neutral
    map->fight_order.assign(kFightRows, std::vector<int>(kFightTypes, 0));
    for (int row = 0; row < kFightRows; row++) {
        for (int type = 0; type < kFightTypes; type++) {
            map->fight_order[row][type] = U8(scn_bytes, kFightOrder + row * kFightTypes + type);
        }
    }

    int n_sites = U16(scn_bytes, kSitesCount);
    for (int i = 0; i < n_sites; i++) {
        SiteRecordView rec(scn_bytes, i);
        Site site;
        site.index = i;
        site.x = rec.X();
        site.y = rec.Y();
        site.name = rec.Name();
        site.type = rec.Type();
        map->sites.push_back(site);
    }

    for (int i = 0; i < kNItems; i++) {
        ItemRecordView rec(scn_bytes, i);
        std::string item_name = rec.Name();
        if (!item_name.empty()) {
            Item item;
            item.index = i;
            item.name = item_name;
            item.type = rec.Type();
            item.value = rec.Value();
            map->items.push_back(item);
        }
    }

    for (int i = 0; i < kNMonsters; i++) {
        std::string monster_name = Cstr(scn_bytes, kMonsters + kMonsterStride * i, 12);
        // keyed by slot: a site's guardian byte indexes this directly
        if (!monster_name.empty()) {
            map->monsters[i] = Monster{i, monster_name, U16(scn_bytes, kMonsterStrength + 2 * i)};
        }
    }

    // tile index -> terrain type id, the same table the original reads
    for (int i = 0; i < kTerrainCount; i++) {
        map->terrain_type[i] = U8(scn_bytes, kTerrainTable + i);
    }

    // terrain grid + road overlay; bit 15 of a map word marks a crossing
    map->tiles.resize(static_cast<size_t>(kMapW) * kMapH);
    map->crossing.resize(static_cast<size_t>(kMapW) * kMapH);
    for (int i = 0; i < kMapW * kMapH; i++) {
        int word = U16(map_bytes, static_cast<size_t>(i) * 2);
        map->tiles[i] = word % 0x8000;
        map->crossing[i] = word >= 0x8000;
    }
    map->roads = std::move(*rd_file);
    if (static_cast<int>(map->roads.size()) < kMapW * kMapH) {
        map->roads.resize(static_cast<size_t>(kMapW) * kMapH, 0);
    }

    if (auto itm_bytes = source.ReadExtension("ITM"); itm_bytes.ok()) {
        map->item_pool = ParseItemPool(BytesToLines(*itm_bytes));
    }
    if (auto cty_bytes = source.ReadExtension("CTY"); cty_bytes.ok()) {
        map->city_text = ParseDescriptions(BytesToLines(*cty_bytes));
    }
    if (auto spc_bytes = source.ReadExtension("SPC"); spc_bytes.ok()) {
        map->site_text = ParseDescriptions(BytesToLines(*spc_bytes));
    }
    if (auto sgn_bytes = source.ReadExtension("SGN"); sgn_bytes.ok()) {
        map->signs = ParseSigns(*sgn_bytes);
    }
    map->combat_cap = U16(scn_bytes, kCombatCap);
    return map;
}

absl::StatusOr<std::unique_ptr<Map>> Load(const std::filesystem::path& dir, std::string_view name) {
    DiskScenarioSource source(dir, std::string(name));
    return LoadFromSource(source, name);
}

absl::StatusOr<std::unique_ptr<Map>> LoadFromFiles(const std::filesystem::path& dir,
                                                   std::string_view name,
                                                   const ScenarioFiles& files) {
    MemoryScenarioSource source(files, dir, std::string(name));
    auto loaded = LoadFromSource(source, name);
    if (!loaded.ok()) return loaded.status();
    (*loaded)->source_files = source.Files();
    return loaded;
}

int CityTileBase(const City& city) {
    if (city.razed) return 0xa0 + 2 * (city.razed_by == kNone ? 0 : city.razed_by);
    int owner = city.owner_index;
    if (owner == kNone) return 96;  // the original passes owner -1
    if (owner < 6) return 0x62 + 2 * owner;
    return 0x80 + 2 * (owner - 6);
}

void SetCityTiles(Map& map, const City& city) {
    int base_tile = CityTileBase(city);
    int top_left = city.y * kMapW + city.x;
    map.tiles[top_left] = base_tile;
    map.tiles[top_left + 1] = base_tile + 1;
    map.tiles[top_left + kMapW] = base_tile + 16;
    map.tiles[top_left + kMapW + 1] = base_tile + 17;
}

void RefreshCityTiles(Map& map) {
    for (auto& city : map.cities) SetCityTiles(map, city);
}

}  // namespace scn
}  // namespace w2
