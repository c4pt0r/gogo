# 3. Language Basics

gogo programs are written in Go syntax and type-checked by Go's own type
checker, so everything that compiles means what it means in Go, with one
deliberate exception (messages are copied, chapter 6). But gogo accepts only
a **subset** of Go. The subset is small on purpose: every feature costs code
in a VM that must fit in 64 KB, and some Go features (shared memory,
goroutines) would break the isolation between actors.

This chapter walks through what you can use, then lists what you cannot and
what to write instead. The compiler checks the subset with an allow-list:
anything it does not know is rejected with an error of the form

```text
main.gg:LINE:COL: gogo: <what> not supported
```

so you will never get a program that silently behaves differently from Go.

## 3.1 Programs and packages

A program is one directory with `package main` and `func main()`. It can be
split over several `.gg` files (gogo source files; other files in the
directory, `.go` files included, are not read). Your program
can import only gogo's own packages, all under the `gogo/` path:

| Package | What it is |
|---|---|
| `gogo/actor` | spawn, send, receive, links, monitors, timers, names |
| `gogo/supervisor` | restart a failing actor |
| `gogo/port` | open and close ports, GPIO |
| `gogo/bytes` | `Bytes` (immutable, shareable) and `Buffer` |
| `gogo/strings`, `gogo/strconv` | string and number helpers |
| `gogo/kbd`, `gogo/power` | keyboard and battery |
| `gogo/fs`, `gogo/kv`, `gogo/ringlog` | storage |
| `gogo/net`, `gogo/dhcp`, `gogo/dns`, `gogo/http` | networking |
| `gogo/config` | settings from the device's configuration store |

Importing anything else, including Go's standard library, fails:

```text
main.gg:2:8: gogo: import "fmt" not supported
```

You cannot write your own packages yet; split a large program over several
files of `package main` instead. Renamed imports (`import s "gogo/strings"`)
are rejected too.

> **Note.** There is no `gogo/fmt` or `gogo/errors` package. Print with
> `println`, and report errors as shown in section 3.9.

## 3.2 Types and values

The supported types are:

- `int`: always **32 bits**, on the board and on your computer. Overflow
  wraps around, as in Go. Constants that do not fit are compile errors.
- `int64`: 64 bits. `actor.Now()` returns one.
- `byte` (`uint8`): arithmetic is cut to 8 bits after every operation.
- `bool` and `string`. A string is an immutable sequence of bytes.
- `rune` only as the type of character literals and of the value in
  `range` over a string.
- Slices (`[]T`), structs, function values and `any`.
- Named types whose underlying type is `int`, `int64`, `bool` or `string`
  (`type Weekday int`), and named structs.
- `const` with `iota`.
- The gogo types `actor.PID`, `actor.Ref`, `bytes.Bytes`, `bytes.Buffer`
  and the struct types of the gogo packages.

There are **no** floats, no unsigned types except `byte`, no `int8/16/32`,
no arrays, no maps, no pointers, no channels, no complex numbers.

```go
// The basic types of the subset: int (32 bits), int64, byte, bool, string,
// named basic types, constants and iota.
package main

// Weekday is a named int type. Its constants come from iota.
type Weekday int

const (
	Sunday Weekday = iota
	Monday
	Tuesday
)

const kilo = 1024

func main() {
	// int is always 32 bits and wraps around on overflow, as in Go.
	big := 2147483647
	big++
	println("int wraps:", big)

	// int64 for larger numbers; actor.Now() returns one too.
	var t int64 = 1 << 40
	println("int64:", t*3)

	// byte arithmetic wraps at 256.
	b := byte(250)
	b += 10
	println("byte:", b)

	println("bool:", 3 > 2, !true)
	println("string:", "gogo"+"!", len("gogo"))
	println("named:", Monday, Tuesday, int(Tuesday)*kilo)

	// Integer division truncates toward zero; % has the dividend's sign.
	println("div:", -7/2, -7%2, 7/2)

	// Conversions between the integer types are explicit.
	var n int = 300
	println("convert:", int64(n)*1000000, byte(n), int(int64(5)))
}
```

