# Scenario generator (`w2scengen`)

This generator is not part of the original game. It is a new world
generator, built alongside the 1993 one (`randommap`'s classic generator,
which stays byte-identical to the original). It writes each world as a
scenario folder the game can play like any other.

```bash
bazel build //cpp:w2scengen
bazel-bin/cpp/w2scengen --seed 7                       # writes original/GEN7/GEN7.* and GEN7.png
bazel-bin/cpp/w2scengen --seed 20 --count 6 --open     # six worlds, previews opened to look at
bazel-bin/cpp/w2scengen --seed 7 --shape pangaea --cities 100 --name BIGLAND
bazel-bin/cpp/warlords2 --window --scenario GEN7       # play the one you like
bazel-bin/cpp/w2scengen --preview ERYTHEA --open       # the same picture of any scenario
python3 tools/mapvote.py render cmp.png original/GEN7 original/ERYTHEA   # side by side
bazel-bin/cpp/w2mapeval --generator scenario --shape islands --seeds 1-100    # score it
```

| option | meaning |
|---|---|
| `--seed N` | the world's seed; the same seed always makes the same world |
| `--shape` | `continents` (default: several land masses split by straits), `pangaea` (one land mass), `islands` |
| `--cities N` | 8 to 100. The `.SCN` holds at most 100 cities and exactly 40 sites |
| `--land F` | the share of land, e.g. 0.55; each shape has its own default |
| `--name NAME` | folder and file name, at most 8 letters; the default is `GEN<seed>` |
| `--tries N` | how many seeds to try if a world breaks a hard rule (default 20) |
| `--count N` | make N worlds, from seeds N, N+1, and so on, each in its own `GEN<seed>` folder |
| `--open` | open the preview pictures in the system's image viewer |
| `--preview NAME` | only draw `NAME/NAME.png` for an existing scenario (any scenario, the originals too) |

