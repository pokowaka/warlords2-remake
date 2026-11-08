// The scenario generator's world and road planners (not the original's). A
// port of cpp/src/warlords/scengen.cpp, line for line: the C++, this and
// the Lua make the same world from the same seed, byte for byte.
//
// randommap's classic generator is a port of the 1993 one, byte for byte.
// The scenario generator is a new design. It uses the same file machinery:
// RANDOM.DAT's tile tables turn terrain into tiles, and the same city, site
// and side records are written. What differs is the order and the methods:
//
//   1. land     domain-warped fractal noise, sea at the edges, a sea level
//               picked for a target land share, small islands and lakes
//               cleaned away (the noise approach of Red Blob Games' mapgen
//               articles and of many 4X map scripts);
//   2. relief   mountain ranges along ridged noise rather than blobs, hills
//               round them, every mountain-walled pocket given a pass;
//   3. rivers   from the feet of the mountains downhill to the sea, winding
//               by a smooth noise, joining where they meet;
//   4. growth   forest and marsh by a moisture field, wetter by the rivers;
//   5. cities   spread evenly by best-candidate (blue noise) sampling, so no
//               clumps and no empty stretches, preferring coasts and rivers;
//   6. sites    the ruins and temples spread the same way, in the wilds;
//   7. roads    after the tiles are drawn: a minimum spanning tree of the
//               cities by travel cost, plus a few extra links where the tree
//               makes a long detour, each laid by A* and merged into roads
//               already there, bridges where they cross a river.
//
// randommap (opts.scenario) calls makeWorld, draws the tiles, calls
// planRoads, then chooses fair capitals and writes the files.
//
// The same world in all three ports takes care: every floating expression
// is written in the C++'s order of operations; + - * / sqrt floor only (no
// pow, hypot or lerp); C++'s integer division on non-negative values is
// Math.floor; every sort has no ties; and the queues pop (cost, tile) pairs
// in their total order (warlords/heap).

import { Heap } from "./heap.js";
import { Rng } from "./rng.js";

// terrain type ids (warlords/move)
const WATER = 2, FOREST = 4, HILLS = 5, MOUNTAINS = 6, PLAIN = 7, MARSH = 8;
const CITY = 10, SITE = 11;

// the 8 neighbours, N first and clockwise (move.DIRS), and the 4
const DIRS = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const DX4 = [0, 1, 0, -1], DY4 = [-1, 0, 1, 0];

/** The world's shapes, as the command line and NAME.INF name them. */
export const SHAPES = ["continents", "pangaea", "islands"];

/** The defaults of cpp/src/warlords/scengen.hpp's Settings. */
export function settings(over = {}) {
  return {
    shape: "continents",
    land: -1,          // the land share wanted; < 0 picks the shape's own
    cities: 80,        // at most 100, as the .SCN holds no more
    sites: 40,         // the .SCN holds exactly 40
    mountains: 0.08, hills: 0.10, forest: 0.17,   // shares of the land
    rivers: 5,
    ...over,
  };
}

/** "continents", "pangaea" or "islands" as itself; null if it is none. */
export function parseShape(s) { return SHAPES.includes(s) ? s : null; }
export function shapeName(s) { return SHAPES.includes(s) ? s : "continents"; }

const uniform = (rng) => rng.below(65536) / 65536.0;
const mix = (a, b, t) => a + t * (b - a);
const dist = (dx, dy) => Math.sqrt(dx * dx + dy * dy);

/** Perlin's gradient noise, its permutation drawn from the dice. */
class Noise {
  constructor(rng) {
    const p = [];
    for (let i = 0; i < 256; i++) p.push(i);
    rng.shuffle(p);
    this.perm = new Int32Array(512);
    for (let i = 0; i < 512; i++) this.perm[i] = p[i & 255];
  }

