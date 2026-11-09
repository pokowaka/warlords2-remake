#include "platform/sound.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <SDL.h>

#include "util/util.hpp"
#include "warlords/ailfm.hpp"
#include "warlords/cues.hpp"
#include "warlords/types.hpp"
#include "warlords/xmi.hpp"

#ifdef W2_VORBIS
#include <vorbis/vorbisfile.h>
#endif

namespace sound {

namespace {

constexpr int kSampleRate = 11000;
constexpr float kFmGain = 2.0F / 32768.0F;

/** @brief Returns the single-letter asset filename prefix (`'S'`, `'M'`, or `'R'`) for `synth`. */
char SynthPrefix(SynthKind synth) {
    switch (synth) {
    case SynthKind::kFm:
        return 'S';
    case SynthKind::kMt32:
        return 'M';
    case SynthKind::kSc55:
        return 'R';
    }
    return 'S';
}

/** @brief Resolves a logical effect token to its `(group, index)` entry in `FILE.DAT`. */
std::optional<std::pair<int, int>> FindEffectEntry(std::string_view name) {
    using Entry = std::pair<std::string_view, std::pair<int, int>>;
    constexpr std::array<Entry, 9> kEffects = {{
        {"army", {64, 0}},
        {"army2", {64, 5}},
        {"ding", {64, 1}},
        {"chord", {64, 2}},
        {"dramatic", {37, 0}},
        {"orch", {37, 1}},
        {"war", {38, 0}},
        {"splash", {39, 0}},
        {"turn", {63, 0}},
    }};
    for (const auto& [key, loc] : kEffects) {
        if (key == name) return loc;
    }
    return std::nullopt;
}

/** @brief Scoped RAII lock over an open `SDL_AudioDeviceID`. */
class ScopedDeviceLock {
  public:
    explicit ScopedDeviceLock(uint32_t device_id) : device_id_(device_id) {
        if (device_id_ != 0) SDL_LockAudioDevice(device_id_);
    }
    ~ScopedDeviceLock() {
        if (device_id_ != 0) SDL_UnlockAudioDevice(device_id_);
    }
    ScopedDeviceLock(const ScopedDeviceLock&) = delete;
    ScopedDeviceLock& operator=(const ScopedDeviceLock&) = delete;

  private:
    uint32_t device_id_ = 0;
};

}  // namespace

struct AudioEngine::VorbisStream {
#ifdef W2_VORBIS
    OggVorbis_File vorbis{};
    bool open = false, eof = false;
    int channels = 1;
    double step = 1.0, pos = 0.0, played = 0.0;
    std::vector<float> buf;
    size_t frames = 0;

    ~VorbisStream() {
        if (open) ov_clear(&vorbis);
    }

    static std::unique_ptr<VorbisStream> Open(const std::filesystem::path& path, int mixer_rate) {
        auto stream = std::make_unique<VorbisStream>();
        std::string resolved = w2::ResolvePath(path).string();
        if (resolved.empty() || ov_fopen(resolved.c_str(), &stream->vorbis) != 0) return nullptr;
        stream->open = true;
        vorbis_info* info = ov_info(&stream->vorbis, -1);
        stream->channels = info->channels;
        stream->step = static_cast<double>(info->rate) / mixer_rate;
        return stream;
    }

