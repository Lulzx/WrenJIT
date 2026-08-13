-- The Computer Language Benchmarks Game n-body kernel, scaled to 500,000
-- time steps for repeatable local runs.

local pi = 3.141592653589793
local solar_mass = 4 * pi * pi
local days_per_year = 365.24

local x = {0, 4.841431442464721, 8.34336671824458,
  12.894369562139131, 15.379697114850917}
local y = {0, -1.1603200440274284, 4.124798564124305,
  -15.111151401698631, -25.919314609987964}
local z = {0, -0.10362204447112311, -0.4035234171143214,
  -0.22330757889265573, 0.17925877295037118}
local vx = {0,
  0.001660076642744037 * days_per_year,
  -0.002767425107268624 * days_per_year,
  0.002964601375647616 * days_per_year,
  0.0026806777249038932 * days_per_year}
local vy = {0,
  0.007699011184197404 * days_per_year,
  0.004998528012349172 * days_per_year,
  0.0023784717395948095 * days_per_year,
  0.001628241700382423 * days_per_year}
local vz = {0,
  -0.0000690460016972063 * days_per_year,
  0.000023041729757376393 * days_per_year,
  -0.000029658956854023756 * days_per_year,
  -0.00009515922545197159 * days_per_year}
local mass = {solar_mass,
  0.0009547919384243266 * solar_mass,
  0.0002858859806661308 * solar_mass,
  0.00004366244043351563 * solar_mass,
  0.000051513890204661145 * solar_mass}

local px, py, pz = 0, 0, 0
for i = 1, 5 do
  px = px + vx[i] * mass[i]
  py = py + vy[i] * mass[i]
  pz = pz + vz[i] * mass[i]
end
vx[1], vy[1], vz[1] = -px / solar_mass, -py / solar_mass,
  -pz / solar_mass

local function energy()
  local e = 0
  for i = 1, 5 do
    e = e + 0.5 * mass[i] *
      (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i])
    for j = i + 1, 5 do
      local dx, dy, dz = x[i] - x[j], y[i] - y[j], z[i] - z[j]
      e = e - mass[i] * mass[j] / math.sqrt(dx * dx + dy * dy + dz * dz)
    end
  end
  return e
end

print("before: " .. energy())
local start = os.clock()
for _ = 1, 500000 do
  for i = 1, 5 do
    for j = i + 1, 5 do
      local dx, dy, dz = x[i] - x[j], y[i] - y[j], z[i] - z[j]
      local distance_squared = dx * dx + dy * dy + dz * dz
      local magnitude = 0.01 /
        (distance_squared * math.sqrt(distance_squared))
      vx[i] = vx[i] - dx * mass[j] * magnitude
      vy[i] = vy[i] - dy * mass[j] * magnitude
      vz[i] = vz[i] - dz * mass[j] * magnitude
      vx[j] = vx[j] + dx * mass[i] * magnitude
      vy[j] = vy[j] + dy * mass[i] * magnitude
      vz[j] = vz[j] + dz * mass[i] * magnitude
    end
  end
  for i = 1, 5 do
    x[i] = x[i] + 0.01 * vx[i]
    y[i] = y[i] + 0.01 * vy[i]
    z[i] = z[i] + 0.01 * vz[i]
  end
end
local elapsed = os.clock() - start
print("after: " .. energy())
print(string.format("elapsed: %.9f", elapsed))
