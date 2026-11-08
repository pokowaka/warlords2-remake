#include "warlords/aicard.hpp"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::aicard {

std::string Path(const std::string& data_dir, int level, int number, const std::string& ext) {
    static const char* letters[3] = {"K", "L", "W"};  // one deck per level
    const char* letter = level >= 0 && level < 3 ? letters[level] : "W";
    return Fmt("%s/CARDS/%s%03d.%s", data_dir.c_str(), letter, number, ext.c_str());
}

std::optional<Card> Load(const std::string& data_dir, int level, int number) {
    auto file = ReadFile(Path(data_dir, level, number, "CRD"));
    if (!file || file->size() < static_cast<size_t>(kRecord)) return std::nullopt;
    const std::string& bytes = *file;
    Card card;
    for (int type = 0; type <= 28; type++) card.fight_order.push_back(U8(bytes, 0x0e + type));
    card.groups = I16(bytes, 0x08);
    card.bold = I16(bytes, 0x0a);
    card.cautious = I16(bytes, 0x2b);
    card.rebuild_type = I16(bytes, 0x2d);
    card.rebuild_type_rich = I16(bytes, 0x2f);
    card.rebuild_limit = I16(bytes, 0x31);
    card.raze = I16(bytes, 0x45);
    card.sack = I16(bytes, 0x47);
    card.pillage = I16(bytes, 0x49);
    card.per_city = I16(bytes, 0x4b);
    card.bonus_human = I16(bytes, 0x4d);
    card.bonus_knight = I16(bytes, 0x4f);
    card.bonus_lord = I16(bytes, 0x51);
    card.bonus_warlord = I16(bytes, 0x53);
    card.die_human = I16(bytes, 0x55);
    card.die_knight = I16(bytes, 0x57);
    card.die_lord = I16(bytes, 0x59);
    card.die_warlord = I16(bytes, 0x5b);
    card.poor = I16(bytes, 0x5f);
    card.early = I16(bytes, 0x61);
    card.human_share = I16(bytes, 0x63);
    card.solidarity = I16(bytes, 0x65);
    return card;
}

std::optional<std::pair<std::string, std::vector<std::string>>> Describe(
        const std::string& data_dir, int level, int number) {
    auto file = ReadFile(Path(data_dir, level, number, "DSC"));
    if (!file) return std::nullopt;
    std::vector<std::string> lines;
    std::string line;
    std::string text = *file + "\n";
    for (char character : text) {
        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line += character;
        }
    }
    std::string name = lines.empty() ? "" : lines.front();
    if (!lines.empty()) lines.erase(lines.begin());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return std::make_pair(name, lines);
}

std::vector<std::string> Deck(const std::string& data_dir, int level) {
    std::vector<std::string> names;
    for (int number = 0; number < 30; number++) {
        auto file = ReadFile(Path(data_dir, level, number, "DSC"));
        if (!file) break;
        std::string name = file->substr(0, 40);
        names.push_back(name.substr(0, name.find('\r')));
    }
    return names;
}

int Count(const std::string& data_dir, int level) {
    int count = 0;
    while (count < 100 && FileExists(Path(data_dir, level, count, "CRD"))) count++;
    return count;
}

}  // namespace w2::aicard