```text
int wraps: -2147483648
int64: 3298534883328
byte: 4
bool: true false
string: gogo! 4
named: 1 2 2048
div: -3 -1 3
convert: 300000000 44 5
```

Conversions between `int`, `int64` and `byte` must be written out, as in
Go. `string(b)` and `[]byte(s)` convert between strings and byte slices
(each copies). Other conversions are rejected.

## 3.3 Control flow

`if`/`else`, `for` in all its forms, `range` over slices, strings and
integers, `switch`, `break` and `continue` (also with labels) and `return`
work as in Go.

```go
// Control flow: if/else chains, the three for forms, range over slices and
// strings, break and continue. Switches, labels and range over an integer
// follow in their own sections.
package main

func classify(n int) string {
	if n < 0 {
		return "negative"
	} else if n == 0 {
		return "zero"
	} else if n < 10 {
		return "small"
	}
	return "large"
}

func main() {
	for _, n := range []int{-5, 0, 7, 42} {
		println(n, classify(n))
	}

	// The C-style loop, the while-style loop and the endless loop.
	sum := 0
	for i := 1; i <= 10; i++ {
		if i%2 == 0 {
			continue
		}
		sum += i
	}
	println("odd sum", sum)

	n := 27
	steps := 0
	for n != 1 {
		if n%2 == 0 {
			n = n / 2
		} else {
			n = 3*n + 1
		}
		steps++
	}
	println("collatz steps", steps)

	i := 0
	for {
		i++
		if i == 5 {
			break
		}
	}
	println("broke at", i)

	// range over a string gives byte indexes and byte values.
	for i, c := range "gogo" {
		if c == 'o' {
			println("o at", i)
		}
	}

	// An if statement may start with a short declaration.
	if half := sum / 2; half > 10 {
		println("half", half)
	}
}
```

```text
-5 negative
0 zero
7 small
42 large
odd sum 25
collatz steps 111
broke at 5
o at 1
o at 3
half 12
```

`range` over a string gives **bytes**, not runes: the subset never decodes
UTF-8.

### Switch

`switch` compares a value with each `case` in order and runs the first
that matches. A case may list several values, a switch without a value
runs the first case whose condition is true, an init statement can come
before the value, and `fallthrough` at the end of a clause runs the next
clause without testing it. As in Go, there is no implicit fall through, and
a plain `break` leaves the switch.

```go
// Expression switch: a tag compared with each case, several values per
// case, a switch with no tag, an init statement and fallthrough.
package main

type Code int

const (
	Ok Code = iota
	Busy
	Gone
)

func describe(c Code) string {
	switch c {
	case Ok:
		return "ok"
	case Busy, Gone:
		return "try later"
	default:
		return "unknown"
	}
}

func size(n int) string {
	switch { // no tag: the first true case runs
	case n < 0:
		return "negative"
	case n < 10:
		return "small"
	}
	return "large"
}

func main() {
	println(describe(Ok), describe(Gone), describe(7))
	println(size(-1), size(3), size(50))

	for _, cmd := range []string{"start", "stop", "jump"} {
		switch s := cmd + "!"; s { // strings compare by content
		case "start!":
			println("starting")
		case "stop!":
			println("stopping")
		default:
			println("unknown", s)
		}
	}

	// fallthrough runs the next clause's body without testing it.
	switch level := 2; level {
	case 2:
		println("level 2")
		fallthrough
	case 1:
		println("level 1")
	}
}
```

```text
ok try later unknown
negative small large
starting
stopping
unknown jump!
level 2
level 1
```

The value can be an `int`, `int64`, `byte`, `bool`, `string`, a named type
of one of them (`type Code int`), an `actor.PID` or an `actor.Ref`. A
switch on an `any` value or on a struct is not supported yet (`switch on a
value of type any not supported`). For messages in an `any`, use a *type*
switch, `switch v := x.(type)`, which is how actors look at what they
receive (chapter 4).

### Labeled break and continue

A label on a `for`, `range`, `switch` or type switch statement lets
`break` and `continue` name it: `break search` leaves the labeled loop
from inside an inner one, and `continue rows` starts its next pass.

