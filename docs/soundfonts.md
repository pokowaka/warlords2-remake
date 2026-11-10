# SoundFonts, Music Synthesis, and Sample Choices

This document details the architecture, sample selection, and validation methodology for the Roland MT-32 and Roland Sound Canvas (SC-55) real-time music synthesis in the Warlords II remake.

---

## 1. Background and Motivation

The original 1993 DOS release of Warlords II supported three distinct music synthesizer standards, represented by three sets of Extended MIDI (`.XMI`) files:

| Driver | Prefix | Target Hardware | Arrangement Character |
|---|---|---|---|
| `ADLIB.ADV` | `S*.XMI` | Yamaha YM3812 (OPL2) FM synthesis | 9-channel FM synth using custom timbres in `MIDPAK.AD` |
| `MT32MPU.ADV` | `M*.XMI` | Roland MT-32 / LAPC-1 | Roland LA synthesis (synthesized transients + PCM samples) |
| `SC32MPU.ADV` | `R*.XMI` | Roland Sound Canvas SC-55 | General MIDI / GS wavetable sample playback |

Early digital remakes often relied on bundling pre-rendered digital audio tracks (OGG Vorbis or MP3), consuming over 160 MB of storage and bandwidth. In this modern C++23 remake, all three music backends synthesize audio in real time:

