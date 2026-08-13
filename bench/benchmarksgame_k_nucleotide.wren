// Benchmarks Game k-nucleotide counting over a deterministic 250k-base input.
var dna = []
var seed = 42
for (i in 0...250000) {
    seed = (seed * 3877 + 29573) % 139968
    dna.add((seed * 4 / 139968).floor)
}
var start = System.clock
var checksum = 0
for (k in [1, 2, 3, 4, 6, 12, 18]) {
    var counts = {}
    var factor = 1
    for (j in 1...k) { factor = factor * 4 }
    var key = 0
    for (j in 0...k) { key = key * 4 + dna[j] }
    counts[key] = 1
    var i = k
    while (i < dna.count) {
        key = (key % factor) * 4 + dna[i]
        var old = counts[key]
        counts[key] = old == null ? 1 : old + 1
        i = i + 1
    }
    for (value in counts.values) { checksum = (checksum + value * k) % 1000000007 }
}
System.print("length: %(dna.count)")
System.print("checksum: %(checksum)")
System.print("elapsed: %(System.clock - start)")
