#include "front/prefs.hpp"

#include <cstdlib>
#include <map>

#include <SDL.h>

#include "util/json.hpp"
#include "util/util.hpp"

namespace prefs {

static const char* file_name = "w2-prefs.json";

std::string UserFile(const std::string& name) {
    static std::string dir = []() {
        // W2_USER_DIR keeps scripted runs and tests away from the player's
        // own settings and saved games
        if (const char* override_dir = std::getenv("W2_USER_DIR")) {
            std::string d = override_dir;
            return d.empty() || d.back() == '/' ? d : d + '/';
        }
        char* p = SDL_GetPrefPath("", "Warlords II Remake");
        std::string d = p ? p : "";
        SDL_free(p);
        return d;
    }();
    return dir + name;
}

namespace {
std::map<std::string, std::string>& Store() {
    static std::map<std::string, std::string> s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (auto text = w2::ReadFile(UserFile(file_name))) {
            try {
                auto j = w2::Json::Parse(*text);
                for (auto& [k, v] : j.Entries()) s[k] = v.Str();
            } catch (...) {
            }
        }
    }
    return s;
}
}  // namespace

std::string Get(const std::string& key) {
    auto& s = Store();
    auto it = s.find(key);
    return it == s.end() ? "" : it->second;
}

void Set(const std::string& key, const std::string& value) {
    auto& s = Store();
    if (value.empty())
        s.erase(key);
    else
        s[key] = value;
    w2::Json j = w2::Json::Object();
    for (auto& [k, v] : s) j.Set(k, v);
    w2::WriteFile(UserFile(file_name), j.Dump());
}

int GetInt(const std::string& key, int dflt) {
    auto v = w2::ToInt(Get(key));
    return v ? static_cast<int>(*v) : dflt;
}

}  // namespace prefs
