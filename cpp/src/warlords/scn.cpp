#include "warlords/scn.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

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
// Where things are in a .SCN file (docs/formats/scenario.md).
const int kSideNames = 0, kSideStride = 20;
// 0xa0 is the side's own colour, 0xb0 the colour it outlines things in.
const int kSideColours = 0xa0, kSideEdges = 0xb0;
const int kSideRecs = 387, kSideRecStride = 20;
const int kLevels = 0xc0, kControllers = 0xd0, kEnhanced = 0xf0;
const int kObserve = 0x147, kCards = 0xe0;
const int kMonsterStrength = 0x1007;
const int kFightOrder = 0x60b, kFightRows = 9, kFightTypes = 29;
const int kCombatCap = 0x112, kDiploScore = 0x10e3;
const int kTerrainTable = 0x710, kTerrainCount = 255;
const int kSitesCount = 0x80f, kSites = 0x811, kSiteStride = 31;
const int kItems = 3305, kItemStride = 29, kNItems = 22;
const int kMonsters = 3943, kMonsterStride = 16, kNMonsters = 10;
const int kCitiesCount = 5499, kCities = 5501, kCityStride = 65;

/// The option words. They are NOT in menu order; see docs/rules.md > Game setup.
const std::pair<const char*, int> kOptions[] = {
    {"neutralCities", 0x11a}, {"diplomacy", 0x11c},      {"quests", 0x11e},
    {"randomTurns", 0x122},   {"hiddenMap", 0x124},      {"intenseCombat", 0x126},
    {"quickStart", 0x128},    {"viewEnemies", 0x12a},    {"militaryAdvisor", 0x12c},
    {"tutorial", 0x12e},      {"viewProduction", 0x132},
};

/** @brief A text file's lines, or std::nullopt if it cannot be read. */
std::optional<std::vector<std::string>> Lines(const std::string& path) {
    auto text = ReadFile(path);
    if (!text) return std::nullopt;
    return SplitLines(*text);
}

/**
 * @brief The whole number a string starts with, after blanks and a sign,
 *        as C's atol reads it; 0 when there is none.
 */
long LeadingInt(const std::string& text) {
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
}  // namespace

std::optional<std::vector<PoolItem>> LoadItemPool(const std::string& path) {
    auto lines = Lines(path);
    if (!lines || lines->empty()) return std::nullopt;
    long count = LeadingInt((*lines)[0]);
    std::vector<PoolItem> pool;
    for (size_t i = 1; i < std::min(lines->size(), static_cast<size_t>(count) + 1); i++) {
        const std::string& line = (*lines)[i];
        PoolItem item;
        std::string name = line.substr(0, std::min<size_t>(20, line.size()));
        for (auto& letter : name)
            if (letter == '_') letter = ' ';
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
static std::map<int, std::array<std::string, 3>> ParseDescriptions(
        const std::vector<std::string>& lines) {
    std::map<int, std::array<std::string, 3>> out;
    for (auto& line : lines) {
        // ^#(\d+)\|(.*)$
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
            } else
                cur += letter;
        }
        std::array<std::string, 3> description;
        for (int part = 0; part < 3; part++)
            description[part] = part < static_cast<int>(parts.size()) ? parts[part] : "";
        out[static_cast<int>(number)] = description;
    }
    return out;
}

std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::string& path) {
    auto lines = Lines(path);
    if (!lines) return {};
    return ParseDescriptions(*lines);
}

/**
 * @brief Parses an .SGN file: a count, then 104-byte records of a position
 *        and two 50-byte lines. A record cut short ends the list.
 */
