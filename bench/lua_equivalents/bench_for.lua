local start = os.clock()
local sum = 0
for i = 1, 1000000 do
  sum = sum + i
end
print(sum)
print(string.format("elapsed: %.6f", os.clock() - start))
