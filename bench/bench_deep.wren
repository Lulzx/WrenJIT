// Linear deep recursion: one call per frame, tests raw call/return cost.
class Deep {
    static down(n) {
        if (n == 0) return 0
        return 1 + Deep.down(n - 1)
    }
}
var start = System.clock
var total = 0
for (i in 1..2000) {
    total = total + Deep.down(1000)
}
System.print(total)
System.print("elapsed: %(System.clock - start)")
