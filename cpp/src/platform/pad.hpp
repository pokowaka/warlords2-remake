// Game controller input via SDL's GameController API, supporting the Steam
// Deck (via Steam Input's virtual Xbox controller) and external gamepads.
//
// Reads controller buttons by physical position, converts analog triggers into
// digital button presses, and applies radial dead zones to thumbsticks.
#pragma once

#include <string>

union SDL_Event;

namespace pad {

/// Synthetic button IDs assigned to the analog triggers after SDL_GameControllerButton.
constexpr int kLeftTrigger = 100, kRightTrigger = 101;

/** @brief Sets SDL controller hints before `SDL_Init`. */
void Hints();

/**
 * @brief Processes an SDL controller hotplug or button event.
 * @return True when `button` and `down` were populated with a button press/release.
 */
bool TranslateEvent(const SDL_Event& event, int& button, bool& down);
/** @brief Polls analog triggers once per frame, emitting a press/release on threshold crossings. */
bool Trigger(int& button, bool& down);

/**
 * @brief Combined normalized thumbstick deflection across connected controllers.
 */
struct Sticks {
    double lx = 0;  ///< Left stick horizontal axis (-1..1, 0 inside dead zone).
    double ly = 0;  ///< Left stick vertical axis (-1..1, 0 inside dead zone).
    double rx = 0;  ///< Right stick horizontal axis (-1..1, 0 inside dead zone).
    double ry = 0;  ///< Right stick vertical axis (-1..1, 0 inside dead zone).
};
/** @brief Reads the combined thumbstick positions across all connected controllers. */
Sticks ReadSticks();
/** @brief Overrides thumbstick input from a test script (`stick LX LY RX RY`), or clears when all
 * 0. */
void Script(const Sticks& sticks);

/**
 * @brief Maps an SDL button name ("a", "dpup", "leftshoulder", "lefttrigger",
 * "righttrigger") to its button ID, or -1 if unknown.
 */
int FromName(const std::string& name);

}  // namespace pad
