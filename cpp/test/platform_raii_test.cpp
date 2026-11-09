/**
 * @file
 * @brief Hermetic GoogleTest suite for `PreferencesStore`, `AiTurnRunner`, and SDL2 RAII classes.
 */
#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <SDL.h>
#include <gtest/gtest.h>

#include "config/preferences_store.hpp"
#include "platform/ai_turn_runner.hpp"
#include "platform/crt.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "test/test_helpers.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/uidata.hpp"

namespace {

using EventList = std::vector<w2::TurnEvent>;

/**
 * @brief Test fixture configuring SDL2 dummy video/audio drivers so no OS window or device opens.
 */
class DummySdlTest : public ::testing::Test {
  protected:
    void SetUp() override {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        setenv("SDL_RENDER_DRIVER", "software", 1);
        ASSERT_EQ(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                           SDL_INIT_GAMECONTROLLER),
                  0)
                << SDL_GetError();
    }

    void TearDown() override { SDL_Quit(); }
};

/**
 * @brief Recording `ITurnObserver` capturing delivered `TurnEvent`s and optionally aborting at a
 * step index.
 */
class RecordingTurnObserver final : public w2::ITurnObserver {
  public:
    explicit RecordingTurnObserver(int abort_at_index = -1) : abort_at_index_(abort_at_index) {}

    w2::TurnControl OnTurnEvent(const w2::TurnEvent& event) override {
        int current_index = static_cast<int>(events_.size());
        events_.push_back(event);
        if (abort_at_index_ >= 0 && current_index == abort_at_index_) {
            return w2::TurnControl::kAbort;
        }
        return w2::TurnControl::kContinue;
    }

    const EventList& Events() const { return events_; }

  private:
    int abort_at_index_ = -1;  ///< Event index that triggers `TurnControl::kAbort`, or `-1`.
    EventList events_;         ///< Events observed on the main thread, in delivery order.
};

TEST(PreferencesStoreTest, RoundTripPersistenceAndTypeNormalization) {
    w2::test::ScopedTempPath temp_dir("w2_prefs_store_test");
    std::filesystem::create_directories(temp_dir.Path());
    std::filesystem::path prefs_file = temp_dir.Path() / "w2-prefs.json";

    {
        w2::PreferencesStore writer(prefs_file);
        EXPECT_EQ(writer.Get("missing"), "");
        EXPECT_EQ(writer.GetInt("zoom", 3), 3);

        writer.Set("screen", "window");
        writer.Set("crt", "on");
        writer.Set("zoom", 4);
        writer.Set("transient", "temp");
        writer.Set("transient", "");
        ASSERT_TRUE(writer.Save().ok());
    }

    {
        w2::PreferencesStore reader(prefs_file);
        EXPECT_EQ(reader.Get("screen"), "window");
        EXPECT_EQ(reader.Get("crt"), "on");
        EXPECT_EQ(reader.GetInt("zoom", 1), 4);
        EXPECT_EQ(reader.Get("transient"), "");
        EXPECT_EQ(reader.GetInt("screen", 7), 7);
    }

    // Verify integer and boolean values written externally normalize cleanly on reload.
    ASSERT_TRUE(w2::WriteFile(prefs_file, R"({"scale":2,"flag_on":true,"flag_off":false})").ok());
    w2::PreferencesStore external_reader(prefs_file);
    EXPECT_EQ(external_reader.GetInt("scale", 0), 2);
    EXPECT_EQ(external_reader.Get("flag_on"), "1");
    EXPECT_EQ(external_reader.Get("flag_off"), "0");
}

TEST(PreferencesStoreTest, UserDirectoryHonoursEnvironmentOverride) {
    w2::test::ScopedTempPath temp_user_dir("w2_user_dir_override");
    std::string dir_str = temp_user_dir.Path().string();
    setenv("W2_USER_DIR", dir_str.c_str(), 1);
    w2::PreferencesStore app_store = w2::PreferencesStore::ForUserApp();
    EXPECT_EQ(app_store.FilePath(),
              temp_user_dir.Path() / std::string(w2::PreferencesStore::kDefaultFileName));
}

