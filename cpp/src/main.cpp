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
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

#include <SDL.h>

#include "absl/flags/flag.h"

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

ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, scenario, "", "Scenario directory name to launch directly.");
ABSL_FLAG(double, seed, 0.0, "Random seed when launching --scenario directly.");
ABSL_FLAG(bool, window, false, "Open in windowed mode.");
ABSL_FLAG(bool, fullscreen, false, "Open in fullscreen mode.");
ABSL_FLAG(std::string, script, "", "Automated UI test script file to execute.");
ABSL_FLAG(std::string, size, "", "Initial window dimensions formatted as WxH.");
ABSL_FLAG(bool, classic_ai, false, "Default computer sides to the 1993 Classic AI.");
ABSL_FLAG(bool, modern_ai, false, "Default computer sides to the Modern neural AI.");
ABSL_FLAG(bool, modern_ui, false, "Enable modern UI overlays and AI Brain Inspector.");
ABSL_FLAG(bool, classic_ui, false, "Disable modern UI overlays.");
ABSL_FLAG(std::string, weights, "", "Path to Modern AI policy weights JSON.");
ABSL_FLAG(bool, diplomacy, false, "Force the Diplomacy game option enabled.");
ABSL_FLAG(bool, hidden_map, false, "Force the Hidden Map game option enabled.");
ABSL_FLAG(bool, military_advisor, false, "Force the Military Advisor option enabled.");
ABSL_FLAG(bool, no_military_advisor, false, "Force the Military Advisor option disabled.");
ABSL_FLAG(bool, spectate, false, "Run all sides as computer players (AI-vs-AI spectator mode).");
ABSL_FLAG(bool, all_ai, false, "Alias for --spectate.");

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
static int Fail(const std::string& msg, SDL_Window* win = nullptr) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", msg.c_str(), win);
    return 1;
}

/** @brief Parses a `WxH` window size string into `size_w` and `size_h`. */
static void ParseWindowSize(std::string_view spec, int& size_w, int& size_h) {
    size_t sep = spec.find('x');
    if (sep == std::string_view::npos) sep = spec.find('X');
    if (sep == std::string_view::npos) return;
    auto parsed_w = w2::ToInt(spec.substr(0, sep));
    auto parsed_h = w2::ToInt(spec.substr(sep + 1));
    if (parsed_w && parsed_h && *parsed_w > 0 && *parsed_h > 0) {
        size_w = static_cast<int>(*parsed_w);
        size_h = static_cast<int>(*parsed_h);
    }
}

/** @brief Entry point: parses CLI flags, initializes SDL/OpenGL and assets, and runs the main loop.
 */
int main(int argc, char** argv) {
    w2::ParseCliFlags(argc, argv,
                      "warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window] [--size WxH] "
                      "[--script FILE]");

    g_app.data_dir = absl::GetFlag(FLAGS_data);
    std::string scenario = w2::Upper(absl::GetFlag(FLAGS_scenario));
    std::string script_file = absl::GetFlag(FLAGS_script);
    std::string weights_file = absl::GetFlag(FLAGS_weights);
    double seed = absl::GetFlag(FLAGS_seed);
    int size_w = 0;
    int size_h = 0;
    if (!absl::GetFlag(FLAGS_size).empty()) {
        ParseWindowSize(absl::GetFlag(FLAGS_size), size_w, size_h);
    }
    bool windowed = absl::GetFlag(FLAGS_window)       ? true
                    : absl::GetFlag(FLAGS_fullscreen) ? false
                                                      : (prefs::Get("screen") == "window");
    bool use_classic_ai = absl::GetFlag(FLAGS_classic_ai)  ? true
                          : absl::GetFlag(FLAGS_modern_ai) ? false
                                                           : (prefs::Get("ai") == "classic");
    bool force_diplomacy = absl::GetFlag(FLAGS_diplomacy);
    bool force_hidden_map = absl::GetFlag(FLAGS_hidden_map);
    bool spectate_all_ai = absl::GetFlag(FLAGS_spectate) || absl::GetFlag(FLAGS_all_ai);
    g_app.modern_ui = absl::GetFlag(FLAGS_modern_ui) && !absl::GetFlag(FLAGS_classic_ui);
    if (absl::GetFlag(FLAGS_military_advisor)) front::opt_military_advisor = 1;
    if (absl::GetFlag(FLAGS_no_military_advisor)) front::opt_military_advisor = 0;

    w2::ai::SetDefaultClassicAi(use_classic_ai);
    std::string weights_path = !weights_file.empty() ? weights_file
                               : w2::FileExists("cpp/ai_weights.json")
                                       ? std::string("cpp/ai_weights.json")
                               : w2::FileExists("ai_weights.json") ? std::string("ai_weights.json")
                                                                   : std::string();
    if (weights_path.empty() || !w2::ai::ml::ActivePolicy().LoadJson(weights_path).ok()) {
        fprintf(stderr,
                "warning: Modern AI weights %s%s%s could not be loaded (missing, or trained for a "
                "different network size); the Modern AI will play with untrained random weights. "
                "Retrain with cpp/test/train.sh or pass --weights PATH.\n",
                weights_path.empty() ? "(none found)" : "'", weights_path.c_str(),
                weights_path.empty() ? "" : "'");
    }
    if (!w2::FileExists(g_app.data_dir / "TERRAIN0/ARMYTYPE.DAT")) {
        return Fail("cannot find the game's files in '" + g_app.data_dir.string() +
                    "': run from the repository root, or pass --data DIR");
    }
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
        return Fail(std::string("SDL renderer: ") + SDL_GetError(), win);
    }
    gfx::Init(ren);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_StopTextInput();
    if (!script_file.empty()) {
        front::LoadScript(script_file);
    }
    if (absl::Status art_status = front::LoadArt(); !art_status.ok()) {
        return Fail("could not start: " + std::string(art_status.message()), win);
    }
    front::InstallHooks();
    auto ui_data = w2::uidata::Load(g_app.data_dir);
    if (!ui_data.ok()) {
        return Fail("could not start: " + std::string(ui_data.status().message()), win);
    }
    sound::Init(g_app.data_dir, ui_data->files);
    if (prefs::Get("4:3") == "on") {
        display::state.wanted = std::make_pair(layout::kScreenW, layout::kScreenH);
    }
    display::state.crt = prefs::Get("crt") == "on";
    display::state.chosen =
            prefs::Get("scale") == "smooth" ? display::kSmooth : prefs::GetInt("scale", 0);
    display::Measure();
    g_app.zoom = prefs::GetInt("zoom", display::state.scale);
    if (scenario.empty()) {
        front::OpenStart();
        if (script_file.empty()) advisor::Say(w2::cues::kGreet);
    } else {
        if (absl::Status cli_status = front::StartCliScenario(scenario, seed, force_diplomacy,
                                                              force_hidden_map, spectate_all_ai);
            !cli_status.ok()) {
            return Fail("could not start: " + std::string(cli_status.message()), win);
        }
    }

    front::InitPointerState(win);
    while (!g_app.quit_requested) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
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
        }
        front::PollFocusedMouse(win);
        display::state.min_h = g_app.starting || !g_app.g ? layout::kScreenH : layout::kShortH;
        display::Measure();
        display::BeginFrame();
        front::PadUpdate();
        front::SyncTyping();
        front::StepScript();
        front::Update();
        front::Draw();
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
