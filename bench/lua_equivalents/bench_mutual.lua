local Parity = {}
function Parity.isEven(n)
  if n == 0 then return true end
  return Parity.isOdd(n - 1)
end
function Parity.isOdd(n)
  if n == 0 then return false end
  return Parity.isEven(n - 1)
end
local start = os.clock()
local count = 0
for i = 1, 2000 do
  if Parity.isEven(500) then count = count + 1 end
end
print(count)
print(string.format("elapsed: %.6f", os.clock() - start))
