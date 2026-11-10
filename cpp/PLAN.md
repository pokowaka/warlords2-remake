# Warlords II — C++/SDL2 Engine

The core Warlords II engine in modern C++23 on SDL2. The C++ codebase serves as
the single source of truth for all game rules, AI strategies, persistence, and
rendering logic.

## Shape

```
cpp/
  BUILD.bazel
  src/
    platform/        the layer over SDL2: window and display, renderer, input,
                     the audio device and mixer, files, timer
    warlords/        the rules core, headless (no SDL): links into the tests
    warlords/ai/     the computer player (Classic and Modern ML policy AI)
    ui/              the dialogs, one file per original dialog
    main.cpp         the CLI entry point and application loop
  test/              headless gtests, golden regressions (trace + worldgen), and UI tests
```

### Decisions

- **Assets:** none are converted or checked in. The game reads the original
  formats from `original/` at runtime: `.PCK`/`.FNT` (planar
  LZ77), `.8SN`, `.XMI`, and the text and record files. The MT-32 and SC-55
  recordings are streamed from `pre-rendered-sound/*.ogg` where they lie,
  through libvorbisfile (optional at build time). The AdLib music is
  synthesized live by the built-in OPL2 and AIL driver.
- **SDL layer:** `gfx` provides clean immediate-mode 2D drawing primitives:
  setColor, rectangle, draw image or quad, scissor, push/pop/translate/scale.
  It draws through SDL_Renderer with nearest-neighbour textures. An image
  keeps its colour indices, so palettes and colour remaps work dynamically.
- **The computer's turn:** AI turns run cooperatively via `AiTurnRunner`.
  On desktop, background execution runs on a worker thread with explicit handoffs
  to the main thread for visual hooks (walk, fight, spoils). Under WebAssembly,
  a cooperative single-threaded event queue allows the UI to pump frames and
  show animations without requiring browser worker threads.
- **Objects:** armies, cities, sides, sites and items are structs. A game owns
  every army it ever made, so pointers held by quests, groups and selections
  stay valid. Lua's nil becomes `std::optional` or an explicit sentinel.
- **Determinism:** the RNG, deterministic unstable sorting and floating-point
  division preserve exact replayability across platforms. Golden regression tests
  (`bazel test //cpp:golden_test`) verify turn-by-turn simulation matches across builds.
- **Files:** saves are JSON in the C++ port's own format (not the Lua's).
  Saves, prefs and shortcuts sit in SDL's per-user folder for the game
  (SDL_GetPrefPath), as `w2-*.json`.

## Steps

- [x] 1. Scaffold: CMake, SDL2 window, platform layer (files, gfx, input, timer)
- [x] 2. Core plumbing: util (fmt, luaSort, json), rng, bytes, pal, pck, armytype, scn, rules
- [x] 3. Rules: move, combat, game, hero, site, quest, diplomacy, slots, history, report, save
- [x] 4. Computer player: aicard, ai, ai/* (with the AI thread handoff)
- [x] 5. Tests: headless unit tests, rules suite, and golden trace regressions
- [x] 6. Screen plumbing: font, uidata, layout, stonetile, menu, screen, display
- [x] 7. Front end: main — map, strategic map, bars, banner, hero offer, assault, input
- [x] 8. Dialogs: kit, then every ui/*
- [x] 9. Start screens, saves and prefs
- [x] 10. Sound: samples, the advisor, OPL2 music, recordings
- [x] 11. Check (offscreen screenshots and a scripted run), README, polish

## Notes along the way

- `cpp/test/compare.sh` plays the same all-computer games in LuaJIT and in
  C++ and compares them after every side's turn: gold, armies, the dice's
  state, a hash of every army and every city's owner. Every scenario, the
  hidden map and a save round trip come out identical.
- The AI's city neighbour table is kept per map, holding city indices, and
  each game resolves them to its own cities. The Lua and the JS port first
  kept the first game's city objects, so a second game of the map, or a
  loaded save, read the neighbours' owners from the old game. That is fixed
  in both, and the save round trip compares with the Lua as it is.
- The save keeps no per-turn marks (done, offered), as the Lua's does not.
- `warlords2 --script FILE` drives the game for checking: keys, clicks,
  frame waits, screenshots (.bmp) and a line of state. With
  `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software` it runs without a
  window; fifteen skipped rounds and a watched one have been run that way.
- The FM music is checked sample for sample against the Lua: `w2fm` and
  `test/fmrender.lua` render a song through the AIL driver and the OPL2 and
  print a checksum per second; SSTARTUP, SINT0, SINT7 and SINT12 match.
- Sound is mixed in one SDL callback: the DIGPAK queue, the FM driver at the
  chip's rate and the recordings, each resampled to the device's rate.
  `SDL_AUDIODRIVER=disk` writes the mix to a file for checking.
