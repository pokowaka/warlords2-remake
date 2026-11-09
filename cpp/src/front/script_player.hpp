/**
 * @file
 * @brief Automated UI test script loader and frame-by-frame command runner (`w2::ScriptPlayer`).
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace w2 {

class MapScreenController;

/**
 * @brief Encapsulates automated `--script` test playback over `MapScreenController`.
 *
 * Reads script commands line by line (such as `wait`, `click`, `key`, `move`, `text`, `shot`,
 * `glass`, `pad`, `stick`, `eval`, `opt`, `load`, `centre`, `pointer`, `tileptr`, `spawn`,
 * `advise`, `select`, `attack`, `near`, `greet`, `sdlclick`, and `quit`) and executes them once
 * per rendered frame until a `wait` or `quit` boundary is reached.
 *
 * Thread safety: Not thread-safe; invoked exclusively on the main UI thread.
 */
class ScriptPlayer {
  public:
    using LineList = std::vector<std::string>;

    ScriptPlayer() = default;
    ~ScriptPlayer() = default;

    ScriptPlayer(const ScriptPlayer&) = delete;
    ScriptPlayer& operator=(const ScriptPlayer&) = delete;

    /** @brief Loads script command lines from `path` and activates script playback. */
    void Load(const std::filesystem::path& path);

    /** @brief Returns true when a script file was loaded for this session. */
    bool IsLoaded() const { return loaded_; }

    /** @brief Advances script playback by one frame against `controller`. */
    void Step(MapScreenController& controller);

  private:
    LineList lines_;         ///< Script command lines in execution order.
    size_t next_index_ = 0;  ///< Index of the next command line to execute.
    int wait_frames_ = 0;    ///< Remaining frames to pause before executing `lines_[next_index_]`.
    bool loaded_ = false;    ///< True when `Load()` has been called.
};

}  // namespace w2
