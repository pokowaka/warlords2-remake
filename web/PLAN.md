# Warlords II — Web Release Plan (WebAssembly)

The legacy handwritten JavaScript port has been superseded by compiling the modern C++23 engine directly to WebAssembly using Emscripten.

## Architecture

- **Single Source of Truth**: All game simulation, AI, file decoders, graphics primitives, and sound generation live in `cpp/src/`.
- **Packaging**: Emscripten preloads `original/` into `/original` inside WebAssembly MEMFS.
- **Audio**: AdLib OPL2 FM synth (`AilFm`) synthesizes songs directly in real time through Web Audio.
- **Saves & Settings**: IndexedDB (`IDBFS`) mapped to `/libsdl` persists games across browser restarts.
- **Build**: Built via `./tools/build_web.sh` and deployed to GitHub Pages via `.github/workflows/pages.yml`.
