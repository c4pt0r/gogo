package main

type Px struct{ V byte }

func inc(c byte) byte { return c + 1 }

func split(x int) (byte, byte) { return byte(x / 256), byte(x) }

func main() {
	x := 300
	println(byte(x))
	var c byte = 250
	c += 10
	println(c)
	println(int(c) + 1000)
	c--
	c -= 5
	println(c)
	println(inc(255))
	var d byte
	d = c * 2
	println(d, -d)
	println(c > d, c == 254, d/5, d%5)
	hi, lo := split(0x1234)
	println(hi, lo)
	var big int64 = 1<<40 + 7
	println(byte(big), int64(c))
	p := Px{V: byte(x + 1)}
	println(p.V)
	m := -1
	println(byte(m))
	var u uint8 = 255
	u++
	println(u)
}
