/**
 * @file
 * @brief Automated UI test script loader and frame-by-frame command runner.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "front/internal.hpp"
#include "platform/display.hpp"
#include "platform/pad.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/save.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

/** @brief A test script driving the UI frame by frame. */
struct Script {
    std::vector<std::string> lines;  ///< Script command lines.
    size_t at = 0;                   ///< Index of the next command line to execute.
    int wait = 0;                    ///< Frames remaining to wait before the next command.
};
static std::optional<Script> script;

void LoadScript(const std::string& path) {
    Script loaded_script;
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) loaded_script.lines.push_back(line);
    script = loaded_script;
}

bool HasScript() {
    return script.has_value();
}

// ------------------------------------------------------------------ scripts

// A script drives the game for testing: one command a line, the coordinates
// UI pixels.
//   wait N          let N frames go by
//   key NAME        press and release a key, by LÖVE's name for it
//   hold NAME / release NAME
//   click X Y [B]   press and release a mouse button (1 left, 2 right)
//   move X Y        move the pointer
//   text STRING     type
//   shot FILE       write the frame as a .bmp
//   glass FILE      write the window as the next frame shows it, the CRT's
//                   tube and all, as a .bmp
//   pad NAME        press and release a controller's button, by SDL's name
//                   for it (a, b, dpup, leftshoulder, lefttrigger ...)
//   padhold NAME / padrelease NAME
//   stick LX LY RX RY   hold the sticks there, -1..1; all 0 lets go
//   eval            print the game's state (turn, side, modals)
//   select TX TY    pick up the stack on a tile
//   attack TX TY    send the selection at a tile, fighting if it must
//   near            list the cities nearest the selection not the player's
//   greet           the advisor's greeting, as the start screens open
//   sdlclick X Y    a left click on UI point (X, Y) sent through SDL's event
//                   queue, as a real one arrives
//   quit
/** @brief Executes commands from the active test script until a `wait` or `quit` is reached. */
void StepScript() {
    if (!script) return;
    if (script->wait > 0) {
        script->wait--;
        return;
    }
    while (script->at < script->lines.size()) {
        std::istringstream stream(script->lines[script->at++]);
        std::string cmd;
        stream >> cmd;
        if (cmd.empty() || cmd[0] == '#') continue;
        if (cmd == "wait") {
            stream >> script->wait;
            return;
        } else if (cmd == "key") {
            std::string key;
            stream >> key;
            keys::Press(key);
            Keypressed(key);
            keys::Release(key);
        } else if (cmd == "hold") {
            std::string key;
            stream >> key;
            keys::Press(key);
        } else if (cmd == "release") {
            std::string key;
            stream >> key;
            keys::Release(key);
        } else if (cmd == "click") {
            int x, y, button = 1;
            stream >> x >> y;
            if (!(stream >> button)) button = 1;
            g_app.mouse_x = x;
            g_app.mouse_y = y;
            display::state.mouse_inside = true;
            display::state.off.reset();
            Mousepressed(x, y, button);
            Mousereleased(x, y, button);
        } else if (cmd == "move") {
            stream >> g_app.mouse_x >> g_app.mouse_y;
            display::state.mouse_inside = true;
            display::state.off.reset();
        } else if (cmd == "pad" || cmd == "padhold" || cmd == "padrelease") {
            std::string key;
            stream >> key;
            int button = pad::FromName(key);
            if (button < 0) continue;
            if (cmd != "padrelease") PadButton(button, true);
            if (cmd != "padhold") PadButton(button, false);
        } else if (cmd == "stick") {
            pad::Sticks sticks;
            stream >> sticks.lx >> sticks.ly >> sticks.rx >> sticks.ry;
            pad::Script(sticks);
        } else if (cmd == "text") {
            std::string text;
            std::getline(stream, text);
            Textinput(w2::Trim(text));
        } else if (cmd == "shot") {
            std::string path;
            stream >> path;
            gfx::SaveShot(path);
        } else if (cmd == "glass") {
            stream >> display::state.glass_shot;
        } else if (cmd == "eval") {
            printf("turn=%d player=%s modals=%d banner=%d offer=%d ai=%d over=%d walk=%d "
                   "assault=%d status=%s\n",
                   g_app.g ? g_app.g->Turn() : 0, g_app.player ? g_app.player->Name().c_str() : "-",
                   static_cast<int>(g_app.modals.size()), static_cast<bool>(g_app.banner),
                   static_cast<bool>(g_app.offer), static_cast<bool>(g_app.ai_run), g_app.over,
                   static_cast<bool>(g_app.walk), static_cast<bool>(g_app.assault),
                   g_app.ai_status ? g_app.ai_status->text.c_str() : "");
            if (g_app.g && g_app.g->HasMap()) {
                const auto& opts = g_app.g->GetMap().GameOptions();
                printf("  options: milAdv=%d diplo=%d hidden=%d viewEn=%d\n", opts.military_advisor,
                       opts.diplomacy, opts.hidden_map, opts.view_enemies);
            }
            if (g_app.g && g_app.g->HasMap() && g_app.player) {
                if (const City* cap = g_app.player->CapitalCity(g_app.g->GetMap())) {
                    printf("  cam=%.2f,%.2f zoom=%d map=%d,%d,%d,%d capital=%d,%d pointer=%d,%d\n",
                           g_app.cx, g_app.cy, g_app.zoom, g_app.map_rect.x, g_app.map_rect.y,
                           g_app.map_rect.w, g_app.map_rect.h, cap->X(), cap->Y(), g_app.mouse_x,
                           g_app.mouse_y);
                }
            }
            fflush(stdout);
        } else if (cmd == "opt") {
            std::string opt_name;
            int val;
            stream >> opt_name >> val;
            if (g_app.g && g_app.g->HasMap()) {
                if (int* field = g_app.g->GetMap().MutableOptions().Field(opt_name)) *field = val;
            }
        } else if (cmd == "load") {
            std::string slot_name;
            stream >> slot_name;
            while (kit::Top()) kit::Pop(kit::Top());
            if (auto loaded = savegame::ReadSlot(slot_name)) {
                TakeLoaded(std::move(loaded));
                SyncLayout();
            }
        } else if (cmd == "centre") {
            int tile_x, tile_y;
            stream >> tile_x >> tile_y;
            CentreOn(tile_x, tile_y);
        } else if (cmd == "pointer") {
            int x, y;
            stream >> x >> y;
            printf("pointer(%d,%d)=%d\n", x, y, PointerKind(x, y));
            fflush(stdout);
        } else if (cmd == "tileptr") {
            int tile_x, tile_y;
            stream >> tile_x >> tile_y;
            int pixel_x = g_app.map_rect.x + static_cast<int>((tile_x + 0.5 - g_app.cx) * kTile *
                                                              g_app.zoom / display::state.scale);
            int pixel_y = g_app.map_rect.y + static_cast<int>((tile_y + 0.5 - g_app.cy) * kTile *
                                                              g_app.zoom / display::state.scale);
            printf("tileptr(%d,%d)=%d\n", tile_x, tile_y, PointerKind(pixel_x, pixel_y));
            fflush(stdout);
        } else if (cmd == "spawn" && g_app.g) {
            int tile_x, tile_y, owner, type;
            stream >> tile_x >> tile_y >> owner >> type;
            Army army;
            army.PlaceAt(w2::TilePos(tile_x, tile_y));
            army.SetOwner(w2::SideId(owner));
            army.SetType(w2::ArmyTypeId(type));
            army.SetMoves(10);
            g_app.g->Add(army);
        } else if (cmd == "advise") {
            int x, y;
            stream >> x >> y;
            miladvisor::Open(g_app.selection ? &g_app.selection->stack : nullptr, x, y);
        } else if (cmd == "select") {
            int x, y;
            stream >> x >> y;
            Select(x, y);
        } else if (cmd == "attack") {
            int x, y;
            stream >> x >> y;
            AttackTile(x, y);
        } else if (cmd == "near" && g_app.g && g_app.selection) {
            std::vector<std::pair<int, City*>> found;
            for (auto& city : g_app.g->GetMap().Cities()) {
                if (city.OwnerIndex() != g_app.player->Index() && !city.IsRazed())
                    found.push_back({std::max(std::abs(city.X() - g_app.selection->x),
                                              std::abs(city.Y() - g_app.selection->y)),
                                     &city});
            }
            std::sort(found.begin(), found.end(),
                      [](auto& left, auto& right) { return left.first < right.first; });
            for (size_t i = 0; i < found.size() && i < 3; i++)
                printf("  %s at %d,%d owner %d, %d away\n", found[i].second->Name().c_str(),
                       found[i].second->X(), found[i].second->Y(), found[i].second->OwnerIndex(),
                       found[i].first);
            fflush(stdout);
        } else if (cmd == "greet") {
            advisor::Say(w2::cues::kGreet);
        } else if (cmd == "sdlclick") {
            // a click on UI point (x, y) as SDL delivers one, in window points,
            // through the event loop
            int ui_x, ui_y;
            stream >> ui_x >> ui_y;
            auto [win_x, win_y] = display::FromUi(ui_x, ui_y);
            int x = static_cast<int>(win_x), y = static_cast<int>(win_y);
            for (Uint32 type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
                SDL_Event event{};
                event.type = type;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                event.button.x = x;
                event.button.y = y;
                SDL_PushEvent(&event);
            }
        } else if (cmd == "quit") {
            g_app.quit_requested = true;
            return;
        }
    }
}

}  // namespace front
