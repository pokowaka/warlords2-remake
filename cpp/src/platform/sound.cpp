// The game's music, effects and advisor voice, mixed in one SDL audio
// callback. A port of the Lua remake's sound.lua and musicthread.lua.
//
// Three switches, as Game > Settings has them and DATA/OPTIONS.SND keeps
// them -- three characters, '1' or '0', for Music, Effects and Speech.
//
// The samples are DIGPAK's: one plays at a time, and one asked for while
// another is sounding waits for it to end (255e:065e calls 255e:0736 first),
// so they queue here. Unsigned 8-bit mono at 11000 Hz.
//
// The AdLib music is synthesized live: the S-prefixed songs through the AIL
// driver into the emulated OPL2 (warlords/ailfm), rendered in the callback
// at the chip's own rate. The MT-32 and Sound Canvas songs are the
// recordings in pre-rendered-sound/<M|R><song>.ogg, streamed through
// libvorbisfile; tick 0 of the song is the recording's start, and a looping
// song starts over at its own length (from its .XMI) while the old one's
// tail rings out beneath.
//
// Everything is optional: with no audio device, or the files missing, the
// calls do nothing, and what waits on a sample does not wait.
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
// The chip's nine voices leave a lot of headroom: a song peaks around a
// fifth of full scale, some 10 dB under the samples. Twice as loud brings
// the two close (musicthread.lua).
constexpr float kFmGain = 2.0F / 32768;

const std::map<std::string, char> kPrefix = {{"fm", 'S'}, {"mt32", 'M'}, {"sc55", 'R'}};

// FILE.DAT's sample names, by what the game calls them for
const std::map<std::string, std::pair<int, int>> kEffect = {
    {"army", {64, 0}},  {"army2", {64, 5}},    {"ding", {64, 1}},
    {"chord", {64, 2}}, {"dramatic", {37, 0}}, {"orch", {37, 1}},
    {"war", {38, 0}},   {"splash", {39, 0}},   {"turn", {63, 0}}};

using Pcm = std::shared_ptr<std::vector<float>>;

// One sample in the queue: the main thread's view of it.
struct Queued {
    Pcm pcm;
    std::function<void()> done;
    double ends_at = 0;  // by the wall clock, should the device stall
};

#ifdef W2_VORBIS
// A recording being streamed.
struct Stream {
    OggVorbis_File vf{};
    bool open = false, eof = false;
    int channels = 1;
    double step = 1;         // source frames per output frame
    std::vector<float> buf;  // decoded frames, interleaved stereo
    size_t frames = 0;       // in buf
    double pos = 0;          // into buf
    double played = 0;       // seconds handed out
    ~Stream() {
        if (open) ov_clear(&vf);
    }
    // Decode more until at least `need` frames lie ahead of pos.
    void Fill(size_t need) {
        size_t at = static_cast<size_t>(pos);
        if (at > 0) {
            buf.erase(buf.begin(), buf.begin() + std::min(buf.size(), at * 2));
            frames -= std::min(frames, at);
            pos -= at;
        }
        while (!eof && frames < need + 2) {
            float** pcm;
            int section;
            long n = ov_read_float(&vf, &pcm, 4096, &section);
            if (n <= 0) {
                eof = true;
                break;
            }
            for (long i = 0; i < n; i++) {
                float l = pcm[0][i], r = channels > 1 ? pcm[1][i] : l;
                buf.push_back(l);
                buf.push_back(r);
            }
            frames += n;
        }
    }
    bool Done() const { return eof && pos + 1 >= frames; }
};
#endif

// Everything the callback touches; the main thread changes it only with
// the device locked.
struct Mixer {
    int rate = 44100;
    // the sample sounding
    Pcm sample;
    double sample_pos = 0;
    bool sample_ended = true;
    // the FM music
    std::unique_ptr<w2::AilFm> fm;
    std::shared_ptr<w2::xmi::Sequence> fm_seq;
    std::vector<int16_t> fm_buf;
    double fm_pos = 0;
    bool fm_was_playing = false, fm_ended = false;
#ifdef W2_VORBIS
    std::unique_ptr<Stream> rec;
    std::vector<std::unique_ptr<Stream>> tails;
#endif
};