    void Fill(size_t need) {
        size_t consumed = static_cast<size_t>(pos);
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + std::min(buf.size(), consumed * 2UZ));
            frames -= std::min(frames, consumed);
            pos -= static_cast<double>(consumed);
        }
        while (!eof && frames < need + 2UZ) {
            float** pcm = nullptr;
            int section = 0;
            long count = ov_read_float(&vorbis, &pcm, 4096, &section);
            if (count <= 0) {
                eof = true;
                break;
            }
            for (long i = 0; i < count; i++) {
                buf.push_back(pcm[0][i]);
                buf.push_back(channels > 1 ? pcm[1][i] : pcm[0][i]);
            }
            frames += static_cast<size_t>(count);
        }
    }

    bool Done() const { return eof && pos + 1.0 >= static_cast<double>(frames); }

    void MixInto(float* out, int out_frames, int rate) {
        if (Done()) return;
        Fill(static_cast<size_t>(std::ceil(out_frames * step)) + 2UZ);
        for (int i = 0; i < out_frames; i++) {
            size_t idx = static_cast<size_t>(pos);
            if (idx + 1UZ >= frames) {
                if (eof) break;
                Fill(static_cast<size_t>(std::ceil((out_frames - i) * step)) + 2UZ);
                idx = static_cast<size_t>(pos);
                if (idx + 1UZ >= frames) break;
            }
            double frac = pos - static_cast<double>(idx);
            out[static_cast<std::ptrdiff_t>(i) * 2] +=
                    static_cast<float>(buf[idx * 2] * (1.0 - frac) + buf[idx * 2 + 2] * frac);
            out[i * 2 + 1] +=
                    static_cast<float>(buf[idx * 2 + 1] * (1.0 - frac) + buf[idx * 2 + 3] * frac);
            pos += step;
        }
        played += static_cast<double>(out_frames) / rate;
    }
#endif
};

struct AudioEngine::MixerState {
    using StreamPtr = std::unique_ptr<VorbisStream>;
    using TailList = std::vector<StreamPtr>;
    using SharedSequence = std::shared_ptr<w2::xmi::Sequence>;

    int rate = 44100;
    SharedPcm sample;
    double sample_pos = 0.0, fm_pos = 0.0;
    bool sample_ended = true, fm_was_playing = false, fm_ended = false;
    std::unique_ptr<w2::AilFm> fm_synth;
    SharedSequence fm_seq;
    std::vector<int16_t> fm_buf;
    StreamPtr rec;
    TailList tails;

    void MixSample(float* out, int frames) {
        if (!sample || sample_ended) return;
        const PcmBuffer& pcm = *sample;
        double step = static_cast<double>(kSampleRate) / rate;
        for (int i = 0; i < frames; i++) {
            size_t idx = static_cast<size_t>(sample_pos);
            if (idx >= pcm.size()) {
                sample_ended = true;
                return;
            }
            double frac = sample_pos - static_cast<double>(idx);
            float next_val = idx + 1UZ < pcm.size() ? pcm[idx + 1UZ] : 0.0F;
            float val = static_cast<float>(pcm[idx] * (1.0 - frac) + next_val * frac);
            out[static_cast<std::ptrdiff_t>(i) * 2] += val;
            out[i * 2 + 1] += val;
            sample_pos += step;
        }
    }

    void MixFm(float* out, int frames) {
        if (!fm_synth || fm_synth->Idle()) {
            if (fm_was_playing) fm_ended = true;
            fm_was_playing = false;
            return;
        }
        double step = w2::Opl::kRate / rate;
        size_t need = static_cast<size_t>(std::ceil(fm_pos + frames * step)) + 2UZ;
        size_t keep = static_cast<size_t>(fm_pos);
        if (keep > 0) {
            fm_buf.erase(fm_buf.begin(), fm_buf.begin() + std::min(keep, fm_buf.size()));
            fm_pos -= static_cast<double>(keep);
            need -= keep;
        }
        if (fm_buf.size() < need) {
            size_t from = fm_buf.size();
            fm_buf.resize(need);
            fm_synth->Render(fm_buf.data() + from, static_cast<int>(need - from));
        }
        for (int i = 0; i < frames; i++) {
            size_t idx = static_cast<size_t>(fm_pos);
            double frac = fm_pos - static_cast<double>(idx);
            float val = static_cast<float>((fm_buf[idx] * (1.0 - frac) + fm_buf[idx + 1UZ] * frac) *
                                           kFmGain);
            val = std::clamp(val, -1.0F, 1.0F);
            out[static_cast<std::ptrdiff_t>(i) * 2] += val;
            out[i * 2 + 1] += val;
            fm_pos += step;
        }
        bool playing_now = fm_synth->Playing();
        if (fm_was_playing && !playing_now) fm_ended = true;
        fm_was_playing = playing_now;
    }
};

std::string_view SynthName(SynthKind synth) {
    switch (synth) {
    case SynthKind::kFm:
        return "fm";
    case SynthKind::kMt32:
        return "mt32";
    case SynthKind::kSc55:
        return "sc55";
    }
    return "fm";
}