  /** About -1..1. */
  at(x, y) {
    const perm = this.perm;
    const xi = Math.floor(x), yi = Math.floor(y);
    const xf = x - xi, yf = y - yi;
    const X = xi & 255, Y = yi & 255;
    const fade = (t) => t * t * t * (t * (t * 6 - 15) + 10);
    const grad = (h, dx, dy) => {
      switch (h & 7) {
        case 0: return dx + dy;
        case 1: return dx - dy;
        case 2: return -dx + dy;
        case 3: return -dx - dy;
        case 4: return dx;
        case 5: return -dx;
        case 6: return dy;
        default: return -dy;
      }
    };
    const u = fade(xf), v = fade(yf);
    const aa = perm[perm[X] + Y], ab = perm[perm[X] + Y + 1];
    const ba = perm[perm[X + 1] + Y], bb = perm[perm[X + 1] + Y + 1];
    const x1 = mix(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
    const x2 = mix(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
    return mix(x1, x2, v);
  }

  /** Fractal Brownian motion: octaves of the noise, each twice as fine. */
  fbm(x, y, octaves = 5) {
    let sum = 0, amp = 1, norm = 0;
    for (let o = 0; o < octaves; o++) {
      sum += amp * this.at(x, y);
      norm += amp;
      amp *= 0.5;
      x *= 2.03;
      y *= 2.03;
    }
    return sum / norm;
  }
}

/** The value at quantile q (0..1) of the listed values: the k-th smallest,
 *  k = floor(q * (n - 1)) (C++'s nth_element). A q below 0 takes the
 *  smallest, as the C++ build's conversion to size_t does. */
function cut(v, q) {
  if (v.length === 0) return 0;
  const k = Math.min(v.length - 1, Math.max(0, Math.floor(q * (v.length - 1))));
  const sorted = Float64Array.from(v).sort();
  return sorted[k];
}

class Grid {
  constructor(w, h) { this.w = w; this.h = h; }
  in(x, y) { return x >= 0 && y >= 0 && x < this.w && y < this.h; }

  /** The 8-connected (or 4-connected) parts of the tiles that pass `test`. */
  parts(test, eight = true) {
    const w = this.w, n = w * this.h;
    const seen = new Uint8Array(n);
    const out = [];
    for (let s = 0; s < n; s++) {
      if (seen[s] || !test(s)) continue;
      const part = [s];
      seen[s] = 1;
      for (let k = 0; k < part.length; k++) {
        const i = part[k], x = i % w, y = Math.floor(i / w);
        for (let d = 0; d < (eight ? 8 : 4); d++) {
          const nx = x + (eight ? DIRS[d][0] : DX4[d]), ny = y + (eight ? DIRS[d][1] : DY4[d]);
          if (!this.in(nx, ny)) continue;
          const j = ny * w + nx;
          if (!seen[j] && test(j)) { seen[j] = 1; part.push(j); }
        }
      }
      out.push(part);
    }
    return out;
  }

  /** Steps (4-connected) from the nearest tile passing `from`. */
  distance(from) {
    const w = this.w, n = w * this.h;
    const d = new Int32Array(n).fill(-1);
    const q = [];
    for (let i = 0; i < n; i++) if (from(i)) { d[i] = 0; q.push(i); }
    for (let head = 0; head < q.length; head++) {
      const i = q[head], x = i % w, y = Math.floor(i / w);
      for (let k = 0; k < 4; k++) {
        const nx = x + DX4[k], ny = y + DY4[k];
        if (!this.in(nx, ny)) continue;
        const j = ny * w + nx;
        if (d[j] < 0) { d[j] = d[i] + 1; q.push(j); }
      }
    }
    return d;
  }
}

/** A double-ended queue of ints (C++'s std::deque, for the 0-1 search). */
class Deque {
  constructor() {
    this.buf = new Int32Array(1024);
    this.head = 0;
    this.n = 0;
  }
  get size() { return this.n; }
  grow() {
    if (this.n < this.buf.length) return;
    const bigger = new Int32Array(this.buf.length * 2);
    for (let k = 0; k < this.n; k++) bigger[k] = this.buf[(this.head + k) % this.buf.length];
    this.buf = bigger;
    this.head = 0;
  }
  pushBack(v) {
    this.grow();
    this.buf[(this.head + this.n) % this.buf.length] = v;
    this.n++;
  }
  pushFront(v) {
    this.grow();
    this.head = (this.head - 1 + this.buf.length) % this.buf.length;
    this.buf[this.head] = v;
    this.n++;
  }
  popFront() {
    const v = this.buf[this.head];
    this.head = (this.head + 1) % this.buf.length;
    this.n--;
    return v;
  }
}

function defaultLand(shape) {
  if (shape === "pangaea") return 0.60;
  if (shape === "islands") return 0.42;
  return 0.55;
}

// ------------------------------------------------------------------ land

/** Step 1: where the land is (true) and its height (0..1 inland). */
function land(s, rng, g, height) {
  const w = g.w, h = g.h, n = w * h;
  const base = new Noise(rng), warp1 = new Noise(rng), warp2 = new Noise(rng), channel = new Noise(rng);
  const f = s.shape === "islands" ? 1.0 / 26 : 1.0 / 42;
  const e = new Float64Array(n);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const px = x * f, py = y * f;
      // domain warping: the noise read where another noise points
      const qx = warp1.fbm(px + 5.2, py + 1.3, 4), qy = warp2.fbm(px + 1.7, py + 9.2, 4);
      let v = base.fbm(px + 1.6 * qx, py + 1.6 * qy, 5);
      // the sea closes in at the map's edges
      let edge = Math.min(x, w - 1 - x, y, h - 1 - y) / 14.0;
      edge = Math.min(1.0, edge);
      v -= (1 - edge) * (1 - edge) * 0.9;
      if (s.shape === "continents") {
        // straits along the zero lines of a slow noise split the land
        const c = Math.abs(channel.fbm(x / 70.0 + 3.1, y / 70.0 + 7.7, 3));
        if (c < 0.09) v -= 0.45 * (0.09 - c) / 0.09;
      } else if (s.shape === "pangaea") {
        const dx = (x - w / 2.0) / (w / 2.0), dy = (y - h / 2.0) / (h / 2.0);
        v += 0.45 * (1 - Math.min(1.0, Math.sqrt(dx * dx + dy * dy)));
      }
      e[y * w + x] = v;
    }
  }
  const share = s.land >= 0 ? s.land : defaultLand(s.shape);
  const sea = cut(e, 1 - share);
  let isLand = new Uint8Array(n);
  for (let i = 0; i < n; i++) isLand[i] = e[i] > sea ? 1 : 0;
  // smooth the coast so the tile set can draw it: a tile goes with most of
  // its neighbours
  for (let pass = 0; pass < 2; pass++) {
    const next = isLand.slice();
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        let k = 0;
        for (let d = 0; d < 8; d++) {
          const nx = x + DIRS[d][0], ny = y + DIRS[d][1];
          if (g.in(nx, ny) && isLand[ny * w + nx]) k++;
        }
        if (k >= 5) next[y * w + x] = 1;
        else if (k <= 3) next[y * w + x] = 0;
      }
    }
    isLand = next;
  }
  // no land on the map's outermost two rows
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) isLand[y * w + x] = 0;
    }
  }
  // specks of land go under; small lakes fill in
  const minIsland = s.shape === "islands" ? 40 : 70;
  for (const part of g.parts((i) => isLand[i])) {
    if (part.length < minIsland) for (const i of part) isLand[i] = 0;
  }
  for (const part of g.parts((i) => !isLand[i], false)) {
    if (part.length < 30) for (const i of part) isLand[i] = 1;
  }
  // the height inland: distance from the coast, roughened by the noise
  const coast = g.distance((i) => !isLand[i]);
  height.fill(0);
  for (let i = 0; i < n; i++) {
    if (isLand[i]) {
      height[i] = 0.65 * Math.min(1.0, coast[i] / 14.0) + 0.35 * (e[i] - sea) / Math.max(1e-6, 1 - sea);
    }
  }
  return isLand;
}

