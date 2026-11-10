/**
 * @file
 * @brief Window lifecycle, viewport geometry, coordinate spaces, and integer/smooth UI scaling
 * (`display::DisplayWindow`).
 */
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "platform/crt.hpp"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace gfx {
class Renderer;
}  // namespace gfx

namespace display {

constexpr int kScreenW = 640;  ///< Classic 640x480 logical screen width.
constexpr int kScreenH = 480;  ///< Classic 640x480 logical screen height.
constexpr int kShortH = 400;   ///< Minimum UI rows required by the main map screen layout.
constexpr int kSmooth = -1;    ///< Sentinel scale value selecting smooth fractional scaling.

/// Logical UI dimensions or coordinate pair `(width, height)` / `(x, y)`.
using UiDimensions = std::pair<int, int>;

/// Floating-point window coordinate pair `(x, y)` in window points.
using WindowPoint = std::pair<double, double>;

/// Optional logical UI coordinate or frame size.
using OptionalUiPoint = std::optional<UiDimensions>;

/**
 * @brief Snapshot of window, offscreen canvas, and logical UI scaling metrics.
 */
struct State {
    double dpi = 1.0;           ///< Device pixels per window point.
    int pw = kScreenW;          ///< Render target width in device pixels.
    int ph = kScreenH;          ///< Render target height in device pixels.
    double shrink = 1.0;        ///< Ratio of physical window pixels to offscreen canvas pixels.
    int scale = 1;              ///< Device pixels per logical UI pixel.
    int max_scale = 1;          ///< Largest integer scale fitting 640x480.
    int max_short = 1;          ///< Largest integer scale fitting 640x400 (`kShortH`).
    int min_h = kScreenH;       ///< Minimum UI height required by the active screen.
    double smooth_scale = 0.0;  ///< Fractional UI scale when smooth scaling is available; else 0.
    bool smooth = false;        ///< True when rendering via the smooth-scaled offscreen canvas.
    int chosen = 0;             ///< User-selected scale: integer scale, `kSmooth`, or 0 for auto.
    bool windowed = false;      ///< True in windowed mode; false in fullscreen desktop mode.
    bool crt = false;           ///< True when the CRT monitor simulation is enabled.
    OptionalUiPoint wanted;     ///< Optional aspect-locked UI frame (e.g. `{640, 480}` for 4:3).
    int w = kScreenW;           ///< Active UI frame width in logical UI pixels.
    int h = kScreenH;           ///< Active UI frame height in logical UI pixels.
    int ox = 0;                 ///< Top-left x origin of the UI frame in device pixels.
    int oy = 0;                 ///< Top-left y origin of the UI frame in device pixels.
    bool mouse_inside = true;   ///< True while the pointer lies inside the window.
    OptionalUiPoint off;        ///< Unclamped UI coordinates when pointer is over the black border.
    std::string glass_shot;     ///< Pending post-CRT screenshot path requested by test scripts.
};

/**
 * @brief RAII owner of the `SDL_Window*`, offscreen supersampling/CRT canvas, and coordinate
 * transforms between physical window points, logical UI pixels, and map pixels.
 *
 * Supports three coordinate spaces:
 * - **Device:** physical window or offscreen canvas pixels (HiDPI-aware).
 * - **UI:** logical interface pixels (`640x480` minimum, `1` UI pixel = `scale` device pixels).
 * - **Map:** map pixels (`40` per tile), drawn at `zoom` device pixels per map pixel.
 *
 * Invariants:
 * - `state_.scale >= 1`, `state_.pw >= kScreenW`, and `state_.ph >= kShortH` after `Measure()`.
 * - All owned SDL resources (`canvas_`, `crt_shader_`, `owned_sdl_renderer_`, `window_`) are
 *   released in reverse creation order by `Close()` and `~DisplayWindow()`.
 *
 * Not thread-safe; called exclusively on the main UI thread.
 */
class DisplayWindow {
  public:
    DisplayWindow() = default;
    ~DisplayWindow();

    DisplayWindow(const DisplayWindow&) = delete;
    DisplayWindow& operator=(const DisplayWindow&) = delete;

    /**
     * @brief Opens the SDL window in fullscreen desktop or windowed mode.
     * @param title UTF-8 window title.
     * @param windowed True for a resizable window; false for fullscreen desktop.
     * @param size_w Explicit device-pixel width when `> 0`, forcing windowed mode.
     * @param size_h Explicit device-pixel height when `> 0`, forcing windowed mode.
     * @return Created `SDL_Window*`, or `nullptr` on failure.
     */
    SDL_Window* Open(const char* title, bool windowed, int size_w = 0, int size_h = 0);

