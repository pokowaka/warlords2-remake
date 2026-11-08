// The game's music, sound effects, and advisor voice, mixed in a single SDL
// audio callback (ported from love2d/sound.lua and love2d/musicthread.lua).
//
// Three switches in Game > Settings (stored in DATA/OPTIONS.SND as three '1'
// or '0' characters) control Music, Effects, and Speech.
//
// Sound effects and speech are 11000 Hz unsigned 8-bit mono DIGPAK samples
// queued one after another. AdLib music is synthesized live from S-prefixed
// .XMI files through the AIL driver and emulated OPL2 (`warlords/ailfm`).
// MT-32 and Sound Canvas songs stream from `pre-rendered-sound/<M|R><song>.ogg`
// via libvorbisfile, looping at the XMI sequence's tick length while the
// previous iteration's reverb tail rings out underneath.
#include "platform/sound.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <random>

#include <SDL.h>

#include "front/prefs.hpp"
#include "util/util.hpp"
#include "warlords/ailfm.hpp"
#include "warlords/cues.hpp"
#include "warlords/xmi.hpp"

#ifdef W2_VORBIS
#include <vorbis/vorbisfile.h>
#endif

namespace sound {

namespace {

constexpr int kSampleRate = 11000;
// The 9-voice OPL2 chip peaks around one-fifth of full scale (~10 dB below
// the digital samples); doubling the gain balances FM music with effects.
constexpr float kFmGain = 2.0F / 32768;

const std::map<std::string, char> kPrefix = {{"fm", 'S'}, {"mt32", 'M'}, {"sc55", 'R'}};

// Logical effect names mapped to (group, index) in FILE.DAT.
const std::map<std::string, std::pair<int, int>> kEffect = {
    {"army", {64, 0}},  {"army2", {64, 5}},    {"ding", {64, 1}},
    {"chord", {64, 2}}, {"dramatic", {37, 0}}, {"orch", {37, 1}},
    {"war", {38, 0}},   {"splash", {39, 0}},   {"turn", {63, 0}}};

using Pcm = std::shared_ptr<std::vector<float>>;

/**
 * @brief A queued digital sample and its completion callback on the main thread.
 */
struct Queued {
    Pcm pcm;                     ///< Normalized float PCM samples at `kSampleRate`.
    std::function<void()> done;  ///< Callback invoked on `Update()` once playback ends.
    double ends_at = 0;          ///< Wall-clock fallback deadline in case audio stalls.
};

#ifdef W2_VORBIS
/**
 * @brief Streaming Ogg Vorbis decoder state for pre-rendered MT-32 / SC-55 music.
 */
struct Stream {
    OggVorbis_File vorbis{};  ///< libvorbisfile handle.
    bool open = false;        ///< True when `vorbis` has been opened.
    bool eof = false;         ///< True once the end of the Ogg stream is reached.
    int channels = 1;         ///< Source channel count (1 or 2).
    double step = 1;          ///< Source frames per output mixer frame.
    std::vector<float> buf;   ///< Decoded interleaved stereo frames.
    size_t frames = 0;        ///< Number of stereo frames currently in `buf`.
    double pos = 0;           ///< Fractional read cursor into `buf` (in frames).
    double played = 0;        ///< Total seconds of audio mixed so far.
    ~Stream() {
        if (open) ov_clear(&vorbis);
    }
    /** @brief Decodes additional Vorbis packets until at least `need` frames lie ahead of `pos`. */
    void Fill(size_t need) {
        size_t consumed = static_cast<size_t>(pos);
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + std::min(buf.size(), consumed * 2));
            frames -= std::min(frames, consumed);
            pos -= consumed;
        }
        while (!eof && frames < need + 2) {
            float** pcm;
            int section;
            long count = ov_read_float(&vorbis, &pcm, 4096, &section);
            if (count <= 0) {
                eof = true;
                break;
            }
            for (long i = 0; i < count; i++) {
                float left = pcm[0][i];
                float right = channels > 1 ? pcm[1][i] : left;
                buf.push_back(left);
                buf.push_back(right);
            }
            frames += count;
        }
    }
    /** @brief True once all frames have been decoded and consumed. */
    bool Done() const { return eof && pos + 1 >= frames; }
};
#endif

