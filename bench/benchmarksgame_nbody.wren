// The Computer Language Benchmarks Game n-body kernel, scaled to 500,000
// time steps for repeatable local runs. The arithmetic and body constants are
// kept in the same order as the canonical benchmark.

var pi = 3.141592653589793
var solarMass = 4 * pi * pi
var daysPerYear = 365.24

var x = [0, 4.841431442464721, 8.34336671824458,
    12.894369562139131, 15.379697114850917]
var y = [0, -1.1603200440274284, 4.124798564124305,
    -15.111151401698631, -25.919314609987964]
var z = [0, -0.10362204447112311, -0.4035234171143214,
    -0.22330757889265573, 0.17925877295037118]
var vx = [0,
    0.001660076642744037 * daysPerYear,
    -0.002767425107268624 * daysPerYear,
    0.002964601375647616 * daysPerYear,
    0.0026806777249038932 * daysPerYear]
var vy = [0,
    0.007699011184197404 * daysPerYear,
    0.004998528012349172 * daysPerYear,
    0.0023784717395948095 * daysPerYear,
    0.001628241700382423 * daysPerYear]
var vz = [0,
    -0.0000690460016972063 * daysPerYear,
    0.000023041729757376393 * daysPerYear,
    -0.000029658956854023756 * daysPerYear,
    -0.00009515922545197159 * daysPerYear]
var mass = [solarMass,
    0.0009547919384243266 * solarMass,
    0.0002858859806661308 * solarMass,
    0.00004366244043351563 * solarMass,
    0.000051513890204661145 * solarMass]

var px = 0
var py = 0
var pz = 0
for (i in 0...5) {
    px = px + vx[i] * mass[i]
    py = py + vy[i] * mass[i]
    pz = pz + vz[i] * mass[i]
}
vx[0] = -px / solarMass
vy[0] = -py / solarMass
vz[0] = -pz / solarMass

var energy = Fn.new {
    var e = 0
    for (i in 0...5) {
        e = e + 0.5 * mass[i] *
            (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i])
        for (j in (i + 1)...5) {
            var dx = x[i] - x[j]
            var dy = y[i] - y[j]
            var dz = z[i] - z[j]
            e = e - mass[i] * mass[j] /
                (dx * dx + dy * dy + dz * dz).sqrt
        }
    }
    return e
}

System.print("before: %(energy.call())")
var start = System.clock
var step = 0
while (step < 500000) {
    var i = 0
    while (i < 5) {
        var j = i + 1
        while (j < 5) {
            var dx = x[i] - x[j]
            var dy = y[i] - y[j]
            var dz = z[i] - z[j]
            var distanceSquared = dx * dx + dy * dy + dz * dz
            var magnitude = 0.01 /
                (distanceSquared * distanceSquared.sqrt)
            vx[i] = vx[i] - dx * mass[j] * magnitude
            vy[i] = vy[i] - dy * mass[j] * magnitude
            vz[i] = vz[i] - dz * mass[j] * magnitude
            vx[j] = vx[j] + dx * mass[i] * magnitude
            vy[j] = vy[j] + dy * mass[i] * magnitude
            vz[j] = vz[j] + dz * mass[i] * magnitude
            j = j + 1
        }
        i = i + 1
    }
    i = 0
    while (i < 5) {
        x[i] = x[i] + 0.01 * vx[i]
        y[i] = y[i] + 0.01 * vy[i]
        z[i] = z[i] + 0.01 * vz[i]
        i = i + 1
    }
    step = step + 1
}
var elapsed = System.clock - start
System.print("after: %(energy.call())")
System.print("elapsed: %(elapsed)")
