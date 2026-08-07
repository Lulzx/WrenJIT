local Deep = {}
function Deep.down(n)
  if n == 0 then return 0 end
  return 1 + Deep.down(n - 1)
end
local start = os.clock()
local total = 0
for i = 1, 2000 do
  total = total + Deep.down(1000)
end
print(total)
print(string.format("elapsed: %.6f", os.clock() - start))