TEST(AiTurnRunnerTest, MultiStepEventHandoffAndNormalCompletion) {
    int completed_phases = 0;
    w2::AiTurnRunner runner([&completed_phases](w2::ITurnObserver& observer) {
        if (observer.OnTurnEvent(w2::PhaseProgressEvent{w2::SideId(2), 10, "diplomacy"}) ==
            w2::TurnControl::kAbort) {
            return;
        }
        completed_phases++;

        w2::WalkEvent walk;
        walk.armies = {w2::ArmyId(11), w2::ArmyId(12)};
        walk.tiles = {w2::TilePos{4, 5}, w2::TilePos{5, 5}};
        walk.sea_before = true;
        if (observer.OnTurnEvent(walk) == w2::TurnControl::kAbort) {
            return;
        }
        completed_phases++;

        if (observer.OnTurnEvent(w2::PauseEvent{5, "Attacking Bane"}) == w2::TurnControl::kAbort) {
            return;
        }
        if (observer.OnTurnEvent(w2::AssaultEvent{w2::TilePos{5, 5}, w2::SideId(2), w2::SideId(0),
                                                  true, "Sirians defeated!"}) ==
            w2::TurnControl::kAbort) {
            return;
        }
        if (observer.OnTurnEvent(w2::FallenEvent{w2::SideId(0), "Lord Bane is defeated"}) ==
            w2::TurnControl::kAbort) {
            return;
        }
        if (observer.OnTurnEvent(w2::NoHumansEvent{}) == w2::TurnControl::kAbort) {
            return;
        }
        if (observer.OnTurnEvent(w2::TurnDoneEvent{w2::SideId(2), w2::SideId(3)}) ==
            w2::TurnControl::kAbort) {
            return;
        }
        completed_phases++;
    });

    EXPECT_FALSE(runner.Done());
    RecordingTurnObserver observer;
    runner.RunToCompletion(observer);
    EXPECT_TRUE(runner.Done());
    EXPECT_EQ(completed_phases, 3);
    ASSERT_EQ(observer.Events().size(), 7UZ);

    const auto* progress = std::get_if<w2::PhaseProgressEvent>(&observer.Events()[0]);
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->side, w2::SideId(2));
    EXPECT_EQ(progress->percent, 10);
    EXPECT_EQ(progress->phase, "diplomacy");

    const auto* walk = std::get_if<w2::WalkEvent>(&observer.Events()[1]);
    ASSERT_NE(walk, nullptr);
    ASSERT_EQ(walk->armies.size(), 2UZ);
    EXPECT_EQ(walk->armies[0], w2::ArmyId(11));
    EXPECT_EQ(walk->tiles[1], (w2::TilePos{5, 5}));
    EXPECT_TRUE(walk->sea_before);

    const auto* done = std::get_if<w2::TurnDoneEvent>(&observer.Events()[6]);
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->finished_side, w2::SideId(2));
    EXPECT_EQ(done->next_side, w2::SideId(3));
}

TEST(AiTurnRunnerTest, ObserverAbortUnwindsCleanlyWithoutExceptions) {
    int steps_after_yield = 0;
    bool saw_abort_signal = false;

    w2::AiTurnRunner runner([&steps_after_yield, &saw_abort_signal](w2::ITurnObserver& observer) {
        for (int step_idx = 0; step_idx < 5; step_idx++) {
            if (observer.OnTurnEvent(w2::PauseEvent{step_idx + 1, "step"}) ==
                w2::TurnControl::kAbort) {
                saw_abort_signal = observer.IsAborted();
                // Verify nested unwind checkpoints also return kAbort immediately without blocking.
                EXPECT_EQ(observer.OnTurnEvent(w2::TurnDoneEvent{}), w2::TurnControl::kAbort);
                return;
            }
            steps_after_yield++;
        }
    });

    RecordingTurnObserver aborting_observer(/*abort_at_index=*/1);
    runner.RunToCompletion(aborting_observer);

    EXPECT_TRUE(runner.Done());
    EXPECT_TRUE(saw_abort_signal);
    EXPECT_EQ(steps_after_yield, 1);
    EXPECT_EQ(aborting_observer.Events().size(), 2UZ);
}

