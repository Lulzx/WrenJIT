// Recursion plus allocation: exercises the GC alongside the call path.
class Node {
    construct new(left, right) {
        _left = left
        _right = right
    }
    left { _left }
    right { _right }
    check {
        if (_left == null) return 1
        return 1 + _left.check + _right.check
    }
}
class Trees {
    static make(depth) {
        if (depth == 0) return Node.new(null, null)
        return Node.new(Trees.make(depth - 1), Trees.make(depth - 1))
    }
}
var start = System.clock
var total = 0
for (i in 1..12) {
    total = total + Trees.make(14).check
}
System.print(total)
System.print("elapsed: %(System.clock - start)")