```go
// Labeled break and continue: leave or restart an outer loop from an
// inner one, or leave a loop from inside a switch.
package main

func main() {
	grid := [][]int{{1, 3, 5}, {4, 5, 6}, {7, 9, 11}}

	// break L leaves both loops at the first match.
	found := false
search:
	for i, row := range grid {
		for j, v := range row {
			if v == 5 {
				println("5 at", i, j)
				found = true
				break search
			}
		}
	}
	println("found", found)

	// continue L goes on with the next row.
rows:
	for i, row := range grid {
		for _, v := range row {
			if v%2 == 0 {
				println("row", i, "has an even number:", v)
				continue rows
			}
		}
		println("row", i, "is all odd")
	}

	// A plain break inside a switch leaves only the switch; break L
	// leaves the loop.
	n := 0
loop:
	for {
		switch {
		case n == 3:
			break loop
		default:
			n++
		}
	}
	println("n", n)
}
```

```text
5 at 0 2
found true
row 0 is all odd
row 1 has an even number: 4
row 2 is all odd
n 3
```

`goto` is not supported (`goto statement not supported`), and neither is a
label on any other statement.

### range over an integer

`for i := range n` runs `i` from 0 to `n-1`, as in Go 1.22. `n` is
evaluated once, `i` has `n`'s type, and when `n` is zero or negative the
body does not run.

```go
// range over an integer: for i := range n runs i from 0 to n-1.
package main

func main() {
	for i := range 3 {
		println("pass", i)
	}

	// No variable: just repeat.
	dots := ""
	for range 4 {
		dots += "."
	}
	println(dots)

	// n is evaluated once; zero or a negative n runs no pass.
	n := 0
	for range n {
		println("never")
	}
	for i := range -2 {
		println("never", i)
	}

	// i has n's type.
	var big int64 = 3
	var total int64
	for i := range big {
		total += i << 32
	}
	println(total)
}
```

```text
pass 0
pass 1
pass 2
....
12884901888
```

## 3.4 Functions and closures

Functions can take and return any number of values, have named results and
bare returns, be passed around as values and be written as literals
(closures).

```go
// Functions: several results, named results, function values, closures,
// and the rule that a captured variable must not change.
package main

func divmod(a, b int) (int, int) {
	return a / b, a % b
}

// minmax has named results and a bare return.
func minmax(xs []int) (lo, hi int) {
	lo, hi = xs[0], xs[0]
	for _, x := range xs {
		if x < lo {
			lo = x
		}
		if x > hi {
			hi = x
		}
	}
	return
}

// apply takes a function value.
func apply(f func(int) int, xs []int) []int {
	out := make([]int, 0, len(xs))
	for _, x := range xs {
		out = append(out, f(x))
	}
	return out
}

// adder returns a closure. step is captured by value when the closure is
// made, and is never assigned again.
func adder(step int) func(int) int {
	return func(x int) int { return x + step }
}

func double(x int) int { return x * 2 }

func show(xs []int) {
	s := ""
	for i, x := range xs {
		if i > 0 {
			s = s + " "
		}
		s = s + itoa(x)
	}
	println(s)
}

// itoa is a small decimal formatter (gogo/strconv.Itoa does the same).
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	b := []byte{}
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	if neg {
		return "-" + string(b)
	}
	return string(b)
}

func main() {
	q, r := divmod(17, 5)
	println("divmod", q, r)
	lo, hi := minmax([]int{4, -2, 9, 3})
	println("minmax", lo, hi)

	xs := []int{1, 2, 3}
	show(apply(double, xs))
	show(apply(adder(10), xs))
	show(apply(func(x int) int { return x * x }, xs))

	// A closure that needs changing state gets it as an argument and
	// returns the new value instead of assigning a captured variable.
	count := func(n int, s string) int { return n + len(s) }
	total := 0
	for _, w := range []string{"go", "gogo", "actor"} {
		total = count(total, w)
	}
	println("letters", total)
}
```

```text
divmod 3 2
minmax -2 9
2 4 6
11 12 13
1 4 9
letters 11
```

### Closures capture by value

In Go a closure refers to the variables it captures; if the variable
changes later, the closure sees the change. In gogo a closure **copies**
the captured values when it is created. That is what lets a closure be sent
to another actor or used to start one: it carries its data with it.

