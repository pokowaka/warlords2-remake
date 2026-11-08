#include "warlords/scenarios.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::scenarios {

namespace {
constexpr size_t RECORD = 84;  // SCENARIO.DAT
constexpr size_t LIST_NAME_MAX = 20, LIST_TEXT_MAX = 30;
}  // namespace

std::string info(const Entry& e) {
    return "name=" + e.name.substr(0, LIST_NAME_MAX) + "\n" +
           "text=" + e.text.substr(0, LIST_TEXT_MAX) + "\n" + "cities=" + std::to_string(e.cities) +
           "\n" + "ruins=" + std::to_string(e.ruins) + "\n" +
           "players=" + std::to_string(e.players) + "\n";
}

std::optional<Entry> parseInfo(const std::string& text, const std::string& dir) {
    Entry e;
    e.dir = dir;
    e.generated = true;
    for (const std::string& raw : splitLines(text)) {
        std::string line = trim(raw);
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "name")
            e.name = value.substr(0, LIST_NAME_MAX);
        else if (key == "text")
            e.text = value.substr(0, LIST_TEXT_MAX);
        else if (key == "cities")
            e.cities = static_cast<int>(toInt(value).value_or(0));
        else if (key == "ruins")
            e.ruins = static_cast<int>(toInt(value).value_or(0));
        else if (key == "players")
            e.players = static_cast<int>(toInt(value).value_or(0));
    }
    if (e.name.empty()) return std::nullopt;
    return e;
}

std::vector<Entry> list(const std::string& dataDir) {
    std::vector<Entry> out;
    std::vector<std::string> listed;
    if (auto s = readFile(dataDir + "/DATA/SCENARIO.DAT")) {
        for (size_t i = 0; i < s->size() / RECORD; i++) {
            size_t o = RECORD * i;
            Entry e{cstr(*s, o, 20),
                    cstr(*s, o + 20, 8),
                    cstr(*s, o + 28, 30),
                    u16(*s, o + 76),
                    u16(*s, o + 78),
                    u16(*s, o + 80),
                    false};
            listed.push_back(upper(e.dir));
            out.push_back(e);
        }
    }
    if (auto lst = readFile(dataDir + "/DATA/SCENGEN.LST")) {
        for (const std::string& raw : splitLines(*lst)) {
            std::string dir = trim(raw);
            if (dir.empty() || std::find(listed.begin(), listed.end(), upper(dir)) != listed.end())
                continue;
            std::string base = dataDir + "/" + dir + "/" + dir;
            auto text = readFile(base + ".INF");
            if (!text || !fileExists(base + ".SCN")) continue;
            if (auto e = parseInfo(*text, dir)) {
                listed.push_back(upper(dir));
                out.push_back(*e);
            }
        }
    }
    return out;
}

bool remember(const std::string& dataDir, const std::string& dir) {
    std::string path = dataDir + "/DATA/SCENGEN.LST";
    std::vector<std::string> dirs;
    if (auto lst = readFile(path))
        for (const std::string& raw : splitLines(*lst))
            if (!trim(raw).empty()) dirs.push_back(trim(raw));
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    std::sort(dirs.begin(), dirs.end());
    std::string text;
    for (auto& d : dirs) text += d + "\n";
    return writeFile(path, text);
}

}  // namespace w2::scenarios
