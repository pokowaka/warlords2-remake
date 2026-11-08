/**
 * @file
 * @brief A Yamaha YM3812 (OPL2), the FM chip on the AdLib and the Sound Blaster.
 *
 * The game's music is played by Miles' AIL AdLib driver (ADLIB.ADV, see
 * ailfm) writing registers to this chip, so this is the last link to the
 * sound the original makes. It follows Nuked-OPL3's model of the chip -- the
 * log-sine and exponent ROMs, the envelope generator's rate counter, the
 * tremolo and vibrato counters -- cut down to OPL2: nine two-operator
 * channels, four waveforms, no rhythm mode (the driver never turns it on).
 * A port of the Lua remake's opl.lua.
 *
 *     Opl chip;
 *     chip.Write(0x20, 0x01); ...
 *     chip.Generate(out, n);     // n signed samples, about +-32767
 *
 * It runs at the chip's own rate, kRate: a 3.579545 MHz crystal / 72.
 */
#pragma once

#include <cstdint>

namespace w2 {

/** @brief An emulated OPL2 chip: registers in, samples out. */
class Opl {
  public:
    static constexpr double kRate = 3579545.0 / 72;  ///< Samples a second, 49715.9 Hz.

    Opl();
    /** @brief Writes `value` to chip register `reg`, as the driver's port writes do. */
    void Write(int reg, int value);
    /** @brief Fills out[0..n-1] with the next n samples. */
    void Generate(int16_t* out, int n);
    /** @brief True once no operator can make a sound. */
    bool Silent() const;

  private:
    /** @brief One operator: its registers and its envelope and phase state. */
    struct Slot {
        int am = 0;          ///< Tremolo on (register 0x20 bit 7).
        int vib = 0;         ///< Vibrato on (0x20 bit 6).
        int egt = 0;         ///< Sustaining envelope (0x20 bit 5).
        int ksr = 0;         ///< Key scale rate (0x20 bit 4).
        int mult = 0;        ///< Frequency multiplier index (0x20 low bits).
        int ksl = 0;         ///< Key scale level (0x40 high bits).
        int tl = 0;          ///< Total level, the attenuation (0x40 low bits).
        int ar = 0;          ///< Attack rate.
        int dr = 0;          ///< Decay rate.
        int sl = 0;          ///< Sustain level; 15 is stored as 31.
        int rr = 0;          ///< Release rate.
        int wf = 0;          ///< Waveform, 0-3.
        bool key = false;    ///< The key is down.
        bool reset = false;  ///< The envelope restarted this step.
        int gen = 3;         ///< Envelope stage: attack, decay, sustain or release.
        int rout = 0x1ff;    ///< Envelope level, 0 loudest to 0x1ff silent.
        int out = 0;         ///< Last output.
        int prout = 0;       ///< The output before that, for feedback.
        uint32_t phase = 0;  ///< Phase accumulator.
        int phase_out = 0;   ///< Phase as fed to the waveform.
        int eg_out = 0x1ff;  ///< Envelope with total level, key scaling and tremolo added.
    };
    /** @brief One two-operator channel. */
    struct Channel {
        int fnum = 0;         ///< F-number, 10 bits.
        int block = 0;        ///< Octave.
        int fb = 0;           ///< Modulator feedback, 0-7.
        int con = 0;          ///< Connection: 0 FM, 1 additive.
        int ksv = 0;          ///< Key scale value for rates.
        int ksl = 0;          ///< Key scale level for attenuation.
        Slot* mod = nullptr;  ///< The modulator operator.
        Slot* car = nullptr;  ///< The carrier operator.
    };
    /** @brief One step of an operator's envelope generator (Nuked's OPL3_EnvelopeCalc). */
    void Envelope(Slot& slot, const Channel& channel);
    /** @brief One step of an operator's phase generator, with the chip's vibrato. */
    void PhaseGen(Slot& slot, const Channel& channel);

    Slot slots_[18];         ///< The 18 operators.
    Channel ch_[9];          ///< The nine channels.
    bool wse_ = false;       ///< Waveform select enabled (register 0x01).
    bool dam_ = false;       ///< Deep tremolo (register 0xbd).
    bool dvb_ = false;       ///< Deep vibrato (register 0xbd).
    uint32_t timer_ = 0;     ///< Sample counter driving tremolo and vibrato.
    uint32_t eg_timer_ = 0;  ///< Envelope clock.
    int eg_state_ = 0;       ///< Toggles each sample; the envelopes step on half of them.
    int eg_add_ = 0;         ///< Which slow rates advance on this envelope step.
    int eg_timer_lo_ = 0;    ///< Low two bits of the envelope clock.
    int trem_pos_ = 0;       ///< Tremolo position, 0-209.
    int trem_ = 0;           ///< Current tremolo attenuation.
    int vib_pos_ = 0;        ///< Vibrato position, 0-7.
};

}  // namespace w2
