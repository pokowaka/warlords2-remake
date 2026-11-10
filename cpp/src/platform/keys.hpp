/**
 * @file
 * @brief Pressed-key tracker (`keys::KeyboardState`) indexed by LÖVE key names.
 */
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_set.h"

namespace keys {

/// Brace-enclosed list of C-string key names queried via `IsDown`.
using KeyNameList = std::initializer_list<const char*>;

/**
 * @brief Tracks currently depressed keys by key identifier (`"lshift"`, `"return"`, `"kp+"`).
 *
 * Clears cleanly on window focus loss so modifier keys never stick across focus transitions.
 *
 * Not thread-safe; mutated and queried on the main UI event thread.
 */
class KeyboardState {
  public:
    KeyboardState() = default;
    ~KeyboardState() = default;

    KeyboardState(const KeyboardState&) = delete;
    KeyboardState& operator=(const KeyboardState&) = delete;

    /** @brief Marks key `name` as pressed. */
    void Press(std::string_view name);
    /** @brief Marks key `name` as released. */
    void Release(std::string_view name);
    /** @brief Clears all currently held keys (e.g. on window focus loss). */
    void Clear();

    /** @brief Returns true if any key in `names` is currently held down. */
    bool IsDown(KeyNameList names) const;
    /** @brief Returns true if key `name` is currently held down. */
    bool IsDown(std::string_view name) const;

    /** @brief Maps an SDL keycode (`SDL_Keycode`) to its LÖVE key name, or `""` if unmapped. */
    static std::string LoveName(int sdl_keycode);

  private:
    using HeldKeySet = absl::flat_hash_set<std::string>;

    HeldKeySet held_keys_;  ///< Set of LÖVE key names currently held down.
};

}  // namespace keys

namespace w2 {
using keys::KeyboardState;
}  // namespace w2
