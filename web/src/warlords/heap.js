// A priority queue of (cost, tile) pairs that pops the smallest pair first,
// cost then tile: what C++'s std::priority_queue<std::pair<cost, int>,
// ..., std::greater<>> does. The order is a total one, so this binary heap
// pops the same pairs in the same order as the C++ and Lua ones, which is
// what lets the planners that use it (warlords/travel, warlords/scengen)
// make the same worlds in all three.

export class Heap {
  constructor() {
    this.costs = [];
    this.tiles = [];
    // the pair pop() took off
    this.cost = 0;
    this.tile = -1;
  }

  get size() { return this.costs.length; }
  empty() { return this.costs.length === 0; }

  less(i, j) {
    const a = this.costs[i], b = this.costs[j];
    return a < b || (a === b && this.tiles[i] < this.tiles[j]);
  }

  swap(i, j) {
    const c = this.costs[i]; this.costs[i] = this.costs[j]; this.costs[j] = c;
    const t = this.tiles[i]; this.tiles[i] = this.tiles[j]; this.tiles[j] = t;
  }

  push(cost, tile) {
    let i = this.costs.length;
    this.costs.push(cost);
    this.tiles.push(tile);
    while (i > 0) {
      const up = (i - 1) >> 1;
      if (!this.less(i, up)) break;
      this.swap(i, up);
      i = up;
    }
  }

  /** Take the smallest pair off into this.cost and this.tile. */
  pop() {
    this.cost = this.costs[0];
    this.tile = this.tiles[0];
    const lastCost = this.costs.pop(), lastTile = this.tiles.pop();
    const n = this.costs.length;
    if (n === 0) return;
    this.costs[0] = lastCost;
    this.tiles[0] = lastTile;
    let i = 0;
    for (;;) {
      const l = 2 * i + 1, r = l + 1;
      let m = i;
      if (l < n && this.less(l, m)) m = l;
      if (r < n && this.less(r, m)) m = r;
      if (m === i) break;
      this.swap(i, m);
      i = m;
    }
  }
}
