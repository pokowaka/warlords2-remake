// Window geometry, coordinate spaces, and integer/smooth UI scaling.
//
// Three coordinate spaces are used, matching the Lua reference:
//   - device: physical window/canvas pixels (HiDPI-aware). When the smooth
//     view or CRT shader is active, rendering targets an offscreen canvas at
//     the next integer scale up and downsamples or warps it onto the window.
//   - UI: logical interface pixels (640x480 minimum), where 1 UI pixel equals
//     `scale` device pixels.
//   - map: map pixels (40 per tile), drawn at `zoom` device pixels per map
//     pixel independently of the UI scale.
//
// Input handlers and hit tests operate in UI pixels.
#pragma once

#include <optional>
#include <string>
#include <utility>

struct SDL_Window;

namespace display {

// NOLINTNEXTLINE(readability-identifier-length)
constexpr int kW = 640, kH = 480;  ///< Classic 640x480 screen dimensions.
constexpr int kShortH = 400;       ///< Minimum UI rows needed by the main screen layout.
constexpr int kSmooth = -1;        ///< Sentinel scale value selecting smooth fractional scaling.

/**
 * @brief Current window, canvas, and scaling state.
 */
struct State {
    double dpi = 1;          ///< Device pixels per window point.
    int pw = 640, ph = 480;  ///< Render target dimensions in device pixels.
    double shrink =
            1;      ///< Ratio of physical window pixels to canvas device pixels (< 1 on canvas).
    int scale = 1;  ///< Device pixels per UI pixel.
    int max_scale = 1;  ///< Largest integer scale that fits 640x480.
    int max_short = 1;  ///< Largest integer scale that fits 640x400 (`kShortH`).
    int min_h = kH;  ///< Minimum UI height required by the active screen (`kH` on start screens).
    double smooth_scale =
            0;              ///< Fractional UI scale when smooth scaling is available; 0 otherwise.
    bool smooth = false;    ///< True when rendering via the smooth-scaled offscreen canvas.
    int chosen = 0;         ///< User-selected scale: integer scale, `kSmooth`, or 0 for auto.
    bool windowed = false;  ///< True in windowed mode; false in fullscreen desktop mode.
    bool crt = false;       ///< True when the CRT monitor effect (View > CRT screen) is enabled.
    std::optional<std::pair<int, int>> wanted;  ///< Optional aspect-locked UI frame (View > 4:3).
    int w = 640, h = 480;                       ///< Active frame dimensions in UI pixels.
    int ox = 0, oy = 0;        ///< Top-left origin of the UI frame in device pixels.
    bool mouse_inside = true;  ///< True while the mouse cursor is inside the window.
    std::optional<std::pair<int, int>>
            off;             ///< UI coordinates when cursor is over the black border.
    std::string glass_shot;  ///< Path for a post-CRT window screenshot (`glass` script cmd).
};
extern State d;  // NOLINT(readability-identifier-length)

/**
 * @brief Opens the SDL window in fullscreen desktop or windowed mode (with
 * optional explicit device-pixel size from `--size`).
 */
SDL_Window* Open(const char* title, bool windowed, int size_w = 0, int size_h = 0);
/** @brief Returns the active `SDL_Window*`. */
SDL_Window* Window();
/** @brief Recomputes window dimensions and selects the active integer or smooth UI scale. */
void Measure();
/** @brief Begins rendering a frame, binding the offscreen canvas when smooth scaling or CRT is
 * active. */
void BeginFrame();
/** @brief Presents the rendered frame to the window (applying downsampling or CRT warp if needed).
 */
void Present();
/** @brief Selects UI scale `n` (or `kSmooth`) and remeasures the display. */
void Choose(int n);
/** @brief Returns the available UI dimensions `(width, height)` in UI pixels. */
std::pair<int, int> UiSize();
/** @brief Centres a UI frame of `width` by `height` UI pixels on the screen. */
void SetFrame(int width, int height);
/** @brief Pushes the UI coordinate transform onto the `gfx` transform stack. */
void PushUi();
/** @brief Pushes the map coordinate transform at `zoom` device pixels per map pixel. */
void PushMap(int x, int y, int cam_x, int cam_y, int zoom);
/** @brief Converts a window position (in points) to clamped UI pixel coordinates. */
std::pair<int, int> ToUi(double x, double y);
/** @brief Converts UI pixel coordinates back to window coordinates (in points). */
std::pair<double, double> FromUi(double x, double y);
/** @brief Returns unclamped UI coordinates when `(x, y)` lies outside the active UI frame, or
 * `std::nullopt`. */
std::optional<std::pair<int, int>> OffFrame(double x, double y);
/** @brief Switches between windowed and fullscreen desktop mode. */
void SetWindowed(bool enabled);
/** @brief True when the window is windowed and the cursor has left it. */
bool PointerAway();

}  // namespace display
