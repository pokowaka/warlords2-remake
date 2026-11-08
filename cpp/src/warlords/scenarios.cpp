#include "warlords/scenarios.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::scenarios {

namespace {
constexpr size_t RECORD = 84;   // SCENARIO.DAT
constexpr size_t LIST_NAME_MAX = 20, LIST_TEXT_MAX = 30;
}  // namespace

std::string info(const Entry& e) {
  return "name=" + e.name.substr(0, LIST_NAME_MAX) + "\n" + "text=" + e.text.substr(0, LIST_TEXT_MAX) + "\n" +
         "cities=" + std::to_string(e.cities) + "\n" + "ruins=" + std::to_string(e.ruins) + "\n" +
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
    if (key == "name") e.name = value.substr(0, LIST_NAME_MAX);
    else if (key == "text") e.text = value.substr(0, LIST_TEXT_MAX);
    else if (key == "cities") e.cities = (int)toInt(value).value_or(0);
    else if (key == "ruins") e.ruins = (int)toInt(value).value_or(0);
    else if (key == "players") e.players = (int)toInt(value).value_or(0);
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
      Entry e{cstr(*s, o, 20), cstr(*s, o + 20, 8), cstr(*s, o + 28, 30), u16(*s, o + 76), u16(*s, o + 78),
              u16(*s, o + 80), false};
      listed.push_back(upper(e.dir));
      out.push_back(e);
    }
  }
  std::vector<Entry> made;
  std::error_code ec;
  for (auto& item : std::filesystem::directory_iterator(dataDir, ec)) {
    if (!item.is_directory(ec)) continue;
    std::string dir = item.path().filename().string();
    if (std::find(listed.begin(), listed.end(), upper(dir)) != listed.end()) continue;
    std::string base = dataDir + "/" + dir + "/" + dir;
    auto text = readFile(base + ".INF");
    if (!text || !fileExists(base + ".SCN")) continue;
    if (auto e = parseInfo(*text, dir)) made.push_back(*e);
  }
  std::sort(made.begin(), made.end(), [](const Entry& a, const Entry& b) { return a.dir < b.dir; });
  out.insert(out.end(), made.begin(), made.end());
  return out;
}

}  // namespace w2::scenarios
