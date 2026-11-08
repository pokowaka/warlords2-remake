// The advisor: a horned helmet that speaks (6dda:026f). docs/re/sound.md.
//
// He waits for any sample still sounding (255e:0736), then draws VOICE.PCK
// at (152, 15) through its mask, colour 10. While his clip plays he blinks:
// a count starts at dice(1, 30, 10), goes up a BIOS tick at a time, and past
// 40 it restarts at 0 and the eyes -- VOICEBIT.PCK, three 160x47 frames, open,
// half shut and shut -- go over his at (232, 261): half, shut, half, open, two
// ticks each. When the clip ends the game goes on. With Speech off he never
// appears.
#include "front/pckimage.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"

namespace advisor {

namespace {
const int kAtX = 152, kAtY = 15;
const int kEyesX = 232, kEyesY = 261, kEyesW = 160, kEyesH = 47;
const int kBlink[4] = {1, 2, 1, 0};  // VOICEBIT rows, 47 apart
const double kTick = 1 / 18.2;
gfx::ImageP voice_pic, voice_bits;

struct Helmet : kit::Modal {
    int group = 0;
    kit::Done after;
    bool waiting = true;
    double next_tick = 0;
    int count = 0;
    int blink_frame = 0, blink_ticks = 0;  // blinkFrame 0: not blinking

    void Start() {
        waiting = false;
        std::weak_ptr<Modal> self = weak_from_this();
        kit::Done a = after;
        bool spoke = sound::Speak(group, [self, a]() {
            if (auto s = self.lock()) kit::Pop(s.get());
            if (a) a();
        });
        if (!spoke) {
            kit::Pop(this);
            if (a) a();
            return;
        }
        count = 11 + front::Roll(30) - 1;
        next_tick = kit::Now() + kTick;
        blink_frame = 0;
    }

    void Update() override {
        if (waiting) {
            if (!sound::Busy()) Start();
            return;
        }
        double t = kit::Now();
        while (next_tick > 0 && t >= next_tick) {
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

    void Draw() override {
        if (waiting) return;
        if (!voice_pic) {
            voice_pic = pckimage::Load(G.data_dir + "/PICS/VOICE.PCK", G.palette, 10);
            voice_bits = pckimage::Load(G.data_dir + "/PICS/VOICEBIT.PCK", G.palette);
        }
        gfx::SetColor(1, 1, 1);
        gfx::Draw(voice_pic, kAtX, kAtY);
        if (blink_frame) {
            int row = kBlink[blink_frame - 1];
            gfx::Draw(voice_bits, gfx::NewQuad(0, row * kEyesH, kEyesW, kEyesH), kEyesX, kEyesY);
        }
    }
    // he holds the game while he speaks: input goes nowhere
};
}  // namespace

void Say(int group, kit::Done after) {
    if (group == w2::kNone || !sound::SpeechOn()) {
        if (after) after();
        return;
    }
    auto d = std::make_shared<Helmet>();
    d->group = group;
    d->after = after;
    kit::Push(d);
    if (!sound::Busy()) d->Start();
}

}  // namespace advisor
