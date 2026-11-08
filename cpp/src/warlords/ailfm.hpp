/**
 * @file
 * @brief The game's music player: Miles' AIL 2.0 AdLib driver (ADLIB.ADV).
 *
 * MIDPAK.COM loads the driver for WARLORD2.EXE; this is rebuilt from its
 * disassembly. The driver reads XMIDI (xmi) at 120 Hz, gives each note one of
 * nine OPL2 voices and writes the chip's registers from a timbre in the
 * Global Timbre Library, MIDPAK.AD. Its tables -- frequencies, the reset
 * values, the velocity curve -- are read from the player's own ADLIB.ADV.
 * The file layouts are in docs/formats/sound.md. A port of the Lua remake's
 * ailfm.lua.
 *
 *     AilFm driver(adv_bytes, ad_bytes);
 *     driver.Play(&seq, true);       // true: start again at the end
 *     driver.Render(out, n);         // n samples at Opl::kRate
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "absl/types/span.h"

#include "warlords/opl.hpp"
#include "warlords/xmi.hpp"

namespace w2 {

/** @brief The AIL AdLib driver playing an XMIDI sequence on an emulated OPL2. */
class AilFm {
  public:
    static constexpr double kRate = Opl::kRate;  ///< Samples a second, the chip's rate.

    /**
     * @brief Sets the driver up as its start-up does.
     * @param adv The bytes of ADLIB.ADV, for its tables.
     * @param library The bytes of MIDPAK.AD, the Global Timbre Library.
     */
    AilFm(absl::Span<const uint8_t> adv, absl::Span<const uint8_t> library);
    /** @brief Starts a parsed sequence from its beginning; it must outlive the play. */
    void Play(const xmi::Sequence* seq, bool loop);
    /** @brief Stops the sequence and lets every sounding note go. */
    void Stop();
    /** @brief Nothing playing and every note faded away. */
    bool Idle() const;
    /** @brief A sequence is playing. */
    bool Playing() const { return playing_; }
    /** @brief The next n samples, stepping the sequence as time passes. */
    void Render(int16_t* out, int n);

  private:
    using Timbre = std::vector<uint8_t>;  ///< A timbre's bytes after its u16 length.
    /** @brief One MIDI channel's controllers. */
    struct Chan {
        int vol = 0;                     ///< Volume (controller 7).
        int expr = 0;                    ///< Expression (controller 11).
        int mod = 0;                     ///< Modulation (controller 1); 64 and up is vibrato.
        int sustain = 0;                 ///< Sustain pedal (controller 64).
        int lock = 0;                    ///< Channel lock (controller 112).
        int bank = 0;                    ///< Timbre bank (controller 114).
        int bend = 0;                    ///< Pitch bend, -0x2000..0x1fff.
        int pan = 0;                     ///< Pan (controller 10), kept but unused.
        int voices = 0;                  ///< How many voices its notes hold.
        const Timbre* timbre = nullptr;  ///< The program's timbre, or none.
    };
    /** @brief A sounding note, with the operator settings copied from its timbre. */
    struct Slot {
        bool active = false;     ///< The slot holds a note.
        bool sustained = false;  ///< Released while the pedal was down.
        int voice = -1;          ///< The chip voice it sounds on, or -1 for none.
        int flags = 0;           ///< Which register groups need writing (see Update()).
        int ch = 0;              ///< Its MIDI channel.
        int key = 0;             ///< The MIDI key that started it.
        int note = 0;            ///< The note it plays.
        int transpose = 0;       ///< Half-tones added from the timbre.
        int vel = 0;             ///< Velocity after the driver's curve.
        int keyon = 0;           ///< 0x20 while the key is down, else 0.
        int fbc = 0;             ///< Feedback and connection register value.
        int conn = 0;            ///< Connection: 1 additive, 0 FM.
        int b0 = 0;              ///< Last value written to the voice's 0xb0 register.
        int mod_ksl = 0;         ///< Modulator key scale level bits.
        int mod_level = 0;       ///< Modulator output level, 0-63.
        int car_ksl = 0;         ///< Carrier key scale level bits.
        int car_level = 0;       ///< Carrier output level, 0-63.
        int mod_avek = 0;        ///< Modulator tremolo/vibrato/sustain/KSR bits.
        int mod_mult = 0;        ///< Modulator frequency multiplier.
        int car_avek = 0;        ///< Carrier tremolo/vibrato/sustain/KSR bits.
        int car_mult = 0;        ///< Carrier frequency multiplier.
        int mod_ad = 0;          ///< Modulator attack/decay.
        int mod_sr = 0;          ///< Modulator sustain/release.
        int mod_ws = 0;          ///< Modulator waveform.
        int car_ad = 0;          ///< Carrier attack/decay.
        int car_sr = 0;          ///< Carrier sustain/release.
        int car_ws = 0;          ///< Carrier waveform.
        bool scale_mod = false;  ///< The modulator follows the volume too.
    };

    /** @brief Sets a channel's program: its timbre from the current bank. */
    void Program(int channel, int program);
    /** @brief Writes whatever the slot's flags say has changed. */
    void Update(Slot& slot);
    /** @brief F-number and block for a slot's note; leaves them in fw_ and block_. */
    int Frequency(const Slot& slot, const Chan& chan);
    /** @brief Gives a slot a free voice, or lets the slots fight over them. */
    void Allocate(Slot& slot);
    /** @brief Hands voices from the weakest notes to stronger notes without one. */
    void Steal();
    /** @brief Keys a slot's note off and gives its voice back. */
    void Release(Slot& slot);
    /** @brief A MIDI note-on; velocity 0 is a note-off. */
    void NoteOn(int channel, int key, int vel);
    /** @brief A MIDI note-off, held while the sustain pedal is down. */
    void NoteOff(int channel, int key);
    /** @brief A MIDI controller change. */
    void Controller(int channel, int number, int value);
    /** @brief One MIDI message. */
    void Message(int status, int data1, int data2);
    /** @brief One interval of AIL's 120 Hz timer: plays the events now due. */
    void Interval();

    // ADLIB.ADV's tables
    int freq_[192];                            ///< F-numbers: 12 half-tones x 16 fine steps.
    int octave_[96];                           ///< The octave of each note from C-2.
    int halftone_[96];                         ///< The half-tone of each note.
    int reset_[0xf6];                          ///< Register values written at start-up.
    int velocity_[16];                         ///< Velocity / 8 -> sensitivity.
    int programs_[9];                          ///< The program each channel 1..8 starts on.
    std::map<int, Timbre> lib_;                ///< The timbre library by bank * 128 + patch.
    Opl chip_;                                 ///< The emulated chip.
    Chan chans_[16];                           ///< The 16 MIDI channels.
    std::map<int, const Timbre*> percussion_;  ///< Channel 10's timbres by key, cached.
    Slot slots_[16];                           ///< The notes, sounding or waiting for a voice.
    int owner_[9];                             ///< Voice -> channel, or -1 when free.
    int next_voice_ = 0;                       ///< The last voice handed out.
    int fw_ = 0;                               ///< The F-number Frequency() worked out.
    int block_ = 0;                            ///< The block Frequency() worked out.
    const xmi::Sequence* seq_ = nullptr;       ///< The sequence playing.
    size_t pos_ = 0;                           ///< The next event to play.
    int tick_ = 0;                             ///< The sequence's clock.
    double until_tick_ = 0;                    ///< Samples left until the next tick.
    bool playing_ = false;                     ///< A sequence is playing.
    bool loop_ = false;                        ///< Start again at the end.
};

}  // namespace w2