struct State {
    std::string data_dir, rec_dir;
    w2::uidata::Strings files;
    Options on{false, false, false};
    SDL_AudioDeviceID dev = 0;
    Mixer mix;
    std::map<std::string, Pcm> samples;  // file name -> PCM, or null when missing
    std::deque<Queued> queue;
    std::optional<Queued> playing;
    int cue = w2::NONE;
    std::optional<std::pair<std::string, bool>> song;
    bool song_playing = false;
    std::string synth = "fm";
    std::string adv, ad;
    // the recording's loop point, and whether it loops
    double rec_at = 0;
    bool rec_loop = false;
};
State sound_state;

std::mt19937 dice{std::random_device{}()};
int Roll(int n) {
    return static_cast<int>(dice() % static_cast<unsigned>(std::max(1, n))) + 1;
}

struct Lock {
    Lock() {
        if (sound_state.dev) SDL_LockAudioDevice(sound_state.dev);
    }
    ~Lock() {
        if (sound_state.dev) SDL_UnlockAudioDevice(sound_state.dev);
    }
};

// ------------------------------------------------------------- the callback

void MixSample(float* out, int n) {
    Mixer& m = sound_state.mix;
    if (!m.sample || m.sample_ended) return;
    const auto& p = *m.sample;
    double step = static_cast<double>(kSampleRate) / m.rate;
    for (int i = 0; i < n; i++) {
        size_t k = static_cast<size_t>(m.sample_pos);
        if (k >= p.size()) {
            m.sample_ended = true;
            return;
        }
        double f = m.sample_pos - k;
        float v = static_cast<float>(p[k] * (1 - f) + (k + 1 < p.size() ? p[k + 1] : 0) * f);
        out[static_cast<std::ptrdiff_t>(i) * 2] += v;
        out[i * 2 + 1] += v;
        m.sample_pos += step;
    }
}

void MixFm(float* out, int n) {
    Mixer& m = sound_state.mix;
    if (!m.fm || m.fm->idle()) {
        if (m.fm_was_playing) m.fm_ended = true;
        m.fm_was_playing = false;
        return;
    }
    double step = w2::Opl::RATE / m.rate;
    size_t need = static_cast<size_t>(std::ceil(m.fm_pos + n * step)) + 2;
    // keep what is still ahead, render the rest
    size_t keep = static_cast<size_t>(m.fm_pos);
    if (keep > 0) {
        m.fm_buf.erase(m.fm_buf.begin(), m.fm_buf.begin() + std::min(keep, m.fm_buf.size()));
        m.fm_pos -= keep;
        need -= keep;
    }
    if (m.fm_buf.size() < need) {
        size_t from = m.fm_buf.size();
        m.fm_buf.resize(need);
        m.fm->render(m.fm_buf.data() + from, static_cast<int>(need - from));
    }
    for (int i = 0; i < n; i++) {
        size_t k = static_cast<size_t>(m.fm_pos);
        double f = m.fm_pos - k;
        float v = static_cast<float>((m.fm_buf[k] * (1 - f) + m.fm_buf[k + 1] * f) * kFmGain);
        v = std::clamp(v, -1.0F, 1.0F);
        out[static_cast<std::ptrdiff_t>(i) * 2] += v;
        out[i * 2 + 1] += v;
        m.fm_pos += step;
    }
    bool now = m.fm->playing();
    if (m.fm_was_playing && !now) m.fm_ended = true;
    m.fm_was_playing = now;
}

#ifdef W2_VORBIS
void MixStream(Stream& s, float* out, int n, int rate) {
    if (s.Done()) return;
    s.Fill(static_cast<size_t>(std::ceil(n * s.step)) + 2);
    for (int i = 0; i < n; i++) {
        size_t k = static_cast<size_t>(s.pos);
        if (k + 1 >= s.frames) {
            if (s.eof) break;
            s.Fill(static_cast<size_t>(std::ceil((n - i) * s.step)) + 2);
            k = static_cast<size_t>(s.pos);
            if (k + 1 >= s.frames) break;
        }
        double f = s.pos - k;
        out[static_cast<std::ptrdiff_t>(i) * 2] +=
                static_cast<float>(s.buf[k * 2] * (1 - f) + s.buf[k * 2 + 2] * f);
        out[i * 2 + 1] += static_cast<float>(s.buf[k * 2 + 1] * (1 - f) + s.buf[k * 2 + 3] * f);
        s.pos += s.step;
    }
    s.played += static_cast<double>(n) / rate;
}
#endif

