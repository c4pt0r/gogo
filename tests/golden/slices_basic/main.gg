// Expected output: real Go's, except cap() after growth (gogo grows x2,
// min 4; Go rounds to size classes) and the last, gogo-only line.
package main

type Point struct {
	X, Y int
}

type Bag struct {
	Name  string
	Items []int
}

func sum(s []int) int {
	t := 0
	for _, v := range s {
		t += v
	}
	return t
}

func evens(n int) []int {
	if n == 0 {
		return nil
	}
	var out []int
	for i := 0; i < n; i++ {
		out = append(out, i*2)
	}
	return out
}

func show(s []int) {
	println(len(s), cap(s), s == nil, sum(s))
}

func reverse(s []int) {
	for i, j := 0, len(s)-1; i < j; i, j = i+1, j-1 {
		s[i], s[j] = s[j], s[i]
	}
}

func main() {
	// literals, index, len/cap
	a := []int{3, 1, 4, 1, 5}
	println(len(a), cap(a), a[0], a[4])
	a[1] = 10
	a[2] += 5
	a[3]++
	a[4]--
	println(a[1], a[2], a[3], a[4], sum(a))

	// nil slices
	var n []int
	show(n)
	println(n == nil, n != nil, a == nil, a != nil)
	n = append(n, 7)
	show(n)
	n = nil
	show(n)
	show(evens(0))
	show(evens(5))
	e := []int{}
	println(e == nil, len(e))

	// make
	m := make([]int, 3)
	show(m)
	m2 := make([]string, 2, 5)
	println(len(m2), cap(m2), m2[1] == "", len(m2[:5][4]))

	// append: one, several, spread
	var b []int
	b = append(b, 1)
	b = append(b, 2, 3, 4)
	b = append(b, a...)
	b = append(b)
	show(b)
	b = append(b[:2], b[5:]...)
	show(b)

	// slicing
	c := b[1:3]
	println(len(c), cap(c), c[0], c[1])
	d := b[2:]
	println(len(d), d[0])
	f := b[:2]
	println(len(f), f[1])
	g := b[:]
	println(len(g), sum(g))

	// range forms
	for i := range c {
		println("i", i)
	}
	for i, v := range c {
		println("iv", i, v)
	}
	for _, v := range []string{"x", "y"} {
		println("v", v)
	}
	k := 0
	for range a {
		k++
	}
	println(k)

	// copy
	dst := make([]int, 3)
	println(copy(dst, a), dst[0], dst[1], dst[2])
	println(copy(dst, []int{9}), dst[0], dst[1])
	reverse(a)
	println(a[0], a[1], a[2], a[3], a[4])

	// strings, structs, int64, any, nested slices
	words := []string{"go", "go", "gadget"}
	words[1] = words[1] + "!"
	println(words[0], words[1], words[2], len(words))
	pts := []Point{{1, 2}, Point{X: 3}}
	pts = append(pts, Point{5, 6})
	println(pts[0].X, pts[0].Y, pts[1].X, pts[1].Y, pts[2].Y)
	big := []int64{1 << 40, 5}
	big[1] = big[0] * 2
	println(big[0], big[1])
	var anys []any
	anys = append(anys, 1, "two", int64(3), Point{4, 4})
	for _, x := range anys {
		switch v := x.(type) {
		case int:
			println("int", v)
		case string:
			println("string", v)
		case int64:
			println("int64", v)
		case Point:
			println("point", v.X)
		}
	}
	grid := [][]int{{1, 2}, {3}}
	grid = append(grid, []int{4, 5, 6})
	grid[0][1] = 20
	println(len(grid), len(grid[2]), grid[0][1], grid[2][2])
	bag := Bag{Name: "bag", Items: []int{1, 2}}
	bag.Items[0] = 100
	println(bag.Name, bag.Items[0], len(bag.Items))
	var empty Bag
	println(empty.Items == nil, len(empty.Items))
	// gogo-only: println of a struct shows its slices' elements (real Go
	// rejects println of a struct).
	println(bag, empty, Holder{[]byte("hi"), [][]string{{"a"}, nil}})
}

type Holder struct {
	B []byte
	S [][]string
}
