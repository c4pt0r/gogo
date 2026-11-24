package main

func upper(b []byte) {
	for i, c := range b {
		if c >= 'a' && c <= 'z' {
			b[i] = c - 32
		}
	}
}

func main() {
	b := []byte{0x47, 0x4f}
	println(string(b), len(b), b[0])
	var c byte = 200
	b[0] = c + 100 // byte arithmetic wraps: 44
	b[1] += 250    // 0x4f + 250 = 329 -> 73
	println(b[0], b[1])
	x := 300
	b[0] = byte(x) // 44
	println(b[0])
	s := "hello, world"
	bs := []byte(s)
	upper(bs[:5])
	println(string(bs), s)
	bs = append(bs, '!')
	bs = append(bs, " ok"...)
	bs = append(bs, []byte{'?', '?'}...)
	println(string(bs), len(bs))
	n := copy(bs, "HEY")
	println(n, string(bs[:6]))
	var nb []byte
	println(string(nb) == "", len(string(nb)), nb == nil)
	nb = append(nb, 'z')
	println(string(nb))
	e := []byte("")
	println(e == nil, len(e))
	for _, v := range []byte("AZ") {
		println(v)
	}
	b2 := make([]byte, 3)
	b2[1] = 255
	b2[1]++
	println(b2[0], b2[1], b2[2])
}
