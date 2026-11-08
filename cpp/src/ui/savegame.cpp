/**
 * @file
 * @brief Game > Save game and Load game.
 *
 * Ten slots, each with a name, as SAVEINFO.DAT has them -- "Not_Used" for an
 * empty one. Save lists all ten in the list chooser titled "Save Game"
 * (group 46), on the slot last used; the one chosen asks for its name -- 15
 * characters, 160 pixels -- and is written. Load lists the slots in use
 * (group 47) and tells the side whose turn it is that "thy turn continues!".
 * The slots are w2-save<n>.json beside the game, and their names
 * w2-saveinfo.json.
 */
#include "front/prefs.hpp"
#include "ui/dialogs.hpp"
#include "util/json.hpp"
#include "util/util.hpp"
#include "warlords/save.hpp"

namespace savegame {

namespace {
const int kSlots = 10;                       ///< The number of save slots.
const char* unused_name = "Not_Used";        ///< The name an empty slot shows.
const char* info_file = "w2-saveinfo.json";  ///< The file of the slots' names.
int last_slot = 0;                           ///< The slot last saved or loaded.

/** @brief The file a slot's game is kept in, from its key ("save3"). */
std::string SlotFile(const std::string& key) {
    return prefs::UserFile("w2-" + key + ".json");
}
/** @brief The key of slot number `slot`: "save" and the number. */
std::string SlotKey(int slot) {
    return "save" + std::to_string(slot);
}

/**
 * @brief The ten slots' names; "Not_Used" for each one the file lacks.
 *
 * A missing or unreadable names file reads as ten empty slots.
 */
std::vector<std::string> ReadInfo() {
    std::vector<std::string> names;
    if (auto text = w2::ReadFile(prefs::UserFile(info_file))) {
        try {
            auto json = w2::Json::Parse(*text);
            for (auto& name : json.Items()) names.push_back(name.Str());
        } catch (...) {
        }
    }
    while (static_cast<int>(names.size()) < kSlots) names.push_back(unused_name);
    names.resize(kSlots);
    return names;
}

/** @brief Writes the slots' names to the names file. */
void WriteInfo(const std::vector<std::string>& names) {
    w2::Json json = w2::Json::Array();
    for (auto& name : names) json.Push(name);
    w2::WriteFile(prefs::UserFile(info_file), json.Dump());
}
}  // namespace

/** @brief Writes `game` to the slot `key`; false when it could not be written. */
bool WriteSlot(const std::string& key, const w2::Game& game) {
    try {
        return w2::WriteFile(SlotFile(key), w2::save::Encode(game));
    } catch (...) {
        return false;
    }
}

/** @brief The game in slot `key`, or null when there is none or it won't read. */
std::unique_ptr<w2::Game> ReadSlot(const std::string& key) {
    auto text = w2::ReadFile(SlotFile(key));
    if (!text) return nullptr;
    try {
        return w2::save::Decode(*text, G.data_dir);
    } catch (...) {
        return nullptr;
    }
}

/** @brief How many slots hold a game. */
int Used() {
    int count = 0;
    for (auto& name : ReadInfo())
        if (name != unused_name) count++;
    return count;
}

/**
 * @brief Game > Save game: choose a slot, name it, and write the game there.
 *
 * An empty name saves as "Not_Used". The names file is only updated when the
 * game was written.
 */
void Save() {
    auto names = ReadInfo();
    choose::Open(kit::Text(0x2e, 0), names, last_slot, [names](int slot) mutable {
        if (slot == w2::kNone) return;
        last_slot = slot;
        input::Options options;
        options.title = kit::Text(0x2e, 0);
        options.lines = {kit::Text(0x2e, 1), kit::Text(0x2e, 2)};
        options.text = names[slot] != unused_name ? names[slot] : "";
        options.max_chars = 15;
        options.max_width = 160;
        options.ok = [names, slot](const std::string& typed) mutable {
            std::string name = typed.empty() ? unused_name : typed;
            if (!WriteSlot(SlotKey(slot), *G.g)) {
                search::ShowMessage("The game could not be saved:",
                                    "the file could not be written.");
                return;
            }
            names[slot] = name;
            WriteInfo(names);
        };
        input::Open(options);
    });
}

/**
 * @brief Game > Load game: choose one of the slots in use and read it.
 *
 * Nothing opens when no slot is in use. The game read goes to `loaded`, then
 * the side whose turn it is is told its turn continues.
 */
void Load(std::function<void(std::unique_ptr<w2::Game>)> loaded) {
    auto names = ReadInfo();
    std::vector<std::string> list;
    std::vector<int> slots;
    for (int i = 0; i < kSlots; i++) {
        if (names[i] != unused_name) {
            list.push_back(names[i]);
            slots.push_back(i);
        }
    }
    if (list.empty()) return;
    int start = 0;
    for (size_t i = 0; i < slots.size(); i++)
        if (slots[i] == last_slot) start = static_cast<int>(i);
    choose::Open(kit::Text(0x2f, 0), list, start, [slots, loaded](int chosen) {
        if (chosen == w2::kNone) return;
        auto game = ReadSlot(SlotKey(slots[chosen]));
        if (!game) return;
        last_slot = slots[chosen];
        std::string name = game->current < static_cast<int>(game->sides.size())
                                   ? game->sides[game->current]->name
                                   : "";
        loaded(std::move(game));
        search::Say(w2::Format(kit::Text(0x2f, 1), name));
    });
}

}  // namespace savegame
