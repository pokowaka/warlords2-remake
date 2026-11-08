#include "warlords/scn.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2 {

const std::vector<std::string>& Options::names() {
    static const std::vector<std::string> kN = {"neutralCities", "diplomacy",     "quests",
                                                "randomTurns",   "hiddenMap",     "intenseCombat",
                                                "quickStart",    "viewEnemies",   "militaryAdvisor",
                                                "tutorial",      "viewProduction"};
    return kN;
}

int* Options::field(const std::string& name) {
    if (name == "neutralCities") return &neutralCities;
    if (name == "diplomacy") return &diplomacy;
    if (name == "quests") return &quests;
    if (name == "randomTurns") return &randomTurns;
    if (name == "hiddenMap") return &hiddenMap;
    if (name == "intenseCombat") return &intenseCombat;
    if (name == "quickStart") return &quickStart;
    if (name == "viewEnemies") return &viewEnemies;
    if (name == "militaryAdvisor") return &militaryAdvisor;
    if (name == "tutorial") return &tutorial;
    if (name == "viewProduction") return &viewProduction;
    return nullptr;
}

namespace scn {

namespace {
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

// Option words. They are NOT in menu order; see docs/rules.md > Game setup.
const std::pair<const char*, int> kOptions[] = {
    {"neutralCities", 0x11a}, {"diplomacy", 0x11c},      {"quests", 0x11e},
    {"randomTurns", 0x122},   {"hiddenMap", 0x124},      {"intenseCombat", 0x126},
    {"quickStart", 0x128},    {"viewEnemies", 0x12a},    {"militaryAdvisor", 0x12c},
    {"tutorial", 0x12e},      {"viewProduction", 0x132},
};

std::optional<std::vector<std::string>> Lines(const std::string& path) {
    auto t = readFile(path);
    if (!t) return std::nullopt;
    return splitLines(*t);
}

long LeadingInt(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    long v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i++] - '0');
        any = true;
    }
    return any ? (neg ? -v : v) : 0;
}
}  // namespace

std::optional<std::vector<PoolItem>> loadItemPool(const std::string& path) {
    auto ls = Lines(path);
    if (!ls || ls->empty()) return std::nullopt;
    long count = LeadingInt((*ls)[0]);
    std::vector<PoolItem> pool;
    for (size_t i = 1; i < std::min(ls->size(), static_cast<size_t>(count) + 1); i++) {
        const std::string& line = (*ls)[i];
        PoolItem p;
        std::string nm = line.substr(0, std::min<size_t>(20, line.size()));
        for (auto& c : nm)
            if (c == '_') c = ' ';
        p.name = trimRight(nm);
        p.type = line.size() > 21 ? static_cast<int>(LeadingInt(line.substr(21, 1))) : 0;
        p.value = line.size() > 23 ? static_cast<int>(LeadingInt(line.substr(23, 1))) : 0;
        pool.push_back(p);
    }
    return pool;
}

