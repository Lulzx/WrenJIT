// The Computer Language Benchmarks Game spectral-norm kernel, scaled to a
// 500-element matrix for repeatable local runs.

var n = 500
var u = []
var v = []
var tmp = []
for (i in 0...n) {
    u.add(1)
    v.add(0)
    tmp.add(0)
}

var multiplyAtAv = Fn.new { |input, output|
    var i = 0
    while (i < n) {
        var sum = 0
        var j = 0
        while (j < n) {
            var ij = i + j
            sum = sum + input[j] /
                (((ij * (ij + 1)) / 2).floor + i + 1)
            j = j + 1
        }
        tmp[i] = sum
        i = i + 1
    }
    i = 0
    while (i < n) {
        var sum = 0
        var j = 0
        while (j < n) {
            var ij = i + j
            sum = sum + tmp[j] /
                (((ij * (ij + 1)) / 2).floor + j + 1)
            j = j + 1
        }
        output[i] = sum
        i = i + 1
    }
}

var start = System.clock
for (iteration in 0...10) {
    multiplyAtAv.call(u, v)
    multiplyAtAv.call(v, u)
}
var vBv = 0
var vv = 0
for (i in 0...n) {
    vBv = vBv + u[i] * v[i]
    vv = vv + v[i] * v[i]
}
var result = (vBv / vv).sqrt
var elapsed = System.clock - start
System.print("result: %(result)")
System.print("elapsed: %(elapsed)")
