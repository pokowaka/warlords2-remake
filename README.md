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
| **Capabilities** | Original 1993 targeting, city claiming, production, hero recruitment, and `.CRD` personality cards | Multi-head `PolicyNetwork` (`DiploNet`, `MissionNet`, `EconNet`, `ValueNet`, `StackValueNet`, `MusterNet`, `Commander` + MoE experts), 27 affine personality adapters (5 archetypes across 9 Knights/Lords/Warlords levels), cross-turn Order Memory, Field-Stack Rallying, standard planting/vectoring, and optional fair lookahead (`--warlord-lookahead` / `w2search`) |

---

### Modern AI: How It Works & What Was Done

The Modern AI represents a complete departure from 1990s strategy game AI design. Rather than relying on hardcoded heuristics or omniscience under fog of war, it is a fully learned neural agent engineered from the ground up to play fairly, strategically, and convincingly.

> [!NOTE]
> **Collaborative AI Engineering:** The entire Modern AI architecture, training harness, reverse engineering, and C++23 implementation was developed through an intensive collaboration between **Claude Opus 5.5** and **Gemini Argon**, pairing deep architectural deconstruction with rapid iterative C++ systems and reinforcement learning engineering.

#### 1. What Was Done
* **Elimination of "Cheating" Fog-of-War**: The 1993 original DOS AI completely ignored Hidden Map fog-of-war, seeing every player city, hero, and troop movement instantly. The Modern AI strictly observes human fog-of-war rules via `SideBelief`, scouting frontiers and remembering last-known locations without unfair information leaks.
* **Hermetic, Zero-Dependency C++ ML Engine**: The entire training pipeline—including forward/backward automatic differentiation, PPO actor-critic optimization, neural rollout buffers, and Evolution Strategies—was implemented entirely in pure C++23 (`cpp/src/warlords/ai/`). It requires zero external dependencies or heavyweight Python ML frameworks (no PyTorch, libtorch, or TensorFlow), enabling deterministic execution, sub-millisecond turn inference, and compilation directly into WebAssembly.
* **High-Throughput Headless Self-Play**: By decoupling the rules engine (`//cpp:w2core`) from rendering, the simulation runs at thousands of full turns per second across multi-core CPUs, allowing millions of game steps to be simulated in minutes.

#### 2. Architecture & Decision Pipeline
Each turn, the Modern AI (`modern::PlayTurn`) executes through a coordinated multi-head residual network:

1. **Belief State Update (`SideBelief`)**: Tracks visible terrain, estimated enemy stack trajectories, city ownership history, frontier fog lines, and localized threat heatmaps.
2. **Diplomacy (`DiploNet`)**: Evaluates bilateral relations across 4 distinct stances (*Allied*, *Peace*, *Uneasy*, *War*), determining peace tributes, war declarations, and multi-faction joint alliance requests.
3. **Empire Meeting Points & Side Standard (`MusterNet`)**: Selects `0..2` land-connected Muster Hub castles where feeder cities and nearby armies consolidate, and decides when a Hero plants or retrieves the faction's Standard for field vectoring.
4. **Multi-Wave Tactical Military Waves (`MissionNet`)**: In up to 6 iterative waves per turn, groups of armies evaluate every legal target (garrison holding, neutral claiming, enemy city sieges, field stack intercepts, ruin/temple exploration, Muster/Standard rallies, and fog scouting) paired with dynamic coalition sizes (1, 2, 4, $N-1$, or all armies) and selective obstacle detouring.
5. **Macro-Economy & Spoils (`EconNet`)**: Decides unit retention vs. disbanding when over budget, up to 3 empire-wide production-slot upgrades per turn, per-city unit production and vectoring, and city-capture spoils (occupy / pillage / sack / raze).
6. **Commander Posture & Mixture of Experts (MoE)**: High-level posture projection (`ctx[25..29]`) modulates decision heads based on game rules, map scale, and empire state.
7. **27 Affine Personality Adapters**: Exact affine Product-of-Experts (`ArchetypeHead`) blends 5 evolved specialists (`Imperial`, `Berserker`, `Guardian`, `Schemer`, `Questor`) across all 27 original character cards and `Knight` / `Lord` / `Warlord` difficulty tiers.

#### 3. Training Curriculum
* **Asymmetric-Critic PPO (`w2arena`)**: Trained via Proximal Policy Optimization where the policy network acts solely on imperfect `SideBelief`, while privileged critics (`ValueNet`, `StackValueNet`) observe the full map during training to guide stable advantage estimation.
* **Antithetic Evolution Strategies (`w2evolve`)**: Applies paired-seed parameter perturbations at deployment temperature (`T = 0.02`) with mid-game snapshot forking to refine consolidation, Muster Hubs, Standard vectoring, and specialist archetypes.
* **Adversarial Sparring & Imitation Probes (`opponents.cpp`, `w2imitate`)**: Evaluates and trains against the 1993 Classic AI, specialized defensive Wardens, empty-city Raiders, and frozen checkpoints across 4 disjoint seed splits.
* **Online Lookahead Search (`w2search` / `--warlord-lookahead`)**: Provides optional 1-ply Warlord tactical lookahead and 2-ply fair specialist portfolio rollout search, pushing Modern AI city share above 50% parity against the omniscient 1993 Classic AI.

