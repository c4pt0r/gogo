package compiler

import (
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"testing/fstest"

	"gogotools/lib"
)

func writeProg(t *testing.T, src string) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "main.gg"), []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

// TestLoadSourceExt: a package is its .gg files; .go files are not read,
// and a directory with only .go files says to rename them.
func TestLoadSourceExt(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "main.go"), []byte("package main\n\nfunc main() {}\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(dir); err == nil || !strings.Contains(err.Error(), "no .gg files, only .go files") || !strings.Contains(err.Error(), "rename main.go to main.gg") {
		t.Fatalf("only .go files: err = %v", err)
	}
	if _, err := Load(t.TempDir()); err == nil || !strings.HasSuffix(err.Error(), ": no .gg files") {
		t.Fatalf("empty dir: err = %v", err)
	}
	// A .go file next to the .gg files is ignored (here it would not type-check).
	if err := os.WriteFile(filepath.Join(dir, "main.gg"), []byte("package main\n\nfunc main() {}\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(dir); err != nil {
		t.Fatalf(".gg beside .go: %v", err)
	}
}

// firstErr returns the first error of a (possibly joined) error.
func firstErr(err error) error {
	var j interface{ Unwrap() []error }
	if errors.As(err, &j) && len(j.Unwrap()) > 0 {
		return j.Unwrap()[0]
	}
	return err
}

const acceptSrc = `package main

import "gogo/actor"

type Point struct{ X, Y int }

type Msg struct {
	From actor.PID
	N    int
	Tag  string
	P    Point
	Ok   bool
}

type Done struct{}

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func noResult(a, b int) {
	println(a, b)
}

func main() {
	p := Point{X: 1, Y: 2}
	q := Point{3, 4}
	sum := p.X + q.Y*2 - 7/2 + 9%4
	sum = -sum
	for i := 0; i < 10; i++ {
		if i == 3 {
			continue
		} else if i > 7 {
			break
		}
		sum += i
	}
	for sum > 0 {
		sum = sum - 1
	}
	for {
		break
	}
	ok := !(sum <= 1 && sum >= 0) || sum != 2
	const k = 5
	var z int = k
	noResult(z, fib(10))
	println("fib", fib(10), ok, true)
	me := actor.Self()
	child := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Msg:
			actor.Send(m.From, Done{})
			println(m.N, m.P.X, m.Tag)
		case Done:
			println("done")
		default:
			println("other")
		}
	})
	actor.Send(child, Msg{From: me, N: 1, Tag: "hi", P: p, Ok: true})
	actor.Send(child, 42)
	if child == me || child != me {
		println(actor.Info(me).Memory)
	}
	info := actor.Info(child)
	println(info.Memory)
}
`

func TestAcceptsM1(t *testing.T) {
	prog, err := Load(writeProg(t, acceptSrc))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
}

// TestAcceptsBitops: every bitwise operator, unary ^ and compound
// assignment on int, int64, byte and named types of them.
func TestAcceptsBitops(t *testing.T) {
	src := `package main

type D int64

func main() {
	a, b := 6, 3
	var x, y int64 = 6, 3
	var p, q byte = 6, 3
	var d D = 5
	s := []byte{1, 2}
	println(a&b, a|b, a^b, a&^b, a<<b, a>>b, ^a)
	println(x&y, x|y, x^y, x&^y, x<<y, x>>y, ^x, x<<p, a<<x)
	println(p&q, p|q, p^q, p&^q, p<<q, p>>q, ^p, d&d, d<<1, ^d)
	a &= b
	a |= b
	a ^= b
	a &^= b
	a <<= b
	a >>= b
	x &= y
	x <<= p
	p >>= 9
	d ^= d
	s[0] <<= 3
	s[1] &^= 1
	println(a, x, p, d, s[0], 1<<a)
}
`
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
}