void Callback(void*, Uint8* stream, int len) {
    float* out = reinterpret_cast<float*>(stream);
    int n = len / static_cast<int>(sizeof(float) * 2);
    std::fill(out, out + static_cast<std::ptrdiff_t>(n) * 2, 0.0F);
    MixFm(out, n);
#ifdef W2_VORBIS
    Mixer& m = sound_state.mix;
    if (m.rec) MixStream(*m.rec, out, n, m.rate);
    for (auto& t : m.tails) MixStream(*t, out, n, m.rate);
#endif
    MixSample(out, n);
    for (int i = 0; i < n * 2; i++) out[i] = std::clamp(out[i], -1.0F, 1.0F);
}

// ------------------------------------------------------------- helpers

// "SOUND\\army.8SN" -> <data>/SOUND/army.8SN, found ignoring case
std::string DataPath(const std::string& name) {
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return sound_state.data_dir + "/" + rel;
}

Pcm SampleData(const std::string& name) {
    auto it = sound_state.samples.find(name);
    if (it != sound_state.samples.end()) return it->second;
    Pcm p;
    if (auto bytes = w2::readFile(DataPath(name)); bytes && !bytes->empty()) {
        p = std::make_shared<std::vector<float>>(bytes->size());
        for (size_t i = 0; i < bytes->size(); i++)
            (*p)[i] = (static_cast<uint8_t>((*bytes)[i]) - 128) / 128.0F;
    }
    sound_state.samples[name] = p;
    return p;
}

void SaveOptionsFile() {
    std::string s;
    s += sound_state.on.music ? '1' : '0';
    s += sound_state.on.effects ? '1' : '0';
    s += sound_state.on.speech ? '1' : '0';
    w2::writeFile(sound_state.data_dir + "/DATA/OPTIONS.SND", s);
}

// "INT12.XMI" -> <recordings>/RINT12.ogg
std::string RecordingPath(const std::string& synth, const std::string& name) {
    std::string n = w2::upper(name);
    if (w2::endsWith(n, ".XMI")) n = n.substr(0, n.size() - 4);
    return sound_state.rec_dir + "/" + kPrefix.at(synth) + n + ".ogg";
}

#ifdef W2_VORBIS
std::unique_ptr<Stream> OpenStream(const std::string& path) {
    auto s = std::make_unique<Stream>();
    std::string real = w2::resolvePath(path);
    if (real.empty() || ov_fopen(real.c_str(), &s->vf) != 0) return nullptr;
    s->open = true;
    vorbis_info* vi = ov_info(&s->vf, -1);
    s->channels = vi->channels;
    s->step = static_cast<double>(vi->rate) / sound_state.mix.rate;
    return s;
}

// the main thread's half of a recording reaching its loop point
void StepRecording() {
    Lock lock;
    Mixer& m = sound_state.mix;
    auto& r = m.rec;
    if (r && (r->played >= sound_state.rec_at || r->Done())) {
        m.tails.push_back(std::move(r));
        if (sound_state.rec_loop && sound_state.song)
            r = OpenStream(RecordingPath(sound_state.synth, sound_state.song->first));
        else
            sound_state.song_playing = false;
    }
    m.tails.erase(std::remove_if(m.tails.begin(), m.tails.end(), [](auto& t) { return t->Done(); }),
                  m.tails.end());
    if (m.tails.size() > 4) m.tails.erase(m.tails.begin(), m.tails.end() - 4);
}
#endif

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

void StopFm() {
    Lock lock;
    if (sound_state.mix.fm) sound_state.mix.fm->stop();
    sound_state.mix.fm_was_playing = false;
    sound_state.mix.fm_ended = false;
}

