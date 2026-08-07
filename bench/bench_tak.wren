// Takeuchi function: deep 3-argument recursion, no allocation.
class Tak {
    static tak(x, y, z) {
        if (!(y < x)) return z
        return Tak.tak(
            Tak.tak(x - 1, y, z),
            Tak.tak(y - 1, z, x),
            Tak.tak(z - 1, x, y))
    }
}
var start = System.clock
var result = Tak.tak(24, 16, 8)
System.print(result)
System.print("elapsed: %(System.clock - start)")
