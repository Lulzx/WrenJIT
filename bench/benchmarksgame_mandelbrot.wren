// Benchmarks Game Mandelbrot kernel. Count set pixels instead of writing PBM.
var n = 600
var inside = 0
var start = System.clock
var y = 0
while (y < n) {
    var ci = 2 * y / n - 1
    var x = 0
    while (x < n) {
        var cr = 2 * x / n - 1.5
        var zr = 0
        var zi = 0
        var iteration = 0
        while (iteration < 50 && zr * zr + zi * zi <= 4) {
            var next = zr * zr - zi * zi + cr
            zi = 2 * zr * zi + ci
            zr = next
            iteration = iteration + 1
        }
        if (iteration == 50) inside = inside + 1
        x = x + 1
    }
    y = y + 1
}
System.print("inside: %(inside)")
System.print("elapsed: %(System.clock - start)")
