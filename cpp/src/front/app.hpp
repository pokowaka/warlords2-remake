/**
 * @file
 * @brief The front end's state, shared with the dialogs: the Lua remake's g_app.
 *
 * main.cpp is the main screen (love2d/main.lua); the dialogs live in ui/ and
 * reach the screen through the functions declared here.
 */
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/menu.hpp"
#include "front/screen.hpp"
#include "platform/coroutine.hpp"
#include "platform/gfx.hpp"
#include "warlords/combat.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/pal.hpp"
#include "warlords/slots.hpp"
#include "warlords/types.hpp"

namespace kit {
struct Modal;
}

/** @brief The stack the player has picked up, and where. */
struct Selection {
    int x = 0;                     ///< The tile the stack stands on, across.
    int y = 0;                     ///< The tile the stack stands on, down.
    w2::slots::Slots slots;        ///< The bottom bar's slots: which armies go along.
    std::vector<w2::Army*> stack;  ///< The armies that move together.
};

/** @brief A stack walking its route, one tile at a time, as the player watches. */
struct Walk {
    std::set<w2::Army*> armies;              ///< The armies walking.
    std::vector<std::pair<int, int>> tiles;  ///< The tiles walked, start first.
    size_t i = 0;                            ///< The tile reached so far.
    double at = 0;                           ///< When the last step was taken.
    w2::Army* top = nullptr;                 ///< The army whose figure is drawn.
    bool computer = false;                   ///< A computer side's move.
    bool sea_before = false;                 ///< At sea as it set off: shown so to the walk's end.
    std::vector<w2::Army*> put_down;  ///< Out of moves: put down once the walk has played out.
};

/** @brief The start-of-turn banner: whose turn it is, in their colours. */
struct Banner {
    std::string name;  ///< The side's name.
    int turn = 0;      ///< The turn number.
    int colour = 15;   ///< The side's colour.
    int edge = 0;      ///< The side's edge colour, for the frame.
};

/** @brief An army falling in a battle: when it was hit, and how long it burns. */
struct Fell {
    double at = 0;    ///< When it was hit.
    double burn = 0;  ///< How long the blast shows.
};

/** @brief Playback stage of an animated map/window battle (`Assault`). */
enum class AssaultPhase {
    kCloud,   ///< Initial war-cry cloud over the assaulted tile.
    kBattle,  ///< Casualty-by-casualty combat playback in the battle window.
    kOver,    ///< Outcome summary displayed while awaiting dismissal or auto-close.
};

/** @brief A battle being shown: the cloud, the armies falling one by one, the result. */
struct Assault {
    int x = 0;                                   ///< The tile fought over, across.
    int y = 0;                                   ///< The tile fought over, down.
    w2::Battle result;                           ///< The battle, already fought.
    std::vector<w2::Army*> def;                  ///< The defenders, in line order.
    std::vector<w2::Army*> atk;                  ///< The attackers, in line order.
    std::vector<std::pair<int, int>> def_slots;  ///< Each defender's place on the board.
    std::vector<std::pair<int, int>> atk_slots;  ///< Each attacker's place on the board.
    int def_side = 8;                            ///< The defenders' side (8: neutral).
    int atk_side = 8;                            ///< The attackers' side.
    size_t step = 0;                             ///< Rounds of the battle shown so far.
    int def_down = 0;                            ///< Defenders fallen so far.
    int atk_down = 0;                            ///< Attackers fallen so far.
    std::vector<Fell> def_fell;                  ///< Each fallen defender's blast.
    std::vector<Fell> atk_fell;                  ///< Each fallen attacker's blast.
    AssaultPhase phase = AssaultPhase::kCloud;   ///< Current playback stage.
    double at = 0;                               ///< When the phase or the last round began.
    double cloud_time = 0;                       ///< How long the war cry lasts.
    double close_at = 0;                         ///< When a computer's battle closes itself.
    bool fast = false;                           ///< Played quickly: the player skipped it.
    bool computer = false;                       ///< A computer side's attack.
    bool window = false;                         ///< Shown in the battle window.
    std::string victor;                          ///< The winner's name.
    std::string outcome;                         ///< One line saying how it ended.
    std::vector<std::string> message;            ///< The result's lines.
    std::function<void()> after;                 ///< Runs once the battle is closed.
};