To make sure the two models never give different answers, gogo rejects any
assignment to a captured variable after its declaration, inside or outside
the closure. It also rejects giving `any` a `byte`, because at run time a
byte cannot be told apart from an `int`:

```go
// This program does NOT compile. Closures copy what they capture when they
// are made, so gogo rejects a captured variable that changes afterwards
// (in Go the closure would see the change; in gogo it could not).
package main

func main() {
	n := 0
	show := func() { println(n) }
	n = 5
	show()
	var b byte = 7
	var v any = b // a byte cannot go into any
	println(v)
}
```

```text
$ gogo build docs/book/examples/03-capture
```

```text
main.gg:9:2: gogo: captured variable must not be reassigned
main.gg:12:14: gogo: byte in interface not supported
```

The fix is to give the closure its own variable that never changes: pass
the changing value as an argument, return the new value, or copy it first
(`n := i` before the closure). A common case is the loop variable: `for i
:= 0; i < 3; i++ { actor.Spawn(func() { work(i) }) }` is rejected because
`i++` assigns `i`. Write `n := i` inside the loop and capture `n`.
`range` variables are fine, because `range` declares fresh ones each time
round.

Similarly, a named result that a closure captures must not be set by a
`return` with values (`captured named result must not be set by return;
use unnamed results`).

Function values have a few more limits: they cannot be compared (not even
with `nil`) and cannot be put into `any`. Calling a nil function value
crashes the calling actor with `badcode`.

### Variadic functions

A function's last parameter can be `...T`: inside the function it is a
`[]T`. Callers pass any number of `T` values, which go into a new slice,
or none (the parameter is then a nil slice), or an existing slice with
`s...`, which the function receives as it is, sharing its array, as in Go.
Func literals and func values can be variadic too.

```go
// Variadic functions: a final ...T parameter is a []T; callers pass
// separate values, none at all, or a slice with s...
package main

func sum(xs ...int) int {
	t := 0
	for _, x := range xs {
		t += x
	}
	return t
}

func join(sep string, parts ...string) string {
	s := ""
	for i, p := range parts {
		if i > 0 {
			s += sep
		}
		s += p
	}
	return s
}

func main() {
	println(sum(), sum(1, 2, 3))
	nums := []int{10, 20, 30}
	println(sum(nums...))
	println(join(", ", "a", "b", "c"))
	println(join(", ") == "")

	// With no values the parameter is a nil slice.
	count := func(xs ...int) bool { return xs == nil }
	println(count(), count(1))

	// s... passes the slice itself: the function sees the same array.
	zero := func(xs ...int) { xs[0] = 0 }
	zero(nums...)
	println(nums[0])
}
```

```text
0 6
60
a, b, c
true
true false
0
```

## 3.5 Structs, and why there are no methods

Structs work as in Go: literals with or without field names, nested
structs, and value semantics (assignment copies). Two things are missing:
**methods** and **field assignment**. You cannot write `func (p Point)
Move(...)`, and you cannot write `p.X = 3`.

The gogo way is to treat structs as immutable values and write ordinary
functions that return a new value:

```go
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
```

```text
1 11 {11 2}
box 12
box 20 {{0 0} {4 5} box}
0 true
true
```

This is less odd in an actor program than it sounds. State lives in an
actor's local variables, and messages are values that are built once and
sent. Comparing structs with `==` is not supported either; compare fields.

Only `actor.PID`, `actor.Ref`, `int`, `int64`, `bool`, `string` and
named types of those can be compared with `==`. For slices only `s == nil`
works.

## 3.6 Slices, the only collection

Without arrays and maps, slices do all the work. `make`, `append` (also
`append(a, b...)`), `copy`, `len`, `cap` and slice expressions `s[i:j]`
behave as in Go, including sharing: two slices of one array see each
other's writes. A small table of key/value pairs searched in a loop stands
in for a map:

```go
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
```

```text
true 0
5 16
100 2 7
2 0 100 8
Gogo
apples 4 true 2
plums false
16 0
```

Indexing out of range crashes the actor with `badcode` (chapter 5); it
never reads or writes outside the slice. Slice literals cannot use keys
(`[]int{2: 5}`), and three-index slices (`s[a:b:c]`) are not supported.
Index assignment works on slices (`s[i] = v`), including through a struct
field (`m.Items[i] = v`); strings are immutable, as in Go.

