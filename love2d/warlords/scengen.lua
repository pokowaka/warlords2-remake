-- The scenario generator's world and road planners (not the original's). A
-- port of cpp/src/warlords/scengen.cpp, which says more; docs/scengen.md.
--
-- randommap's classic generator is a port of the 1993 one, byte for byte.
-- The scenario generator is a new design. It uses the same file machinery:
-- RANDOM.DAT's tile tables turn terrain into tiles, and the same city, site
-- and side records are written. What differs is the order and the methods:
--
--   1. land     domain-warped fractal noise, sea at the edges, a sea level
--               picked for a target land share, small islands and lakes
--               cleaned away;
--   2. relief   mountain ranges along ridged noise rather than blobs, hills
--               round them, every mountain-walled pocket given a pass;
--   3. rivers   from the feet of the mountains downhill to the sea, winding
--               by a smooth noise, joining where they meet;
--   4. growth   forest and marsh by a moisture field, wetter by the rivers;
--   5. cities   spread evenly by best-candidate (blue noise) sampling,
--               preferring coasts and rivers;
--   6. sites    the ruins and temples spread the same way, in the wilds;
--   7. roads    after the tiles are drawn: a minimum spanning tree of the
--               cities by travel cost, plus a few extra links where the tree
--               makes a long detour, each laid by A* and merged into roads
--               already there, bridges where they cross a river.
--
-- randommap (opts.scenario) calls makeWorld, draws the tiles, calls
-- planRoads, then chooses fair capitals and writes the files.
--
-- The C++ and JavaScript ports make the same worlds from the same seed, byte
-- for byte, so everything here keeps to arithmetic IEEE doubles give alike
-- in all three: + - * / sqrt floor, each expression in the C++'s order, no
-- pow, sorts with no ties, and queues ordered by (cost, tile). Grids are
-- 0-based, w x h, as the C++'s vectors are; lists (cities, sites, parts,
-- spots) are Lua's 1-based ones.

local move = require("warlords.move")
local heap = require("warlords.heap")

local M = {}

-- terrain type ids
local WATER, FOREST, HILLS, MOUNTAINS = move.WATER, move.FOREST, move.HILLS, move.MOUNTAINS
local PLAIN, MARSH, CITY, SITE = move.PLAIN, 8, 10, 11

local DIRS = move.DIRS
local DX4 = { [0] = 0, 1, 0, -1 }
local DY4 = { [0] = -1, 0, 1, 0 }

M.SHAPES = { "continents", "pangaea", "islands" }

--- The settings a world is made with; `t`'s own fields over the defaults.
--- land < 0 picks the shape's own share.
function M.settings(t)
  t = t or {}
  local s = { shape = "continents", land = -1, cities = 80, sites = 40,
              mountains = 0.08, hills = 0.10, forest = 0.17, rivers = 5 }
  for k, v in pairs(t) do s[k] = v end
  return s
end

--- "continents", "pangaea" or "islands", or nil if it is none.
function M.parseShape(s)
  for _, name in ipairs(M.SHAPES) do if s == name then return name end end
  return nil
end

function M.shapeName(shape)
  if shape == "pangaea" or shape == "islands" then return shape end
  return "continents"
end

local function uniform(rng) return rng:below(65536) / 65536.0 end
local function mix(a, b, t) return a + t * (b - a) end
local function dist(dx, dy) return math.sqrt(dx * dx + dy * dy) end
local function abs(v) return v < 0 and -v or v end
local function min(a, b) if b < a then return b end return a end
local function max(a, b) if a < b then return b end return a end

-- the row and column of a 0-based tile index
local function col(i, w) return i % w end
local function row(i, w) return (i - i % w) / w end

------------------------------------------------------------------ noise

--- Perlin's gradient noise, its permutation drawn from the dice.
local Noise = {}
Noise.__index = Noise

local function newNoise(rng)
  local p = {}
  for i = 1, 256 do p[i] = i - 1 end
  rng:shuffle(p)
  local perm = {}
  for i = 0, 511 do perm[i] = p[i % 256 + 1] end
  return setmetatable({ perm = perm }, Noise)
end

local function fade(t) return t * t * t * (t * (t * 6 - 15) + 10) end

local function grad(h, dx, dy)
  h = h % 8
  if h == 0 then return dx + dy end
  if h == 1 then return dx - dy end
  if h == 2 then return -dx + dy end
  if h == 3 then return -dx - dy end
  if h == 4 then return dx end
  if h == 5 then return -dx end
  if h == 6 then return dy end
  return -dy
end

