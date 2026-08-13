local digits = 2000
local boxes, a = math.floor(digits * 10 / 3) + 1, {}
for i = 1, boxes do a[i] = 2 end
local predigit, nines, checksum, produced = 0, 0, 0, 0
local start = os.clock()
for _ = 1, digits do
  local q = 0
  for i = boxes, 1, -1 do
    local x, denominator = 10 * a[i] + q * i, 2 * i - 1
    a[i], q = x % denominator, math.floor(x / denominator)
  end
  a[1], q = q % 10, math.floor(q / 10)
  if q == 9 then nines = nines + 1
  elseif q == 10 then
    checksum, produced = (checksum * 10 + predigit + 1) % 1000000007, produced + 1
    for _ = 1, nines do checksum, produced = checksum * 10 % 1000000007, produced + 1 end
    predigit, nines = 0, 0
  else
    checksum, produced = (checksum * 10 + predigit) % 1000000007, produced + 1
    for _ = 1, nines do checksum, produced = (checksum * 10 + 9) % 1000000007, produced + 1 end
    predigit, nines = q, 0
  end
end
checksum, produced = (checksum * 10 + predigit) % 1000000007, produced + 1
print("digits: " .. produced)
print("checksum: " .. checksum)
print(string.format("elapsed: %.9f", os.clock() - start))