### Printing slices

`println` prints a slice as its elements in brackets, the way it prints a
struct's fields in braces. A nil or empty slice prints as `[]`, and a
`[]byte` prints its bytes as numbers.

```go
// println prints a slice's elements, like a struct's fields. Go's println
// prints a slice as [len/cap]address instead.
package main

type Point struct{ X, Y int }

func main() {
	println([]int{1, 2, 3})
	var none []string
	println(none, len(none))
	println([]string{"a", "b"}, []bool{true})
	println([]byte("hi")) // bytes print as numbers
	println([]Point{{1, 2}, {3, 4}})
	println([][]int{{1}, {2, 3}})
}
```

```text
[1 2 3]
[] 0
[a b] [true]
[104 105]
[{1 2} {3 4}]
[[1] [2 3]]
```

This differs from Go: Go's `println` prints any slice as its length,
capacity and the address of its array, `[3/3]0xc000014090`; only
`fmt.Println` prints the elements. gogo has no addresses to show, so it
prints what `fmt.Println` would.

## 3.7 Strings, strconv and strings

Strings can be indexed (a byte), sliced (a new string), concatenated with
`+` and compared with `==`, `<` and friends. Two small libraries, written in
the gogo subset and compiled into your image when you import them, cover the
rest:

- `gogo/strings`: `Index`, `IndexByte`, `Cut`, `HasPrefix`, `HasSuffix`,
  `TrimSpace`, `ToLower`, `EqualFold`, `Split`.
- `gogo/strconv`: `Itoa`, `Atoi`, `FormatInt64`, `ParseInt64`.

All of them work on bytes. `ToLower`, `EqualFold` and `TrimSpace` know only
ASCII. Parse functions return `(value, ok bool)` instead of an error.

```go
// Strings are immutable byte sequences. gogo/strings and gogo/strconv
// cover the common needs; both work on bytes, not runes.
package main

import (
	"gogo/strconv"
	"gogo/strings"
)

func main() {
	line := "  GET /index.html HTTP/1.0  "
	t := strings.TrimSpace(line)
	parts := strings.Split(t, " ")
	println(len(parts), parts[0], parts[1])

	method, rest, found := strings.Cut(t, " ")
	println(method, found, strings.HasPrefix(rest, "/"), strings.HasSuffix(t, "1.0"))
	println(strings.Index(t, "HTTP"), strings.IndexByte(t, '/'), strings.ToLower(method))
	println(strings.EqualFold("Content-Length", "content-length"))

	// Numbers and strings.
	n, ok := strconv.Atoi("-1234")
	println(n, ok)
	_, ok = strconv.Atoi("12x")
	println(ok)
	println(strconv.Itoa(99) + " bottles")
	println(strconv.FormatInt64(255, 16), strconv.FormatInt64(-5, 2))
	big, ok := strconv.ParseInt64("9000000000", 10)
	println(big, ok)

	// Indexing gives a byte; slicing gives a new string; < compares.
	s := "héllo"
	println(len(s), s[0], s[3:], "abc" < "abd")
}
```

```text
3 GET /index.html
GET true true true
16 4 get
true
-1234 true
false
99 bottles
ff -101
9000000000 true
6 104 llo true
```

Note the last line: `"héllo"` is 6 bytes long, because `é` is two bytes
in UTF-8.

To turn a byte into a one-character string, convert a one-element slice:
`string([]byte{c})`. (`string(rune(c))` is a conversion the subset
rejects.) Another common trick is to slice a constant string: with `const
digits = "0123456789"`, `digits[d:d+1]` is the digit `d`, and no slice is
allocated. Chapter 8 uses it for keyboard characters.

## 3.8 Bit operations

`&`, `|`, `^`, `&^`, `<<`, `>>`, unary `^` and their assignment forms work on
`int`, `int64` and `byte`, with Go's rules: a shift count of the type width
or more gives 0 (or -1 when shifting a negative number right), `>>` keeps the
sign, and a **negative shift count crashes the actor** with `badshift`.

