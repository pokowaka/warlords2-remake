#include "platform/display.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include <SDL.h>

#include "platform/crt.hpp"
#include "platform/gfx.hpp"

namespace display {

State state;

namespace {
SDL_Window* win = nullptr;
SDL_Texture* canvas =
        nullptr;         // Offscreen target when downsampling or rendering through the CRT shader.
bool no_canvas = false;  // True when the renderer does not support target textures.

/** @brief True when the CRT shader is active and supported. */
bool Tube() {
    return state.crt && !no_canvas;
}
/** @brief True when rendering should target the offscreen `canvas` texture. */
bool OnCanvas() {
    return state.shrink < 1 || Tube();
}

/** @brief Captures a post-present window screenshot if requested by a test script (`glass`). */
void Shoot() {
    if (state.glass_shot.empty()) return;
    gfx::SaveShot(state.glass_shot);
    state.glass_shot.clear();
}
}  // namespace

SDL_Window* Open(const char* title, bool windowed, int size_w, int size_h) {
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    SDL_DisplayMode mode;
    int width = 1280, height = 960;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        width = std::max(kScreenW, mode.w * 4 / 5);
        height = std::max(kScreenH, mode.h * 4 / 5);
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
        SDL_SetWindowMinimumSize(win, std::min(kScreenW, width), std::min(kScreenH, height));
        SDL_SetWindowSize(win, width, height);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (win) {
        SDL_SetWindowMinimumSize(win, kScreenW, kScreenH);
    }
    state.windowed = windowed;
    return win;
}

SDL_Window* Window() {
    return win;
}

void Measure() {
    int win_w = kScreenW, win_h = kScreenH;
    if (win) SDL_GetWindowSize(win, &win_w, &win_h);
    int real_w = win_w, real_h = win_h;
    // Query the window's physical pixel dimensions directly (since renderer
    // output size reflects the bound offscreen canvas when one is active).
    if (win) SDL_GetWindowSizeInPixels(win, &real_w, &real_h);
    double fit = std::min(static_cast<double>(real_w) / kScreenW,
                          static_cast<double>(real_h) / kScreenH);
    state.max_scale = std::max(1, static_cast<int>(fit));
    // Offer smooth scaling when the largest integer scale leaves > 25% unused.
    state.smooth_scale = !no_canvas && fit > 1 && state.max_scale / fit < 0.75 ? fit : 0;
    // On the main screen, allow a larger integer scale if it fits `kShortH` rows.
    state.max_short = std::max(state.max_scale,
                               static_cast<int>(std::min(static_cast<double>(real_w) / kScreenW,
                                                         static_cast<double>(real_h) / kShortH)));
    int fits = std::max(state.max_scale,
                        static_cast<int>(std::min(static_cast<double>(real_w) / kScreenW,
                                                  static_cast<double>(real_h) / state.min_h)));
    int pick = state.chosen > state.max_scale && state.chosen > fits ? 0 : state.chosen;
    state.smooth = state.smooth_scale > 0 && (pick == kSmooth || pick == 0);
    state.scale =
            state.smooth ? state.max_scale + 1 : std::min(pick > 0 ? pick : state.max_scale, fits);
    state.shrink = state.smooth ? fit / state.scale : 1;
    state.pw = state.shrink < 1 ? std::max(kScreenW * state.scale,
                                           static_cast<int>(std::lround(real_w / state.shrink)))
                                : real_w;
    state.ph = state.shrink < 1 ? std::max(kScreenH * state.scale,
                                           static_cast<int>(std::lround(real_h / state.shrink)))
                                : real_h;
    state.dpi = win_w > 0 ? static_cast<double>(state.pw) / win_w : 1;
}

void BeginFrame() {
    SDL_Renderer* ren = gfx::Renderer();
    if (!ren) return;
    int canvas_w = 0, canvas_h = 0;
    if (canvas) SDL_QueryTexture(canvas, nullptr, nullptr, &canvas_w, &canvas_h);
    if (canvas && (!OnCanvas() || canvas_w != state.pw || canvas_h != state.ph)) {
        SDL_SetRenderTarget(ren, nullptr);
        SDL_DestroyTexture(canvas);
        canvas = nullptr;
    }
    if (!OnCanvas()) return;
    if (!canvas) {
        canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                   state.pw, state.ph);
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
    state.chosen = n == kSmooth ? kSmooth : std::max(1, n);
    Measure();
}

std::pair<int, int> UiSize() {
    int width = state.pw / state.scale, height = state.ph / state.scale;
    if (state.wanted) {
        width = std::min(width, state.wanted->first);
        height = std::min(height, state.wanted->second);
    }
    return {width, height};
}

void SetFrame(int width, int height) {
    state.w = width;
    state.h = height;
    state.ox = (state.pw - width * state.scale) / 2;
    state.oy = (state.ph - height * state.scale) / 2;
}

void PushUi() {
    gfx::Push();
    gfx::Origin();
    gfx::Translate(state.ox, state.oy);
    gfx::Scale(state.scale);
}

void PushMap(int x, int y, int cam_x, int cam_y, int zoom) {
    gfx::Push();
    gfx::Origin();
    gfx::Translate(state.ox + x * state.scale - cam_x, state.oy + y * state.scale - cam_y);
    gfx::Scale(zoom);
}

std::pair<int, int> ToUi(double x, double y) {
    double pix_x = x * state.dpi, pix_y = y * state.dpi;
    if (Tube()) std::tie(pix_x, pix_y) = crt::Unwarp(pix_x, pix_y);
    int ui_x = static_cast<int>(std::floor((pix_x - state.ox) / state.scale));
    int ui_y = static_cast<int>(std::floor((pix_y - state.oy) / state.scale));
    return {std::clamp(ui_x, 0, state.w - 1), std::clamp(ui_y, 0, state.h - 1)};
}

std::pair<double, double> FromUi(double x, double y) {
    double pix_x = state.ox + x * state.scale, pix_y = state.oy + y * state.scale;
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
    return {pix_x / state.dpi, pix_y / state.dpi};
}

std::optional<std::pair<int, int>> OffFrame(double x, double y) {
    double pix_x = x * state.dpi, pix_y = y * state.dpi;
    if (pix_x >= state.ox && pix_y >= state.oy && pix_x < state.ox + state.w * state.scale &&
        pix_y < state.oy + state.h * state.scale)
        return std::nullopt;
    return std::make_pair(static_cast<int>(std::floor((pix_x - state.ox) / state.scale)),
                          static_cast<int>(std::floor((pix_y - state.oy) / state.scale)));
}

void SetWindowed(bool enabled) {
    if (!win) {
        state.windowed = enabled;
        return;
    }
    SDL_SetWindowFullscreen(win, enabled ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    state.windowed = enabled;
    Measure();
}

bool PointerAway() {
    return state.windowed && !state.mouse_inside;
}

}  // namespace display
