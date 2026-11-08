// The game's sound: the samples (.8SN) queued one after another as DIGPAK
// plays them, the advisor's voice, and the music -- the AdLib songs
// synthesized live through the remake's own OPL2, or the MT-32 and Sound
// Canvas recordings streamed from pre-rendered-sound/. docs/re/sound.md.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "warlords/types.hpp"
#include "warlords/uidata.hpp"

namespace sound {

/** Open the audio device and read what the game plays: FILE.DAT's names,
 *  OPTIONS.SND's switches. Quiet if there is no device. */
void Init(const std::string& data_dir, const w2::uidata::Strings& files);
void Shutdown();

struct Options {
    bool music = true, effects = true, speech = true;
};
Options& MutableOptions();
/** Write OPTIONS.SND after a switch has changed. */
void SaveOptions();
void Toggle(const std::string& which);  // "music", "effects", "speech"

/** Play a cue's song (warlords/cues). */
void Music(int cue, const std::vector<w2::Side*>& sides = {});
void StopMusic();
/** The synthesizer the songs play on: "fm", "mt32" or "sc55". */
std::string Synth();
bool SynthAvailable(const std::string& s);
void SetSynth(const std::string& s);

/** Queue a named sample ("turn", "war", "ding", ...); how long it plays, in
 *  seconds, 0 if it does not. */
double Effect(const std::string& name);
/** The advisor says a FILE.DAT group; `after` runs when he has finished.
 *  False when there is nothing to say (no clip, speech off, no device). */
bool Speak(int group, std::function<void()> after);
bool SpeechOn();
/** Is a sample still sounding, or waiting to? */
bool Busy();
/** Called every frame: runs what finished playing. */
void Update();

}  // namespace sound