OptionalSynthKind ParseSynthKind(std::string_view name) {
    if (name == "fm") return SynthKind::kFm;
    if (name == "mt32") return SynthKind::kMt32;
    if (name == "sc55") return SynthKind::kSc55;
    return std::nullopt;
}

AudioEngine::AudioEngine() : mixer_(std::make_unique<MixerState>()) {}
AudioEngine::~AudioEngine() {
    Shutdown();
}

void AudioEngine::AudioCallback(void* userdata, uint8_t* stream, int byte_len) {
    auto* self = static_cast<AudioEngine*>(userdata);
    self->MixOutput(reinterpret_cast<float*>(stream),
                    byte_len / static_cast<int>(sizeof(float) * 2));
}

void AudioEngine::MixOutput(float* out, int frames) {
    std::fill(out, out + static_cast<std::ptrdiff_t>(frames) * 2, 0.0F);
    mixer_->MixFm(out, frames);
#ifdef W2_VORBIS
    if (mixer_->rec) mixer_->rec->MixInto(out, frames, mixer_->rate);
    for (auto& tail : mixer_->tails) tail->MixInto(out, frames, mixer_->rate);
#endif
    mixer_->MixSample(out, frames);
    for (int i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0F, 1.0F);
}

int AudioEngine::RollDie(int sides) {
    return static_cast<int>(rng_() % static_cast<unsigned>(std::max(1, sides))) + 1;
}

std::filesystem::path AudioEngine::DataPath(std::string_view relative_name) const {
    std::string rel(relative_name);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return data_dir_ / rel;
}

std::filesystem::path AudioEngine::RecordingPath(SynthKind synth, std::string_view xmi_name) const {
    std::string base = w2::Upper(xmi_name);
    if (base.ends_with(".XMI")) base = base.substr(0, base.size() - 4);
    return rec_dir_ / (std::string(1, SynthPrefix(synth)) + base + ".ogg");
}

AudioEngine::SharedPcm AudioEngine::LoadSample(std::string_view relative_name) {
    std::string key(relative_name);
    if (auto found = samples_.find(key); found != samples_.end()) return found->second;
    SharedPcm pcm;
    if (auto bytes = w2::ReadBytes(DataPath(key)); bytes.ok() && !bytes->empty()) {
        pcm = std::make_shared<PcmBuffer>(bytes->size());
        for (size_t i = 0; i < bytes->size(); i++) {
            (*pcm)[i] = static_cast<float>(static_cast<int>((*bytes)[i]) - 128) / 128.0F;
        }
    }
    samples_.emplace(std::move(key), pcm);
    return pcm;
}

void AudioEngine::StepRecording() {
#ifdef W2_VORBIS
    ScopedDeviceLock dev_lock(device_id_);
    MixerState::StreamPtr& rec = mixer_->rec;
    if (rec && (rec->played >= rec_loop_at_ || rec->Done())) {
        mixer_->tails.push_back(std::move(rec));
        if (rec_loop_ && song_.has_value()) {
            rec = VorbisStream::Open(RecordingPath(synth_, song_->first), mixer_->rate);
        } else {
            song_playing_ = false;
        }
    }
    auto& tails = mixer_->tails;
    tails.erase(std::remove_if(tails.begin(), tails.end(),
                               [](const auto& tail) { return tail->Done(); }),
                tails.end());
    if (tails.size() > 4UZ) tails.erase(tails.begin(), tails.end() - 4);
#endif
}

void AudioEngine::StopStreams() {
    MixerState::StreamPtr old_rec;
    MixerState::TailList old_tails;
    ScopedDeviceLock dev_lock(device_id_);
    if (mixer_->fm_synth) mixer_->fm_synth->Stop();
    mixer_->fm_was_playing = mixer_->fm_ended = false;
    old_rec = std::move(mixer_->rec);
    old_tails = std::move(mixer_->tails);
    mixer_->tails.clear();
}