// Start a song, `loop`ing or not, on the synthesizer chosen. False when it
// cannot play: the recording, or the FM driver, missing.
bool StartSong(const std::string& name, bool loop) {
    if (!sound_state.dev) return false;
#ifdef W2_VORBIS
    if (sound_state.synth != "fm") {
        // the recording, and the song's own length from the file it was made
        // of: the driver starts a loop again one tick after it
        auto bytes = w2::readFile(sound_state.data_dir + "/SOUND/" + kPrefix.at(sound_state.synth) +
                                  w2::upper(name));
        auto seq = bytes ? w2::xmi::parse(*bytes) : std::nullopt;
        auto stream = seq ? OpenStream(RecordingPath(sound_state.synth, name)) : nullptr;
        if (stream) {
            StopFm();
            StopRecordings();
            Lock lock;
            sound_state.mix.rec = std::move(stream);
            sound_state.rec_at = (seq->length + 1.0) / w2::xmi::TICK_RATE;
            sound_state.rec_loop = loop;
            sound_state.song = std::make_pair(name, loop);
            sound_state.song_playing = true;
            return true;
        }
    }
#endif
    // the FM version's files carry an S: SINT12.XMI (255e:032f)
    if (sound_state.adv.empty() || sound_state.ad.empty()) return false;
    auto bytes = w2::readFile(sound_state.data_dir + "/SOUND/S" + w2::upper(name));
    auto seq = bytes ? w2::xmi::parse(*bytes) : std::nullopt;
    if (!seq) return false;
    StopRecordings();
    auto shared = std::make_shared<w2::xmi::Sequence>(std::move(*seq));
    std::shared_ptr<w2::xmi::Sequence> old;
    {
        Lock lock;
        if (!sound_state.mix.fm)
            sound_state.mix.fm = std::make_unique<w2::AilFm>(sound_state.adv, sound_state.ad);
        old = sound_state.mix.fm_seq;
        sound_state.mix.fm_seq = shared;
        sound_state.mix.fm->play(shared.get(), loop);
        sound_state.mix.fm_was_playing = true;
        sound_state.mix.fm_ended = false;
    }
    sound_state.song = std::make_pair(name, loop);
    sound_state.song_playing = true;
    return true;
}

// queue a sample behind whatever is sounding; `done` runs once it has ended
double Enqueue(const std::string& name, std::function<void()> done) {
    Pcm p = sound_state.dev ? SampleData(name) : nullptr;
    if (!p) {
        if (done) done();
        return 0;
    }
    sound_state.queue.push_back(Queued{p, std::move(done)});
    update();
    return static_cast<double>(p->size()) / kSampleRate;
}

}  // namespace

// ------------------------------------------------------------- the interface

