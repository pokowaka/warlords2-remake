// The remake's own settings, none of them the original's: the interface's
// scale, the map's zoom, full screen or a window, 4:3, the CRT screen, the
// synthesizer the music plays on, the button shortcuts. Kept in
// w2-prefs.json, beside the saves in the player's own folder for the game
// (SDL_GetPrefPath); a setting never changed is left to its default.
#pragma once

#include <string>

namespace prefs {

/** A setting's value as written; "" when there is none. */
std::string Get(const std::string& key);
/** Change a setting ("" removes it), and write the file. */
void Set(const std::string& key, const std::string& value);
inline void Set(const std::string& key, int value) {
    Set(key, std::to_string(value));
}
int GetInt(const std::string& key, int dflt = 0);
/** Where a file of the player's own goes: prefs, saves. */
std::string UserFile(const std::string& name);

}  // namespace prefs
