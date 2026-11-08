#include "warlords/scenarios.hpp"

#include <algorithm>

#include "absl/types/span.h"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::scenarios {

namespace {
constexpr size_t kRecord = 84;  ///< The size of one SCENARIO.DAT record.
/// How long a name and a description line may be in New Scenario's list.
constexpr size_t kListNameMax = 20, kListTextMax = 30;
}  // namespace

std::string Info(const Entry& entry) {
    return "name=" + entry.name.substr(0, kListNameMax) + "\n" +
           "text=" + entry.text.substr(0, kListTextMax) + "\n" +
           "cities=" + std::to_string(entry.cities) + "\n" +
           "ruins=" + std::to_string(entry.ruins) + "\n" +
           "players=" + std::to_string(entry.players) + "\n";
}

std::optional<Entry> ParseInfo(std::string_view text, const std::string& dir) {
    Entry entry;
    entry.dir = dir;
    entry.generated = true;
    for (const std::string& raw : SplitLines(text)) {
        std::string line = Trim(raw);
        auto equals_at = line.find('=');
        if (line.empty() || line[0] == '#' || equals_at == std::string::npos) continue;
        std::string key = Trim(line.substr(0, equals_at));
        std::string value = Trim(line.substr(equals_at + 1));
        if (key == "name")
            entry.name = value.substr(0, kListNameMax);
        else if (key == "text")
            entry.text = value.substr(0, kListTextMax);
        else if (key == "cities")
            entry.cities = static_cast<int>(ToInt(value).value_or(0));
        else if (key == "ruins")
            entry.ruins = static_cast<int>(ToInt(value).value_or(0));
        else if (key == "players")
            entry.players = static_cast<int>(ToInt(value).value_or(0));
    }
    if (entry.name.empty()) return std::nullopt;
    return entry;
}

std::vector<Entry> List(const std::filesystem::path& data_dir) {
    std::vector<Entry> out;
    std::vector<std::string> listed;  // the folders already in the list, upper case
    if (auto records = ReadBytes(data_dir / "DATA/SCENARIO.DAT"); records.ok()) {
        absl::Span<const uint8_t> bytes(*records);
        for (size_t i = 0; i < bytes.size() / kRecord; i++) {
            size_t offset = kRecord * i;
            Entry entry{Cstr(bytes, offset, 20),
                        Cstr(bytes, offset + 20, 8),
                        Cstr(bytes, offset + 28, 30),
                        U16(bytes, offset + 76),
                        U16(bytes, offset + 78),
                        U16(bytes, offset + 80),
                        false};
            listed.push_back(Upper(entry.dir));
            out.push_back(entry);
        }
    }
    if (auto lst = ReadFile(data_dir / "DATA/SCENGEN.LST"); lst.ok()) {
        for (const std::string& raw : SplitLines(*lst)) {
            std::string dir = Trim(raw);
            if (dir.empty() || std::find(listed.begin(), listed.end(), Upper(dir)) != listed.end())
                continue;
            std::filesystem::path folder = data_dir / dir;
            auto text = ReadFile(folder / (dir + ".INF"));
            if (!text.ok() || !FileExists(folder / (dir + ".SCN"))) continue;
            if (auto entry = ParseInfo(*text, dir)) {
                listed.push_back(Upper(dir));
                out.push_back(*entry);
            }
        }
    }
    return out;
}

bool Remember(const std::filesystem::path& data_dir, const std::string& dir) {
    std::filesystem::path path = data_dir / "DATA/SCENGEN.LST";
    std::vector<std::string> dirs;
    if (auto lst = ReadFile(path); lst.ok()) {
        for (const std::string& raw : SplitLines(*lst)) {
            std::string trimmed = Trim(raw);
            if (!trimmed.empty()) dirs.push_back(trimmed);
        }
    }
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    std::sort(dirs.begin(), dirs.end());
    std::string text;
    for (const auto& listed_dir : dirs) text += listed_dir + "\n";
    return WriteFile(path, text).ok();
}

}  // namespace w2::scenarios
