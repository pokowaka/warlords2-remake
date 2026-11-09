#include "platform/pad.hpp"

#include <algorithm>
#include <cmath>

#include <SDL.h>

namespace pad {

namespace {

constexpr double kDeadZone = 0.15;
constexpr double kPullThreshold = 0.5;
constexpr double kReleaseThreshold = 0.3;

/** @brief Reads one thumbstick, removes its radial dead zone, and rescales over `0..1`. */
void AccumulateStick(SDL_GameController* controller, SDL_GameControllerAxis axis_x,
                     SDL_GameControllerAxis axis_y, double& x, double& y) {
    double stick_x = SDL_GameControllerGetAxis(controller, axis_x) / 32767.0;
    double stick_y = SDL_GameControllerGetAxis(controller, axis_y) / 32767.0;
    double mag = std::hypot(stick_x, stick_y);
    if (mag <= kDeadZone) return;
    double scale = std::min(1.0, (mag - kDeadZone) / (1.0 - kDeadZone)) / mag;
    x += stick_x * scale;
    y += stick_y * scale;
}

}  // namespace

GamepadInput::~GamepadInput() {
    Shutdown();
}

void GamepadInput::Shutdown() {
    for (auto& [joy_id, slot] : controllers_) {
        (void)joy_id;
        if (slot.controller != nullptr) {
            SDL_GameControllerClose(slot.controller);
        }
    }
    controllers_.clear();
    scripted_sticks_.reset();
    pending_warps_.clear();
    cursor_x_ = 0.0;
    cursor_y_ = 0.0;
    last_window_x_ = 0.0;
    last_window_y_ = 0.0;
}

void GamepadInput::ApplyHints() {
    // Disable SDL's direct Steam Deck HID driver so Steam Input's virtual pad
    // and desktop trackpad mouse mode both work as expected.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "0");
    // Use physical button positions (bottom face button is always A).
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Pressing Enter on the Steam Deck on-screen keyboard dismisses it.
    SDL_SetHint(SDL_HINT_RETURN_KEY_HIDES_IME, "1");
}

int GamepadInput::ButtonFromName(std::string_view name) {
    if (name == "lefttrigger") return kLeftTrigger;
    if (name == "righttrigger") return kRightTrigger;
    std::string name_str(name);
    return SDL_GameControllerGetButtonFromString(name_str.c_str());
}

bool GamepadInput::TranslateEvent(const SDL_Event& event, int& button, bool& down) {
    switch (event.type) {
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which);
        if (controller != nullptr) {
            int32_t joy_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
            auto existing = controllers_.find(joy_id);
            if (existing != controllers_.end() && existing->second.controller != nullptr) {
                SDL_GameControllerClose(existing->second.controller);
            }
            controllers_[joy_id] = ControllerSlot{controller, false, false};
        }
        return false;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {
        auto pad_it = controllers_.find(event.cdevice.which);
        if (pad_it != controllers_.end()) {
            SDL_GameControllerClose(pad_it->second.controller);
            controllers_.erase(pad_it);
        }
        return false;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        button = event.cbutton.button;
        down = event.type == SDL_CONTROLLERBUTTONDOWN;
        return true;
    default:
        break;
    }
    return false;
}

bool GamepadInput::PollTrigger(int& button, bool& down) {
    for (auto& [joy_id, slot] : controllers_) {
        (void)joy_id;
        for (int trig = 0; trig < 2; trig++) {
            bool& was_down = trig == 0 ? slot.left_trig : slot.right_trig;
            SDL_GameControllerAxis axis =
                    trig == 0 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            double val = SDL_GameControllerGetAxis(slot.controller, axis) / 32767.0;
            if (was_down ? val < kReleaseThreshold : val > kPullThreshold) {
                was_down = !was_down;
                button = trig == 0 ? kLeftTrigger : kRightTrigger;
                down = was_down;
                return true;
            }
        }
    }
    return false;
}

Sticks GamepadInput::ReadSticks() const {
    if (scripted_sticks_.has_value()) return *scripted_sticks_;
    Sticks sticks;
    for (const auto& [joy_id, slot] : controllers_) {
        (void)joy_id;
        AccumulateStick(slot.controller, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
                        sticks.lx, sticks.ly);
        AccumulateStick(slot.controller, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
                        sticks.rx, sticks.ry);
    }
    sticks.lx = std::clamp(sticks.lx, -1.0, 1.0);
    sticks.ly = std::clamp(sticks.ly, -1.0, 1.0);
    sticks.rx = std::clamp(sticks.rx, -1.0, 1.0);
    sticks.ry = std::clamp(sticks.ry, -1.0, 1.0);
    return sticks;
}

void GamepadInput::SetScriptedSticks(const Sticks& sticks) {
    if (sticks.lx == 0.0 && sticks.ly == 0.0 && sticks.rx == 0.0 && sticks.ry == 0.0) {
        scripted_sticks_.reset();
    } else {
        scripted_sticks_ = sticks;
    }
}

void GamepadInput::RecordWarp(int win_x, int win_y) {
    if (pending_warps_.size() >= kMaxPendingWarps) {
        pending_warps_.erase(pending_warps_.begin());
    }
    pending_warps_.emplace_back(win_x, win_y);
}

bool GamepadInput::ConsumeWarp(int win_x, int win_y) {
    auto found = std::find(pending_warps_.begin(), pending_warps_.end(), WindowWarp{win_x, win_y});
    if (found == pending_warps_.end()) return false;
    pending_warps_.erase(pending_warps_.begin(), found + 1);
    return true;
}

}  // namespace pad