/**
 * @brief State shared with the SDL audio callback (guarded by `SDL_LockAudioDevice`).
 */
struct Mixer {
    int rate = 44100;                           ///< Output device sample rate in Hz.
    Pcm sample;                                 ///< Currently playing `.8SN` sample.
    double sample_pos = 0;                      ///< Read cursor into `sample`.
    bool sample_ended = true;                   ///< True when `sample` has finished playing.
    std::unique_ptr<w2::AilFm> fm_synth;        ///< Live AdLib OPL2 synthesizer.
    std::shared_ptr<w2::xmi::Sequence> fm_seq;  ///< Active XMI sequence playing on `fm_synth`.
    std::vector<int16_t> fm_buf;                ///< Buffered OPL2 samples at `Opl::kRate`.
    double fm_pos = 0;                          ///< Read cursor into `fm_buf`.
    bool fm_was_playing = false;  ///< True when `fm_synth` was playing on the last callback.
    bool fm_ended = false;        ///< Latched when a non-looping FM song finishes.
#ifdef W2_VORBIS
    std::unique_ptr<Stream> rec;  ///< Active pre-rendered music stream.
    std::vector<std::unique_ptr<Stream>>
            tails;  ///< Fading reverb tails from previous loop iterations.
#endif
};

/**
 * @brief Module-wide audio subsystem state owned by the main thread.
 */
struct State {
    std::string data_dir;                              ///< Path to `original/`.
    std::string rec_dir;                               ///< Path to `pre-rendered-sound/`.
    w2::uidata::Strings files;                         ///< `FILE.DAT` string table.
    Options enabled{false, false, false};              ///< User-toggled audio switches.
    SDL_AudioDeviceID dev = 0;                         ///< Open SDL audio device ID (0 if none).
    Mixer mix;                                         ///< Audio thread mixer state.
    std::map<std::string, Pcm> samples;                ///< Cache of decoded `.8SN` samples.
    std::deque<Queued> queue;                          ///< Pending sample queue.
    std::optional<Queued> playing;                     ///< Currently playing sample entry.
    int cue = w2::kNone;                               ///< Currently playing music cue ID.
    std::optional<std::pair<std::string, bool>> song;  ///< Active `(filename, loop)` pair.
    bool song_playing = false;                         ///< True while a song is actively playing.
    std::string synth = "fm";  ///< Active synthesizer (`"fm"`, `"mt32"`, `"sc55"`).
    std::string adv;           ///< Contents of `ADLIB.ADV`.
    std::string ad_bank;       ///< Contents of `MIDPAK.AD`.
    double rec_at = 0;         ///< Loop point in seconds for `mix.rec`.
    bool rec_loop = false;     ///< True when `mix.rec` should loop at `rec_at`.
};
State sound_state;

std::mt19937 dice{std::random_device{}()};
/** @brief Returns a random integer in `1..n` for selecting among variant clips/songs. */
int Roll(int n) {
    return static_cast<int>(dice() % static_cast<unsigned>(std::max(1, n))) + 1;
}

/** @brief RAII guard locking the SDL audio device while mutating `sound_state.mix`. */
struct Lock {
    Lock() {
        if (sound_state.dev) SDL_LockAudioDevice(sound_state.dev);
    }
    ~Lock() {
        if (sound_state.dev) SDL_UnlockAudioDevice(sound_state.dev);
    }
};

// ------------------------------------------------------------- the callback

/** @brief Mixes the active `.8SN` digital sample into stereo `out` (`n` frames). */
void MixSample(float* out, int n) {
    Mixer& mix = sound_state.mix;
    if (!mix.sample || mix.sample_ended) return;
    const auto& pcm = *mix.sample;
    double step = static_cast<double>(kSampleRate) / mix.rate;
    for (int i = 0; i < n; i++) {
        size_t idx = static_cast<size_t>(mix.sample_pos);
        if (idx >= pcm.size()) {
            mix.sample_ended = true;
            return;
        }
        double frac = mix.sample_pos - idx;
        float val = static_cast<float>(pcm[idx] * (1 - frac) +
                                       (idx + 1 < pcm.size() ? pcm[idx + 1] : 0) * frac);
        out[static_cast<std::ptrdiff_t>(i) * 2] += val;
        out[i * 2 + 1] += val;
        mix.sample_pos += step;
    }
}

