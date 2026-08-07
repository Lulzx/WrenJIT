local Tak = {}
function Tak.tak(x, y, z)
  if not (y < x) then return z end
  return Tak.tak(Tak.tak(x-1, y, z), Tak.tak(y-1, z, x), Tak.tak(z-1, x, y))
end
local start = os.clock()
local result = Tak.tak(24, 16, 8)
print(result)
print(string.format("elapsed: %.6f", os.clock() - start))
