#include "front/prefs.hpp"

#include <cstdlib>
#include <map>

#include <SDL.h>

#include "util/json.hpp"
#include "util/util.hpp"

namespace prefs {

static const char* file_name = "w2-prefs.json";  ///< The settings file, in the user folder.

std::string UserFile(const std::string& name) {
    static std::string dir = []() {
        // W2_USER_DIR keeps scripted runs and tests away from the player's
        // own settings and saved games
        if (const char* override_dir = std::getenv("W2_USER_DIR")) {
            std::string dir_path = override_dir;
            return dir_path.empty() || dir_path.back() == '/' ? dir_path : dir_path + '/';
        }
        char* pref_path = SDL_GetPrefPath("", "Warlords II Remake");
        std::string dir_path = pref_path ? pref_path : "";
        SDL_free(pref_path);
        return dir_path;
    }();
    return dir + name;
}

namespace {
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
        if (auto text = w2::ReadFile(UserFile(file_name))) {
            try {
                auto json = w2::Json::Parse(*text);
                for (auto& [name, value] : json.Entries()) settings[name] = value.Str();
            } catch (...) {
            }
        }
    }
    return settings;
}
}  // namespace

std::string Get(const std::string& key) {
    auto& settings = Store();
    auto found = settings.find(key);
    return found == settings.end() ? "" : found->second;
}

void Set(const std::string& key, const std::string& value) {
    auto& settings = Store();
    if (value.empty())
        settings.erase(key);
    else
        settings[key] = value;
    w2::Json json = w2::Json::Object();
    for (auto& [name, setting] : settings) json.Set(name, setting);
    w2::WriteFile(UserFile(file_name), json.Dump());
}

int GetInt(const std::string& key, int fallback) {
    auto number = w2::ToInt(Get(key));
    return number ? static_cast<int>(*number) : fallback;
}

}  // namespace prefs