- **AdLib / FM**: Live cycle-accurate OPL2 FM emulation via [`w2::AilFm`](file:///Users/erwinj/src/warlords2-remake/cpp/src/warlords/ailfm.hpp) and [`opl.cpp`](file:///Users/erwinj/src/warlords2-remake/cpp/src/warlords/opl.cpp).
- **Roland MT-32 & Sound Canvas SC-55**: Live SoundFont2 (SF2) wavetable synthesis via [`sound::SoundFontSynth`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/soundfont.hpp) wrapping TinySoundFont (`tsf`).

This eliminated 160+ MB of pre-rendered files, allowing the full game and all soundtrack options to run instantaneously on native desktop builds and in WebAssembly (`warlords2.wasm`).

---

## 2. Sample Choices and SoundFont Selection

### Base Bank: GeneralUser GS v1.471
To provide out-of-the-box wavetable synthesis without requiring proprietary ROM dumps, the game ships with [`reference/original/SOUND/warlords2.sf2`](file:///Users/erwinj/src/warlords2-remake/reference/original/SOUND/warlords2.sf2), derived from **GeneralUser GS v1.471** by S. Christian Collins:

1. **Standards Compliance**: Conforms closely to the Roland Sound Canvas SC-55 / GS specification, with instrument assignments and controller responses matching the General MIDI standard.
2. **Drum Consistency**: Includes a balanced standard General MIDI drum kit (Bank 128, Preset 0) suitable for Warlords II's battle and turn marches.
3. **Licensing**: Permissive license allowing redistribution and pruning for open-source and non-commercial game engines.

### SoundFont Pruning (`tools/prune_soundfont.py`)
A full GeneralUser GS bank is ~30 MB uncompressed. Because Warlords II uses only a subset of MIDI instruments across its 24 compositions, [`tools/prune_soundfont.py`](file:///Users/erwinj/src/warlords2-remake/tools/prune_soundfont.py) scans all `R*.XMI` and `M*.XMI` files:
- Retains exactly the **38 melodic presets** and **1 drum kit** utilized in the game.
- Preserves all 22 active standard GM drum notes plus all MT-32 remapped percussion keys.
- Reduces the distribution SoundFont size down to ~5.8 MB without any loss in audio quality.

### Authentic Hardware ROM SoundFonts
For audiophiles desiring the exact sound of physical Roland hardware:
- [`AudioEngine::ResolveSoundFontPath()`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/sound.cpp#L13-L21) automatically probes for `SOUND/SC-55.sf2` or `SOUND/SC-55.SF2` in the data directory before falling back to `warlords2.sf2`.
- Dropping an authentic Roland SC-55 ROM soundfont into `SOUND/` immediately activates hardware-identical sample playback.

---

## 3. MT-32 vs SC-55 Mapping

### Melodic Program Mapping
The Roland MT-32 pre-dates General MIDI, using different program numbers for its timbre library. When playing `M*.XMI` songs, [`SoundFontSynth::Interval()`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/soundfont.cpp#L190-L215) translates program change events (`0xC0`) using the lookup table `kMt32ToGm`:

```cpp
// Roland MT-32 timbre index -> General MIDI program index
constexpr std::array<uint8_t, 128> kMt32ToGm = { ... };
```

### Drum Key Discrepancies
Unlike General MIDI where drum kits are standardized across notes 35–81, the MT-32 drum machine assigned several Latin percussion instruments to different key numbers.

When played directly through a General MIDI bank, MT-32 tracks produced jarring errors (e.g. playing Handclaps instead of Shakers, or Bongos instead of Wood Blocks).

[`RemapMt32Drum()`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/soundfont.cpp#L43-L68) remaps MT-32 percussion keys on Channel 10 (channel index 9):

| MT-32 Key | Original MT-32 Instrument | Unmapped GM Sound (Buggy) | Remapped GM Key | Remapped GM Instrument (Correct) |
|---|---|---|---|---|
| **39** | Shaker | 39: Handclap | **82** | Shaker |
| **60** | Hi Wood Block | 60: High Bongo | **76** | Hi Wood Block |
| **61** | Low Wood Block | 61: Low Bongo | **77** | Low Wood Block |
| **62** | Mute Triangle | 62: Mute Hi Conga | **80** | Mute Triangle |
| **73** | Vibraslap / Quijada | 73: Short Guiro | **58** | Vibraslap |

---

## 4. Audio Validation Against Reference Vorbis Tracks

To verify synthesis accuracy, we compared real-time SoundFont rendering against ground-truth reference Vorbis recordings captured from dedicated emulator hardware (`88EmuCli` for SC-55 and `MUNT` for MT-32):

1. **Rendering Tool (`//cpp:w2sfrender`)**:
   [`cpp/test/sfrender.cpp`](file:///Users/erwinj/src/warlords2-remake/cpp/test/sfrender.cpp) performs headless, offline rendering of any `.XMI` using [`SoundFontSynth`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/soundfont.hpp) to 44.1 kHz 16-bit stereo WAV.
2. **Test Suite**:
   Rendered and evaluated 10 representative songs across both sound sets:
   - `RSTARTUP`, `RINT0`, `RINT2`, `RINT14`, `RINT22` (SC-55)
   - `MSTARTUP`, `MINT0`, `MINT2`, `MINT14`, `MINT22` (MT-32)
3. **Findings & Root Causes**:
   - **Melodic instruments**: Frequency spectrum and timing matched the reference tracks cleanly.
   - **TinySoundFont Channel Reset Bug**: TinySoundFont's `tsf_reset()` wiped channel preset assignments. Songs that did not explicitly emit a `0xC9` program change (e.g. `RINT21.XMI` and `MINT21.XMI`) defaulted Channel 10 to Bank 0 Preset 0 (Harpsichord), turning drum hits into piano/harpsichord notes.
   - **Fix**: In [`SoundFontSynth::Play()`](file:///Users/erwinj/src/warlords2-remake/cpp/src/platform/soundfont.cpp#L161-L168), all 16 channels are explicitly initialized upon song start, ensuring channel 9 is permanently set to Bank 128 (`tsf_channel_set_presetnumber(soundfont_, ch, 0, ch == 9 ? 1 : 0)`).

---

## 5. Verification & Test Coverage

- **Automated Unit Tests**:
  [`cpp/test/platform_raii_test.cpp`](file:///Users/erwinj/src/warlords2-remake/cpp/test/platform_raii_test.cpp#L432-L467) verifies:
  - MT-32 percussion remapping during `MINT2.XMI` playback.
  - Channel 9 drum persistence across repeated stops and starts on tracks without `0xC9` (`RINT21.XMI`).
- **Quality Gates**:
  All changes pass Bazel compilation, unit tests (`bazel test //cpp/...`), and clang-tidy (`tools/tidy.sh`) with 0 findings.
