// For-loop style iteration using ranges
var start = System.clock
var sum = 0
for (i in 1..1000000) {
    sum = sum + i
}
System.print(sum)
System.print("elapsed: %(System.clock - start)")
