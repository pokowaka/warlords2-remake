#include "platform/pad.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include <SDL.h>

namespace pad {

namespace {
/**
 * @brief State for an open `SDL_GameController` and its trigger hysteresis.
 */
struct Pad {
    SDL_GameController* controller = nullptr;  ///< Underlying SDL game controller handle.
    bool left_trig = false;                    ///< True while left trigger is held past threshold.
    bool right_trig = false;                   ///< True while right trigger is held past threshold.
};
std::map<SDL_JoystickID, Pad> pads;
std::optional<Sticks> scripted;

const double kDead = 0.15;               // Radial thumbstick dead zone.
const double kPull = 0.5, kLetGo = 0.3;  // Trigger press and release thresholds.

/**
 * @brief Reads one thumbstick, removes its radial dead zone, and rescales the
 * remaining range smoothly over `0..1`.
 */
void Stick(SDL_GameController* controller, SDL_GameControllerAxis axis_x,
           SDL_GameControllerAxis axis_y, double& x, double& y) {
    double stick_x = SDL_GameControllerGetAxis(controller, axis_x) / 32767.0;
    double stick_y = SDL_GameControllerGetAxis(controller, axis_y) / 32767.0;
    double mag = std::hypot(stick_x, stick_y);
    if (mag <= kDead) return;
    double scale = std::min(1.0, (mag - kDead) / (1 - kDead)) / mag;
    x += stick_x * scale;
    y += stick_y * scale;
}
}  // namespace

void Hints() {
    // Disable SDL's direct Steam Deck HID driver so Steam Input's virtual pad
    // and desktop trackpad mouse mode both work as expected.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "0");
    // Use physical button positions (bottom face button is always A).
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Pressing Enter on the Steam Deck on-screen keyboard dismisses it.
    SDL_SetHint(SDL_HINT_RETURN_KEY_HIDES_IME, "1");
}

bool TranslateEvent(const SDL_Event& event, int& button, bool& down) {
    switch (event.type) {
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which);
        if (controller)
            pads[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))] =
                    Pad{controller};
        return false;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {
        auto pad_it = pads.find(event.cdevice.which);
        if (pad_it != pads.end()) {
            SDL_GameControllerClose(pad_it->second.controller);
            pads.erase(pad_it);
        }
        return false;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        button = event.cbutton.button;
        down = event.type == SDL_CONTROLLERBUTTONDOWN;
        return true;
    }
    return false;
}

bool Trigger(int& button, bool& down) {
    for (auto& [joy_id, pad_entry] : pads) {
        (void)joy_id;
        for (int trig = 0; trig < 2; trig++) {
            bool& was_down = trig == 0 ? pad_entry.left_trig : pad_entry.right_trig;
            double val = SDL_GameControllerGetAxis(pad_entry.controller,
                                                   trig == 0 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
                                                             : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) /
                         32767.0;
            if (was_down ? val < kLetGo : val > kPull) {
                was_down = !was_down;
                button = trig == 0 ? kLeftTrigger : kRightTrigger;
                down = was_down;
                return true;
            }
        }
    }
    return false;
}

Sticks ReadSticks() {
    if (scripted) return *scripted;
    Sticks sticks;
    for (const auto& [joy_id, pad_entry] : pads) {
        (void)joy_id;
        Stick(pad_entry.controller, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, sticks.lx,
              sticks.ly);
        Stick(pad_entry.controller, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
              sticks.rx, sticks.ry);
    }
    sticks.lx = std::clamp(sticks.lx, -1.0, 1.0);
    sticks.ly = std::clamp(sticks.ly, -1.0, 1.0);
    sticks.rx = std::clamp(sticks.rx, -1.0, 1.0);
    sticks.ry = std::clamp(sticks.ry, -1.0, 1.0);
    return sticks;
}

void Script(const Sticks& sticks) {
    if (sticks.lx == 0 && sticks.ly == 0 && sticks.rx == 0 && sticks.ry == 0)
        scripted.reset();
    else
        scripted = sticks;
}

int FromName(const std::string& name) {
    if (name == "lefttrigger") return kLeftTrigger;
    if (name == "righttrigger") return kRightTrigger;
    return SDL_GameControllerGetButtonFromString(name.c_str());
}

}  // namespace pad