/** @brief Renders and mixes live OPL2 FM music into stereo `out` (`n` frames). */
void MixFm(float* out, int n) {
    Mixer& mix = sound_state.mix;
    if (!mix.fm_synth || mix.fm_synth->Idle()) {
        if (mix.fm_was_playing) mix.fm_ended = true;
        mix.fm_was_playing = false;
        return;
    }
    double step = w2::Opl::kRate / mix.rate;
    size_t need = static_cast<size_t>(std::ceil(mix.fm_pos + n * step)) + 2;
    size_t keep = static_cast<size_t>(mix.fm_pos);
    if (keep > 0) {
        mix.fm_buf.erase(mix.fm_buf.begin(),
                         mix.fm_buf.begin() + std::min(keep, mix.fm_buf.size()));
        mix.fm_pos -= keep;
        need -= keep;
    }
    if (mix.fm_buf.size() < need) {
        size_t from = mix.fm_buf.size();
        mix.fm_buf.resize(need);
        mix.fm_synth->Render(mix.fm_buf.data() + from, static_cast<int>(need - from));
    }
    for (int i = 0; i < n; i++) {
        size_t idx = static_cast<size_t>(mix.fm_pos);
        double frac = mix.fm_pos - idx;
        float val = static_cast<float>((mix.fm_buf[idx] * (1 - frac) + mix.fm_buf[idx + 1] * frac) *
                                       kFmGain);
        val = std::clamp(val, -1.0F, 1.0F);
        out[static_cast<std::ptrdiff_t>(i) * 2] += val;
        out[i * 2 + 1] += val;
        mix.fm_pos += step;
    }
    bool playing_now = mix.fm_synth->Playing();
    if (mix.fm_was_playing && !playing_now) mix.fm_ended = true;
    mix.fm_was_playing = playing_now;
}

#ifdef W2_VORBIS
/** @brief Decodes and mixes `stream` into stereo `out` (`n` frames at `rate` Hz). */
void MixStream(Stream& stream, float* out, int n, int rate) {
    if (stream.Done()) return;
    stream.Fill(static_cast<size_t>(std::ceil(n * stream.step)) + 2);
    for (int i = 0; i < n; i++) {
        size_t idx = static_cast<size_t>(stream.pos);
        if (idx + 1 >= stream.frames) {
            if (stream.eof) break;
            stream.Fill(static_cast<size_t>(std::ceil((n - i) * stream.step)) + 2);
            idx = static_cast<size_t>(stream.pos);
            if (idx + 1 >= stream.frames) break;
        }
        double frac = stream.pos - idx;
        out[static_cast<std::ptrdiff_t>(i) * 2] += static_cast<float>(
                stream.buf[idx * 2] * (1 - frac) + stream.buf[idx * 2 + 2] * frac);
        out[i * 2 + 1] += static_cast<float>(stream.buf[idx * 2 + 1] * (1 - frac) +
                                             stream.buf[idx * 2 + 3] * frac);
        stream.pos += stream.step;
    }
    stream.played += static_cast<double>(n) / rate;
}
#endif

/** @brief SDL audio callback mixing FM/streamed music and digital samples. */
void Callback(void*, Uint8* stream, int len) {
    float* out = reinterpret_cast<float*>(stream);
    int frames = len / static_cast<int>(sizeof(float) * 2);
    std::fill(out, out + static_cast<std::ptrdiff_t>(frames) * 2, 0.0F);
    MixFm(out, frames);
#ifdef W2_VORBIS
    Mixer& mix = sound_state.mix;
    if (mix.rec) MixStream(*mix.rec, out, frames, mix.rate);
    for (auto& tail : mix.tails) MixStream(*tail, out, frames, mix.rate);
#endif
    MixSample(out, frames);
    for (int i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0F, 1.0F);
}

// ------------------------------------------------------------- helpers

/** @brief Converts a DOS relative asset path (e.g. `"SOUND\\army.8SN"`) to a path under `data_dir`.
 */
