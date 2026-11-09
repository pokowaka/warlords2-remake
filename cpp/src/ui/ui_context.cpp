#include "ui/ui_context.hpp"

namespace w2 {

UiContext::UiContext(display::DisplayWindow& display, gfx::Renderer& renderer,
                     sound::AudioEngine& audio, keys::KeyboardState& keyboard,
                     pad::GamepadInput& gamepad, PreferencesStore& prefs, AssetCatalog& assets,
                     Viewport& viewport, ModalStack& modals, IScreenActions& actions)
        : display_(display)
        , renderer_(renderer)
        , audio_(audio)
        , keyboard_(keyboard)
        , gamepad_(gamepad)
        , prefs_(prefs)
        , assets_(assets)
        , viewport_(viewport)
        , modals_(modals)
        , actions_(actions) {}

void UiContext::SavePref(std::string_view key, std::string_view value) {
    prefs_.Set(key, value);
    prefs_.Save().IgnoreError();
}

void UiContext::SavePref(std::string_view key, int value) {
    prefs_.Set(key, value);
    prefs_.Save().IgnoreError();
}

const screen::Art* UiContext::ControlArt(int bitmap_id) {
    screen::Screen* scr = assets_.MainScreenPtr();
    return scr ? scr->ArtFor(renderer_, bitmap_id) : nullptr;
}

UiContext::UiCoordPair UiContext::MapToUi(double tile_x, double tile_y) const {
    return viewport_.MapToUi(tile_x, tile_y, display_.GetState().scale);
}

UiContext::TileCoordPair UiContext::ViewCentre() const {
    return viewport_.ViewCentre(display_.GetState().scale);
}

void UiContext::CentreOn(int x, int y) {
    viewport_.CentreOn(x, y, game_, display_.GetState().scale);
}

}  // namespace w2
