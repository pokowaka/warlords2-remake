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
| `capital-gap` | hard | two capitals are less than 1.5 turns of travel apart. The designers never go under 2.5 (Erythea), and the threshold is about 60% of that |
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
| `capitalGapMinTurns`*, `capitalGapMedianTurns`* | turns of travel to the nearest rival capital (12 movement points per turn, a Light Infantry's; going to sea or coming ashore costs a whole turn) |
| `expansionMinShare`*, `expansionMaxShare`* | the neutral cities a side reaches before anyone else (a Voronoi split by travel cost), as a share of a fair split, for the worst-off and best-off side |
| `expansionGini`*, `incomeGini`* | the inequality of that split across sides, by city count and by income (0 is perfectly even) |
| `near4MinShare`* | neutral cities within 4 turns of the worst-off capital, as a share of a fair split |
| `remoteCityFrac`* | share of neutral cities more than 5 turns from every capital: wild land that one side will pick up unchallenged |
| `hinterlandMaxShare`* | the most uncontested cities any side gets (the next capital is twice as far away and at least 2 turns further), as a share of a fair split |
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

## Travel cost

The fairness checks measure distance by travel, not straight lines
(`cpp/src/warlords/travel.{hpp,cpp}`). It is a Dijkstra search over the
movement rules: each tile costs its terrain's cost, a road tile costs 1, and
mountains are shut unless a road crosses them. Changing between land and sea
is only possible at a crossing (a bridge, a bit-15 tile or a port city) and
costs a whole turn. The generator's fair capital placement uses the same model.

## Fair capitals (`randommap::Options::fairCapitals`, generator `fair`)

This is not part of the original game. The world is generated exactly as by the
classic generator; only the choice of the eight capitals differs. The search
picks capitals from the cities using travel cost between every pair of cities.
It starts from 12 random sets and swaps one capital at a time, keeping any swap
that lowers this penalty:

```
3 × (cities more than 5 turns from every capital)
+ (the gap between the largest and smallest first-reach share)
+ (the mean turns from a city to its nearest capital)
+ 10 × (how far the closest two capitals fall short of 3 turns)
```

With the option off (the default), the world is the original's, byte for byte.
Over 200 seeds:

| | classic | fair | hand-made |
|---|---|---|---|
| `capital-gap` failures | 19% of maps | **0%** | 0% |
| closest capitals (turns, median) | 2.4 | **3.6** | 2.5–5.5 |
| `expansionGini` (median) | 0.28 | **0.11** | 0.06–0.24 |
| `incomeGini` (median) | 0.29 | **0.12** | 0.06–0.24 |
| `remoteCityFrac` (median) | 0.12 | **0.00** | 0.00–0.10 |
| `hinterlandMaxShare` (median) | 0.78 | 0.89 | 0.43–0.78 |
| penalty (median) | 17.8 | **11.4** | 0 |

The one trade-off: spreading the capitals out leaves each side more
uncontested hinterland. It costs about 0.1 s per map.

## Human votes

`tools/mapvote.py` checks the scores against what a player prefers. It draws
pairs of maps side by side (A | B, using the game's own tiles, with roads,
cities, capitals and sites drawn bold). The voter picks A, B or `=`, and the
votes go to `tools/mapvotes.jsonl`.

```bash
cpp/build/w2mapeval --seeds 1-200 --json /tmp/mv/eval.json --export /tmp/mv/maps
python3 tools/mapvote.py ballot --eval /tmp/mv/eval.json --maps /tmp/mv/maps --out /tmp/mv/b1 --batch 1
python3 tools/mapvote.py record --ballot /tmp/mv/b1/ballot-1.json 1A 2B 3= --note 2 "roads make sense"
python3 tools/mapvote.py agree --eval /tmp/mv/eval.json
```

A ballot mixes three kinds of pair:

- **clear:** a low penalty against a high one, to test that the score points the right way;
- **close:** similar penalties but different problems, to learn which problems matter more;
- **rules:** a map that breaks a hard rule but otherwise scores well, against one that passes.

Each pair also gets a `-issues.png` with the problems ringed. It is shown only
after the vote, so it does not sway it. `agree` reports how often the lower
penalty won, and for each metric how often the map closer to the reference
won. That shows which metrics deserve more weight.

### What the votes have shown

Batch 1 (12 pairs from the classic generator): the evaluator agreed on all 5
clear-cut pairs. The voter's notes pointed at fairness: capitals right next to
each other, and a swathe of cities that would become one side's hinterland.
Of the measures added after that batch, `remoteCityFrac` agrees with 10 of the
12 votes, and `capitalGapMinTurns` and `expansionGini` with about 7.

## Next

- A problem overlay image (stray roads, crowded cities, unreachable places)
  for checking by eye.
- Tier 4: AI self-play seat bias, on the many-core machine, through the
  `w2arena` evaluation harness.