bool AudioEngine::StartSong(const std::string& name, bool loop) {
    if (device_id_ == 0) return false;
#ifdef W2_VORBIS
    if (synth_ != SynthKind::kFm) {
        auto bytes = w2::ReadBytes(data_dir_ / "SOUND" /
                                   (std::string(1, SynthPrefix(synth_)) + w2::Upper(name)));
        auto seq = bytes.ok() ? w2::xmi::Parse(*bytes) : std::nullopt;
        auto stream = seq ? VorbisStream::Open(RecordingPath(synth_, name), mixer_->rate) : nullptr;
        if (stream) {
            StopStreams();
            ScopedDeviceLock dev_lock(device_id_);
            mixer_->rec = std::move(stream);
            rec_loop_at_ = (seq->length + 1.0) / w2::xmi::kTickRate;
            rec_loop_ = loop;
            song_ = std::make_pair(name, loop);
            song_playing_ = true;
            return true;
        }
    }
#endif
    if (adv_.empty() || ad_bank_.empty()) return false;
    auto bytes = w2::ReadBytes(data_dir_ / "SOUND" / ("S" + w2::Upper(name)));
    auto seq = bytes.ok() ? w2::xmi::Parse(*bytes) : std::nullopt;
    if (!seq) return false;
    StopStreams();
    auto shared_seq = std::make_shared<w2::xmi::Sequence>(std::move(*seq));
    MixerState::SharedSequence prev_seq;
    {
        ScopedDeviceLock dev_lock(device_id_);
        if (!mixer_->fm_synth) mixer_->fm_synth = std::make_unique<w2::AilFm>(adv_, ad_bank_);
        prev_seq = mixer_->fm_seq;
        mixer_->fm_seq = shared_seq;
        mixer_->fm_synth->Play(shared_seq.get(), loop);
        mixer_->fm_was_playing = true;
        mixer_->fm_ended = false;
    }
    song_ = std::make_pair(name, loop);
    song_playing_ = true;
    return true;
}

double AudioEngine::EnqueueSample(std::string_view relative_name, CompletionCallback done) {
    SharedPcm pcm = device_id_ != 0 ? LoadSample(relative_name) : nullptr;
    if (!pcm) {
        if (done) done();
        return 0.0;
    }
    queue_.push_back(QueuedSample{pcm, std::move(done), 0.0});
    Update();
    return static_cast<double>(pcm->size()) / kSampleRate;
}

void AudioEngine::Init(const std::filesystem::path& data_dir, const w2::uidata::Strings& files,
                       OptionalSynthKind preferred_synth) {
    Shutdown();
    data_dir_ = data_dir;
    files_ = files;
    rec_dir_ = w2::FileExists("pre-rendered-sound") ? std::filesystem::path("pre-rendered-sound")
                                                    : data_dir / "../pre-rendered-sound";
    std::string opts = w2::ReadFile(data_dir / "DATA/OPTIONS.SND").value_or("111");
    while (opts.size() < 3UZ) opts += '1';
    options_ = Options{opts[0] == '1', opts[1] == '1', opts[2] != '0'};

    SynthKind desired = preferred_synth.value_or(SynthKind::kMt32);
    synth_ = SynthAvailable(desired) ? desired : SynthKind::kFm;
    adv_ = w2::ReadBytes(data_dir / "ADLIB.ADV").value_or(ByteData{});
    ad_bank_ = w2::ReadBytes(data_dir / "MIDPAK.AD").value_or(ByteData{});

    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &AudioEngine::AudioCallback;
    want.userdata = this;
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_id_ == 0) return;
    mixer_->rate = have.freq;
    SDL_PauseAudioDevice(device_id_, 0);
}

void AudioEngine::Shutdown() {
    if (device_id_ != 0) SDL_CloseAudioDevice(device_id_);
    device_id_ = 0;
    mixer_ = std::make_unique<MixerState>();
    samples_.clear();
    queue_.clear();
    playing_.reset();
    cue_ = w2::kNone;
    song_.reset();
    song_playing_ = false;
}

void AudioEngine::SaveOptions() const {
    if (data_dir_.empty()) return;
    std::string content = {options_.music ? '1' : '0', options_.effects ? '1' : '0',
                           options_.speech ? '1' : '0'};
    w2::WriteFile(data_dir_ / "DATA/OPTIONS.SND", content).IgnoreError();
}