std::string DataPath(const std::string& name) {
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return sound_state.data_dir + "/" + rel;
}

/** @brief Loads and caches an 8-bit unsigned `.8SN` sample as normalized float PCM. */
Pcm SampleData(const std::string& name) {
    auto found_it = sound_state.samples.find(name);
    if (found_it != sound_state.samples.end()) return found_it->second;
    Pcm pcm;
    if (auto bytes = w2::ReadFile(DataPath(name)); bytes && !bytes->empty()) {
        pcm = std::make_shared<std::vector<float>>(bytes->size());
        for (size_t i = 0; i < bytes->size(); i++)
            (*pcm)[i] = (static_cast<uint8_t>((*bytes)[i]) - 128) / 128.0F;
    }
    sound_state.samples[name] = pcm;
    return pcm;
}

/** @brief Writes the three audio toggle flags to `DATA/OPTIONS.SND`. */
void SaveOptionsFile() {
    std::string content;
    content += sound_state.enabled.music ? '1' : '0';
    content += sound_state.enabled.effects ? '1' : '0';
    content += sound_state.enabled.speech ? '1' : '0';
    w2::WriteFile(sound_state.data_dir + "/DATA/OPTIONS.SND", content);
}

/** @brief Builds the path to a pre-rendered `.ogg` recording for `synth` and XMI `name`. */
std::string RecordingPath(const std::string& synth, const std::string& name) {
    std::string base = w2::Upper(name);
    if (w2::EndsWith(base, ".XMI")) base = base.substr(0, base.size() - 4);
    return sound_state.rec_dir + "/" + kPrefix.at(synth) + base + ".ogg";
}

#ifdef W2_VORBIS
/** @brief Opens an Ogg Vorbis file at `path` for streaming, or returns nullptr. */
std::unique_ptr<Stream> OpenStream(const std::string& path) {
    auto stream = std::make_unique<Stream>();
    std::string real = w2::ResolvePath(path);
    if (real.empty() || ov_fopen(real.c_str(), &stream->vorbis) != 0) return nullptr;
    stream->open = true;
    vorbis_info* info = ov_info(&stream->vorbis, -1);
    stream->channels = info->channels;
    stream->step = static_cast<double>(info->rate) / sound_state.mix.rate;
    return stream;
}

/** @brief Main-thread check that seamlessly loops `mix.rec` at `rec_at` and prunes finished reverb
 * tails. */
void StepRecording() {
    Lock lock;
    Mixer& mix = sound_state.mix;
    auto& rec = mix.rec;
    if (rec && (rec->played >= sound_state.rec_at || rec->Done())) {
        mix.tails.push_back(std::move(rec));
        if (sound_state.rec_loop && sound_state.song)
            rec = OpenStream(RecordingPath(sound_state.synth, sound_state.song->first));
        else
            sound_state.song_playing = false;
    }
    mix.tails.erase(std::remove_if(mix.tails.begin(), mix.tails.end(),
                                   [](auto& tail) { return tail->Done(); }),
                    mix.tails.end());
    if (mix.tails.size() > 4) mix.tails.erase(mix.tails.begin(), mix.tails.end() - 4);
}
#endif

/** @brief Stops and releases any active Vorbis streams outside the audio lock. */
void StopRecordings() {
#ifdef W2_VORBIS
    std::unique_ptr<Stream> rec;
    std::vector<std::unique_ptr<Stream>> tails;
    {
        Lock lock;
        rec = std::move(sound_state.mix.rec);
        tails = std::move(sound_state.mix.tails);
        sound_state.mix.tails.clear();
    }
#endif
}

/** @brief Stops live OPL2 FM playback. */
void StopFm() {
    Lock lock;
    if (sound_state.mix.fm_synth) sound_state.mix.fm_synth->Stop();
    sound_state.mix.fm_was_playing = false;
    sound_state.mix.fm_ended = false;
}

