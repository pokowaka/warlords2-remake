/**
 * @file
 * @brief Abstract interface (`w2::IScreenActions`) exposing main-screen commands and shared
 * strategic-map renderers to modal dialogs.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace w2 {

struct Battle;
class City;
class Game;
struct Site;

/**
 * @brief Main-screen commands and shared map-panel renderers invoked by modal dialogs.
 *
 * Implemented by `MapScreenController` and accessed via `UiContext::Actions()` whenever a modal
 * dialog transitions screens, adopts a loaded game, initiates combat, or draws a strategic map.
 */
class IScreenActions {
  public:
    using OwnerOverrideList = std::vector<int>;

    virtual ~IScreenActions() = default;

    /** @brief Opens the city inspection/production dialog for `city`. */
    virtual void OpenCity(City* city) = 0;

    /** @brief Opens the active quest status dialog. */
    virtual void OpenQuest() = 0;

    /** @brief Transitions back to the title / scenario selection screens. */
    virtual void OpenStart() = 0;

    /** @brief Adopts `game` as the active loaded session and centres on the current side's capital.
     */
    virtual void TakeLoaded(std::unique_ptr<Game> game) = 0;

    /** @brief Begins visual playback of decided combat `result` at tile `(x, y)`. */
    virtual void StartAssault(int x, int y, const Battle& result) = 0;

    /** @brief Opens the city-captured spoils modal (`Occupy`/`Pillage`/`Sack`/`Raze`) for `city`.
     */
    virtual void PresentVictory(City* city, const std::string& victor) = 0;

    /** @brief Selects the current human player's stack on tile `(x, y)`, if any. */
    virtual void SelectAt(int x, int y) = 0;

    /** @brief Commits bottom-bar army-slot changes to the active stack selection. */
    virtual void AfterSlotChange() = 0;

    /** @brief Starts executing computer turns from `Game::CurrentSide()` onward. */
    virtual void PlayComputer() = 0;

    /** @brief Displays `message` in the main screen's status bar. */
    virtual void Say(const std::string& message) = 0;

    /** @brief Aborts any active turn/modals and returns to the start screens. */
    virtual void Quit() = 0;

    /** @brief Rolls a non-gameplay UI die in `1..n` without advancing `Game::GetRng()`. */
    virtual int Roll(int n) = 0;

    /** @brief Returns true when menu item `menu_key` is currently selectable. */
    virtual bool MenuEnabled(const std::string& menu_key) = 0;

    /** @brief Draws the 224x312 strategic map at `(x, y)` with optional marked city and overrides.
     */
    virtual void DrawStrategicMap(int x, int y, const City* mark = nullptr, bool no_cities = false,
                                  const OwnerOverrideList* owners = nullptr, int scroll = 0,
                                  int rows = 312) = 0;

    /** @brief Draws explored ruin/temple site markers onto a strategic map at `(x, y)`. */
    virtual void DrawSiteMarkers(int x, int y, const Site* highlight = nullptr) = 0;

    /** @brief Draws the city vectoring map at `(x, y)` for `city` or all cities (`see_all`). */
    virtual void DrawVectorMap(int x, int y, City* city, int filter, bool see_all) = 0;

    /** @brief Draws a hero figure icon on a strategic map at `(x, y)` for tile `(tile_x, tile_y)`.
     */
    virtual void DrawHeroFigure(int x, int y, int tile_x, int tile_y, bool dark = false) = 0;

    /** @brief Draws the strategic-map panel at `(x, y)` with `city` highlighted. */
    virtual void DrawStrategicPanel(int x, int y, const City* city) = 0;
};

}  // namespace w2
