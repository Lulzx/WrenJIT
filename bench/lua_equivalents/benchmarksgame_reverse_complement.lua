local dna, seed = {}, 42
for i = 1, 1000000 do
  seed = (seed * 3877 + 29573) % 139968
  dna[i] = math.floor(seed * 4 / 139968)
end
local length = #dna
local start, lo, hi = os.clock(), 1, length
while lo <= hi do
  dna[lo], dna[hi] = 3 - dna[hi], 3 - dna[lo]
  lo, hi = lo + 1, hi - 1
end
local checksum, i = 0, 1
while i <= length do
  checksum = (checksum * 5 + dna[i]) % 1000000007
  i = i + 1
end
print("length: " .. length)
print("checksum: " .. checksum)
print(string.format("elapsed: %.9f", os.clock() - start))
