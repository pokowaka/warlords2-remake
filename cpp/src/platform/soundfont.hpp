/**
 * @file
 * @brief Real-time SoundFont synthesizer (`sound::SoundFontSynth`) using TinySoundFont (`tsf`).
 */
#pragma once

#include <cstdint>
#include <filesystem>

#include "absl/types/span.h"

#include "warlords/xmi.hpp"

struct tsf;

namespace sound {

/**
 * @brief Real-time SoundFont synthesizer rendering Roland Sound Canvas SC-55 and
 * Roland MT-32 XMIDI sequences using TinySoundFont.
 *
 * Invariants:
 * - Thread-safety: Not internally synchronized; callers must ensure mutations
 *   (Play/Stop/Load) and audio rendering (Render) do not race (e.g. guarded by
 *   the audio device lock in AudioEngine).
 * - Lifetime: When `Play(seq, ...)` is active, `*seq` must outlive playback.
 */
class SoundFontSynth {
  public:
    SoundFontSynth();
    ~SoundFontSynth();

    SoundFontSynth(const SoundFontSynth&) = delete;
    SoundFontSynth& operator=(const SoundFontSynth&) = delete;

    SoundFontSynth(SoundFontSynth&& other) noexcept;
    SoundFontSynth& operator=(SoundFontSynth&& other) noexcept;

    /**
     * @brief Loads an SF2 SoundFont file from disk.
     * @param path Path to the .sf2 file.
     * @param sample_rate Audio output rate (e.g. 44100).
     * @return True if loaded successfully.
     */
    bool Load(const std::filesystem::path& path, int sample_rate);

    /**
     * @brief Loads an SF2 SoundFont from an in-memory buffer.
     * @param data SoundFont binary file image.
     * @param sample_rate Audio output rate.
     * @return True if loaded successfully.
     */
    bool Load(absl::Span<const uint8_t> data, int sample_rate);

    /** @brief Returns true if a SoundFont is actively loaded in memory. */
    bool Loaded() const { return soundfont_ != nullptr; }

    /** @brief Sets or updates the audio output sample rate. */
    void SetSampleRate(int sample_rate);

    /**
     * @brief Starts playing an XMIDI sequence.
     * @param seq Parsed XMIDI sequence (must remain valid during playback).
     * @param is_mt32 True to apply Roland MT-32 patch mapping to General MIDI.
     * @param loop True to loop when reaching the end of the sequence.
     */
    void Play(const w2::xmi::Sequence* seq, bool is_mt32, bool loop);

    /** @brief Stops sequence playback and releases active notes. */
    void Stop();

    /** @brief True while a sequence is actively advancing ticks. */
    bool Playing() const { return playing_; }

    /** @brief True when nothing is playing and all voice release tails have completed. */
    bool Idle() const;

    /**
     * @brief Mixes the next `frames` of interleaved stereo float samples into `out`.
     * @param out Interleaved stereo float buffer (size >= frames * 2). Samples are accumulated.
     * @param frames Number of stereo frames.
     */
    void Render(float* out, int frames);

  private:
    void Interval();

    tsf* soundfont_ = nullptr;
    const w2::xmi::Sequence* seq_ = nullptr;
    int rate_ = 44100;
    size_t pos_ = 0;
    int tick_ = 0;
    double until_tick_ = 0.0;
    bool is_mt32_ = false;
    bool loop_ = false;
    bool playing_ = false;
};

}  // namespace sound
