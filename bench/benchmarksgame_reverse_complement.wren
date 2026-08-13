// Benchmarks Game reverse-complement over deterministic encoded DNA.
var dna = []
var seed = 42
for (i in 0...1000000) {
    seed = (seed * 3877 + 29573) % 139968
    dna.add((seed * 4 / 139968).floor)
}
var length = dna.count
var start = System.clock
var lo = 0
var hi = length - 1
while (lo <= hi) {
    var left = 3 - dna[hi]
    var right = 3 - dna[lo]
    dna[lo] = left
    dna[hi] = right
    lo = lo + 1
    hi = hi - 1
}
var checksum = 0
var i = 0
while (i < length) {
    checksum = (checksum * 5 + dna[i]) % 1000000007
    i = i + 1
}
System.print("length: %(length)")
System.print("checksum: %(checksum)")
System.print("elapsed: %(System.clock - start)")