// ------------------------------------------------------------------ relief

/** Step 2: ranges along ridged noise, hills round them, and a pass into
 *  every pocket of ground the mountains would wall off. */
function relief(s, rng, g, t, height) {
  const w = g.w, n = w * g.h;
  const ridges = new Noise(rng), lumps = new Noise(rng);
  const coast = g.distance((i) => t[i] === WATER);
  const score = new Float64Array(n).fill(-1);
  const eligible = [];
  for (let i = 0; i < n; i++) {
    if (t[i] === WATER) continue;
    const x = i % w, y = Math.floor(i / w);
    const r = 1 - Math.abs(ridges.fbm(x / 30.0, y / 30.0, 4));   // ridged: high along lines
    score[i] = r * r + 0.35 * height[i];
    if (coast[i] >= 2) eligible.push(score[i]);
  }
  let landTiles = 0;
  for (let i = 0; i < n; i++) if (t[i] !== WATER) landTiles++;
  // the crests first, at a third of the share, then the ranges widened
  // round them onto the next-highest ground, so they are thick enough to
  // draw as ranges rather than lines
  const mShare = Math.min(0.9, s.mountains * landTiles / Math.max(1, eligible.length));
  const crest = cut(eligible, 1 - mShare / 3), flank = cut(eligible, 1 - mShare * 1.6);
  for (let i = 0; i < n; i++) if (score[i] >= crest && coast[i] >= 2) t[i] = MOUNTAINS;
  for (let pass = 0; pass < 2; pass++) {
    const next = t.slice();
    for (let i = 0; i < n; i++) {
      if (t[i] === WATER || t[i] === MOUNTAINS || coast[i] < 2 || score[i] < flank) continue;
      const x = i % w, y = Math.floor(i / w);
      for (let d = 0; d < 8; d++) {
        const nx = x + DIRS[d][0], ny = y + DIRS[d][1];
        if (g.in(nx, ny) && t[ny * w + nx] === MOUNTAINS) { next[i] = MOUNTAINS; break; }
      }
    }
    t.set(next);
  }
  // hills: the foothills and lumps of their own
  const hscore = new Float64Array(n).fill(-1);
  const hs = [];
  const nearMount = g.distance((i) => t[i] === MOUNTAINS);
  for (let i = 0; i < n; i++) {
    if (t[i] !== PLAIN) continue;
    const x = i % w, y = Math.floor(i / w);
    const l = lumps.fbm(x / 13.0 + 11.3, y / 13.0 + 2.9, 3);
    const foot = nearMount[i] >= 0 && nearMount[i] <= 2 ? 0.6 - 0.2 * nearMount[i] : 0;
    hscore[i] = l + foot + 0.2 * score[i];
    hs.push(hscore[i]);
  }
  const hCut = cut(hs, 1 - Math.min(0.9, s.hills * landTiles / Math.max(1, hs.length)));
  for (let i = 0; i < n; i++) if (t[i] === PLAIN && hscore[i] >= hCut) t[i] = HILLS;
  // passes: on each landmass, every pocket of open ground the mountains cut
  // off gets the cheapest way through them to the landmass's largest one
  for (const mass of g.parts((i) => t[i] !== WATER)) {
    const onMass = new Uint8Array(n);
    for (const i of mass) onMass[i] = 1;
    const pockets = g.parts((i) => onMass[i] && t[i] !== MOUNTAINS);
    if (pockets.length < 2) continue;
    pockets.sort((a, b) => (a.length !== b.length ? b.length - a.length : a[0] - b[0]));
    // 0-1 search from the largest: a mountain tile costs 1, open ground 0
    const cost = new Int32Array(n).fill(1 << 30), from = new Int32Array(n).fill(-1);
    const q = new Deque();
    for (const i of pockets[0]) { cost[i] = 0; q.pushBack(i); }
    while (q.size > 0) {
      const i = q.popFront();
      const x = i % w, y = Math.floor(i / w);
      for (let d = 0; d < 4; d++) {
        const nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        const j = ny * w + nx;
        if (!onMass[j]) continue;
        const c = cost[i] + (t[j] === MOUNTAINS ? 1 : 0);
        if (c < cost[j]) {
          cost[j] = c;
          from[j] = i;
          if (t[j] === MOUNTAINS) q.pushBack(j); else q.pushFront(j);
        }
      }
    }
    for (let p = 1; p < pockets.length; p++) {
      for (let i = pockets[p][0]; i >= 0 && cost[i] > 0; i = from[i]) if (t[i] === MOUNTAINS) t[i] = HILLS;
    }
  }
}

