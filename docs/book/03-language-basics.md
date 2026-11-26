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

A program is a directory with `package main` and `func main()`, the
*build root*. `package main` can be split over several `.gg` files (gogo
source files; other files in the directory, `.go` files included, are not
read). A program can import gogo's own packages, all under the `gogo/`
path, and packages of your own (section 3.1.1):

| Package | What it is |
|---|---|
| `gogo/actor` | spawn, send, receive, links, monitors, timers, names |
| `gogo/supervisor` | restart a failing actor |
| `gogo/port`, `gogo/gpio` | open and close ports, the stream protocol; GPIO pins |
| `gogo/bytes` | `Bytes` (immutable, shareable) and `Buffer` |
| `gogo/strings`, `gogo/strconv` | string and number helpers |
| `gogo/kbd`, `gogo/power` | keyboard and battery |
| `gogo/fs`, `gogo/kv`, `gogo/ringlog` | storage |
| `gogo/net`, `gogo/dhcp`, `gogo/dns`, `gogo/http` | networking |
| `gogo/sys` | the wall clock, randomness, settings from the device's configuration store |
| `gogo/errors` | `New`, `Is`, `Unwrap`, and the error codes every package shares (`errors.NotFound`, ...) |

Go's standard library is not available. Importing `fmt` looks for a
package of your own called `fmt`, finds none and says where it looked:

```text
main.gg:2:8: gogo: import "fmt": package not found; searched:
	. (build root: serves only imports starting with "app/")
	/home/you/gogo/fmt (search path: no such directory)
```

Renamed imports (`import s "gogo/strings"`) are rejected.

### 3.1.1 Packages of your own

Any import that does not start with `gogo/` is a package of your own: a
directory of `.gg` files, all with the same `package` clause. Inside the
build root, the import path starts with the root directory's name. For a
program in `app/`:

```text
app/
  main.gg              package main: import "app/shapes", "app/text/banner"
  shapes/
    shapes.gg          package shapes
    square.gg          package shapes
  text/banner/
    banner.gg          package banner
```

```go
// A program of three packages: package main here, app/shapes and the
// nested app/text/banner. The imports start with app/, the name of this
// directory (the build root).
package main

import (
	"app/shapes"
	"app/text/banner"
)

func main() {
	r := shapes.Rect{W: 3, H: 4}
	println(banner.Title("shapes"))
	println("area", shapes.Area(r), "perimeter", shapes.Perimeter(r))
	sq := shapes.Square(5)
	println("square", sq.W, sq.H, shapes.Area(sq))
}
```

```go
// Package shapes measures rectangles. Its name is its directory's name.
package shapes

// Rect is a W by H rectangle.
type Rect struct{ W, H int }

// Area is W times H.
func Area(r Rect) int { return r.W * r.H }

// Perimeter is the length around r.
func Perimeter(r Rect) int { return 2 * (r.W + r.H) }
```

```text
$ gogo run docs/book/examples/03-packages/app
----------
| shapes |
----------
area 12 perimeter 14
square 5 5 25
```

An import is looked up in this order, and the first directory that holds
`.gg` files wins:

1. **The build root.** `import "app/shapes"` in a program built from
   `app/` is `app/shapes/`; `app/text/banner` is `app/text/banner/`.
2. **The search path**, for packages shared between programs: each
   directory of `$GOGOPATH` in turn (separated by `:`, `;` on Windows),
   where `import "lib/ring"` is `<dir>/lib/ring/`. When `GOGOPATH` is
   unset the search path is `$HOME/gogo`. `gogo build -path dirs` and
   `gogo run -path dirs` replace it for one build, and `gogo env` prints
   it (chapter 12).

`examples/packages` in the repository uses both: a package of its own
(`app/sensors`) and shared packages from a `GOGOPATH` directory
(`units/temp`, `units/stats`, one importing the other, with a struct type
passed between packages), plus a `#!` script that imports a shared
package. Its README has the commands and what `gogo env` prints.

If the same import path is found in more than one place, the first is
used and the build warns, naming them all:

```text
gogo: warning: package "lib/ring" is in 2 places; using the first: /home/you/gogo/lib/ring, /home/you/work/pkgs/lib/ring
```

A program kept inside a search path directory (`~/gogo/app`, the GOPATH
habit) finds its packages once, through the root: the same directory is
one place.

There is no `go.mod` and no version: a package is whatever is in its
directory when you build. The rules, as in Go:

- A package's name (its `package` clause) is its directory's name, the
  last element of its import path, so the name you use in code is always
  the last element of the import. `package utils` in `app/util/` is an
  error, and so the directory's name must be a Go identifier: `my-lib/`
  cannot hold a package (`mylib/` or `my_lib/` can).
- An import path matches the directory names letter for letter, case
  included, even on a file system that ignores case (macOS by default):
  `import "app/Util"` for `app/util/` is an error, so a program that
  builds on a Mac also builds on Linux. Two import paths that name the
  same directory (through a symlink) are an error too.
- `import "strings"` looks for a package of your own; the error suggests
  `gogo/strings`. A build root named `gogo` cannot have packages of its
  own, since every `gogo/...` import is gogo's.
- Only the build root is `package main`; a `package main` below it is an
  error. The root itself cannot be imported.
- Imports must not form a cycle; the error shows it:
  `b/b.gg:3:8: gogo: import cycle app/a -> app/b -> app/a`.
- A package that cannot be found is an error listing every place
  searched.
- Your packages may import `gogo/` packages and each other. They follow
  the same subset rules as `package main`, and are compiled into the image
  like the `gogo/` libraries: there is no separate linking step, and
  importing a package compiles all of it.
- A package's struct types are the image's own, like those of `package
  main`. A module loaded with `code.Load` (chapter 14) compiles its own
  copy of every package it imports, so even when the program and the
  module import the same package, its struct types cannot be sent from
  one to the other (the send fails with `BadMsg`). Each package's
  `//gogo:optional` covers its own opens only, and the image requires the
  drivers your packages open (chapter 12).
- In crash reports, `gogo dis` and `actor.Reason.Func`, a function of
  your package is named by its import path, `app/shapes.Area`; a gogo
  library's by its package name, `strings.Index`.

A module built for hot loading (chapter 14) finds its packages the same
way, from its own directory. `gogo run file.gg` builds a single file, with
its directory as the build root (chapter 2, *Scripts*).

