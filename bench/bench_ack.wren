// Ackermann: extreme recursion depth relative to work done per frame.
class Ack {
    static ack(m, n) {
        if (m == 0) return n + 1
        if (n == 0) return Ack.ack(m - 1, 1)
        return Ack.ack(m - 1, Ack.ack(m, n - 1))
    }
}
var start = System.clock
var result = Ack.ack(3, 7)
System.print(result)
System.print("elapsed: %(System.clock - start)")
