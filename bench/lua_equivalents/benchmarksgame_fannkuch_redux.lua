local n, perm, count = 10, {}, {}
for i = 1, n do perm[i], count[i] = i - 1, 0 end
local max_flips, checksum, r, sign, done = 0, 0, n, 1, false
local start = os.clock()
while not done do
  while r ~= 1 do count[r] = r r = r - 1 end
  if perm[1] ~= 0 then
    local copy = {unpack(perm)}
    local flips = 0
    while copy[1] ~= 0 do
      local lo, hi = 1, copy[1] + 1
      while lo < hi do copy[lo], copy[hi] = copy[hi], copy[lo] lo, hi = lo + 1, hi - 1 end
      flips = flips + 1
    end
    if flips > max_flips then max_flips = flips end
    checksum = checksum + sign * flips
  end
  local advanced = false
  while not advanced and not done do
    if r == n then done = true else
      local first = perm[1]
      for i = 1, r do perm[i] = perm[i + 1] end
      perm[r + 1] = first
      count[r + 1] = count[r + 1] - 1
      if count[r + 1] > 0 then sign, advanced = -sign, true else r = r + 1 end
    end
  end
end
print("checksum: " .. checksum)
print("max flips: " .. max_flips)
print(string.format("elapsed: %.9f", os.clock() - start))
