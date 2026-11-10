# Warlords II — remade

A remake of SSG's *Warlords II* (1993) that reads the original game's
own data files and plays it: the maps, armies, art, music and computer
players are the original's, decoded rather than redrawn.

Play it in the browser: <https://jstrand.github.io/warlords2-remake/>

Steve Fawkner, who holds the rights to *Warlords* based video games, has given me permission for this remake to be published.

## What's here

| folder | what it is |
|---|---|
| [`cpp/`](cpp/README.md) | the core engine in modern C++ on SDL2 (source of truth) |
| [`web/`](web/README.md) | the browser WebAssembly release built from C++ via Emscripten |
| [`docs/`](docs/) | the file formats, the rules, and the reverse engineering of `WARLORD2.EXE` |
| `tools/` | decoders and exporters for the data files, the Ghidra and DOSBox helpers |
| `original/` | the original game's files, read as they are at runtime |
| `pre-rendered-sound/` | the music, recorded in advance |

## Quick start

```sh
bazel run //cpp:play                                                # desktop C++ on SDL2
./tools/build_web.sh && python3 -m http.server -d build/web 8765    # WebAssembly browser release (http://localhost:8765/)
```

Run from the repository root, so that `original/` is found. Each version's
README has its controls, options and tests.

## Beyond the original

On a 640 × 480 screen the game looks as the original does, pixel for pixel.
On a bigger one the map grows to fill it, and the strategic map, the control
panel and the bottom bar move to the edges. A few things are new:

| to | do |
|---|---|
| scale the interface | View › Interface 1x, 2x… picks a whole-number scale, so the art stays sharp; the biggest that fits is the default |
| zoom the map | the mouse wheel, `PageUp` / `PageDown` or keypad `+` / `-`, or View › Map 1x, 2x… |
| move the view | drag the map with the pointer; it slides smoothly, not a tile at a time |
| switch the screen | View › Full screen / Window; View › 4:3 shows only the original's 640 × 480, with black round it, and Fit available space fills the screen or window again |
| save quickly | `F5` saves, `F9` loads |
| change the music | Game › AdLib music, MT-32 music or SC-55 music; the MT-32 is the default, and the two Roland sets play from recordings in `pre-rendered-sound/` |

The View menu's choices and the music are remembered for next time.