/** @brief A city just taken: the dialog that asks what to do with it. */
struct Victory {
    w2::City* city = nullptr;  ///< The city taken.
    std::string who;           ///< Who took it.
    std::string where;         ///< The city's name, as told.
    int enter = 285;  ///< What Enter and Escape choose: Occupy (285), or what a quest wants.
};

/** @brief What a computer side is doing, shown while it plays. */
struct AiStatus {
    w2::Side* side = nullptr;  ///< The side playing.
    std::string text;          ///< What it is doing.
    int progress = 0;          ///< How far through its turn it is.
};

/** @brief The map being dragged with the mouse. */
struct Drag {
    double cx = 0;  ///< The camera's tile across when the drag began.
    double cy = 0;  ///< The camera's tile down when the drag began.
    double dx = 0;  ///< How far the mouse has moved across since.
    double dy = 0;  ///< How far the mouse has moved down since.
};

/** @brief Everything the front end keeps: the art, the screen, the game and what is on it. */
struct App {
    std::vector<std::shared_ptr<kit::Modal>> modals;  ///< The dialogs open, topmost last.
    int mouse_x = -1;                ///< The pointer, in UI pixels; -1 before it moves.
    int mouse_y = -1;                ///< The pointer, in UI pixels; -1 before it moves.
    w2::pal::Palette palette;        ///< The game's palette.
    std::filesystem::path data_dir;  ///< Where the game's DATA folder is.

    // the art
    std::array<gfx::ImageP, 2> sheets;    ///< SCENERY0/1.PCK: the terrain tile sheets.
    gfx::ImageP road_img;                 ///< ROAD.PCK: the road tiles.
    gfx::ImageP shadow_img;               ///< ASHADOW.PCK: army shadows.
    gfx::ImageP marble;                   ///< MARBLE.PCK: the popups' face.
    gfx::ImageP big_army;                 ///< BIGARMY.PCK, for the city dialog.
    gfx::ImageP city_pic;                 ///< CITY.PCK, for the banner.
    gfx::ImageP war_pic;                  ///< WAR.PCK: the battle picture.
    gfx::ImageP shield_img;               ///< BSHIELD.PCK: the small shields.
    gfx::ImageP victory_pic;              ///< VICTORY.PCK: the picture of a city taken.
    gfx::ImageP hero_pic_m;               ///< MHERO.PCK: a hero's picture, male.
    gfx::ImageP hero_pic_f;               ///< A hero's picture, female.
    std::array<gfx::ImageP, 9> army_img;  ///< The army sheets, one per side and neutral.
    gfx::ImageP atrans_shields;           ///< ATRANS2.PCK: shields, flags and hero figures.
    gfx::ImageP fog_img;                  ///< HIDDEN.PCK: the fog over unseen ground.
    gfx::ImageP curs_img;                 ///< CURS.PCK: the animated selection box.
    gfx::ImageP abits;                    ///< ABITS.PCK: rings and small pieces of the bar.
    gfx::ImageP pointer_img;              ///< STAND.PCK: the mouse pointers.
    gfx::ImageP shields_img;              ///< SHIELDS.PCK: the big 40x40 shields.
    gfx::ImageP city_back;                ///< CITYBACK.PCK.
    gfx::ImageP search_pic;               ///< SEARCH.PCK.
    gfx::ImageP temple_pic;               ///< TEMPLE.PCK.
    gfx::ImageP scroll_pic;               ///< SCROLL.PCK.
    /** A promotion's pictures (popups 16-18), by level. */
    std::map<int, gfx::ImageP> level_pic;
    std::array<gfx::ImageP, 4> medal_pic;    ///< A medal's pictures (popups 24-27).
    std::map<int, gfx::ImageP> move_bars;    ///< The moves-left bars, loaded as needed.
    std::unique_ptr<screen::Screen> screen;  ///< The main screen's layout and art.
    FontP font;                              ///< TEXT, the small font.
    FontP big_font;                          ///< CHANCE17.
    FontP title_font;                        ///< CHANCE36.

