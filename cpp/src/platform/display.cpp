#include "platform/display.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include <SDL.h>

#include "platform/crt.hpp"
#include "platform/gfx.hpp"

namespace display {

State d;  // NOLINT(readability-identifier-length)

namespace {
SDL_Window* win = nullptr;
SDL_Texture* canvas =
        nullptr;         // Offscreen target when downsampling or rendering through the CRT shader.
bool no_canvas = false;  // True when the renderer does not support target textures.

/** @brief True when the CRT shader is active and supported. */
bool Tube() {
    return d.crt && !no_canvas;
}
/** @brief True when rendering should target the offscreen `canvas` texture. */
bool OnCanvas() {
    return d.shrink < 1 || Tube();
}

/** @brief Captures a post-present window screenshot if requested by a test script (`glass`). */
void Shoot() {
    if (d.glass_shot.empty()) return;
    gfx::SaveShot(d.glass_shot);
    d.glass_shot.clear();
}
}  // namespace

SDL_Window* Open(const char* title, bool windowed, int size_w, int size_h) {
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    SDL_DisplayMode mode;
    int width = 1280, height = 960;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        width = std::max(kW, mode.w * 4 / 5);
        height = std::max(kH, mode.h * 4 / 5);
    }
    bool sized = size_w > 0 && size_h > 0;
    if (sized) {
        windowed = true;
        width = size_w;
        height = size_h;
    }
    if (!windowed) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    // Allow an unfocused click on the window to both focus it and register in-game.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                           flags);
    if (win) SDL_RaiseWindow(win);
    if (win && sized) {
        // Convert requested device-pixel dimensions into window points on HiDPI displays.
        int win_w, win_h, pix_w, pix_h;
        SDL_GetWindowSize(win, &win_w, &win_h);
        SDL_GetWindowSizeInPixels(win, &pix_w, &pix_h);
        double dpi_ratio = win_w > 0 ? static_cast<double>(pix_w) / win_w : 1;
        width = static_cast<int>(std::lround(size_w / dpi_ratio));
        height = static_cast<int>(std::lround(size_h / dpi_ratio));
        SDL_SetWindowMinimumSize(win, std::min(kW, width), std::min(kH, height));
        SDL_SetWindowSize(win, width, height);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (win) {
        SDL_SetWindowMinimumSize(win, kW, kH);
    }
    d.windowed = windowed;
    return win;
}

SDL_Window* Window() {
    return win;
}

void Measure() {
    int win_w = kW, win_h = kH;
    if (win) SDL_GetWindowSize(win, &win_w, &win_h);
    int real_w = win_w, real_h = win_h;
    // Query the window's physical pixel dimensions directly (since renderer
    // output size reflects the bound offscreen canvas when one is active).
    if (win) SDL_GetWindowSizeInPixels(win, &real_w, &real_h);
    double fit = std::min(static_cast<double>(real_w) / kW, static_cast<double>(real_h) / kH);
    d.max_scale = std::max(1, static_cast<int>(fit));
    // Offer smooth scaling when the largest integer scale leaves > 25% unused.
    d.smooth_scale = !no_canvas && fit > 1 && d.max_scale / fit < 0.75 ? fit : 0;
    // On the main screen, allow a larger integer scale if it fits `kShortH` rows.
    d.max_short = std::max(d.max_scale,
                           static_cast<int>(std::min(static_cast<double>(real_w) / kW,
                                                     static_cast<double>(real_h) / kShortH)));
    int fits = std::max(d.max_scale,
                        static_cast<int>(std::min(static_cast<double>(real_w) / kW,
                                                  static_cast<double>(real_h) / d.min_h)));
    int pick = d.chosen > d.max_scale && d.chosen > fits ? 0 : d.chosen;
    d.smooth = d.smooth_scale > 0 && (pick == kSmooth || pick == 0);
    d.scale = d.smooth ? d.max_scale + 1 : std::min(pick > 0 ? pick : d.max_scale, fits);
    d.shrink = d.smooth ? fit / d.scale : 1;
    d.pw = d.shrink < 1 ? std::max(kW * d.scale, static_cast<int>(std::lround(real_w / d.shrink)))
                        : real_w;
    d.ph = d.shrink < 1 ? std::max(kH * d.scale, static_cast<int>(std::lround(real_h / d.shrink)))
                        : real_h;
    d.dpi = win_w > 0 ? static_cast<double>(d.pw) / win_w : 1;
}

void BeginFrame() {
    SDL_Renderer* ren = gfx::Renderer();
    if (!ren) return;
    int canvas_w = 0, canvas_h = 0;
    if (canvas) SDL_QueryTexture(canvas, nullptr, nullptr, &canvas_w, &canvas_h);
    if (canvas && (!OnCanvas() || canvas_w != d.pw || canvas_h != d.ph)) {
        SDL_SetRenderTarget(ren, nullptr);
        SDL_DestroyTexture(canvas);
        canvas = nullptr;
    }
    if (!OnCanvas()) return;
    if (!canvas) {
        canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, d.pw,
                                   d.ph);
        if (canvas) SDL_SetTextureScaleMode(canvas, SDL_ScaleModeLinear);
    }
    if (!canvas || SDL_SetRenderTarget(ren, canvas) != 0) {
        if (canvas) SDL_DestroyTexture(canvas);
        canvas = nullptr;
        no_canvas = true;
        Measure();
    }
}