TEST(AiTurnRunnerTest, DestructorMidTurnCancelsAndJoinsCleanly) {
    bool body_reached_tail = false;
    bool abort_observed = false;
    {
        w2::AiTurnRunner runner([&body_reached_tail, &abort_observed](w2::ITurnObserver& observer) {
            if (observer.OnTurnEvent(w2::PauseEvent{10, "first"}) == w2::TurnControl::kAbort) {
                abort_observed = true;
                return;
            }
            body_reached_tail = true;
        });

        w2::OptionalTurnEvent first_event = runner.Resume();
        ASSERT_TRUE(first_event.has_value());
        EXPECT_TRUE(std::holds_alternative<w2::PauseEvent>(*first_event));
        EXPECT_FALSE(runner.Done());
        // `runner` leaves scope here while parked at `first` — destructor must unwind cleanly.
    }
    EXPECT_TRUE(abort_observed);
    EXPECT_FALSE(body_reached_tail);
}

TEST_F(DummySdlTest, RendererTransformMathAndSafeImageDestructionAfterShutdown) {
    w2::DisplayWindow display_win;
    SDL_Window* win = display_win.Open("Warlords II Test", /*windowed=*/true, 640, 480);
    ASSERT_NE(win, nullptr);
    SDL_Renderer* sdl_ren = display_win.CreateRenderer();
    ASSERT_NE(sdl_ren, nullptr);

    gfx::ImageP surviving_image;
    {
        w2::Renderer renderer(sdl_ren, /*take_ownership=*/false);
        renderer.Begin();
        renderer.Push();
        renderer.Translate(16.0, 24.0);
        renderer.Scale(2.0, 3.0);
        renderer.Translate(5.0, 4.0);

        double dev_x = 0.0;
        double dev_y = 0.0;
        renderer.TransformPoint(10.0, 20.0, dev_x, dev_y);
        EXPECT_DOUBLE_EQ(dev_x, 16.0 + 2.0 * (5.0 + 10.0));
        EXPECT_DOUBLE_EQ(dev_y, 24.0 + 3.0 * (4.0 + 20.0));

        double inv_x = 0.0;
        double inv_y = 0.0;
        renderer.InverseTransformPoint(dev_x, dev_y, inv_x, inv_y);
        EXPECT_DOUBLE_EQ(inv_x, 10.0);
        EXPECT_DOUBLE_EQ(inv_y, 20.0);

        renderer.Pop();
        renderer.TransformPoint(10.0, 20.0, dev_x, dev_y);
        EXPECT_DOUBLE_EQ(dev_x, 10.0);
        EXPECT_DOUBLE_EQ(dev_y, 20.0);

        const uint8_t rgba_pixels[4 * 4] = {
            255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
        };
        surviving_image = renderer.NewImageRgba(2, 2, rgba_pixels);
        ASSERT_NE(surviving_image, nullptr);
        EXPECT_EQ(surviving_image->Width(), 2);
        EXPECT_EQ(surviving_image->Height(), 2);

        renderer.Clear(0.0F, 0.0F, 0.0F);
        renderer.SetColor(1.0F, 0.0F, 0.0F, 1.0F);
        renderer.Rectangle(gfx::kFill, 0.0, 0.0, 8.0, 8.0);
        int read_w = 0;
        int read_h = 0;
        gfx::ByteBuffer pixels = renderer.ReadPixels(read_w, read_h);
        EXPECT_GT(read_w, 0);
        EXPECT_GT(read_h, 0);
        ASSERT_GE(pixels.size(), 4UZ);
        EXPECT_EQ(pixels[0], 255);
        EXPECT_EQ(pixels[1], 0);
        EXPECT_EQ(pixels[2], 0);

        // Exercise DisplayWindow + CrtShader coordinate unwarping and frame presentation.
        display_win.Measure();
        display_win.SetFrame(display::kScreenW, display::kScreenH);
        display_win.SetCrtEnabled(true);
        auto ui_pt = display_win.ToUi(320.0, 240.0);
        EXPECT_NEAR(ui_pt.first, 320, 2);
        EXPECT_NEAR(ui_pt.second, 240, 2);
        auto win_pt = display_win.FromUi(ui_pt.first, ui_pt.second);
        auto roundtrip_ui = display_win.ToUi(win_pt.first, win_pt.second);
        EXPECT_EQ(roundtrip_ui, ui_pt);

        display_win.BeginFrame(renderer);
        renderer.Clear(0.1F, 0.2F, 0.3F);
        display_win.Present(renderer);

        renderer.Shutdown();
    }
    // Destroy display_win (and SDL_Renderer) BEFORE surviving_image leaves scope to verify that
    // Image's weak renderer handle prevents any use-after-free or invalid SDL_DestroyTexture call.
    display_win.Close();
    surviving_image.reset();
}

