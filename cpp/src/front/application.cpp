#include "front/application.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include <SDL.h>

#include "front/layout.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/cues.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

namespace {

int FailStartup(const std::string& msg, SDL_Window* win = nullptr) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", msg.c_str(), win);
    return 1;
}

void ParseWindowSize(std::string_view spec, int& size_w, int& size_h) {
    size_t sep = spec.find('x');
    if (sep == std::string_view::npos) sep = spec.find('X');
    if (sep == std::string_view::npos) return;
    auto parsed_w = ToInt(spec.substr(0, sep));
    auto parsed_h = ToInt(spec.substr(sep + 1));
    if (parsed_w && parsed_h && *parsed_w > 0 && *parsed_h > 0) {
        size_w = static_cast<int>(*parsed_w);
        size_h = static_cast<int>(*parsed_h);
    }
}

}  // namespace

Application::Application()
        : prefs_(PreferencesStore::ForUserApp())
        , ui_context_(display_, renderer_, audio_, keyboard_, gamepad_, prefs_, assets_, viewport_,
                      modals_, controller_) {
    controller_.BindContext(ui_context_, script_player_);
}

Application::~Application() {
    controller_.ShutdownSession();
    gamepad_.Shutdown();
    audio_.Shutdown();
    display_.Close(false);
    renderer_.Shutdown();
    display_.Close(true);
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

int Application::Run(const ApplicationConfig& config) {
    Application app;
    return app.Execute(config);
}

int Application::Execute(const ApplicationConfig& config) {
    bool windowed = config.force_window       ? true
                    : config.force_fullscreen ? false
                                              : (prefs_.Get("screen") == "window");
    bool use_classic_ai = config.force_classic_ai  ? true
                          : config.force_modern_ai ? false
                                                   : (prefs_.Get("ai") == "classic");
    int size_w = 0;
    int size_h = 0;
    if (!config.window_size.empty()) {
        ParseWindowSize(config.window_size, size_w, size_h);
    }

    ui_context_.SetModernUi(config.modern_ui);
    ui_context_.SetOptMilitaryAdvisor(config.military_advisor_override);
    assets_.SetDataDir(config.data_dir);

    ai::SetDefaultClassicAi(use_classic_ai);
    std::string weights_path = !config.weights_file.empty() ? config.weights_file
                               : FileExists("cpp/ai_weights.json")
                                       ? std::string("cpp/ai_weights.json")
                               : FileExists("ai_weights.json") ? std::string("ai_weights.json")
                                                               : std::string();
    if (weights_path.empty() || !ai::ml::ActivePolicy().LoadJson(weights_path).ok()) {
        fprintf(stderr,
                "warning: Modern AI weights %s%s%s could not be loaded (missing, or trained for a "
                "different network size); the Modern AI will play with untrained random weights. "
                "Retrain with cpp/test/train.sh or pass --weights PATH.\n",
                weights_path.empty() ? "(none found)" : "'", weights_path.c_str(),
                weights_path.empty() ? "" : "'");
    }
    if (config.force_personalities >= 0) {
        ai::ml::ActivePolicy().SetUsePersonalities(config.force_personalities != 0);
    }
    if (config.warlord_lookahead) {
        ai::ml::ActivePolicy().SetWarlordLookahead(true);
    }

    if (!FileExists(config.data_dir / "TERRAIN0/ARMYTYPE.DAT")) {
        return FailStartup("cannot find the game's files in '" + config.data_dir.string() +
                           "': run from the repository root, or pass --data DIR");
    }

    pad::GamepadInput::ApplyHints();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        return FailStartup(std::string("SDL: ") + SDL_GetError());
    }
    sdl_initialized_ = true;

    SDL_Window* win = display_.Open("Warlords II", windowed, size_w, size_h);
    if (!win) {
        return FailStartup(std::string("SDL window: ") + SDL_GetError());
    }
    SDL_Renderer* sdl_ren = display_.CreateRenderer();
    if (!sdl_ren) {
        return FailStartup(std::string("SDL renderer: ") + SDL_GetError(), win);
    }
    renderer_.Init(sdl_ren, /*take_ownership=*/false);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_StopTextInput();

    if (!config.script_file.empty()) {
        script_player_.Load(config.script_file);
    }

    if (absl::Status art_status = assets_.Load(renderer_, config.data_dir); !art_status.ok()) {
        return FailStartup("could not start: " + std::string(art_status.message()), win);
    }
    assets_.ApplySavedShortcuts(prefs_.Get("shortcuts"));

    auto ui_data = uidata::Load(config.data_dir);
    if (!ui_data.ok()) {
        return FailStartup("could not start: " + std::string(ui_data.status().message()), win);
    }
    audio_.Init(config.data_dir, ui_data->files, sound::ParseSynthKind(prefs_.Get("music")));

    if (prefs_.Get("4:3") == "on") {
        display_.SetOriginalAspect(true);
    }
    display_.SetCrtEnabled(prefs_.Get("crt") == "on");
    display_.MutableState().chosen =
            prefs_.Get("scale") == "smooth" ? display::kSmooth : prefs_.GetInt("scale", 0);
    display_.Measure();
    viewport_.SetInitialZoom(prefs_.GetInt("zoom", display_.GetState().scale));

    if (config.scenario.empty()) {
        controller_.OpenStart();
        if (config.script_file.empty()) advisor::Say(ui_context_, cues::kGreet);
    } else {
        if (absl::Status cli_status = controller_.StartCliScenario(
                    config.scenario, config.seed, config.force_diplomacy, config.force_hidden_map,
                    config.spectate_all_ai);
            !cli_status.ok()) {
            return FailStartup("could not start: " + std::string(cli_status.message()), win);
        }
    }

    controller_.InitPointerState(win);
    while (!ui_context_.QuitRequested()) {
        PumpEvents();
        controller_.PollFocusedMouse(win);
        display_.MutableState().min_h = (ui_context_.Starting() || !ui_context_.GamePtr())
                                                ? layout::kScreenH
                                                : layout::kShortH;
        display_.Measure();
        display_.BeginFrame(renderer_);
        controller_.PadUpdate();
        controller_.SyncTyping();
        script_player_.Step(controller_);
        controller_.Update();
        controller_.Draw();
        display_.Present(renderer_);
    }

    controller_.ShutdownSession();
    return 0;
}