// ------------------------------------------------------------------ rivers

/** Step 3: rivers from the feet of the hills down to the sea (or into
 *  another river), along the cheapest path over a noise field, so they wind
 *  along its valleys. Two tiles wide, as the tile set draws rivers best. */
function rivers(s, rng, g, t) {
  const w = g.w, n = w * g.h;
  const wander = new Noise(rng);
  const coast = g.distance((i) => t[i] === WATER);
  const mount = g.distance((i) => t[i] === MOUNTAINS || t[i] === HILLS);
  // sources: well inland, by the high ground, apart from each other
  const sources = [];
  for (let i = 0; i < n; i++) {
    if (t[i] !== WATER && t[i] !== MOUNTAINS && t[i] !== HILLS && coast[i] >= 6 && mount[i] >= 1 && mount[i] <= 2) {
      sources.push(i);
    }
  }
  rng.shuffle(sources);
  const want = s.rivers;
  const river = new Uint8Array(n);
  const started = [];
  // the way down: the cheapest path to the sea, a tile costing more where
  // the noise is high, so the river follows the noise's valleys and winds
  const cost = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const v = 0.5 + 0.5 * wander.fbm((i % w) / 12.0 + 3.3, Math.floor(i / w) / 12.0 + 1.1, 3);
    cost[i] = 1 + 14 * v * v;
  }
  for (const src of sources) {
    if (started.length >= want) break;
    let apart = true;
    for (const o of started) {
      if (dist(o % w - src % w, Math.floor(o / w) - Math.floor(src / w)) < 22) apart = false;
    }
    if (!apart) continue;
    const best = new Float64Array(n).fill(1e18);
    const from = new Int32Array(n).fill(-1);
    const q = new Heap();
    best[src] = 0;
    q.push(0, src);
    let end = -1;
    while (!q.empty()) {
      q.pop();
      const c = q.cost, i = q.tile;
      if (c > best[i]) continue;
      if (t[i] === WATER || river[i]) { end = i; break; }
      const x = i % w, y = Math.floor(i / w);
      for (let d = 0; d < 4; d++) {
        const nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        const j = ny * w + nx;
        if (t[j] === MOUNTAINS || t[j] === HILLS) continue;
        const nc = c + cost[j];
        if (nc < best[j]) { best[j] = nc; from[j] = i; q.push(nc, j); }
      }
    }
    if (end < 0) continue;
    const path = [];
    for (let i = from[end]; i >= 0; i = from[i]) path.push(i);
    path.reverse();
    if (path.length < 12) continue;
    started.push(src);
    for (let k = 0; k < path.length; k++) {
      const i = path[k], x = i % w, y = Math.floor(i / w);
      river[i] = 1;
      // the second tile: beside it, across the way the river runs
      const j = k + 1 < path.length ? path[k + 1] : (k > 0 ? path[k - 1] : i);
      const alongX = Math.abs(j - i) === 1;
      const ox = alongX ? 0 : 1, oy = alongX ? 1 : 0;
      if (g.in(x + ox, y + oy) && t[(y + oy) * w + x + ox] !== MOUNTAINS) river[(y + oy) * w + x + ox] = 1;
    }
  }
  for (let i = 0; i < n; i++) if (river[i]) t[i] = WATER;
}