    // the screen
    int open_menu = -1;         ///< The menu dropped down, -1 for none.
    layout::Layout layout;      ///< Where the main screen's pieces go.
    bool have_layout = false;   ///< `layout` has been computed.
    layout::Rect map_rect;      ///< The map, placed.
    layout::Rect strat_rect;    ///< The strategic map, placed.
    layout::Rect bar_rect;      ///< The bottom bar, placed.
    layout::Rect menu_rect;     ///< The menu bar, placed.
    layout::Rect panel_rect;    ///< The control panel, placed.
    menu::Layout menu_layout;   ///< The menu bar, measured.
    int zoom = 1;               ///< The map's zoom.
    double cx = 0;              ///< The tile at the map's top left, across (fractional).
    double cy = 0;              ///< The tile at the map's top left, down (fractional).
    std::array<int, 4> view{};  ///< The tiles drawn this frame: x0, y0, x1, y1.
    gfx::ImageP strat_image;    ///< The strategic map, rebuilt when the fog changes.

    // the game
    bool starting = false;                      ///< The start screens are up.
    std::string scenario = "ERYTHEA";           ///< The scenario to play.
    double seed = 0;                            ///< The new game's seed.
    std::unique_ptr<w2::Game> g;                ///< The game, null before one starts.
    w2::Side* player = nullptr;                 ///< The human side whose turn it is.
    std::optional<std::pair<int, int>> cursor;  ///< Where the army cycle last stopped.
    std::optional<Selection> selection;         ///< The stack picked up.
    std::optional<w2::move::Preview> route;     ///< The selected stack's planned route.
    std::optional<Walk> walk;                   ///< A stack walking.

    // the turn's openings
    std::optional<Banner> banner;           ///< The start-of-turn banner, while it shows.
    std::shared_ptr<w2::HeroOffer> offer;   ///< A hero offering to join.
    bool offer_female = false;              ///< The hero on offer is a woman.
    std::string offer_name;                 ///< The name the player gives the hero.
    std::optional<screen::View> hero_view;  ///< The hero offer's dialog.
    bool map_after_banner = false;  ///< Centre the map on the capital once the banner closes.

    // battles
    std::optional<Assault> assault;            ///< A battle being shown.
    std::optional<Victory> victory;            ///< A city just taken, asking what to do.
    std::optional<Victory> victory_under;      ///< A victory waiting under another dialog.
    w2::Army* medal = nullptr;                 ///< What the assault just closed gave a medal to.
    std::optional<screen::View> victory_view;  ///< The city-taken dialog.

    // the computer's turn
    std::unique_ptr<Coroutine> ai_run;  ///< The computer sides' turns, run a step a frame.
    bool ai_wait = false;               ///< The computer's turn has paused and waits to be resumed.
    bool ai_skip = false;               ///< Running the round through unwatched.
    bool ai_mods_held =
            false;  ///< Shift or Alt was already down: only a fresh press opens Settings.
    w2::Side* ai_side = nullptr;        ///< The computer side playing.
    std::optional<AiStatus> ai_status;  ///< What the computer is doing, shown.
    double ai_resume_at = 0;            ///< When the computer goes on after a pause; 0 none.

