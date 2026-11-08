#include "warlords/opl.hpp"

#include <cmath>

namespace w2 {

namespace {

// The chip's ROMs, as the die shows them: a quarter sine in log2 form, 256
// steps to the octave, and the 2^x table that undoes it.
struct Roms {
    int logsin[256], exp[256];
    Roms() {
        for (int i = 0; i < 256; i++) {
            logsin[i] = static_cast<int>(std::floor(
                    -std::log(std::sin((i + 0.5) * M_PI / 512)) / std::log(2.0) * 256 + 0.5));
            exp[i] = static_cast<int>(std::floor(std::pow(2.0, (255 - i) / 256.0) * 1024 + 0.5));
        }
    }
};
const Roms kR;

// frequency multiplier, doubled (the chip's 0.5 is 1)
const int kMt[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
const int kKslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
const int kKslshift[4] = {8, 1, 2, 0};
const int kIncstep[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};

// operator register offsets -> operator number (-1 for none)
const int kSlotOfReg[32] = {0,  1,  2,  3,  4,  5,  -1, -1, 6,  7,  8,  9,  10, 11, -1, -1,
                            12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
const int kChMod[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};

enum { kAttack, kDecay, kSustain, kRelease };

// the exponent: level 0 is loudest, every 256 halves it
inline int CalcExp(int level) {
    if (level > 0x1fff) level = 0x1fff;
    return (kR.exp[level & 0xff] * 2) >> (level >> 8);
}

inline int LogsinOf(int phase) {
    return (phase & 0x100) ? kR.logsin[(phase & 0xff) ^ 0xff] : kR.logsin[phase & 0xff];
}

// the four OPL2 waveforms: sine, half sine, absolute sine, quarter pulses
inline int Wave(int wf, int phase, int env) {
    phase &= 0x3ff;
    int lv;
    if (wf == 0) {
        int v = CalcExp(LogsinOf(phase) + env * 8);
        return (phase & 0x200) ? -v - 1 : v;
    } else if (wf == 1) {
        lv = (phase & 0x200) ? 0x1000 : LogsinOf(phase);
    } else if (wf == 2) {
        lv = LogsinOf(phase);
    } else {
        lv = (phase & 0x100) ? 0x1000 : kR.logsin[phase & 0xff];
    }
    return CalcExp(lv + env * 8);
}

}  // namespace

Opl::Opl() {
    for (int c = 0; c < 9; c++) {
        ch_[c].mod = &slots_[kChMod[c]];
        ch_[c].car = &slots_[kChMod[c] + 3];
    }
}

static void UpdateKsl(int& ksl, int& ksv, int fnum, int block) {
    int k = kKslrom[fnum >> 6] * 4 - (8 - block) * 32;
    ksl = k < 0 ? 0 : k;
    ksv = block * 2 + ((fnum >> 9) & 1);
}

void Opl::Write(int reg, int v) {
    int hi = reg & 0xe0;
    if (reg == 0x01) {
        wse_ = (v & 0x20) != 0;
    } else if (reg == 0xbd) {
        dam_ = (v & 0x80) != 0;
        dvb_ = (v & 0x40) != 0;
    } else if ((hi >= 0x20 && hi <= 0x80) || hi == 0xe0) {
        int n = kSlotOfReg[reg & 0x1f];
        if (n < 0) return;
        Slot& s = slots_[n];
        if (hi == 0x20) {
            s.am = v >> 7;
            s.vib = (v >> 6) & 1;
            s.egt = (v >> 5) & 1;
            s.ksr = (v >> 4) & 1;
            s.mult = v & 15;
        } else if (hi == 0x40) {
            s.ksl = v >> 6;
            s.tl = v & 0x3f;
        } else if (hi == 0x60) {
            s.ar = v >> 4;
            s.dr = v & 15;
        } else if (hi == 0x80) {
            s.sl = v >> 4;
            s.rr = v & 15;
            if (s.sl == 15) s.sl = 31;
        } else {
            s.wf = wse_ ? (v & 3) : 0;
        }
    } else if (reg >= 0xa0 && reg <= 0xa8) {
        Channel& c = ch_[reg - 0xa0];
        c.fnum = (c.fnum & 0x300) | v;
        UpdateKsl(c.ksl, c.ksv, c.fnum, c.block);
    } else if (reg >= 0xb0 && reg <= 0xb8) {
        Channel& c = ch_[reg - 0xb0];
        c.fnum = (c.fnum & 0xff) | ((v & 3) << 8);
        c.block = (v >> 2) & 7;
        UpdateKsl(c.ksl, c.ksv, c.fnum, c.block);
        bool on = (v & 0x20) != 0;
        c.mod->key = on;
        c.car->key = on;
    } else if (reg >= 0xc0 && reg <= 0xc8) {
        Channel& c = ch_[reg - 0xc0];
        c.fb = (v >> 1) & 7;
        c.con = v & 1;
    }
}

// One step of an operator's envelope generator (Nuked's OPL3_EnvelopeCalc).
void Opl::Envelope(Slot& s, const Channel& c) {
    int eo = s.rout + s.tl * 4 + (c.ksl >> kKslshift[s.ksl]);
    if (s.am) eo += trem_;
    s.eg_out = eo > 0x1ff ? 0x1ff : eo;

    bool reset = false;
    int rate;
    if (s.key && s.gen == kRelease) {
        reset = true;
        rate = s.ar;
    } else if (s.gen == kAttack)
        rate = s.ar;
    else if (s.gen == kDecay)
        rate = s.dr;
    else if (s.gen == kSustain)
        rate = s.egt == 0 ? s.rr : 0;
    else
        rate = s.rr;
    s.reset = reset;

    int shift = 0, rate_hi = 0;
    if (rate != 0) {
        int ks = c.ksv >> (s.ksr == 1 ? 0 : 2);
        int r = ks + rate * 4;
        rate_hi = r >> 2;
        int rate_lo = r & 3;
        if (rate_hi > 15) rate_hi = 15;
        if (rate_hi < 12) {
            if (eg_state_ == 1) {
                int es = rate_hi + eg_add_;
                if (es == 12)
                    shift = 1;
                else if (es == 13)
                    shift = (rate_lo >> 1) & 1;
                else if (es == 14)
                    shift = rate_lo & 1;
            }
        } else {
            shift = (rate_hi & 3) + kIncstep[rate_lo][eg_timer_lo_];
            if (shift & 4) shift = 3;
            if (shift == 0) shift = eg_state_;
        }
    }

    int rout = s.rout, inc = 0;
    if (reset && rate_hi == 15) rout = 0;
    bool off = (s.rout & 0x1f8) == 0x1f8;
    if (s.gen != kAttack && !reset && off) rout = 0x1ff;
    if (s.gen == kAttack) {
        if (s.rout == 0)
            s.gen = kDecay;
        else if (s.key && shift > 0 && rate_hi != 15)
            inc = static_cast<int>(std::floor((-s.rout - 1) / std::pow(2.0, 4 - shift)));
    } else if (s.gen == kDecay) {
        if ((s.rout >> 4) == s.sl)
            s.gen = kSustain;
        else if (!off && !reset && shift > 0)
            inc = 1 << (shift - 1);
    } else {
        if (!off && !reset && shift > 0) inc = 1 << (shift - 1);
    }
    s.rout = (rout + inc) & 0x1ff;
    if (reset) s.gen = kAttack;
    if (!s.key) s.gen = kRelease;
}

// the phase generator, with the chip's vibrato
void Opl::PhaseGen(Slot& s, const Channel& c) {
    int f = c.fnum;
    if (s.vib) {
        int range = (f >> 7) & 7;
        int vp = vib_pos_;
        if ((vp & 3) == 0)
            range = 0;
        else if (vp & 1)
            range >>= 1;
        if (!dvb_) range >>= 1;
        if (vp & 4) range = -range;
        f += range;
    }
    uint32_t base = (static_cast<uint32_t>(f << c.block) & 0xffffffff) >> 1;
    s.phase_out = static_cast<int>(s.phase >> 9);
    if (s.reset) s.phase = 0;
    s.phase = (s.phase + ((base * kMt[s.mult]) >> 1)) & 0x7ffff;
}

bool Opl::Silent() const {
    for (const Slot& s : slots_)
        if (s.key || s.gen != kRelease || s.rout < 0x1f8) return false;
    return true;
}

void Opl::Generate(int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (Channel& c : ch_) {
            Slot& m = *c.mod;
            Slot& k = *c.car;
            // skip a channel both of whose envelopes are silent
            if (!(m.key || k.key || m.rout < 0x1f8 || k.rout < 0x1f8 || m.gen != kRelease ||
                  k.gen != kRelease))
                continue;
            // modulator, with feedback from its last two outputs
            int fbmod = 0;
            if (c.fb != 0)
                fbmod = static_cast<int>(std::floor((m.prout + m.out) / std::pow(2.0, 9 - c.fb)));
            m.prout = m.out;
            Envelope(m, c);
            PhaseGen(m, c);
            m.out = Wave(m.wf, m.phase_out + fbmod, m.eg_out);
            // carrier, frequency-modulated by it (FM) or beside it (AM)
            k.prout = k.out;
            Envelope(k, c);
            PhaseGen(k, c);
            if (c.con == 0) {
                k.out = Wave(k.wf, k.phase_out + m.out, k.eg_out);
                acc += k.out;
            } else {
                k.out = Wave(k.wf, k.phase_out, k.eg_out);
                acc += m.out + k.out;
            }
        }
        if (acc > 32767)
            acc = 32767;
        else if (acc < -32768)
            acc = -32768;
        out[i] = static_cast<int16_t>(acc);

        // the chip's counters: tremolo every 64 samples, vibrato every 1024
        uint32_t t = timer_;
        if ((t & 0x3f) == 0x3f) trem_pos_ = (trem_pos_ + 1) % 210;
        int tp = trem_pos_;
        if (tp >= 105) tp = 210 - tp;
        trem_ = tp >> (dam_ ? 2 : 4);
        if ((t & 0x3ff) == 0x3ff) vib_pos_ = (vib_pos_ + 1) & 7;
        timer_ = (t + 1) & 0xffff;
        // the envelope clock: runs at half the sample rate, and each step's
        // trailing zeros pick which slow rates advance
        if (eg_state_ == 1) {
            uint32_t et = eg_timer_;
            int sh = 0;
            while (sh < 13 && ((et >> sh) & 1) == 0) sh++;
            eg_add_ = sh > 12 ? 0 : sh + 1;
            eg_timer_lo_ = static_cast<int>(et & 3);
            eg_timer_ = (et + 1) & 0xfffffff;
        }
        eg_state_ = 1 - eg_state_;
    }
}

}  // namespace w2
