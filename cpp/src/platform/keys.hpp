// Tracks pressed keys by LÖVE key names ("lshift", "ralt", "a", "return",
// "kp+", etc.), matching love.keyboard.isDown in the Lua reference.
#pragma once

#include <initializer_list>
#include <string>

namespace keys {

/** @brief Marks key `name` as pressed. */
void Press(const std::string& name);
/** @brief Marks key `name` as released. */
void Release(const std::string& name);
/** @brief Clears all pressed keys (e.g. on focus loss). */
void Clear();
/** @brief Returns true if any of the keys in `names` is currently held down. */
bool IsDown(std::initializer_list<const char*> names);
/** @brief Returns true if key `name` is currently held down. */
bool IsDown(const std::string& name);

/** @brief Maps an SDL keycode to its LÖVE key name, or returns `""` if unknown. */
std::string LoveName(int sdl_keycode);

}  // namespace keys
