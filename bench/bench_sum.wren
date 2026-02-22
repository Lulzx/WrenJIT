// Sum 1 to 1,000,000 in a tight loop
var sum = 0
var i = 0
while (i < 1000000) {
    sum = sum + i
    i = i + 1
}
System.print(sum)
