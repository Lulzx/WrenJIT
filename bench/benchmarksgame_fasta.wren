// Benchmarks Game FASTA random generator. Output is reduced to a checksum so
// terminal throughput does not dominate this local CPU comparison.
var codes = [97, 99, 103, 116, 66, 68, 72, 75, 77, 78, 82, 83, 86, 87, 89]
var cumulative = [0.27, 0.39, 0.51, 0.78, 0.80, 0.82, 0.84, 0.86,
    0.88, 0.90, 0.92, 0.94, 0.96, 0.98, 1]
var seed = 42
var checksum = 0
var start = System.clock
var i = 0
while (i < 500000) {
    seed = (seed * 3877 + 29573) % 139968
    var r = seed / 139968
    var j = 0
    while (r >= cumulative[j]) { j = j + 1 }
    checksum = (checksum + codes[j]) % 1000000007
    i = i + 1
}
System.print("length: %(i)")
System.print("checksum: %(checksum)")
System.print("elapsed: %(System.clock - start)")
