/**
 * @file
 * @brief Warlords II SDL2 entry point: CLI parsing, initialization, and the frame loop.
 *
 *     warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window]
 *               [--size WxH] [--script FILE]
 *
 * Run from the repository root, or point --data at your copy of the game.
 */
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <random>
#include <string>

#include <SDL.h>

#include "front/app.hpp"
#include "front/internal.hpp"
#include "front/layout.hpp"
#include "front/prefs.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/cues.hpp"
#include "warlords/uidata.hpp"

/// The front end's shared state.
App g_app;

/// Dice for what does not touch the game's own dice (front::Roll).
static std::mt19937 loose_dice(static_cast<unsigned>(time(nullptr)));

namespace front {

int opt_military_advisor = -1;

int Roll(int n) {
    return n <= 0 ? 1 : static_cast<int>(loose_dice() % static_cast<unsigned>(n)) + 1;
}

void Say(const std::string& message) {
    g_app.status = message;
}

}  // namespace front

/** @brief Prints an error message to stderr and shows an SDL error dialog box. */
static int Fail(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", msg.c_str(), nullptr);
    return 1;
}

/** @brief Entry point: parses CLI flags, initializes SDL/OpenGL and assets, and runs the main loop.
 */
int main(int argc, char** argv) {
    std::string scenario;
    std::string script_file;
    std::string weights_file;
    double seed = 0;
    int size_w = 0;
    int size_h = 0;
    bool windowed = prefs::Get("screen") == "window";
    bool use_classic_ai = prefs::Get("ai") == "classic";
    bool force_diplomacy = false;
    bool force_hidden_map = false;
    bool spectate_all_ai = false;
    g_app.modern_ui = false;
    g_app.data_dir = "original";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--data")
            g_app.data_dir = next();
        else if (arg == "--scenario")
            scenario = w2::Upper(next());
        else if (arg == "--seed")
            seed = atof(next().c_str());
        else if (arg == "--window")
            windowed = true;
        else if (arg == "--fullscreen")
            windowed = false;
        else if (arg == "--script")
            script_file = next();
        else if (arg == "--size")
            sscanf(next().c_str(), "%dx%d", &size_w, &size_h);
        else if (arg == "--classic-ai")
            use_classic_ai = true;
        else if (arg == "--modern-ai")
            use_classic_ai = false;
        else if (arg == "--modern-ui")
            g_app.modern_ui = true;
        else if (arg == "--classic-ui")
            g_app.modern_ui = false;
        else if (arg == "--weights")
            weights_file = next();
        else if (arg == "--diplomacy")
            force_diplomacy = true;
        else if (arg == "--hidden-map")
            force_hidden_map = true;
        else if (arg == "--military-advisor")
            front::opt_military_advisor = 1;
        else if (arg == "--no-military-advisor")
            front::opt_military_advisor = 0;
        else if (arg == "--spectate" || arg == "--all-ai")
            spectate_all_ai = true;
    }
    w2::ai::SetDefaultClassicAi(use_classic_ai);
    std::string weights_path = !weights_file.empty() ? weights_file
                               : w2::FileExists("cpp/ai_weights.json")
                                       ? std::string("cpp/ai_weights.json")
                               : w2::FileExists("ai_weights.json") ? std::string("ai_weights.json")
                                                                   : std::string();
    if (weights_path.empty() || !w2::ai::ml::ActivePolicy().LoadJson(weights_path)) {
        fprintf(stderr,
                "warning: Modern AI weights %s%s%s could not be loaded (missing, or trained for a "
                "different network size); the Modern AI will play with untrained random weights. "
                "Retrain with cpp/test/train.sh or pass --weights PATH.\n",
                weights_path.empty() ? "(none found)" : "'", weights_path.c_str(),
                weights_path.empty() ? "" : "'");
    }
    if (!w2::FileExists(g_app.data_dir + "/TERRAIN0/ARMYTYPE.DAT"))
        return Fail("cannot find the game's files in '" + g_app.data_dir +
                    "': run from the repository root, or pass --data DIR");
    pad::Hints();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        return Fail(std::string("SDL: ") + SDL_GetError());
    }
    SDL_Window* win = display::Open("Warlords II", windowed, size_w, size_h);
    if (!win) {
        return Fail(std::string("SDL window: ") + SDL_GetError());
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_Renderer* ren =
            SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        return Fail(std::string("SDL renderer: ") + SDL_GetError());
    }
    gfx::Init(ren);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_StopTextInput();
    if (!script_file.empty()) {
        front::LoadScript(script_file);
    }
    try {
        front::LoadArt();
        front::InstallHooks();
        auto ui_data = w2::uidata::Load(g_app.data_dir);
        sound::Init(g_app.data_dir, ui_data.files);
        if (prefs::Get("4:3") == "on")
            display::state.wanted = std::make_pair(layout::kScreenW, layout::kScreenH);
        display::state.crt = prefs::Get("crt") == "on";
        display::state.chosen =
                prefs::Get("scale") == "smooth" ? display::kSmooth : prefs::GetInt("scale", 0);
        display::Measure();
        g_app.zoom = prefs::GetInt("zoom", display::state.scale);
        if (scenario.empty()) {
            front::OpenStart();
            if (script_file.empty()) advisor::Say(w2::cues::kGreet);
        } else {
            front::StartCliScenario(scenario, seed, force_diplomacy, force_hidden_map,
                                    spectate_all_ai);
        }
    } catch (const std::exception& err) {
        fprintf(stderr, "could not start: %s\n", err.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", err.what(), win);
        return 1;
    }

    front::InitPointerState(win);
    while (!g_app.quit_requested) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            try {
                switch (event.type) {
                case SDL_QUIT:
                    g_app.quit_requested = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        display::Measure();
                        g_app.have_layout = false;
                    } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                        display::state.mouse_inside = false;
                    } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        keys::Clear();
                        front::PadLetGo();
                    } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        SDL_ShowCursor(SDL_DISABLE);
                    }
                    break;
                case SDL_KEYDOWN: {
                    std::string key = keys::LoveName(event.key.keysym.sym);
                    if (key.empty() || event.key.repeat) break;
                    keys::Press(key);
                    front::Keypressed(key);
                    break;
                }
                case SDL_KEYUP: {
                    std::string key = keys::LoveName(event.key.keysym.sym);
                    if (!key.empty()) keys::Release(key);
                    break;
                }
                case SDL_TEXTINPUT:
                    if (!keys::IsDown({"lalt", "ralt", "lctrl", "rctrl", "lgui", "rgui"}))
                        front::Textinput(w2::Utf8ToLatin1(event.text.text));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    if (event.type == SDL_MOUSEBUTTONDOWN &&
                        !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
                        SDL_RaiseWindow(win);
                    }
                    auto [x, y] = display::ToUi(event.button.x, event.button.y);
                    g_app.mouse_x = x;
                    g_app.mouse_y = y;
                    int button = event.button.button == SDL_BUTTON_RIGHT    ? 2
                                 : event.button.button == SDL_BUTTON_MIDDLE ? 3
                                                                            : 1;
                    if (event.type == SDL_MOUSEBUTTONDOWN)
                        front::Mousepressed(x, y, button);
                    else
                        front::Mousereleased(x, y, button);
                    break;
                }
                case SDL_MOUSEMOTION:
                    front::HandleMouseMotion(event.motion.x, event.motion.y);
                    break;
                case SDL_MOUSEWHEEL:
                    if (event.wheel.y != 0) front::Wheelmoved(event.wheel.y > 0 ? 1 : -1);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                case SDL_CONTROLLERDEVICEREMOVED:
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int button = 0;
                    bool down = false;
                    if (pad::TranslateEvent(event, button, down))
                        front::PadButton(button, down);
                    else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
                        front::PadLetGo();
                    break;
                }
                }
            } catch (const std::exception& ex) {
                fprintf(stderr, "error: %s\n", ex.what());
            }
        }
        front::PollFocusedMouse(win);
        try {
            display::state.min_h = g_app.starting || !g_app.g ? layout::kScreenH : layout::kShortH;
            display::Measure();
            display::BeginFrame();
            front::PadUpdate();
            front::SyncTyping();
            front::StepScript();
            front::Update();
            front::Draw();
        } catch (const std::exception& ex) {
            fprintf(stderr, "error: %s\n", ex.what());
        }
        display::Present();
    }
    g_app.ai_run.reset();
    g_app.modals.clear();
    sound::Shutdown();
    gfx::Shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