> **Note.** There is no `gogo/fmt` package. Print with `println`, and
> report errors as shown in section 3.10.

## 3.2 Types and values

The supported types are:

- `int`: always **32 bits**, on the board and on your computer. Overflow
  wraps around, as in Go. Constants that do not fit are compile errors.
- `int64`: 64 bits. `actor.Now()` returns one.
- `byte` (`uint8`): arithmetic is cut to 8 bits after every operation.
- The other sized integers, as in Go: `int8`, `int16`, `int32` (`rune`),
  `uint8`, `uint16`, `uint32`, `uint64` and `uint` (32 bits, like `int`).
  See *Sized integers* below.
- `float64` and `float32`: IEEE 754 floating point, as in Go (see
  *Floating point* below).
- `bool` and `string`. A string is an immutable sequence of bytes.
- `rune` (`int32`) is the type of character literals and of the value in
  `range` over a string (which gives bytes, not decoded runes).
- Slices (`[]T`), arrays (`[N]T`), structs, function values and `any`.
- Named types whose underlying type is an integer or float type, `bool`
  or `string` (`type Weekday int`, `type Reg uint32`), and named structs.
- Type aliases, another name for a type of the subset (`type Millis =
  int64`, `type Err = errors.Code`): the two names are interchangeable.
- `const` with `iota`.
- The gogo types `actor.PID`, `actor.Ref`, `bytes.Bytes`, `bytes.Buffer`
  and the struct types of the gogo packages.

There are **no** maps, no pointers, no channels, no complex numbers and
no `uintptr`.

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

Conversions between the integer types must be written out, as in Go, and
so must a conversion to a named type built on one (`Weekday(n)`,
`errors.Code(n)`). `string(b)` and
`[]byte(s)` convert between strings and byte slices (each copies). Other
conversions are rejected.

### Floating point

`float64` and `float32` behave as in Go: `+ - * /`, comparisons,
negation, `++` and the compound assignments, with IEEE 754 rounding. An
untyped constant such as `1.5` is a `float64` unless its context says
otherwise, and constant expressions (`1.0/3`, `math.Pi * 2`) are computed
exactly by the compiler. Division by zero gives `+Inf`, `-Inf` or `NaN`
instead of ending the actor, and `NaN` equals nothing, itself included.

```go
// float64 and float32: Go semantics, printed as Go's println prints them.
package main

import (
	"gogo/math"
	"gogo/strconv"
)

// Celsius is a named float type.
type Celsius float64

func fahrenheit(c Celsius) float64 { return float64(c)*9/5 + 32 }

func main() {
	x := 1.5 // untyped float constants default to float64
	var f float32 = 0.1
	println("values:", x, x/3, f, 1e21, 1e-7)

	// float32 rounds every result to float32 (24 bits), float64 to 53
	// bits: println shows the shortest digits that read back exactly.
	println("0.1+0.2:", f+0.2, x/15+0.2)

	// Division by zero is not a crash: it gives an infinity or NaN.
	var zero float64
	nan := zero / zero
	println("special:", 1/zero, -1/zero, nan, nan == nan, -zero)

	// Conversions are explicit; to an integer they truncate toward zero.
	n := 7
	println("convert:", float64(n)/2, int(-2.9*x), int64(x*1e12), float64(f))

	println("named:", fahrenheit(21.5))
	println("math:", math.Sqrt(2), math.Floor(-x), math.Round(2.5), math.Pi)

	s := strconv.FormatFloat(math.Pi, 'f', 3, 64)
	v, err := strconv.ParseFloat("6.02e23", 64)
	println("strconv:", s, v, err == nil)
}
```

```text
values: 1.5 0.5 0.1 1e+21 1e-07
0.1+0.2: 0.3 0.30000000000000004
special: +Inf -Inf NaN false -0
convert: 3.5 -4 1500000000000 0.10000000149011612
named: 70.7
math: 1.4142135623730951 -2 3 3.141592653589793
strconv: 3.142 6.02e+23 true
```

- `println` prints a float the way Go's `println` does (Go 1.27): the
  shortest digits that read back as the same value, as `%e` for large and
  small exponents (`1e+21`, `1e-07`), `-0`, `NaN`, `+Inf`. A `float32`
  prints at its own precision (`0.1`, not `0.10000000149011612`). Older Go
  releases printed `+1.500000e+000`.
- Conversions are explicit. `int(f)`, `int64(f)` and `byte(f)` truncate
  toward zero. Go leaves NaN and out-of-range values to the
  implementation; gogo gives 0 for NaN and the nearest end of the range
  otherwise (`int(1e10)` is 2147483647, since `int` is 32 bits);
  `byte(f)` keeps the low 8 bits of that `int` (`byte(300.7)` is 44).
- `float32` and `float64` values can go into `any` (a type switch tells
  them apart), into structs and slices, and into messages, which copy
  them like any value. A named float type cannot go into `any`, like a
  named int.
- `gogo/math` has `Sqrt`, `Floor`, `Ceil`, `Trunc`, `Round`,
  `RoundToEven`, `Abs`, `Copysign`, `Signbit`, `Inf`, `NaN`, `IsNaN`,
  `IsInf`, `Max`, `Min` and Go's constants (`Pi`, `E`, `MaxFloat64`, ...);
  `Pow`, `Exp`, `Log` and the trigonometric functions are not there yet.
  `strconv.FormatFloat` and `strconv.ParseFloat` work as in Go, except
  that `ParseFloat`'s error is `errors.BadFormat` or `errors.Overflow`
  (with `±Inf`, when the value is out of range) and `FormatFloat` has no
  `'x'` format. Both are exact, on the board as on your computer.
- **Cost.** Every float is a small heap object (8 or 12 bytes), so each
  arithmetic result allocates; keep tight loops on integers where you
  can. On the ESP32-S3, `float32` arithmetic runs on the FPU and
  `float64` in software; with the interpreter's own cost on top, a
  `float64` operation takes about twice as long as a `float32` one
  (20000 multiply-adds: 313 ms in `float32`, 409 ms in `float64`, 226 ms
  on `int`s): prefer `float32` for sensor maths. On a chip without an FPU (the RP2040) both are software.
