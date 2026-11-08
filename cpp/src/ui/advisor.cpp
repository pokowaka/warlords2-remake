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
#include "front/pckimage.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"

namespace advisor {

namespace {
const int kAtX = 152, kAtY = 15;  ///< Where the helmet is drawn.
/// Where the eyes go over the helmet, and the size of one VOICEBIT frame.
const int kEyesX = 232, kEyesY = 261, kEyesW = 160, kEyesH = 47;
const int kBlink[4] = {1, 2, 1, 0};  ///< VOICEBIT rows, 47 apart, for blink frames 1-4.
const double kTick = 1 / 18.2;       ///< One BIOS timer tick, in seconds.
gfx::ImageP voice_pic;               ///< VOICE.PCK, the helmet.
gfx::ImageP voice_bits;              ///< VOICEBIT.PCK, the eyes' frames.

/**
 * @brief The speaking helmet: holds the game while his clip plays.
 *
 * Input goes nowhere while he is up; he pops himself when the clip ends.
 */
struct Helmet : kit::Modal {
    int group = 0;         ///< The FILE.DAT group he says.
    kit::Done after;       ///< Runs once he has finished (or could not speak).
    bool waiting = true;   ///< Still waiting for an earlier sample to end.
    double next_tick = 0;  ///< When the next blink tick falls due, in kit::Now() seconds.
    int count = 0;         ///< Ticks towards the next blink; it blinks past 40.
    int blink_frame = 0;   ///< 1-4 while blinking (index into kBlink plus one), 0 when not.
    int blink_ticks = 0;   ///< Ticks the current blink frame has shown.

    /** @brief Starts the clip; with no clip to play he goes at once. */
    void Start() {
        waiting = false;
        std::weak_ptr<Modal> self = weak_from_this();
        kit::Done after_copy = after;
        bool spoke = sound::Speak(group, [self, after_copy]() {
            if (auto helmet = self.lock()) kit::Pop(helmet.get());
            if (after_copy) after_copy();
        });
        if (!spoke) {
            kit::Pop(this);
            if (after_copy) after_copy();
            return;
        }
        count = 11 + front::Roll(30) - 1;
        next_tick = kit::Now() + kTick;
        blink_frame = 0;
    }

    /** @brief Starts once the sound is free, then advances the blinking by the ticks gone by. */
    void Update() override {
        if (waiting) {
            if (!sound::Busy()) Start();
            return;
        }
        double now = kit::Now();
        while (next_tick > 0 && now >= next_tick) {
            next_tick += kTick;
            if (blink_frame) {
                blink_ticks++;
                if (blink_ticks >= 2) {
                    blink_ticks = 0;
                    blink_frame++;
                    if (blink_frame > 4) blink_frame = 0;
                }
            } else {
                count++;
                if (count > 40) {
                    count = 0;
                    blink_frame = 1;
                    blink_ticks = 0;
                }
            }
        }
    }

    /** @brief Draws the helmet, and the eyes over it while he blinks. */
    void Draw() override {
        if (waiting) return;
        if (!voice_pic) {
            voice_pic = pckimage::Load(G.data_dir + "/PICS/VOICE.PCK", G.palette, 10);
            voice_bits = pckimage::Load(G.data_dir + "/PICS/VOICEBIT.PCK", G.palette, 10);
        }
        gfx::SetColor(1, 1, 1);
        gfx::Draw(voice_pic, kAtX, kAtY);
        if (blink_frame) {
            int row = kBlink[blink_frame - 1];
            gfx::Draw(voice_bits, gfx::NewQuad(0, row * kEyesH, kEyesW, kEyesH), kEyesX, kEyesY);
        }
    }
};
}  // namespace

/**
 * @brief Has the advisor say a FILE.DAT group, then runs `after`.
 *
 * With no group or Speech off, `after` runs at once and he never appears.
 */
void Say(int group, kit::Done after) {
    if (group == w2::kNone || !sound::SpeechOn()) {
        if (after) after();
        return;
    }
    auto helmet = std::make_shared<Helmet>();
    helmet->group = group;
    helmet->after = after;
    kit::Push(helmet);
    if (!sound::Busy()) helmet->Start();
}

}  // namespace advisor
