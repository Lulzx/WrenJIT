// Benchmarks Game fannkuch-redux, n = 10.
var n = 10
var perm = []
var count = []
for (i in 0...n) {
    perm.add(i)
    count.add(0)
}
var maxFlips = 0
var checksum = 0
var r = n
var sign = 1
var done = false
var start = System.clock
while (!done) {
    while (r != 1) {
        count[r - 1] = r
        r = r - 1
    }
    var first = perm[0]
    if (first != 0) {
        var copy = perm.toList
        var flips = 0
        while (copy[0] != 0) {
            var k = copy[0]
            var lo = 0
            var hi = k
            while (lo < hi) {
                var t = copy[lo]
                copy[lo] = copy[hi]
                copy[hi] = t
                lo = lo + 1
                hi = hi - 1
            }
            flips = flips + 1
        }
        if (flips > maxFlips) maxFlips = flips
        checksum = checksum + sign * flips
    }
    var advanced = false
    while (!advanced && !done) {
        if (r == n) {
            done = true
        } else {
            var firstValue = perm[0]
            var i = 0
            while (i < r) {
                perm[i] = perm[i + 1]
                i = i + 1
            }
            perm[r] = firstValue
            count[r] = count[r] - 1
            if (count[r] > 0) {
                sign = -sign
                advanced = true
            } else {
                r = r + 1
            }
        }
    }
}
System.print("checksum: %(checksum)")
System.print("max flips: %(maxFlips)")
System.print("elapsed: %(System.clock - start)")
