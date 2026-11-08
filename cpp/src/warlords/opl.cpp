#include "warlords/opl.hpp"

#include <cmath>

namespace w2 {

namespace {

/**
 * @brief The chip's ROMs, as the die shows them.
 *
 * A quarter sine in log2 form, 256 steps to the octave, and the 2^x table
 * that undoes it.
 */
struct Roms {
    int logsin[256];  ///< -log2(sin) of a quarter wave, in 1/256ths.
    int exp[256];     ///< 2^x of the fraction, scaled to 1024..2048.
    Roms() {
        for (int i = 0; i < 256; i++) {
            logsin[i] = static_cast<int>(std::floor(
                    -std::log(std::sin((i + 0.5) * M_PI / 512)) / std::log(2.0) * 256 + 0.5));
            exp[i] = static_cast<int>(std::floor(std::pow(2.0, (255 - i) / 256.0) * 1024 + 0.5));
        }
    }
};
const Roms kRoms;

/// Frequency multiplier, doubled (the chip's 0.5 is 1).
const int kMultiplier[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
/// Key scale level attenuation by the F-number's top four bits.
const int kKslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
/// How far each key scale level setting shifts the attenuation down.
const int kKslshift[4] = {8, 1, 2, 0};
/// Envelope increments of the fast rates, by the rate's low bits and the clock's.
const int kIncstep[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};

/// Operator register offsets -> operator number (-1 for none).
const int kSlotOfReg[32] = {0,  1,  2,  3,  4,  5,  -1, -1, 6,  7,  8,  9,  10, 11, -1, -1,
                            12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
/// Each channel's modulator operator; its carrier is three further on.
const int kChMod[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};

/** @brief The envelope generator's stages. */
enum {
    kAttack,   ///< Rising to full level.
    kDecay,    ///< Falling to the sustain level.
    kSustain,  ///< Holding (or releasing slowly, for a non-sustaining envelope).
    kRelease,  ///< Falling to silence after key off.
};

/** @brief The exponent: level 0 is loudest, every 256 halves it. */
inline int CalcExp(int level) {
    if (level > 0x1fff) level = 0x1fff;
    return (kRoms.exp[level & 0xff] * 2) >> (level >> 8);
}

/** @brief The log-sine of a half-wave phase, mirroring the quarter-wave ROM. */
inline int LogsinOf(int phase) {
    return (phase & 0x100) ? kRoms.logsin[(phase & 0xff) ^ 0xff] : kRoms.logsin[phase & 0xff];
}

/**
 * @brief One sample of an OPL2 waveform.
 * @param waveform 0 sine, 1 half sine, 2 absolute sine, 3 quarter pulses.
 * @param phase The phase, ten bits a cycle.
 * @param env The envelope attenuation.
 */
inline int Wave(int waveform, int phase, int env) {
    phase &= 0x3ff;
    int level;
    if (waveform == 0) {
        int value = CalcExp(LogsinOf(phase) + env * 8);
        return (phase & 0x200) ? -value - 1 : value;
    } else if (waveform == 1) {
        level = (phase & 0x200) ? 0x1000 : LogsinOf(phase);
    } else if (waveform == 2) {
        level = LogsinOf(phase);
    } else {
        level = (phase & 0x100) ? 0x1000 : kRoms.logsin[phase & 0xff];
    }
    return CalcExp(level + env * 8);
}

}  // namespace

Opl::Opl() {
    for (int channel = 0; channel < 9; channel++) {
        ch_[channel].mod = &slots_[kChMod[channel]];
        ch_[channel].car = &slots_[kChMod[channel] + 3];
    }
}

/** @brief Recomputes a channel's key scaling after its F-number or block changed. */
static void UpdateKsl(int& ksl, int& ksv, int fnum, int block) {
    int level = kKslrom[fnum >> 6] * 4 - (8 - block) * 32;
    ksl = level < 0 ? 0 : level;
    ksv = block * 2 + ((fnum >> 9) & 1);
}

void Opl::Write(int reg, int value) {
    int group = reg & 0xe0;
    if (reg == 0x01) {
        wse_ = (value & 0x20) != 0;
    } else if (reg == 0xbd) {
        dam_ = (value & 0x80) != 0;
        dvb_ = (value & 0x40) != 0;
    } else if ((group >= 0x20 && group <= 0x80) || group == 0xe0) {
        int slot_index = kSlotOfReg[reg & 0x1f];
        if (slot_index < 0) return;
        Slot& slot = slots_[slot_index];
        if (group == 0x20) {
            slot.am = value >> 7;
            slot.vib = (value >> 6) & 1;
            slot.egt = (value >> 5) & 1;
            slot.ksr = (value >> 4) & 1;
            slot.mult = value & 15;
        } else if (group == 0x40) {
            slot.ksl = value >> 6;
            slot.tl = value & 0x3f;
        } else if (group == 0x60) {
            slot.ar = value >> 4;
            slot.dr = value & 15;
        } else if (group == 0x80) {
            slot.sl = value >> 4;
            slot.rr = value & 15;
            if (slot.sl == 15) slot.sl = 31;
        } else {
            slot.wf = wse_ ? (value & 3) : 0;
        }
    } else if (reg >= 0xa0 && reg <= 0xa8) {
        Channel& channel = ch_[reg - 0xa0];
        channel.fnum = (channel.fnum & 0x300) | value;
        UpdateKsl(channel.ksl, channel.ksv, channel.fnum, channel.block);
    } else if (reg >= 0xb0 && reg <= 0xb8) {
        Channel& channel = ch_[reg - 0xb0];
        channel.fnum = (channel.fnum & 0xff) | ((value & 3) << 8);
        channel.block = (value >> 2) & 7;
        UpdateKsl(channel.ksl, channel.ksv, channel.fnum, channel.block);
        bool key_on = (value & 0x20) != 0;
        channel.mod->key = key_on;
        channel.car->key = key_on;
    } else if (reg >= 0xc0 && reg <= 0xc8) {
        Channel& channel = ch_[reg - 0xc0];
        channel.fb = (value >> 1) & 7;
        channel.con = value & 1;
    }
}

void Opl::Envelope(Slot& slot, const Channel& channel) {
    int level = slot.rout + slot.tl * 4 + (channel.ksl >> kKslshift[slot.ksl]);
    if (slot.am) level += trem_;
    slot.eg_out = level > 0x1ff ? 0x1ff : level;

    bool reset = false;
    int rate;
    if (slot.key && slot.gen == kRelease) {
        reset = true;
        rate = slot.ar;
    } else if (slot.gen == kAttack)
        rate = slot.ar;
    else if (slot.gen == kDecay)
        rate = slot.dr;
    else if (slot.gen == kSustain)
        rate = slot.egt == 0 ? slot.rr : 0;
    else
        rate = slot.rr;
    slot.reset = reset;

    int shift = 0, rate_hi = 0;
    if (rate != 0) {
        int key_scale = channel.ksv >> (slot.ksr == 1 ? 0 : 2);
        int scaled_rate = key_scale + rate * 4;
        rate_hi = scaled_rate >> 2;
        int rate_lo = scaled_rate & 3;
        if (rate_hi > 15) rate_hi = 15;
        if (rate_hi < 12) {
            if (eg_state_ == 1) {
                int step = rate_hi + eg_add_;
                if (step == 12)
                    shift = 1;
                else if (step == 13)
                    shift = (rate_lo >> 1) & 1;
                else if (step == 14)
                    shift = rate_lo & 1;
            }
        } else {
            shift = (rate_hi & 3) + kIncstep[rate_lo][eg_timer_lo_];
            if (shift & 4) shift = 3;
            if (shift == 0) shift = eg_state_;
        }
    }

    int rout = slot.rout, inc = 0;
    if (reset && rate_hi == 15) rout = 0;
    bool off = (slot.rout & 0x1f8) == 0x1f8;
    if (slot.gen != kAttack && !reset && off) rout = 0x1ff;
    if (slot.gen == kAttack) {
        if (slot.rout == 0)
            slot.gen = kDecay;
        else if (slot.key && shift > 0 && rate_hi != 15)
            inc = static_cast<int>(std::floor((-slot.rout - 1) / std::pow(2.0, 4 - shift)));
    } else if (slot.gen == kDecay) {
        if ((slot.rout >> 4) == slot.sl)
            slot.gen = kSustain;
        else if (!off && !reset && shift > 0)
            inc = 1 << (shift - 1);
    } else {
        if (!off && !reset && shift > 0) inc = 1 << (shift - 1);
    }
    slot.rout = (rout + inc) & 0x1ff;
    if (reset) slot.gen = kAttack;
    if (!slot.key) slot.gen = kRelease;
}

void Opl::PhaseGen(Slot& slot, const Channel& channel) {
    int fnum = channel.fnum;
    if (slot.vib) {
        int range = (fnum >> 7) & 7;
        int vib_pos = vib_pos_;
        if ((vib_pos & 3) == 0)
            range = 0;
        else if (vib_pos & 1)
            range >>= 1;
        if (!dvb_) range >>= 1;
        if (vib_pos & 4) range = -range;
        fnum += range;
    }
    uint32_t base = (static_cast<uint32_t>(fnum << channel.block) & 0xffffffff) >> 1;
    slot.phase_out = static_cast<int>(slot.phase >> 9);
    if (slot.reset) slot.phase = 0;
    slot.phase = (slot.phase + ((base * kMultiplier[slot.mult]) >> 1)) & 0x7ffff;
}

bool Opl::Silent() const {
    for (const Slot& slot : slots_)
        if (slot.key || slot.gen != kRelease || slot.rout < 0x1f8) return false;
    return true;
}

void Opl::Generate(int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (Channel& channel : ch_) {
            Slot& modulator = *channel.mod;
            Slot& carrier = *channel.car;
            // skip a channel both of whose envelopes are silent
            if (!(modulator.key || carrier.key || modulator.rout < 0x1f8 || carrier.rout < 0x1f8 ||
                  modulator.gen != kRelease || carrier.gen != kRelease))
                continue;
            // modulator, with feedback from its last two outputs
            int fbmod = 0;
            if (channel.fb != 0)
                fbmod = static_cast<int>(std::floor((modulator.prout + modulator.out) /
                                                    std::pow(2.0, 9 - channel.fb)));
            modulator.prout = modulator.out;
            Envelope(modulator, channel);
            PhaseGen(modulator, channel);
            modulator.out = Wave(modulator.wf, modulator.phase_out + fbmod, modulator.eg_out);
            // carrier, frequency-modulated by it (FM) or beside it (AM)
            carrier.prout = carrier.out;
            Envelope(carrier, channel);
            PhaseGen(carrier, channel);
            if (channel.con == 0) {
                carrier.out = Wave(carrier.wf, carrier.phase_out + modulator.out, carrier.eg_out);
                acc += carrier.out;
            } else {
                carrier.out = Wave(carrier.wf, carrier.phase_out, carrier.eg_out);
                acc += modulator.out + carrier.out;
            }
        }
        if (acc > 32767)
            acc = 32767;
        else if (acc < -32768)
            acc = -32768;
        out[i] = static_cast<int16_t>(acc);

        // the chip's counters: tremolo every 64 samples, vibrato every 1024
        uint32_t timer = timer_;
        if ((timer & 0x3f) == 0x3f) trem_pos_ = (trem_pos_ + 1) % 210;
        int trem_pos = trem_pos_;
        if (trem_pos >= 105) trem_pos = 210 - trem_pos;
        trem_ = trem_pos >> (dam_ ? 2 : 4);
        if ((timer & 0x3ff) == 0x3ff) vib_pos_ = (vib_pos_ + 1) & 7;
        timer_ = (timer + 1) & 0xffff;
        // the envelope clock: runs at half the sample rate, and each step's
        // trailing zeros pick which slow rates advance
        if (eg_state_ == 1) {
            uint32_t eg_timer = eg_timer_;
            int trailing_zeros = 0;
            while (trailing_zeros < 13 && ((eg_timer >> trailing_zeros) & 1) == 0) trailing_zeros++;
            eg_add_ = trailing_zeros > 12 ? 0 : trailing_zeros + 1;
            eg_timer_lo_ = static_cast<int>(eg_timer & 3);
            eg_timer_ = (eg_timer + 1) & 0xfffffff;
        }
        eg_state_ = 1 - eg_state_;
    }
}

}  // namespace w2
