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
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config/preferences_store.hpp"
#include "nlohmann/json.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/save.hpp"

namespace savegame {

namespace {

using SlotNameList = std::vector<std::string>;
using SlotIndexList = std::vector<int>;

constexpr int kSlots = 10;                             ///< The number of save slots.
constexpr const char* kUnusedName = "Not_Used";        ///< The name an empty slot shows.
constexpr const char* kInfoFile = "w2-saveinfo.json";  ///< The file of the slots' names.

/** @brief The file a slot's game is kept in, from its key ("save3"). */
std::filesystem::path SlotFile(const std::string& key) {
    return w2::PreferencesStore::UserFilePath("w2-" + key + ".json");
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
SlotNameList ReadInfo() {
    SlotNameList names;
    if (auto text = w2::ReadFile(w2::PreferencesStore::UserFilePath(kInfoFile)); text.ok()) {
        auto json = nlohmann::ordered_json::parse(*text, nullptr, /*allow_exceptions=*/false);
        if (!json.is_discarded() && json.is_array()) {
            for (const auto& entry : json) {
                names.push_back(entry.is_string()
                                        ? w2::Utf8ToLatin1(entry.get_ref<const std::string&>())
                                        : "");
            }
        }
    }
    while (static_cast<int>(names.size()) < kSlots) names.emplace_back(kUnusedName);
    names.resize(kSlots);
    return names;
}

/** @brief Writes the slots' names to the names file. */
void WriteInfo(const SlotNameList& names) {
    nlohmann::ordered_json json = nlohmann::ordered_json::array();
    for (const auto& name : names) json.push_back(w2::Latin1ToUtf8(name));
    w2::WriteFile(w2::PreferencesStore::UserFilePath(kInfoFile), json.dump()).IgnoreError();
}

/** @brief Prompts for save-slot `slot`'s label and writes the active game there. */
void PromptSlotName(w2::UiContext& ctx, SlotNameList names, int slot) {
    input::Options options;
    options.title = kit::Text(ctx, 0x2e, 0);
    options.lines = {kit::Text(ctx, 0x2e, 1), kit::Text(ctx, 0x2e, 2)};
    options.text = names[slot] != kUnusedName ? names[slot] : "";
    options.max_chars = 15;
    options.max_width = 160;
    options.ok = [&ctx, names = std::move(names), slot](const std::string& typed) mutable {
        std::string name = typed.empty() ? kUnusedName : typed;
        if (!WriteSlot(SlotKey(slot), ctx.ActiveGame())) {
            search::ShowMessage(ctx,
                                "The game could not be saved:", "the file could not be written.");
            return;
        }
        names[slot] = name;
        WriteInfo(names);
    };
    input::Open(ctx, options);
}

}  // namespace

/** @brief Writes `game` to the slot `key`; false when it could not be written. */
bool WriteSlot(const std::string& key, const w2::Game& game) {
    return w2::WriteFile(SlotFile(key), w2::save::Encode(game)).ok();
}

/** @brief The game in slot `key`, or null when there is none or it won't read. */
std::unique_ptr<w2::Game> ReadSlot(const w2::UiContext& ctx, const std::string& key) {
    auto text = w2::ReadFile(SlotFile(key));
    if (!text.ok()) return nullptr;
    auto decoded = w2::save::Decode(*text, ctx.Assets().DataDir());
    return decoded.ok() ? std::move(*decoded) : nullptr;
}

/** @brief How many slots hold a game. */
int Used() {
    int count = 0;
    for (const auto& name : ReadInfo()) {
        if (name != kUnusedName) count++;
    }
    return count;
}

/**
 * @brief Game > Save game: choose a slot, name it, and write the game there.
 *
 * An empty name saves as "Not_Used". The names file is only updated when the
 * game was written.
 */
void Save(w2::UiContext& ctx) {
    SlotNameList names = ReadInfo();
    choose::Open(ctx, kit::Text(ctx, 0x2e, 0), names, ctx.LastSaveSlot(),
                 [&ctx, names](int slot) mutable {
                     if (slot == w2::kNone) return;
                     ctx.SetLastSaveSlot(slot);
                     PromptSlotName(ctx, std::move(names), slot);
                 });
}

/**
 * @brief Game > Load game: choose one of the slots in use and read it.
 *
 * Nothing opens when no slot is in use. The game read goes to `after`, then
 * the side whose turn it is is told its turn continues.
 */
void Load(w2::UiContext& ctx, LoadedGameCallback after) {
    SlotNameList names = ReadInfo();
    SlotNameList list;
    SlotIndexList slots;
    for (int i = 0; i < kSlots; i++) {
        if (names[i] != kUnusedName) {
            list.push_back(names[i]);
            slots.push_back(i);
        }
    }
    if (list.empty()) return;
    int start = 0;
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i] == ctx.LastSaveSlot()) start = static_cast<int>(i);
    }
    choose::Open(ctx, kit::Text(ctx, 0x2f, 0), list, start,
                 [&ctx, slots, after = std::move(after)](int chosen) {
                     if (chosen == w2::kNone) return;
                     auto game = ReadSlot(ctx, SlotKey(slots[chosen]));
                     if (!game) return;
                     ctx.SetLastSaveSlot(slots[chosen]);
                     std::string name = game->CurrentSide() ? game->CurrentSide()->Name() : "";
                     after(std::move(game));
                     search::Say(ctx, w2::Format(kit::Text(ctx, 0x2f, 1), name));
                 });
}

}  // namespace savegame
