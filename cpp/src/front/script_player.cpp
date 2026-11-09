#include "front/script_player.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include <SDL.h>

#include "front/map_screen_controller.hpp"
#include "platform/display.hpp"
#include "platform/pad.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"

namespace w2 {

void ScriptPlayer::Load(const std::filesystem::path& path) {
    lines_.clear();
    next_index_ = 0;
    wait_frames_ = 0;
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) {
        lines_.push_back(line);
    }
    loaded_ = true;
}

void ScriptPlayer::Step(MapScreenController& controller) {
    if (!loaded_) return;
    if (wait_frames_ > 0) {
        --wait_frames_;
        return;
    }

    UiContext& ctx = controller.Ctx();
    display::DisplayWindow& disp = ctx.Display();

    while (next_index_ < lines_.size()) {
        std::istringstream stream(lines_[next_index_++]);
        std::string cmd;
        stream >> cmd;
        if (cmd.empty() || cmd[0] == '#') continue;

        if (cmd == "wait") {
            stream >> wait_frames_;
            return;
        }
        if (cmd == "key") {
            std::string key;
            stream >> key;
            ctx.Keyboard().Press(key);
            controller.Keypressed(key);
            ctx.Keyboard().Release(key);
        } else if (cmd == "hold") {
            std::string key;
            stream >> key;
            ctx.Keyboard().Press(key);
        } else if (cmd == "release") {
            std::string key;
            stream >> key;
            ctx.Keyboard().Release(key);
        } else if (cmd == "click") {
            int x = 0;
            int y = 0;
            int button = 1;
            stream >> x >> y;
            if (!(stream >> button)) button = 1;
            controller.SetMousePos(x, y);
            disp.MutableState().mouse_inside = true;
            disp.MutableState().off.reset();
            controller.Mousepressed(x, y, button);
            controller.Mousereleased(x, y, button);
        } else if (cmd == "move") {
            int x = 0;
            int y = 0;
            stream >> x >> y;
            controller.SetMousePos(x, y);
            disp.MutableState().mouse_inside = true;
            disp.MutableState().off.reset();
        } else if (cmd == "pad" || cmd == "padhold" || cmd == "padrelease") {
            std::string key;
            stream >> key;
            int button = pad::GamepadInput::ButtonFromName(key);
            if (button < 0) continue;
            if (cmd != "padrelease") controller.PadButton(button, true);
            if (cmd != "padhold") controller.PadButton(button, false);
        } else if (cmd == "stick") {
            pad::Sticks sticks;
            stream >> sticks.lx >> sticks.ly >> sticks.rx >> sticks.ry;
            ctx.Gamepad().SetScriptedSticks(sticks);
        } else if (cmd == "text") {
            std::string text;
            std::getline(stream, text);
            controller.Textinput(Trim(text));
        } else if (cmd == "shot") {
            std::string path;
            stream >> path;
            ctx.Renderer().SaveShot(path);
        } else if (cmd == "glass") {
            stream >> disp.MutableState().glass_shot;
        } else if (cmd == "eval") {
            Game* game = ctx.GamePtr();
            const IMapScreenState& state = controller.State();
            const AiStatus* ai_status = state.ActiveAiStatus();
            printf("turn=%d player=%s modals=%d banner=%d offer=%d ai=%d over=%d walk=%d "
                   "assault=%d status=%s\n",
                   game ? game->Turn() : 0, ctx.Player() ? ctx.Player()->Name().c_str() : "-",
                   static_cast<int>(ctx.Modals().DialogCountExcludingTurnOpeners()),
                   ctx.Modals().HasTurnBanner(), ctx.Modals().HasHeroOffer(),
                   state.IsComputerTurn(), state.IsGameOver(), state.IsWalking(),
                   state.IsAssaulting(), ai_status ? ai_status->text.c_str() : "");
            if (game && game->HasMap()) {
                const auto& opts = game->GetMap().GameOptions();
                printf("  options: milAdv=%d diplo=%d hidden=%d viewEn=%d\n", opts.military_advisor,
                       opts.diplomacy, opts.hidden_map, opts.view_enemies);
            }
            if (game && game->HasMap() && ctx.Player()) {
                if (const City* cap = ctx.Player()->CapitalCity(game->GetMap())) {
                    const Viewport& viewport = ctx.GetViewport();
                    const auto& map_rect = viewport.MapRect();
                    printf("  cam=%.2f,%.2f zoom=%d map=%d,%d,%d,%d capital=%d,%d pointer=%d,%d\n",
                           viewport.Cx(), viewport.Cy(), viewport.Zoom(), map_rect.x, map_rect.y,
                           map_rect.w, map_rect.h, cap->X(), cap->Y(), controller.MouseX(),
                           controller.MouseY());
                }
            }
            fflush(stdout);
        } else if (cmd == "opt") {
            std::string opt_name;
            int val = 0;
            stream >> opt_name >> val;
            if (ctx.GamePtr() && ctx.GamePtr()->HasMap()) {
                if (int* field = ctx.GamePtr()->GetMap().MutableOptions().Field(opt_name)) {
                    *field = val;
                }
            }
        } else if (cmd == "load") {
            std::string slot_name;
            stream >> slot_name;
            ctx.Modals().Clear();
            if (auto loaded = savegame::ReadSlot(ctx, slot_name)) {
                controller.TakeLoaded(std::move(loaded));
                controller.SyncLayout();
            }
        } else if (cmd == "centre") {
            int tile_x = 0;
            int tile_y = 0;
            stream >> tile_x >> tile_y;
            ctx.CentreOn(tile_x, tile_y);
        } else if (cmd == "pointer") {
            int x = 0;
            int y = 0;
            stream >> x >> y;
            printf("pointer(%d,%d)=%d\n", x, y, controller.PointerKind(x, y));
            fflush(stdout);
        } else if (cmd == "tileptr") {
            int tile_x = 0;
            int tile_y = 0;
            stream >> tile_x >> tile_y;
            const Viewport& viewport = ctx.GetViewport();
            const auto& map_rect = viewport.MapRect();
            int scale = disp.GetState().scale;
            int pixel_x = map_rect.x +
                          static_cast<int>((tile_x + 0.5 - viewport.Cx()) *
                                           MapScreenController::kTile * viewport.Zoom() / scale);
            int pixel_y = map_rect.y +
                          static_cast<int>((tile_y + 0.5 - viewport.Cy()) *
                                           MapScreenController::kTile * viewport.Zoom() / scale);
            printf("tileptr(%d,%d)=%d\n", tile_x, tile_y, controller.PointerKind(pixel_x, pixel_y));
            fflush(stdout);
        } else if (cmd == "spawn" && ctx.GamePtr()) {
            int tile_x = 0;
            int tile_y = 0;
            int owner = 0;
            int type = 0;
            stream >> tile_x >> tile_y >> owner >> type;
            Army army;
            army.PlaceAt(TilePos(tile_x, tile_y));
            army.SetOwner(SideId(owner));
            army.SetType(ArmyTypeId(type));
            army.SetMoves(10);
            ctx.ActiveGame().Add(army);
        } else if (cmd == "advise") {
            int x = 0;
            int y = 0;
            stream >> x >> y;
            miladvisor::Open(ctx, ctx.ActiveSelection() ? &ctx.ActiveSelection()->stack : nullptr,
                             x, y);
        } else if (cmd == "select") {
            int x = 0;
            int y = 0;
            stream >> x >> y;
            controller.Select(x, y);
        } else if (cmd == "attack") {
            int x = 0;
            int y = 0;
            stream >> x >> y;
            controller.AttackTile(x, y);
        } else if (cmd == "near" && ctx.GamePtr() && ctx.ActiveSelection()) {
            const Selection& sel = *ctx.ActiveSelection();
            std::vector<std::pair<int, City*>> found;
            for (auto& city : ctx.ActiveGame().GetMap().Cities()) {
                if (city.OwnerIndex() != ctx.Player()->Index() && !city.IsRazed()) {
                    found.push_back(
                            {std::max(std::abs(city.X() - sel.x), std::abs(city.Y() - sel.y)),
                             &city});
                }
            }
            std::sort(found.begin(), found.end(),
                      [](const auto& left, const auto& right) { return left.first < right.first; });
            for (size_t i = 0; i < found.size() && i < 3; i++) {
                printf("  %s at %d,%d owner %d, %d away\n", found[i].second->Name().c_str(),
                       found[i].second->X(), found[i].second->Y(), found[i].second->OwnerIndex(),
                       found[i].first);
            }
            fflush(stdout);
        } else if (cmd == "greet") {
            advisor::Say(ctx, cues::kGreet);
        } else if (cmd == "sdlclick") {
            int ui_x = 0;
            int ui_y = 0;
            stream >> ui_x >> ui_y;
            auto [win_x, win_y] = disp.FromUi(ui_x, ui_y);
            int x = static_cast<int>(win_x);
            int y = static_cast<int>(win_y);
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
            ctx.RequestQuit();
            return;
        }
    }
}

}  // namespace w2
