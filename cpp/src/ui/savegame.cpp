// Game > Save game and Load game (7721:093b, 7721:026b).
//
// Ten slots, each with a name, as SAVEINFO.DAT has them -- "Not_Used" for an
// empty one. Save lists all ten in the list chooser titled "Save Game"
// (group 46), on the slot last used; the one chosen asks for its name -- 15
// characters, 160 pixels -- and is written (7721:0995). Load lists the slots
// in use (group 47) and tells the side whose turn it is that "thy turn
// continues!". The slots are w2-save<n>.json beside the game, and their names
// w2-saveinfo.json.
#include "front/prefs.hpp"
#include "ui/dialogs.hpp"
#include "util/json.hpp"
#include "util/util.hpp"
#include "warlords/save.hpp"

namespace savegame {

namespace {
const int kSlots = 10;
const char* unused = "Not_Used";
const char* info = "w2-saveinfo.json";
int last = 0;

std::string SlotFile(const std::string& key) {
    return prefs::userFile("w2-" + key + ".json");
}
std::string SlotKey(int i) {
    return "save" + std::to_string(i);
}

std::vector<std::string> ReadInfo() {
    std::vector<std::string> names;
    if (auto text = w2::readFile(prefs::userFile(info))) {
        try {
            auto j = w2::Json::parse(*text);
            for (auto& n : j.items()) names.push_back(n.str());
        } catch (...) {
        }
    }
    while (static_cast<int>(names.size()) < kSlots) names.push_back(unused);
    names.resize(kSlots);
    return names;
}

void WriteInfo(const std::vector<std::string>& names) {
    w2::Json j = w2::Json::array();
    for (auto& n : names) j.push(n);
    w2::writeFile(prefs::userFile(info), j.dump());
}
}  // namespace

bool writeSlot(const std::string& key, const w2::Game& g) {
    try {
        return w2::writeFile(SlotFile(key), w2::save::encode(g));
    } catch (...) {
        return false;
    }
}

std::unique_ptr<w2::Game> readSlot(const std::string& key) {
    auto text = w2::readFile(SlotFile(key));
    if (!text) return nullptr;
    try {
        return w2::save::decode(*text, G.dataDir);
    } catch (...) {
        return nullptr;
    }
}

int used() {
    int n = 0;
    for (auto& name : ReadInfo())
        if (name != unused) n++;
    return n;
}

void save() {
    auto names = ReadInfo();
    choose::open(kit::text(0x2e, 0), names, last, [names](int slot) mutable {
        if (slot == w2::NONE) return;
        last = slot;
        input::Options o;
        o.title = kit::text(0x2e, 0);
        o.lines = {kit::text(0x2e, 1), kit::text(0x2e, 2)};
        o.text = names[slot] != unused ? names[slot] : "";
        o.maxChars = 15;
        o.maxWidth = 160;
        o.ok = [names, slot](const std::string& typed) mutable {
            std::string name = typed.empty() ? unused : typed;
            if (!writeSlot(SlotKey(slot), *G.g)) {
                search::message("The game could not be saved:", "the file could not be written.");
                return;
            }
            names[slot] = name;
            WriteInfo(names);
        };
        input::open(o);
    });
}

void load(std::function<void(std::unique_ptr<w2::Game>)> loaded) {
    auto names = ReadInfo();
    std::vector<std::string> list;
    std::vector<int> slots;
    for (int i = 0; i < kSlots; i++) {
        if (names[i] != unused) {
            list.push_back(names[i]);
            slots.push_back(i);
        }
    }
    if (list.empty()) return;
    int start = 0;
    for (size_t k = 0; k < slots.size(); k++)
        if (slots[k] == last) start = static_cast<int>(k);
    choose::open(kit::text(0x2f, 0), list, start, [slots, loaded](int k) {
        if (k == w2::NONE) return;
        auto g = readSlot(SlotKey(slots[k]));
        if (!g) return;
        last = slots[k];
        std::string name =
                g->current < static_cast<int>(g->sides.size()) ? g->sides[g->current]->name : "";
        loaded(std::move(g));
        search::say(w2::format(kit::text(0x2f, 1), name));
    });
}

}  // namespace savegame
