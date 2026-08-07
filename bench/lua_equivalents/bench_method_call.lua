local Toggle = {}
Toggle.__index = Toggle
function Toggle.new(start) return setmetatable({state = start}, Toggle) end
function Toggle:value() return self.state end
function Toggle:activate()
  self.state = not self.state
  return self
end
local start = os.clock()
local t = Toggle.new(true)
local n = 0
for i = 1, 3000000 do
  if t:activate():value() then n = n + 1 end
end
print(n)
print(string.format("elapsed: %.6f", os.clock() - start))