void AudioEngine::Toggle(AudioChannel channel) {
    switch (channel) {
    case AudioChannel::kMusic:
        options_.music = !options_.music;
        if (options_.music) {
            cue_ = w2::kNone;
            Music(w2::cues::kPlay);
        } else {
            StopMusic();
        }
        break;
    case AudioChannel::kEffects:
        options_.effects = !options_.effects;
        break;
    case AudioChannel::kSpeech:
        options_.speech = !options_.speech;
        break;
    }
    SaveOptions();
}

void AudioEngine::Music(int cue, bool has_active_human) {
    if (!options_.music || (cue == cue_ && song_playing_)) return;
    w2::Side synthetic_human{};
    synthetic_human.SetInUse(true);
    synthetic_human.SetAlive(true);
    SidePtrList cue_sides;
    if (has_active_human) cue_sides.push_back(&synthetic_human);
    auto [name, loop] =
            w2::cues::Song(files_, cue, cue_sides, [this](int n) { return RollDie(n); });
    if (!name.empty() && StartSong(name, loop)) cue_ = cue;
}

void AudioEngine::StopMusic() {
    cue_ = w2::kNone;
    song_.reset();
    song_playing_ = false;
    StopStreams();
}

bool AudioEngine::SynthAvailable(SynthKind synth) const {
    if (synth == SynthKind::kFm) return true;
#ifdef W2_VORBIS
    return w2::FileExists(RecordingPath(synth, "STARTUP.XMI"));
#else
    return false;
#endif
}

void AudioEngine::SetSynth(SynthKind synth) {
    if (synth == synth_) return;
    synth_ = synth;
    if (options_.music && song_.has_value() && song_playing_) {
        SongSpec current_song = *song_;
        StartSong(current_song.first, current_song.second);
    }
}

double AudioEngine::Effect(std::string_view name) {
    if (!options_.effects || files_.empty()) return 0.0;
    auto entry = FindEffectEntry(name);
    if (!entry.has_value()) return 0.0;
    auto [group, index] = *entry;
    if (group < 0 || group >= static_cast<int>(files_.size()) || index < 0 ||
        index >= static_cast<int>(files_[group].size())) {
        return 0.0;
    }
    const std::string& file_name = files_[group][index];
    return file_name.empty() ? 0.0 : EnqueueSample(file_name, nullptr);
}

bool AudioEngine::Speak(int group, CompletionCallback after) {
    if (!options_.speech || files_.empty()) return false;
    auto [sample_name, subtitle] =
            w2::cues::Clip(files_, group, [this](int n) { return RollDie(n); });
    (void)subtitle;
    if (sample_name.empty() || device_id_ == 0 || !LoadSample(sample_name)) return false;
    EnqueueSample(sample_name, std::move(after));
    return true;
}

void AudioEngine::Update() {
    if (playing_.has_value()) {
        bool ended = [this]() {
            ScopedDeviceLock dev_lock(device_id_);
            return mixer_->sample_ended;
        }();
        if (ended || w2::Now() >= playing_->ends_at) {
            CompletionCallback done = std::move(playing_->done);
            playing_.reset();
            if (done) done();
        }
    }
    if (!playing_.has_value() && !queue_.empty()) {
        QueuedSample next_clip = std::move(queue_.front());
        queue_.pop_front();
        next_clip.ends_at =
                w2::Now() + static_cast<double>(next_clip.pcm->size()) / kSampleRate + 0.25;
        {
            ScopedDeviceLock dev_lock(device_id_);
            mixer_->sample = next_clip.pcm;
            mixer_->sample_pos = 0.0;
            mixer_->sample_ended = false;
        }
        playing_ = std::move(next_clip);
    }
    {
        ScopedDeviceLock dev_lock(device_id_);
        if (mixer_->fm_ended) {
            mixer_->fm_ended = false;
            if (song_.has_value() && !song_->second) song_playing_ = false;
        }
    }
    StepRecording();
}

void AudioEngine::Music(int cue, SidePtrSpan sides) {
    bool has_human = std::any_of(sides.begin(), sides.end(), [](const w2::Side* side) {
        return side != nullptr && side->IsAlive() && !side->IsComputerControlled();
    });
    Music(cue, has_human);
}

}  // namespace sound
