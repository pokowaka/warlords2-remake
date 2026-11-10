/**
 * @file
 * @brief Dependency-injection bundle (`w2::UiContext`) aggregating platform subsystems,
 * `AssetCatalog`, `Viewport`, `ModalStack`, `IScreenActions`, and active session state for modal
 * dialogs and UI views.
 */
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"

#include "config/preferences_store.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/asset_catalog.hpp"
#include "ui/modal_stack.hpp"
#include "ui/screen_actions.hpp"
#include "ui/viewport.hpp"
#include "warlords/scenario_source.hpp"
#include "warlords/slots.hpp"
#include "warlords/types.hpp"

namespace w2 {

/**
 * @brief Active army stack picked up on a map tile and its 8-slot grouping state.
 */
struct Selection {
    int x = 0;                 ///< Tile x coordinate where the selected stack stands.
    int y = 0;                 ///< Tile y coordinate where the selected stack stands.
    slots::Slots slots;        ///< Bottom-bar slot assignments and move-group flags.
    std::vector<Army*> stack;  ///< Subset of armies currently selected to move together.
};

/**
 * @brief Session-persisted Random World generator sliders and new-game option presets used by the
 * start screens (`ui/start.cpp`).
 */
struct StartWorldSettings {
    bool enabled = false;                               ///< True when Random World is selected.
    int terrain_set = 0;                                ///< Selected terrain set index.
    bool allies = false;                                ///< True when cities can produce allies.
    bool modern_mapgen = false;                         ///< True when using modern `scengen`.
    int shape = 0;                                      ///< Landmass layout (`0`=Continents, `1`=Pangaea, `2`=Islands).
    std::array<int, 4> sliders{{3, 3, 2, 3}};           ///< Water, Hills, Cities, Forest (`0..6`).
    std::array<bool, 4> set{{true, true, true, true}};  ///< False (`?`) leaves slider random.
    ScenarioFiles generated_files;                      ///< Last generated in-memory world.
    std::array<int, 10> remembered_options{{0, 0, 0, 0, 1, 0, 0, 0, 1, 0}};  ///< Setup options.
};

/**
 * @brief Non-owning subsystem reference bundle and active UI session context passed into
 * `kit::Modal` subclasses and `kit` drawing helpers.
 *
 * Provides modal dialogs and UI primitives with unified access to platform services (`Display()`,
 * `Renderer()`, `Audio()`, `Keyboard()`, `Gamepad()`, `Prefs()`), UI infrastructure (`Assets()`,
 * `GetViewport()`, `Modals()`, `Actions()`), and active game/turn state (`ActiveGame()`,
 * `Player()`, `ActiveSelection()`).
 *
 * Ownership & lifetimes: All referenced subsystems (`display_`, `renderer_`, `audio_`, `keyboard_`,
 * `gamepad_`, `prefs_`, `assets_`, `viewport_`, `modals_`, `actions_`) must outlive this
 * `UiContext` and every `kit::Modal` pushed onto `modals_`.
 *
 * Thread safety: Not thread-safe; accessed exclusively on the main UI thread.
 */
class UiContext {
  public:
    using UiCoordPair = std::pair<double, double>;
    using TileCoordPair = std::pair<int, int>;

    UiContext(display::DisplayWindow& display, gfx::Renderer& renderer, sound::AudioEngine& audio,
              keys::KeyboardState& keyboard, pad::GamepadInput& gamepad, PreferencesStore& prefs,
              AssetCatalog& assets, Viewport& viewport, ModalStack& modals,
              IScreenActions& actions);
    ~UiContext() = default;

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    display::DisplayWindow& Display() { return display_; }
    const display::DisplayWindow& Display() const { return display_; }

    gfx::Renderer& Renderer() { return renderer_; }
    const gfx::Renderer& Renderer() const { return renderer_; }

    sound::AudioEngine& Audio() { return audio_; }
    const sound::AudioEngine& Audio() const { return audio_; }

    keys::KeyboardState& Keyboard() { return keyboard_; }
    const keys::KeyboardState& Keyboard() const { return keyboard_; }

    pad::GamepadInput& Gamepad() { return gamepad_; }
    const pad::GamepadInput& Gamepad() const { return gamepad_; }

    PreferencesStore& Prefs() { return prefs_; }
    const PreferencesStore& Prefs() const { return prefs_; }

    AssetCatalog& Assets() { return assets_; }
    const AssetCatalog& Assets() const { return assets_; }

    Viewport& GetViewport() { return viewport_; }
    const Viewport& GetViewport() const { return viewport_; }

