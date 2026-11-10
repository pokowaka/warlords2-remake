/**
 * @file
 * @brief Encapsulated game audio engine (`sound::AudioEngine`): `.8SN` digital effects,
 * advisor speech clips, and live OPL2 FM / SoundFont background music.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"

#include "warlords/uidata.hpp"

namespace w2 {
class Side;
}  // namespace w2

namespace sound {

/** @brief Music synthesizer backend selection. */
enum class SynthKind {
    kFm,    ///< Live emulated AdLib OPL2 FM synthesis (`S*.XMI`).
    kMt32,  ///< Live Roland MT-32 SoundFont synthesis (`M*.XMI`).
    kSc55,  ///< Live Roland Sound Canvas SC-55 SoundFont synthesis (`R*.XMI`).
};

/** @brief User-toggleable audio output channel persisted in `DATA/OPTIONS.SND`. */
enum class AudioChannel {
    kMusic,    ///< Background music tracks.
    kEffects,  ///< Digital `.8SN` sound effects.
    kSpeech,   ///< Digital advisor voice clips.
};

/**
 * @brief User-toggled audio switches persisted in `DATA/OPTIONS.SND`.
 */
struct Options {
    bool music = true;    ///< True when background music is enabled.
    bool effects = true;  ///< True when sound effects are enabled.
    bool speech = true;   ///< True when advisor voice clips are enabled.
};

/// Optional synthesizer selection passed when initializing `AudioEngine`.
using OptionalSynthKind = std::optional<SynthKind>;

/// Callback invoked on the main thread once a queued speech or effect clip completes.
using CompletionCallback = std::function<void()>;

/// Faction pointer list used to inspect whether a human side remains active.
using SidePtrList = std::vector<w2::Side*>;

/// Read-only span of faction pointers used to inspect whether a human side remains active.
using SidePtrSpan = absl::Span<w2::Side* const>;

/** @brief Formats `synth` as its lowercase prefs/menu token (`"fm"`, `"mt32"`, or `"sc55"`). */
std::string_view SynthName(SynthKind synth);

/** @brief Parses a prefs/menu token into a `SynthKind`, or `std::nullopt` if unrecognized. */
OptionalSynthKind ParseSynthKind(std::string_view name);

/**
 * @brief RAII audio subsystem owning an `SDL_AudioDeviceID`, mixer state, sample cache, and
 * sequential DIGPAK playback queue.
 *
 * Passes `this` via `SDL_AudioSpec::userdata` into `AudioCallback`, mixing live
 * AdLib OPL2 FM synthesis (`w2::AilFm`), live Roland Sound Canvas / MT-32 SoundFont
 * synthesis (`sound::SoundFontSynth`), and 11000 Hz `.8SN` digital clips per instance.
 *
 * Invariants:
 * - When `device_id_ != 0`, mutations to `*mixer_` on the main thread are
 *   serialized with `AudioCallback` via `ScopedDeviceLock` (`SDL_LockAudioDevice`).
 * - `~AudioEngine()` / `Shutdown()` closes `device_id_` before destroying
 *   `*mixer_` so no callback can run concurrently with teardown.
 *
 * Thread safety:
 * - Public methods are called on the main UI thread; `AudioCallback` runs on
 *   SDL's real-time audio thread guarded by SDL's device lock.
 */
class AudioEngine {
  public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /**
     * @brief Opens the SDL audio device and loads `FILE.DAT` metadata and `OPTIONS.SND` switches.
     *
     * Operates silently (invoking completion callbacks immediately) when no
     * audio device is available.
     *
     * @param data_dir Root directory containing `DATA/`, `SOUND/`, `ADLIB.ADV`, and `MIDPAK.AD`.
     * @param files Parsed `DATA/FILE.DAT` string groups.
     * @param preferred_synth Desired music backend (defaults to `kMt32` if installed, else `kFm`).
     */
    void Init(const std::filesystem::path& data_dir, const w2::uidata::Strings& files,
              OptionalSynthKind preferred_synth = std::nullopt);

    /** @brief Closes the SDL audio device and releases active streams and synthesizer state. */
    void Shutdown();

    /** @brief Read-only access to the active channel switches. */
    const Options& GetOptions() const { return options_; }
    /** @brief Mutable access to the active channel switches. */
    Options& MutableOptions() { return options_; }

    /** @brief Persists `options_` as three `'1'`/`'0'` characters in
     * `<data_dir_>/DATA/OPTIONS.SND`.
     */
    void SaveOptions() const;