/** @brief Starts playing song `name` (looping when `loop` is true) on the selected synthesizer. */
bool StartSong(const std::string& name, bool loop) {
    if (!sound_state.dev) return false;
#ifdef W2_VORBIS
    if (sound_state.synth != "fm") {
        // Read the XMI sequence alongside the recording to determine its exact loop tick length.
        auto bytes = w2::ReadFile(sound_state.data_dir + "/SOUND/" + kPrefix.at(sound_state.synth) +
                                  w2::Upper(name));
        auto seq = bytes ? w2::xmi::Parse(*bytes) : std::nullopt;
        auto stream = seq ? OpenStream(RecordingPath(sound_state.synth, name)) : nullptr;
        if (stream) {
            StopFm();
            StopRecordings();
            Lock lock;
            sound_state.mix.rec = std::move(stream);
            sound_state.rec_at = (seq->length + 1.0) / w2::xmi::kTickRate;
            sound_state.rec_loop = loop;
            sound_state.song = std::make_pair(name, loop);
            sound_state.song_playing = true;
            return true;
        }
    }
#endif
    // AdLib FM songs use the 'S'-prefixed XMI files (e.g. SINT12.XMI).
    if (sound_state.adv.empty() || sound_state.ad_bank.empty()) return false;
    auto bytes = w2::ReadFile(sound_state.data_dir + "/SOUND/S" + w2::Upper(name));
    auto seq = bytes ? w2::xmi::Parse(*bytes) : std::nullopt;
    if (!seq) return false;
    StopRecordings();
    auto shared = std::make_shared<w2::xmi::Sequence>(std::move(*seq));
    std::shared_ptr<w2::xmi::Sequence> old;
    {
        Lock lock;
        if (!sound_state.mix.fm_synth)
            sound_state.mix.fm_synth =
                    std::make_unique<w2::AilFm>(sound_state.adv, sound_state.ad_bank);
        old = sound_state.mix.fm_seq;
        sound_state.mix.fm_seq = shared;
        sound_state.mix.fm_synth->Play(shared.get(), loop);
        sound_state.mix.fm_was_playing = true;
        sound_state.mix.fm_ended = false;
    }
    sound_state.song = std::make_pair(name, loop);
    sound_state.song_playing = true;
    return true;
}

/** @brief Appends sample `name` to the playback queue and returns its duration in seconds. */
double Enqueue(const std::string& name, std::function<void()> done) {
    Pcm pcm = sound_state.dev ? SampleData(name) : nullptr;
    if (!pcm) {
        if (done) done();
        return 0;
    }
    sound_state.queue.push_back(Queued{pcm, std::move(done)});
    Update();
    return static_cast<double>(pcm->size()) / kSampleRate;
}

}  // namespace

// ------------------------------------------------------------- the interface

void Init(const std::string& data_dir, const w2::uidata::Strings& files) {
    sound_state.data_dir = data_dir;
    sound_state.files = files;
    sound_state.rec_dir = w2::FileExists("pre-rendered-sound")
                                  ? "pre-rendered-sound"
                                  : data_dir + "/../pre-rendered-sound";
    std::string opts = w2::ReadFile(data_dir + "/DATA/OPTIONS.SND").value_or("111");
    while (opts.size() < 3) opts += '1';
    sound_state.enabled.music = opts[0] == '1';
    sound_state.enabled.effects = opts[1] == '1';
    sound_state.enabled.speech = opts[2] != '0';
    // Default to MT-32 recordings when available, falling back to live AdLib FM.
    std::string synth = prefs::Get("music");
    if (synth.empty()) synth = "mt32";
    if (kPrefix.count(synth) && SynthAvailable(synth)) sound_state.synth = synth;
    sound_state.adv = w2::ReadFile(data_dir + "/ADLIB.ADV").value_or("");
    sound_state.ad_bank = w2::ReadFile(data_dir + "/MIDPAK.AD").value_or("");
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = Callback;
    sound_state.dev =
            SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!sound_state.dev) return;
    sound_state.mix.rate = have.freq;
    SDL_PauseAudioDevice(sound_state.dev, 0);
}

void Shutdown() {
    if (!sound_state.dev) return;
    SDL_CloseAudioDevice(sound_state.dev);
    sound_state.dev = 0;
#ifdef W2_VORBIS
    sound_state.mix.rec.reset();
    sound_state.mix.tails.clear();
#endif
    sound_state.mix.fm_synth.reset();
}

