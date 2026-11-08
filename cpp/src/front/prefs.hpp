/**
 * @file
 * @brief The remake's own settings, none of them the original's.
 *
 * The interface's scale, the map's zoom, full screen or a window, 4:3, the
 * CRT screen, the synthesizer the music plays on, the button shortcuts. Kept
 * in w2-prefs.json, beside the saves in the player's own folder for the game
 * (SDL_GetPrefPath); a setting never changed is left to its default.
 */
#pragma once

#include <filesystem>
#include <string>

namespace prefs {

/** @brief A setting's value as written; "" when there is none. */
std::string Get(const std::string& key);
/** @brief Changes a setting ("" removes it), and writes the file. */
void Set(const std::string& key, const std::string& value);
/** @brief Changes a setting to a number, and writes the file. */
inline void Set(const std::string& key, int value) {
    Set(key, std::to_string(value));
}
/** @brief A setting as a number; `fallback` when it is unset or not a number. */
int GetInt(const std::string& key, int fallback = 0);
/** @brief Where a file of the player's own goes: prefs, saves. */
std::filesystem::path UserFile(const std::string& name);

}  // namespace prefs