void init(const std::string& data_dir, const w2::uidata::Strings& files) {
    sound_state.data_dir = data_dir;
    sound_state.files = files;
    sound_state.rec_dir = w2::fileExists("pre-rendered-sound")
                                  ? "pre-rendered-sound"
                                  : data_dir + "/../pre-rendered-sound";
    std::string opts = w2::readFile(data_dir + "/DATA/OPTIONS.SND").value_or("111");
    while (opts.size() < 3) opts += '1';
    sound_state.on.music = opts[0] == '1';
    sound_state.on.effects = opts[1] == '1';
    sound_state.on.speech = opts[2] != '0';
    // the MT-32 unless another was picked; AdLib when its recordings are missing
    std::string synth = prefs::get("music");
    if (synth.empty()) synth = "mt32";
    if (kPrefix.count(synth) && synthAvailable(synth)) sound_state.synth = synth;
    sound_state.adv = w2::readFile(data_dir + "/ADLIB.ADV").value_or("");
    sound_state.ad = w2::readFile(data_dir + "/MIDPAK.AD").value_or("");
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

void shutdown() {
    if (!sound_state.dev) return;
    SDL_CloseAudioDevice(sound_state.dev);
    sound_state.dev = 0;
#ifdef W2_VORBIS
    sound_state.mix.rec.reset();
    sound_state.mix.tails.clear();
#endif
    sound_state.mix.fm.reset();
}

Options& options() {
    return sound_state.on;
}
void saveOptions() {
    SaveOptionsFile();
}

// Turn one switch over (64d2:0576). Music turned on starts the play music;
// turned off it stops at once.
void toggle(const std::string& which) {
    if (which == "music")
        sound_state.on.music = !sound_state.on.music;
    else if (which == "effects")
        sound_state.on.effects = !sound_state.on.effects;
    else if (which == "speech")
        sound_state.on.speech = !sound_state.on.speech;
    if (which == "music") {
        if (sound_state.on.music) {
            sound_state.cue = w2::NONE;
            music(w2::cues::PLAY);
        } else {
            stopMusic();
        }
    }
    SaveOptionsFile();
}

// Play a cue -- unless it is the one already playing, which goes on
// undisturbed (6dda:0000).
void music(int cue, const std::vector<w2::Side*>& sides) {
    if (!sound_state.on.music) return;
    if (cue == sound_state.cue && sound_state.song_playing) return;
    auto [name, loop] = w2::cues::song(sound_state.files, cue, sides, Roll);
    if (!name.empty() && StartSong(name, loop)) sound_state.cue = cue;
}

void stopMusic() {
    sound_state.cue = w2::NONE;
    sound_state.song.reset();
    sound_state.song_playing = false;
    StopFm();
    StopRecordings();
}

std::string synth() {
    return sound_state.synth;
}

// FM wants ADLIB.ADV; the others their recordings, and libvorbisfile.
bool synthAvailable(const std::string& s) {
    if (s == "fm") return true;
#ifdef W2_VORBIS
    return kPrefix.count(s) && w2::fileExists(RecordingPath(s, "STARTUP.XMI"));
#else
    return false;
#endif
}

void setSynth(const std::string& s) {
    if (!kPrefix.count(s) || s == sound_state.synth) return;
    sound_state.synth = s;
    prefs::set("music", s);
    if (sound_state.on.music && sound_state.song && sound_state.song_playing) {
        auto song = *sound_state.song;
        StartSong(song.first, song.second);
    }
}

double effect(const std::string& what) {
    if (!sound_state.on.effects || sound_state.files.empty()) return 0;
    auto e = kEffect.find(what);
    if (e == kEffect.end()) return 0;
    auto [group, index] = e->second;
    if (group >= static_cast<int>(sound_state.files.size()) ||
        index >= static_cast<int>(sound_state.files[group].size()))
        return 0;
    const std::string& name = sound_state.files[group][index];
    if (name.empty()) return 0;
    return Enqueue(name, nullptr);
}

bool speak(int group, std::function<void()> after) {
    if (!sound_state.on.speech || sound_state.files.empty()) return false;
    auto [name, subtitle] = w2::cues::clip(sound_state.files, group, Roll);
    if (name.empty() || !sound_state.dev || !SampleData(name)) return false;
    Enqueue(name, std::move(after));
    return true;
}

bool speechOn() {
    return sound_state.on.speech;
}

bool busy() {
    return sound_state.playing.has_value() || !sound_state.queue.empty();
}

// Call every frame: start the next sample when the last has ended, hear
// whether a song ran out, and start a looping recording over.
void update() {
    if (sound_state.playing) {
        bool ended;
        {
            Lock lock;
            ended = sound_state.mix.sample_ended;
        }
        // or, should the device stall, once its length has passed
        if (ended || w2::now() >= sound_state.playing->ends_at) {
            auto done = std::move(sound_state.playing->done);
            sound_state.playing.reset();
            if (done) done();
        }
    }
    if (!sound_state.playing && !sound_state.queue.empty()) {
        Queued q = std::move(sound_state.queue.front());
        sound_state.queue.pop_front();
        q.ends_at = w2::now() + static_cast<double>(q.pcm->size()) / kSampleRate + 0.25;
        {
            Lock lock;
            sound_state.mix.sample = q.pcm;
            sound_state.mix.sample_pos = 0;
            sound_state.mix.sample_ended = false;
        }
        sound_state.playing = std::move(q);
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
