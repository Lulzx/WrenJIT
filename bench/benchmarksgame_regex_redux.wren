// The nine Benchmarks Game regex-redux variant patterns, represented as base
// masks because Wren has no regular-expression module. Lua uses the same matcher.
var dna = []
var seed = 42
for (i in 0...250000) {
    seed = (seed * 3877 + 29573) % 139968
    dna.add((seed * 4 / 139968).floor)
}
var A = 1
var C = 2
var G = 4
var T = 8
var patterns = [
    [[A,G,G,G,T,A,A,A], [T,T,T,A,C,C,C,T]],
    [[C|G|T,G,G,G,T,A,A,A], [T,T,T,A,C,C,C,A|C|G]],
    [[A,A|C|T,G,G,T,A,A,A], [T,T,T,A,C,C,A|G|T,T]],
    [[A,G,A|C|T,G,T,A,A,A], [T,T,T,A,C,A|G|T,C,T]],
    [[A,G,G,A|C|T,T,A,A,A], [T,T,T,A,A|G|T,C,C,T]],
    [[A,G,G,G,A|C|G,A,A,A], [T,T,T,C|G|T,C,C,C,T]],
    [[A,G,G,G,T,C|G|T,A,A], [T,T,A|C|G,A,C,C,C,T]],
    [[A,G,G,G,T,A,C|G|T,A], [T,A|C|G,T,A,C,C,C,T]],
    [[A,G,G,G,T,A,A,C|G|T], [A|C|G,T,T,A,C,C,C,T]]
]
var start = System.clock
var checksum = 0
for (alternatives in patterns) {
    var count = 0
    var i = 0
    while (i <= dna.count - 8) {
        for (pattern in alternatives) {
            var j = 0
            while (j < 8 && (pattern[j] & (1 << dna[i + j])) != 0) { j = j + 1 }
            if (j == 8) count = count + 1
        }
        i = i + 1
    }
    checksum = checksum * 31 + count
}
System.print("length: %(dna.count)")
System.print("checksum: %(checksum)")
System.print("elapsed: %(System.clock - start)")