--- About -1..1.
function Noise:at(x, y)
  local perm = self.perm
  local xi, yi = math.floor(x), math.floor(y)
  local xf, yf = x - xi, y - yi
  local X, Y = xi % 256, yi % 256
  local u, v = fade(xf), fade(yf)
  local aa, ab = perm[perm[X] + Y], perm[perm[X] + Y + 1]
  local ba, bb = perm[perm[X + 1] + Y], perm[perm[X + 1] + Y + 1]
  local x1 = mix(grad(aa, xf, yf), grad(ba, xf - 1, yf), u)
  local x2 = mix(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u)
  return mix(x1, x2, v)
end

--- Fractal Brownian motion: octaves of the noise, each twice as fine.
function Noise:fbm(x, y, octaves)
  local sum, amp, norm = 0, 1, 0
  for _ = 1, octaves or 5 do
    sum = sum + amp * self:at(x, y)
    norm = norm + amp
    amp = amp * 0.5
    x = x * 2.03
    y = y * 2.03
  end
  return sum / norm
end

--- The value at quantile q (0..1) of the listed values: the k-th smallest,
--- k = floor(q * (n - 1)), as the C++'s nth_element finds it.
local function cut(v, q)
  local n = #v
  if n == 0 then return 0 end
  local s = {}
  for i = 1, n do s[i] = v[i] end
  table.sort(s)
  -- below 0 only for settings far off the defaults; the C++'s cast of a
  -- negative double would be undefined there
  local k = math.max(0, math.min(n - 1, math.floor(q * (n - 1))))
  return s[k + 1]
end

------------------------------------------------------------------ the grid

local Grid = {}
Grid.__index = Grid

local function newGrid(w, h) return setmetatable({ w = w, h = h }, Grid) end

function Grid:inside(x, y) return x >= 0 and y >= 0 and x < self.w and y < self.h end