// Each source marks the offending line with "// ERR".
func TestRejects(t *testing.T) {
	tests := []struct {
		name string
		src  string
		want []string
	}{
		{"chan", "package main\nfunc main() {\n\tch := make(chan int) // ERR\n\t_ = ch\n}\n", []string{"channel"}},
		{"go", "package main\nfunc f() {}\nfunc main() {\n\tgo f() // ERR\n}\n", []string{"go statement"}},
		{"map", "package main\nfunc main() {\n\tm := map[string]int{} // ERR\n\t_ = m\n}\n", []string{"map"}},
		{"defer", "package main\nfunc f() {}\nfunc main() {\n\tdefer f() // ERR\n}\n", []string{"defer"}},
		{"float", "package main\nfunc main() {\n\tx := 1.5 // ERR\n\t_ = x\n}\n", []string{"float"}},
		{"method-pointer-receiver", "package main\ntype P struct{ X int }\nfunc (p *P) M() {} // ERR\nfunc main() {}\n", []string{"pointer receivers are not supported yet"}},
		{"method-pointer-call", "package main\ntype P struct{ X int }\nfunc (p *P) M() {} // ERR\nfunc main() {\n\tp := P{}\n\tp.M()\n}\n", []string{"pointer receivers are not supported yet"}},
		{"method-interface", "package main\ntype S interface{ Area() int } // ERR\nfunc main() {}\n", []string{"interface types are not supported yet"}},
		{"method-interface-param", "package main\nfunc f(s interface{ M() }) {} // ERR\nfunc main() {}\n", []string{"interface not supported"}},
		{"method-on-slice-type", "package main\ntype L []int // ERR\nfunc (l L) Len() int { return len(l) }\nfunc main() {}\n", []string{"type definition"}},
		{"method-value-any", "package main\ntype P struct{ X int }\nfunc (p P) M() {}\nfunc main() {\n\tvar x any = P{}.M // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"method-on-named-byte", "package main\ntype B byte // ERR\nfunc (b B) M() {}\nfunc main() {}\n", []string{"type definition"}},
		{"slice-typeswitch", "package main\nfunc main() {\n\tvar x any = []int{1}\n\tswitch x.(type) {\n\tcase []int: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"slice-keyed", "package main\nfunc main() {\n\ts := []int{2: 1} // ERR\n\t_ = s\n}\n", []string{"keyed slice element"}},
		{"slice-of-map", "package main\nfunc main() {\n\tvar s []map[int]int // ERR\n\t_ = s\n}\n", []string{"slice of map"}},
		{"slice-3index", "package main\nfunc main() {\n\ts := []int{1, 2}\n\tprintln(len(s[0:1:2])) // ERR\n}\n", []string{"3-index"}},
		{"slice-named", "package main\ntype B []byte // ERR\nfunc main() {}\n", []string{"type definition"}},
		{"variadic-multi", "package main\nfunc f(xs ...int) {}\nfunc g() (int, int) { return 1, 2 }\nfunc main() {\n\tf(g()) // ERR\n}\n", []string{"multiple values not supported"}},
		{"variadic-any-byte", "package main\nfunc f(xs ...any) {}\nfunc main() {\n\tvar b byte = 1\n\tf(1, b) // ERR\n}\n", []string{"byte in interface not supported"}},
		{"variadic-builtin-spread", "package main\nfunc main() {\n\ts := []any{1}\n\tprintln(s...) // ERR\n}\n", []string{"println"}},
		{"nil-any", "package main\nfunc main() {\n\tvar x any = nil // ERR\n\t_ = x\n}\n", []string{"nil"}},
		{"slice-byte-any", "package main\nfunc main() {\n\tb := []byte(\"a\")\n\tvar x []any\n\tx = append(x, b[0]) // ERR\n\t_ = x\n}\n", []string{"byte in interface"}},
		{"string-index-assign", "package main\nfunc main() {\n\ts := \"ab\"\n\ts[0] = 1 // ERR\n}\n", []string{"cannot assign"}},
		{"funcvalue-any", "package main\nfunc f() {}\nfunc main() {\n\tvar x any = f // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-send", "package main\nimport \"gogo/actor\"\nfunc f() {}\nfunc main() {\n\tactor.Send(actor.Self(), f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-println", "package main\nfunc f() {}\nfunc main() {\n\tprintln(f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-compare", "package main\nfunc f() {}\nfunc main() {\n\tg := f\n\tprintln(g == f) // ERR\n}\n", []string{"compared to nil"}},
		{"funcvalue-actor", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tg := actor.Self // ERR\n\t_ = g\n}\n", []string{"selector other than a field read"}},
		{"funcvalue-variadic-badelem", "package main\nfunc run(f func(xs ...float64)) {} // ERR\nfunc main() {}\n", []string{"func type func(xs ...float64) not supported"}},
		{"funcvalue-badparam", "package main\nfunc run(f func(m map[int]int) int) {} // ERR\nfunc main() {}\n", []string{"func type func(m map[int]int) int not supported"}},
		{"funcvalue-badresult", "package main\nfunc main() {\n\tf := func() float64 { return 1 } // ERR\n\t_ = f\n}\n", []string{"func type func() float64 not supported"}},
		{"funcvalue-args-any", "package main\nfunc main() {\n\tvar x any = func(n int) int { return n } // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-result-any", "package main\nfunc id(n int) int { return n }\nfunc pair() (func(int) int, int) { return id, 1 }\nfunc main() {\n\tvar a any\n\tvar b int\n\ta, b = pair() // ERR\n\t_, _ = a, b\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-nil", "package main\nfunc main() {\n\tvar f func(int)\n\tprintln(f == nil) // ERR\n}\n", []string{"comparison of func(int)"}},
		{"import", "package main\nimport \"fmt\" // ERR\nfunc main() { fmt.Println() }\n", []string{`import "fmt"`}},
		{"fieldassign", "package main\ntype P struct{ X int }\nfunc main() {\n\tp := P{}\n\tp.X = 1 // ERR\n}\n", []string{"field assignment"}},
		{"pointer", "package main\nfunc main() {\n\tx := 1\n\tp := &x // ERR\n\t_ = p\n}\n", []string{"pointer"}},
		{"array", "package main\nfunc main() {\n\tvar a [3]int // ERR\n\t_ = a\n}\n", []string{"array"}},
		{"panic", "package main\nfunc main() {\n\tpanic(1) // ERR\n}\n", []string{"panic"}},
		{"range-func", "package main\nfunc seq(yield func(int) bool) {}\nfunc main() {\n\tfor i := range seq { // ERR\n\t\t_ = i\n\t}\n}\n", []string{"range over func"}},
		{"range-int-assign", "package main\nfunc main() {\n\tvar i int\n\tfor i = range 3 { // ERR\n\t}\n\tprintln(i)\n}\n", []string{"range assigning to existing variables"}},
		{"string-sub", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s[0] - s[0], len(s))\n\tt := s[:1] + s[0:] // ok\n\tprintln(t)\n\tvar i int64\n\tprintln(s[i]) // ERR\n}\n", []string{"index of type int64"}},
		{"string-slice3", "package main\nfunc main() {\n\ts := \"abc\"\n\tprintln(s[0:1:2]) // ERR\n}\n", []string{"3-index"}},
		{"range-assign", "package main\nfunc main() {\n\tvar i int\n\tfor i = range \"ab\" { // ERR\n\t}\n\tprintln(i)\n}\n", []string{"range"}},
		{"rune-any", "package main\nfunc main() {\n\tfor _, c := range \"ab\" {\n\t\tvar x any = c // ERR\n\t\t_ = x\n\t}\n}\n", []string{"rune in interface"}},
		{"conversion", "package main\ntype C bool\nfunc main() {\n\tx := true\n\ty := C(x) // ERR\n\t_ = y\n}\n", []string{"conversion"}},
		{"conversion-string", "package main\nfunc main() {\n\tx := 65\n\ts := string(x) // ERR\n\t_ = s\n}\n", []string{"conversion"}},
		{"conversion-named-string", "package main\ntype T string\nfunc main() {\n\tx := \"a\"\n\tt := T(x) // ERR\n\t_ = t\n}\n", []string{"conversion"}},
		{"int64-mix", "package main\nfunc main() {\n\tvar a int64 = 1\n\tb := 2\n\tprintln(a + b) // ERR\n}\n", []string{"mismatched types int64 and int"}},
		{"int64-mix-assign", "package main\nfunc main() {\n\tvar a int64\n\tb := 2\n\ta = b // ERR\n\t_ = a\n}\n", []string{"cannot use b"}},
		{"int32-conv", "package main\nfunc main() {\n\tx := 1\n\ty := int32(x) // ERR\n\t_ = y\n}\n", []string{"int32"}},
		{"switch-any", "package main\nfunc main() {\n\tvar x any = 1\n\tswitch x { // ERR\n\tcase 1:\n\t}\n}\n", []string{"switch on a value of type any not supported"}},
		{"switch-struct", "package main\ntype P struct{ X int }\nfunc main() {\n\tp := P{1}\n\tswitch p { // ERR\n\tcase P{1}:\n\t}\n}\n", []string{"switch on a value of type main.P not supported"}},
		{"switch-case-any", "package main\nfunc main() {\n\tx := 1\n\tvar a any = 1\n\tswitch x {\n\tcase a: // ERR\n\t}\n}\n", []string{"switch case of type any not supported"}},
		{"switch-slice", "package main\nfunc main() {\n\ts := []int{1}\n\tswitch s { // ERR\n\tcase nil:\n\t}\n}\n", []string{"switch on a value of type []int not supported"}},
		{"goto", "package main\nfunc main() {\n\tgoto end // ERR\nend:\n\tfor {\n\t}\n}\n", []string{"goto statement not supported"}},
		{"label-block", "package main\nfunc main() {\nL: // ERR\n\t{\n\t\tprintln(1)\n\t}\n\tgoto L\n}\n", []string{"label on a statement other than for or switch not supported"}},
		{"error", "package main\nfunc f() error { return nil } // ERR\nfunc main() { f() }\n", []string{"error"}},
		{"bit-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s & s) // ERR\n}\n", []string{"operator &"}},
		{"bit-bool", "package main\nfunc main() {\n\tb := true\n\tprintln(b | b) // ERR\n}\n", []string{"operator |"}},
		{"shift-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s << 1) // ERR\n}\n", []string{"shift"}},
		{"xor-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(^s) // ERR\n}\n", []string{"operator ^"}},
		{"andnot-assign-string", "package main\nfunc main() {\n\ts := \"a\"\n\ts &^= s // ERR\n}\n", []string{"operator &^"}},
		{"init", "package main\nfunc init() {} // ERR\nfunc main() {}\n", []string{"func init not supported"}},
		{"intcast", "package main\nfunc main() {\n\tvar x int32 = 1 // ERR\n\t_ = x\n}\n", []string{"int32"}},
		{"namedbasic-any", "package main\ntype C int\nfunc main() {\n\tvar x any = C(1) // ERR\n\t_ = x\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-var", "package main\ntype C int\nfunc main() {\n\tvar c C = 1\n\tvar x any\n\tx = c // ERR\n\t_ = x\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-arg", "package main\ntype C bool\nfunc f(v any) {}\nfunc main() {\n\tvar c C\n\tf(c) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-send", "package main\nimport \"gogo/actor\"\ntype C string\nfunc main() {\n\tvar c C = \"x\"\n\tactor.Send(actor.Self(), c) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-field", "package main\ntype C int\ntype B struct{ V any }\nfunc main() {\n\tvar c C\n\tprintln(B{c}) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-return", "package main\ntype C int\nfunc f() any {\n\tvar c C\n\treturn c // ERR\n}\nfunc main() { println(f()) }\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-case", "package main\ntype C int\nfunc main() {\n\tvar x any = 1\n\tswitch x.(type) {\n\tcase C: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"multiret-println", "package main\nfunc f() (int, int) { return 1, 2 }\nfunc main() {\n\tprintln(f()) // ERR\n}\n", []string{"multiple values not supported"}},
		{"multiret-var", "package main\nfunc f() (int, int) { return 1, 2 }\nfunc main() {\n\tvar a, b = f() // ERR\n\tprintln(a, b)\n}\n", []string{"multiple values not supported"}},
		{"multiret-builtin-return", "package main\nimport \"gogo/actor\"\nfunc f() (any, bool) {\n\treturn actor.ReceiveTimeout(5) // ERR\n}\nfunc main() { a, b := f(); println(a, b) }\n", []string{"multiple values not supported"}},
		{"multiret-named-any", "package main\ntype C int\nfunc g() (C, int) { return 1, 2 }\nfunc f() (any, int) {\n\treturn g() // ERR\n}\nfunc main() { a, b := f(); println(a, b) }\n", []string{"named basic type in interface not supported"}},
		{"multiret-arg-any", "package main\ntype C int\nfunc g() (C, int) { return 1, 2 }\nfunc h(a any, b int) {}\nfunc main() {\n\th(g()) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"byte-any", "package main\nfunc main() {\n\tvar c byte = 3\n\tvar x any = c // ERR\n\t_ = x\n}\n", []string{"byte in interface not supported"}},
		{"byte-send", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 3\n\tactor.Send(actor.Self(), byte(x)) // ERR\n}\n", []string{"byte in interface not supported"}},
		{"byte-case", "package main\nfunc main() {\n\tvar x any = 1\n\tswitch x.(type) {\n\tcase byte: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"byte-named", "package main\ntype B byte // ERR\nfunc main() {}\n", []string{"type definition"}},
		{"uint16", "package main\nfunc main() {\n\tvar x uint16 = 1 // ERR\n\t_ = x\n}\n", []string{"uint16"}},
		{"tworesult-single", "package main\nimport \"gogo/actor\"\nfunc main() {\n\ta, b := actor.Self() // ERR\n\t_, _ = a, b\n}\n", []string{"assignment mismatch"}},
		{"tworesult-stmt", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tactor.ReceiveTimeout(5) // ERR\n}\n", []string{"multiple values not supported"}},
		{"tworesult-var", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar m, ok = actor.ReceiveTimeout(5) // ERR\n\tprintln(m, ok)\n}\n", []string{"multiple values not supported"}},
		{"ref-literal", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tr := actor.Ref{} // ERR\n\tprintln(r)\n}\n", []string{"composite literal of actor.Ref not supported"}},
		{"any-compare-right-named", "package main\ntype C int\nfunc main() {\n\tvar c C = 3\n\tvar x any = 3\n\tprintln(c == x) // ERR\n}\n", []string{"comparison of any"}},
		{"any-compare-right-const", "package main\nfunc main() {\n\tvar x any = 5\n\tprintln(5 == x) // ERR\n}\n", []string{"comparison of any"}},
		{"any-compare-right-pid", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar x any = 5\n\tprintln(actor.Self() != x) // ERR\n}\n", []string{"comparison of any"}},
		{"any-compare", "package main\nfunc main() {\n\tvar a, b any\n\tprintln(a == b) // ERR\n}\n", []string{"comparison of any"}},
		{"iface-literal", "package main\nfunc main() {\n\tvar x interface{ M() } // ERR\n\t_ = x\n}\n", []string{"interface"}},
		{"captured-reassign-after", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 1\n\tactor.Spawn(func() { println(x) })\n\tx = 2 // ERR\n}\n", []string{"captured variable must not be reassigned"}},
		{"captured-reassign-before", "package main\nfunc main() {\n\tvar x int\n\tx = 1 // ERR\n\tf := func() { println(x) }\n\t_ = f\n}\n", []string{"captured variable must not be reassigned"}},
		{"captured-reassign-loop", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tfor i := 0; i < 3; i++ { // ERR\n\t\tactor.Spawn(func() { println(i) })\n\t}\n}\n", []string{"captured variable must not be reassigned"}},
		{"captured-reassign-param", "package main\nfunc f(n int) func() {\n\tn = n + 1 // ERR\n\treturn func() { println(n) }\n}\nfunc main() { _ = f(1) }\n", []string{"captured variable must not be reassigned"}},
		{"captured-reassign-redeclare", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 1\n\tactor.Spawn(func() { println(x) })\n\tx, y := 2, 3 // ERR\n\t_ = y\n}\n", []string{"captured variable must not be reassigned"}},
		{"funcvalue-return-any", "package main\nfunc f() {}\nfunc g() any {\n\treturn f // ERR\n}\nfunc main() { _ = g() }\n", []string{"func value in interface not supported"}},
		{"funcvalue-anyfield", "package main\ntype B struct{ V any }\nfunc f() {}\nfunc main() {\n\tb := B{V: f} // ERR\n\t_ = b\n}\n", []string{"func value in interface not supported"}},
		{"captured-assign-nested", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\tf := func() {\n\t\t\tx = 2 // ERR\n\t\t}\n\t\tactor.Spawn(f)\n\t})\n\tprintln(x)\n}\n", []string{"captured variable"}},
		{"captured-named-return", "package main\nfunc f() (r int, g func() int) {\n\th := func() int { return r }\n\treturn 5, h // ERR\n}\nfunc main() { n, g := f(); println(n, g()) }\n", []string{"captured named result must not be set by return"}},
		{"captured-named-return-call", "package main\nfunc pair() (int, func() int) { return 1, func() int { return 0 } }\nfunc f() (r int, g func() int) {\n\th := func() int { return r }\n\t_ = h\n\treturn pair() // ERR\n}\nfunc main() { n, _ := f(); println(n) }\n", []string{"captured named result must not be set by return"}},
		{"captured-named-return-lit", "package main\nfunc main() {\n\tf := func() (r int, g func() int) {\n\t\th := func() int { return r }\n\t\treturn 9, h // ERR\n\t}\n\tn, g := f()\n\tprintln(n, g())\n}\n", []string{"captured named result must not be set by return"}},
		{"captured-named-return-third", "package main\nfunc f() (a, b int, g func() int) {\n\tg = func() int { return a + b }\n\treturn 1, 2, g // ERR\n}\nfunc main() { x, y, g := f(); println(x, y, g()) }\n", []string{"captured"}},
		{"captured-opassign-before", "package main\nfunc main() {\n\tx := 1\n\tx += 1 // ERR\n\tf := func() { println(x) }\n\t_ = f\n}\n", []string{"captured variable must not be reassigned"}},
		{"import-unknown-lib", "package main\nimport \"gogo/nosuch\" // ERR\nfunc main() { nosuch.X() }\n", []string{`import "gogo/nosuch" not supported`}},
		{"fs-file-case", "package main\nimport (\n\t\"gogo/actor\"\n\t\"gogo/fs\"\n)\nfunc main() {\n\tvar x any = actor.Self()\n\tswitch x.(type) {\n\tcase fs.File: // ERR\n\t\tprintln(\"file\")\n\tcase actor.PID:\n\t}\n}\n", []string{"type switch case fs.File not supported; wrap it in a struct"}},
		{"port-open-stmt", "package main\nimport \"gogo/port\"\nfunc main() {\n\tport.Open(\"gpio\", port.Host{}) // ERR\n}\n", []string{"multiple values not supported"}},
		{"port-open-var", "package main\nimport \"gogo/port\"\nfunc main() {\n\tvar p, ok = port.Open(\"gpio\", port.Host{}) // ERR\n\tprintln(p, ok)\n}\n", []string{"multiple values not supported"}},
		{"fs-stat-stmt", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tfs.Stat(\"/flash\") // ERR\n}\n", []string{"multiple values not supported"}},
		{"fs-readfile-single", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tb := fs.ReadFile(\"/flash/a\", 9) // ERR\n\t_ = b\n}\n", []string{"assignment mismatch"}},
		{"fs-read-method-stmt", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar f fs.File\n\tf.Read(1) // ERR\n}\n", []string{"multiple values not supported"}},
		{"fs-size-var", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar f fs.File\n\tvar n, e = f.Size() // ERR\n\tprintln(n, e)\n}\n", []string{"multiple values not supported"}},
		{"fs-method-value", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar f fs.File\n\tg := f.Close // ERR\n\t_ = g\n}\n", []string{"selector other than a field read not supported"}},
		{"port-open-arg", "package main\nimport \"gogo/port\"\nfunc f(p any, ok bool) {}\nfunc main() {\n\tf(port.Open(\"gpio\", port.Host{})) // ERR\n}\n", []string{"multiple values not supported"}},
		{"bytes-buffer-case", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tvar x any = bytes.NewBuffer(1)\n\tswitch x.(type) {\n\tcase bytes.Buffer: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"bytes-literal", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.Bytes{} // ERR\n\t_ = x\n}\n", []string{"composite literal of bytes.Bytes"}},
		{"buffer-literal", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tb := bytes.Buffer{} // ERR\n\t_ = b\n}\n", []string{"zero value of bytes.Buffer", "use bytes.NewBuffer"}},
		{"buffer-var-zero", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tvar b bytes.Buffer // ERR\n\tb.WriteByte(1)\n}\n", []string{"zero value of bytes.Buffer", "use bytes.NewBuffer"}},
		{"buffer-field-omitted", "package main\nimport \"gogo/bytes\"\ntype T struct {\n\tN int\n\tB bytes.Buffer\n}\nfunc main() {\n\tt := T{N: 1} // ERR\n\tprintln(t.N)\n}\n", []string{"use bytes.NewBuffer"}},
		{"buffer-field-empty-lit", "package main\nimport \"gogo/bytes\"\ntype T struct{ B bytes.Buffer }\nfunc main() {\n\tt := []T{{}} // ERR\n\t_ = t\n}\n", []string{"use bytes.NewBuffer"}},
		{"bytes-println", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tprintln(x) // ERR\n}\n", []string{"println of bytes.Bytes"}},
		{"bytes-compare", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tprintln(x == x) // ERR\n}\n", []string{"comparison of"}},
		{"bytes-methodvalue", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tf := x.Len // ERR\n\t_ = f\n}\n", []string{"not supported"}},
		{"port-renamed", "package main\nimport p \"gogo/port\" // ERR\nfunc main() { p.Close(p.Edge{}) }\n", []string{`import p "gogo/port" not supported`}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			wantLine := 0
			for i, l := range strings.Split(tc.src, "\n") {
				if strings.Contains(l, "// ERR") {
					wantLine = i + 1
				}
			}
			var err error
			prog, lerr := Load(writeProg(t, tc.src))
			if lerr != nil {
				err = firstErr(lerr)
			} else if errs := Check(prog); len(errs) > 0 {
				err = errs[0]
			}
			if err == nil {
				t.Fatal("expected an error, got none")
			}
			msg := err.Error()
			for _, w := range tc.want {
				if !strings.Contains(msg, w) {
					t.Errorf("error %q lacks %q", msg, w)
				}
			}
			if !strings.Contains(msg, ":"+itoa(wantLine)+":") {
				t.Errorf("error %q not on line %d", msg, wantLine)
			}
		})
	}
}

func itoa(n int) string { return strconv.Itoa(n) }

// TestAcceptsMultiResultsAndByte: user functions with several (or named)
// results in every supported form, and the byte type.
func TestAcceptsMultiResultsAndByte(t *testing.T) {
	src := `package main

import "gogo/actor"

type P struct{ B byte }

func divmod(a, b int) (int, int) { return a / b, a % b }

func named(n int) (q, r int) {
	q, r = n/10, n%10
	return
}

func one() (n int) {
	n = 3
	return
}

func blank() (_ int, ok bool) { return }

func fwd(a, b int) (int, int) { return divmod(a, b) }

func boxed() (any, int64) {
	var x int64 = 1
	return x, x
}

func sum(a, b int) int { return a + b }

func pid() (actor.PID, any) { return actor.Self(), 1 }

func bytes(x int) (byte, int) {
	var c byte = 250
	c += byte(x)
	c++
	d := c * 2 / 3 % 7
	return -d, int(c) + int(uint8(x))
}

func main() {
	a, b := divmod(7, 2)
	a, b = b, a
	_, c := named(42)
	d, _ := fwd(9, 2)
	divmod(1, 1)
	println(a, b, c, d, one(), sum(divmod(9, 4)))
	_, ok := blank()
	var v any
	var n int64
	v, n = boxed()
	actor.Send(pid())
	e, f := bytes(3)
	println(ok, v, n, e, f, P{e}.B, e < 3, e == byte(f))
}
`
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
}

func TestIntIs32Bit(t *testing.T) {
	src := "package main\nconst c = 1 << 31\nfunc main() {\n\tvar x int = c\n\t_ = x\n}\n"
	_, err := Load(writeProg(t, src))
	if err == nil {
		t.Fatal("expected overflow error")
	}
	if !strings.Contains(firstErr(err).Error(), ":4:") {
		t.Errorf("error lacks position: %v", err)
	}
}

func TestLoadMain(t *testing.T) {
	if _, err := Load(writeProg(t, "package main\nfunc other() {}\n")); err != nil {
		t.Fatalf("a package without main is a module: %v", err)
	}
	if _, err := Load(writeProg(t, "package main\nfunc main(x int) {}\n")); err == nil {
		t.Fatal("expected error for func main with parameters")
	}
	if _, err := Load(writeProg(t, "package foo\nfunc main() {}\n")); err == nil {
		t.Fatal("expected error for non-main package")
	}
}

func checkSrc(t *testing.T, src string) []error {
	t.Helper()
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	return Check(prog)
}

func TestRejectsMore(t *testing.T) {
	const hdr = "package main\nimport \"gogo/actor\"\ntype P struct{ X int }\ntype Q struct{ Y int }\n"
	tests := []struct{ name, src, want string }{
		{"global-var", "package main\nvar g int\nfunc main() { println(g) }\n", "global variable"},
		{"captured-assign", hdr + "func main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\tx = 2\n\t})\n\tprintln(x)\n}\n", "captured variable"},
		{"captured-opassign", hdr + "func main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\tx += 2\n\t})\n\tprintln(x)\n}\n", "captured variable"},
		{"captured-incdec", hdr + "func main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\tx++\n\t})\n\tprintln(x)\n}\n", "captured variable"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			errs := checkSrc(t, tc.src)
			if len(errs) == 0 || !strings.Contains(errs[0].Error(), tc.want) {
				t.Fatalf("want error containing %q, got %v", tc.want, errs)
			}
		})
	}
}

func TestAcceptsForwardAndLocals(t *testing.T) {
	const src = "package main\nimport \"gogo/actor\"\ntype P struct{ X int }\nconst c = 3\nfunc main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\ty := x + c\n\t\ty = 2\n\t\ty++\n\t\tswitch m := actor.Receive().(type) {\n\t\tcase P:\n\t\t\tprintln(m.X, y)\n\t\tdefault:\n\t\t\tactor.Send(actor.Self(), m)\n\t\t}\n\t})\n}\n"
	if errs := checkSrc(t, src); len(errs) != 0 {
		t.Fatalf("unexpected: %v", errs)
	}
}

// COMP-1: a bare return leaves captured named results untouched, and a
// return with values is fine when no closure captures a named result
// (here the literal captures only the parameter).
func TestAcceptsNamedResultReturns(t *testing.T) {
	const src = "package main\nfunc f(n int) (r int, g func() int) {\n\tr = n\n\tg = func() int { return n }\n\treturn\n}\nfunc k(n int) (r int, g func() int) {\n\th := func() int { return n }\n\treturn n, h\n}\nfunc b() (r int, g func() int) {\n\tg = func() int { return r }\n\treturn\n}\nfunc main() {\n\tx, g := f(1)\n\ty, h := k(2)\n\tz, i := b()\n\tprintln(x, g(), y, h(), z, i())\n}\n"
	if errs := checkSrc(t, src); len(errs) != 0 {
		t.Fatalf("unexpected: %v", errs)
	}
}

func TestErrorsSortedByPosition(t *testing.T) {
	errs := checkSrc(t, "package main\nfunc f() (float64, float32, complex64) { return 0, 0, 0 }\nfunc g() (float64, int) { return 0, 0 }\nfunc main() {}\n")
	if len(errs) < 2 {
		t.Fatalf("want several errors, got %v", errs)
	}
	re := regexp.MustCompile(`:(\d+):(\d+): gogo`)
	prev := [2]int{}
	for _, e := range errs {
		m := re.FindStringSubmatch(e.Error())
		if m == nil {
			t.Fatalf("unparsable error %q", e)
		}
		l, _ := strconv.Atoi(m[1])
		c, _ := strconv.Atoi(m[2])
		cur := [2]int{l, c}
		if cur[0] < prev[0] || (cur[0] == prev[0] && cur[1] < prev[1]) {
			t.Errorf("errors not sorted: %v", errs)
		}
		prev = cur
	}
}

// Since M2, `any` is a first-class value: the binder of a default or
// multi-type clause may be copied and printed.
const acceptAnySrc = `package main

import "gogo/actor"

type P struct{ X int }

type Q struct{ Y int }

type C int

type Box struct {
	V any
	R actor.Ref
}

func id(v any) any { return v }

func double(c C) C { return c*2 + 1 }

func main() {
	switch m := actor.Receive().(type) {
	case P:
		println(m.X)
	case Q, int:
		println(m)
	default:
		y := m
		println(y)
		actor.Send(actor.Self(), m)
	}
	var a any = P{1}
	a = id(a)
	b := Box{V: "s", R: actor.MakeRef()}
	switch v := id(b.V).(type) {
	case string, bool:
		println(v)
	case actor.Ref:
		println(v == b.R, v != b.R)
	case actor.PID:
		println(v == actor.Self())
	}
	switch b.V.(type) {
	case int:
	}
	var c C = 3
	println(double(c) == 7, c < 4, c)
	m, ok := actor.ReceiveTimeout(10)
	println(m, ok)
	m, ok = actor.Call(actor.Self(), a, 100)
	p, found := actor.Whereis("x")
	p, _ = actor.Whereis("y")
	println(p, found, actor.Normal, actor.BadCode)
	child := actor.SpawnLink(func() { println(b.V) }) // a is reassigned above: not capturable
	child = actor.SpawnOpt(func() { println(b) }, actor.Options{MaxMemory: 4096})
	actor.Link(child)
	actor.Unlink(child)
	actor.TrapExit(true)
	actor.Kill(child, actor.Killed)
	r := actor.Monitor(child)
	actor.Demonitor(r)
	t := actor.SendAfter(5, child, b)
	println(actor.CancelTimer(t))
	actor.Sleep(1)
	actor.Reply(actor.Request{From: child, Ref: r, Body: a}, 1)
	println(actor.Register("x", child))
	actor.Unregister("x")
	println(actor.ExitMsg{}, actor.DownMsg{}, actor.Reason{Code: 100})
	actor.Exit(100)
}
`

func TestAcceptsAny(t *testing.T) {
	if errs := checkSrc(t, acceptAnySrc); len(errs) != 0 {
		t.Fatalf("unexpected: %v", errs)
	}
}

const acceptInt64Src = `package main

import "gogo/actor"

type Stamp struct {
	At int64
	N  int
}

const big int64 = 1 << 40

func later(t int64, d int) int64 { return t + int64(d) }

func main() {
	start := actor.Now()
	var z int64
	z = later(start, 5) - big
	z++
	z *= 2
	n := int(z) + 1
	println(z < big, z == 0, -z, n, int64(n), int64(int64(3)), int(4))
	var v any = Stamp{At: z}
	switch x := v.(type) {
	case int64:
		println(x)
	case Stamp:
		println(x.At)
	}
	actor.Send(actor.Self(), z)
}
`

func TestAcceptsInt64(t *testing.T) {
	if errs := checkSrc(t, acceptInt64Src); len(errs) != 0 {
		t.Fatalf("unexpected: %v", errs)
	}
}

const acceptFuncValSrc = `package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

type Job struct {
	Name string
	Run  func()
}

func work() { println("work") }

func pass(f func()) func() { return f }

func main() {
	me := actor.Self()
	var f func()
	f = work
	g := func() {
		h := func() { actor.Send(me, 1) }
		actor.Spawn(h)
	}
	j := Job{"j", pass(g)}
	actor.Spawn(j.Run)
	actor.SpawnLink(f)
	actor.SpawnOpt(work, actor.Options{})
	supervisor.Supervise(func() { println(me) }, supervisor.Spec{Restart: supervisor.Transient, MaxRestarts: 1, PeriodMs: 100})
	actor.Send(me, j)

	// Calling func values of any signature.
	f()
	j.Run()
	func() {}()
	pass(f)()
	add := func(a, b int) int { return a + b }
	println(add(1, 2))
	hs := []func(int) (int, string){func(n int) (int, string) { return n, "x" }}
	n, s := hs[0](3)
	println(n, s)
	hs[0](4)
	r := Rec{}
	println(r.F(r, 1) == 0)
	var bf func(byte, rune, int64, []byte, any) byte
	_ = bf
}

type Rec struct{ F func(Rec, int) int }
`

func TestAcceptsFuncValues(t *testing.T) {
	if errs := checkSrc(t, acceptFuncValSrc); len(errs) != 0 {
		t.Fatalf("unexpected: %v", errs)
	}
}

// fakeLibs is a library set with the real gogo/actor stubs plus extra
// packages given as dir/file.go -> source.
func fakeLibs(t *testing.T, extra map[string]string) fstest.MapFS {
	t.Helper()
	src, err := lib.StubFS.ReadFile("actor/actor.gg")
	if err != nil {
		t.Fatal(err)
	}
	psrc, err := lib.StubFS.ReadFile("port/port.gg")
	if err != nil {
		t.Fatal(err)
	}
	bsrc, err := lib.StubFS.ReadFile("bytes/bytes.gg")
	if err != nil {
		t.Fatal(err)
	}
	fsys := fstest.MapFS{"actor/actor.gg": {Data: src}, "port/port.gg": {Data: psrc}, "bytes/bytes.gg": {Data: bsrc}}
	for name, s := range extra {
		fsys[name] = &fstest.MapFile{Data: []byte(s)}
	}
	return fsys
}

// loadErr returns the first Load or Check error of src with library set libs.
func loadErr(t *testing.T, src string, libs fstest.MapFS) error {
	t.Helper()
	prog, err := LoadWith(writeProg(t, src), libs)
	if err != nil {
		return firstErr(err)
	}
	if errs := Check(prog); len(errs) > 0 {
		return errs[0]
	}
	return nil
}

func TestLibraries(t *testing.T) {
	const useLib = "package main\nimport \"gogo/lib\"\nfunc main() { lib.F() }\n"
	tests := []struct{ name, lib, want string }{
		{"lib-imports-fmt", "package lib\nimport \"fmt\"\nfunc F() { fmt.Println() }\n", `lib/lib.gg:2:8: gogo: import "fmt" not supported`},
		{"lib-imports-itself", "package lib\nimport \"gogo/lib\"\nfunc F() { lib.F() }\n", `import cycle gogo/lib -> gogo/lib`},
		{"lib-imports-missing", "package lib\nimport \"gogo/nolib\"\nfunc F() {}\n", `import "gogo/nolib" not supported`},
		{"lib-renamed-actor", "package lib\nimport a \"gogo/actor\"\nfunc F() { a.Self() }\n", `import a "gogo/actor" not supported`},
		{"lib-wrong-package", "package other\nfunc F() {}\n", "package other"},
		{"lib-global", "package lib\nvar g int\nfunc F() { println(g) }\n", "lib/lib.gg:2:1: gogo: global variable not supported"},
		{"lib-subset", "package lib\nfunc F() {\n\tm := map[int]int{}\n\t_ = m\n}\n", "lib/lib.gg:3:7: gogo: map not supported"},
		{"lib-type-error", "package lib\nfunc F() { undefined() }\n", "undefined"},
		{"lib-captured-reassign", "package lib\nimport \"gogo/actor\"\nfunc F() {\n\tx := 1\n\tx = 2\n\tactor.Spawn(func() { println(x) })\n}\n", "lib/lib.gg:5:2: gogo: captured variable must not be reassigned"},
		{"lib-imports-port", "package lib\nimport (\n\t\"gogo/actor\"\n\t\"gogo/port\"\n)\nfunc F() { port.Close(actor.Self()) }\n", ""},
		{"lib-ok", "package lib\nimport \"gogo/actor\"\nfunc F() { actor.Spawn(func() { println(1) }) }\n", ""},
		{"lib-bytes-ok", "package lib\nimport \"gogo/bytes\"\nfunc F() {\n\tb := bytes.NewBuffer(4)\n\tb.WriteString(\"x\")\n\tprintln(b.Freeze().Len())\n}\n", ""},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := loadErr(t, useLib, fakeLibs(t, map[string]string{"lib/lib.gg": tc.lib}))
			if tc.want == "" {
				if err != nil {
					t.Fatalf("unexpected: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("want error containing %q, got %v", tc.want, err)
			}
		})
	}
}

// TestAcceptsBytes: the gogo/bytes API, method syntax included, in every
// position the subset allows.
func TestAcceptsBytes(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Frame struct {
	N    int
	Body bytes.Bytes
	Buf  bytes.Buffer
}

func two() (int, int) { return 1, 2 }

func main() {
	b := bytes.NewBuffer(8)
	b.WriteString("ab")
	b.Write([]byte("cd"))
	b.WriteByte('e')
	b.WriteBytes(bytes.FromString("f"))
	x := b.Freeze()
	var zero bytes.Bytes
	p := make([]byte, 2)
	println(b.Len(), x.Len(), x.At(0), x.Slice(two()).String(), x.Equal(zero), x.CopyTo(p))
	f := Frame{N: 1, Body: x, Buf: bytes.NewBuffer(0)}
	var g bytes.Buffer = bytes.NewBuffer(1)
	println(Frame{2, x, g}.N)
	actor.Send(actor.Self(), f)
	actor.Send(actor.Self(), x)
	for i := 0; i < 2; i++ {
		switch m := actor.Receive().(type) {
		case Frame:
			println(m.Body.Len())
		case bytes.Bytes:
			println(m.String())
		}
	}
	_ = bytes.FromSlice(p)
}
`
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
	if _, err := Compile(prog); err != nil {
		t.Fatalf("Compile: %v", err)
	}
}

// Libraries may import other libraries (acyclically); they load in
// dependency order, each once.
func TestLibraryImportsLibrary(t *testing.T) {
	libs := fakeLibs(t, map[string]string{
		"a/a.gg": "package a\nimport \"gogo/b\"\nfunc F() int { return b.G() + 1 }\n",
		"b/b.gg": "package b\nimport \"gogo/c\"\nfunc G() int { return c.H() + 1 }\n",
		"c/c.gg": "package c\nfunc H() int { return 1 }\n",
	})
	prog, err := LoadWith(writeProg(t, "package main\nimport (\n\t\"gogo/a\"\n\t\"gogo/c\"\n)\nfunc main() { println(a.F(), c.H()) }\n"), libs)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	var got []string
	for _, l := range prog.Libs {
		got = append(got, l.Pkg.Path())
	}
	if strings.Join(got, " ") != "gogo/c gogo/b gogo/a" {
		t.Fatalf("Libs = %v, want dependency order c b a", got)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
	cyc := fakeLibs(t, map[string]string{
		"a/a.gg": "package a\nimport \"gogo/b\"\nfunc F() { b.G() }\n",
		"b/b.gg": "package b\nimport \"gogo/a\"\nfunc G() { a.F() }\n",
	})
	err = loadErr(t, "package main\nimport \"gogo/a\"\nfunc main() { a.F() }\n", cyc)
	if err == nil || !strings.Contains(err.Error(), "import cycle gogo/a -> gogo/b -> gogo/a") {
		t.Fatalf("want an import cycle error, got %v", err)
	}
}

func TestMainImportsSupervisor(t *testing.T) {
	src := "package main\nimport \"gogo/supervisor\"\nfunc main() {\n\tsupervisor.Supervise(func() { println(1) }, supervisor.Spec{})\n}\n"
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(prog.Libs) != 1 || prog.Libs[0].Pkg.Path() != "gogo/supervisor" {
		t.Fatalf("Libs = %v, want gogo/supervisor", prog.Libs)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
}
