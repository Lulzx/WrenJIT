local function tree(depth)
  if depth == 0 then return false end
  return {tree(depth - 1), tree(depth - 1)}
end
local function check(node)
  if not node then return 1 end
  return 1 + check(node[1]) + check(node[2])
end
local min_depth, max_depth = 4, 14
local start = os.clock()
local stretch, long_lived = check(tree(max_depth + 1)), tree(max_depth)
local total = 0
for depth = min_depth, max_depth, 2 do
  local iterations = 2 ^ (max_depth - depth + min_depth)
  local sum = 0
  for _ = 1, iterations do sum = sum + check(tree(depth)) end
  total = total + sum
end
print("check: " .. stretch + total + check(long_lived))
print(string.format("elapsed: %.9f", os.clock() - start))
