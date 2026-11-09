#include <algorithm>
#include <cmath>
#include <tuple>

#include <SDL.h>

#include "front/map_screen_controller.hpp"
#include "platform/display.hpp"
#include "platform/pad.hpp"
#include "ui/kit.hpp"

namespace w2 {

namespace {

constexpr double kPointerSpeed = 1.2;
constexpr double kSlideSpeed = 600.0;

}  // namespace

void MapScreenController::PointTo(double x, double y) {
    UiContext& ctx = Ctx();
    display::DisplayWindow& disp = ctx.Display();
    pad::GamepadInput& pad_in = ctx.Gamepad();
    const auto& disp_state = disp.GetState();

    x = std::clamp(x, 0.0, disp_state.w - 0.001);
    y = std::clamp(y, 0.0, disp_state.h - 0.001);
    auto [prev_pad_x, prev_pad_y] = pad_in.VirtualCursor();
    double dx = x - prev_pad_x;
    double dy = y - prev_pad_y;
    pad_in.SetVirtualCursor(x, y);
    disp.MutableState().mouse_inside = true;
    disp.MutableState().off.reset();
    Mousemoved(dx, dy);

    if (static_cast<int>(x) == mouse_x_ && static_cast<int>(y) == mouse_y_) return;
    mouse_x_ = static_cast<int>(x);
    mouse_y_ = static_cast<int>(y);

    SDL_Window* win = disp.Window();
    if (win && SDL_GetKeyboardFocus() == win) {
        auto [win_x, win_y] = disp.FromUi(mouse_x_ + 0.5, mouse_y_ + 0.5);
        double rounded_x = std::round(win_x);
        double rounded_y = std::round(win_y);
        pad_in.SetLastWindowMouse(rounded_x, rounded_y);
        pad_in.RecordWarp(static_cast<int>(rounded_x), static_cast<int>(rounded_y));
        SDL_WarpMouseInWindow(win, static_cast<int>(rounded_x), static_cast<int>(rounded_y));
    }
}

void MapScreenController::PadMouse(int& held, int button, bool down) {
    if (down && held++ == 0) {
        Mousepressed(mouse_x_, mouse_y_, button);
    } else if (!down && held > 0 && --held == 0) {
        Mousereleased(mouse_x_, mouse_y_, button);
    }
}

void MapScreenController::PadKey(const std::string& key, bool down) {
    UiContext& ctx = Ctx();
    if (!down) {
        ctx.Keyboard().Release(key);
        return;
    }
    ctx.Keyboard().Press(key);
    Keypressed(key);
}

void MapScreenController::PadButton(int button, bool down) {
    UiContext& ctx = Ctx();
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
    case pad::kRightTrigger:
        PadMouse(pad_left_held_, 1, down);
        break;
    case SDL_CONTROLLER_BUTTON_X:
    case pad::kLeftTrigger:
        PadMouse(pad_right_held_, 2, down);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        PadKey("escape", down);
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        PadKey("return", down);
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        if (down) Wheelmoved(-1);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        if (down) Wheelmoved(1);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        PadKey("8", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        PadKey("2", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        PadKey("4", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        PadKey("6", down);
        break;
    case SDL_CONTROLLER_BUTTON_BACK:
        down ? ctx.Keyboard().Press("lshift") : ctx.Keyboard().Release("lshift");
        break;
    case SDL_CONTROLLER_BUTTON_START:
        down ? ctx.Keyboard().Press("lalt") : ctx.Keyboard().Release("lalt");
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        PadKey("space", down);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        PadKey("home", down);
        break;
    default:
        break;
    }
}

void MapScreenController::PadLetGo() {
    if (pad_left_held_ != 0) Mousereleased(mouse_x_, mouse_y_, 1);
    if (pad_right_held_ != 0) Mousereleased(mouse_x_, mouse_y_, 2);
    pad_left_held_ = 0;
    pad_right_held_ = 0;
}

void MapScreenController::PadUpdate() {
    UiContext& ctx = Ctx();
    pad::GamepadInput& pad_in = ctx.Gamepad();
    const auto& disp_state = ctx.Display().GetState();

    uint64_t now = SDL_GetPerformanceCounter();
    if (last_pad_counter_ == 0) last_pad_counter_ = now;
    double delta_t = std::min(
            0.1, static_cast<double>(now - last_pad_counter_) / SDL_GetPerformanceFrequency());
    last_pad_counter_ = now;

    int button = 0;
    bool down = false;
    while (pad_in.PollTrigger(button, down)) PadButton(button, down);

    auto sticks = pad_in.ReadSticks();
    if (sticks.rx != 0.0 || sticks.ry != 0.0) {
        auto [pad_x, pad_y] = pad_in.VirtualCursor();
        if (static_cast<int>(pad_x) != mouse_x_ || static_cast<int>(pad_y) != mouse_y_) {
            pad_x = mouse_x_ + 0.5;
            pad_y = mouse_y_ + 0.5;
            pad_in.SetVirtualCursor(pad_x, pad_y);
        }
        double step = kPointerSpeed * disp_state.h * std::hypot(sticks.rx, sticks.ry) * delta_t;
        PointTo(pad_x + sticks.rx * step, pad_y + sticks.ry * step);
    }
    if ((sticks.lx != 0.0 || sticks.ly != 0.0) &&
        !(ctx.Starting() || !ctx.GamePtr() || state_->BlocksWheelAndLeftStick() ||
          !ctx.Modals().Empty())) {
        Viewport& viewport = ctx.GetViewport();
        double slide = kSlideSpeed * std::hypot(sticks.lx, sticks.ly) * delta_t * disp_state.scale /
                       (kTile * viewport.Zoom());
        viewport.PanTiles(sticks.lx * slide, sticks.ly * slide, ctx.GamePtr(), disp_state.scale);
    }
}

void MapScreenController::SyncTyping() {
    kit::Modal* top = Ctx().Modals().Top();
    int want = top ? top->TypingState() : 0;
    if (want == typing_now_) return;
    if (want == 0 || typing_now_ == 2) SDL_StopTextInput();
    typing_now_ = want;
    if (want == 0) return;
    const char* hint = SDL_GetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    SDL_StartTextInput();
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, nullptr);
}

void MapScreenController::InitPointerState(SDL_Window* win) {
    UiContext& ctx = Ctx();
    display::DisplayWindow& disp = ctx.Display();
    pad::GamepadInput& pad_in = ctx.Gamepad();

    int mouse_x = 0;
    int mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    pad_in.SetLastWindowMouse(mouse_x, mouse_y);
    disp.MutableState().mouse_inside = (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS) != 0;
    disp.MutableState().off = disp.OffFrame(mouse_x, mouse_y);
    std::tie(mouse_x_, mouse_y_) = disp.ToUi(mouse_x, mouse_y);
    pad_in.SetVirtualCursor(mouse_x_ + 0.5, mouse_y_ + 0.5);
}

void MapScreenController::HandleMouseMotion(int win_x, int win_y) {
    UiContext& ctx = Ctx();
    display::DisplayWindow& disp = ctx.Display();
    pad::GamepadInput& pad_in = ctx.Gamepad();

    if (pad_in.ConsumeWarp(win_x, win_y)) return;
    auto [last_x, last_y] = pad_in.LastWindowMouse();
    auto [x, y] = disp.ToUi(win_x, win_y);
    mouse_x_ = x;
    mouse_y_ = y;
    disp.MutableState().mouse_inside = true;
    disp.MutableState().off = disp.OffFrame(win_x, win_y);
    const auto& disp_state = disp.GetState();
    double scale = disp_state.dpi / disp_state.scale;
    Mousemoved((win_x - last_x) * scale, (win_y - last_y) * scale);
    pad_in.SetLastWindowMouse(win_x, win_y);
}

void MapScreenController::PollFocusedMouse(SDL_Window* win) {
    if (HasScript() || SDL_GetKeyboardFocus() != win) return;
    UiContext& ctx = Ctx();
    display::DisplayWindow& disp = ctx.Display();
    pad::GamepadInput& pad_in = ctx.Gamepad();
    auto [last_x, last_y] = pad_in.LastWindowMouse();

    int global_x = 0;
    int global_y = 0;
    int win_x = 0;
    int win_y = 0;
    int win_w = 0;
    int win_h = 0;
    SDL_GetGlobalMouseState(&global_x, &global_y);
    SDL_GetWindowPosition(win, &win_x, &win_y);
    SDL_GetWindowSize(win, &win_w, &win_h);
    int mouse_x = global_x - win_x;
    int mouse_y = global_y - win_y;
    if (mouse_x >= 0 && mouse_y >= 0 && mouse_x < win_w && mouse_y < win_h &&
        (mouse_x != static_cast<int>(last_x) || mouse_y != static_cast<int>(last_y))) {
        auto [x, y] = disp.ToUi(mouse_x, mouse_y);
        mouse_x_ = x;
        mouse_y_ = y;
        disp.MutableState().mouse_inside = true;
        disp.MutableState().off = disp.OffFrame(mouse_x, mouse_y);
        const auto& disp_state = disp.GetState();
        double scale = disp_state.dpi / disp_state.scale;
        Mousemoved((mouse_x - last_x) * scale, (mouse_y - last_y) * scale);
        pad_in.SetLastWindowMouse(mouse_x, mouse_y);
    }
}

}  // namespace w2