- **Firmware.** Floats are a firmware option (`GOGO_FLOAT`, requirement
  `float`): an image that uses them needs firmware built with it, and
  image format 15.2 (chapter 12). `gogo firmware` turns it on when an
  image needs it. A program that only imports `gogo/strconv` for `Atoi`
  or `Itoa` does not need it.

### Sized integers

`int8`, `int16`, `int32`, `int64`, `uint8` (`byte`), `uint16`, `uint32`,
`uint64` and `uint` follow Go: arithmetic wraps around at the type's
width after every operation, `/` truncates toward zero and `%` takes the
dividend's sign (for the signed types), the unsigned types divide,
compare and shift right as unsigned numbers, and a shift by the width or
more gives 0 (or -1 for a negative signed value shifted right). Division
by zero ends the actor with `divzero`, as for `int`. A constant that does
not fit its type is a compile error. `uint` is 32 bits, like `int`.

```go
// Sized integers: Go's int8 .. uint64, with wraparound at each type's
// width, unsigned division, comparison and shifts, and explicit
// conversions.
package main

import "gogo/strconv"

// Reg is a named uint32 with methods, like a hardware register.
type Reg uint32

func (r Reg) Bit(n int) bool { return r>>n&1 != 0 }

// crc32 is the IEEE CRC-32 (the one zip and Ethernet use).
func crc32(s string) uint32 {
	crc := ^uint32(0)
	for i := 0; i < len(s); i++ {
		crc ^= uint32(s[i])
		for k := 0; k < 8; k++ {
			if crc&1 != 0 {
				crc = crc>>1 ^ 0xEDB88320
			} else {
				crc >>= 1
			}
		}
	}
	return ^crc
}

// fnv1a is the 64-bit FNV-1a hash.
func fnv1a(s string) uint64 {
	h := uint64(14695981039346656037)
	for i := 0; i < len(s); i++ {
		h ^= uint64(s[i])
		h *= 1099511628211
	}
	return h
}

func main() {
	// Each type wraps at its own width.
	var a int8 = 127
	var b uint16 = 65535
	var c uint32 = 0
	a++
	b++
	c--
	println("wrap:", a, b, c)

	// Unsigned values print, divide, compare and shift as unsigned.
	var big uint64 = 1<<64 - 1
	println("unsigned:", big, big/3, c > 1<<31, c>>28)

	// uint and int are 32 bits, as everywhere in gogo.
	var u uint = 1 << 31
	println("uint:", u, u*2, int(u))

	// Conversions are explicit; they keep the low bits or extend.
	n := -1
	w := 200
	println("convert:", uint8(n), uint32(n), uint64(n), int8(w), int64(c))

	r := Reg(0x80000001)
	println("register:", r.Bit(31), r.Bit(1), uint32(r))

	println("crc32:", crc32("hello"), strconv.FormatUint(uint64(crc32("hello")), 16))
	println("fnv1a:", fnv1a("gogo"))
	v, err := strconv.ParseUint("18446744073709551615", 10, 64)
	println("parse:", v, err == nil)
}
```

```text
wrap: -128 0 4294967295
unsigned: 18446744073709551615 6148914691236517205 true 15
uint: 2147483648 0 -2147483648
convert: 255 4294967295 18446744073709551615 -56 4294967295
register: true false 2147483649
crc32: 907060870 3610a686
fnv1a: 11313272551757906769
parse: 18446744073709551615 true
```

- Conversions follow Go: to a narrower type they keep the low bits
  (`uint8(n)` of an `int` -1 is 255, `int8(w)` of 200 is -56), to a wider
  one they extend by the source's sign (`int64(c)` of a `uint32` is never
  negative). From a float they truncate toward zero; where Go leaves the
  result to the implementation (NaN, out of range), gogo does what Go does
  on arm64, as for `int` (for `f := -1.0`, `uint32(f)` is 4294967295 and
  `uint64(f)` is 0).
- They work everywhere an `int` does: struct fields, slices, arrays,
  `switch`, indexes (any integer of up to 32 bits; not `int64` or
  `uint64`), `range` over a sized count, `any` and messages. In an
  interface a sized value keeps its type, as in Go: `any(uint32(1))` is
  not `any(1)`, and a type switch tells them apart.
- `println` prints an unsigned value as unsigned. A struct, slice or array
  holding a `uint32`, `uint` or `uint64` cannot be printed whole (`println
  of main.P not supported`): print its fields one by one.
- `gogo/strconv` has `FormatUint`, `ParseUint` and `ParseInt` with a bit
  size (0 means 32 bits, the size of `int` and `uint` in gogo).
- **Cost.** `int8`, `int16`, `uint8`, `uint16` and `int32` are as cheap as
  `int` (a narrow result costs one extra instruction to cut it back);
  `uint64` as cheap as `int64`. `uint32` (and `uint`) `+ - * & | ^ <<` cost
  what `int`'s do, but its `/`, `%`, `<`, `<=`, `>`, `>=` and `>>` widen
  both operands to 64 bits first: three or four instructions instead of
  one, and values from 2^30 up are heap objects, as large `int`s are.
- **Firmware.** Programs that use the unsigned 64-bit operations or the
  narrow conversions need image format 15.3 (chapter 12); `byte`, `int32`
  and `uint32` addition alone do not.

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

The value can be of any type `==` works on (section 3.5): an `int`,
`int64`, `byte`, `bool`, `string`, a named type of one of them (`type Code
int`), a float, an `actor.PID`, an `actor.Ref`, a struct, an array, or an interface value such
as an `any`, whose cases may be values of different types (`case 1, "one",
nil:`). To ask *which type* an `any` holds, use a *type* switch, `switch v
:= x.(type)`, which is how actors look at what they receive (chapter 4).

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
// Functions: several results, named results, function values and
// closures.
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