Options& MutableOptions() {
    return sound_state.enabled;
}
void SaveOptions() {
    SaveOptionsFile();
}

void Toggle(const std::string& which) {
    if (which == "music")
        sound_state.enabled.music = !sound_state.enabled.music;
    else if (which == "effects")
        sound_state.enabled.effects = !sound_state.enabled.effects;
    else if (which == "speech")
        sound_state.enabled.speech = !sound_state.enabled.speech;
    if (which == "music") {
        if (sound_state.enabled.music) {
            sound_state.cue = w2::kNone;
            Music(w2::cues::kPlay);
        } else {
            StopMusic();
        }
    }
    SaveOptionsFile();
}

void Music(int cue, const std::vector<w2::Side*>& sides) {
    if (!sound_state.enabled.music) return;
    if (cue == sound_state.cue && sound_state.song_playing) return;
    auto [name, loop] = w2::cues::Song(sound_state.files, cue, sides, Roll);
    if (!name.empty() && StartSong(name, loop)) sound_state.cue = cue;
}

void StopMusic() {
    sound_state.cue = w2::kNone;
    sound_state.song.reset();
    sound_state.song_playing = false;
    StopFm();
    StopRecordings();
}

std::string Synth() {
    return sound_state.synth;
}

bool SynthAvailable(const std::string& synth_name) {
    if (synth_name == "fm") return true;
#ifdef W2_VORBIS
    return kPrefix.count(synth_name) && w2::FileExists(RecordingPath(synth_name, "STARTUP.XMI"));
#else
    return false;
#endif
}

void SetSynth(const std::string& synth_name) {
    if (!kPrefix.count(synth_name) || synth_name == sound_state.synth) return;
    sound_state.synth = synth_name;
    prefs::Set("music", synth_name);
    if (sound_state.enabled.music && sound_state.song && sound_state.song_playing) {
        auto song = *sound_state.song;
        StartSong(song.first, song.second);
    }
}

double Effect(const std::string& name) {
    if (!sound_state.enabled.effects || sound_state.files.empty()) return 0;
    auto effect_it = kEffect.find(name);
    if (effect_it == kEffect.end()) return 0;
    auto [group, index] = effect_it->second;
    if (group >= static_cast<int>(sound_state.files.size()) ||
        index >= static_cast<int>(sound_state.files[group].size()))
        return 0;
    const std::string& file_name = sound_state.files[group][index];
    if (file_name.empty()) return 0;
    return Enqueue(file_name, nullptr);
}

bool Speak(int group, std::function<void()> after) {
    if (!sound_state.enabled.speech || sound_state.files.empty()) return false;
    auto [name, subtitle] = w2::cues::Clip(sound_state.files, group, Roll);
    (void)subtitle;
    if (name.empty() || !sound_state.dev || !SampleData(name)) return false;
    Enqueue(name, std::move(after));
    return true;
}

bool SpeechOn() {
    return sound_state.enabled.speech;
}

bool Busy() {
    return sound_state.playing.has_value() || !sound_state.queue.empty();
}

void Update() {
    if (sound_state.playing) {
        bool ended;
        {
            Lock lock;
            ended = sound_state.mix.sample_ended;
        }
        if (ended || w2::Now() >= sound_state.playing->ends_at) {
            auto done = std::move(sound_state.playing->done);
            sound_state.playing.reset();
            if (done) done();
        }
    }
    if (!sound_state.playing && !sound_state.queue.empty()) {
        Queued next_clip = std::move(sound_state.queue.front());
        sound_state.queue.pop_front();
        next_clip.ends_at =
                w2::Now() + static_cast<double>(next_clip.pcm->size()) / kSampleRate + 0.25;
        {
            Lock lock;
            sound_state.mix.sample = next_clip.pcm;
            sound_state.mix.sample_pos = 0;
            sound_state.mix.sample_ended = false;
        }
        sound_state.playing = std::move(next_clip);
    }
    {
        Lock lock;
        if (sound_state.mix.fm_ended) {
            sound_state.mix.fm_ended = false;
            if (sound_state.song && !sound_state.song->second) sound_state.song_playing = false;
        }
    }
#ifdef W2_VORBIS
    StepRecording();
#endif
}

}  // namespace sound
