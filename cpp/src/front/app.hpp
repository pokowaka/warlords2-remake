// The front end's state, shared with the dialogs: the Lua remake's G.
//
// main.cpp is the main screen (love2d/main.lua); the dialogs live in ui/ and
// reach the screen through the functions declared here.
#pragma once

#include <array>
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

struct Selection {
    int x = 0, y = 0;
    w2::slots::Slots slots;
    std::vector<w2::Army*> stack;
};

struct Walk {
    std::set<w2::Army*> armies;
    std::vector<std::pair<int, int>> tiles;
    size_t i = 0;
    double at = 0;
    w2::Army* top = nullptr;
    bool computer = false;
    bool sea_before = false;          // at sea as it set off: shown so to the walk's end
    std::vector<w2::Army*> put_down;  // out of moves: put down once the walk has played out
};

struct Banner {
    std::string name;
    int turn = 0, colour = 15, edge = 0;
};

struct Fell {
    double at = 0, burn = 0;
};

struct Assault {
    int x = 0, y = 0;
    w2::Battle result;
    std::vector<w2::Army*> def, atk;
    std::vector<std::pair<int, int>> def_slots, atk_slots;
    int def_side = 8, atk_side = 8;
    size_t step = 0;
    int def_down = 0, atk_down = 0;
    std::vector<Fell> def_fell, atk_fell;
    std::string phase = "cloud";
    double at = 0, cloud_time = 0, close_at = 0;
    bool fast = false, computer = false, window = false;
    std::string victor, outcome;
    std::vector<std::string> message;
    std::function<void()> after;
};

struct Victory {
    w2::City* city = nullptr;
    std::string who, where;
    int enter = 285;  // what Enter and Escape choose: Occupy (285), or what a quest wants
};

struct AiStatus {
    w2::Side* side = nullptr;
    std::string text;
    int progress = 0;
};

struct Drag {
    double cx = 0, cy = 0, dx = 0, dy = 0;
};

struct App {
    std::vector<std::shared_ptr<kit::Modal>> modals;
    int mouse_x = -1, mouse_y = -1;
    w2::pal::Palette palette;
    std::string data_dir;

    // the art
    std::array<gfx::ImageP, 2> sheets;
    gfx::ImageP road_img, shadow_img, marble, big_army, city_pic, war_pic, shield_img, victory_pic,
            hero_pic_m, hero_pic_f;
    std::array<gfx::ImageP, 9> army_img;
    gfx::ImageP atrans_shields, fog_img, curs_img, abits, pointer_img;
    gfx::ImageP shields_img, city_back, search_pic, temple_pic, scroll_pic;
    // a promotion's (popups 16-18, by level) and a medal's (popups 24-27) pictures
    std::map<int, gfx::ImageP> level_pic;
    std::array<gfx::ImageP, 4> medal_pic;
    std::map<int, gfx::ImageP> move_bars;
    std::unique_ptr<screen::Screen> screen;
    FontP font, big_font, title_font;

    // the screen
    int open_menu = -1;
    layout::Layout layout;
    bool have_layout = false;
    layout::Rect map_rect, strat_rect, bar_rect, menu_rect, panel_rect;
    menu::Layout menu_layout;
    int zoom = 1;
    double cx = 0, cy = 0;
    std::array<int, 4> view{};  // the tiles drawn this frame: x0, y0, x1, y1
    gfx::ImageP strat_image;

    // the game
    bool starting = false;
    std::string scenario = "ERYTHEA";
    double seed = 0;
    std::unique_ptr<w2::Game> g;
    w2::Side* player = nullptr;
    std::optional<std::pair<int, int>> cursor;  // where the army cycle last stopped
    std::optional<Selection> selection;
    std::optional<w2::move::Preview> route;
    std::optional<Walk> walk;

    // the turn's openings
    std::optional<Banner> banner;
    std::shared_ptr<w2::HeroOffer> offer;
    bool offer_female = false;
    std::string offer_name;
    std::optional<screen::View> hero_view;
    bool map_after_banner = false;

    // battles
    std::optional<Assault> assault;
    std::optional<Victory> victory, victory_under;
    w2::Army* medal = nullptr;  // what the assault just closed gave a medal to
    std::optional<screen::View> victory_view;

    // the computer's turn
    std::unique_ptr<Coroutine> ai_run;
    bool ai_wait = false, ai_skip = false, ai_mods_held = false;
    w2::Side* ai_side = nullptr;
    std::optional<AiStatus> ai_status;
    double ai_resume_at = 0;

    bool over = false;
    std::optional<Drag> drag;
    int pressed = w2::kNone;
    struct MoveAll {
        std::set<w2::Army*> seen;
        int moved = 0;
    };
    std::optional<MoveAll> move_all;
    double last_select_click = -1;  // when the select pointer was last clicked (1b62:0405)
    std::vector<std::string> key_buffer;
    std::string status;
    bool modern_ui = false;
    bool quit_requested = false;
};
extern App G;

// What the dialogs ask of the main screen.
namespace front {
constexpr int kNone = w2::kNone;
void StratDirty();
/** The strategic map as the original paints it anywhere it appears, every
 *  city it can see as an 8x8 shield (834b:0ed7). `mark` gets a white box;
 *  `owners` is who held each city, for History (834b:12d3). Only `rows` of
 *  it show, from `scroll` rows down (the main screen's, on a short screen). */
void DrawStrategicMap(int x, int y, const w2::City* mark = nullptr, bool no_cities = false,
                      const std::vector<int>* owners = nullptr, int scroll = 0, int rows = 312);
/** Every site shown to the side, marked on a strategic map (834b:05e4). */
void DrawSiteMarkers(int x, int y, const w2::Site* highlight = nullptr);
/** The map as the city dialog's Vector mode shows it. `filter` hides the
 *  cities that could take no more vectors (NONE for no filter). */
void DrawVectorMap(int x, int y, w2::City* city, int filter, bool see_all);
/** A hero's figure on the strategic map (834b:1f5f). */
void DrawHeroFigure(int x, int y, int tx, int ty, bool dark = false);
void DrawStrategicPanel(int x, int y, const w2::City* city);
void OpenCity(w2::City* city);
void OpenQuest();
void Quit();
void OpenStart();
void TakeLoaded(std::unique_ptr<w2::Game> g);
void StartAssault(int x, int y, const w2::Battle& result);
void CloseAssault();
void PresentVictory(w2::City* city, const std::string& victor);
void CentreOn(int x, int y);
void SetZoom(int z);
void SelectAt(int x, int y);
void AfterSlotChange();
std::pair<int, int> ViewCentre();
std::pair<double, double> MapToUi(double tx, double ty);
bool MenuEnabled(const std::string& k);
void PlayComputer();
void SyncLayout();
void Say(const std::string& s);
/** A random roll for what does not touch the game's dice: 1..n. */
int Roll(int n);
}  // namespace front
