local n, inside = 600, 0
local start = os.clock()
for y = 0, n - 1 do
  local ci = 2 * y / n - 1
  for x = 0, n - 1 do
    local cr, zr, zi, iteration = 2 * x / n - 1.5, 0, 0, 0
    while iteration < 50 and zr * zr + zi * zi <= 4 do
      local next_zr = zr * zr - zi * zi + cr
      zi, zr, iteration = 2 * zr * zi + ci, next_zr, iteration + 1
    end
    if iteration == 50 then inside = inside + 1 end
  end
end
print("inside: " .. inside)
print(string.format("elapsed: %.9f", os.clock() - start))
