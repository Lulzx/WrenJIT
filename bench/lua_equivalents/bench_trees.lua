local Node = {}
Node.__index = Node
function Node.new(left, right)
  return setmetatable({left = left, right = right}, Node)
end
function Node:check()
  if self.left == nil then return 1 end
  return 1 + self.left:check() + self.right:check()
end
local Trees = {}
function Trees.make(depth)
  if depth == 0 then return Node.new(nil, nil) end
  return Node.new(Trees.make(depth - 1), Trees.make(depth - 1))
end
local start = os.clock()
local total = 0
for i = 1, 12 do
  total = total + Trees.make(14):check()
end
print(total)
print(string.format("elapsed: %.6f", os.clock() - start))
