/**
 * @file
 * @brief Gamepad controller mapping, pointer warping, and SDL text-input synchronization.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "platform/display.hpp"
#include "platform/pad.hpp"

namespace front {

// Not the original's: a game controller plays as the mouse and a few keys,
// so that the Steam Deck plays with Steam's gamepad layout in Gaming Mode
// (cpp/linux/README.md has the table). The right stick is the pointer, and
// the right trackpad with it, as that layout makes the trackpad the right
// stick; the left stick slides the map. The touchscreen and a trackpad set
// to the mouse go on working beside them.
static const double kPointerSpeed =
        1.2;  ///< The frame's height a second, the stick all the way over.
static const double kSlideSpeed = 600;  ///< UI pixels a second, likewise.

static double last_x = 0, last_y = 0;  ///< The mouse, in window points, as last seen.
static std::vector<std::pair<int, int>>
        warps;                           ///< Where the mouse was put, its motion not yet back.
static double pad_x = 0, pad_y = 0;      ///< The pointer as the stick has it, in UI pixels.
static int pad_left = 0, pad_right = 0;  ///< The controls holding each mouse button down.
static int typing_now = 0;  ///< Text input: 0 off, 1 on, 2 on with the Deck's keyboard up.

/**
 * @brief Moves the pointer to UI point (`x`, `y`), dragging the map if a drag is active.
 *
 * Also warps the OS mouse cursor so a trackpad or mouse continues from there.
 */
static void PointTo(double x, double y) {
    x = std::clamp(x, 0.0, display::state.w - 0.001);
    y = std::clamp(y, 0.0, display::state.h - 0.001);
    double dx = x - pad_x, dy = y - pad_y;
    pad_x = x;
    pad_y = y;
    display::state.mouse_inside = true;
    display::state.off.reset();
    Mousemoved(dx, dy);
    if (static_cast<int>(x) == g_app.mouse_x && static_cast<int>(y) == g_app.mouse_y) return;
    g_app.mouse_x = static_cast<int>(x);
    g_app.mouse_y = static_cast<int>(y);
    SDL_Window* win = display::Window();
    if (win && SDL_GetKeyboardFocus() == win) {
        auto [win_x, win_y] = display::FromUi(g_app.mouse_x + 0.5, g_app.mouse_y + 0.5);
        last_x = std::round(win_x);
        last_y = std::round(win_y);
        if (warps.size() >= 16) warps.erase(warps.begin());
        warps.push_back({static_cast<int>(last_x), static_cast<int>(last_y)});
        SDL_WarpMouseInWindow(win, static_cast<int>(last_x), static_cast<int>(last_y));
    }
}

/** @brief Presses or releases mouse `button` while any gamepad control for it is held. */
static void PadMouse(int& held, int button, bool down) {
    if (down && held++ == 0)
        Mousepressed(g_app.mouse_x, g_app.mouse_y, button);
    else if (!down && held > 0 && --held == 0)
        Mousereleased(g_app.mouse_x, g_app.mouse_y, button);
}

/** @brief Presses or releases virtual key `key` from a gamepad button event. */
static void PadKey(const std::string& key, bool down) {
    if (!down) {
        keys::Release(key);
        return;
    }
    keys::Press(key);
    Keypressed(key);
}

/** @brief Translates a gamepad button press/release (`button`, `down`) into mouse or key actions.
 */
void PadButton(int button, bool down) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
    case pad::kRightTrigger:
        PadMouse(pad_left, 1, down);
        break;
    case SDL_CONTROLLER_BUTTON_X:
    case pad::kLeftTrigger:
        PadMouse(pad_right, 2, down);
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
    // the D-pad steps the stack, as 8 2 4 6 do
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
    // held, as the keys are held for a Shift-click or an Alt-click
    case SDL_CONTROLLER_BUTTON_BACK:
        down ? keys::Press("lshift") : keys::Release("lshift");
        break;
    case SDL_CONTROLLER_BUTTON_START:
        down ? keys::Press("lalt") : keys::Release("lalt");
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        PadKey("space", down);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        PadKey("home", down);
        break;
    }
}

/** @brief Releases any held gamepad mouse buttons when the controller disconnects or focus is lost.
 */
void PadLetGo() {
    if (pad_left) Mousereleased(g_app.mouse_x, g_app.mouse_y, 1);
    if (pad_right) Mousereleased(g_app.mouse_x, g_app.mouse_y, 2);
    pad_left = pad_right = 0;
}

