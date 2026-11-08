#include "warlords/ailfm.hpp"

#include <algorithm>
#include <cmath>

namespace w2 {

namespace {

// ADLIB.ADV file offsets (it loads at offset 0 of its segment)
constexpr size_t kAdvFreq = 0x101;       // 192 words: 12 half-tones x 16 fine steps of F-number
constexpr size_t kAdvOctave = 0x281;     // 96 bytes: the octave of each note from C-2
constexpr size_t kAdvHalftone = 0x2e1;   // 96 bytes: its half-tone
constexpr size_t kAdvReset = 0x340;      // register values written to 1..0xF5 at start-up
constexpr size_t kAdvVelocity = 0x52b;   // 16 bytes: velocity / 8 -> sensitivity, 82..127
constexpr size_t kAdvPrograms = 0x2203;  // the program each channel 1..8 starts on

// operator register offsets of voice v's modulator; the carrier is +3
const int kOpreg[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

int ByteAt(const std::string& s, size_t i) {
    return i < s.size() ? static_cast<uint8_t>(s[i]) : 0;
}
int U16(const std::string& s, size_t i) {
    return ByteAt(s, i) + ByteAt(s, i + 1) * 256;
}
int S16(const std::string& s, size_t i) {
    int v = U16(s, i);
    return v >= 0x8000 ? v - 0x10000 : v;
}
int S8(int v) {
    return v >= 0x80 ? v - 0x100 : v;
}

// hi8(a * b * 2), then one more unless it came to 0: the driver's product
int Scale(int a, int b) {
    int v = (a * b * 2 / 256) % 256;
    return v == 0 ? 0 : v + 1;
}

inline int FloorDiv(int a, int b) {
    return static_cast<int>(std::floor(static_cast<double>(a) / b));
}

}  // namespace

AilFm::AilFm(const std::string& adv, const std::string& ad) {
    for (int i = 0; i < 192; i++) freq_[i] = S16(adv, kAdvFreq + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 96; i++) {
        octave_[i] = ByteAt(adv, kAdvOctave + i);
        halftone_[i] = ByteAt(adv, kAdvHalftone + i);
    }
    for (int r = 1; r <= 0xf5; r++) reset_[r] = ByteAt(adv, kAdvReset + r);
    for (int i = 0; i < 16; i++) velocity_[i] = ByteAt(adv, kAdvVelocity + i);
    for (int ch = 1; ch <= 8; ch++) programs_[ch] = ByteAt(adv, kAdvPrograms + ch);
    // The Global Timbre Library: a directory of 6-byte entries { patch, bank,
    // offset:u32 } ended by bank 0xFF, and at each offset a timbre -- a u16
    // length then the bytes.
    for (size_t p = 0; p + 6 <= ad.size(); p += 6) {
        int patch = ByteAt(ad, p), bank = ByteAt(ad, p + 1);
        if (bank == 0xff) break;
        size_t off =
                static_cast<size_t>(U16(ad, p + 2)) + static_cast<size_t>(U16(ad, p + 4)) * 65536;
        int len = U16(ad, off);
        Timbre t;
        for (int i = 2; i < len; i++) t.push_back(static_cast<uint8_t>(ByteAt(ad, off + i)));
        lib_[bank * 128 + patch] = t;
    }
    for (int r = 1; r <= 0xf5; r++) chip_.Write(r, reset_[r]);
    // channel state, as the driver's start-up leaves it on channels 1..9
    for (int ch = 1; ch <= 9; ch++) {
        Chan& c = chans_[ch];
        c.vol = 127;
        c.expr = 127;
        if (ch <= 8) Program(ch, programs_[ch]);
    }
    for (int& o : owner_) o = -1;
}

void AilFm::Program(int ch, int p) {
    auto it = lib_.find(chans_[ch].bank * 128 + p);
    chans_[ch].timbre = it == lib_.end() ? nullptr : &it->second;
}

// Write whatever the slot's flags say has changed (1807).
void AilFm::Update(Slot& s) {
    int v = s.voice;
    if (v < 0) return;
    int f = s.flags;
    Chan& c = chans_[s.ch];
    int m = kOpreg[v], k = kOpreg[v] + 3;
    if (f >= 0x80) {
        int vib = c.mod >= 64 ? 0x40 : 0;
        chip_.Write(0x20 + m, s.mod_mult + s.mod_avek + (s.mod_avek % 0x80 >= 0x40 ? 0 : vib));
        chip_.Write(0x20 + k, s.car_mult + s.car_avek + (s.car_avek % 0x80 >= 0x40 ? 0 : vib));
        f -= 0x80;
    }
    if (f >= 0x40) {
        int vol = Scale(Scale(c.vol, c.expr), s.vel);
        int ml = s.mod_level;
        if (s.scale_mod) ml = ml * vol / 127;
        int cl = s.car_level * vol / 127;
        chip_.Write(0x40 + m, 63 - ml % 64 + s.mod_ksl);
        chip_.Write(0x40 + k, 63 - cl % 64 + s.car_ksl);
        f -= 0x40;
    }
    if (f >= 0x20) {
        chip_.Write(0x60 + m, s.mod_ad);
        chip_.Write(0x60 + k, s.car_ad);
        chip_.Write(0x80 + m, s.mod_sr);
        chip_.Write(0x80 + k, s.car_sr);
        f -= 0x20;
    }
    if (f >= 0x10) {
        chip_.Write(0xe0 + k, s.car_ws);
        chip_.Write(0xe0 + m, s.mod_ws);
        f -= 0x10;
    }
    if (f >= 0x08) {
        chip_.Write(0xc0 + v, s.fbc % 16);
        f -= 0x08;
    }
    if (f % 2 == 1) {
        if (s.keyon == 0) {
            chip_.Write(0xb0 + v, s.b0 - s.b0 % 64 + s.b0 % 32);  // key off, pitch kept
        } else {
            chip_.Write(0xa0 + v, Frequency(s, c) % 256);
            s.b0 = (fw_ / 256) % 4 + block_ * 4 + s.keyon;
            chip_.Write(0xb0 + v, s.b0);
        }
        f -= 1;
    }
    s.flags = f;
}

// F-number and block for a slot's note (1b61): the note less two octaves,
// folded into the 96 the tables cover, in sixteenths of a half-tone.
int AilFm::Frequency(const Slot& s, const Chan& c) {
    int n = s.note + s.transpose - 24;
    while (n < 0) n += 12;
    while (n > 95) n -= 12;
    int bend = FloorDiv(c.bend, 32) * 12;  // +-12 half-tones over the range
    int x = FloorDiv(n * 256 + bend + 8, 16);
    while (x < 0) x += 192;
    while (x > 0x5ff) x -= 192;
    int semi = x / 16;
    int fw = freq_[halftone_[semi] * 16 + x % 16];
    int block = octave_[semi] - 1;
    // the upper half-tones are stored an octave down, flagged negative
    if (fw < 0) block++;
    if (block < 0) {
        block++;
        fw = FloorDiv(fw, 2);
    }
    if (fw < 0) fw += 0x10000;
    fw_ = fw;
    block_ = block;
    return fw;
}

// Give a slot a free voice, round-robin from the last one (1740); with none
// free, let the slots fight over them (1c69).
void AilFm::Allocate(Slot& s) {
    int v = next_voice_;
    for (int i = 0; i < 9; i++) {
        v = (v + 1) % 9;
        next_voice_ = v;
        if (owner_[v] < 0) {
            s.voice = v;
            owner_[v] = s.ch;
            chans_[s.ch].voices++;
            s.flags = 0xf9;
            Update(s);
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
    int n = 0;
    for (int i = 0; i < 16; i++) {
        Slot& s = slots_[i];
        if (s.active) {
            n++;
            Chan& c = chans_[s.ch];
            int p = (c.lock >= 64 ? 0xffff : 0x7fff) - c.voices;
            prio[i] = p < 0 ? 0 : p;
        }
    }
    while (n > 0) {
        int best = -1, best_p = 0, worst = -1, worst_p = 0xffff;
        for (int i = 0; i < 16; i++) {
            Slot& s = slots_[i];
            if (!s.active) continue;
            int p = prio[i];
            if (s.voice < 0) {
                if (p >= best_p) {
                    best = i;
                    best_p = p;
                }
            } else if (p <= worst_p) {
                worst = i;
                worst_p = p;
            }
        }
        if (best_p < worst_p || best_p == 0 || best < 0 || worst < 0) return;
        Slot& w = slots_[worst];
        Slot& b = slots_[best];
        int v = w.voice;
        Release(w);
        w.active = false;
        b.voice = v;
        owner_[v] = b.ch;
        chans_[b.ch].voices++;
        b.flags = 0xf9;
        Update(b);
        n--;
    }
}

// Key off and give the voice back (179c).
void AilFm::Release(Slot& s) {
    if (s.voice < 0) return;
    s.keyon = 0;
    if (s.flags % 2 == 0) s.flags++;
    Update(s);
    chans_[s.ch].voices--;
    owner_[s.voice] = -1;
    s.voice = -1;
}

void AilFm::NoteOn(int ch, int key, int vel) {
    if (vel == 0) return NoteOff(ch, key);
    const Timbre* t;
    if (ch == 9) {
        auto it = percussion_.find(key);
        if (it == percussion_.end()) {
            auto l = lib_.find(127 * 128 + key);
            t = l == lib_.end() ? nullptr : &l->second;
            percussion_[key] = t;
        } else {
            t = it->second;
        }
    } else {
        t = chans_[ch].timbre;
    }
    if (!t || t->size() != 12) return;
    Slot* sp = nullptr;
    for (Slot& s : slots_)
        if (!s.active) {
            sp = &s;
            break;
        }
    if (!sp) return;
    Slot& s = *sp;
    const Timbre& timbre = *t;
    s.ch = ch;
    s.key = key;
    // a drum plays at the pitch its timbre's transpose byte names
    if (ch == 9) {
        s.note = timbre[0];
        s.transpose = 0;
    } else {
        s.note = key;
        s.transpose = S8(timbre[0]);
    }
    s.vel = velocity_[vel / 8];
    s.active = true;
    s.sustained = false;
    s.voice = -1;
    // Copy a 14-byte timbre into the slot (1d6c). Bytes, after the length:
    // transpose, then the modulator's 20/40/60/80/E0 registers, C0, and the
    // carrier's 20/40/60/80/E0.
    s.keyon = 0x20;
    s.fbc = timbre[6];
    s.conn = timbre[6] % 2;
    s.mod_ksl = timbre[2] - timbre[2] % 64;
    s.mod_level = 63 - timbre[2] % 64;
    s.car_ksl = timbre[8] - timbre[8] % 64;
    s.car_level = 63 - timbre[8] % 64;
    s.mod_avek = timbre[1] - timbre[1] % 16;
    s.mod_mult = timbre[1] % 16;
    s.car_avek = timbre[7] - timbre[7] % 16;
    s.car_mult = timbre[7] % 16;
    s.mod_ad = timbre[3];
    s.mod_sr = timbre[4];
    s.mod_ws = timbre[5];
    s.car_ad = timbre[9];
    s.car_sr = timbre[10];
    s.car_ws = timbre[11];
    // which operators follow the volume: the carrier always, the modulator
    // only when it is heard directly (additive)
    s.scale_mod = s.conn == 1;
    s.flags = 0xf9;
    Allocate(s);
}

void AilFm::NoteOff(int ch, int key) {
    Chan& c = chans_[ch];
    for (Slot& s : slots_) {
        if (s.active && s.key == key && s.ch == ch) {
            if (c.sustain >= 64) {
                s.sustained = true;
            } else {
                Release(s);
                s.active = false;
            }
        }
    }
}

void AilFm::Controller(int ch, int n, int v) {
    Chan& c = chans_[ch];
    int bitv = 0;
    int* field = nullptr;
    if (n == 114)
        c.bank = v;
    else if (n == 112)
        c.lock = v;
    else if (n == 64) {
        c.sustain = v;
        if (v < 64) {
            for (Slot& s : slots_)
                if (s.active && s.ch == ch && s.sustained) NoteOff(ch, s.key);
        }
    } else if (n == 1) {
        field = &c.mod;
        bitv = 0x80;
    } else if (n == 7) {
        field = &c.vol;
        bitv = 0x40;
    } else if (n == 11) {
        field = &c.expr;
        bitv = 0x40;
    } else if (n == 10) {
        field = &c.pan;
        bitv = 0x40;
    }
    if (field) {
        *field = v;
        for (Slot& s : slots_) {
            if (s.active && s.ch == ch) {
                if ((s.flags / bitv) % 2 == 0) s.flags += bitv;
                Update(s);
            }
        }
    }
}

// One MIDI message (2009). Notes only sound on channels 2-10 (1-9 here).
void AilFm::Message(int st, int a, int b) {
    int hi = st & 0xf0, ch = st & 15;
    if (hi == 0x90) {
        if (ch >= 1 && ch <= 9) NoteOn(ch, a, b);
    } else if (hi == 0x80) {
        NoteOff(ch, a);
    } else if (hi == 0xb0) {
        Controller(ch, a, b);
    } else if (hi == 0xc0) {
        Program(ch, a);
    } else if (hi == 0xe0) {
        chans_[ch].bend = b * 128 + a - 0x2000;
        for (Slot& s : slots_) {
            if (s.active && s.ch == ch) {
                if (s.flags % 2 == 0) s.flags++;
                Update(s);
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
    for (Slot& s : slots_) {
        if (s.active) {
            Release(s);
            s.active = false;
        }
    }
    playing_ = false;
}

// one interval of AIL's 120 Hz timer
void AilFm::Interval() {
    const xmi::Sequence* q = seq_;
    if (!playing_ || !q) return;
    int t = tick_;
    while (pos_ < q->events.size() && q->events[pos_].t <= t) {
        const auto& e = q->events[pos_];
        Message(e.st, e.a, e.b);
        pos_++;
    }
    tick_ = t + 1;
    if (pos_ >= q->events.size() && tick_ > q->length) {
        if (loop_)
            Play(q, true);
        else
            Stop();
    }
}

bool AilFm::Idle() const {
    return !playing_ && chip_.Silent();
}

void AilFm::Render(int16_t* out, int n) {
    const double per = Opl::kRate / xmi::kTickRate;
    int i = 0;
    while (i < n) {
        if (until_tick_ <= 0) {
            Interval();
            until_tick_ += per;
        }
        int k = std::min(n - i, static_cast<int>(std::ceil(until_tick_)));
        chip_.Generate(out + i, k);
        i += k;
        until_tick_ -= k;
    }
}

}  // namespace w2
