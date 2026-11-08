// Game audio subsystem: `.8SN` sound effects queued sequentially (as DIGPAK
// played them), the advisor's speech clips, and background music — either live
// AdLib OPL2 synthesis (`warlords/ailfm`) or pre-rendered MT-32 / Sound Canvas
// Ogg Vorbis streams from `pre-rendered-sound/` (see `docs/re/sound.md`).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "warlords/types.hpp"
#include "warlords/uidata.hpp"

namespace sound {

/**
 * @brief Opens the SDL audio device and loads sound metadata (`FILE.DAT` sample
 * names and `OPTIONS.SND` toggles). Silent when no audio device is available.
 */
void Init(const std::filesystem::path& data_dir, const w2::uidata::Strings& files);
/** @brief Closes the audio device and releases active streams and synthesizer state. */
void Shutdown();

/**
 * @brief User-toggled audio switches persisted in `DATA/OPTIONS.SND`.
 */
struct Options {
    bool music = true;    ///< True when background music is enabled.
    bool effects = true;  ///< True when sound effects are enabled.
    bool speech = true;   ///< True when advisor voice clips are enabled.
};
/** @brief Returns a mutable reference to the active audio options. */
Options& MutableOptions();
/** @brief Persists the current audio options to `DATA/OPTIONS.SND`. */
void SaveOptions();
/** @brief Toggles `"music"`, `"effects"`, or `"speech"` and saves `OPTIONS.SND`. */
void Toggle(const std::string& which);

/** @brief Starts playing the song for `cue` (unless that cue is already playing). */
void Music(int cue, const std::vector<w2::Side*>& sides = {});
/** @brief Stops any playing background music. */
void StopMusic();
/** @brief Returns the active music synthesizer (`"fm"`, `"mt32"`, or `"sc55"`). */
std::string Synth();
/** @brief Returns true if synthesizer `synth_name` has its required assets available. */
bool SynthAvailable(const std::string& synth_name);
/** @brief Switches the active music synthesizer to `synth_name` and restarts the current song. */
void SetSynth(const std::string& synth_name);

/** @brief Queues sound effect `name` and returns its duration in seconds (0 if silent). */
double Effect(const std::string& name);
/**
 * @brief Queues a random advisor clip from `FILE.DAT` group `group` and runs
 * `after` when playback finishes. Returns false if speech is disabled or unavailable.
 */
bool Speak(int group, std::function<void()> after);
/** @brief Returns true when advisor speech is enabled. */
bool SpeechOn();
/** @brief Returns true while a sample or speech clip is playing or queued. */
bool Busy();
/** @brief Per-frame audio pump: advances the sample queue, fires completion callbacks, and loops
 * streams. */
void Update();

}  // namespace sound
