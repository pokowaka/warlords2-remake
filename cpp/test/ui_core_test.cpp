/**
 * @file
 * @brief Hermetic GoogleTest suite for `//cpp:ui_core` (`ModalStack`, `Viewport`, `AssetCatalog`,
 * and `UiContext`) under SDL2's dummy video/audio drivers.
 */
#include <filesystem>
#include <memory>
#include <string>

#include <SDL.h>

#include "gtest/gtest.h"

#include "config/preferences_store.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "test/test_helpers.hpp"
#include "ui/asset_catalog.hpp"
#include "ui/kit.hpp"
#include "ui/modal_stack.hpp"
#include "ui/screen_actions.hpp"
#include "ui/ui_context.hpp"
#include "ui/viewport.hpp"

namespace {

class StubScreenActions final : public w2::IScreenActions {
  public:
    void OpenCity(w2::City*) override {}
    void OpenQuest() override {}
    void OpenStart() override {}
    void TakeLoaded(std::unique_ptr<w2::Game>) override {}
    void StartAssault(int, int, const w2::Battle&) override {}
    void PresentVictory(w2::City*, const std::string&) override {}
    void SelectAt(int, int) override {}
    void AfterSlotChange() override {}
    void PlayComputer() override {}
    void Say(const std::string& message) override { last_say_ = message; }
    void Quit() override {}
    int Roll(int) override { return 1; }
    bool MenuEnabled(const std::string&) override { return true; }
    void DrawStrategicMap(int, int, const w2::City*, bool, const OwnerOverrideList*, int,
                          int) override {}
    void DrawSiteMarkers(int, int, const w2::Site*) override {}
    void DrawVectorMap(int, int, w2::City*, int, bool) override {}
    void DrawHeroFigure(int, int, int, int, bool) override {}
    void DrawStrategicPanel(int, int, const w2::City*) override {}

    const std::string& LastSay() const { return last_say_; }

  private:
    std::string last_say_;
};

class DummyModal final : public kit::Modal {
  public:
    explicit DummyModal(w2::UiContext& ctx, bool is_banner = false, bool is_offer = false)
            : kit::Modal(ctx) {
        if (is_banner) {
            SetGreysMainControls(true);
            SetIsTurnBanner(true);
        }
        if (is_offer) {
            SetGreysMainControls(true);
            SetIsHeroOffer(true);
        }
    }
    void Draw() override {}
};

TEST(UiCoreTest, ModalStackLifecycleAndTurnOpenerPredicates) {
    const char* tmp_dir = std::getenv("TEST_TMPDIR");
    std::filesystem::path pref_path =
            std::filesystem::path(tmp_dir ? tmp_dir : "/tmp") / "ui_core_prefs.json";
    w2::PreferencesStore prefs(pref_path);
    display::DisplayWindow display;
    gfx::Renderer renderer;
    sound::AudioEngine audio;
    keys::KeyboardState keyboard;
    pad::GamepadInput gamepad;
    w2::AssetCatalog assets;
    w2::Viewport viewport;
    w2::ModalStack modals;
    StubScreenActions actions;
    w2::UiContext ctx(display, renderer, audio, keyboard, gamepad, prefs, assets, viewport, modals,
                      actions);

    EXPECT_TRUE(modals.Empty());
    auto banner = std::make_shared<DummyModal>(ctx, /*is_banner=*/true, /*is_offer=*/false);
    auto dialog = std::make_shared<DummyModal>(ctx, /*is_banner=*/false, /*is_offer=*/false);
    kit::Push(ctx, banner);
    kit::Push(ctx, dialog);

    EXPECT_EQ(modals.Size(), 2U);
    EXPECT_EQ(modals.Top(), dialog.get());
    EXPECT_TRUE(modals.AnyGreysMainControls());
    EXPECT_TRUE(modals.HasTurnBanner());
    EXPECT_FALSE(modals.HasHeroOffer());
    EXPECT_EQ(modals.DialogCountExcludingTurnOpeners(), 1U);

    dialog->PopSelf();
    EXPECT_EQ(modals.Top(), banner.get());
    EXPECT_EQ(modals.DialogCountExcludingTurnOpeners(), 0U);

    modals.Clear();
    EXPECT_TRUE(modals.Empty());
}

TEST(UiCoreTest, AssetCatalogViewportAndUiContextRoundTripUnderDummySdl) {
    ASSERT_EQ(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER), 0);

    const char* tmp_dir = std::getenv("TEST_TMPDIR");
    std::filesystem::path pref_path =
            std::filesystem::path(tmp_dir ? tmp_dir : "/tmp") / "ui_core_sdl_prefs.json";
    std::filesystem::remove(pref_path);

    {
        w2::PreferencesStore prefs(pref_path);
        display::DisplayWindow display;
        SDL_Window* win = display.Open("ui_core_test", /*windowed=*/true, 640, 480);
        ASSERT_NE(win, nullptr);
        SDL_Renderer* sdl_ren = display.CreateRenderer();
        ASSERT_NE(sdl_ren, nullptr);

        gfx::Renderer renderer(sdl_ren, /*take_ownership=*/false);
        sound::AudioEngine audio;
        keys::KeyboardState keyboard;
        pad::GamepadInput gamepad;
        w2::AssetCatalog assets;
        w2::Viewport viewport;
        w2::ModalStack modals;
        StubScreenActions actions;
        w2::UiContext ctx(display, renderer, audio, keyboard, gamepad, prefs, assets, viewport,
                          modals, actions);

        ASSERT_TRUE(assets.Load(renderer, w2::test::kData).ok());
        assets.ApplySavedShortcuts("[525,526,527,535]");
        EXPECT_EQ(assets.MainScreen().ui.shortcuts[0], 525);
        EXPECT_GT(assets.TextFont().LineHeight(), 0);

        display.Measure();
        auto game = w2::test::SetupGame("ERYTHEA");
        ctx.SetGame(game.get());
        viewport.SyncLayout(display, assets.MainScreen(), assets.TextFont(), game.get(), false);
        EXPECT_TRUE(viewport.HaveLayout());

        ctx.CentreOn(40, 50);
        auto [centre_x, centre_y] = ctx.ViewCentre();
        EXPECT_NEAR(centre_x, 40, 1);
        EXPECT_NEAR(centre_y, 50, 1);

        auto [ui_x, ui_y] = ctx.MapToUi(40.5, 50.5);
        auto [map_x, map_y] = viewport.UiToMap(ui_x, ui_y, display.GetState().scale);
        EXPECT_NEAR(map_x, 40.5, 1e-2);
        EXPECT_NEAR(map_y, 50.5, 1e-2);

        ctx.SetVectorSeeAll(true);
        EXPECT_TRUE(ctx.VectorSeeAll());
        ctx.SavePref("zoom", 3);
        EXPECT_EQ(prefs.GetInt("zoom"), 3);

        display.Close(false);
        renderer.Shutdown();
        display.Close(true);
    }

    SDL_Quit();
}

}  // namespace