    bool over = false;         ///< The game has ended.
    std::optional<Drag> drag;  ///< The map being dragged.
    int pressed = w2::kNone;   ///< The control held down, kNone for none.
    /** @brief Move All in progress: the armies already moved. */
    struct MoveAll {
        std::set<w2::Army*> seen;  ///< Armies already given their turn.
        int moved = 0;             ///< How many stacks have moved.
    };
    std::optional<MoveAll> move_all;      ///< Move All in progress.
    double last_select_click = -1;        ///< When the select pointer was last clicked.
    std::vector<std::string> key_buffer;  ///< Keys pressed while a walk plays, handled after it.
    std::string status;                   ///< The status line's text.
    bool modern_ui = false;  ///< The remake's own additions to the interface are on (--modern-ui).
    bool quit_requested = false;  ///< The player asked to quit.
};
/** The front end's state. Named for the Lua remake's G; it goes away when the
 *  front end is restructured. */
extern App g_app;

/** @brief What the dialogs ask of the main screen. */
namespace front {
constexpr int kNone = w2::kNone;  ///< No index: as w2::kNone.
/** @brief Marks the strategic map image stale, to be rebuilt. */
void StratDirty();
/**
 * @brief The strategic map as the original paints it anywhere it appears.
 *
 * Every city it can see is an 8x8 shield. `mark` gets a white box; `owners`
 * is who held each city, for History. Only `rows` of it show, from `scroll`
 * rows down (the main screen's, on a short screen).
 */
void DrawStrategicMap(int x, int y, const w2::City* mark = nullptr, bool no_cities = false,
                      const std::vector<int>* owners = nullptr, int scroll = 0, int rows = 312);
/** @brief Marks every site shown to the side on a strategic map drawn at (x, y). */
void DrawSiteMarkers(int x, int y, const w2::Site* highlight = nullptr);
/**
 * @brief The map as the city dialog's Vector mode shows it.
 *
 * `filter` hides the cities that could take no more vectors (kNone for no
 * filter).
 */
void DrawVectorMap(int x, int y, w2::City* city, int filter, bool see_all);
/** @brief A hero's figure on a strategic map drawn at (x, y), at tile (tile_x, tile_y). */
void DrawHeroFigure(int x, int y, int tile_x, int tile_y, bool dark = false);
/** @brief The strategic map with `city` marked, as a dialog's panel shows it. */
void DrawStrategicPanel(int x, int y, const w2::City* city);
/** @brief Opens the city dialog for `city`. */
void OpenCity(w2::City* city);
/** @brief Opens the quest dialog. */
void OpenQuest();
/** @brief Asks to quit. */
void Quit();
/** @brief Opens the start screens. */
void OpenStart();
/** @brief Plays a loaded game. */
void TakeLoaded(std::unique_ptr<w2::Game> game);
/** @brief Shows a battle already fought at tile (x, y). */
void StartAssault(int x, int y, const w2::Battle& result);
/** @brief Closes the battle shown, and goes on to what follows it. */
void CloseAssault();
/** @brief Asks what to do with a city just taken by `victor`. */
void PresentVictory(w2::City* city, const std::string& victor);
/** @brief Scrolls the map so tile (x, y) is in the middle. */
void CentreOn(int x, int y);
/** @brief Sets the map's zoom. */
void SetZoom(int zoom);
/** @brief Picks up the player's stack at tile (x, y), if there is one. */
void SelectAt(int x, int y);
/** @brief Brings the screen up to date after the bottom bar's slots change. */
void AfterSlotChange();
/** @brief The tile in the middle of the map. */
std::pair<int, int> ViewCentre();
/** @brief Where a tile position on the map is in UI pixels. */
std::pair<double, double> MapToUi(double tile_x, double tile_y);
/** @brief True when the menu item with key or action `menu_key` can be chosen now. */
bool MenuEnabled(const std::string& menu_key);
/** @brief Runs the computer sides' turns. */
void PlayComputer();
/** @brief Places the main screen's pieces for the window's size. */
void SyncLayout();
/** @brief Puts a line in the status bar. */
void Say(const std::string& message);
/** @brief A random roll for what does not touch the game's dice: 1..n. */
int Roll(int n);
}  // namespace front