TEST_F(DummySdlTest, AudioEngineHermeticOptionsSynthAndDummyDeviceLifecycle) {
    w2::test::ScopedTempPath temp_data("w2_audio_engine_test");
    std::filesystem::create_directories(temp_data.Path() / "DATA");
    ASSERT_TRUE(w2::WriteFile(temp_data.Path() / "DATA/OPTIONS.SND", "101").ok());

    auto loaded_ui = w2::uidata::Load(w2::test::kData);
    ASSERT_TRUE(loaded_ui.ok()) << loaded_ui.status();

    EXPECT_EQ(sound::SynthName(sound::SynthKind::kFm), "fm");
    EXPECT_EQ(sound::SynthName(sound::SynthKind::kMt32), "mt32");
    EXPECT_EQ(sound::SynthName(sound::SynthKind::kSc55), "sc55");
    EXPECT_EQ(sound::ParseSynthKind("mt32"), sound::SynthKind::kMt32);
    EXPECT_EQ(sound::ParseSynthKind("unknown"), std::nullopt);

    w2::AudioEngine engine;
    engine.Init(temp_data.Path(), loaded_ui->files, sound::SynthKind::kFm);
    EXPECT_TRUE(engine.GetOptions().music);
    EXPECT_FALSE(engine.GetOptions().effects);
    EXPECT_TRUE(engine.GetOptions().speech);
    EXPECT_EQ(engine.Synth(), sound::SynthKind::kFm);
    EXPECT_TRUE(engine.SynthAvailable(sound::SynthKind::kFm));

    engine.Toggle(sound::AudioChannel::kEffects);
    EXPECT_TRUE(engine.GetOptions().effects);
    engine.Toggle(sound::AudioChannel::kMusic);
    EXPECT_FALSE(engine.GetOptions().music);

    auto persisted = w2::ReadFile(temp_data.Path() / "DATA/OPTIONS.SND");
    ASSERT_TRUE(persisted.ok()) << persisted.status();
    EXPECT_EQ(*persisted, "011");

    engine.Music(w2::cues::kComputer, /*has_active_human=*/false);
    engine.Update();
    engine.StopMusic();
    engine.Shutdown();
}

