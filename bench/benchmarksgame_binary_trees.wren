// Benchmarks Game binary-trees, max depth 14.
class GameTree {
    construct new(depth) {
        if (depth > 0) {
            _left = GameTree.new(depth - 1)
            _right = GameTree.new(depth - 1)
        }
    }
    check {
        if (_left == null) return 1
        return 1 + _left.check + _right.check
    }
}
var minDepth = 4
var maxDepth = 14
var start = System.clock
var stretch = GameTree.new(maxDepth + 1).check
var longLived = GameTree.new(maxDepth)
var total = 0
var depth = minDepth
while (depth <= maxDepth) {
    var iterations = 1
    for (i in 0...(maxDepth - depth + minDepth)) { iterations = iterations * 2 }
    var check = 0
    for (i in 0...iterations) { check = check + GameTree.new(depth).check }
    total = total + check
    depth = depth + 2
}
System.print("check: %(stretch + total + longLived.check)")
System.print("elapsed: %(System.clock - start)")
