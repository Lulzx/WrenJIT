// Mutual recursion across two static methods: defeats single-function inlining.
class Parity {
    static isEven(n) {
        if (n == 0) return true
        return Parity.isOdd(n - 1)
    }
    static isOdd(n) {
        if (n == 0) return false
        return Parity.isEven(n - 1)
    }
}
var start = System.clock
var count = 0
for (i in 1..2000) {
    if (Parity.isEven(500)) count = count + 1
}
System.print(count)
System.print("elapsed: %(System.clock - start)")