/** @brief Polls gamepad triggers and analog sticks once per frame to move the pointer and pan the
 * map. */
void PadUpdate() {
    static Uint64 then = SDL_GetPerformanceCounter();
    Uint64 now = SDL_GetPerformanceCounter();
    double delta_t = std::min(0.1, static_cast<double>(now - then) / SDL_GetPerformanceFrequency());
    then = now;
    int button;
    bool down;
    while (pad::Trigger(button, down)) PadButton(button, down);
    // the speed goes as the square of the push: slow for aiming, quick across
    auto sticks = pad::ReadSticks();
    if (sticks.rx != 0 || sticks.ry != 0) {
        // the mouse or a touch moved it since
        if (static_cast<int>(pad_x) != g_app.mouse_x || static_cast<int>(pad_y) != g_app.mouse_y) {
            pad_x = g_app.mouse_x + 0.5;
            pad_y = g_app.mouse_y + 0.5;
        }
        double step = kPointerSpeed * display::state.h * std::hypot(sticks.rx, sticks.ry) * delta_t;
        PointTo(pad_x + sticks.rx * step, pad_y + sticks.ry * step);
    }
    if ((sticks.lx != 0 || sticks.ly != 0) &&
        !(g_app.starting || !g_app.g || g_app.over || kit::Top() || g_app.banner || g_app.offer ||
          g_app.victory || g_app.drag)) {
        double slide = kSlideSpeed * std::hypot(sticks.lx, sticks.ly) * delta_t *
                       display::state.scale / (kTile * g_app.zoom);
        g_app.cx += sticks.lx * slide;
        g_app.cy += sticks.ly * slide;
        ClampCamera();
    }
}

/**
 * @brief Synchronizes SDL text input state with whether a text field is active.
 *
 * On the Steam Deck, SDL brings Steam's on-screen keyboard up with text input;
 * the hero's name field waits for a click before requesting the on-screen
 * keyboard.
 */
void SyncTyping() {
    if (!g_app.offer) offer_typing = false;
    auto top = kit::Top();
    int want = g_app.offer ? (offer_typing ? 2 : 1) : top && top->Typing() ? 2 : 0;
    if (want == typing_now) return;
    if (want == 0 || typing_now == 2) SDL_StopTextInput();
    typing_now = want;
    if (want == 0) return;
    // as SDL starts it at first: text, but no keyboard, unless asked for
    const char* hint = SDL_GetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    SDL_StartTextInput();
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, nullptr);
}

void InitPointerState(SDL_Window* win) {
    int mouse_x = 0;
    int mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    last_x = mouse_x;
    last_y = mouse_y;
    display::state.mouse_inside = (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS) != 0;
    display::state.off = display::OffFrame(mouse_x, mouse_y);
    std::tie(g_app.mouse_x, g_app.mouse_y) = display::ToUi(mouse_x, mouse_y);
    pad_x = g_app.mouse_x + 0.5;
    pad_y = g_app.mouse_y + 0.5;
}

void HandleMouseMotion(int win_x, int win_y) {
    auto warp_it = std::find(warps.begin(), warps.end(), std::make_pair(win_x, win_y));
    if (warp_it != warps.end()) {
        warps.erase(warps.begin(), warp_it + 1);
        return;
    }
    auto [x, y] = display::ToUi(win_x, win_y);
    g_app.mouse_x = x;
    g_app.mouse_y = y;
    display::state.mouse_inside = true;
    display::state.off = display::OffFrame(win_x, win_y);
    double scale = display::state.dpi / display::state.scale;
    Mousemoved((win_x - last_x) * scale, (win_y - last_y) * scale);
    last_x = win_x;
    last_y = win_y;
}

void PollFocusedMouse(SDL_Window* win) {
    if (HasScript() || SDL_GetKeyboardFocus() != win) return;
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
        auto [x, y] = display::ToUi(mouse_x, mouse_y);
        g_app.mouse_x = x;
        g_app.mouse_y = y;
        display::state.mouse_inside = true;
        display::state.off = display::OffFrame(mouse_x, mouse_y);
        double scale = display::state.dpi / display::state.scale;
        Mousemoved((mouse_x - last_x) * scale, (mouse_y - last_y) * scale);
        last_x = mouse_x;
        last_y = mouse_y;
    }
}

}  // namespace front
