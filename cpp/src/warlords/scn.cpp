#include "warlords/scn.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2 {

const std::vector<std::string>& Options::Names() {
    static const std::vector<std::string> kN = {"neutralCities", "diplomacy",     "quests",
                                                "randomTurns",   "hiddenMap",     "intenseCombat",
                                                "quickStart",    "viewEnemies",   "militaryAdvisor",
                                                "tutorial",      "viewProduction"};
    return kN;
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
    auto t = ReadFile(path);
    if (!t) return std::nullopt;
    return SplitLines(*t);
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

std::optional<std::vector<PoolItem>> LoadItemPool(const std::string& path) {
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
        p.name = TrimRight(nm);
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

std::map<int, std::array<std::string, 3>> LoadDescriptions(const std::string& path) {
    auto ls = Lines(path);
    if (!ls) return {};
    return ParseDescriptions(*ls);
}

static std::vector<Sign> ParseSigns(const std::string& s) {
    std::vector<Sign> out;
    if (s.size() < 2) return out;
    int n = U16(s, 0);
    for (int i = 0; i < n; i++) {
        size_t o = 2 + 104 * i;
        if (o + 104 > s.size()) break;
        Sign sg;
        sg.index = i;
        sg.x = U16(s, o);
        sg.y = U16(s, o + 2);
        sg.lines = {Cstr(s, o + 4, 50), Cstr(s, o + 54, 50)};
        out.push_back(sg);
    }
    return out;
}

std::vector<Sign> LoadSigns(const std::string& path) {
    auto s = ReadFile(path);
    if (!s) return {};
    return ParseSigns(*s);
}

std::unique_ptr<Map> LoadFromFiles(const std::string& dir, const std::string& name,
                                   const std::map<std::string, std::string>& files) {
    std::string base = dir + "/" + name;
    auto get_opt = [&](const char* ext) -> std::optional<std::string> {
        auto it = files.find(ext);
        if (it != files.end()) return it->second;
        return ReadFile(base + "." + ext);
    };
    auto get_must = [&](const char* ext) -> std::string {
        auto v = get_opt(ext);
        if (!v) throw std::runtime_error("cannot open: " + base + "." + ext);
        return *v;
    };
    std::string s = get_must("SCN");
    if (s.size() != 12001)
        throw std::runtime_error(Fmt("unexpected .SCN size: %d", static_cast<int>(s.size())));
    auto m = std::make_unique<Map>();
    m->name = name;

    for (auto& [k, off] : kOptions) {
        uint16_t val = U16(s, off);
        if (std::string(k) == "militaryAdvisor" && val == 0) val = 1;
        *m->options.Field(k) = val;
    }

    for (int i = 0; i < 8; i++) {
        int o = kSideRecs + kSideRecStride * i;
        Side& sd = m->sides[i];
        sd.index = i;
        sd.name = Cstr(s, kSideNames + kSideStride * i, kSideStride);
        sd.colour = U16(s, kSideColours + 2 * i);
        sd.edge = U16(s, kSideEdges + 2 * i);
        sd.gold = U16(s, o + 2);
        sd.cap_x = U16(s, o + 6);
        sd.cap_y = U16(s, o + 8);
        sd.computer = U16(s, kControllers + 2 * i) != 0;
        sd.level = U16(s, kLevels + 2 * i);
        sd.enhanced = U16(s, kEnhanced + 2 * i) != 0;
        sd.observe = U16(s, kObserve + 2 * i) != 0;
        sd.diplo_score = U16(s, kDiploScore + 2 * i);
        sd.card = U16(s, kCards + 2 * i);
    }

    int n_cities = U16(s, kCitiesCount);
    m->cities.resize(n_cities);
    m->city_at.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    m->city_tile.assign(static_cast<size_t>(kMapW) * kMapH, nullptr);
    for (int i = 0; i < n_cities; i++) {
        int o = kCities + kCityStride * i;
        City& c = m->cities[i];
        c.index = i;
        c.x = U16(s, o);
        c.y = U16(s, o + 2);
        c.name = Cstr(s, o + 4, 16);
        c.income = U8(s, o + 42);
        for (int k = 0; k < 4; k++) {
            int t = U8(s, o + 22 + k);
            // the game removes Navy (type 5) from every city at start
            if (t != 255 && t != 5) c.produces.push_back(t);
        }
        c.defence = c.produces.size() >= 3 ? 2 : 1;
        m->city_at[c.y * kMapW + c.x] = &c;
    }
    // A city covers a 2x2 footprint; cityAt keys the top-left, cityTile every tile.
    for (auto& c : m->cities) {
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                int k = (c.y + dy) * kMapW + (c.x + dx);
                if (k >= 0 && k < kMapW * kMapH) m->city_tile[k] = &c;
            }
        }
    }
    // Ownership is derived: at scenario start a side owns only its capital.
    for (auto& sd : m->sides) {
        int k = sd.cap_y * kMapW + sd.cap_x;
        City* c = k >= 0 && k < kMapW * kMapH ? m->city_at[k] : nullptr;
        sd.in_use = c != nullptr;
        sd.capital = c;
        if (c) c->owner = &sd;
    }

    // fight order: 29 bytes per player, row 8 for neutral
    m->fight_order.assign(kFightRows, std::vector<int>(kFightTypes, 0));
    for (int row = 0; row < kFightRows; row++)
        for (int t = 0; t < kFightTypes; t++)
            m->fight_order[row][t] = U8(s, kFightOrder + row * kFightTypes + t);

    int n_sites = U16(s, kSitesCount);
    for (int i = 0; i < n_sites; i++) {
        int o = kSites + kSiteStride * i;
        Site st;
        st.index = i;
        st.x = U16(s, o);
        st.y = U16(s, o + 2);
        st.name = Cstr(s, o + 4, 20);
        st.type = U16(s, o + 24);
        m->sites.push_back(st);
    }

    for (int i = 0; i < kNItems; i++) {
        int o = kItems + kItemStride * i;
        std::string nm = Cstr(s, o, 20);
        if (!nm.empty()) {
            Item it;
            it.index = i;
            it.name = nm;
            it.type = U8(s, o + 20);
            it.value = U8(s, o + 21);
            m->items.push_back(it);
        }
    }

    for (int i = 0; i < kNMonsters; i++) {
        std::string nm = Cstr(s, kMonsters + kMonsterStride * i, 12);
        // keyed by slot: a site's guardian byte indexes this directly
        if (!nm.empty()) m->monsters[i] = Monster{i, nm, U16(s, kMonsterStrength + 2 * i)};
    }

    // tile index -> terrain type id, the same table WARLORD2.EXE reads
    for (int i = 0; i < kTerrainCount; i++) m->terrain_type[i] = U8(s, kTerrainTable + i);

    // terrain grid + road overlay; bit 15 of a map word marks a crossing
    std::string mp = get_must("MAP");
    m->tiles.resize(static_cast<size_t>(kMapW) * kMapH);
    m->crossing.resize(static_cast<size_t>(kMapW) * kMapH);
    for (int i = 0; i < kMapW * kMapH; i++) {
        int w = U16(mp, static_cast<size_t>(i) * 2);
        m->tiles[i] = w % 0x8000;
        m->crossing[i] = w >= 0x8000;
    }
    m->roads = get_must("RD");
    if (static_cast<int>(m->roads.size()) < kMapW * kMapH)
        m->roads.resize(static_cast<size_t>(kMapW) * kMapH, '\0');

    m->item_pool = LoadItemPool(base + ".ITM");
    if (auto cty = get_opt("CTY")) m->city_text = ParseDescriptions(SplitLines(*cty));
    if (auto spc = get_opt("SPC")) m->site_text = ParseDescriptions(SplitLines(*spc));
    if (auto sgn = get_opt("SGN")) m->signs = ParseSigns(*sgn);
    m->combat_cap = U16(s, kCombatCap);
    return m;
}

std::unique_ptr<Map> Load(const std::string& dir, const std::string& name) {
    return LoadFromFiles(dir, name, {});
}

int CityTileBase(const City& city) {
    if (city.razed) return 0xa0 + 2 * (city.razed_by == kNone ? 0 : city.razed_by);
    int o = city.owner_index;
    if (o == kNone) return 96;  // the original passes owner -1
    if (o < 6) return 0x62 + 2 * o;
    return 0x80 + 2 * (o - 6);
}

void SetCityTiles(Map& map, const City& city) {
    int t = CityTileBase(city);
    int i = city.y * kMapW + city.x;
    map.tiles[i] = t;
    map.tiles[i + 1] = t + 1;
    map.tiles[i + kMapW] = t + 16;
    map.tiles[i + kMapW + 1] = t + 17;
}

void RefreshCityTiles(Map& map) {
    for (auto& c : map.cities) SetCityTiles(map, c);
}

}  // namespace scn
}  // namespace w2
