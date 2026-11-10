# Warlords II — in the Browser (WebAssembly)

The modern C++23 remake of Warlords II compiled directly to WebAssembly (`warlords2.wasm` and `warlords2.js`) using Emscripten. The C++ codebase serves as the single source of truth for both desktop and web releases.

All game subsystems run natively inside the browser via WebAssembly:
- Pure C++23 game model, rules, combat, and scenario loaders (`.SCN`, `.PCK`, `.XMI`).
- Both classic 1993 AI and modern neural policy ML computer players.
- Real-time live synthesizers: AdLib / OPL2 FM synthesis (`AilFm`) and Roland MT-32 / Roland Sound Canvas SC-55 SoundFont synthesis (`SoundFontSynth`), eliminating 160+ MB of pre-rendered songs.
- SDL2 immediate-mode 2D graphics rendering to HTML5 Canvas via WebGL.
- Persistent saved games and user preferences via browser IndexedDB (`IDBFS`).

## Building

To build the web distribution into `build/web/`:

```sh
./tools/build_web.sh
```

Flags:
- `--clean`: Clean previous intermediate build files.
- `--debug`: Compile with `-g` assertions and symbols enabled.
- `--out DIR`: Custom output directory (default: `build/web`).

## Running Locally

Serve `build/web/` with any local HTTP server:

```sh
python3 -m http.server -d build/web 8765
# http://localhost:8765/                          start screen
# http://localhost:8765/?scenario=ISLADIA         directly launch a scenario
# http://localhost:8765/?scenario=ERYTHEA&seed=7  launch with fixed RNG seed
# http://localhost:8765/?scenario=RANDOM&seed=7   launch a random world
# http://localhost:8765/?spectate=1               AI-vs-AI spectator mode
```

URL parameters are forwarded directly into CLI flags in `cpp/src/main.cpp`.

## Deployment

Pushes to `main` automatically build and deploy `build/web/` to GitHub Pages via `.github/workflows/pages.yml`.

The static build package consists of:
- `index.html`: Web wrapper and canvas host.
- `warlords2.js`: Emscripten runtime loader (~190 KB).
- `warlords2.wasm`: Compiled C++ engine (~2.7 MB).
- `warlords2.data`: Preloaded original game assets (~4.5 MB).
- `_headers`: Caching headers for Netlify / Cloudflare.
