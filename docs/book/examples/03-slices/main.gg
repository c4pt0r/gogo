// Slices: literals, make, append, copy, slicing and sharing, nil slices.
// Slices are the only collection: there are no arrays and no maps.
package main

type Entry struct {
	Key   string
	Value int
}

// lookup is a linear search over a slice of pairs, the usual stand-in for
// a small map.
func lookup(tbl []Entry, key string) (int, bool) {
	for _, e := range tbl {
		if e.Key == key {
			return e.Value, true
		}
	}
	return 0, false
}

// set returns tbl with key set to v.
func set(tbl []Entry, key string, v int) []Entry {
	for i, e := range tbl {
		if e.Key == key {
			tbl[i] = Entry{key, v}
			return tbl
		}
	}
	return append(tbl, Entry{key, v})
}

func main() {
	var s []int // nil
	println(s == nil, len(s))
	for i := 0; i < 5; i++ {
		s = append(s, i*i)
	}
	println(len(s), s[4])

	// Slicing shares the array: a write through one view shows in the other.
	mid := s[1:3]
	mid[0] = 100
	println(s[1], len(mid), cap(mid))

	// copy copies; make sets the length and capacity.
	c := make([]int, 2, 8)
	n := copy(c, s)
	println(n, c[0], c[1], cap(c))

	// []byte and string convert both ways (each conversion copies).
	b := []byte("gogo")
	b[0] = 'G'
	println(string(b))

	var tbl []Entry
	tbl = set(tbl, "apples", 3)
	tbl = set(tbl, "pears", 5)
	tbl = set(tbl, "apples", 4)
	v, ok := lookup(tbl, "apples")
	println("apples", v, ok, len(tbl))
	_, ok = lookup(tbl, "plums")
	println("plums", ok)

	// Swapping elements with a tuple assignment works.
	s[0], s[4] = s[4], s[0]
	println(s[0], s[4])
}
