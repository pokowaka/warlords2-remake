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
        bool spoke = sound::speak(group, [self, a]() {
            if (auto s = self.lock()) kit::pop(s.get());
            if (a) a();
        });
        if (!spoke) {
            kit::pop(this);
            if (a) a();
            return;
        }
        count = 11 + front::roll(30) - 1;
        next_tick = kit::now() + kTick;
        blink_frame = 0;
    }

    void update() override {
        if (waiting) {
            if (!sound::busy()) Start();
            return;
        }
        double t = kit::now();
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

    void draw() override {
        if (waiting) return;
        if (!voice_pic) {
            voice_pic = pckimage::load(G.dataDir + "/PICS/VOICE.PCK", G.palette, 10);
            voice_bits = pckimage::load(G.dataDir + "/PICS/VOICEBIT.PCK", G.palette);
        }
        gfx::setColor(1, 1, 1);
        gfx::draw(voice_pic, kAtX, kAtY);
        if (blink_frame) {
            int row = kBlink[blink_frame - 1];
            gfx::draw(voice_bits, gfx::newQuad(0, row * kEyesH, kEyesW, kEyesH), kEyesX, kEyesY);
        }
    }
    // he holds the game while he speaks: input goes nowhere
};
}  // namespace

void say(int group, kit::Done after) {
    if (group == w2::NONE || !sound::speechOn()) {
        if (after) after();
        return;
    }
    auto d = std::make_shared<Helmet>();
    d->group = group;
    d->after = after;
    kit::push(d);
    if (!sound::busy()) d->Start();
}

}  // namespace advisor
