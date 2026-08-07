// Isolates method dispatch cost: no recursion, no arithmetic beyond a counter.
class Toggle {
    construct new(start) { _state = start }
    value { _state }
    activate {
        _state = !_state
        return this
    }
}
var start = System.clock
var t = Toggle.new(true)
var n = 0
for (i in 1..3000000) {
    if (t.activate.value) n = n + 1
}
System.print(n)
System.print("elapsed: %(System.clock - start)")
