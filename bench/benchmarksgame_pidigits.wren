// Array-based spigot variant of Benchmarks Game pidigits. This avoids relying
// on a bignum extension, which neither stock Wren nor stock LuaJIT provides.
var digits = 2000
var boxes = (digits * 10 / 3).floor + 1
var a = []
for (i in 0...boxes) { a.add(2) }
var predigit = 0
var nines = 0
var checksum = 0
var produced = 0
var start = System.clock
for (iteration in 0...digits) {
    var q = 0
    var i = boxes
    while (i > 0) {
        var x = 10 * a[i - 1] + q * i
        var denominator = 2 * i - 1
        a[i - 1] = x % denominator
        q = (x / denominator).floor
        i = i - 1
    }
    a[0] = q % 10
    q = (q / 10).floor
    if (q == 9) {
        nines = nines + 1
    } else if (q == 10) {
        checksum = (checksum * 10 + predigit + 1) % 1000000007
        produced = produced + 1
        for (j in 0...nines) {
            checksum = (checksum * 10) % 1000000007
            produced = produced + 1
        }
        predigit = 0
        nines = 0
    } else {
        checksum = (checksum * 10 + predigit) % 1000000007
        produced = produced + 1
        for (j in 0...nines) {
            checksum = (checksum * 10 + 9) % 1000000007
            produced = produced + 1
        }
        predigit = q
        nines = 0
    }
}
checksum = (checksum * 10 + predigit) % 1000000007
produced = produced + 1
System.print("digits: %(produced)")
System.print("checksum: %(checksum)")
System.print("elapsed: %(System.clock - start)")
