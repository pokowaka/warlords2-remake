#include "warlords/scenarios.hpp"

#include <algorithm>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::scenarios {

namespace {
constexpr size_t kRecord = 84;  // SCENARIO.DAT
constexpr size_t kListNameMax = 20, kListTextMax = 30;
}  // namespace

std::string Info(const Entry& e) {
    return "name=" + e.name.substr(0, kListNameMax) + "\n" +
           "text=" + e.text.substr(0, kListTextMax) + "\n" + "cities=" + std::to_string(e.cities) +
           "\n" + "ruins=" + std::to_string(e.ruins) + "\n" +
           "players=" + std::to_string(e.players) + "\n";
}

std::optional<Entry> ParseInfo(const std::string& text, const std::string& dir) {
    Entry e;
    e.dir = dir;
    e.generated = true;
    for (const std::string& raw : SplitLines(text)) {
        std::string line = Trim(raw);
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "name")
            e.name = value.substr(0, kListNameMax);
        else if (key == "text")
            e.text = value.substr(0, kListTextMax);
        else if (key == "cities")
            e.cities = static_cast<int>(ToInt(value).value_or(0));
        else if (key == "ruins")
            e.ruins = static_cast<int>(ToInt(value).value_or(0));
        else if (key == "players")
            e.players = static_cast<int>(ToInt(value).value_or(0));
    }
    if (e.name.empty()) return std::nullopt;
    return e;
}

std::vector<Entry> List(const std::string& data_dir) {
    std::vector<Entry> out;
    std::vector<std::string> listed;
    if (auto s = ReadFile(data_dir + "/DATA/SCENARIO.DAT")) {
        for (size_t i = 0; i < s->size() / kRecord; i++) {
            size_t o = kRecord * i;
            Entry e{Cstr(*s, o, 20),
                    Cstr(*s, o + 20, 8),
                    Cstr(*s, o + 28, 30),
                    U16(*s, o + 76),
                    U16(*s, o + 78),
                    U16(*s, o + 80),
                    false};
            listed.push_back(Upper(e.dir));
            out.push_back(e);
        }
    }
    if (auto lst = ReadFile(data_dir + "/DATA/SCENGEN.LST")) {
        for (const std::string& raw : SplitLines(*lst)) {
            std::string dir = Trim(raw);
            if (dir.empty() || std::find(listed.begin(), listed.end(), Upper(dir)) != listed.end())
                continue;
            std::string base = data_dir + "/" + dir + "/" + dir;
            auto text = ReadFile(base + ".INF");
            if (!text || !FileExists(base + ".SCN")) continue;
            if (auto e = ParseInfo(*text, dir)) {
                listed.push_back(Upper(dir));
                out.push_back(*e);
            }
        }
    }
    return out;
}

bool Remember(const std::string& data_dir, const std::string& dir) {
    std::string path = data_dir + "/DATA/SCENGEN.LST";
    std::vector<std::string> dirs;
    if (auto lst = ReadFile(path))
        for (const std::string& raw : SplitLines(*lst))
            if (!Trim(raw).empty()) dirs.push_back(Trim(raw));
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    std::sort(dirs.begin(), dirs.end());
    std::string text;
    for (auto& d : dirs) text += d + "\n";
    return WriteFile(path, text);
}

}  // namespace w2::scenarios
