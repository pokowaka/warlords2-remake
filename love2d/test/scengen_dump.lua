-- The scenario generator's worlds, written out for comparison with the C++
-- and JavaScript ports (cpp/test/scengen_compare.sh):
--
--     luajit love2d/test/scengen_dump.lua DIR SHAPE FIRST COUNT
--
-- writes, for seeds FIRST .. FIRST+COUNT-1, DIR/<shape>-<seed>/RANDOM.<ext>
-- for every file the generator makes, and TITLE.TXT with the world's name
-- and line -- what `bazel-bin/cpp/w2scengen --dump DIR --shape SHAPE --seed
-- FIRST --count COUNT` writes, byte for byte.

package.path = "love2d/?.lua;" .. package.path

local randommap = require("warlords.randommap")
local rng       = require("warlords.rng")
local scengen   = require("warlords.scengen")

local dir, shapeArg, first, count = arg[1], arg[2], tonumber(arg[3] or "1"), tonumber(arg[4] or "1")
local DATA = "original"
local shape = shapeArg and scengen.parseShape(shapeArg)
if not dir or not shape or not first or not count then
  io.stderr:write("usage: luajit love2d/test/scengen_dump.lua DIR continents|pangaea|islands FIRST COUNT\n")
  os.exit(2)
end

local function write(path, bytes)
  local f = assert(io.open(path, "wb"))
  f:write(bytes)
  f:close()
end

for seed = first, first + count - 1 do
  local files = randommap.generateNow({
    dataDir = DATA, rng = rng.new(seed), scenario = true, world = { shape = shape },
  })
  local out = ("%s/%s-%d"):format(dir, scengen.shapeName(shape), seed)
  os.execute(("mkdir -p '%s'"):format(out))
  for ext, bytes in pairs(files) do write(("%s/%s.%s"):format(out, randommap.DIR, ext), bytes) end
  local name, text = scengen.title(shape, seed)
  write(out .. "/TITLE.TXT", name .. "\n" .. text .. "\n")
end