    ModalStack& Modals() { return modals_; }
    const ModalStack& Modals() const { return modals_; }

    IScreenActions& Actions() { return actions_; }
    const IScreenActions& Actions() const { return actions_; }

    /** @brief Updates `key` in `prefs_` and writes `w2-prefs.json` immediately. */
    void SavePref(std::string_view key, std::string_view value);
    /** @brief Updates integer `key` in `prefs_` and writes `w2-prefs.json` immediately. */
    void SavePref(std::string_view key, int value);

    /** @brief Looks up (lazily decoding onto `renderer_`) control bitmap `bitmap_id`. */
    const screen::Art* ControlArt(int bitmap_id);

    /** @brief Converts fractional map tile `(tile_x, tile_y)` to logical UI pixels. */
    UiCoordPair MapToUi(double tile_x, double tile_y) const;
    /** @brief Returns the integral map tile at the centre of the viewport. */
    TileCoordPair ViewCentre() const;
    /** @brief Centres the camera on map tile `(x, y)` clamped to the active map. */
    void CentreOn(int x, int y);

    Game* GamePtr() const { return game_; }
    Game& ActiveGame() const {
        ABSL_DCHECK(game_ != nullptr);
        return *game_;
    }
    void SetGame(Game* game) { game_ = game; }

    Side* Player() const { return player_; }
    void SetPlayer(Side* player) { player_ = player; }

    Selection* ActiveSelection() { return selection_ ? &*selection_ : nullptr; }
    const Selection* ActiveSelection() const { return selection_ ? &*selection_ : nullptr; }
    void SetSelection(Selection sel) { selection_ = std::move(sel); }
    void ClearSelection() { selection_.reset(); }

    const std::string& Scenario() const { return scenario_; }
    void SetScenario(std::string scenario) { scenario_ = std::move(scenario); }

    bool Starting() const { return starting_; }
    void SetStarting(bool starting) { starting_ = starting; }

    bool ModernUi() const { return modern_ui_; }
    void SetModernUi(bool enabled) { modern_ui_ = enabled; }

    bool VectorSeeAll() const { return vector_see_all_; }
    void SetVectorSeeAll(bool enabled) { vector_see_all_ = enabled; }

    int OptMilitaryAdvisor() const { return opt_military_advisor_; }
    void SetOptMilitaryAdvisor(int value) { opt_military_advisor_ = value; }

    int LastSaveSlot() const { return last_save_slot_; }
    void SetLastSaveSlot(int slot) { last_save_slot_ = slot; }

    StartWorldSettings& StartWorld() { return start_world_; }
    const StartWorldSettings& StartWorld() const { return start_world_; }

    bool QuitRequested() const { return quit_requested_; }
    void RequestQuit() { quit_requested_ = true; }

  private:
    display::DisplayWindow& display_;     ///< Window and logical UI scaling manager.
    gfx::Renderer& renderer_;             ///< Immediate-mode 2D renderer.
    sound::AudioEngine& audio_;           ///< Music, effects, and speech engine.
    keys::KeyboardState& keyboard_;       ///< Depressed LÖVE-named key tracker.
    pad::GamepadInput& gamepad_;          ///< Gamepad buttons, sticks, and cursor warps.
    PreferencesStore& prefs_;             ///< Persistent JSON user settings store.
    AssetCatalog& assets_;                ///< Loaded `.PCK` sheets, screen layout, and fonts.
    Viewport& viewport_;                  ///< Camera position, zoom, and placed layout rects.
    ModalStack& modals_;                  ///< Active stack of open `kit::Modal` dialogs.
    IScreenActions& actions_;             ///< Main-screen action and map-panel callbacks.
    Game* game_ = nullptr;                ///< Active game session, or `nullptr` before start.
    Side* player_ = nullptr;              ///< Active human player faction.
    std::optional<Selection> selection_;  ///< Currently picked-up army stack, if any.
    std::string scenario_ = "ERYTHEA";    ///< Active or last-selected scenario directory.
    bool starting_ = false;               ///< True while title/scenario setup screens are open.
    bool modern_ui_ = false;              ///< True when `--modern-ui` overlays are enabled.
    bool vector_see_all_ = false;         ///< Session toggle for City > Vector "See All".
    int opt_military_advisor_ = -1;       ///< CLI override for Military Advisor (`-1` = none).
    int last_save_slot_ = 0;              ///< Most recently saved or loaded slot index (`0..9`).
    StartWorldSettings start_world_{};    ///< Session Random World and setup option settings.
    bool quit_requested_ = false;         ///< Latched when the user exits the application.
};

}  // namespace w2
