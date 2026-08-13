-- The Computer Language Benchmarks Game spectral-norm kernel, scaled to a
-- 500-element matrix for repeatable local runs.

local n = 500
local u, v, tmp = {}, {}, {}
for i = 1, n do
  u[i], v[i], tmp[i] = 1, 0, 0
end

local function multiply_at_av(input, output)
  for i = 0, n - 1 do
    local sum = 0
    for j = 0, n - 1 do
      local ij = i + j
      sum = sum + input[j + 1] /
        (math.floor(ij * (ij + 1) / 2) + i + 1)
    end
    tmp[i + 1] = sum
  end
  for i = 0, n - 1 do
    local sum = 0
    for j = 0, n - 1 do
      local ij = i + j
      sum = sum + tmp[j + 1] /
        (math.floor(ij * (ij + 1) / 2) + j + 1)
    end
    output[i + 1] = sum
  end
end

local start = os.clock()
for _ = 1, 10 do
  multiply_at_av(u, v)
  multiply_at_av(v, u)
end
local vbv, vv = 0, 0
for i = 1, n do
  vbv = vbv + u[i] * v[i]
  vv = vv + v[i] * v[i]
end
local result = math.sqrt(vbv / vv)
local elapsed = os.clock() - start
print("result: " .. result)
print(string.format("elapsed: %.9f", elapsed))
