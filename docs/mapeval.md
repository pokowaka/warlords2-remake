# Map evaluator (`w2mapeval`)

A tool for scoring maps, so that map generators can be compared with numbers
instead of by eye. The library is `cpp/src/warlords/mapeval.{hpp,cpp}`, the
command is `cpp/test/mapeval.cpp`, and the tests are `testMapEval` in
`cpp/test/run.cpp`.

The evaluator only looks at the loaded `Map` (whatever `scn::load` or
`scn::loadFromFiles` produced). It never looks inside a generator. This means
every generator, and every hand-made `.SCN`, is judged the same way.

## Running it

```bash
cmake --build cpp/build --target w2mapeval
cpp/build/w2mapeval                                  # reference + classic generator, seeds 1-50
cpp/build/w2mapeval --seeds 1-200 --json out.json    # baseline, with every issue and metric
cpp/build/w2mapeval --seeds 7 --issues 50            # one map, its issues listed with tiles
cpp/build/w2mapeval --sliders 6,0,6,0 --seeds 1-100  # Water, Hills, Cities, Forest (7 = chance)
cpp/build/w2mapeval --generator none --scenarios TUTORIA --issues 20
```

Maps are generated in memory, one per seed, in parallel across all cores.
Nothing is written to `original/RANDOM`. On a laptop, 200 maps take about 0.2 s.

To add a generator, write a `randommap::Files f(const randommap::Options&)` and
register it in `generators()` in `cpp/test/mapeval.cpp`.

## The reference

The metrics are scored against the designers' own maps: Erythea, Hadesha,
Isladia, Dragon and Sorcery (`--reference` changes the list). Tutoria has only
6 cities, so it is left out. For each scored metric, the reference records the
range these maps span. A map scores **0 inside the range**. Outside it, the
penalty grows by one for each reference standard deviation of distance (or
each tenth of the mean, when the designers never varied the metric). A map's
penalty is the sum over all scored metrics.

## Rules

A rule is **hard** if none of the five hand-made maps ever breaks it. A map
that breaks a hard rule fails. Soft rules are only counted (and scored as a
rate), because the designers broke them now and then.

| rule | kind | fires when |
|---|---|---|
| `city-terrain` | hard | a city's 2×2 footprint is not all castle tiles |
| `city-gap` | hard | two footprints overlap or touch (fewer than 1 tile between them) |
| `city-name` | hard | two cities share a name |
| `dead-end` | hard | a road piece has an arm that leads to nothing: no road, bridge, city, site, signpost or crossing beyond it, and no place next to it |
| `road-to-nowhere` | hard | a road network (roads and bridges, 4-connected) touches no city, site, signpost or crossing |
| `spur-road` | soft | a road network touches exactly one place. Erythea and Hadesha each have one |
| `unreachable` | hard | a city or site cannot be reached from some capital, using the real movement rules: mountains are shut unless a road crosses them (a pass), and a stack can only change between land and sea at a crossing, i.e. a bridge, a bit-15 tile or a port city (`docs/rules.md` > Movement) |
| `signpost` | hard | a signpost is not on a signpost tile, or a "*N leagues dir*" sign names no city, points the wrong way, or gets the distance wrong (any footprint tile counts, within 4 leagues or 25%) |
| `port` | hard | a bit-15 crossing does not touch both land and sea |

## Metrics

Metrics marked * are scored against the reference. The others are counts, kept
for context.

| metric | meaning |
|---|---|
| `cityGapMin`*, `cityGapP5`*, `cityGapMedian`* | tiles between each city's footprint and its nearest neighbour's |
| `crowdedCityFrac`* | share of cities with another city within 2 tiles |
| `clarkEvansR`* | mean nearest-neighbour distance divided by the value random points on the same land would give: < 1 clustered, 1 random, > 1 evenly spread |
| `spurRoadsPerCity`* | soft spur roads per city |
| `roadShapeMismatchFrac`* | road pieces whose drawn arms disagree with their neighbours (broken autotiling) |
| `cityRoadCoverage`* | share of cities touching a road |
| `mainNetworkCityShare`* | share of cities on the largest road network |
| `roadTilesPerCity`* | road and bridge tiles per city |
| `roadDetourMedian`*, `roadDetourP95`* | for each pair of cities joined directly by road: the road's length divided by the Manhattan gap between their footprints |
| `landReachShare`* | share of cities a capital reaches by land alone (averaged over capitals) |
| `landFrac`*, `coastComplexity`* | land share; coast tiles ÷ √land |
| `biggestLandmassCityShare`* | share of cities on the largest landmass |
| `forestPatchMedian`*, `hillsPatchMedian`*, `mountainPatchMedian`* | median connected patch size per terrain type |
| `speckleFrac`* | share of forest/hills/mountain/marsh tiles that stand alone |
| `cities`, `roadTiles`, `roadNetworks`, `roadsToNowhere`, `spurRoads`, `deadEnds`, `capitals`, `inhabitedLandmasses`, `marshPatchMedian`, `signs`, `pointingSigns`, `crossings` | counts |

## Baseline: the classic (1993) generator

`cpp/build/w2mapeval --seeds 1-200`, default sliders (3,3,2,3):

- **8% of maps pass every hard rule.** The median penalty is 9.6 (p95 24.3); every hand-made map scores 0.
- **Roads to nowhere: 81% of maps, about 2 per map.** These are mostly a bridge over a river with a one-tile stub on each side and no road leading to it.
- **Duplicate city names: 49% of maps.** The syllable generator does not check for repeats.
- **Unreachable places: 12% of maps.** Ruins walled in by mountains, sites on islands with no port, and now and then a whole capital cut off (120 issues).
- **Cities are clumped:** Clark–Evans R is 1.06–1.29, while every hand-made map is 1.37–1.70, so all 200 maps fall outside the range. Half the maps have more crowded cities than any hand-made one.
- **Few cities on roads:** 24–47% of cities touch a road, against 74–100% on hand-made maps. Road networks are small (the largest links 12% of cities, median).
- **Too much land:** median 75% land, while the most land-heavy hand-made map has 66%.
- **Forests in giant blobs:** the median patch is 43 tiles, against 1–34 on hand-made maps.

What the classic generator gets right: legal city footprints, no overlapping
cities, consistent road autotiling, correct signposts and legal ports.

The classic generator stays byte-identical to the original, so its failures
are reported, not asserted in `w2test`. These numbers are the baseline a new
generator has to beat.

## Next

- A problem overlay image (stray roads, crowded cities, unreachable places)
  for checking by eye.
- Tier 3 fairness: territory by travel cost from each capital, the cities and
  income each side can reach first, early expansion room, and the Gini spread
  across sides.
- Tier 4: AI self-play seat bias, on the many-core machine, through the
  `w2arena` evaluation harness.