static std::vector<Sign> ParseSigns(const std::string& bytes) {
    std::vector<Sign> out;
    if (bytes.size() < 2) return out;
    int count = U16(bytes, 0);
    for (int i = 0; i < count; i++) {
        size_t offset = 2 + 104 * i;
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

std::vector<Sign> LoadSigns(const std::string& path) {
    auto bytes = ReadFile(path);
    if (!bytes) return {};
    return ParseSigns(*bytes);
}

std::unique_ptr<Map> LoadFromFiles(const std::string& dir, const std::string& name,
                                   const std::map<std::string, std::string>& files) {
    std::string base = dir + "/" + name;
    auto get_opt = [&](const char* ext) -> std::optional<std::string> {
        auto found = files.find(ext);
        if (found != files.end()) return found->second;
        return ReadFile(base + "." + ext);
    };
    auto get_must = [&](const char* ext) -> std::string {
        auto bytes = get_opt(ext);
        if (!bytes) throw std::runtime_error("cannot open: " + base + "." + ext);
        return *bytes;
    };
    std::string scn_bytes = get_must("SCN");
    if (scn_bytes.size() != 12001)
        throw std::runtime_error(
                Fmt("unexpected .SCN size: %d", static_cast<int>(scn_bytes.size())));
    auto map = std::make_unique<Map>();
    map->name = name;

    for (auto& [option, offset] : kOptions) {
        uint16_t value = U16(scn_bytes, offset);
        if (std::string(option) == "militaryAdvisor" && value == 0) value = 1;
        *map->options.Field(option) = value;
    }

    for (int i = 0; i < 8; i++) {
        int record = kSideRecs + kSideRecStride * i;
        Side& side = map->sides[i];
        side.index = i;
        side.name = Cstr(scn_bytes, kSideNames + kSideStride * i, kSideStride);
        side.colour = U16(scn_bytes, kSideColours + 2 * i);
        side.edge = U16(scn_bytes, kSideEdges + 2 * i);
        side.gold = U16(scn_bytes, record + 2);
        side.cap_x = U16(scn_bytes, record + 6);
        side.cap_y = U16(scn_bytes, record + 8);
        side.computer = U16(scn_bytes, kControllers + 2 * i) != 0;
        side.level = U16(scn_bytes, kLevels + 2 * i);
        side.enhanced = U16(scn_bytes, kEnhanced + 2 * i) != 0;
        side.observe = U16(scn_bytes, kObserve + 2 * i) != 0;
        side.diplo_score = U16(scn_bytes, kDiploScore + 2 * i);
        side.card = U16(scn_bytes, kCards + 2 * i);
    }

    int n_cities = U16(scn_bytes, kCitiesCount);
    map->cities.resize(n_cities);
    map->city_at.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    map->city_tile.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    for (int i = 0; i < n_cities; i++) {
        int record = kCities + kCityStride * i;
        City& city = map->cities[i];
        city.index = i;
        city.x = U16(scn_bytes, record);
        city.y = U16(scn_bytes, record + 2);
        city.name = Cstr(scn_bytes, record + 4, 16);
        city.income = U8(scn_bytes, record + 42);
        for (int slot = 0; slot < 4; slot++) {
            int army_type = U8(scn_bytes, record + 22 + slot);
            // the game removes Navy (type 5) from every city at start
            if (army_type != 255 && army_type != 5) city.produces.push_back(army_type);
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
    for (int row = 0; row < kFightRows; row++)
        for (int type = 0; type < kFightTypes; type++)
            map->fight_order[row][type] = U8(scn_bytes, kFightOrder + row * kFightTypes + type);

    int n_sites = U16(scn_bytes, kSitesCount);
    for (int i = 0; i < n_sites; i++) {
        int record = kSites + kSiteStride * i;
        Site site;
        site.index = i;
        site.x = U16(scn_bytes, record);
        site.y = U16(scn_bytes, record + 2);
        site.name = Cstr(scn_bytes, record + 4, 20);
        site.type = U16(scn_bytes, record + 24);
        map->sites.push_back(site);
    }

    for (int i = 0; i < kNItems; i++) {
        int record = kItems + kItemStride * i;
        std::string item_name = Cstr(scn_bytes, record, 20);
        if (!item_name.empty()) {
            Item item;
            item.index = i;
            item.name = item_name;
            item.type = U8(scn_bytes, record + 20);
            item.value = U8(scn_bytes, record + 21);
            map->items.push_back(item);
        }
    }

    for (int i = 0; i < kNMonsters; i++) {
        std::string monster_name = Cstr(scn_bytes, kMonsters + kMonsterStride * i, 12);
        // keyed by slot: a site's guardian byte indexes this directly
        if (!monster_name.empty())
            map->monsters[i] = Monster{i, monster_name, U16(scn_bytes, kMonsterStrength + 2 * i)};
    }

    // tile index -> terrain type id, the same table the original reads
    for (int i = 0; i < kTerrainCount; i++) map->terrain_type[i] = U8(scn_bytes, kTerrainTable + i);

    // terrain grid + road overlay; bit 15 of a map word marks a crossing
    std::string map_bytes = get_must("MAP");
    map->tiles.resize(static_cast<size_t>(kMapW) * kMapH);
    map->crossing.resize(static_cast<size_t>(kMapW) * kMapH);
    for (int i = 0; i < kMapW * kMapH; i++) {
        int word = U16(map_bytes, static_cast<size_t>(i) * 2);
        map->tiles[i] = word % 0x8000;
        map->crossing[i] = word >= 0x8000;
    }
    map->roads = get_must("RD");
    if (static_cast<int>(map->roads.size()) < kMapW * kMapH)
        map->roads.resize(static_cast<size_t>(kMapW) * kMapH, '\0');

    map->item_pool = LoadItemPool(base + ".ITM");
    if (auto cty = get_opt("CTY")) map->city_text = ParseDescriptions(SplitLines(*cty));
    if (auto spc = get_opt("SPC")) map->site_text = ParseDescriptions(SplitLines(*spc));
    if (auto sgn = get_opt("SGN")) map->signs = ParseSigns(*sgn);
    map->combat_cap = U16(scn_bytes, kCombatCap);
    return map;
}

std::unique_ptr<Map> Load(const std::string& dir, const std::string& name) {
    return LoadFromFiles(dir, name, {});
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
