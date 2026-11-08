-- Travel cost over a map: how many movement points a land army needs to get
-- from one place to every tile, under the game's own rules (docs/rules.md >
-- Movement). The random map generator's fair capital placement uses it
-- (warlords/randommap). A port of cpp/src/warlords/travel.cpp.
--
-- The model is the walker's, slightly simplified: a tile costs its terrain's
-- movement cost, any tile with a road on it costs 1 (so a road over the
-- mountains is a pass), mountains without a road are shut, and an army
-- changes between land and sea only at a crossing (a bridge, a marked tile,
-- a port city), which ends its move -- counted here as a whole turn.

local move = require("warlords.move")
local heap = require("warlords.heap")

local M = {}

M.TURN = 12                 -- movement points in a turn: a Light Infantry's
M.SEA_SWITCH = M.TURN       -- going to sea or ashore ends the move
M.UNREACHED = 2147483647    -- a tile that cannot be reached (C++'s INT_MAX)

--- Movement points from the nearest of `sources` (a list of 0-based tile
--- indices) to every tile, UNREACHED where it cannot be reached.
---
--- g: { w, h, terrain, road, crossing }, each grid 0-based, w x h: terrain
--- type ids (move.ROAD .. move.SITE), road true where a road runs, crossing
--- true where land and sea may meet.
function M.costs(g, sources)
  local w, h = g.w, g.h
  local n = w * h
  local terrain, road, crossing = g.terrain, g.road, g.crossing
  local UNREACHED = M.UNREACHED
  local dist = {}
  for i = 0, n - 1 do dist[i] = UNREACHED end
  local function sea(i) return terrain[i] == move.WATER or terrain[i] == move.SHORE end
  local function open(i) return terrain[i] ~= move.MOUNTAINS or road[i] end
  local function step(i)
    if road[i] then return 1 end
    local c = move.COST[terrain[i]]
    return c > 0 and c or 1
  end
  local q = heap.new()
  for _, s in ipairs(sources) do
    if s >= 0 and s < n and dist[s] ~= 0 then
      dist[s] = 0
      q:push(0, s)
    end
  end
  while not q:empty() do
    local d, i = q:pop()
    if d == dist[i] then
      local x = i % w
      local y = (i - x) / w
      for k = 0, 7 do
        local nx, ny = x + move.DIRS[k][1], y + move.DIRS[k][2]
        if nx >= 0 and ny >= 0 and nx < w and ny < h then
          local j = ny * w + nx
          if open(j) then
            local c = d + step(j)
            local ok = true
            if sea(i) ~= sea(j) then
              if not crossing[i] and not crossing[j] then ok = false end
              c = c + M.SEA_SWITCH
            end
            if ok and c < dist[j] then
              dist[j] = c
              q:push(c, j)
            end
          end
        end
      end
    end
  end
  return dist
end

return M