void Present() {
    SDL_Renderer* ren = gfx::Renderer();
    if (!ren) return;
    if (!canvas || SDL_GetRenderTarget(ren) != canvas) {
        Shoot();
        SDL_RenderPresent(ren);
        return;
    }
    if (Tube()) {
        crt::Present(ren, canvas);
    } else {
        SDL_SetRenderTarget(ren, nullptr);
        SDL_RenderSetClipRect(ren, nullptr);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, canvas, nullptr, nullptr);
    }
    Shoot();
    SDL_RenderPresent(ren);
    // Keep `canvas` bound between frames so script screenshots read from it.
    SDL_SetRenderTarget(ren, canvas);
}

void Choose(int n) {
    d.chosen = n == kSmooth ? kSmooth : std::max(1, n);
    Measure();
}

std::pair<int, int> UiSize() {
    int width = d.pw / d.scale, height = d.ph / d.scale;
    if (d.wanted) {
        width = std::min(width, d.wanted->first);
        height = std::min(height, d.wanted->second);
    }
    return {width, height};
}

void SetFrame(int width, int height) {
    d.w = width;
    d.h = height;
    d.ox = (d.pw - width * d.scale) / 2;
    d.oy = (d.ph - height * d.scale) / 2;
}

void PushUi() {
    gfx::Push();
    gfx::Origin();
    gfx::Translate(d.ox, d.oy);
    gfx::Scale(d.scale);
}

void PushMap(int x, int y, int cam_x, int cam_y, int zoom) {
    gfx::Push();
    gfx::Origin();
    gfx::Translate(d.ox + x * d.scale - cam_x, d.oy + y * d.scale - cam_y);
    gfx::Scale(zoom);
}

std::pair<int, int> ToUi(double x, double y) {
    double pix_x = x * d.dpi, pix_y = y * d.dpi;
    if (Tube()) std::tie(pix_x, pix_y) = crt::Unwarp(pix_x, pix_y);
    int ui_x = static_cast<int>(std::floor((pix_x - d.ox) / d.scale));
    int ui_y = static_cast<int>(std::floor((pix_y - d.oy) / d.scale));
    return {std::clamp(ui_x, 0, d.w - 1), std::clamp(ui_y, 0, d.h - 1)};
}

std::pair<double, double> FromUi(double x, double y) {
    double pix_x = d.ox + x * d.scale, pix_y = d.oy + y * d.scale;
    if (Tube()) {
        // Fixed-point iteration inverting the CRT barrel distortion.
        double win_x = pix_x, win_y = pix_y;
        for (int i = 0; i < 8; i++) {
            auto [cur_x, cur_y] = crt::Unwarp(win_x, win_y);
            win_x += pix_x - cur_x;
            win_y += pix_y - cur_y;
        }
        pix_x = win_x;
        pix_y = win_y;
    }
    return {pix_x / d.dpi, pix_y / d.dpi};
}

std::optional<std::pair<int, int>> OffFrame(double x, double y) {
    double pix_x = x * d.dpi, pix_y = y * d.dpi;
    if (pix_x >= d.ox && pix_y >= d.oy && pix_x < d.ox + d.w * d.scale &&
        pix_y < d.oy + d.h * d.scale)
        return std::nullopt;
    return std::make_pair(static_cast<int>(std::floor((pix_x - d.ox) / d.scale)),
                          static_cast<int>(std::floor((pix_y - d.oy) / d.scale)));
}

void SetWindowed(bool enabled) {
    if (!win) {
        d.windowed = enabled;
        return;
    }
    SDL_SetWindowFullscreen(win, enabled ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    d.windowed = enabled;
    Measure();
}

bool PointerAway() {
    return d.windowed && !d.mouse_inside;
}

}  // namespace display
