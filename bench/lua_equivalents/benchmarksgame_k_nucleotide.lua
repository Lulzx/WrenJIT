local dna, seed = {}, 42
for i = 1, 250000 do
  seed = (seed * 3877 + 29573) % 139968
  dna[i] = math.floor(seed * 4 / 139968)
end
local start, checksum = os.clock(), 0
for _, k in ipairs({1,2,3,4,6,12,18}) do
  local counts, factor, key = {}, 4 ^ (k - 1), 0
  for j = 1, k do key = key * 4 + dna[j] end
  counts[key] = 1
  for i = k + 1, #dna do
    key = (key % factor) * 4 + dna[i]
    counts[key] = (counts[key] or 0) + 1
  end
  for _, value in pairs(counts) do checksum = (checksum + value * k) % 1000000007 end
end
print("length: " .. #dna)
print("checksum: " .. checksum)
print(string.format("elapsed: %.9f", os.clock() - start))
