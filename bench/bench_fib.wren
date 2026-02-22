// Recursive Fibonacci
class Fib {
    static compute(n) {
        if (n < 2) return n
        return Fib.compute(n - 1) + Fib.compute(n - 2)
    }
}
var start = System.clock
var result = Fib.compute(35)
var elapsed = System.clock - start
System.print(result)
System.print("elapsed: %(elapsed)")
