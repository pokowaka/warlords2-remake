# Warlords II — remade

A remake of SSG's *Warlords II* (1993) that reads the original game's
own data files directly at runtime: the maps, armies, art, music, and
rules are decoded straight from the 1993 DOS assets rather than redrawn
or converted.

Play it in the browser: <https://jstrand.github.io/warlords2-remake/>

Steve Fawkner, who holds the rights to *Warlords* based video games, has given `jstrand` permission for this remake to be published.

---

## Complete Rework & Modern C++23 Architecture

This branch (`oo-ai`) is a **complete architectural rework** of the repository. Earlier prototypes and duplicate ports (the Lua/LÖVE engine and handwritten JavaScript port) have been retired in favor of a single, unified **modern C++23 engine** (`cpp/`) that serves as the sole source of truth for both desktop (SDL2) and web (WebAssembly via Emscripten):

- **Encapsulated Object-Oriented Domain & Zero Mutable Globals**: Re-engineered into a strict acyclic graph of 12 layered Bazel libraries (`//cpp:util` → `model` → `formats` → `rules` → `ai` → `worldgen` → `persistence` → `platform` → `ui_core` → `ui_dialogs` → `ui_front` → `warlords2`), using compile-time strong entity identifiers (`SideId`, `CityId`, `ArmyId`, `ArmyTypeId`), RAII platform wrappers, explicit `UiContext` dependency injection across all 28 modal dialogs, and exception-free `absl::StatusOr<T>` error boundaries.
- **Headless Core & Multi-Threaded Turn Monitor**: The core rules, world generation, and AI (`//cpp:w2core`) have zero dependency on SDL2 or UI headers, enabling deterministic sub-second unit tests and high-throughput multi-threaded self-play. In interactive play, `AiTurnRunner` coordinates background AI computation with main-thread UI animations via an `absl::Mutex` monitor (or cooperative event queues in single-threaded WebAssembly).
- **Unified WebAssembly Release**: The browser version compiles the C++23 engine directly to WebAssembly (`warlords2.wasm`), featuring real-time AdLib / OPL2 FM synthesis (`AilFm`) and real-time SoundFont synthesis for Roland MT-32 and Roland Sound Canvas SC-55 music (eliminating 160+ MB of pre-rendered audio across both desktop and web releases) and persistent saves/preferences via IndexedDB (`IDBFS`).
- **Strict Quality Gates**: Enforced by 11 headless GoogleTest suites, hermetic SDL/UI tests, 15 full-game golden trace regressions, automated screenshot verification, and zero-warning `clang-tidy` enforcement (`sh tools/check.sh`).

---

## Computer Players & Modern ML AI

Two distinct computer player architectures implement `IComputerPlayer` and can be mixed freely per faction:

| | Classic AI (`--classic-ai`) | Modern ML AI (`--modern-ai`, default) |
|---|---|---|
| **Architecture** | Faithful 19-phase expert system reverse-engineered from 1993 `WARLORD2.EXE` (`ClassicAiPlayer`) | Learned neural policy (`ModernMlPlayer`) with `v8` multi-head residual MLPs (`cpp/ai_weights.json`) |
| **Information** | Omniscient (reads the full map even under Hidden Map, as the 1993 original does) | **Fair information**: sees only what a human player would via per-side `SideBelief` fog-of-war tracking |
| **Capabilities** | Original 1993 targeting, city claiming, production, hero recruitment, and `.CRD` personality cards | Multi-head `PolicyNetwork` (`DiploNet`, `MissionNet`, `EconNet`, `ValueNet`, `StackValueNet`, `MusterNet`, `Commander` + MoE experts), 27 affine personality adapters (5 archetypes across 9 Knights/Lords/Warlords levels), cross-turn Order Memory, Field-Stack Rallying, standard planting/vectoring, and optional online portfolio lookahead search (`--ai-search`) |

The Modern AI is trained entirely in C++ (no external ML framework) using PPO self-play with asymmetric privileged critics (`w2arena`), paired-seed antithetic Evolution Strategies (`w2evolve`), and behavioral cloning (`w2imitate`) against Classic, Raider, Warden, and Frozen opponents. With `--modern-ui`, players also get enhanced diplomatic Parley & Attitude Ledgers, turn-start Envoy Missives, and the **`F10` AI Brain Inspector** to inspect neural policy utilities and belief state live. See [`AI-DESIGN.md`](AI-DESIGN.md) for the full architecture and training pipeline.

---

## What's here

| Path | Description |
|---|---|
| [`cpp/`](cpp/README.md) | Modern C++23 engine on SDL2 (single source of truth for rules, AI, UI, and tests) |
| [`web/`](web/README.md) | Browser WebAssembly release compiled from `cpp/` via Emscripten |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Detailed guide to the layered Bazel graph, domain models, `AiTurnRunner`, and UI state machine |
| [`AI-DESIGN.md`](AI-DESIGN.md) | Design, network topology (`v7`/`v8`), training curriculum, and evaluation of the Modern ML AI |
| [`docs/`](docs/) | Binary file format specs, decoded 1993 game rules, and reverse engineering notes for `WARLORD2.EXE` |
| `tools/` | Web builder (`build_web.sh`), quality gate runner (`check.sh`, `tidy.sh`), format decoders, and Ghidra/DOSBox helpers |
| `original/` | The original 1993 game's data files, read directly at runtime |

---

## Quick start

```sh
# Desktop C++23 on SDL2 (requires Bazel / bazelisk)
bazel run //:warlords2

# Build and serve the WebAssembly browser release at http://localhost:8765/
./tools/build_web.sh --serve --port 8765

# Run all unit tests, golden game traces, UI checks, and clang-tidy
sh tools/check.sh
```

Run from the repository root so that `original/` is found. See [`cpp/README.md`](cpp/README.md) and [`web/README.md`](web/README.md) for CLI flags, controller bindings (including Steam Deck support), and AI training tools (`w2arena`, `w2evolve`, `w2search`, `w2aidiag`).

---

## Beyond the original

On a 640 × 480 screen the game looks as the original does, pixel for pixel.
On a bigger one the map grows to fill it, and the strategic map, the control
panel, and the bottom bar move to the edges. A few things are new:

| To | Do |
|---|---|
| **Choose the AI & UI** | Game › Modern AI / Classic AI toggles the fair-information neural AI vs the 1993 AI; `--modern-ui` enables diplomatic Parley ledgers and the `F10` AI Brain Inspector |
| **Generate worlds** | Play classic 1993 `RANDOM.DAT` maps or modern balanced random worlds (`scengen` / `mapeval`) |
| **Scale the interface** | View › Interface 1x, 2x… picks a whole-number scale so the pixel art stays sharp; the biggest that fits is the default |
| **Zoom & pan the map** | Mouse wheel, `PageUp` / `PageDown`, keypad `+` / `-`, or View › Map 1x, 2x…; drag the map with the pointer for smooth pixel panning |
| **Switch display & CRT** | View › Full screen / Window, View › 4:3 aspect lock, and optional OpenGL CRT shader (curvature, scanlines, aperture grille, and halation glow) |
| **Save quickly** | `F5` quick-saves, `F9` quick-loads (persisted as JSON on desktop or IndexedDB in the browser) |
| **Change the music** | Game › AdLib music (live OPL2 & AIL synth), MT-32 music, or SC-55 music (live TinySoundFont synthesis) |

The View menu's choices and the music settings are remembered for next time.