TEST_F(DummySdlTest, KeyboardAndGamepadInputRaiiState) {
    w2::KeyboardState keyboard;
    keyboard.Press("lshift");
    EXPECT_TRUE(keyboard.IsDown("lshift"));
    EXPECT_TRUE(keyboard.IsDown({"rshift", "lshift"}));
    keyboard.Release("lshift");
    EXPECT_FALSE(keyboard.IsDown("lshift"));
    keyboard.Press("return");
    keyboard.Clear();
    EXPECT_FALSE(keyboard.IsDown("return"));
    EXPECT_EQ(w2::KeyboardState::LoveName(SDLK_RETURN), "return");

    w2::GamepadInput gamepad;
    w2::GamepadInput::ApplyHints();
    EXPECT_EQ(w2::GamepadInput::ButtonFromName("lefttrigger"), pad::kLeftTrigger);
    EXPECT_EQ(w2::GamepadInput::ButtonFromName("righttrigger"), pad::kRightTrigger);

    pad::Sticks scripted{0.5, -0.25, 0.75, 0.0};
    gamepad.SetScriptedSticks(scripted);
    pad::Sticks read_back = gamepad.ReadSticks();
    EXPECT_DOUBLE_EQ(read_back.lx, 0.5);
    EXPECT_DOUBLE_EQ(read_back.ly, -0.25);
    EXPECT_DOUBLE_EQ(read_back.rx, 0.75);
    gamepad.SetScriptedSticks(pad::Sticks{});
    EXPECT_DOUBLE_EQ(gamepad.ReadSticks().lx, 0.0);

    gamepad.RecordWarp(120, 240);
    EXPECT_TRUE(gamepad.ConsumeWarp(120, 240));
    EXPECT_FALSE(gamepad.ConsumeWarp(120, 240));
}

TEST(AiTurnRunnerTest, PlayTurnObserverBridgeEmitsEventsAndAbortsEarlyOnRealGame) {
    class WorkerBridge final : public w2::IAiTurnEvents {
      public:
        explicit WorkerBridge(w2::SideId side_id, w2::ITurnObserver& out)
                : side_id_(side_id), out_(out) {}

        bool OnPhase(std::string_view phase) override {
            return out_.OnTurnEvent(w2::PhaseProgressEvent{side_id_, 50, std::string(phase)}) ==
                   w2::TurnControl::kContinue;
        }

        bool OnWalk(w2::Game&, const ArmyStack& stack, const TileCoords& walked,
                    bool sea_before) override {
            w2::WalkEvent walk;
            for (const w2::Army* army : stack) walk.armies.push_back(army->Id());
            for (const auto& [tile_x, tile_y] : walked) walk.tiles.emplace_back(tile_x, tile_y);
            walk.sea_before = sea_before;
            return out_.OnTurnEvent(walk) == w2::TurnControl::kContinue;
        }

        bool IsAborted() const override { return out_.IsAborted(); }

      private:
        w2::SideId side_id_;
        w2::ITurnObserver& out_;
    };

    w2::game::NewGameOptions opts;
    opts.seed = 42.0;
    auto created = w2::game::NewGame("original", "ERYTHEA", opts);
    ASSERT_TRUE(created.ok()) << created.status();
    std::unique_ptr<w2::Game> game = std::move(*created);
    for (w2::Side* side : game->Sides()) side->SetComputerControlled(true);
    w2::Side* first_side = w2::game::Begin(*game);
    ASSERT_NE(first_side, nullptr);

    bool returned_without_throwing = false;
    w2::AiTurnRunner runner(
            [&game, first_side, &returned_without_throwing](w2::ITurnObserver& worker_observer) {
                WorkerBridge bridge(first_side->Id(), worker_observer);
                w2::ai::PlayTurn(*game, *first_side, &bridge);
                returned_without_throwing = true;
            });

    RecordingTurnObserver abort_after_two(/*abort_at_index=*/2);
    runner.RunToCompletion(abort_after_two);
    EXPECT_TRUE(runner.Done());
    EXPECT_TRUE(returned_without_throwing);
    EXPECT_EQ(abort_after_two.Events().size(), 3UZ);
    EXPECT_EQ(game->ActiveAiEvents(), nullptr);
}

}  // namespace
