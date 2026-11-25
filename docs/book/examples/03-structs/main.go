// Structs are values. There are no methods and no field assignment: write
// functions that take a struct and return a new one.
package main

type Point struct{ X, Y int }

type Rect struct {
	Min, Max Point
	Name     string
}

// move is what would be a method in Go: it returns a moved copy.
func move(p Point, dx, dy int) Point {
	return Point{p.X + dx, p.Y + dy}
}

func area(r Rect) int {
	return (r.Max.X - r.Min.X) * (r.Max.Y - r.Min.Y)
}

// grow returns r with Max moved, keeping the other fields.
func grow(r Rect, d int) Rect {
	return Rect{Min: r.Min, Max: move(r.Max, d, d), Name: r.Name}
}

func main() {
	p := Point{1, 2}
	q := move(p, 10, 0)
	println(p.X, q.X, q) // p is unchanged: structs are values

	r := Rect{Min: Point{0, 0}, Max: Point{3, 4}, Name: "box"}
	println(r.Name, area(r))
	r = grow(r, 1) // "updating" a field: build a new value and reassign
	println(r.Name, area(r), r)

	// The zero value of a struct has every field zero.
	var z Rect
	println(z.Max.X, z.Name == "")

	// Compare structs field by field: == on structs is not in the subset.
	println(samePoint(p, Point{1, 2}))
}

func samePoint(a, b Point) bool { return a.X == b.X && a.Y == b.Y }