Every world gets a preview picture, `NAME/NAME.png`, so you can look at it
before playing it. It shows the map in the game's own tiles at 5 pixels a
tile, with roads, cities, capitals (in their sides' colours) and ruins and
temples drawn over it (`cpp/src/warlords/preview.{hpp,cpp}`, a plain PNG with
no image library needed).

## In the game's scenario list

Every world `w2scengen` makes appears in the game's **New Scenario** list,
after the shipped scenarios, under its name (see Names below, or its `--name`). Choosing it shows its name, cities, ruins and players
on the crystal ball. Back on the start menu, its map stands where the
shipped scenarios show their artwork. **Begin** sets it up like any other
scenario.

`SCENARIO.DAT` is left as shipped. The list reads it first, then the folders
named in `DATA/SCENGEN.LST` (one a line; `w2scengen` keeps it), each holding
`NAME/NAME.SCN` together with `NAME/NAME.INF`
(`cpp/src/warlords/scenarios.{hpp,cpp}`). `NAME.INF` is a few `key=value` lines:

```
name=Saltmarch Realms
text=Eight warlords, one throne
cities=80
ruins=40
players=8
```

The start menu's picture is `NAME/PICS/SCENARIO.PCK`, as the shipped
scenarios have it: the map at 264 × 225 in the game's 16 colours, centred on
the sea (`preview::menuPicture`, written with `pck::encode`). To take a world
out of the list, delete its folder. The web game sees the scenarios bundled in
the virtual filesystem (`warlords2.data`).

## Deterministic cross-platform generation

The native C++ and WebAssembly browser builds make the same world from the same seed and
shape, byte for byte, as the classic random map generator does (`scengen`
keeps to arithmetic IEEE doubles give alike everywhere: no `pow`, `hypot` or
`lerp`, sorts without ties, queues ordered by cost then tile). Both list
generated worlds in New Scenario the same way. `bazel test //cpp:worldgen_test`
verifies world generation across seeds and presets.

## Names

Each world gets a name and a line of description drawn from its seed alone
(`scengen::title`): "Saltmarch Realms", "The Drowned Expanse", "Emberspire
Isles"; "Eight warlords, one throne", "Here be monsters (and loot)". The
nouns suit the shape (Kingdoms and Marches, Realm and Expanse, Isles and
Shoals). `--name` gives your own name instead.

## How a world is made

The code is in `cpp/src/warlords/scengen.{hpp,cpp}` (the planners) and the
`nextModern` phases in `cpp/src/warlords/randommap.cpp` (the file side). The
planners only decide where things go. Turning terrain into tiles reuses the
original's machinery: `RANDOM.DAT`'s tile tables, `toTiles` and `shapes`.

1. **Land.** Domain-warped fractal noise, with the sea closing in at the
   map's edges, and a sea level chosen to give the target land share. For
   `continents`, straits are cut along the zero lines of a slow noise. The
   coast is smoothed so the tile set can draw it. Specks of land are sunk
   and small lakes filled in.
2. **Relief.** Mountain crests follow *ridged* noise, so they form ranges
   rather than blobs, and are then widened onto the next-highest ground.
   Foothills and lumps of hills come next. Every pocket of ground the
   mountains wall off gets a pass: the cheapest way through to the rest of
   its land mass becomes hills.
3. **Rivers.** They start at the feet of the hills, well inland, and follow
   the cheapest path over a noise field to the sea or into another river, so
   they wind along its valleys. They are two tiles wide, which the tile set
   draws best.
4. **Growth.** Forest where a moisture field is high (wetter near water),
   and a few small marshes on the wettest low ground by the water.
5. **Cities.** Spread by *best-candidate* (blue-noise) sampling: each city is
   the best of a few random spots, the best being the one furthest from the
   cities already placed, with a bonus for spots near water. No clumps, no
   empty stretches. Cities only go on plain or forest.
6. **Sites.** The 40 ruins and temples are spread the same way, in the wild,
   only on ground an army can walk to from a city.
7. **Roads**, once the terrain has been drawn as tiles: a minimum spanning
   tree of the cities by road-building cost, plus a few links where the tree
   would send travellers a long way round. Each link is laid by A*, and roads
   already built are cheap, so new roads join them. A small pull towards the
   straight line makes roads run in staircases rather than long L shapes.
   Where a road crosses a straight stretch of two-tile river, a bridge is
   built. Road pieces come from their north, east, south and west neighbours,
   with an arm into the city a road ends at.
8. **Landings.** Every stretch of land with a city or a site gets a way to
   the sea: a port city, or else a crossing on its shore. Otherwise a river
   or a strait could cut it off from armies that cannot fly.
9. **Capitals** are the fair ones (`fairCapitals`, see `docs/mapeval.md`).
   City names never repeat. Signposts stand by the roads and point the way to
   the nearest city. The `.SCN` is marked as an ordinary scenario.

Each world is scored by the map evaluator as it is made. One that breaks a
hard rule is regenerated from the next seed. Over 100 seeds per shape, none
needed a second try.

## Ideas it borrows

- Noise-based land with domain warping and an edge falloff: the usual 4X
  map-script approach, and Red Blob Games' map generation articles.
- Mountains first, then water running downhill from them: Red Blob Games'
  mapgen4, which builds elevation and rivers from ridges and coasts.
- Blue-noise placement (Poisson disc, best-candidate) for even but not
  grid-like settlements.
- A minimum spanning tree plus extra links, routed by A* over terrain cost,
  for road networks.
- Fair start placement as a separate, checked step: Civilization map
  scripts' "start plot" normalisation.

## Status

The first version is for judging by eye. It passes every hard rule over 300
seeds (100 per shape). Its median penalty against the hand-made maps is 10–17
by shape, against 17.8 for the classic generator. The measures still outside
the designers' range point at what to tune next:

- cities are a little too evenly spread: the designers put some cities close
  together (`crowdedCityFrac`);
- roads end at cities rather than running past them, so there are many small
  road networks (`mainNetworkCityShare`);
- forest comes in small patches.
