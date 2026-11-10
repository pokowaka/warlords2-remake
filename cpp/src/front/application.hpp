/**
 * @file
 * @brief Top-level RAII composition root (`w2::Application`) owning SDL2, platform subsystems,
 * `AssetCatalog`, `Viewport`, `ModalStack`, `MapScreenController`, `UiContext`, and `ScriptPlayer`.
 */
#pragma once

#include <filesystem>
#include <string>

#include "config/preferences_store.hpp"
#include "front/map_screen_controller.hpp"
#include "front/script_player.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/asset_catalog.hpp"
#include "ui/modal_stack.hpp"
#include "ui/ui_context.hpp"
#include "ui/viewport.hpp"

namespace w2 {

/**
 * @brief Parsed CLI configuration passed from `main()` into `Application::Run`.
 */
struct ApplicationConfig {
    std::filesystem::path data_dir = "original";  ///< Root Warlords II game data directory.
    std::string scenario;                ///< Optional scenario directory to launch directly.
    std::string script_file;             ///< Optional automated UI test script path.
    std::string weights_file;            ///< Optional Modern AI weights JSON path.
    double seed = 0.0;                   ///< Optional RNG seed for `--scenario`.
    std::string window_size;             ///< Optional `WxH` window size override.
    bool force_window = false;           ///< True when `--window` was passed.
    bool force_fullscreen = false;       ///< True when `--fullscreen` was passed.
    bool force_classic_ai = false;       ///< True when `--classic-ai` was passed.
    bool force_modern_ai = false;        ///< True when `--modern-ai` was passed.
    std::string mapgen;                  ///< Optional `--mapgen` mode (`classic`/`continents`/...).
    bool modern_ui = false;              ///< True when `--modern-ui` overlays are active.
    bool force_diplomacy = false;        ///< True when `--diplomacy` was passed.
    bool force_hidden_map = false;       ///< True when `--hidden-map` was passed.
    int military_advisor_override = -1;  ///< `1` force on, `0` force off, `-1` default.
    bool spectate_all_ai = false;        ///< True when `--spectate` / `--all-ai` is active.
    int force_personalities = -1;        ///< `1` force on, `0` force off, `-1` weights default.
    bool warlord_lookahead = false;      ///< True when `--warlord_lookahead` is active.
};

/**
 * @brief Composition root owning all runtime subsystems on the stack and driving the SDL2 main
 * loop.
 *
 * Initializes SDL, `PreferencesStore`, `DisplayWindow`, `gfx::Renderer`, `AssetCatalog`,
 * `sound::AudioEngine`, `Viewport`, `ModalStack`, `MapScreenController`, `UiContext`, and
 * `ScriptPlayer` in dependency order and tears them down deterministically in reverse order.
 */
class Application {
  public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * @brief Initializes SDL, loads assets, enters the start screen or CLI scenario, and runs the
     * frame loop until quit.
     * @return Process exit code (`0` on clean exit, `1` on startup error).
     */
    int Execute(const ApplicationConfig& config);

    /** @brief Advances one frame (polls SDL events, updates UI state, and presents the frame). */
    void Tick();

    /** @brief Convenience entry point constructing an `Application` on the stack and running it. */
    static int Run(const ApplicationConfig& config);

  private:
    void PumpEvents();

    bool sdl_initialized_ = false;    ///< True once `SDL_Init` succeeded.
    PreferencesStore prefs_;          ///< User preferences persisted as `w2-prefs.json`.
    display::DisplayWindow display_;  ///< SDL2 window, logical frame, and CRT pass.
    gfx::Renderer renderer_;          ///< Immediate-mode 2D renderer.
    sound::AudioEngine audio_;        ///< Music, digital `.8SN` effects, and speech engine.
    keys::KeyboardState keyboard_;    ///< Depressed LÖVE-named key tracker.
    pad::GamepadInput gamepad_;       ///< Gamepad buttons, sticks, and cursor warps.
    AssetCatalog assets_;             ///< Palette, `.PCK` textures, `Screen`, and fonts.
    Viewport viewport_;               ///< Map camera, zoom, and placed screen rects.
    ModalStack modals_;               ///< Active stack of `kit::Modal` dialogs.
    ScriptPlayer script_player_;      ///< Automated `--script` test driver.
    MapScreenController controller_;  ///< Main map screen controller and `IScreenActions`.
    UiContext ui_context_;            ///< Non-owning DI bundle passed to modals and views.
};

}  // namespace w2
