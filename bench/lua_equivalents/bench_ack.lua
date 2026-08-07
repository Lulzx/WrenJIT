local Ack = {}
function Ack.ack(m, n)
  if m == 0 then return n + 1 end
  if n == 0 then return Ack.ack(m - 1, 1) end
  return Ack.ack(m - 1, Ack.ack(m, n - 1))
end
local start = os.clock()
local result = Ack.ack(3, 7)
print(result)
print(string.format("elapsed: %.6f", os.clock() - start))
