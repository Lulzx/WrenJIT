local codes = {97,99,103,116,66,68,72,75,77,78,82,83,86,87,89}
local cumulative = {.27,.39,.51,.78,.80,.82,.84,.86,.88,.90,.92,.94,.96,.98,1}
local seed, checksum = 42, 0
local start = os.clock()
for _ = 1, 500000 do
  seed = (seed * 3877 + 29573) % 139968
  local r, j = seed / 139968, 1
  while r >= cumulative[j] do j = j + 1 end
  checksum = (checksum + codes[j]) % 1000000007
end
print("length: 500000")
print("checksum: " .. checksum)
print(string.format("elapsed: %.9f", os.clock() - start))
