/**
 * @file
 * @brief Gamepad input and virtual cursor state (`pad::GamepadInput`) over SDL's
 * GameController API.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

union SDL_Event;
struct _SDL_GameController;

namespace pad {

/// Synthetic button IDs assigned to analog triggers beyond `SDL_GameControllerButton`.
constexpr int kLeftTrigger = 100;
constexpr int kRightTrigger = 101;

/**
 * @brief Combined normalized thumbstick deflection across connected controllers.
 */
struct Sticks {
    double lx = 0.0;  ///< Left stick horizontal axis (`-1..1`, `0` inside radial dead zone).
    double ly = 0.0;  ///< Left stick vertical axis (`-1..1`, `0` inside radial dead zone).
    double rx = 0.0;  ///< Right stick horizontal axis (`-1..1`, `0` inside radial dead zone).
    double ry = 0.0;  ///< Right stick vertical axis (`-1..1`, `0` inside radial dead zone).
};

/// Optional scripted thumbstick override injected by UI test scripts.
using OptionalSticks = std::optional<Sticks>;

/// Integer `(x, y)` window coordinate recorded when warping the OS mouse cursor.
using WindowWarp = std::pair<int, int>;

/// Floating-point `(x, y)` cursor position in UI or window coordinates.
using CursorPoint = std::pair<double, double>;

/**
 * @brief RAII manager for connected `SDL_GameController` devices, trigger hysteresis,
 * thumbstick dead-zone filtering, and virtual gamepad cursor state.
 *
 * Reads controller buttons by physical layout, converts analog triggers into
 * digital press/release events with hysteresis (`0.5` press / `0.3` release),
 * and tracks the virtual cursor / warp queue used by Steam Deck and gamepad
 * navigation.
 *
 * Invariants:
 * - Every entry in `controllers_` holds a non-null `_SDL_GameController*`
 *   closed on `SDL_CONTROLLERDEVICEREMOVED`, `Shutdown()`, or `~GamepadInput()`.
 * - `pending_warps_.size() <= kMaxPendingWarps`.
 *
 * Not thread-safe; called exclusively on the main UI thread.
 */
class GamepadInput {
  public:
    static constexpr size_t kMaxPendingWarps = 16;  ///< Bound on queued OS cursor warp points.

    GamepadInput() = default;
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    /** @brief Closes all open `SDL_GameController` handles and resets virtual cursor state. */
    void Shutdown();

    /** @brief Configures SDL controller and Steam Deck hints prior to `SDL_Init`. */
    static void ApplyHints();

    /**
     * @brief Maps an SDL button token (`"a"`, `"dpup"`, `"lefttrigger"`, `"righttrigger"`)
     * to its button ID, or `-1` if unknown.
     */
    static int ButtonFromName(std::string_view name);

    /**
     * @brief Processes an SDL controller hotplug or button event.
     * @return True when `button` and `down` were populated with a button press/release.
     */
    bool TranslateEvent(const SDL_Event& event, int& button, bool& down);

    /** @brief Polls analog triggers, emitting one press/release on threshold crossing. */
    bool PollTrigger(int& button, bool& down);

    /** @brief Reads combined dead-zone-filtered thumbsticks (or scripted override if active). */
    Sticks ReadSticks() const;

    /** @brief Overrides thumbstick deflection from a test script (clears when all axes are 0). */
    void SetScriptedSticks(const Sticks& sticks);

    /** @brief Number of currently open game controllers. */
    size_t ConnectedCount() const { return controllers_.size(); }

    /** @brief Virtual cursor position in logical UI pixels. */
    CursorPoint VirtualCursor() const { return {cursor_x_, cursor_y_}; }
    /** @brief Updates the virtual cursor position in logical UI pixels. */
    void SetVirtualCursor(double x, double y) {
        cursor_x_ = x;
        cursor_y_ = y;
    }

    /** @brief Last observed OS mouse position in window points. */
    CursorPoint LastWindowMouse() const { return {last_window_x_, last_window_y_}; }
    /** @brief Records the last observed OS mouse position in window points. */
    void SetLastWindowMouse(double x, double y) {
        last_window_x_ = x;
        last_window_y_ = y;
    }

    /** @brief Records a synthetic OS mouse warp to `(win_x, win_y)` so echo motion is ignored. */
    void RecordWarp(int win_x, int win_y);
    /** @brief Consumes warp echo `(win_x, win_y)` if pending; returns true when matched. */
    bool ConsumeWarp(int win_x, int win_y);

  private:
    struct ControllerSlot {
        _SDL_GameController* controller = nullptr;  ///< Open SDL controller handle.
        bool left_trig = false;                     ///< Left trigger currently past threshold.
        bool right_trig = false;                    ///< Right trigger currently past threshold.
    };
    using ControllerMap = std::map<int32_t, ControllerSlot>;
    using WarpQueue = std::vector<WindowWarp>;

    ControllerMap controllers_;       ///< Open controllers keyed by `SDL_JoystickID`.
    OptionalSticks scripted_sticks_;  ///< Active scripted thumbstick override, if any.
    double cursor_x_ = 0.0;           ///< Virtual stick cursor x in UI pixels.
    double cursor_y_ = 0.0;           ///< Virtual stick cursor y in UI pixels.
    double last_window_x_ = 0.0;      ///< Last seen OS mouse x in window points.
    double last_window_y_ = 0.0;      ///< Last seen OS mouse y in window points.
    WarpQueue pending_warps_;         ///< Outstanding synthetic OS mouse warps.
};

/** @brief Returns the process-default `GamepadInput` instance. */
GamepadInput& DefaultGamepad();
void Hints();
bool TranslateEvent(const SDL_Event& event, int& button, bool& down);
bool Trigger(int& button, bool& down);
Sticks ReadSticks();
void Script(const Sticks& sticks);
int FromName(const std::string& name);

}  // namespace pad

namespace w2 {
using pad::GamepadInput;
}  // namespace w2