// ------------------------------------------------------------------ growth

/** Step 4: forest where it is wet, marsh where it is wet and low. */
function growth(s, rng, g, t, height) {
  const w = g.w, n = w * g.h;
  const wet = new Noise(rng);
  const water = g.distance((i) => t[i] === WATER);
  const m = new Float64Array(n).fill(-9);
  const ms = [];
  let landTiles = 0;
  for (let i = 0; i < n; i++) {
    if (t[i] === WATER) continue;
    landTiles++;
    if (t[i] !== PLAIN) continue;
    const x = i % w, y = Math.floor(i / w);
    m[i] = wet.fbm(x / 26.0 + 4.4, y / 26.0 + 8.8, 4) + (water[i] <= 3 ? 0.08 * (4 - water[i]) : 0);
    ms.push(m[i]);
  }
  if (ms.length === 0) return;
  const fCut = cut(ms, 1 - Math.min(0.9, s.forest * landTiles / ms.length));
  for (let i = 0; i < n; i++) if (t[i] === PLAIN && m[i] >= fCut) t[i] = FOREST;
  // marsh: a few small patches on the wettest low ground by the water
  const low = [];
  for (let i = 0; i < n; i++) if (t[i] === PLAIN && water[i] <= 2) low.push(m[i] - height[i]);
  const mCut = cut(low, 0.97);
  for (let i = 0; i < n; i++) {
    if (t[i] === PLAIN && water[i] <= 2 && m[i] - height[i] >= mCut) t[i] = MARSH;
  }
}

// ------------------------------------------------------------------ cities, sites

function footprintOk(g, t, x, y) {
  if (x < 3 || y < 3 || x + 1 >= g.w - 3 || y + 1 >= g.h - 3) return false;
  for (let dy = 0; dy <= 1; dy++) {
    for (let dx = 0; dx <= 1; dx++) {
      const k = t[(y + dy) * g.w + x + dx];
      if (k !== PLAIN && k !== FOREST) return false;
    }
  }
  return true;
}

/** Best-candidate sampling (Mitchell): each new point the best of a few
 *  random candidates, the best being the furthest from the points already
 *  placed, plus `bonus`. Gives an even spread with no clumps. */
function spread(rng, spots, count, w, avoid, minDist, bonus, candidates) {
  const out = [];
  if (spots.length === 0) return out;
  const nearest = (x, y) => {
    let best = 1e9;
    for (const [ax, ay] of out) best = Math.min(best, dist(ax - x, ay - y));
    for (const [ax, ay] of avoid) best = Math.min(best, dist(ax - x, ay - y));
    return best;
  };
  for (let k = 0; k < count; k++) {
    let pick = -1, pickScore = -1e9;
    // `candidates` that keep their distance; on crowded land it may take
    // many draws to find them
    for (let c = 0, found = 0; found < candidates && c < 600; c++) {
      const s = spots[rng.below(spots.length)];
      const x = s % w, y = Math.floor(s / w);
      const d = nearest(x, y);
      if (d < minDist) continue;
      found++;
      const score = Math.min(d, 12.0) + bonus(s);
      if (score > pickScore) { pickScore = score; pick = s; }
    }
    if (pick < 0) continue;
    out.push([pick % w, Math.floor(pick / w)]);
  }
  return out;
}

