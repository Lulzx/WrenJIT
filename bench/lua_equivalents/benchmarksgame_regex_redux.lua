local bit = require("bit")
local dna, seed = {}, 42
for i = 1, 250000 do
  seed = (seed * 3877 + 29573) % 139968
  dna[i] = math.floor(seed * 4 / 139968)
end
local A,C,G,T = 1,2,4,8
local function bor(...) local v=0 for i=1,select('#',...) do v=bit.bor(v,select(i,...)) end return v end
local patterns = {
  {{A,G,G,G,T,A,A,A},{T,T,T,A,C,C,C,T}},
  {{bor(C,G,T),G,G,G,T,A,A,A},{T,T,T,A,C,C,C,bor(A,C,G)}},
  {{A,bor(A,C,T),G,G,T,A,A,A},{T,T,T,A,C,C,bor(A,G,T),T}},
  {{A,G,bor(A,C,T),G,T,A,A,A},{T,T,T,A,C,bor(A,G,T),C,T}},
  {{A,G,G,bor(A,C,T),T,A,A,A},{T,T,T,A,bor(A,G,T),C,C,T}},
  {{A,G,G,G,bor(A,C,G),A,A,A},{T,T,T,bor(C,G,T),C,C,C,T}},
  {{A,G,G,G,T,bor(C,G,T),A,A},{T,T,bor(A,C,G),A,C,C,C,T}},
  {{A,G,G,G,T,A,bor(C,G,T),A},{T,bor(A,C,G),T,A,C,C,C,T}},
  {{A,G,G,G,T,A,A,bor(C,G,T)},{bor(A,C,G),T,T,A,C,C,C,T}}
}
local start, checksum = os.clock(), 0
for _, alternatives in ipairs(patterns) do
  local count = 0
  for i = 1, #dna - 7 do
    for _, pattern in ipairs(alternatives) do
      local j = 1
      while j <= 8 and bit.band(pattern[j], bit.lshift(1, dna[i+j-1])) ~= 0 do j = j + 1 end
      if j == 9 then count = count + 1 end
    end
  end
  checksum = checksum * 31 + count
end
print("length: " .. #dna)
print("checksum: " .. checksum)
print(string.format("elapsed: %.9f", os.clock() - start))
