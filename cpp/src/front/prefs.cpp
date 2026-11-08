#include "front/prefs.hpp"

#include <cstdlib>
#include <map>

#include <SDL.h>

#include "nlohmann/json.hpp"
#include "util/util.hpp"

namespace prefs {

namespace {
constexpr const char* kPrefsFileName = "w2-prefs.json";  ///< Settings file in the user folder.

/**
 * @brief Every setting by name, read from the file on first use.
 *
 * A file that is missing or does not parse leaves the settings empty.
 */
std::map<std::string, std::string>& Store() {
    static std::map<std::string, std::string> settings;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (auto text = w2::ReadFile(UserFile(kPrefsFileName)); text.ok()) {
            auto json = nlohmann::ordered_json::parse(*text, nullptr, /*allow_exceptions=*/false);
            if (!json.is_discarded() && json.is_object()) {
                for (const auto& [name, value] : json.items()) {
                    if (value.is_string()) {
                        settings[name] = value.get<std::string>();
                    } else if (value.is_number_integer()) {
                        settings[name] = std::to_string(value.get<int64_t>());
                    } else if (value.is_boolean()) {
                        settings[name] = value.get<bool>() ? "1" : "0";
                    }
                }
            }
        }
    }
    return settings;
}
}  // namespace

std::filesystem::path UserFile(const std::string& name) {
    static const std::filesystem::path kDir = []() -> std::filesystem::path {
        // W2_USER_DIR keeps scripted runs and tests away from the player's
        // own settings and saved games.
        if (const char* override_dir = std::getenv("W2_USER_DIR")) {
            return std::filesystem::path(override_dir);
        }
        char* pref_path = SDL_GetPrefPath("", "Warlords II Remake");
        std::filesystem::path dir_path = pref_path ? pref_path : "";
        SDL_free(pref_path);
        return dir_path;
    }();
    return kDir / name;
}

std::string Get(const std::string& key) {
    auto& settings = Store();
    auto found = settings.find(key);
    return found == settings.end() ? "" : found->second;
}

void Set(const std::string& key, const std::string& value) {
    auto& settings = Store();
    if (value.empty()) {
        settings.erase(key);
    } else {
        settings[key] = value;
    }
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    for (const auto& [name, setting] : settings) json[name] = setting;
    w2::WriteFile(UserFile(kPrefsFileName), json.dump()).IgnoreError();
}

int GetInt(const std::string& key, int fallback) {
    auto number = w2::ToInt(Get(key));
    return number ? static_cast<int>(*number) : fallback;
}

}  // namespace prefs