// the words a world's name is made of
const ADJECTIVES = ["Shattered", "Sundered", "Burning", "Frozen", "Whispering", "Forgotten",
  "Gilded", "Crimson", "Iron", "Silver", "Hollow", "Endless",
  "Drowned", "Ashen", "Emerald", "Howling", "Sunken", "Twilight",
  "Broken", "Haunted", "Golden", "Bitter", "Wild", "Lost"];
const FIRST = ["Thorn", "Dusk", "Ember", "Raven", "Storm", "Frost", "Gold", "Wolf",
  "Mist", "Drake", "Shade", "Iron", "Bright", "Grim", "Moon", "Star",
  "Oak", "Black", "Red", "Winter", "Wyrm", "Ash", "Salt", "Hawk"];
const SECOND = ["mere", "hold", "vale", "fell", "wick", "haven", "march", "moor",
  "crest", "reach", "gard", "shire", "fall", "wood", "spire", "ford"];
const NOUNS = [
  ["Kingdoms", "Marches", "Dominions", "Realms", "Baronies", "Lands", "Reaches", "Shires"],      // continents
  ["Realm", "Expanse", "Heartland", "Empire", "Dominion", "Wilds", "Steppe", "Highlands"],       // pangaea
  ["Isles", "Archipelago", "Atolls", "Shoals", "Skerries", "Islands", "Straits", "Sea Lords"]];  // islands
const LINES = ["Eight warlords, one throne", "Where empires rise and fall",
  "Dragons stir beyond the hills", "Gold, steel and treachery",
  "A land waiting for a king", "Fortune favours the bold",
  "Old ruins, older magic", "Heroes wanted. Apply within",
  "The war begins at dawn", "Every road leads to war",
  "Crowns are won, not given", "Banners over every hill",
  "Beware the dark between towns", "Here be monsters (and loot)",
  "Many cities, one Warlord", "Sharpen your swords"];

const pickWord = (rng, list) => list[rng.below(list.length)];

/** A world's name for New Scenario's list (at most 20 characters) and its
 *  line of description (at most 30), drawn from the seed alone -- "The
 *  Shattered Isles", "Kingdoms of Ravenmere" -- so the same seed always gets
 *  the same name, and naming never changes the world. { name, text }. */
export function title(shape, seed) {
  const rng = new Rng(seed + 1000003.0);   // its own dice: naming never touches the world's
  const nouns = NOUNS[shape === "pangaea" ? 1 : shape === "islands" ? 2 : 0];
  const t = { name: "", text: "" };
  for (let tries = 0; tries < 50; tries++) {
    const first = pickWord(rng, FIRST);
    const place = first + pickWord(rng, SECOND);
    const noun = pickWord(rng, nouns), adj = pickWord(rng, ADJECTIVES);
    const form = rng.below(3);
    if (form === 0) t.name = "The " + adj + " " + noun;
    else if (form === 1) t.name = noun + " of " + place;
    else t.name = place + " " + noun;
    if (t.name.length <= 20) break;
  }
  if (t.name.length > 20) t.name = t.name.slice(0, 20);
  t.text = pickWord(rng, LINES);
  return t;
}

/** Steps 1-6: the ground, the cities and the sites, on a W x H grid:
 *  { terrain (terrain type ids), cities and sites ([x, y], a city's the top
 *  left of its 2x2 footprint) }. */