void Application::PumpEvents() {
    SDL_Window* win = display_.Window();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            ui_context_.RequestQuit();
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                display_.Measure();
                viewport_.InvalidateLayout();
            } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                display_.MutableState().mouse_inside = false;
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                keyboard_.Clear();
                controller_.PadLetGo();
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                SDL_ShowCursor(SDL_DISABLE);
            }
            break;
        case SDL_KEYDOWN: {
            std::string key = keys::KeyboardState::LoveName(event.key.keysym.sym);
            if (key.empty() || event.key.repeat) break;
            keyboard_.Press(key);
            controller_.Keypressed(key);
            break;
        }
        case SDL_KEYUP: {
            std::string key = keys::KeyboardState::LoveName(event.key.keysym.sym);
            if (!key.empty()) keyboard_.Release(key);
            break;
        }
        case SDL_TEXTINPUT:
            if (!keyboard_.IsDown({"lalt", "ralt", "lctrl", "rctrl", "lgui", "rgui"})) {
                controller_.Textinput(Utf8ToLatin1(event.text.text));
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
                SDL_RaiseWindow(win);
            }
            auto [x, y] = display_.ToUi(event.button.x, event.button.y);
            controller_.SetMousePos(x, y);
            int button = event.button.button == SDL_BUTTON_RIGHT    ? 2
                         : event.button.button == SDL_BUTTON_MIDDLE ? 3
                                                                    : 1;
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                controller_.Mousepressed(x, y, button);
            } else {
                controller_.Mousereleased(x, y, button);
            }
            break;
        }
        case SDL_MOUSEMOTION:
            controller_.HandleMouseMotion(event.motion.x, event.motion.y);
            break;
        case SDL_MOUSEWHEEL:
            if (event.wheel.y != 0) controller_.Wheelmoved(event.wheel.y > 0 ? 1 : -1);
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int button = 0;
            bool down = false;
            if (gamepad_.TranslateEvent(event, button, down)) {
                controller_.PadButton(button, down);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                controller_.PadLetGo();
            }
            break;
        }
        default:
            break;
        }
    }
}

}  // namespace w2
