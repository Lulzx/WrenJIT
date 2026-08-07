class Box {
  construct new(value) { _value = value }
  toggle { _value = !_value }
}
var box = Box.new(false)
var i = 0
while (i < 10000000) {
  box.toggle
  i = i + 1
}
System.print(i)