export function makeWorld(s, rng, w, h) {
  const g = new Grid(w, h);
  const n = w * h;
  const height = new Float64Array(n);
  const isLand = land(s, rng, g, height);
  const t = new Uint8Array(n).fill(WATER);
  for (let i = 0; i < n; i++) if (isLand[i]) t[i] = PLAIN;
  relief(s, rng, g, t, height);
  rivers(s, rng, g, t);
  growth(s, rng, g, t, height);

  // cities: on open ground with room round them, rather by water
  const water = g.distance((i) => t[i] === WATER);
  const spots = [];
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (footprintOk(g, t, x, y)) spots.push(y * w + x);
  const want = Math.min(100, Math.max(8, s.cities));
  const cities = spread(rng, spots, want, w, [], 2.9, (i) => {
    const d = water[i];
    return d >= 0 && d <= 2 ? 0.6 : 0.0;
  }, 4);
  for (const [x, y] of cities) {
    for (let dy = 0; dy <= 1; dy++) for (let dx = 0; dx <= 1; dx++) t[(y + dy) * w + x + dx] = CITY;
  }

  // sites: in the wilds, on ground an army can walk to from a city
  const open = g.parts((i) => t[i] !== WATER && t[i] !== MOUNTAINS);
  const part = new Int32Array(n).fill(-1);
  const settled = new Uint8Array(open.length);
  for (let k = 0; k < open.length; k++) for (const i of open[k]) part[i] = k;
  for (const [x, y] of cities) if (part[y * w + x] >= 0) settled[part[y * w + x]] = 1;
  const wild = [];
  for (let i = 0; i < n; i++) {
    const x = i % w, y = Math.floor(i / w);
    if (t[i] !== PLAIN || part[i] < 0 || !settled[part[i]]) continue;
    if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) continue;
    let clear = true;
    for (let dy = -2; dy <= 2 && clear; dy++) {
      for (let dx = -2; dx <= 2 && clear; dx++) {
        if (g.in(x + dx, y + dy) && t[(y + dy) * w + x + dx] === CITY) clear = false;
      }
    }
    if (clear) wild.push(i);
  }
  const avoid = cities.map(([x, y]) => [x, y]);
  const sites = spread(rng, wild, s.sites, w, avoid, 3.0, () => 0.0, 20);
  for (const [x, y] of sites) t[y * w + x] = SITE;
  return { terrain: t, cities, sites };
}

// ------------------------------------------------------------------ roads

/** Step 7: the roads between the cities over the finished terrain. `spans`
 *  are where a river may be bridged, { x, y, ew }: two water tiles in a row,
 *  (x, y) and the next east (ew) or south, with firm ground beyond both.
 *  Returns { road: 1 where a road runs, bridges: the indices into `spans`
 *  of the ones used }. */
