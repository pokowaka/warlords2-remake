#include "platform/display.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include <SDL.h>

#include "absl/base/no_destructor.h"

#include "platform/gfx.hpp"

namespace display {

DisplayWindow::~DisplayWindow() {
    Close(/*destroy_window=*/true);
}

SDL_Window* DisplayWindow::Open(const char* title, bool windowed, int size_w, int size_h) {
    Close(/*destroy_window=*/true);
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    SDL_DisplayMode mode;
    int width = 1280;
    int height = 960;
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
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                               flags);
    if (window_ != nullptr) SDL_RaiseWindow(window_);
    if (window_ != nullptr && sized) {
        int win_w = 0;
        int win_h = 0;
        int pix_w = 0;
        int pix_h = 0;
        SDL_GetWindowSize(window_, &win_w, &win_h);
        SDL_GetWindowSizeInPixels(window_, &pix_w, &pix_h);
        double dpi_ratio = win_w > 0 ? static_cast<double>(pix_w) / win_w : 1.0;
        width = static_cast<int>(std::lround(size_w / dpi_ratio));
        height = static_cast<int>(std::lround(size_h / dpi_ratio));
        SDL_SetWindowMinimumSize(window_, std::min(kScreenW, width), std::min(kScreenH, height));
        SDL_SetWindowSize(window_, width, height);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (window_ != nullptr) {
        SDL_SetWindowMinimumSize(window_, kScreenW, kScreenH);
    }
    state_.windowed = windowed;
    return window_;
}

SDL_Renderer* DisplayWindow::CreateRenderer() {
    if (window_ == nullptr) return nullptr;
    if (owned_sdl_renderer_ != nullptr) {
        SDL_DestroyRenderer(owned_sdl_renderer_);
        owned_sdl_renderer_ = nullptr;
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    owned_sdl_renderer_ =
            SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (owned_sdl_renderer_ == nullptr) {
        owned_sdl_renderer_ = SDL_CreateRenderer(window_, -1, 0);
    }
    if (owned_sdl_renderer_ == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        owned_sdl_renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    return owned_sdl_renderer_;
}

void DisplayWindow::Close(bool destroy_window) {
    if (canvas_ != nullptr) {
        SDL_DestroyTexture(canvas_);
        canvas_ = nullptr;
    }
    crt_shader_.Shutdown();
    if (owned_sdl_renderer_ != nullptr) {
        SDL_DestroyRenderer(owned_sdl_renderer_);
        owned_sdl_renderer_ = nullptr;
    }
    if (destroy_window && window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    no_canvas_ = false;
}

void DisplayWindow::SetOriginalAspect(bool enabled) {
    if (enabled) {
        state_.wanted = std::make_pair(kScreenW, kScreenH);
    } else {
        state_.wanted.reset();
    }
}

void DisplayWindow::Measure() {
    int win_w = kScreenW;
    int win_h = kScreenH;
    if (window_ != nullptr) SDL_GetWindowSize(window_, &win_w, &win_h);
    int real_w = win_w;
    int real_h = win_h;
    if (window_ != nullptr) SDL_GetWindowSizeInPixels(window_, &real_w, &real_h);
    double fit = std::min(static_cast<double>(real_w) / kScreenW,
                          static_cast<double>(real_h) / kScreenH);
    state_.max_scale = std::max(1, static_cast<int>(fit));
    state_.smooth_scale = !no_canvas_ && fit > 1.0 && state_.max_scale / fit < 0.75 ? fit : 0.0;
    state_.max_short = std::max(state_.max_scale,
                                static_cast<int>(std::min(static_cast<double>(real_w) / kScreenW,
                                                          static_cast<double>(real_h) / kShortH)));
    int fits = std::max(state_.max_scale,
                        static_cast<int>(std::min(static_cast<double>(real_w) / kScreenW,
                                                  static_cast<double>(real_h) / state_.min_h)));
    int pick = state_.chosen > state_.max_scale && state_.chosen > fits ? 0 : state_.chosen;
    state_.smooth = state_.smooth_scale > 0.0 && (pick == kSmooth || pick == 0);
    state_.scale = state_.smooth ? state_.max_scale + 1
                                 : std::min(pick > 0 ? pick : state_.max_scale, fits);
    state_.shrink = state_.smooth ? fit / state_.scale : 1.0;
    state_.pw = state_.shrink < 1.0
                        ? std::max(kScreenW * state_.scale,
                                   static_cast<int>(std::lround(real_w / state_.shrink)))
                        : real_w;
    state_.ph = state_.shrink < 1.0
                        ? std::max(kScreenH * state_.scale,
                                   static_cast<int>(std::lround(real_h / state_.shrink)))
                        : real_h;
    state_.dpi = win_w > 0 ? static_cast<double>(state_.pw) / win_w : 1.0;
}

void DisplayWindow::CaptureGlassShot(gfx::Renderer& renderer) {
    if (state_.glass_shot.empty()) return;
    renderer.SaveShot(state_.glass_shot);
    state_.glass_shot.clear();
}

void DisplayWindow::BeginFrame(gfx::Renderer& renderer) {
    SDL_Renderer* sdl_ren = renderer.SdlRenderer();
    if (sdl_ren == nullptr) return;
    int canvas_w = 0;
    int canvas_h = 0;
    if (canvas_ != nullptr) SDL_QueryTexture(canvas_, nullptr, nullptr, &canvas_w, &canvas_h);
    if (canvas_ != nullptr && (!UsesCanvas() || canvas_w != state_.pw || canvas_h != state_.ph)) {
        SDL_SetRenderTarget(sdl_ren, nullptr);
        SDL_DestroyTexture(canvas_);
        canvas_ = nullptr;
    }
    if (!UsesCanvas()) return;
    if (canvas_ == nullptr) {
        canvas_ = SDL_CreateTexture(sdl_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                    state_.pw, state_.ph);
        if (canvas_ != nullptr) SDL_SetTextureScaleMode(canvas_, SDL_ScaleModeLinear);
    }
    if (canvas_ == nullptr || SDL_SetRenderTarget(sdl_ren, canvas_) != 0) {
        if (canvas_ != nullptr) SDL_DestroyTexture(canvas_);
        canvas_ = nullptr;
        no_canvas_ = true;
        Measure();
    }
}

void DisplayWindow::Present(gfx::Renderer& renderer) {
    SDL_Renderer* sdl_ren = renderer.SdlRenderer();
    if (sdl_ren == nullptr) return;
    if (canvas_ == nullptr || SDL_GetRenderTarget(sdl_ren) != canvas_) {
        CaptureGlassShot(renderer);
        SDL_RenderPresent(sdl_ren);
        return;
    }
    if (TubeActive()) {
        crt_shader_.Present(sdl_ren, canvas_, CrtGeometry());
    } else {
        SDL_SetRenderTarget(sdl_ren, nullptr);
        SDL_RenderSetClipRect(sdl_ren, nullptr);
        SDL_SetRenderDrawColor(sdl_ren, 0, 0, 0, 255);
        SDL_RenderClear(sdl_ren);
        SDL_RenderCopy(sdl_ren, canvas_, nullptr, nullptr);
    }
    CaptureGlassShot(renderer);
    SDL_RenderPresent(sdl_ren);
    SDL_SetRenderTarget(sdl_ren, canvas_);
}

void DisplayWindow::Choose(int n) {
    state_.chosen = n == kSmooth ? kSmooth : std::max(1, n);
    Measure();
}

UiDimensions DisplayWindow::UiSize() const {
    int width = state_.pw / state_.scale;
    int height = state_.ph / state_.scale;
    if (state_.wanted.has_value()) {
        width = std::min(width, state_.wanted->first);
        height = std::min(height, state_.wanted->second);
    }
    return {width, height};
}

void DisplayWindow::SetFrame(int width, int height) {
    state_.w = width;
    state_.h = height;
    state_.ox = (state_.pw - width * state_.scale) / 2;
    state_.oy = (state_.ph - height * state_.scale) / 2;
}

void DisplayWindow::PushUi(gfx::Renderer& renderer) const {
    renderer.Push();
    renderer.Origin();
    renderer.Translate(state_.ox, state_.oy);
    renderer.Scale(state_.scale);
}

void DisplayWindow::PushMap(gfx::Renderer& renderer, int x, int y, int cam_x, int cam_y,
                            int zoom) const {
    renderer.Push();
    renderer.Origin();
    renderer.Translate(state_.ox + x * state_.scale - cam_x, state_.oy + y * state_.scale - cam_y);
    renderer.Scale(zoom);
}

UiDimensions DisplayWindow::ToUi(double x, double y) const {
    double pix_x = x * state_.dpi;
    double pix_y = y * state_.dpi;
    if (TubeActive()) {
        std::tie(pix_x, pix_y) = crt_shader_.Unwarp(CrtGeometry(), pix_x, pix_y);
    }
    int ui_x = static_cast<int>(std::floor((pix_x - state_.ox) / state_.scale));
    int ui_y = static_cast<int>(std::floor((pix_y - state_.oy) / state_.scale));
    return {std::clamp(ui_x, 0, state_.w - 1), std::clamp(ui_y, 0, state_.h - 1)};
}

WindowPoint DisplayWindow::FromUi(double x, double y) const {
    double pix_x = state_.ox + x * state_.scale;
    double pix_y = state_.oy + y * state_.scale;
    if (TubeActive()) {
        crt::FrameGeometry geom = CrtGeometry();
        double win_x = pix_x;
        double win_y = pix_y;
        for (int i = 0; i < 8; i++) {
            auto [cur_x, cur_y] = crt_shader_.Unwarp(geom, win_x, win_y);
            win_x += pix_x - cur_x;
            win_y += pix_y - cur_y;
        }
        pix_x = win_x;
        pix_y = win_y;
    }
    return {pix_x / state_.dpi, pix_y / state_.dpi};
}

OptionalUiPoint DisplayWindow::OffFrame(double x, double y) const {
    double pix_x = x * state_.dpi;
    double pix_y = y * state_.dpi;
    if (pix_x >= state_.ox && pix_y >= state_.oy && pix_x < state_.ox + state_.w * state_.scale &&
        pix_y < state_.oy + state_.h * state_.scale) {
        return std::nullopt;
    }
    return std::make_pair(static_cast<int>(std::floor((pix_x - state_.ox) / state_.scale)),
                          static_cast<int>(std::floor((pix_y - state_.oy) / state_.scale)));
}

void DisplayWindow::SetWindowed(bool enabled) {
    if (window_ == nullptr) {
        state_.windowed = enabled;
        return;
    }
    SDL_SetWindowFullscreen(window_, enabled ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    state_.windowed = enabled;
    Measure();
}

crt::FrameGeometry DisplayWindow::CrtGeometry() const {
    return crt::FrameGeometry{state_.pw, state_.ph,    state_.ox,  state_.oy,    state_.w,
                              state_.h,  state_.scale, state_.dpi, state_.shrink};
}

DisplayWindow& DefaultWindow() {
    static absl::NoDestructor<DisplayWindow> instance;
    return *instance;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
State& state = DefaultWindow().MutableState();

SDL_Window* Open(const char* title, bool windowed, int size_w, int size_h) {
    return DefaultWindow().Open(title, windowed, size_w, size_h);
}
SDL_Window* Window() {
    return DefaultWindow().Window();
}
void Measure() {
    DefaultWindow().Measure();
}
void BeginFrame() {
    DefaultWindow().BeginFrame(gfx::DefaultRenderer());
}
void Present() {
    DefaultWindow().Present(gfx::DefaultRenderer());
}
void Choose(int n) {
    DefaultWindow().Choose(n);
}
UiDimensions UiSize() {
    return DefaultWindow().UiSize();
}
void SetFrame(int width, int height) {
    DefaultWindow().SetFrame(width, height);
}
void PushUi() {
    DefaultWindow().PushUi(gfx::DefaultRenderer());
}
void PushMap(int x, int y, int cam_x, int cam_y, int zoom) {
    DefaultWindow().PushMap(gfx::DefaultRenderer(), x, y, cam_x, cam_y, zoom);
}
UiDimensions ToUi(double x, double y) {
    return DefaultWindow().ToUi(x, y);
}
WindowPoint FromUi(double x, double y) {
    return DefaultWindow().FromUi(x, y);
}
OptionalUiPoint OffFrame(double x, double y) {
    return DefaultWindow().OffFrame(x, y);
}
void SetWindowed(bool enabled) {
    DefaultWindow().SetWindowed(enabled);
}
bool PointerAway() {
    return DefaultWindow().PointerAway();
}

}  // namespace display