--- The 8-connected (or 4-connected) parts of the tiles that pass `test`, in
--- the order of their first tiles, each part in the order it was found.
function Grid:parts(test, four)
  local w, n = self.w, self.w * self.h
  local seen, out = {}, {}
  local steps = four and 4 or 8
  for s = 0, n - 1 do
    if not seen[s] and test(s) then
      local part = { s }
      seen[s] = true
      local k = 1
      while k <= #part do
        local i = part[k]
        local x, y = col(i, w), row(i, w)
        for d = 0, steps - 1 do
          local nx, ny
          if four then nx, ny = x + DX4[d], y + DY4[d]
          else nx, ny = x + DIRS[d][1], y + DIRS[d][2] end
          if self:inside(nx, ny) then
            local j = ny * w + nx
            if not seen[j] and test(j) then seen[j] = true part[#part + 1] = j end
          end
        end
        k = k + 1
      end
      out[#out + 1] = part
    end
  end
  return out
end

--- Steps (4-connected) from the nearest tile passing `from`; -1 where none.
function Grid:distance(from)
  local w, n = self.w, self.w * self.h
  local d, q = {}, {}
  for i = 0, n - 1 do
    if from(i) then d[i] = 0 q[#q + 1] = i else d[i] = -1 end
  end
  local head = 1
  while head <= #q do
    local i = q[head]
    head = head + 1
    local x, y = col(i, w), row(i, w)
    for k = 0, 3 do
      local nx, ny = x + DX4[k], y + DY4[k]
      if self:inside(nx, ny) then
        local j = ny * w + nx
        if d[j] < 0 then d[j] = d[i] + 1 q[#q + 1] = j end
      end
    end
  end
  return d
end

--- A double-ended queue, for the 0-1 search.
local function newDeque() return { first = 1, last = 0 } end
local function pushBack(q, v) q.last = q.last + 1 q[q.last] = v end
local function pushFront(q, v) q.first = q.first - 1 q[q.first] = v end
local function popFront(q)
  local v = q[q.first]
  q[q.first] = nil
  q.first = q.first + 1
  return v
end
local function dequeEmpty(q) return q.first > q.last end

local function defaultLand(shape)
  if shape == "pangaea" then return 0.60 end
  if shape == "islands" then return 0.42 end
  return 0.55
end

local function copy(t, n)
  local c = {}
  for i = 0, n - 1 do c[i] = t[i] end
  return c
end

------------------------------------------------------------------ land

--- Step 1: where the land is (true) and its height (0..1 inland).
local function land(s, rng, g, height)
  local w, h = g.w, g.h
  local n = w * h
  local base, warp1, warp2, channel = newNoise(rng), newNoise(rng), newNoise(rng), newNoise(rng)
  local f = s.shape == "islands" and 1.0 / 26 or 1.0 / 42
  local e = {}
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local px, py = x * f, y * f
      -- domain warping: the noise read where another noise points
      local qx, qy = warp1:fbm(px + 5.2, py + 1.3, 4), warp2:fbm(px + 1.7, py + 9.2, 4)
      local v = base:fbm(px + 1.6 * qx, py + 1.6 * qy, 5)
      -- the sea closes in at the map's edges
      local edge = math.min(x, w - 1 - x, y, h - 1 - y) / 14.0
      edge = min(1.0, edge)
      v = v - (1 - edge) * (1 - edge) * 0.9
      if s.shape == "continents" then
        -- straits along the zero lines of a slow noise split the land
        local c = abs(channel:fbm(x / 70.0 + 3.1, y / 70.0 + 7.7, 3))
        if c < 0.09 then v = v - 0.45 * (0.09 - c) / 0.09 end
      elseif s.shape == "pangaea" then
        local dx, dy = (x - w / 2.0) / (w / 2.0), (y - h / 2.0) / (h / 2.0)
        v = v + 0.45 * (1 - min(1.0, math.sqrt(dx * dx + dy * dy)))
      end
      e[y * w + x] = v
    end
  end
  local share = s.land >= 0 and s.land or defaultLand(s.shape)
  local list = {}
  for i = 0, n - 1 do list[i + 1] = e[i] end
  local sea = cut(list, 1 - share)
  local isLand = {}
  for i = 0, n - 1 do isLand[i] = e[i] > sea end
  -- smooth the coast so the tile set can draw it: a tile goes with most of
  -- its neighbours
  for _ = 1, 2 do
    local nextLand = copy(isLand, n)
    for y = 0, h - 1 do
      for x = 0, w - 1 do
        local k = 0
        for d = 0, 7 do
          local nx, ny = x + DIRS[d][1], y + DIRS[d][2]
          if g:inside(nx, ny) and isLand[ny * w + nx] then k = k + 1 end
        end
        if k >= 5 then nextLand[y * w + x] = true
        elseif k <= 3 then nextLand[y * w + x] = false end
      end
    end
    isLand = nextLand
  end
  -- no land on the map's outermost two rows
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      if x < 2 or y < 2 or x >= w - 2 or y >= h - 2 then isLand[y * w + x] = false end
    end
  end
  -- specks of land go under; small lakes fill in
  local minIsland = s.shape == "islands" and 40 or 70
  for _, part in ipairs(g:parts(function(i) return isLand[i] end)) do
    if #part < minIsland then for _, i in ipairs(part) do isLand[i] = false end end
  end
  for _, part in ipairs(g:parts(function(i) return not isLand[i] end, true)) do
    if #part < 30 then for _, i in ipairs(part) do isLand[i] = true end end
  end
  -- the height inland: distance from the coast, roughened by the noise
  local coast = g:distance(function(i) return not isLand[i] end)
  for i = 0, n - 1 do
    height[i] = 0
    if isLand[i] then
      height[i] = 0.65 * min(1.0, coast[i] / 14.0) + 0.35 * (e[i] - sea) / max(1e-6, 1 - sea)
    end
  end
  return isLand
end

------------------------------------------------------------------ relief

--- Step 2: ranges along ridged noise, hills round them, and a pass into
--- every pocket of ground the mountains would wall off.
local function relief(s, rng, g, t, height)
  local w = g.w
  local n = w * g.h
  local ridges, lumps = newNoise(rng), newNoise(rng)
  local coast = g:distance(function(i) return t[i] == WATER end)
  local score, eligible = {}, {}
  for i = 0, n - 1 do
    score[i] = -1
    if t[i] ~= WATER then
      local x, y = col(i, w), row(i, w)
      local r = 1 - abs(ridges:fbm(x / 30.0, y / 30.0, 4))   -- ridged: high along lines
      score[i] = r * r + 0.35 * height[i]
      if coast[i] >= 2 then eligible[#eligible + 1] = score[i] end
    end
  end
  local landTiles = 0
  for i = 0, n - 1 do if t[i] ~= WATER then landTiles = landTiles + 1 end end
  -- the crests first, at a third of the share, then the ranges widened
  -- round them onto the next-highest ground
  local mShare = min(0.9, s.mountains * landTiles / max(1, #eligible))
  local crest, flank = cut(eligible, 1 - mShare / 3), cut(eligible, 1 - mShare * 1.6)
  for i = 0, n - 1 do
    if score[i] >= crest and coast[i] >= 2 then t[i] = MOUNTAINS end
  end
  for _ = 1, 2 do
    local nextT = copy(t, n)
    for i = 0, n - 1 do
      if not (t[i] == WATER or t[i] == MOUNTAINS or coast[i] < 2 or score[i] < flank) then
        local x, y = col(i, w), row(i, w)
        for d = 0, 7 do
          local nx, ny = x + DIRS[d][1], y + DIRS[d][2]
          if g:inside(nx, ny) and t[ny * w + nx] == MOUNTAINS then nextT[i] = MOUNTAINS break end
        end
      end
    end
    for i = 0, n - 1 do t[i] = nextT[i] end
  end
  -- hills: the foothills and lumps of their own
  local hscore, hs = {}, {}
  local nearMount = g:distance(function(i) return t[i] == MOUNTAINS end)
  for i = 0, n - 1 do
    hscore[i] = -1
    if t[i] == PLAIN then
      local x, y = col(i, w), row(i, w)
      local l = lumps:fbm(x / 13.0 + 11.3, y / 13.0 + 2.9, 3)
      local foot = (nearMount[i] >= 0 and nearMount[i] <= 2) and 0.6 - 0.2 * nearMount[i] or 0
      hscore[i] = l + foot + 0.2 * score[i]
      hs[#hs + 1] = hscore[i]
    end
  end
  local hCut = cut(hs, 1 - min(0.9, s.hills * landTiles / max(1, #hs)))
  for i = 0, n - 1 do
    if t[i] == PLAIN and hscore[i] >= hCut then t[i] = HILLS end
  end
  -- passes: on each landmass, every pocket of open ground the mountains cut
  -- off gets the cheapest way through them to the landmass's largest one
  for _, mass in ipairs(g:parts(function(i) return t[i] ~= WATER end)) do
    local onMass = {}
    for _, i in ipairs(mass) do onMass[i] = true end
    local pockets = g:parts(function(i) return onMass[i] and t[i] ~= MOUNTAINS end)
    if #pockets >= 2 then
      table.sort(pockets, function(a, b)
        if #a ~= #b then return #a > #b end
        return a[1] < b[1]
      end)
      -- 0-1 search from the largest: a mountain tile costs 1, open ground 0
      local cost, from = {}, {}
      for i = 0, n - 1 do cost[i] = 1073741824 from[i] = -1 end
      local q = newDeque()
      for _, i in ipairs(pockets[1]) do cost[i] = 0 pushBack(q, i) end
      while not dequeEmpty(q) do
        local i = popFront(q)
        local x, y = col(i, w), row(i, w)
        for d = 0, 3 do
          local nx, ny = x + DX4[d], y + DY4[d]
          if g:inside(nx, ny) then
            local j = ny * w + nx
            if onMass[j] then
              local c = cost[i] + (t[j] == MOUNTAINS and 1 or 0)
              if c < cost[j] then
                cost[j] = c
                from[j] = i
                if t[j] == MOUNTAINS then pushBack(q, j) else pushFront(q, j) end
              end
            end
          end
        end
      end
      for p = 2, #pockets do
        local i = pockets[p][1]
        while i >= 0 and cost[i] > 0 do
          if t[i] == MOUNTAINS then t[i] = HILLS end
          i = from[i]
        end
      end
    end
  end
end

------------------------------------------------------------------ rivers

--- Step 3: rivers from the feet of the hills down to the sea (or into
--- another river), along the cheapest path over a noise field, so they wind
--- along its valleys. Two tiles wide, as the tile set draws rivers best.
local function rivers(s, rng, g, t)
  local w = g.w
  local n = w * g.h
  local wander = newNoise(rng)
  local coast = g:distance(function(i) return t[i] == WATER end)
  local mount = g:distance(function(i) return t[i] == MOUNTAINS or t[i] == HILLS end)
  -- sources: well inland, by the high ground, apart from each other
  local sources = {}
  for i = 0, n - 1 do
    if t[i] ~= WATER and t[i] ~= MOUNTAINS and t[i] ~= HILLS and coast[i] >= 6 and mount[i] >= 1 and mount[i] <= 2 then
      sources[#sources + 1] = i
    end
  end
  rng:shuffle(sources)
  local want = s.rivers
  local river, started = {}, {}
  for i = 0, n - 1 do river[i] = false end
  -- the way down: the cheapest path to the sea, a tile costing more where
  -- the noise is high, so the river follows the noise's valleys and winds
  local cost = {}
  for i = 0, n - 1 do
    local v = 0.5 + 0.5 * wander:fbm(col(i, w) / 12.0 + 3.3, row(i, w) / 12.0 + 1.1, 3)
    cost[i] = 1 + 14 * v * v
  end
  for _, src in ipairs(sources) do
    if #started >= want then break end
    local apart = true
    for _, o in ipairs(started) do
      if dist(col(o, w) - col(src, w), row(o, w) - row(src, w)) < 22 then apart = false end
    end
    if apart then
      local best, from = {}, {}
      for i = 0, n - 1 do best[i] = 1e18 from[i] = -1 end
      local q = heap.new()
      best[src] = 0
      q:push(0, src)
      local finish = -1
      while not q:empty() do
        local c, i = q:pop()
        if not (c > best[i]) then
          if t[i] == WATER or river[i] then finish = i break end
          local x, y = col(i, w), row(i, w)
          for d = 0, 3 do
            local nx, ny = x + DX4[d], y + DY4[d]
            if g:inside(nx, ny) then
              local j = ny * w + nx
              if t[j] ~= MOUNTAINS and t[j] ~= HILLS then
                local nc = c + cost[j]
                if nc < best[j] then best[j] = nc from[j] = i q:push(nc, j) end
              end
            end
          end
        end
      end
      if finish >= 0 then
        local back = {}
        local i = from[finish]
        while i >= 0 do back[#back + 1] = i i = from[i] end
        local path = {}
        for k = #back, 1, -1 do path[#path + 1] = back[k] end
        if #path >= 12 then
          started[#started + 1] = src
          for k = 1, #path do
            local i = path[k]
            local x, y = col(i, w), row(i, w)
            river[i] = true
            -- the second tile: beside it, across the way the river runs
            local j = k < #path and path[k + 1] or (k > 1 and path[k - 1] or i)
            local alongX = abs(j - i) == 1
            local ox, oy = alongX and 0 or 1, alongX and 1 or 0
            if g:inside(x + ox, y + oy) and t[(y + oy) * w + x + ox] ~= MOUNTAINS then
              river[(y + oy) * w + x + ox] = true
            end
          end
        end
      end
    end
  end
  for i = 0, n - 1 do if river[i] then t[i] = WATER end end
end

------------------------------------------------------------------ growth

--- Step 4: forest where it is wet, marsh where it is wet and low.
local function growth(s, rng, g, t, height)
  local w = g.w
  local n = w * g.h
  local wet = newNoise(rng)
  local water = g:distance(function(i) return t[i] == WATER end)
  local m, ms = {}, {}
  local landTiles = 0
  for i = 0, n - 1 do
    m[i] = -9
    if t[i] ~= WATER then
      landTiles = landTiles + 1
      if t[i] == PLAIN then
        local x, y = col(i, w), row(i, w)
        m[i] = wet:fbm(x / 26.0 + 4.4, y / 26.0 + 8.8, 4) + (water[i] <= 3 and 0.08 * (4 - water[i]) or 0)
        ms[#ms + 1] = m[i]
      end
    end
  end
  if #ms == 0 then return end
  local fCut = cut(ms, 1 - min(0.9, s.forest * landTiles / #ms))
  for i = 0, n - 1 do
    if t[i] == PLAIN and m[i] >= fCut then t[i] = FOREST end
  end
  -- marsh: a few small patches on the wettest low ground by the water
  local low = {}
  for i = 0, n - 1 do
    if t[i] == PLAIN and water[i] <= 2 then low[#low + 1] = m[i] - height[i] end
  end
  local mCut = cut(low, 0.97)
  for i = 0, n - 1 do
    if t[i] == PLAIN and water[i] <= 2 and m[i] - height[i] >= mCut then t[i] = MARSH end
  end
end

------------------------------------------------------------------ cities, sites

local function footprintOk(g, t, x, y)
  if x < 3 or y < 3 or x + 1 >= g.w - 3 or y + 1 >= g.h - 3 then return false end
  for dy = 0, 1 do
    for dx = 0, 1 do
      local k = t[(y + dy) * g.w + x + dx]
      if k ~= PLAIN and k ~= FOREST then return false end
    end
  end
  return true
end

--- Best-candidate sampling (Mitchell): each new point the best of a few
--- random candidates, the best being the furthest from the points already
--- placed, plus `bonus`. Gives an even spread with no clumps. `spots` are
--- tile indices; the points come out as { x, y }.
local function spread(rng, spots, count, w, avoid, minDist, bonus, candidates)
  local out = {}
  if #spots == 0 then return out end
  local function nearest(x, y)
    local best = 1e9
    for _, a in ipairs(out) do best = min(best, dist(a[1] - x, a[2] - y)) end
    for _, a in ipairs(avoid) do best = min(best, dist(a[1] - x, a[2] - y)) end
    return best
  end
  for _ = 1, count do
    local pick, pickScore = -1, -1e9
    -- `candidates` that keep their distance; on crowded land it may take
    -- many draws to find them
    local c, found = 0, 0
    while found < candidates and c < 600 do
      local s = spots[rng:below(#spots) + 1]
      local x, y = col(s, w), row(s, w)
      local d = nearest(x, y)
      if d >= minDist then
        found = found + 1
        local score = min(d, 12.0) + bonus(s)
        if score > pickScore then pickScore = score pick = s end
      end
      c = c + 1
    end
    if pick >= 0 then out[#out + 1] = { col(pick, w), row(pick, w) } end
  end
  return out
end

-- the words a world's name is made of
local ADJECTIVES = { "Shattered", "Sundered", "Burning", "Frozen", "Whispering", "Forgotten",
                     "Gilded", "Crimson", "Iron", "Silver", "Hollow", "Endless",
                     "Drowned", "Ashen", "Emerald", "Howling", "Sunken", "Twilight",
                     "Broken", "Haunted", "Golden", "Bitter", "Wild", "Lost" }
local FIRST = { "Thorn", "Dusk", "Ember", "Raven", "Storm", "Frost", "Gold", "Wolf",
                "Mist", "Drake", "Shade", "Iron", "Bright", "Grim", "Moon", "Star",
                "Oak", "Black", "Red", "Winter", "Wyrm", "Ash", "Salt", "Hawk" }
local SECOND = { "mere", "hold", "vale", "fell", "wick", "haven", "march", "moor",
                 "crest", "reach", "gard", "shire", "fall", "wood", "spire", "ford" }
local NOUNS = {
  continents = { "Kingdoms", "Marches", "Dominions", "Realms", "Baronies", "Lands", "Reaches", "Shires" },
  pangaea = { "Realm", "Expanse", "Heartland", "Empire", "Dominion", "Wilds", "Steppe", "Highlands" },
  islands = { "Isles", "Archipelago", "Atolls", "Shoals", "Skerries", "Islands", "Straits", "Sea Lords" },
}
local LINES = { "Eight warlords, one throne", "Where empires rise and fall",
                "Dragons stir beyond the hills", "Gold, steel and treachery",
                "A land waiting for a king", "Fortune favours the bold",
                "Old ruins, older magic", "Heroes wanted. Apply within",
                "The war begins at dawn", "Every road leads to war",
                "Crowns are won, not given", "Banners over every hill",
                "Beware the dark between towns", "Here be monsters (and loot)",
                "Many cities, one Warlord", "Sharpen your swords" }

local function pickWord(rng, list) return list[rng:below(#list) + 1] end

--- A world's name for New Scenario's list (at most 20 characters) and its
--- line of description (at most 30), drawn from the seed alone -- "The
--- Shattered Isles", "Kingdoms of Ravenmere" -- so the same seed always gets
--- the same name, and naming never changes the world. Returns name, text.
-- How far each letter moves the pen in CHANCE17, the font New Scenario's
-- list draws names in (CHANCE17.FIN), and the widest name the list's box
-- holds: "The Frozen Steppe", 140 pixels; the shipped names reach 130.
local UPPER_WIDTH = { 15, 16, 13, 17, 15, 18, 15, 16, 12, 14, 15, 14, 18, 16, 15, 15, 15, 16, 14, 13, 16, 17, 24, 16, 13, 14 }
local LOWER_WIDTH = { 7, 6, 7, 9, 6, 8, 6, 7, 4, 7, 7, 4, 12, 8, 7, 8, 6, 6, 6, 5, 9, 9, 14, 9, 9, 8 }
local NAME_WIDTH, NAME_CHARS = 140, 20

local function nameWidth(s)
  local n = 0
  for i = 1, #s do
    local c = s:byte(i)
    if c >= 65 and c <= 90 then n = n + UPPER_WIDTH[c - 64]
    elseif c >= 97 and c <= 122 then n = n + LOWER_WIDTH[c - 96]
    elseif c == 32 then n = n + 7
    elseif c == 39 then n = n + 3
    else n = n + 16 end
  end
  return n
end
local function fits(s) return #s <= NAME_CHARS and nameWidth(s) <= NAME_WIDTH end
M.nameWidth = nameWidth

function M.title(shape, seed)
  local rng = require("warlords.rng").new(seed + 1000003)   -- its own dice
  local nouns = NOUNS[M.shapeName(shape)]
  local name, noun = "", ""
  for _ = 1, 50 do
    local first = pickWord(rng, FIRST)
    local place = first .. pickWord(rng, SECOND)
    noun = pickWord(rng, nouns)
    local adj = pickWord(rng, ADJECTIVES)
    local form = rng:below(3)
    if form == 0 then name = "The " .. adj .. " " .. noun
    elseif form == 1 then name = noun .. " of " .. place
    else name = place .. " " .. noun end
    if fits(name) then break end
  end
  if not fits(name) then name = "The " .. noun end   -- fits whatever the noun
  return name, pickWord(rng, LINES)
end

--- Steps 1-6: the ground, the cities and the sites, on a w x h grid.
--- Returns { terrain (0-based, w x h), cities = { {x, y}... } (the top-left
--- of each 2x2 footprint), sites = { {x, y}... } }.
function M.makeWorld(settings, rng, w, h)
  local s = M.settings(settings)
  local g = newGrid(w, h)
  local n = w * h
  local height = {}
  local isLand = land(s, rng, g, height)
  local t = {}
  for i = 0, n - 1 do t[i] = isLand[i] and PLAIN or WATER end
  relief(s, rng, g, t, height)
  rivers(s, rng, g, t)
  growth(s, rng, g, t, height)

  -- cities: on open ground with room round them, rather by water
  local water = g:distance(function(i) return t[i] == WATER end)
  local spots = {}
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      if footprintOk(g, t, x, y) then spots[#spots + 1] = y * w + x end
    end
  end
  local want = math.max(8, math.min(100, s.cities))
  local cities = spread(rng, spots, want, w, {}, 2.9, function(i)
    local d = water[i]
    return (d >= 0 and d <= 2) and 0.6 or 0.0
  end, 4)
  for _, c in ipairs(cities) do
    for dy = 0, 1 do
      for dx = 0, 1 do t[(c[2] + dy) * w + c[1] + dx] = CITY end
    end
  end

  -- sites: in the wilds, on ground an army can walk to from a city
  local open = g:parts(function(i) return t[i] ~= WATER and t[i] ~= MOUNTAINS end)
  local part, settled = {}, {}
  for i = 0, n - 1 do part[i] = -1 end
  for k, p in ipairs(open) do
    for _, i in ipairs(p) do part[i] = k end
  end
  for _, c in ipairs(cities) do
    local p = part[c[2] * w + c[1]]
    if p >= 0 then settled[p] = true end
  end
  local wild = {}
  for i = 0, n - 1 do
    local x, y = col(i, w), row(i, w)
    if t[i] == PLAIN and part[i] >= 0 and settled[part[i]]
       and not (x < 3 or y < 3 or x >= w - 3 or y >= h - 3) then
      local clear = true
      for dy = -2, 2 do
        for dx = -2, 2 do
          if clear and g:inside(x + dx, y + dy) and t[(y + dy) * w + x + dx] == CITY then clear = false end
        end
      end
      if clear then wild[#wild + 1] = i end
    end
  end
  local avoid = {}
  for _, c in ipairs(cities) do avoid[#avoid + 1] = { c[1], c[2] } end
  local sites = spread(rng, wild, s.sites, w, avoid, 3.0, function() return 0.0 end, 20)
  for _, p in ipairs(sites) do t[p[2] * w + p[1]] = SITE end
  return { terrain = t, cities = cities, sites = sites }
end

------------------------------------------------------------------ roads

--- Step 7: the roads between the cities over the finished terrain.
---
--- terrain: 0-based, w x h. cities: { {x, y}... }. spans: where a bridge may
--- go, { {x =, y =, ew =}... } -- two water tiles in a row with firm ground
--- beyond both ends, `ew` spanning east-west (x, x+1), else north-south.
--- Returns { road (0-based, true where a road runs), bridges (the indices
--- into `spans` used, ascending) }.
function M.planRoads(terrain, w, h, cities, spans, rng)
  local g = newGrid(w, h)
  local n = w * h
  local road = {}
  for i = 0, n - 1 do road[i] = false end
  -- the cost of building a road over each kind of ground
  local BUILD = { [PLAIN] = 1, [FOREST] = 2, [HILLS] = 3, [MARSH] = 5, [MOUNTAINS] = 9 }
  local function build(i) return BUILD[terrain[i]] or -1 end
  -- a bridge span: from the ground before it to the ground after it
  local leap = {}   -- tile -> { {tile beyond, span}... }
  for k, sp in ipairs(spans) do
    local ax, ay = sp.ew and sp.x - 1 or sp.x, sp.ew and sp.y or sp.y - 1
    local bx, by = sp.ew and sp.x + 2 or sp.x, sp.ew and sp.y or sp.y + 2
    if g:inside(ax, ay) and g:inside(bx, by) then
      local a, b = ay * w + ax, by * w + bx
      leap[a] = leap[a] or {}
      leap[a][#leap[a] + 1] = { b, k }
      leap[b] = leap[b] or {}
      leap[b][#leap[b] + 1] = { a, k }
    end
  end
  -- the tiles a road may leave a city from: the eight beside its footprint
  local RING = { { 0, -1 }, { 1, -1 }, { 2, 0 }, { 2, 1 }, { 1, 2 }, { 0, 2 }, { -1, 1 }, { -1, 0 } }
  local m = #cities
  local door, doorOf = {}, {}
  for c = 1, m do
    local x, y = cities[c][1], cities[c][2]
    local d = {}
    for _, r in ipairs(RING) do
      local dx, dy = x + r[1], y + r[2]
      if g:inside(dx, dy) and build(dy * w + dx) > 0 and terrain[dy * w + dx] ~= MOUNTAINS then
        d[#d + 1] = dy * w + dx
      end
    end
    door[c] = d
    for _, i in ipairs(d) do doorOf[i] = c end
  end
  -- A*: the cheapest road from any door of a to any door of b, existing
  -- roads cheap so new ones join them; returns its cost (-1 for none), and
  -- with `tiles` given, its tiles and the spans it bridges into those lists
  local function route(a, b, tiles, used)
    if #door[a] == 0 or #door[b] == 0 then return -1 end
    local bx, by = cities[b][1], cities[b][2]
    local ax0, ay0 = cities[a][1], cities[a][2]
    local function guess(i) return 0.5 * (abs(col(i, w) - bx) + abs(row(i, w) - by)) end
    -- a little extra the further a tile is off the straight line, so a road
    -- runs as a staircase along it rather than as one long L
    local lx, ly = bx - ax0, by - ay0
    local len = max(1.0, dist(lx, ly))
    local function off(i) return 0.08 * abs((col(i, w) - ax0) * ly - (row(i, w) - ay0) * lx) / len end
    local cost, from, via = {}, {}, {}
    for i = 0, n - 1 do cost[i] = 1e18 from[i] = -1 via[i] = -1 end
    local q = heap.new()
    for _, i in ipairs(door[a]) do
      cost[i] = road[i] and 0.3 or build(i)
      q:push(cost[i] + guess(i), i)
    end
    local finish = -1
    while not q:empty() do
      local f, i = q:pop()
      if not (f - guess(i) > cost[i] + 1e-9) then
        if doorOf[i] == b then finish = i break end
        local x, y = col(i, w), row(i, w)
        local function relax(j, c, span)
          if c < cost[j] then
            cost[j] = c
            from[j] = i
            via[j] = span
            q:push(c + guess(j), j)
          end
        end
        for d = 0, 3 do
          local nx, ny = x + DX4[d], y + DY4[d]
          if g:inside(nx, ny) then
            local j = ny * w + nx
            local c = build(j)
            if c >= 0 then relax(j, cost[i] + (road[j] and 0.3 or c + off(j)), -1) end
          end
        end
        if leap[i] then
          for _, l in ipairs(leap[i]) do relax(l[1], cost[i] + 4, l[2]) end
        end
      end
    end
    if finish < 0 then return -1 end
    if tiles then
      local i = finish
      while i >= 0 do
        tiles[#tiles + 1] = i
        if used and via[i] >= 0 then used[#used + 1] = via[i] end
        i = from[i]
      end
    end
    return cost[finish]
  end
  -- the links worth having: each city's nearest few, by travel cost
  local links = {}
  for a = 1, m do
    local near = {}
    for b = 1, m do
      if b ~= a then
        near[#near + 1] = { dist(cities[a][1] - cities[b][1], cities[a][2] - cities[b][2]), b }
      end
    end
    table.sort(near, function(p, q)
      if p[1] ~= q[1] then return p[1] < q[1] end
      return p[2] < q[2]
    end)
    for k = 1, math.min(4, #near) do
      local b = near[k][2]
      if not (near[k][1] > 26 or b < a) then
        local c = route(a, b)
        if c > 0 then links[#links + 1] = { cost = c, a = a, b = b } end
      end
    end
  end
  local function cheaper(p, q)
    if p.cost ~= q.cost then return p.cost < q.cost end
    if p.a ~= q.a then return p.a < q.a end
    return p.b < q.b
  end
  table.sort(links, cheaper)
  -- the minimum spanning tree (Kruskal), then a few links where the tree
  -- would send a traveller the long way round
  local root = {}
  for i = 1, m do root[i] = i end
  local function find(i)
    if root[i] == i then return i end
    root[i] = find(root[i])
    return root[i]
  end
  local tree, chosen = {}, {}
  for i = 1, m do tree[i] = {} end
  local function join(l)
    chosen[#chosen + 1] = l
    tree[l.a][#tree[l.a] + 1] = { l.b, l.cost }
    tree[l.b][#tree[l.b] + 1] = { l.a, l.cost }
  end
  for _, l in ipairs(links) do
    local ra, rb = find(l.a), find(l.b)
    if not (ra == rb or l.cost > 34) then
      root[ra] = rb
      join(l)
    end
  end
  local function treeCost(a, b)
    local d = {}
    for i = 1, m do d[i] = -1 end
    local q, head = { a }, 1
    d[a] = 0
    while head <= #q do
      local i = q[head]
      head = head + 1
      for _, e in ipairs(tree[i]) do
        local j, c = e[1], e[2]
        if d[j] < 0 then d[j] = d[i] + c q[#q + 1] = j end
      end
    end
    return d[b]
  end
  for _, l in ipairs(links) do
    local t = treeCost(l.a, l.b)
    if t > 2.2 * l.cost and l.cost <= 24 and uniform(rng) < 0.7 then join(l) end
  end
  -- lay them, cheapest first, each joining what is there already
  table.sort(chosen, cheaper)
  local bridged = {}
  for _, l in ipairs(chosen) do
    local tiles, used = {}, {}
    if route(l.a, l.b, tiles, used) >= 0 then
      for _, i in ipairs(tiles) do road[i] = true end
      for _, k in ipairs(used) do bridged[k] = true end
    end
  end
  local bridges = {}
  for k = 1, #spans do if bridged[k] then bridges[#bridges + 1] = k end end
  return { road = road, bridges = bridges }
end

return M
