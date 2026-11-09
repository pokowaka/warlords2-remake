/**
 * @file
 * @brief The advisor: a horned helmet that speaks. See docs/re/sound.md.
 *
 * He waits for any sample still sounding, then draws VOICE.PCK at (152, 15)
 * through its mask, colour 10. While his clip plays he blinks: a count
 * starts at dice(1, 30, 10), goes up a BIOS tick at a time, and past 40 it
 * restarts at 0 and the eyes -- VOICEBIT.PCK, three 160x47 frames, open,
 * half shut and shut -- go over his at (232, 261): half, shut, half, open,
 * two ticks each. When the clip ends the game goes on. With Speech off he
 * never appears.
 */
#include <array>
#include <memory>
#include <utility>

#include "ui/dialogs.hpp"

namespace advisor {

namespace {
constexpr int kAtX = 152, kAtY = 15;  ///< Where the helmet is drawn.
/// Where the eyes go over the helmet, and the size of one VOICEBIT frame.
constexpr int kEyesX = 232, kEyesY = 261, kEyesW = 160, kEyesH = 47;
constexpr int kMaskKey = 10;  ///< Transparent mask palette index for VOICE*.PCK.
constexpr std::array<int, 4> kBlink = {1, 2, 1, 0};  ///< VOICEBIT rows, 47 apart, for frames 1-4.
constexpr double kTick = 1.0 / 18.2;                 ///< One BIOS timer tick, in seconds.

/**
 * @brief The speaking helmet: holds the game while his clip plays.
 *
 * Input goes nowhere while he is up; he pops himself when the clip ends.
 */
class Helmet final : public kit::Modal {
  public:
    Helmet(w2::UiContext& ctx, int group, kit::Done after)
            : kit::Modal(ctx), group_(group), after_(std::move(after)) {}

    /** @brief Starts the clip; with no clip to play he goes at once. */
    void Start() {
        w2::UiContext& ctx = Ctx();
        waiting_ = false;
        std::weak_ptr<Modal> self = weak_from_this();
        kit::Done after_copy = after_;
        bool spoke = ctx.Audio().Speak(group_, [self, after_copy]() {
            if (auto helmet = self.lock()) helmet->PopSelf();
            if (after_copy) after_copy();
        });
        if (!spoke) {
            PopSelf();
            if (after_copy) after_copy();
            return;
        }
        count_ = 11 + ctx.Actions().Roll(30) - 1;
        next_tick_ = kit::Now() + kTick;
        blink_frame_ = 0;
    }

    /** @brief Starts once the sound is free, then advances the blinking by the ticks gone by. */
    void Update() override {
        if (waiting_) {
            if (!Ctx().Audio().Busy()) Start();
            return;
        }
        double now = kit::Now();
        while (next_tick_ > 0 && now >= next_tick_) {
            next_tick_ += kTick;
            if (blink_frame_ != 0) {
                blink_ticks_++;
                if (blink_ticks_ >= 2) {
                    blink_ticks_ = 0;
                    blink_frame_++;
                    if (blink_frame_ > 4) blink_frame_ = 0;
                }
            } else {
                count_++;
                if (count_ > 40) {
                    count_ = 0;
                    blink_frame_ = 1;
                    blink_ticks_ = 0;
                }
            }
        }
    }

    /** @brief Draws the helmet, and the eyes over it while he blinks. */
    void Draw() override {
        if (waiting_) return;
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const auto& data_dir = ctx.Assets().DataDir();
        gfx::ImageP voice_pic =
                ctx.Assets().CachedImage(ren, data_dir / "PICS" / "VOICE.PCK", kMaskKey);
        gfx::ImageP voice_bits =
                ctx.Assets().CachedImage(ren, data_dir / "PICS" / "VOICEBIT.PCK", kMaskKey);
        ren.SetColor(1, 1, 1);
        ren.Draw(voice_pic, kAtX, kAtY);
        if (blink_frame_ != 0) {
            int row = kBlink[blink_frame_ - 1];
            ren.Draw(voice_bits, gfx::NewQuad(0, row * kEyesH, kEyesW, kEyesH), kEyesX, kEyesY);
        }
    }

  private:
    int group_ = 0;         ///< The FILE.DAT group he says.
    kit::Done after_;       ///< Runs once he has finished (or could not speak).
    bool waiting_ = true;   ///< Still waiting for an earlier sample to end.
    double next_tick_ = 0;  ///< When the next blink tick falls due, in kit::Now() seconds.
    int count_ = 0;         ///< Ticks towards the next blink; it blinks past 40.
    int blink_frame_ = 0;   ///< 1-4 while blinking (index into kBlink plus one), 0 when not.
    int blink_ticks_ = 0;   ///< Ticks the current blink frame has shown.
};
}  // namespace

/**
 * @brief Has the advisor say a FILE.DAT group, then runs `after`.
 *
 * With no group or Speech off, `after` runs at once and he never appears.
 */
void Say(w2::UiContext& ctx, int group, kit::Done after) {
    if (group == w2::kNone || !ctx.Audio().SpeechOn()) {
        if (after) after();
        return;
    }
    auto helmet = std::make_shared<Helmet>(ctx, group, std::move(after));
    kit::Push(ctx, helmet);
    if (!ctx.Audio().Busy()) helmet->Start();
}

}  // namespace advisor