static std::map<int, std::array<std::string, 3>> ParseDescriptions(
        const std::vector<std::string>& ls) {
    std::map<int, std::array<std::string, 3>> out;
    for (auto& line : ls) {
        // ^#(\d+)\|(.*)$
        if (line.size() < 2 || line[0] != '#') continue;
        size_t i = 1;
        long n = 0;
        bool any = false;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            n = n * 10 + (line[i++] - '0');
            any = true;
        }
        if (!any || i >= line.size() || line[i] != '|') continue;
        std::string rest = line.substr(i + 1);
        std::vector<std::string> parts;
        std::string cur;
        for (char c : rest) {
            if (c == '|') {
                parts.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        std::array<std::string, 3> d;
        for (int k = 0; k < 3; k++) d[k] = k < static_cast<int>(parts.size()) ? parts[k] : "";
        out[static_cast<int>(n)] = d;
    }
    return out;
}

std::map<int, std::array<std::string, 3>> loadDescriptions(const std::string& path) {
    auto ls = Lines(path);
    if (!ls) return {};
    return ParseDescriptions(*ls);
}

static std::vector<Sign> ParseSigns(const std::string& s) {
    std::vector<Sign> out;
    if (s.size() < 2) return out;
    int n = u16(s, 0);
    for (int i = 0; i < n; i++) {
        size_t o = 2 + 104 * i;
        if (o + 104 > s.size()) break;
        Sign sg;
        sg.index = i;
        sg.x = u16(s, o);
        sg.y = u16(s, o + 2);
        sg.lines = {cstr(s, o + 4, 50), cstr(s, o + 54, 50)};
        out.push_back(sg);
    }
    return out;
}

std::vector<Sign> loadSigns(const std::string& path) {
    auto s = readFile(path);
    if (!s) return {};
    return ParseSigns(*s);
}

std::unique_ptr<Map> loadFromFiles(const std::string& dir, const std::string& name,
                                   const std::map<std::string, std::string>& files) {
    std::string base = dir + "/" + name;
    auto get_opt = [&](const char* ext) -> std::optional<std::string> {
        auto it = files.find(ext);
        if (it != files.end()) return it->second;
        return readFile(base + "." + ext);
    };
    auto get_must = [&](const char* ext) -> std::string {
        auto v = get_opt(ext);
        if (!v) throw std::runtime_error("cannot open: " + base + "." + ext);
        return *v;
    };
    std::string s = get_must("SCN");
    if (s.size() != 12001)
        throw std::runtime_error(fmt("unexpected .SCN size: %d", static_cast<int>(s.size())));
    auto m = std::make_unique<Map>();
    m->name = name;

    for (auto& [k, off] : kOptions) {
        uint16_t val = u16(s, off);
        if (std::string(k) == "militaryAdvisor" && val == 0) val = 1;
        *m->options.field(k) = val;
    }

    for (int i = 0; i < 8; i++) {
        int o = kSideRecs + kSideRecStride * i;
        Side& sd = m->sides[i];
        sd.index = i;
        sd.name = cstr(s, kSideNames + kSideStride * i, kSideStride);
        sd.colour = u16(s, kSideColours + 2 * i);
        sd.edge = u16(s, kSideEdges + 2 * i);
        sd.gold = u16(s, o + 2);
        sd.capX = u16(s, o + 6);
        sd.capY = u16(s, o + 8);
        sd.computer = u16(s, kControllers + 2 * i) != 0;
        sd.level = u16(s, kLevels + 2 * i);
        sd.enhanced = u16(s, kEnhanced + 2 * i) != 0;
        sd.observe = u16(s, kObserve + 2 * i) != 0;
        sd.diploScore = u16(s, kDiploScore + 2 * i);
        sd.card = u16(s, kCards + 2 * i);
    }

    int n_cities = u16(s, kCitiesCount);
    m->cities.resize(n_cities);
    m->cityAt.assign(static_cast<size_t>(MAP_W) * MAP_H, nullptr);
    m->cityTile.assign(static_cast<size_t>(MAP_W) * MAP_H, nullptr);
    for (int i = 0; i < n_cities; i++) {
        int o = kCities + kCityStride * i;
        City& c = m->cities[i];
        c.index = i;
        c.x = u16(s, o);
        c.y = u16(s, o + 2);
        c.name = cstr(s, o + 4, 16);
        c.income = u8(s, o + 42);
        for (int k = 0; k < 4; k++) {
            int t = u8(s, o + 22 + k);
            // the game removes Navy (type 5) from every city at start
            if (t != 255 && t != 5) c.produces.push_back(t);
        }
        c.defence = c.produces.size() >= 3 ? 2 : 1;
        m->cityAt[c.y * MAP_W + c.x] = &c;
    }
    // A city covers a 2x2 footprint; cityAt keys the top-left, cityTile every tile.
    for (auto& c : m->cities) {
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int k = (c.y + dy) * MAP_W + (c.x + dx);
                if (k >= 0 && k < MAP_W * MAP_H) m->cityTile[k] = &c;
            }
        }
    }
    // Ownership is derived: at scenario start a side owns only its capital.
    for (auto& sd : m->sides) {
        int k = sd.capY * MAP_W + sd.capX;
        City* c = k >= 0 && k < MAP_W * MAP_H ? m->cityAt[k] : nullptr;
        sd.inUse = c != nullptr;
        sd.capital = c;
        if (c) c->owner = &sd;
    }

    // fight order: 29 bytes per player, row 8 for neutral
    m->fightOrder.assign(kFightRows, std::vector<int>(kFightTypes, 0));
    for (int row = 0; row < kFightRows; row++)
        for (int t = 0; t < kFightTypes; t++)
            m->fightOrder[row][t] = u8(s, kFightOrder + row * kFightTypes + t);

    int n_sites = u16(s, kSitesCount);
    for (int i = 0; i < n_sites; i++) {
        int o = kSites + kSiteStride * i;
        Site st;
        st.index = i;
        st.x = u16(s, o);
        st.y = u16(s, o + 2);
        st.name = cstr(s, o + 4, 20);
        st.type = u16(s, o + 24);
        m->sites.push_back(st);
    }

    for (int i = 0; i < kNItems; i++) {
        int o = kItems + kItemStride * i;
        std::string nm = cstr(s, o, 20);
        if (!nm.empty()) {
            Item it;
            it.index = i;
            it.name = nm;
            it.type = u8(s, o + 20);
            it.value = u8(s, o + 21);
            m->items.push_back(it);
        }
    }

    for (int i = 0; i < kNMonsters; i++) {
        std::string nm = cstr(s, kMonsters + kMonsterStride * i, 12);
        // keyed by slot: a site's guardian byte indexes this directly
        if (!nm.empty()) m->monsters[i] = Monster{i, nm, u16(s, kMonsterStrength + 2 * i)};
    }

    // tile index -> terrain type id, the same table WARLORD2.EXE reads
    for (int i = 0; i < kTerrainCount; i++) m->terrainType[i] = u8(s, kTerrainTable + i);

    // terrain grid + road overlay; bit 15 of a map word marks a crossing
    std::string mp = get_must("MAP");
    m->tiles.resize(static_cast<size_t>(MAP_W) * MAP_H);
    m->crossing.resize(static_cast<size_t>(MAP_W) * MAP_H);
    for (int i = 0; i < MAP_W * MAP_H; i++) {
        int w = u16(mp, static_cast<size_t>(i) * 2);
        m->tiles[i] = w % 0x8000;
        m->crossing[i] = w >= 0x8000;
    }
    m->roads = get_must("RD");
    if (static_cast<int>(m->roads.size()) < MAP_W * MAP_H)
        m->roads.resize(static_cast<size_t>(MAP_W) * MAP_H, '\0');

    m->itemPool = loadItemPool(base + ".ITM");
    if (auto cty = get_opt("CTY")) m->cityText = ParseDescriptions(splitLines(*cty));
    if (auto spc = get_opt("SPC")) m->siteText = ParseDescriptions(splitLines(*spc));
    if (auto sgn = get_opt("SGN")) m->signs = ParseSigns(*sgn);
    m->combatCap = u16(s, kCombatCap);
    return m;
}

std::unique_ptr<Map> load(const std::string& dir, const std::string& name) {
    return loadFromFiles(dir, name, {});
}

int cityTileBase(const City& city) {
    if (city.razed) return 0xa0 + 2 * (city.razedBy == NONE ? 0 : city.razedBy);
    int o = city.ownerIndex;
    if (o == NONE) return 96;  // the original passes owner -1
    if (o < 6) return 0x62 + 2 * o;
    return 0x80 + 2 * (o - 6);
}

void setCityTiles(Map& map, const City& city) {
    int t = cityTileBase(city);
    int i = city.y * MAP_W + city.x;
    map.tiles[i] = t;
    map.tiles[i + 1] = t + 1;
    map.tiles[i + MAP_W] = t + 16;
    map.tiles[i + MAP_W + 1] = t + 17;
}

void refreshCityTiles(Map& map) {
    for (auto& c : map.cities) setCityTiles(map, c);
}

}  // namespace scn
}  // namespace w2