    /** @brief Toggles `channel`, starts/stops music if needed, and writes `OPTIONS.SND`. */
    void Toggle(AudioChannel channel);

    /**
     * @brief Starts playing the song for `cue` unless that cue is already actively playing.
     * @param cue Music cue constant (`w2::cues::kTitle`, `kPlay`, `kComputer`, ...).
     * @param has_active_human True while at least one living human faction remains in play.
     */
    void Music(int cue, bool has_active_human = true);

    /**
     * @brief Starts playing the song for `cue`, deriving human presence from `sides`.
     * @param cue Music cue constant.
     * @param sides Participating scenario sides.
     */
    void Music(int cue, SidePtrSpan sides);

    /** @brief Stops any playing background music and clears active streams. */
    void StopMusic();

    /** @brief Returns the active music synthesizer backend. */
    SynthKind Synth() const { return synth_; }

    /** @brief Returns true when `synth` has its required asset files installed on disk. */
    bool SynthAvailable(SynthKind synth) const;

    /** @brief Switches the active music synthesizer to `synth` and restarts any playing song. */
    void SetSynth(SynthKind synth);

    /** @brief Queues sound effect `name` and returns its duration in seconds (`0` if silent). */
    double Effect(std::string_view name);

    /**
     * @brief Queues a random advisor voice clip from `FILE.DAT` `group` and runs `after` when done.
     * @return False if speech is disabled or the clip cannot be played.
     */
    bool Speak(int group, CompletionCallback after);

    /** @brief Returns true when advisor speech is enabled. */
    bool SpeechOn() const { return options_.speech; }

    /** @brief Returns true while a sample or speech clip is playing or queued. */
    bool Busy() const { return playing_.has_value() || !queue_.empty(); }

    /** @brief Per-frame audio pump: advances the sample queue, runs callbacks, and loops streams.
     */
    void Update();

  private:
    using PcmBuffer = std::vector<float>;
    using SharedPcm = std::shared_ptr<PcmBuffer>;
    using SampleCache = absl::flat_hash_map<std::string, SharedPcm>;
    using ByteData = std::vector<uint8_t>;
    using SongSpec = std::pair<std::string, bool>;
    using OptionalSongSpec = std::optional<SongSpec>;

    struct QueuedSample {
        SharedPcm pcm;            ///< Normalized mono PCM samples at 11000 Hz.
        CompletionCallback done;  ///< Main-thread completion callback.
        double ends_at = 0.0;     ///< Monotonic fallback deadline in seconds.
    };
    using SampleQueue = std::deque<QueuedSample>;
    using OptionalQueuedSample = std::optional<QueuedSample>;

    struct MixerState;

    static void AudioCallback(void* userdata, uint8_t* stream, int byte_len);
    void MixOutput(float* out, int frames);

    int RollDie(int sides);
    std::filesystem::path DataPath(std::string_view relative_name) const;
    std::filesystem::path ResolveSoundFontPath() const;
    bool EnsureSoundFontLoaded();
    SharedPcm LoadSample(std::string_view relative_name);
    void StopStreams();
    bool StartSong(const std::string& name, bool loop);
    double EnqueueSample(std::string_view relative_name, CompletionCallback done);

    std::filesystem::path data_dir_;            ///< Root game data directory (`original/`).
    w2::uidata::Strings files_;                 ///< `DATA/FILE.DAT` string table.
    Options options_{false, false, false};      ///< Active Music / Effects / Speech toggles.
    uint32_t device_id_ = 0;                    ///< Open `SDL_AudioDeviceID` (`0` when closed).
    std::unique_ptr<MixerState> mixer_;         ///< Audio-callback mixer state.
    SampleCache samples_;                       ///< Decoded `.8SN` float PCM cache.
    SampleQueue queue_;                         ///< Pending sequential digital samples.
    OptionalQueuedSample playing_;              ///< Currently playing digital sample.
    int cue_ = -1;                              ///< Currently playing music cue (`kNone` = `-1`).
    OptionalSongSpec song_;                     ///< Active `(xmi_filename, loop)` song descriptor.
    bool song_playing_ = false;                 ///< True while background music is playing.
    SynthKind synth_ = SynthKind::kFm;          ///< Selected music synthesizer backend.
    ByteData adv_;                              ///< Raw `ADLIB.ADV` driver image.
    ByteData ad_bank_;                          ///< Raw `MIDPAK.AD` timbre bank.
    std::mt19937 rng_{std::random_device{}()};  ///< Non-gameplay PRNG for clip/song variety.
};

}  // namespace sound

namespace w2 {
using sound::AudioEngine;
}  // namespace w2
