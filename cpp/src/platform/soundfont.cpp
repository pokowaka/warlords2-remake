/**
 * @file
 * @brief Implementation of `sound::SoundFontSynth`.
 */
#include "platform/soundfont.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#define TSF_IMPLEMENTATION
#include "platform/third_party/tsf.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "util/util.hpp"

namespace sound {

namespace {

/**
 * @brief Roland MT-32 timbre number to General MIDI program number translation table.
 * Standard ScummVM / DOSBox Roland MT-32 patch map.
 */
constexpr std::array<uint8_t, 128> kMt32ToGm = {
      0,   1,   0,   2,   4,   4,   5,   3,  16,  17,  18,  16,  16,  19,  20,  21,
      6,   6,   6,   7,   7,   7,   8, 112,  62,  62,  63,  63,  38,  38,  39,  39,
     88,  95,  52,  98,  97,  99,  14,  54, 102,  96,  53, 102,  81, 100,  14,  80,
     48,  48,  49,  45,  41,  40,  42,  42,  43,  46,  45,  24,  25,  28,  27, 104,
     32,  32,  34,  33,  36,  37,  35,  35,  79,  73,  72,  72,  74,  75,  64,  65,
     66,  67,  71,  71,  68,  69,  70,  22,  56,  59,  57,  57,  60,  60,  58,  61,
     61,  11,  11,  98,  14,   9,  14,  13,  12, 107, 107,  77,  78,  78,  76,  76,
     47, 117, 127, 118, 118, 116, 115, 119, 115, 112,  55, 124, 123,   0,  14, 117
};

}  // namespace

SoundFontSynth::SoundFontSynth() = default;

SoundFontSynth::~SoundFontSynth() {
    if (soundfont_ != nullptr) {
        tsf_close(soundfont_);
        soundfont_ = nullptr;
    }
}

SoundFontSynth::SoundFontSynth(SoundFontSynth&& other) noexcept
    : soundfont_(std::exchange(other.soundfont_, nullptr)),
      seq_(std::exchange(other.seq_, nullptr)),
      rate_(other.rate_),
      pos_(other.pos_),
      tick_(other.tick_),
      until_tick_(other.until_tick_),
      is_mt32_(other.is_mt32_),
      loop_(other.loop_),
      playing_(other.playing_) {
    other.playing_ = false;
}

SoundFontSynth& SoundFontSynth::operator=(SoundFontSynth&& other) noexcept {
    if (this != &other) {
        if (soundfont_ != nullptr) {
            tsf_close(soundfont_);
        }
        soundfont_ = std::exchange(other.soundfont_, nullptr);
        seq_ = std::exchange(other.seq_, nullptr);
        rate_ = other.rate_;
        pos_ = other.pos_;
        tick_ = other.tick_;
        until_tick_ = other.until_tick_;
        is_mt32_ = other.is_mt32_;
        loop_ = other.loop_;
        playing_ = other.playing_;
        other.playing_ = false;
    }
    return *this;
}

bool SoundFontSynth::Load(const std::filesystem::path& path, int sample_rate) {
    if (soundfont_ != nullptr) {
        tsf_close(soundfont_);
        soundfont_ = nullptr;
    }
    rate_ = sample_rate;
    std::filesystem::path resolved = w2::ResolvePath(path);
    std::string path_str = resolved.empty() ? path.string() : resolved.string();
    soundfont_ = tsf_load_filename(path_str.c_str());
    if (soundfont_ == nullptr) {
        return false;
    }
    tsf_set_output(soundfont_, TSF_STEREO_INTERLEAVED, rate_, 0.0F);
    for (int ch = 0; ch < 16; ++ch) {
        tsf_channel_set_presetnumber(soundfont_, ch, 0, ch == 9 ? 1 : 0);
    }
    return true;
}

bool SoundFontSynth::Load(absl::Span<const uint8_t> data, int sample_rate) {
    if (soundfont_ != nullptr) {
        tsf_close(soundfont_);
        soundfont_ = nullptr;
    }
    rate_ = sample_rate;
    soundfont_ = tsf_load_memory(data.data(), static_cast<int>(data.size()));
    if (soundfont_ == nullptr) {
        return false;
    }
    tsf_set_output(soundfont_, TSF_STEREO_INTERLEAVED, rate_, 0.0F);
    for (int ch = 0; ch < 16; ++ch) {
        tsf_channel_set_presetnumber(soundfont_, ch, 0, ch == 9 ? 1 : 0);
    }
    return true;
}

void SoundFontSynth::SetSampleRate(int sample_rate) {
    rate_ = sample_rate;
    if (soundfont_ != nullptr) {
        tsf_set_output(soundfont_, TSF_STEREO_INTERLEAVED, rate_, 0.0f);
    }
}

void SoundFontSynth::Play(const w2::xmi::Sequence* seq, bool is_mt32, bool loop) {
    Stop();
    seq_ = seq;
    is_mt32_ = is_mt32;
    loop_ = loop;
    pos_ = 0;
    tick_ = 0;
    until_tick_ = 0.0;
    playing_ = (seq != nullptr && soundfont_ != nullptr);
}

void SoundFontSynth::Stop() {
    if (soundfont_ != nullptr) {
        tsf_reset(soundfont_);
        for (int i = 0; i < soundfont_->voiceNum; ++i) {
            soundfont_->voices[i].playingPreset = -1;
        }
    }
    playing_ = false;
    seq_ = nullptr;
}

bool SoundFontSynth::Idle() const {
    return !playing_ && (soundfont_ == nullptr || tsf_active_voice_count(soundfont_) == 0);
}

void SoundFontSynth::Interval() {
    if (!playing_ || seq_ == nullptr || soundfont_ == nullptr) return;
    int tick = tick_;
    while (pos_ < seq_->events.size() && seq_->events[pos_].t <= tick) {
        const auto& ev = seq_->events[pos_++];
        uint8_t st = ev.st & 0xF0;
        int ch = ev.st & 0x0F;
        if (st == 0x90) {
            if (ev.b > 0) {
                tsf_channel_note_on(soundfont_, ch, ev.a, static_cast<float>(ev.b) / 127.0F);
            } else {
                tsf_channel_note_off(soundfont_, ch, ev.a);
            }
        } else if (st == 0x80) {
            tsf_channel_note_off(soundfont_, ch, ev.a);
        } else if (st == 0xC0) {
            int preset = ev.a;
            if (is_mt32_ && ch != 9 && preset >= 0 && preset < static_cast<int>(kMt32ToGm.size())) {
                preset = kMt32ToGm[static_cast<size_t>(preset)];
            }
            tsf_channel_set_presetnumber(soundfont_, ch, preset, ch == 9 ? 1 : 0);
        } else if (st == 0xB0) {
            tsf_channel_midi_control(soundfont_, ch, ev.a, ev.b);
        } else if (st == 0xE0) {
            int pitch = ev.a | (ev.b << 7);
            tsf_channel_set_pitchwheel(soundfont_, ch, pitch);
        }
    }
    tick_++;
    if (pos_ >= seq_->events.size() && tick_ > seq_->length) {
        if (loop_) {
            pos_ = 0;
            tick_ = 0;
            for (int c = 0; c < 16; ++c) {
                tsf_channel_note_off_all(soundfont_, c);
            }
        } else {
            playing_ = false;
        }
    }
}

void SoundFontSynth::Render(float* out, int frames) {
    if (soundfont_ == nullptr) return;
    if (!playing_) {
        if (tsf_active_voice_count(soundfont_) > 0) {
            tsf_render_float(soundfont_, out, frames, 1);
        }
        return;
    }
    const double samples_per_tick = static_cast<double>(rate_) / w2::xmi::kTickRate;
    int done = 0;
    while (done < frames) {
        if (until_tick_ <= 0.0) {
            Interval();
            until_tick_ += samples_per_tick;
            if (!playing_) {
                int remaining = frames - done;
                if (remaining > 0 && tsf_active_voice_count(soundfont_) > 0) {
                    tsf_render_float(soundfont_, out + done * 2, remaining, 1);
                }
                break;
            }
        }
        int chunk = std::min(frames - done, static_cast<int>(std::ceil(until_tick_)));
        tsf_render_float(soundfont_, out + done * 2, chunk, 1);
        done += chunk;
        until_tick_ -= chunk;
    }
}

}  // namespace sound