```go
// Bit operations on int, int64 and byte.
package main

const (
	FlagRead  = 1 << iota // 1
	FlagWrite             // 2
	FlagExec              // 4
)

func main() {
	perm := FlagRead | FlagWrite
	println(perm&FlagWrite != 0, perm&FlagExec != 0)
	perm |= FlagExec
	perm &^= FlagWrite // clear a bit
	println(perm)

	// >> is an arithmetic shift: the sign is kept. A shift by the width
	// or more gives 0 (or -1 for a negative value shifted right).
	x := -16
	one := 1
	println(x>>2, one<<30, x>>40, one<<40)
	println(^0, 0xF0^0xFF)

	// byte results are cut to 8 bits after every operation.
	var b byte = 0x81
	println(b<<1, b>>1, ^b)

	// int64 has the full 64 bits.
	var w int64 = 1
	w = w<<62 | 5
	println(w, w>>60, w&0xFF)

	// Pack and unpack an IPv4 address held in an int, as gogo/net does.
	// Addresses from 128.0.0.0 up are negative ints.
	ip := ipv4(192, 168, 1, 42)
	println(ip, ip>>24&0xFF, ip>>16&0xFF, ip>>8&0xFF, ip&0xFF)
}

func ipv4(a, b, c, d int) int { return a<<24 | b<<16 | c<<8 | d }
```

```text
true false
5
-4 1073741824 -1 0
-1 15
2 64 126
4611686018427387909 4 5
-1062731478 192 168 1 42
```

gogo stores IPv4 addresses in an `int`, which is why `192.168.1.42` prints
as a negative number. Chapter 10 shows how to format one.

## 3.9 Errors without `error`

The `error` type is not in the subset, and neither is `panic`. Library
functions report failure in one of two ways, and your code can do the same:

- a `bool` result, like `strconv.Atoi` or `actor.Whereis`;
- a named `int` type of error codes, where 0 means success, like `fs.Err`.

```go
// There is no error type in the subset. Report failure with a bool, or
// with a named int type of error codes, as gogo/fs does with fs.Err.
package main

import "gogo/strconv"

// ParseErr is a small set of error codes; 0 means success.
type ParseErr int

const (
	OK ParseErr = iota
	Empty
	BadNumber
	OutOfRange
)

// describe names a code for messages.
func describe(e ParseErr) string {
	if e == OK {
		return "ok"
	} else if e == Empty {
		return "empty input"
	} else if e == BadNumber {
		return "not a number"
	}
	return "out of range"
}

// parsePercent parses "0".."100".
func parsePercent(s string) (int, ParseErr) {
	if s == "" {
		return 0, Empty
	}
	n, ok := strconv.Atoi(s)
	if !ok {
		return 0, BadNumber
	}
	if n < 0 || n > 100 {
		return 0, OutOfRange
	}
	return n, OK
}

func main() {
	for _, s := range []string{"42", "", "x1", "250"} {
		n, err := parsePercent(s)
		if err != OK {
			println("["+s+"]", "error:", describe(err), int(err))
			continue
		}
		println("["+s+"]", "=", n)
	}
}
```

```text
[42] = 42
[] error: empty input 1
[x1] error: not a number 2
[250] error: out of range 3
```

For failures you do not expect and cannot handle, gogo's answer is
different: let the actor crash, and let a supervisor restart it. That is
the subject of chapter 5. To stop the current actor deliberately, call
`actor.Exit(code)` with a code of 100 or more.

## 3.10 What is not supported

Here is a program that uses many of the features gogo leaves out:

```go
// This program does NOT compile: it collects the Go features the gogo
// subset rejects, so you can see the compiler's messages.
package main

var counter = 0 // package-level variable

type Shape interface{ Area() int } // user interface

type Sq struct{ S int }

func (s Sq) Area() int { return s.S * s.S } // method

func Max[T int | int64](a, b T) T { // generics
	if a > b {
		return a
	}
	return b
}

func main() {
	ch := make(chan int) // channel
	go func() { ch <- 1 }()
	defer println("bye")
	m := map[string]int{} // map
	p := &Sq{2}           // pointer
	x := 1.5              // float
	if counter > 0 {
		goto done // goto
	}
done:
	var a [4]int // array
	s := Sq{1}
	s.S = 2 // field assignment
	println(m, p, x, a[0])
	panic("boom")
}
```