    /**
     * @brief Creates an `SDL_Renderer*` owned by this `DisplayWindow` (preferring OpenGL + vsync,
     * falling back to software).
     */
    SDL_Renderer* CreateRenderer();

    /**
     * @brief Destroys any offscreen canvas, CRT textures, owned renderer, and `SDL_Window*`.
     * @param destroy_window When false, releases auxiliary textures without destroying `window_`.
     */
    void Close(bool destroy_window = true);

    /** @brief Returns the underlying `SDL_Window*`, or `nullptr` if closed. */
    SDL_Window* Window() const { return window_; }

    /** @brief Returns the `SDL_Renderer*` created via `CreateRenderer()`, if any. */
    SDL_Renderer* OwnedSdlRenderer() const { return owned_sdl_renderer_; }

    /** @brief Read-only access to current viewport and scaling metrics. */
    const State& GetState() const { return state_; }

    /** @brief Mutable access to viewport and scaling state. */
    State& MutableState() { return state_; }

    /** @brief Enables or disables the CRT simulation pass. */
    void SetCrtEnabled(bool enabled) { state_.crt = enabled; }
    /** @brief True when the CRT simulation pass is enabled. */
    bool CrtEnabled() const { return state_.crt; }

    /** @brief Locks the UI frame to classic 640x480 (4:3) when `enabled`, or fills the window. */
    void SetOriginalAspect(bool enabled);

    /** @brief Recomputes physical pixel dimensions and selects integer or smooth UI scaling. */
    void Measure();

    /** @brief Binds the offscreen canvas on `renderer` when smooth scaling or CRT is active. */
    void BeginFrame(gfx::Renderer& renderer);

    /** @brief Presents the rendered frame through `renderer` (applying downsample or CRT warp). */
    void Present(gfx::Renderer& renderer);

    /** @brief Selects UI scale `n` (or `kSmooth`) and remeasures the display. */
    void Choose(int n);

    /** @brief Returns available logical UI dimensions `(width, height)` in UI pixels. */
    UiDimensions UiSize() const;

    /** @brief Centres a logical UI frame of `width` by `height` UI pixels within the target. */
    void SetFrame(int width, int height);

    /** @brief Pushes the UI coordinate transform onto `renderer`'s transform stack. */
    void PushUi(gfx::Renderer& renderer) const;

    /** @brief Pushes the map coordinate transform onto `renderer` at `zoom` device px/map px. */
    void PushMap(gfx::Renderer& renderer, int x, int y, int cam_x, int cam_y, int zoom) const;

    /** @brief Converts window coordinates `(x, y)` (in points) to clamped logical UI pixels. */
    UiDimensions ToUi(double x, double y) const;

    /** @brief Converts logical UI coordinates `(x, y)` back to window coordinates (in points). */
    WindowPoint FromUi(double x, double y) const;

    /** @brief Returns unclamped UI coordinates when `(x, y)` lies outside the active UI frame. */
    OptionalUiPoint OffFrame(double x, double y) const;

    /** @brief Switches between windowed and fullscreen desktop mode. */
    void SetWindowed(bool enabled);

    /** @brief True when in windowed mode and the cursor has left the window. */
    bool PointerAway() const { return state_.windowed && !state_.mouse_inside; }

    /** @brief Builds the `crt::FrameGeometry` snapshot corresponding to `state_`. */
    crt::FrameGeometry CrtGeometry() const;

  private:
    bool TubeActive() const { return state_.crt && !no_canvas_; }
    bool UsesCanvas() const { return state_.shrink < 1.0 || TubeActive(); }
    void CaptureGlassShot(gfx::Renderer& renderer);

    State state_{};                               ///< Viewport, scaling, and frame metrics.
    SDL_Window* window_ = nullptr;                ///< Owned SDL window handle.
    SDL_Renderer* owned_sdl_renderer_ = nullptr;  ///< Optional SDL renderer created by this class.
    SDL_Texture* canvas_ = nullptr;               ///< Offscreen target for smooth downsample / CRT.
    bool no_canvas_ = false;                      ///< Latched if target textures are unsupported.
    crt::CrtShader crt_shader_;                   ///< Owned CRT post-processing pass.
};

}  // namespace display

namespace w2 {
using display::DisplayWindow;
}  // namespace w2
