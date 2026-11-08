#include "front/prefs.hpp"

#include <map>

#include <SDL.h>

#include "util/json.hpp"
#include "util/util.hpp"

namespace prefs {

static const char* file_name = "w2-prefs.json";

std::string userFile(const std::string& name) {
    static std::string dir = []() {
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
        if (auto text = w2::readFile(userFile(file_name))) {
            try {
                auto j = w2::Json::parse(*text);
                for (auto& [k, v] : j.entries()) s[k] = v.str();
            } catch (...) {
            }
        }
    }
    return s;
}
}  // namespace

std::string get(const std::string& key) {
    auto& s = Store();
    auto it = s.find(key);
    return it == s.end() ? "" : it->second;
}

void set(const std::string& key, const std::string& value) {
    auto& s = Store();
    if (value.empty())
        s.erase(key);
    else
        s[key] = value;
    w2::Json j = w2::Json::object();
    for (auto& [k, v] : s) j.set(k, v);
    w2::writeFile(userFile(file_name), j.dump());
}

int getInt(const std::string& key, int dflt) {
    auto v = w2::toInt(get(key));
    return v ? static_cast<int>(*v) : dflt;
}

}  // namespace prefs
