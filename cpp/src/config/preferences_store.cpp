#include "config/preferences_store.hpp"

#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <utility>

#include <SDL.h>

#include "util/util.hpp"

namespace w2 {

PreferencesStore::PreferencesStore(std::filesystem::path file_path)
        : file_path_(std::move(file_path)) {}

std::filesystem::path PreferencesStore::UserDirectory() {
    if (const char* override_dir = std::getenv("W2_USER_DIR");
        override_dir != nullptr && override_dir[0] != '\0') {
        return std::filesystem::path(override_dir);
    }
    char* pref_path = SDL_GetPrefPath("", "Warlords II Remake");
    std::filesystem::path dir_path = pref_path != nullptr ? pref_path : "";
    SDL_free(pref_path);
    return dir_path;
}

std::filesystem::path PreferencesStore::UserFilePath(std::string_view file_name) {
    return UserDirectory() / file_name;
}

PreferencesStore PreferencesStore::ForUserApp() {
    return PreferencesStore(UserFilePath(kDefaultFileName));
}

void PreferencesStore::EnsureLoaded() {
    if (loaded_) return;
    loaded_ = true;
    values_ = nlohmann::ordered_json::object();
    if (file_path_.empty()) return;
    auto text = ReadFile(file_path_);
    if (!text.ok()) return;
    auto parsed = nlohmann::ordered_json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return;
    for (const auto& [name, val] : parsed.items()) {
        if (val.is_string()) {
            values_[name] = val.get<std::string>();
        } else if (val.is_number_integer()) {
            values_[name] = std::to_string(val.get<int64_t>());
        } else if (val.is_boolean()) {
            values_[name] = val.get<bool>() ? "1" : "0";
        }
    }
}

std::string PreferencesStore::Get(std::string_view key) {
    EnsureLoaded();
    std::string key_str(key);
    auto found = values_.find(key_str);
    if (found == values_.end() || !found->is_string()) return "";
    return found->get<std::string>();
}

int PreferencesStore::GetInt(std::string_view key, int fallback) {
    auto number = ToInt(Get(key));
    return number.has_value() ? static_cast<int>(*number) : fallback;
}

void PreferencesStore::Set(std::string_view key, std::string_view value) {
    EnsureLoaded();
    std::string key_str(key);
    if (value.empty()) {
        values_.erase(key_str);
    } else {
        values_[key_str] = std::string(value);
    }
}

void PreferencesStore::Set(std::string_view key, int value) {
    Set(key, std::to_string(value));
}

absl::Status PreferencesStore::Save() {
    EnsureLoaded();
    if (file_path_.empty()) {
        return absl::InvalidArgumentError(
                "Failed to save preferences: backing file path is empty; pass a valid filesystem "
                "path when constructing PreferencesStore.");
    }
    if (std::filesystem::path parent = file_path_.parent_path(); !parent.empty()) {
        std::error_code dir_err;
        std::filesystem::create_directories(parent, dir_err);
    }
    return WriteFile(file_path_, values_.dump());
}

}  // namespace w2
