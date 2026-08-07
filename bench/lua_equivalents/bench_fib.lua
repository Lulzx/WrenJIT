local Fib = {}
function Fib.compute(n)
  if n < 2 then return n end
  return Fib.compute(n - 1) + Fib.compute(n - 2)
end
local start = os.clock()
local result = Fib.compute(35)
print(result)
print(string.format("elapsed: %.6f", os.clock() - start))
