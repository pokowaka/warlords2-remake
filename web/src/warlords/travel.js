// Travel cost over a map: how many movement points a land army needs to get
// from one place to every tile, under the game's own rules (docs/rules.md >
// Movement). The random map generator's fair capital placement uses it
// (warlords/randommap). A port of cpp/src/warlords/travel.cpp.
//
// The model is the walker's, slightly simplified: a tile costs its terrain's
// movement cost, any tile with a road on it costs 1 (so a road over the
// mountains is a pass), mountains without a road are shut, and an army
// changes between land and sea only at a crossing (a bridge, a marked tile,
// a port city), which ends its move -- counted here as a whole turn.

import { COST, DIRS, MOUNTAINS, SHORE, WATER } from "./move.js";
import { Heap } from "./heap.js";

/** Movement points in a turn: a Light Infantry's. */
export const TURN = 12;
/** Going to sea or ashore ends the move: a turn's worth of points. */
export const SEA_SWITCH = TURN;
/** A tile that cannot be reached (C++'s INT_MAX). */
export const UNREACHED = 2147483647;

/** Movement points from the nearest of `sources` (tile indices) to every
 *  tile, UNREACHED where it cannot be reached. The ground is { w, h,
 *  terrain, road, crossing }: terrain type ids, and 1 where a road runs and
 *  where land and sea may meet. */
export function costs(g, sources) {
  const n = g.w * g.h;
  const dist = new Array(n).fill(UNREACHED);
  const sea = (i) => g.terrain[i] === WATER || g.terrain[i] === SHORE;
  const open = (i) => g.terrain[i] !== MOUNTAINS || g.road[i];
  const step = (i) => {
    if (g.road[i]) return 1;
    const c = COST[g.terrain[i]];
    return c > 0 ? c : 1;
  };
  const q = new Heap();
  for (const s of sources) {
    if (s < 0 || s >= n || dist[s] === 0) continue;
    dist[s] = 0;
    q.push(0, s);
  }
  while (!q.empty()) {
    q.pop();
    const d = q.cost, i = q.tile;
    if (d !== dist[i]) continue;
    const x = i % g.w, y = Math.floor(i / g.w);
    for (let k = 0; k < 8; k++) {
      const nx = x + DIRS[k][0], ny = y + DIRS[k][1];
      if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) continue;
      const j = ny * g.w + nx;
      if (!open(j)) continue;
      let c = d + step(j);
      if (sea(i) !== sea(j)) {
        if (!g.crossing[i] && !g.crossing[j]) continue;
        c += SEA_SWITCH;
      }
      if (c < dist[j]) {
        dist[j] = c;
        q.push(c, j);
      }
    }
  }
  return dist;
}
