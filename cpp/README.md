# Warlords II — in modern C++ on SDL2

The standalone engine in modern C++23, drawing and playing sound through
SDL2. The rules core, the computer player, the FM music, and the SoundFont
synthesizer read the original game files directly and define how the game behaves.

Nothing is converted and no assets are checked in. The game reads the
original files from `original/` as they are, and synthesizes AdLib FM, Roland MT-32,
and Roland Sound Canvas SC-55 music in real-time.

## Building

You need Bazel (best through bazelisk, which picks the version in
`.bazelversion`) and a C++23 compiler: Xcode's on macOS, gcc or clang on
Linux. Bazel fetches SDL2 and GoogleTest itself, from the Bazel
Central Registry (`MODULE.bazel`).

```sh
brew install bazelisk                     # macOS
bazel build //cpp/...                     # the game, the tools and the tests
bazel test //cpp/...                      # the rules suite, golden regressions, and UI tests
sh tools/check.sh                         # all quality gates and clang-tidy
sh tools/package.sh                       # a zip to share: the game and its files
```

Bazel puts the programs in `bazel-bin/cpp/`. `bazel run //cpp:play` starts
the game in the repository root. For clangd and clang-tidy,
`python3 tools/compile_commands.py` writes `compile_commands.json`, and
`sh tools/tidy.sh` runs clang-tidy with `cpp/.clang-tidy`.

## Running

Run from the repository root, so that `original/` is found:

```sh
bazel-bin/cpp/warlords2                          # the start screens
bazel-bin/cpp/warlords2 --scenario ISLADIA       # straight into a scenario
bazel-bin/cpp/warlords2 --scenario ERYTHEA --seed 7 --window
bazel-bin/cpp/warlords2 --scenario RANDOM --seed 7   # a random world, made from that seed
bazel-bin/cpp/warlords2 --data /path/to/WARLORD2 # the game's files elsewhere
```

Random Map works as the original's does: the start menu's settings, then
Begin makes a new world (`src/warlords/randommap.cpp`,
`docs/re/random_map.md`). The world is kept in memory, not written over
`RANDOM/`, which holds the last world the original made; a random game's
save carries its map.

The controls:

- Saves, the quick save (F5 and F9), preferences and the button shortcuts are
  `w2-*.json` files in SDL's per-user folder for the game. On macOS that is
  `~/Library/Application Support/Warlords II Remake/`.
- The sound switches live in `original/DATA/OPTIONS.SND`, as they do for the
  original.
- The AdLib music is synthesized live by the built-in OPL2 emulator and AIL driver.
  Roland MT-32 and Roland Sound Canvas SC-55 music are synthesized in real-time
  via TinySoundFont (`original/SOUND/warlords2.sf2`).
- A game controller plays as well: the right stick is the pointer, R2 or A
  and L2 or X are the left and right buttons, the left stick slides the map
  (the whole table is under Controller below, made for the Steam Deck). Text
  input is on only while a name field is typed into, so that on the Deck it
  brings up Steam's on-screen keyboard.

## Layout

```
src/util/         printf-style formatting, deterministic sorting, JSON, filesystem helpers
src/warlords/     the rules core, domain models, XMI, OPL2 synth, and AIL driver (no SDL)
src/warlords/ai/  Classic 1993 expert AI and Modern neural policy AI
src/platform/     the slim layer over SDL2: window, renderer, input, audio mixer, threads
src/front/        fonts, images, screen layout, menus, prefs, and application lifecycle
src/ui/           one file per dialog
src/main.cpp      the CLI entry point and application initialization
test/             rules suite, golden regression traces, and UI tests
```

### Architecture Highlights

- `w2::NONE` (-1) represents empty sentinel IDs and indices in the domain model.
- Strong entity identifiers (`SideId`, `CityId`, `ArmyId`, `ArmyTypeId`) prevent mixing index spaces.
- A game owns every army it ever created (`ArmyRegistry`), preserving pointer stability across selections and quests.
- The computer's turn runs cooperatively via `AiTurnRunner`, supporting background threads on desktop and synchronous queues in WebAssembly.
- Dialogs are `kit::Modal` instances managed by `ModalStack`. Handler callbacks operate over `UiContext&`.

## Tests

From the repository root, after building:

```sh
bazel test //cpp:headless_tests         # 11 unit & subsystem test suites
bazel test //cpp:golden_test            # 15 golden trace games and world generations
bazel test //cpp:ui_test                # UI modal screens & dialog rendering verification
```

The game can also be driven by a script, with no window, for checking the
screens:

```sh
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy \
  bazel-bin/cpp/warlords2 --window --scenario ERYTHEA --seed 1 --script steps.txt
```

A script has one command per line: `wait N` (frames), `key NAME`,
`hold NAME`, `release NAME`, `click X Y [BUTTON]`, `move X Y`, `text STRING`,
`pad NAME`, `padhold NAME`, `padrelease NAME`, `stick LX LY RX RY`,
`select TX TY`, `attack TX TY`, `near`, `shot FILE.bmp`, `glass FILE.bmp`,
`eval` and `quit`. `shot` writes the frame as drawn; `glass` writes the
window as shown, through the CRT screen's tube when that is on.
Keys go by SDL key names, a controller's buttons by SDL's (`a`,
`dpup`, `leftshoulder`, and `lefttrigger`, `righttrigger`); the sticks are
held where `stick` puts them, -1 to 1, until it sets them all to 0. Points
are in the 640x480 interface's pixels, and tiles in map coordinates. With `SDL_AUDIODRIVER=disk` the mix is
written to a file.

## Controller

The touchscreen is the mouse (a tap is a left click), and the game reads the
controller itself, through Steam Input's gamepad: leave the controller layout
on Steam's **Gamepad** default.

| Deck | does |
|---|---|
| right trackpad or right stick | move the pointer: the further over, the faster |
| left stick | slide the map |
| R2 or A | left click: pick up, move, press buttons; hold to drag the map |
| L2 or X | right click, held: what is on a tile, an army's numbers, a button's help |
| B / Y | Esc / Enter: quit army / next army, Cancel / OK |
| L1 / R1 | zoom the map out / in, about the pointer |
| D-pad | step the stack one tile, as 8 2 4 6 do |
| hold View (⧉) | Shift, for a Shift-click (the Military Advisor's verdict) |
| hold Menu (☰) | Alt, for an Alt-click (plan a route) with R2 |
| L3 / R3 | group the whole stack / centre on it |

The menus, End Turn and everything else are on the screen, under the
pointer. Steam's Gamepad layout hands the game the right trackpad as the
right stick, so the thumb on the trackpad steers the pointer as the stick
does, and R2 clicks. For the trackpad as a true mouse instead, pick the
