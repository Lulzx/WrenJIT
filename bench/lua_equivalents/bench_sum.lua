local start = os.clock()
local sum = 0
local i = 0
while i < 1000000 do
  sum = sum + i
  i = i + 1
end
print(sum)
print(string.format("elapsed: %.6f", os.clock() - start))