export function planRoads(terrain, w, h, cities, spans, rng) {
  const g = new Grid(w, h);
  const n = w * h;
  const road = new Uint8Array(n);
  // the cost of building a road over each kind of ground
  const build = (i) => {
    switch (terrain[i]) {
      case PLAIN: return 1;
      case FOREST: return 2;
      case HILLS: return 3;
      case MARSH: return 5;
      case MOUNTAINS: return 9;
      default: return -1;   // water, shore, bridges, cities, sites, signposts
    }
  };
  // a bridge span: from the ground before it to the ground after it
  const leap = [];   // tile -> [[tile beyond, span], ...]
  for (let i = 0; i < n; i++) leap.push(null);
  for (let k = 0; k < spans.length; k++) {
    const sp = spans[k];
    const ax = sp.ew ? sp.x - 1 : sp.x, ay = sp.ew ? sp.y : sp.y - 1;
    const bx = sp.ew ? sp.x + 2 : sp.x, by = sp.ew ? sp.y : sp.y + 2;
    if (!g.in(ax, ay) || !g.in(bx, by)) continue;
    const a = ay * w + ax, b = by * w + bx;
    (leap[a] || (leap[a] = [])).push([b, k]);
    (leap[b] || (leap[b] = [])).push([a, k]);
  }
  // the tiles a road may leave a city from: the eight beside its footprint
  const RING = [[0, -1], [1, -1], [2, 0], [2, 1], [1, 2], [0, 2], [-1, 1], [-1, 0]];
  const doors = (c) => {
    const [x, y] = cities[c];
    const d = [];
    for (const r of RING) {
      const dx = x + r[0], dy = y + r[1];
      if (g.in(dx, dy) && build(dy * w + dx) > 0 && terrain[dy * w + dx] !== MOUNTAINS) d.push(dy * w + dx);
    }
    return d;
  };
  const door = [];
  const doorOf = new Int32Array(n).fill(-1);
  for (let c = 0; c < cities.length; c++) {
    door.push(doors(c));
    for (const i of door[c]) doorOf[i] = c;
  }
  // A*: the cheapest road from any door of a to any door of b, existing
  // roads cheap so new ones join them; returns its cost, and its tiles and
  // the spans it bridges into the lists given
  const cost = new Float64Array(n), from = new Int32Array(n), via = new Int32Array(n);
  const route = (a, b, tiles, used) => {
    if (door[a].length === 0 || door[b].length === 0) return -1;
    const [bx, by] = cities[b];
    const [ax0, ay0] = cities[a];
    const guess = (i) => 0.5 * (Math.abs(i % w - bx) + Math.abs(Math.floor(i / w) - by));
    // a little extra the further a tile is off the straight line, so a road
    // runs as a staircase along it rather than as one long L
    const lx = bx - ax0, ly = by - ay0, len = Math.max(1.0, dist(lx, ly));
    const off = (i) => 0.08 * Math.abs((i % w - ax0) * ly - (Math.floor(i / w) - ay0) * lx) / len;
    cost.fill(1e18);
    from.fill(-1);
    via.fill(-1);
    const q = new Heap();
    for (const i of door[a]) {
      cost[i] = road[i] ? 0.3 : build(i);
      q.push(cost[i] + guess(i), i);
    }
    let end = -1;
    while (!q.empty()) {
      q.pop();
      const f = q.cost, i = q.tile;
      if (f - guess(i) > cost[i] + 1e-9) continue;
      if (doorOf[i] === b) { end = i; break; }
      const x = i % w, y = Math.floor(i / w);
      const relax = (j, c, span) => {
        if (c < cost[j]) {
          cost[j] = c;
          from[j] = i;
          via[j] = span;
          q.push(c + guess(j), j);
        }
      };
      for (let d = 0; d < 4; d++) {
        const nx = x + DX4[d], ny = y + DY4[d];
        if (!g.in(nx, ny)) continue;
        const j = ny * w + nx;
        const c = build(j);
        if (c < 0) continue;
        relax(j, cost[i] + (road[j] ? 0.3 : c + off(j)), -1);
      }
      if (leap[i]) for (const [j, span] of leap[i]) relax(j, cost[i] + 4, span);
    }
    if (end < 0) return -1;
    if (tiles) {
      for (let i = end; i >= 0; i = from[i]) {
        tiles.push(i);
        if (used && via[i] >= 0) used.push(via[i]);
      }
    }
    return cost[end];
  };
  // the links worth having: each city's nearest few, by travel cost
  const links = [];
  const m = cities.length;
  for (let a = 0; a < m; a++) {
    const near = [];
    for (let b = 0; b < m; b++) {
      if (b !== a) near.push([dist(cities[a][0] - cities[b][0], cities[a][1] - cities[b][1]), b]);
    }
    near.sort((p, q) => (p[0] !== q[0] ? (p[0] < q[0] ? -1 : 1) : p[1] - q[1]));
    for (let k = 0; k < 4 && k < near.length; k++) {
      const b = near[k][1];
      if (near[k][0] > 26 || b < a) continue;
      const c = route(a, b, null, null);
      if (c > 0) links.push({ cost: c, a, b });
    }
  }
  const cheaper = (p, q) => {
    if (p.cost !== q.cost) return p.cost < q.cost ? -1 : 1;
    return p.a !== q.a ? p.a - q.a : p.b - q.b;
  };
  links.sort(cheaper);
  // the minimum spanning tree (Kruskal), then a few links where the tree
  // would send a traveller the long way round
  const root = [];
  for (let i = 0; i < m; i++) root.push(i);
  const find = (i) => (root[i] === i ? i : (root[i] = find(root[i])));
  const tree = [];
  for (let i = 0; i < m; i++) tree.push([]);
  const chosen = [];
  for (const l of links) {
    const ra = find(l.a), rb = find(l.b);
    if (ra === rb || l.cost > 34) continue;
    root[ra] = rb;
    chosen.push(l);
    tree[l.a].push([l.b, l.cost]);
    tree[l.b].push([l.a, l.cost]);
  }
  const treeCost = (a, b) => {
    const d = new Float64Array(m).fill(-1);
    const q = [a];
    d[a] = 0;
    for (let head = 0; head < q.length; head++) {
      const i = q[head];
      for (const [j, c] of tree[i]) if (d[j] < 0) { d[j] = d[i] + c; q.push(j); }
    }
    return d[b];
  };
  for (const l of links) {
    const t = treeCost(l.a, l.b);
    if (t > 2.2 * l.cost && l.cost <= 24 && uniform(rng) < 0.7) {
      chosen.push(l);
      tree[l.a].push([l.b, l.cost]);
      tree[l.b].push([l.a, l.cost]);
    }
  }
  // lay them, cheapest first, each joining what is there already
  chosen.sort(cheaper);
  const bridged = new Uint8Array(spans.length);
  for (const l of chosen) {
    const tiles = [], used = [];
    if (route(l.a, l.b, tiles, used) < 0) continue;
    for (const i of tiles) road[i] = 1;
    for (const k of used) bridged[k] = 1;
  }
  const bridges = [];
  for (let k = 0; k < spans.length; k++) if (bridged[k]) bridges.push(k);
  return { road, bridges };
}
