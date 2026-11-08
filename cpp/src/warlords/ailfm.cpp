#include "warlords/ailfm.hpp"

#include <algorithm>
#include <cmath>

namespace w2 {

namespace {

// ADLIB.ADV file offsets (it loads at offset 0 of its segment)
constexpr size_t kAdvFreq = 0x101;       ///< 192 words: 12 half-tones x 16 fine steps of F-number.
constexpr size_t kAdvOctave = 0x281;     ///< 96 bytes: the octave of each note from C-2.
constexpr size_t kAdvHalftone = 0x2e1;   ///< 96 bytes: its half-tone.
constexpr size_t kAdvReset = 0x340;      ///< Register values written to 1..0xF5 at start-up.
constexpr size_t kAdvVelocity = 0x52b;   ///< 16 bytes: velocity / 8 -> sensitivity, 82..127.
constexpr size_t kAdvPrograms = 0x2203;  ///< The program each channel 1..8 starts on.

/// Operator register offsets of each voice's modulator; the carrier is +3.
const int kOpreg[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

/** @brief The unsigned byte at `offset`, or 0 past the end. */
int ByteAt(const std::string& bytes, size_t offset) {
    return offset < bytes.size() ? static_cast<uint8_t>(bytes[offset]) : 0;
}
/** @brief The unsigned little-endian word at `offset`. */
int U16(const std::string& bytes, size_t offset) {
    return ByteAt(bytes, offset) + ByteAt(bytes, offset + 1) * 256;
}
/** @brief The signed little-endian word at `offset`. */
int S16(const std::string& bytes, size_t offset) {
    int value = U16(bytes, offset);
    return value >= 0x8000 ? value - 0x10000 : value;
}
/** @brief A byte read as signed. */
int S8(int byte) {
    return byte >= 0x80 ? byte - 0x100 : byte;
}

/** @brief The driver's product: hi8(level * factor * 2), then one more unless it came to 0. */
int Scale(int level, int factor) {
    int value = (level * factor * 2 / 256) % 256;
    return value == 0 ? 0 : value + 1;
}

/** @brief Division rounding towards minus infinity, as the Lua's // does. */
inline int FloorDiv(int dividend, int divisor) {
    return static_cast<int>(std::floor(static_cast<double>(dividend) / divisor));
}

}  // namespace

AilFm::AilFm(const std::string& adv, const std::string& library) {
    for (int i = 0; i < 192; i++) freq_[i] = S16(adv, kAdvFreq + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 96; i++) {
        octave_[i] = ByteAt(adv, kAdvOctave + i);
        halftone_[i] = ByteAt(adv, kAdvHalftone + i);
    }
    for (int reg = 1; reg <= 0xf5; reg++) reset_[reg] = ByteAt(adv, kAdvReset + reg);
    for (int i = 0; i < 16; i++) velocity_[i] = ByteAt(adv, kAdvVelocity + i);
    for (int channel = 1; channel <= 8; channel++)
        programs_[channel] = ByteAt(adv, kAdvPrograms + channel);
    // The Global Timbre Library: a directory of 6-byte entries { patch, bank,
    // offset:u32 } ended by bank 0xFF, and at each offset a timbre -- a u16
    // length then the bytes.
    for (size_t entry = 0; entry + 6 <= library.size(); entry += 6) {
        int patch = ByteAt(library, entry), bank = ByteAt(library, entry + 1);
        if (bank == 0xff) break;
        size_t offset = static_cast<size_t>(U16(library, entry + 2)) +
                        static_cast<size_t>(U16(library, entry + 4)) * 65536;
        int length = U16(library, offset);
        Timbre timbre;
        for (int i = 2; i < length; i++)
            timbre.push_back(static_cast<uint8_t>(ByteAt(library, offset + i)));
        lib_[bank * 128 + patch] = timbre;
    }
    for (int reg = 1; reg <= 0xf5; reg++) chip_.Write(reg, reset_[reg]);
    // channel state, as the driver's start-up leaves it on channels 1..9
    for (int channel = 1; channel <= 9; channel++) {
        Chan& chan = chans_[channel];
        chan.vol = 127;
        chan.expr = 127;
        if (channel <= 8) Program(channel, programs_[channel]);
    }
    for (int& owner : owner_) owner = -1;
}

void AilFm::Program(int channel, int program) {
    auto found = lib_.find(chans_[channel].bank * 128 + program);
    chans_[channel].timbre = found == lib_.end() ? nullptr : &found->second;
}

void AilFm::Update(Slot& slot) {
    int voice = slot.voice;
    if (voice < 0) return;
    int flags = slot.flags;
    Chan& chan = chans_[slot.ch];
    int mod_reg = kOpreg[voice], car_reg = kOpreg[voice] + 3;
    if (flags >= 0x80) {
        int vib = chan.mod >= 64 ? 0x40 : 0;
        chip_.Write(0x20 + mod_reg,
                    slot.mod_mult + slot.mod_avek + (slot.mod_avek % 0x80 >= 0x40 ? 0 : vib));
        chip_.Write(0x20 + car_reg,
                    slot.car_mult + slot.car_avek + (slot.car_avek % 0x80 >= 0x40 ? 0 : vib));
        flags -= 0x80;
    }
    if (flags >= 0x40) {
        int vol = Scale(Scale(chan.vol, chan.expr), slot.vel);
        int mod_level = slot.mod_level;
        if (slot.scale_mod) mod_level = mod_level * vol / 127;
        int car_level = slot.car_level * vol / 127;
        chip_.Write(0x40 + mod_reg, 63 - mod_level % 64 + slot.mod_ksl);
        chip_.Write(0x40 + car_reg, 63 - car_level % 64 + slot.car_ksl);
        flags -= 0x40;
    }
    if (flags >= 0x20) {
        chip_.Write(0x60 + mod_reg, slot.mod_ad);
        chip_.Write(0x60 + car_reg, slot.car_ad);
        chip_.Write(0x80 + mod_reg, slot.mod_sr);
        chip_.Write(0x80 + car_reg, slot.car_sr);
        flags -= 0x20;
    }
    if (flags >= 0x10) {
        chip_.Write(0xe0 + car_reg, slot.car_ws);
        chip_.Write(0xe0 + mod_reg, slot.mod_ws);
        flags -= 0x10;
    }
    if (flags >= 0x08) {
        chip_.Write(0xc0 + voice, slot.fbc % 16);
        flags -= 0x08;
    }
    if (flags % 2 == 1) {
        if (slot.keyon == 0) {
            chip_.Write(0xb0 + voice,
                        slot.b0 - slot.b0 % 64 + slot.b0 % 32);  // key off, pitch kept
        } else {
            chip_.Write(0xa0 + voice, Frequency(slot, chan) % 256);
            slot.b0 = (fw_ / 256) % 4 + block_ * 4 + slot.keyon;
            chip_.Write(0xb0 + voice, slot.b0);
        }
        flags -= 1;
    }
    slot.flags = flags;
}

// The note less two octaves, folded into the 96 the tables cover, in
// sixteenths of a half-tone.
int AilFm::Frequency(const Slot& slot, const Chan& chan) {
    int note = slot.note + slot.transpose - 24;
    while (note < 0) note += 12;
    while (note > 95) note -= 12;
    int bend = FloorDiv(chan.bend, 32) * 12;  // +-12 half-tones over the range
    int pitch = FloorDiv(note * 256 + bend + 8, 16);
    while (pitch < 0) pitch += 192;
    while (pitch > 0x5ff) pitch -= 192;
    int semi = pitch / 16;
    int fnum = freq_[halftone_[semi] * 16 + pitch % 16];
    int block = octave_[semi] - 1;
    // the upper half-tones are stored an octave down, flagged negative
    if (fnum < 0) block++;
    if (block < 0) {
        block++;
        fnum = FloorDiv(fnum, 2);
    }
    if (fnum < 0) fnum += 0x10000;
    fw_ = fnum;
    block_ = block;
    return fnum;
}

// A free voice goes round-robin from the last one handed out; with none
// free, the slots fight over them.
void AilFm::Allocate(Slot& slot) {
    int voice = next_voice_;
    for (int i = 0; i < 9; i++) {
        voice = (voice + 1) % 9;
        next_voice_ = voice;
        if (owner_[voice] < 0) {
            slot.voice = voice;
            owner_[voice] = slot.ch;
            chans_[slot.ch].voices++;
            slot.flags = 0xf9;
            Update(slot);
            return;
        }
    }
    Steal();
}

// A note's claim to a voice is 0x7FFF less the voices its channel already
// holds (a channel locked by controller 112 claims 0xFFFF). While the best
// claim of a note without a voice is at least the weakest claim of one
// with a voice, the weaker note is cut off and its voice handed over.
void AilFm::Steal() {
    int prio[16] = {};
    int active_count = 0;
    for (int i = 0; i < 16; i++) {
        Slot& slot = slots_[i];
        if (slot.active) {
            active_count++;
            Chan& chan = chans_[slot.ch];
            int priority = (chan.lock >= 64 ? 0xffff : 0x7fff) - chan.voices;
            prio[i] = priority < 0 ? 0 : priority;
        }
    }
    while (active_count > 0) {
        int best = -1, best_priority = 0, worst = -1, worst_priority = 0xffff;
        for (int i = 0; i < 16; i++) {
            Slot& slot = slots_[i];
            if (!slot.active) continue;
            int priority = prio[i];
            if (slot.voice < 0) {
                if (priority >= best_priority) {
                    best = i;
                    best_priority = priority;
                }
            } else if (priority <= worst_priority) {
                worst = i;
                worst_priority = priority;
            }
        }
        if (best_priority < worst_priority || best_priority == 0 || best < 0 || worst < 0) return;
        Slot& loser = slots_[worst];
        Slot& winner = slots_[best];
        int voice = loser.voice;
        Release(loser);
        loser.active = false;
        winner.voice = voice;
        owner_[voice] = winner.ch;
        chans_[winner.ch].voices++;
        winner.flags = 0xf9;
        Update(winner);
        active_count--;
    }
}

void AilFm::Release(Slot& slot) {
    if (slot.voice < 0) return;
    slot.keyon = 0;
    if (slot.flags % 2 == 0) slot.flags++;
    Update(slot);
    chans_[slot.ch].voices--;
    owner_[slot.voice] = -1;
    slot.voice = -1;
}

void AilFm::NoteOn(int channel, int key, int vel) {
    if (vel == 0) return NoteOff(channel, key);
    const Timbre* found;
    if (channel == 9) {
        auto cached = percussion_.find(key);
        if (cached == percussion_.end()) {
            auto entry = lib_.find(127 * 128 + key);
            found = entry == lib_.end() ? nullptr : &entry->second;
            percussion_[key] = found;
        } else {
            found = cached->second;
        }
    } else {
        found = chans_[channel].timbre;
    }
    if (!found || found->size() != 12) return;
    Slot* free_slot = nullptr;
    for (Slot& candidate : slots_)
        if (!candidate.active) {
            free_slot = &candidate;
            break;
        }
    if (!free_slot) return;
    Slot& slot = *free_slot;
    const Timbre& timbre = *found;
    slot.ch = channel;
    slot.key = key;
    // a drum plays at the pitch its timbre's transpose byte names
    if (channel == 9) {
        slot.note = timbre[0];
        slot.transpose = 0;
    } else {
        slot.note = key;
        slot.transpose = S8(timbre[0]);
    }
    slot.vel = velocity_[vel / 8];
    slot.active = true;
    slot.sustained = false;
    slot.voice = -1;
    // Copy a 14-byte timbre into the slot. Bytes, after the length:
    // transpose, then the modulator's 20/40/60/80/E0 registers, C0, and the
    // carrier's 20/40/60/80/E0.
    slot.keyon = 0x20;
    slot.fbc = timbre[6];
    slot.conn = timbre[6] % 2;
    slot.mod_ksl = timbre[2] - timbre[2] % 64;
    slot.mod_level = 63 - timbre[2] % 64;
    slot.car_ksl = timbre[8] - timbre[8] % 64;
    slot.car_level = 63 - timbre[8] % 64;
    slot.mod_avek = timbre[1] - timbre[1] % 16;
    slot.mod_mult = timbre[1] % 16;
    slot.car_avek = timbre[7] - timbre[7] % 16;
    slot.car_mult = timbre[7] % 16;
    slot.mod_ad = timbre[3];
    slot.mod_sr = timbre[4];
    slot.mod_ws = timbre[5];
    slot.car_ad = timbre[9];
    slot.car_sr = timbre[10];
    slot.car_ws = timbre[11];
    // which operators follow the volume: the carrier always, the modulator
    // only when it is heard directly (additive)
    slot.scale_mod = slot.conn == 1;
    slot.flags = 0xf9;
    Allocate(slot);
}

void AilFm::NoteOff(int channel, int key) {
    Chan& chan = chans_[channel];
    for (Slot& slot : slots_) {
        if (slot.active && slot.key == key && slot.ch == channel) {
            if (chan.sustain >= 64) {
                slot.sustained = true;
            } else {
                Release(slot);
                slot.active = false;
            }
        }
    }
}

void AilFm::Controller(int channel, int number, int value) {
    Chan& chan = chans_[channel];
    int flag_bit = 0;
    int* field = nullptr;
    if (number == 114)
        chan.bank = value;
    else if (number == 112)
        chan.lock = value;
    else if (number == 64) {
        chan.sustain = value;
        if (value < 64) {
            for (Slot& slot : slots_)
                if (slot.active && slot.ch == channel && slot.sustained) NoteOff(channel, slot.key);
        }
    } else if (number == 1) {
        field = &chan.mod;
        flag_bit = 0x80;
    } else if (number == 7) {
        field = &chan.vol;
        flag_bit = 0x40;
    } else if (number == 11) {
        field = &chan.expr;
        flag_bit = 0x40;
    } else if (number == 10) {
        field = &chan.pan;
        flag_bit = 0x40;
    }
    if (field) {
        *field = value;
        for (Slot& slot : slots_) {
            if (slot.active && slot.ch == channel) {
                if ((slot.flags / flag_bit) % 2 == 0) slot.flags += flag_bit;
                Update(slot);
            }
        }
    }
}

// Notes only sound on channels 2-10 (1-9 here).
void AilFm::Message(int status, int data1, int data2) {
    int kind = status & 0xf0, channel = status & 15;
    if (kind == 0x90) {
        if (channel >= 1 && channel <= 9) NoteOn(channel, data1, data2);
    } else if (kind == 0x80) {
        NoteOff(channel, data1);
    } else if (kind == 0xb0) {
        Controller(channel, data1, data2);
    } else if (kind == 0xc0) {
        Program(channel, data1);
    } else if (kind == 0xe0) {
        chans_[channel].bend = data2 * 128 + data1 - 0x2000;
        for (Slot& slot : slots_) {
            if (slot.active && slot.ch == channel) {
                if (slot.flags % 2 == 0) slot.flags++;
                Update(slot);
            }
        }
    }
}

void AilFm::Play(const xmi::Sequence* seq, bool loop) {
    Stop();
    seq_ = seq;
    pos_ = 0;
    tick_ = 0;
    loop_ = loop;
    playing_ = seq != nullptr;
}

void AilFm::Stop() {
    for (Slot& slot : slots_) {
        if (slot.active) {
            Release(slot);
            slot.active = false;
        }
    }
    playing_ = false;
}

void AilFm::Interval() {
    const xmi::Sequence* seq = seq_;
    if (!playing_ || !seq) return;
    int tick = tick_;
    while (pos_ < seq->events.size() && seq->events[pos_].t <= tick) {
        const auto& event = seq->events[pos_];
        Message(event.st, event.a, event.b);
        pos_++;
    }
    tick_ = tick + 1;
    if (pos_ >= seq->events.size() && tick_ > seq->length) {
        if (loop_)
            Play(seq, true);
        else
            Stop();
    }
}

bool AilFm::Idle() const {
    return !playing_ && chip_.Silent();
}

void AilFm::Render(int16_t* out, int n) {
    const double samples_per_tick = Opl::kRate / xmi::kTickRate;
    int done = 0;
    while (done < n) {
        if (until_tick_ <= 0) {
            Interval();
            until_tick_ += samples_per_tick;
        }
        int chunk = std::min(n - done, static_cast<int>(std::ceil(until_tick_)));
        chip_.Generate(out + done, chunk);
        done += chunk;
        until_tick_ -= chunk;
    }
}

}  // namespace w2
