#include "warlords/aicard.hpp"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::aicard {

std::string Path(const std::string& data_dir, int level, int number, const std::string& ext) {
    static const char* letters[3] = {"K", "L", "W"};  // "KLW", 7bab:21c1
    const char* l = level >= 0 && level < 3 ? letters[level] : "W";
    return Fmt("%s/CARDS/%s%03d.%s", data_dir.c_str(), l, number, ext.c_str());
}

std::optional<Card> Load(const std::string& data_dir, int level, int number) {
    auto f = ReadFile(Path(data_dir, level, number, "CRD"));
    if (!f || f->size() < static_cast<size_t>(kRecord)) return std::nullopt;
    const std::string& s = *f;
    Card c;
    for (int t = 0; t <= 28; t++) c.fight_order.push_back(U8(s, 0x0e + t));
    c.groups = I16(s, 0x08);
    c.bold = I16(s, 0x0a);
    c.cautious = I16(s, 0x2b);
    c.rebuild_type = I16(s, 0x2d);
    c.rebuild_type_rich = I16(s, 0x2f);
    c.rebuild_limit = I16(s, 0x31);
    c.raze = I16(s, 0x45);
    c.sack = I16(s, 0x47);
    c.pillage = I16(s, 0x49);
    c.per_city = I16(s, 0x4b);
    c.bonus_human = I16(s, 0x4d);
    c.bonus_knight = I16(s, 0x4f);
    c.bonus_lord = I16(s, 0x51);
    c.bonus_warlord = I16(s, 0x53);
    c.die_human = I16(s, 0x55);
    c.die_knight = I16(s, 0x57);
    c.die_lord = I16(s, 0x59);
    c.die_warlord = I16(s, 0x5b);
    c.poor = I16(s, 0x5f);
    c.early = I16(s, 0x61);
    c.human_share = I16(s, 0x63);
    c.solidarity = I16(s, 0x65);
    return c;
}

std::optional<std::pair<std::string, std::vector<std::string>>> Describe(
        const std::string& data_dir, int level, int number) {
    auto s = ReadFile(Path(data_dir, level, number, "DSC"));
    if (!s) return std::nullopt;
    std::vector<std::string> lines;
    std::string cur;
    std::string text = *s + "\n";
    for (char ch : text) {
        if (ch == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    std::string name = lines.empty() ? "" : lines.front();
    if (!lines.empty()) lines.erase(lines.begin());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return std::make_pair(name, lines);
}

std::vector<std::string> Deck(const std::string& data_dir, int level) {
    std::vector<std::string> out;
    for (int n = 0; n < 30; n++) {
        auto s = ReadFile(Path(data_dir, level, n, "DSC"));
        if (!s) break;
        std::string name = s->substr(0, 40);
        out.push_back(name.substr(0, name.find('\r')));
    }
    return out;
}

int Count(const std::string& data_dir, int level) {
    int n = 0;
    while (n < 100 && FileExists(Path(data_dir, level, n, "CRD"))) n++;
    return n;
}

}  // namespace w2::aicard
