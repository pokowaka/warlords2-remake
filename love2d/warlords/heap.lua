-- A binary heap of (cost, tile) pairs that pops the smallest pair first,
-- cost then tile: what C++'s std::priority_queue<pair<cost, tile>,
-- vector<...>, greater<...>> pops (warlords/travel, warlords/scengen).
--
-- The order is a total one -- no two pairs tie unless they are the same --
-- so any correct heap pops them in the same order as the C++ one, and the
-- ports make the same worlds from the same seed.

local heap = {}
heap.__index = heap

function heap.new()
  return setmetatable({ cost = {}, tile = {}, n = 0 }, heap)
end

local function less(c1, t1, c2, t2)
  return c1 < c2 or (c1 == c2 and t1 < t2)
end

function heap:empty() return self.n == 0 end

function heap:push(c, t)
  local cost, tile = self.cost, self.tile
  local i = self.n + 1
  self.n = i
  while i > 1 do
    local p = math.floor(i / 2)
    if not less(c, t, cost[p], tile[p]) then break end
    cost[i], tile[i] = cost[p], tile[p]
    i = p
  end
  cost[i], tile[i] = c, t
end

--- The smallest pair, taken off the heap: cost, tile.
function heap:pop()
  local cost, tile = self.cost, self.tile
  local n = self.n
  local topC, topT = cost[1], tile[1]
  local c, t = cost[n], tile[n]
  cost[n], tile[n] = nil, nil
  n = n - 1
  self.n = n
  if n > 0 then
    local i = 1
    while true do
      local k = 2 * i
      if k > n then break end
      if k < n and less(cost[k + 1], tile[k + 1], cost[k], tile[k]) then k = k + 1 end
      if not less(cost[k], tile[k], c, t) then break end
      cost[i], tile[i] = cost[k], tile[k]
      i = k
    end
    cost[i], tile[i] = c, t
  end
  return topC, topT
end

return heap