`gogo build` reports each one:

```text
main.gg:5:1: gogo: global variable not supported
main.gg:7:6: gogo: type definition other than struct or int, int64, bool, string not supported
main.gg:11:1: gogo: method not supported
main.gg:13:1: gogo: generic function not supported
main.gg:21:8: gogo: channel not supported
main.gg:22:2: gogo: go statement not supported
main.gg:23:2: gogo: defer statement not supported
main.gg:24:7: gogo: map not supported
main.gg:25:7: gogo: pointer not supported
main.gg:26:7: gogo: float not supported
main.gg:28:3: gogo: goto statement not supported
main.gg:30:1: gogo: label on a statement other than for or switch not supported
main.gg:31:8: gogo: array not supported
main.gg:33:2: gogo: field assignment not supported
main.gg:34:10: gogo: map not supported
main.gg:34:13: gogo: pointer not supported
main.gg:34:16: gogo: float not supported
main.gg:34:19: gogo: index of [4]int not supported
main.gg:35:2: gogo: builtin panic not supported
```

The full list, with what to do instead:

| Go feature | Error | gogo way |
|---|---|---|
| `go f()` | `go statement not supported` | `actor.Spawn(f)` |
| channels, `select`, `<-` | `channel not supported`, `operator <- not supported` | messages: `actor.Send`, `actor.Receive` |
| `defer`, `recover` | `defer statement not supported` | let the actor exit; ports and files close automatically |
| `panic(v)` | `builtin panic not supported` | `actor.Exit(code)` |
| `error` | `error not supported` | `bool` or an `int` code type |
| package-level `var` | `global variable not supported` | `const`, or state in an actor |
| maps | `map not supported` | a slice of key/value structs |
| arrays `[N]T` | `array not supported` | slices |
| pointers, `&x`, `*p` | `pointer not supported` | values; state in actors |
| methods | `method not supported` | functions taking the struct |
| your own interfaces | `type definition other than struct or int, int64, bool, string not supported` | function values, or messages and a type switch |
| `p.X = v` | `field assignment not supported` | build a new struct |
| `switch` on an `any` or a struct | `switch on a value of type any not supported` | a type switch; compare fields |
| type assertion `x.(T)` | `type assertion not supported` (and `multi-value assignment not supported` for `v, ok := x.(T)`) | a type switch |
| `goto` | `goto statement not supported` | labeled `break`/`continue`, or a function and `return` |
| `float32/64` | `float not supported` | integers (fixed point: millivolts, percent) |
| `uint32`, `int16`, ... | `uint32 not supported` | `int`, `int64`, `byte` |
| generics | `generic function not supported` | write the function per type |
| `fmt`, other std packages | `import "fmt" not supported` | `println`, `gogo/strconv` |
| `struct == struct` | `comparison of main.P not supported` | compare fields |
| `func init()` | `func init not supported` | call it from `main` |
| embedded fields | `embedded field not supported` | a named field |

> **Note.** Struct methods, maps, arrays, pointers, `uint32`,
> package-level variables, the `error` interface, `panic` and type
> assertions are not supported yet. They are planned for
> the "language completeness" milestone, next after embed mode (see
> `docs/design.md` §5.2).

### Small surprises

- `println` accepts any number of `int`, `int64`, `bool`, `string`, PID,
  Ref, struct and slice arguments (structs print as `{1 2}`, slices as
  `[1 2]`, unlike Go), but not `Bytes` or `Buffer` (use `.String()`), and
  not a multi-result call (`println(divmod(7, 2))` is `multiple values not
  supported`).
- A two-result gogo builtin cannot be a statement on its own: `actor.
  Call(p, req, 100)` on a line by itself is rejected with `multiple values
  not supported`. Write `_, _ = actor.Call(p, req, 100)`. Your own
  multi-result functions can be called as statements.
- The zero value of a PID cannot be written as a literal
  (`actor.PID{}` is rejected); declare one: `var none actor.PID`.
- Unary `+` is rejected (`operator + not supported`).

With the language in hand, we can turn to what gogo is really about:
actors.