// adder returns a closure that captures step.
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

	// A closure that changes a variable it captures.
	total := 0
	count := func(s string) { total += len(s) }
	for _, w := range []string{"go", "gogo", "actor"} {
		count(w)
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

### Closures share what they capture

As in Go, a closure refers to the variables it captures: when the
variable changes later, inside or outside the closure, the other side
sees the change.

```go
// Closures share the variables they capture, as in Go: a counter kept in
// a closure, and a loop variable that is the iteration's own.
package main

func counter() (func() int, func()) {
	n := 0
	return func() int { return n }, func() { n++ }
}

func main() {
	get, inc := counter()
	inc()
	inc()
	println(get())

	x := 1
	show := func() { println(x) }
	x = 5
	show()

	var fs []func()
	for i := 0; i < 3; i++ {
		fs = append(fs, func() { println(i) })
	}
	for _, f := range fs {
		f()
	}
}
```

```text
$ gogo run docs/book/examples/03-capture
2
5
0
1
2
```

Each iteration of a loop has its own loop variable, as in Go 1.22: the
three closures print 0, 1 and 2, not 3 three times. A deferred func
literal can set a named result (`defer func() { err = wrap(err) }()`).

How it works: a closure copies the values it captures when it is made.
A captured variable that is assigned after its declaration is kept in a
box instead (the same box `&x` gives, section 3.6), and the closure
copies the box, so both sides share it; a variable that is never
assigned after its declaration needs no box. The difference shows when a
closure goes to another actor: `actor.Spawn(func() { ... })` copies what
the closure captures, boxes included, into the new actor, so a variable
the new actor changes is its own (chapter 4).

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

### Defer

`defer f(args)` runs the call when the function returns, as in Go:
deferred calls run last deferred first, at every `return` and at the end
of the function, and the function value, the receiver and the arguments
are evaluated at the `defer` statement. A method value (`defer c.Close()`)
and a func literal (`defer func() { ... }()`) work too:

```go
// defer: the deferred calls run when the function returns, last deferred
// first; their arguments are evaluated at the defer statement.
package main

type Conn struct{ Name string }

func (c Conn) Close() { println("close", c.Name) }

func open(name string) Conn {
	println("open", name)
	return Conn{name}
}

// copyAll returns early on a bad item: every return runs the deferred
// calls registered before it.
func copyAll(items []int) (n int) {
	src := open("src")
	defer src.Close()
	dst := open("dst")
	defer dst.Close()
	for _, it := range items {
		if it < 0 {
			println("bad item", it)
			return n
		}
		n++
	}
	return n
}

func count() {
	x := 1
	defer println("deferred x =", x) // x is evaluated now: 1
	x = 2
	if x > 1 {
		defer println("only when x > 1")
	}
	defer func() {
		println("func literal, last deferred, runs first")
	}()
	println("x =", x)
}

func main() {
	println("copied", copyAll([]int{1, 2, 3}))
	println("copied", copyAll([]int{1, -2, 3}))
	count()
}
```

```text
open src
open dst
close dst
close src
copied 3
open src
open dst
bad item -2
close dst
close src
copied 1
x = 2
func literal, last deferred, runs first
only when x > 1
deferred x = 1
```

gogo's `defer` is **static**: a `defer` statement may not be inside a
loop (`defer inside a loop not supported`), so each one runs at most once
per call and the compiler knows them all. It compiles the deferred calls
at each return, in order; there is no run-time list of deferred calls and
no cost when none was registered. A `defer` inside an `if` or a `switch`
clause is fine (a flag records whether it ran). To defer in a loop, move
the loop body into a function (a func literal is a function of its own,
so a `defer` in a literal called in a loop is allowed).

Two differences from Go:

- **Deferred calls run on normal returns only.** A crash (division by
  zero, an index out of range, `badcode`), a `panic` (also one inside a
  deferred call) and `actor.Exit` end the actor without running them, so
  Go's `defer mu.Unlock()` habit does not carry over to panics. There is no `recover`: the actor ends, and its
  ports and files are closed for it (section 5.7), so `defer f.Close()` is
  for actors that live on, not a safety net.
- **A deferred func literal sees variables, as in Go.** `x := 1; defer
  func() { println(x) }(); x = 2` prints 2, and `defer func() { err =
  wrap(err) }()` changes the result: with named results, `return x` sets
  them before the deferred calls run, and the caller gets what the
  deferred calls left in them.

A deferred call whose single argument is a call with several results,
`defer f(g())`, is rejected; assign the results to variables first.

## 3.5 Structs and methods

Structs work as in Go: literals with or without field names, nested
structs, value semantics (assignment copies) and field assignment:
`p.X = v`, `r.Max.X++`, `pts[i].Y += 2`, `m.Items[i].Name = "a"`, and
fields in multi-assignments (`p.X, p.Y = p.Y, p.X`). Index expressions and
right-hand sides are evaluated first, then the assignments happen left to
right, as in Go.

```go
// Structs are values: assignment copies, and assigning a field changes only
// the variable (or slice element) written, never an earlier copy.
package main

type Point struct{ X, Y int }

type Rect struct {
	Min, Max Point
	Name     string
}

// move returns a moved copy: p is the function's own copy.
func move(p Point, dx, dy int) Point {
	p.X += dx
	p.Y += dy
	return p
}

func area(r Rect) int {
	return (r.Max.X - r.Min.X) * (r.Max.Y - r.Min.Y)
}

func main() {
	p := Point{1, 2}
	q := move(p, 10, 0)
	println(p.X, q.X, q) // p is unchanged: structs are values

	r := Rect{Min: Point{0, 0}, Max: Point{3, 4}, Name: "box"}
	println(r.Name, area(r))
	old := r
	r.Max.X++ // a nested field
	r.Max.Y++
	r.Name += "!"
	println(r.Name, area(r), r)
	println(old.Name, area(old)) // the copy made before is unchanged

	// A slice element's fields, in place: the slice shares its array.
	pts := []Point{{0, 0}, {5, 5}}
	for i := range pts {
		pts[i].X, pts[i].Y = pts[i].Y, pts[i].X+1 // swap and bump
	}
	println(pts[0], pts[1])

	// The zero value of a struct has every field zero.
	var z Rect
	println(z.Max.X, z.Name == "")

	// == compares structs field by field.
	println(p == Point{1, 2}, q != p)
}
```

```text
1 11 {11 2}
box 12
box! 20 {{0 0} {4 5} box!}
box 12
{0 1} {5 6}
0 true
true true
```

Writing a field changes only the variable or slice element written:
`old` keeps the value it had when it was copied, and a function or a
method that writes a field of its parameter changes only its own copy.
A slice element is written in place, because a slice shares its array
(`pts[i].X = v` is seen through every slice of that array, as in Go).

**What a field write costs.** Inside the VM a struct never changes. `p.X =
v` builds a new struct with `X` replaced, copies the other fields into it,
and stores it back into `p` (or into the slice element); for a path such
as `r.Max.X++` it builds every struct on the path, here a new `Point` and
a new `Rect`. So each write allocates one struct per level (2 words plus
one per field), and the old one becomes garbage for the collector. This
is exactly what rebuilding the struct with a literal by hand
(`p = Point{p.X + 1, p.Y}`) costs, so prefer the clearer field write.
In a tight loop it is noticeable: on the host, `p.X++` on a 3-field
struct takes about twice as long as `x++` on an `int`. In a hot loop
over a big struct, keep the changing values in local variables and
write them back once at the end.

A field of a variable that a closure captures can be written on either
side; the closure sees the change (section 3.4).

`==` and `!=` work on `int`, `int64`, `byte`, floats, `bool`, `string`
and named types of those, `actor.PID`, `actor.Ref`, structs (field by
field; a `bytes.Bytes` field by its contents), arrays (element by
element) and interface values (section 3.9). A struct with a
`bytes.Buffer` field does not compare. For slices only `s
== nil` works.

### Methods

A named struct type, or a named `int`, `int64`, `bool` or `string` type
(`type Celsius int`), can have methods with a **value receiver**, as in
Go: `func (p Point) Add(q Point) Point`. You call them as `x.M(args)`,
also on expressions and in chains (`p.Moved(1, 0).Moved(0, 1)`), and they
can be variadic. A package's methods are used from other packages like
its functions, and two types can have methods of the same name.

```go
// Methods with value receivers: on a struct and on a named int. A method
// gets a copy of its receiver, so it returns the new value instead of
// changing the old one.
package main

import "gogo/actor"

type Point struct{ X, Y int }

func (p Point) Add(q Point) Point { return Point{p.X + q.X, p.Y + q.Y} }

func (p Point) Moved(dx, dy int) Point { return p.Add(Point{dx, dy}) }

// Reset changes only its own copy of p.
func (p Point) Reset() {
	p.X, p.Y = 0, 0
	println("inside", p.X, p.Y)
}

type Celsius int

func (c Celsius) Fahrenheit() int { return int(c)*9/5 + 32 }

// Sum is variadic, like a function.
func (p Point) Sum(more ...Point) Point {
	for _, q := range more {
		p = p.Add(q)
	}
	return p
}

func (p Point) Show() { println("show", p.X, p.Y) }

func main() {
	p := Point{1, 2}
	p.Reset()
	println(p.X, p.Y) // unchanged
	p = p.Moved(10, 0).Moved(0, 10)
	println(p)
	println(Celsius(100).Fahrenheit(), p.Sum(), p.Sum(Point{1, 1}, Point{2, 2}))

	// A method value remembers its receiver as it was.
	show := p.Show
	p = Point{}
	show()

	// It can be spawned like any func(): the receiver goes with it.
	done := actor.Self()
	actor.Spawn(func() {
		show()
		actor.Send(done, true)
	})
	actor.Receive()

	// A method expression takes the receiver as its first argument.
	f := Celsius.Fahrenheit
	println(f(-40))
}
```

```text
inside 0 0
1 2
{11 12}
212 {11 12} {14 15}
show 11 12
show 11 12
-40
```

A few things to know:

- **A method works on a copy.** Like a parameter, the receiver is the
  method's own copy: assigning to it or to its fields changes nothing for
  the caller. To "update" a value, return the new one and assign it:
  `p = p.Moved(1, 0)`.
  A slice field still shares its array, as in Go.
- **A method value** `f := x.M` is a func value that captured a copy of
  `x` when it was made. Like any func value it can be spawned, stored in a
  struct field, and sent inside a struct to another actor, which gets its
  own copy of the receiver.
- **A method expression** `T.M` is the method as a function whose first
  parameter is the receiver: `Celsius.Fahrenheit(-40)`.
- **Names.** In `gogo dis`, in crash reports and in `actor.Reason.Func`, a
  method is `Type.Method`, with its package's prefix outside `main`
  (`app/geom.Point.Add`); a func literal inside it is
  `main.Type.Method.func1`.
- **Calls are static.** The compiler knows each call's method, so a call
  costs the same as a function call; a call through an interface value
  (section 3.9) adds a type check per candidate type. Pointer receivers,
  `func (p *Point) M()`, wait for pointers (`pointer receivers are not
  supported yet`).

### Embedded fields

A struct can embed another named type: a field without a name, named
after its type. The embedded struct's fields and methods are *promoted*:
`s.X` means `s.Point.X` and `s.Dist()` means `s.Point.Dist()`, for reading,
for field assignment and for method values, as in Go:

```go
// Embedded fields: the fields and methods of an embedded struct are
// promoted to the outer one, and a shallower name hides a deeper one.
package main

type Point struct{ X, Y int }

func (p Point) Dist() int { return abs(p.X) + abs(p.Y) }

func (p Point) Moved(dx, dy int) Point { return Point{p.X + dx, p.Y + dy} }

// Sprite embeds a Point: s.X is s.Point.X, s.Dist() is s.Point.Dist().
type Sprite struct {
	Point
	Name string
}

// Label is the outer type's own method.
func (s Sprite) Label() string { return s.Name }

// Player embeds a Sprite and has its own X, which hides Sprite's (depth 1
// beats depth 2); the promoted one is still reachable by its full path.
type Player struct {
	Sprite
	X     int
	Lives int
}

func abs(n int) int {
	if n < 0 {
		return -n
	}
	return n
}

func main() {
	s := Sprite{Point{3, -4}, "ship"}
	println(s.X, s.Y, s.Dist(), s.Label())

	// Promoted fields can be assigned, like any field.
	s.X = 10
	s.Point.Y++
	println(s.X, s.Y, s.Moved(1, 1).X)

	p := Player{Sprite: s, X: 99, Lives: 3}
	p.Y = 0 // p.Sprite.Point.Y
	println(p.X, p.Sprite.X, p.Y, p.Dist(), p.Label(), p.Lives)

	// A method value of a promoted method keeps its receiver.
	dist := p.Dist
	p.Sprite.X = 0
	println(dist(), p.Dist())
}
```

```text
3 -4 7 ship
10 -3 11
99 10 0 10 ship 3
10 0
```

Go's rules decide which name a selector means: the shallowest one wins
(`p.X` is Player's own `X`, not the promoted `p.Sprite.X`), and two at the
same depth make the selector ambiguous, a compile error (`ambiguous
selector c.X`) unless you write the path. An embedded field is an
ordinary field underneath: a literal sets it by its type's name
(`Player{Sprite: s}`), and a promoted field costs one `GETFIELD` per
level. A type can embed a named struct, a named basic type
(`type Celsius int`) or an interface, from any package; not a pointer. A method
expression of a promoted method (`Sprite.Dist`) is rejected: write a
method value or call it on the embedded field. The methods of an
embedded gogo type (`bytes.Buffer`, `bytes.Bytes`, `fs.File`) are not
promoted either (`method WriteByte of an embedded gogo/bytes type not
supported`): call them on the field, `w.Buffer.WriteByte(b)`. Promoted
methods count toward implementing an interface (section 3.9), and the
methods of an embedded interface are promoted too: `struct { Namer; K
int }` is a `Namer`, and its `Name()` calls the field's.

## 3.6 Slices and arrays

Without maps, slices do most of the work. `make`, `append` (also
`append(a, b...)`), `copy`, `len`, `cap` and slice expressions `s[i:j]`
behave as in Go, including sharing: two slices of one array see each
other's writes. A small table of key/value pairs searched in a loop stands
in for a map:

```go
// Slices: literals, make, append, copy, slicing and sharing, nil slices.
// Slices are the main collection: arrays are fixed, and there are no maps.
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

### Arrays

An array `[N]T` has a fixed length that is part of its type, and it is a
**value**, as in Go: assigning it, passing it, returning it or storing it
in a field or an element copies it. Indexing (checked, like a slice's),
`len`, `cap`, `range`, element and field assignment through it
(`a[i] = v`, `pts[i].X = v`, `b.Cells[r][c] = v`), literals with `[...]`
and index keys, arrays of structs and arrays in struct fields all work as
in Go:

```go
// Arrays are values: assigning, passing or returning one copies it. A
// slice of an array variable shares it, as in Go.
package main

type Point struct{ X, Y int }

// Board holds its cells in an array field: copying a Board copies them.
type Board struct {
	Cells [3][3]byte
	Moves int
}

// play returns the board with one more move: b is play's own copy.
func play(b Board, row, col int, who byte) Board {
	b.Cells[row][col] = who
	b.Moves++
	return b
}

func sum(a [4]int) int {
	t := 0
	for _, v := range a {
		t += v
	}
	return t
}

func main() {
	var a [4]int // the zero value: four zeros
	a[1] = 5
	a[3] += 2
	b := a // a copy
	b[0] = 9
	println(a[0], b[0], len(a), sum(a))

	// Literals: [...] counts the elements; keys set given indices.
	primes := [...]int{2, 3, 5, 7, 11}
	flags := [8]bool{1: true, 6: true}
	println(len(primes), primes[4], flags[1], flags[2])

	// a[:] shares the variable's array; a = b copies b into it.
	s := a[:]
	s[0] = -1
	println(a[0])
	a = b
	println(s[0])

	// Arrays of structs, and arrays in struct fields.
	pts := [2]Point{{1, 2}, {3, 4}}
	pts[1].X = 30
	var empty Board
	game := play(empty, 1, 1, 'x')
	game = play(game, 0, 2, 'o')
	println(pts[1].X, empty.Moves, game.Moves, game.Cells[1][1], game.Cells[0][2])
}
```

```text
0 9 4 7
5 11 true false
-1
9
30 0 2 120 111
```

Inside the VM an array is a slice of length and capacity N, so it prints
like one (`println(a)` gives `[0 5 0 2]`) and `a[:]` is an ordinary slice
of it. Some things to know:

- **Copies are real copies.** `b := a`, `f(a)`, `return a` and `p.Cells = c`
  each allocate a new array of N elements (an array literal or a call
  result is not copied again). Pass a slice, `a[:]`, when a function only
  reads or should write the caller's array.
- **Only an array variable can be sliced.** `a[:]` shares `a`, and
  `a = b` copies `b`'s elements into that shared array, as in Go. An array
  in a field or an element is not changed in place (a write through it
  copies it), so `p.Cells[:]` is rejected (`slice of an array that is not
  a variable not supported`): copy it first, `c := p.Cells; c[:]`.
- **`==` compares the elements**, as in Go: `a == b` and a `switch` on an
  array hold when every element is equal.
- **Not in `any`.** An array cannot go into an interface, so it cannot be
  a message on its own (`array in interface not supported`): send a slice,
  or a struct with the array as a field, which is copied with the message.
- A closure that captures an array variable shares its elements with the
  variable, as in Go. Named array types (`type Grid [9]int`) are not
  supported.

## 3.7 Strings, strconv and strings

Strings can be indexed (a byte), sliced (a new string), concatenated with
`+` and compared with `==`, `<` and friends. Two small libraries, written in
the gogo subset and compiled into your image when you import them, cover the
rest:

- `gogo/strings`: `Index`, `IndexByte`, `Cut`, `HasPrefix`, `HasSuffix`,
  `TrimSpace`, `ToLower`, `EqualFold`, `Split`.
- `gogo/strconv`: `Itoa`, `Atoi`, `FormatInt64`, `ParseInt64`,
  `FormatUint`, `ParseUint`, `ParseInt` (with a bit size).

All of them work on bytes. `ToLower`, `EqualFold` and `TrimSpace` know only
ASCII. `Atoi` has Go's signature, `(int, error)`, the error
`errors.BadFormat` or `errors.Overflow`; `ParseInt64`, gogo's own, returns
`(value, ok bool)`.

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
	n, err := strconv.Atoi("-1234")
	println(n, err == nil)
	_, err = strconv.Atoi("12x")
	println(err.Error())
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
bad format
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
every integer type, with Go's rules: a shift count of the type width or
more gives 0 (or -1 when shifting a negative number right), `>>` keeps the
sign of a signed type and shifts zeros into an unsigned one, and a
**negative shift count crashes the actor** with `badshift`.

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

## 3.9 Interfaces

An interface is a set of methods. Any type that has those methods
satisfies it, a struct or a named basic type alike, without saying so, and
a value of an interface type holds a value of any such type. Calling a
method on it calls the method of the type it holds:

```go
// Interfaces: a method set any type can satisfy (structs and named basic
// types alike), interface values that hold one of them, type switches and
// assertions on them, and == between them.
package main

type Shape interface {
	Area() int
	Name() string
}

type Rect struct{ W, H int }
type Square int

func (r Rect) Area() int      { return r.W * r.H }
func (r Rect) Name() string   { return "rect" }
func (s Square) Area() int    { return int(s) * int(s) }
func (s Square) Name() string { return "square" }

// Stringer is a smaller interface; every Shape that has String is one.
type Stringer interface{ String() string }

func (s Square) String() string { return "a square" }

func total(shapes []Shape) int {
	t := 0
	for _, s := range shapes {
		t += s.Area() // calls Rect.Area or Square.Area
	}
	return t
}

func describe(x any) string {
	switch v := x.(type) {
	case nil:
		return "nothing"
	case Stringer:
		return v.String()
	case Shape:
		return v.Name()
	case int:
		return "an int"
	}
	return "something else"
}

func main() {
	shapes := []Shape{Rect{2, 3}, Square(4)}
	println(total(shapes))
	for _, s := range shapes {
		println(s.Name(), describe(s))
	}
	println(describe(nil), describe(7), describe("x"))

	// x.(T) takes the value out (a panic if it holds another type);
	// with ok it reports instead.
	sq := shapes[1].(Square)
	r, ok := shapes[1].(Rect)
	println(int(sq), r.W, ok)

	// Interface values are equal when they hold equal values of the
	// same type.
	var a, b Shape = Rect{2, 3}, Rect{2, 3}
	println(a == b, a == shapes[0], a == Shape(Square(6)))
	var none Shape
	println(none == nil)
}
```

```text
22
rect rect
square a square
nothing an int something else
4 0 false
true true false
true
```

- **Satisfying an interface** needs the methods, with value receivers,
  a type's own or promoted from an embedded field (an embedded
  interface's included). Interfaces can embed other interfaces (`type ReadWriter interface {
  Reader; Writer }`), and one interface value can be assigned to another
  interface whose methods it has. The methods of gogo's own types
  (`bytes.Bytes`, `bytes.Buffer`, `fs.File`) are builtins of the VM and
  do not count: `var s Stringer = b` for a `Bytes` b is a compile
  error; wrap it in a struct of your own.
- **`any`** (the empty interface) holds any value. Inside one, a value
  keeps its type: an `int64` is not an `int`, a `Celsius` (`type Celsius
  int`) is neither, and a `byte` or a `uint32` is not an `int` either.
- **Type switches** take interface types as cases too (`case Stringer:`
  matches every value whose type has `String() string`), and `case nil`
  matches the nil interface value.
- **Type assertions:** `v := x.(T)` gives the value as a `T` and panics
  (`interface conversion: ...`) if `x` holds another type; `v, ok :=
  x.(T)` sets `ok` to false instead. `T` may be an interface type.
- **`==`** compares interface values by the type and the value they hold
  (structs field by field). `x == nil` is true only for the nil
  interface value. A method call on a nil interface value panics, as in
  Go.
- **`error`** is the predefined interface `interface{ Error() string }`
  (section 3.10).

How it works: gogo knows every type in your program when it compiles it,
so a method call on an interface value is a call of a small function the
compiler writes, which checks the value's type against the types of your
program that have that method and calls the right one. A value goes into
an interface as it is; a named basic value (and a `byte`) is wrapped in a
small box so that its type can be told apart, which costs an allocation
unless the value is a constant.

Interface values cross actors in messages like any other value. A type
of your program is your module's own (chapter 14): a value of it, in an
interface or not, cannot be sent to a hot-loaded module, and the module's
types cannot be sent to you.

## 3.10 Errors

As in Go, a function that can fail returns an `error` as its last result,
`nil` for success. `error` is the predefined interface `interface{ Error()
string }`, so any type with that method is one (section 3.9), and
`gogo/errors` has what Go's `errors` has (but `As`):

- `errors.New(text)` makes an error whose `Error()` is `text`;
- `errors.Is(err, target)` reports whether `err`, or an error it wraps
  (through `Unwrap() error` methods), is `target`; `errors.Unwrap(err)`
  takes one layer off;
- `errors.Code` is gogo's one set of error codes, shared by every
  package (`errors.NotFound`, `errors.Busy`, ...; appendix C). A `Code`
  is an `error`: its `Error()` says what it means (`"not found"`), and
  `err == errors.NotFound` works whichever package returned `err`.

```go
// Errors are values of the interface error. gogo/errors has Go's New, Is
// and Unwrap, and Code, the shared error codes, which are errors too.
package main

import (
	"gogo/errors"
	"gogo/strconv"
)

// ErrEmpty is a sentinel error, compared with ==.
const ErrEmpty = errors.BadArg

// RangeError is an error type of our own, with the details.
type RangeError struct {
	N   int
	Err error // what it wraps
}

func (e RangeError) Error() string { return strconv.Itoa(e.N) + " is out of range" }
func (e RangeError) Unwrap() error { return e.Err }

// parsePercent parses "0".."100".
func parsePercent(s string) (int, error) {
	if s == "" {
		return 0, ErrEmpty
	}
	n, err := strconv.Atoi(s)
	if err != nil {
		return 0, errors.New("not a number: " + s)
	}
	if n < 0 || n > 100 {
		return 0, RangeError{n, errors.Overflow}
	}
	return n, nil
}

func main() {
	for _, s := range []string{"42", "", "x1", "250"} {
		n, err := parsePercent(s)
		switch {
		case err == nil:
			println("["+s+"]", "=", n)
		case err == ErrEmpty:
			println("["+s+"]", "empty:", err.Error())
		case errors.Is(err, errors.Overflow):
			println("["+s+"]", "too big:", err.Error())
		default:
			println("["+s+"]", "error:", err.Error())
		}
	}
}
```

```text
[42] = 42
[] empty: bad argument
[x1] error: not a number: x1
[250] too big: 250 is out of range
```

Compare an error with a sentinel by `==` (`err == errors.NotFound`), or
with `errors.Is` when it may be wrapped; a type switch or `err.(T)` gets
at an error type's details. Unlike Go, two errors from `errors.New` with
the same text are equal: an error is a value, compared by what it holds.

> **Warning.** `gogo/fs` and `gogo/code` still return an `errors.Code`
> (`fs.Err`, `code.Err`), not an `error`, and their success is
> `errors.OK`, the code 0. Put into an `error`, `errors.OK` is a
> non-nil error, like a typed nil pointer in Go: `return
> fs.WriteFile(p, b)` in a function that returns `error` would report
> every success as a failure. The compiler rejects that form, and the
> same in a `var` or an assignment (`errors.Code result used as an
> error`); passing the result to compare it (`errors.Is(fs.Remove(p),
> errors.NotFound)`) and an explicit `error(...)` conversion are
> allowed. Check the code first:
>
> ```go
> func save(p string, b bytes.Bytes) error {
> 	if c := fs.WriteFile(p, b); c != fs.OK {
> 		return c
> 	}
> 	return nil
> }
> ```
>
> The functions that return `error` (`port.Open`, `strconv.Atoi`,
> `http.Get`, ...) return a true `nil` on success.

For failures you do not expect and cannot handle, gogo's answer is
different: let the actor crash, and let a supervisor restart it. That is
the subject of chapter 5. `panic(v)` crashes the current actor on
purpose: it prints `panic: ` and `v` on standard error and ends the actor
with the reason `actor.Panic`. There is no `recover`. To stop the current
actor with a code of your own instead, call `actor.Exit(code)` with a
code of 100 or more.

## 3.11 What is not supported

Here is a program that uses many of the features gogo leaves out:

```go
// This program does NOT compile: it collects the Go features the gogo
// subset rejects, so you can see the compiler's messages.
package main

var counter = 0 // fine: package-level variables are read-only

type Shape interface{ Area() int } // fine: interfaces are supported

type Sq struct{ S int }

func (s *Sq) Area() int { return s.S * s.S } // fine: pointer receivers are supported

func Max[T int | int64](a, b T) T { // generics
	if a > b {
		return a
	}
	return b
}

func main() {
	ch := make(chan int) // channel
	go func() { ch <- 1 }()
	sq := []Sq{{2}}
	defer recover()    // recover
	m := map[*Sq]int{} // a map with pointer keys
	p := &sq[0]        // a pointer into a slice
	x := 1.5i          // complex
	if counter > 0 {
		goto done // goto
	}
	counter++ // a write to a package-level variable
done:
	var a any = [4]int{} // array in an interface
	y := any(1).(int)    // fine: type assertions are supported
	_ = y
	println(m, p, x, a)
	println(recover())
}
```

`gogo build` reports each one:

```text
main.gg:13:1: gogo: generic function not supported
main.gg:21:8: gogo: channel not supported
main.gg:22:2: gogo: go statement not supported
main.gg:24:8: gogo: recover (a panic ends the actor: let its supervisor restart it) not supported
main.gg:25:7: gogo: map with key type *main.Sq: a pointer key (no pointer keys: pointers move) not supported
main.gg:26:7: gogo: interior pointer &sq[0] not supported: gogo has no pointers into a slice element or a struct field; use a []*T or a *T field, or s[i].F = v
main.gg:27:7: gogo: complex not supported
main.gg:29:3: gogo: goto statement not supported
main.gg:31:2: gogo: package-level variable counter is read-only
main.gg:32:1: gogo: label on a statement other than for or switch not supported
main.gg:33:14: gogo: array in interface not supported: use a slice or a struct with an array field
main.gg:36:10: gogo: map with key type *main.Sq: a pointer key (no pointer keys: pointers move) not supported
main.gg:36:16: gogo: complex not supported
main.gg:37:10: gogo: recover (a panic ends the actor: let its supervisor restart it) not supported
```

The full list, with what to do instead:

| Go feature | Error | gogo way |
|---|---|---|
| `go f()` | `go statement not supported` | `actor.Spawn(f)` |
| channels, `select`, `<-` | `channel not supported`, `operator <- not supported` | messages: `actor.Send`, `actor.Receive` |
| `defer` in a loop | `defer inside a loop not supported: deferred calls are static (move the loop body into a function)` | a function per iteration |
| `recover()` | `recover (a panic ends the actor: let its supervisor restart it) not supported` | let the actor exit; a supervisor restarts it (chapter 5); ports and files close automatically |
| `error` | `error not supported` | `bool` or an `int` code type |
| writing a package-level `var` | `package-level variable counter is read-only` | state in an actor |
| a map with pointer keys | `map with key type *T: a pointer key (no pointer keys: pointers move) not supported` | a key the pointer's target identifies |
| a pointer into a slice element or a field, `&s[i]`, `&v.F` | `interior pointer &s[i] not supported` | a `[]*T` or a `*T` field, or `s[i].F = v` |
| `goto` | `goto statement not supported` | labeled `break`/`continue`, or a function and `return` |
| `complex64/128` | `complex not supported` | two floats |
| `uintptr` | `uintptr not supported` | `uint32` |
| generics | `generic function not supported` | write the function per type |
| `fmt`, other std packages | `import "fmt": package not found` | `println`, `gogo/strconv` |
| an array in `any` | `array in interface not supported` | a slice, or a struct holding the array |
| `func init()` | `func init not supported` | call it from `main` |

### Small surprises

- `println` accepts any number of integer, float, `bool`, `string`, PID,
  Ref, struct and slice arguments (structs print as `{1 2}`, slices as
  `[1 2]`, unlike Go), but not `Bytes` or `Buffer` (use `.String()`), not
  a struct or slice holding `uint32` or `uint64` values, and not a
  multi-result call (`println(divmod(7, 2))` is `multiple values not
  supported`). An interface value prints the value it holds (Go prints two
  addresses); a sized integer or a named basic value inside one prints in
  its box, `{5}`.
- A two-result gogo builtin cannot be a statement on its own: `actor.
  Call(p, req, 100)` on a line by itself is rejected with `multiple values
  not supported`. Write `_, _ = actor.Call(p, req, 100)`. Your own
  multi-result functions can be called as statements.
- The zero value of a PID cannot be written as a literal
  (`actor.PID{}` is rejected); declare one: `var none actor.PID`.
- Unary `+` is rejected (`operator + not supported`).

With the language in hand, we can turn to what gogo is really about:
actors.