#### 4. The `F10` AI Brain Inspector
When launched with `--modern-ui`, human players can press **`F10`** at any time to open the interactive **AI Brain Inspector**:
* Inspect the AI's internal mental map (`SideBelief`), showing what it thinks the world looks like versus ground truth.
* View live network logit utilities across candidate targets, diplomatic stances, and economic priorities.
* Review diplomatic Parley ledgers, attitude vectors, and turn-start envoy missives.

See [`AI-DESIGN.md`](AI-DESIGN.md) for complete mathematical formulations, network topologies, training curves, and tournament logs.

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
| `reference/` | 1993 DOS reference material: game assets (`reference/original/`), Ghidra decompilation (`reference/decompiled/`), and screenshots (`reference/screenshots/`) |
| `original/` | Symlink to `reference/original/` for runtime data access |

---

## Quick start

```sh
# Desktop C++23 on SDL2 (requires Bazel / bazelisk)
bazel run //:warlords2

# Build and serve the WebAssembly browser release at http://localhost:8765/
bazel run //:web

# Or build WebAssembly artifacts via Bazel without serving
bazel run //:web_build       # outputs to build/web/
bazel build //web:bundle     # outputs to bazel-bin/web/

# Run all unit tests, golden game traces, UI checks, and clang-tidy
sh tools/check.sh
```

Run from the repository root so that `original/` is found. See [`cpp/README.md`](cpp/README.md) and [`web/README.md`](web/README.md) for CLI flags, controller bindings (including Steam Deck support), and AI training tools (`w2arena`, `w2evolve`, `w2search`, `w2aidiag`).

---

## Generating Scenarios

You can generate fair, balanced custom scenarios with relief, river systems, cities, ruins, and start-menu preview pictures using the scenario generator (`w2scengen`):

```sh
# Generate a new scenario (writes to original/GEN<seed>/ and registers in New Scenario)
bazel run //:scengen

# Generate multiple scenarios with specific land shapes (continents, pangaea, islands)
bazel run //:scengen -- --shape continents --count 4 --seed 101
bazel run //:scengen -- --shape pangaea    --seed 102
bazel run //:scengen -- --shape islands    --seed 103

# Generate a named scenario with a custom city count (8 to 100)
bazel run //:scengen -- --name MYLAND --shape continents --cities 60

# Launch directly into a generated scenario
bazel run //:warlords2 -- --scenario GEN101
```

Generated scenarios are registered in `original/DATA/SCENGEN.LST` and appear automatically after the 7 standard scenarios in the **New Scenario** list with full crystal ball metadata and menu artwork. See [`docs/scengen.md`](docs/scengen.md) for complete details on generator parameters, fairness metrics, and preview PNG generation.

You can also use the modern scenario generator directly inside the game's **Random Map** screen:
* Click the **Generator:** line on the Random Map panel to cycle between `Classic (1993)`, `Modern (Continents)`, `Modern (Pangaea)`, and `Modern (Islands)` (your Water, Hills, Cities, and Forest sliders map directly into the modern generator).
* Toggle anytime via **Game › Classic map generator** or **Game › Modern map (Continents / Pangaea / Islands)** (saved in user preferences).
* Or launch from the command line with `--mapgen=classic|continents|pangaea|islands`.

---

## Beyond the original

On a 640 × 480 screen the game looks as the original does, pixel for pixel.
On a bigger one the map grows to fill it, and the strategic map, the control
panel, and the bottom bar move to the edges. A few things are new:

| To | Do |
|---|---|
| **Choose the AI & UI** | Game › Modern AI / Classic AI toggles the fair-information neural AI vs the 1993 AI; `--modern-ui` enables diplomatic Parley ledgers and the `F10` AI Brain Inspector |
| **Generate worlds** | Switch between classic 1993 `RANDOM.DAT` maps and modern balanced random worlds (`Continents`, `Pangaea`, `Islands`) in the Random Map panel, the `Game` menu, or via `--mapgen` |
| **Scale the interface** | View › Interface 1x, 2x… picks a whole-number scale so the pixel art stays sharp; the biggest that fits is the default |
| **Zoom & pan the map** | Mouse wheel, `PageUp` / `PageDown`, keypad `+` / `-`, or View › Map 1x, 2x…; drag the map with the pointer for smooth pixel panning |
| **Switch display & CRT** | View › Full screen / Window, View › 4:3 aspect lock, and optional OpenGL CRT shader (curvature, scanlines, aperture grille, and halation glow) |
| **Save quickly & auto-save** | `F5` quick-saves, `F9` quick-loads, and `F8` loads the auto-save (game automatically saves at the start of each human turn; both `[Auto Save]` and `[Quick Save]` appear at the top of Game › Load game) |
| **Change the music** | Game › AdLib music (live OPL2 & AIL synth), MT-32 music, or SC-55 music (live TinySoundFont synthesis) |

The View menu's choices and the music settings are remembered for next time.
