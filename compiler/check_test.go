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
		{"map-pointer-key", "package main\nfunc main() {\n\tm := map[*int]int{} // ERR\n\t_ = m\n}\n", []string{"map with key type *int: a pointer key"}},
		{"map-global-write", "package main\nvar m = map[string]int{\"a\": 1}\nfunc main() {\n\tm[\"b\"] = 2 // ERR\n}\n", []string{"package-level variable m is read-only"}},
		{"map-global-key", "package main\nvar k = \"a\"\nvar m = map[string]int{k: 1} // ERR\nfunc main() { println(m[k]) }\n", []string{"a map key that is not a constant"}},
		{"defer-loop", "package main\nfunc f() {}\nfunc main() {\n\tfor i := 0; i < 2; i++ {\n\t\tdefer f() // ERR\n\t}\n}\n", []string{"defer inside a loop not supported"}},
		{"defer-range-switch", "package main\nfunc main() {\n\tfor range 2 {\n\t\tswitch {\n\t\tcase true:\n\t\t\tdefer println() // ERR\n\t\t}\n\t}\n}\n", []string{"defer inside a loop not supported"}},
		{"defer-loop-in-literal", "package main\nfunc main() {\n\tf := func() {\n\t\tfor {\n\t\t\tdefer println() // ERR\n\t\t}\n\t}\n\tf()\n}\n", []string{"defer inside a loop not supported"}},
		{"defer-multivalue-arg", "package main\nfunc two() (int, int) { return 1, 2 }\nfunc f(a, b int) {}\nfunc main() {\n\tdefer f(two()) // ERR\n}\n", []string{"deferred call with a multi-value argument not supported"}},
		{"defer-tworesult-builtin", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tdefer actor.ReceiveTimeout(5) // ERR\n}\n", []string{"multiple values not supported"}},
		{"defer-recover", "package main\nfunc main() {\n\tdefer recover() // ERR\n}\n", []string{"recover (a panic ends the actor"}},
		{"complex", "package main\nfunc main() {\n\tx := 1.5i // ERR\n\t_ = x\n}\n", []string{"complex"}},
		{"float-rem", "package main\nfunc main() {\n\tx := 1.5\n\tprintln(x % 2) // ERR\n}\n", []string{"%"}},
		{"float-func-value", "package main\nimport \"gogo/strconv\"\nfunc main() {\n\tf := strconv.FormatFloat // ERR\n\t_ = f\n}\n", []string{"strconv.FormatFloat as a func value not supported"}},
		{"method-pointer-elem", "package main\ntype P struct{ X int }\nfunc (p *P) M() {}\nfunc main() {\n\ts := []P{{}}\n\ts[0].M() // ERR\n}\n", []string{"pointer receiver", "no interior pointers"}},
		{"method-pointer-promoted", "package main\ntype A struct{ X int }\nfunc (a *A) M() {}\ntype B struct{ A }\nfunc main() {\n\tb := &B{}\n\tb.M() // ERR\n}\n", []string{"promoted from an embedded field"}},
		{"method-expr-value-on-ptr", "package main\ntype A struct{ X int }\nfunc (a A) M() {}\nfunc main() {\n\tf := (*A).M // ERR\n\t_ = f\n}\n", []string{"(*T).M of a value method"}},
		{"method-on-slice-type", "package main\ntype L []int // ERR\nfunc (l L) Len() int { return len(l) }\nfunc main() {}\n", []string{"type definition"}},
		{"method-value-any", "package main\ntype P struct{ X int }\nfunc (p P) M() {}\nfunc main() {\n\tvar x any = P{}.M // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"slice-typeswitch", "package main\nfunc main() {\n\tvar x any = []int{1}\n\tswitch x.(type) {\n\tcase []int: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"slice-keyed", "package main\nfunc main() {\n\ts := []int{2: 1} // ERR\n\t_ = s\n}\n", []string{"keyed slice element"}},
		{"slice-of-chan", "package main\nfunc main() {\n\tvar s []chan int // ERR\n\t_ = s\n}\n", []string{"slice of channel"}},
		{"slice-3index", "package main\nfunc main() {\n\ts := []int{1, 2}\n\tprintln(len(s[0:1:2])) // ERR\n}\n", []string{"3-index"}},
		{"slice-named", "package main\ntype B []byte // ERR\nfunc main() {}\n", []string{"type definition"}},
		{"variadic-multi", "package main\nfunc f(xs ...int) {}\nfunc g() (int, int) { return 1, 2 }\nfunc main() {\n\tf(g()) // ERR\n}\n", []string{"multiple values not supported"}},
		{"variadic-builtin-spread", "package main\nfunc main() {\n\ts := []any{1}\n\tprintln(s...) // ERR\n}\n", []string{"println"}},
		{"string-index-assign", "package main\nfunc main() {\n\ts := \"ab\"\n\ts[0] = 1 // ERR\n}\n", []string{"cannot assign"}},
		{"funcvalue-any", "package main\nfunc f() {}\nfunc main() {\n\tvar x any = f // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-send", "package main\nimport \"gogo/actor\"\nfunc f() {}\nfunc main() {\n\tactor.Send(actor.Self(), f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-println", "package main\nfunc f() {}\nfunc main() {\n\tprintln(f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-compare", "package main\nfunc f() {}\nfunc main() {\n\tg := f\n\tprintln(g == f) // ERR\n}\n", []string{"compared to nil"}},
		{"funcvalue-actor", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tg := actor.Self // ERR\n\t_ = g\n}\n", []string{"selector other than a field read"}},
		{"funcvalue-variadic-badelem", "package main\nfunc run(f func(xs ...complex128)) {} // ERR\nfunc main() {}\n", []string{"func type func(xs ...complex128) not supported"}},
		{"funcvalue-badparam", "package main\nfunc run(f func(m chan int) int) {} // ERR\nfunc main() {}\n", []string{"func type func(m chan int) int not supported"}},
		{"funcvalue-badresult", "package main\nfunc main() {\n\tf := func() complex128 { return 1 } // ERR\n\t_ = f\n}\n", []string{"func type func() complex128 not supported"}},
		{"funcvalue-args-any", "package main\nfunc main() {\n\tvar x any = func(n int) int { return n } // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-result-any", "package main\nfunc id(n int) int { return n }\nfunc pair() (func(int) int, int) { return id, 1 }\nfunc main() {\n\tvar a any\n\tvar b int\n\ta, b = pair() // ERR\n\t_, _ = a, b\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-nil", "package main\nfunc main() {\n\tvar f func(int)\n\tprintln(f == nil) // ERR\n}\n", []string{"comparison of func(int)"}},
		{"import", "package main\nimport \"fmt\" // ERR\nfunc main() { fmt.Println() }\n", []string{`import "fmt"`}},
		{"fieldassign-map", "package main\ntype P struct{ X int }\nfunc main() {\n\tm := map[string]P{}\n\tm[\"a\"].X = 1 // ERR\n}\n", []string{"cannot assign"}},
		{"fieldassign-call", "package main\ntype P struct{ X int }\nfunc f() P { return P{} }\nfunc main() {\n\tf().X = 1 // ERR\n}\n", []string{"cannot assign"}},
		{"embed-pointer", "package main\ntype A struct{ X int }\ntype B struct {\n\t*A // ERR\n}\nfunc main() {}\n", []string{"embedded pointer field not supported"}},
		{"embed-ambiguous-field", "package main\ntype A struct{ X int }\ntype B struct{ X int }\ntype C struct {\n\tA\n\tB\n}\nfunc main() {\n\tvar c C\n\tprintln(c.X) // ERR\n}\n", []string{"ambiguous selector c.X"}},
		{"embed-ambiguous-method", "package main\ntype A struct{}\nfunc (A) M() {}\ntype B struct{}\nfunc (B) M() {}\ntype C struct {\n\tA\n\tB\n}\nfunc main() {\n\tvar c C\n\tc.M() // ERR\n}\n", []string{"ambiguous selector c.M"}},
		{"embed-stub-method", "package main\nimport \"gogo/bytes\"\ntype W struct{ bytes.Buffer }\nfunc main() {\n\tw := W{bytes.NewBuffer(4)}\n\tw.WriteByte(1) // ERR\n}\n", []string{"method WriteByte of an embedded gogo/bytes type not supported: call it on the field"}},
		{"embed-stub-method-value", "package main\nimport \"gogo/bytes\"\ntype W struct{ bytes.Buffer }\nfunc main() {\n\tw := W{bytes.NewBuffer(4)}\n\tf := w.Len // ERR\n\t_ = f\n}\n", []string{"method Len of an embedded gogo/bytes type not supported"}},
		{"embed-method-expr", "package main\ntype A struct{}\nfunc (A) M() {}\ntype B struct{ A }\nfunc main() {\n\tf := B.M // ERR\n\t_ = f\n}\n", []string{"method expression of a promoted method not supported"}},
		{"pointer-interior-elem", "package main\nfunc main() {\n\ts := []int{1}\n\tp := &s[0] // ERR\n\t_ = p\n}\n", []string{"interior pointer &s[0] not supported"}},
		{"pointer-interior-field", "package main\ntype T struct{ X int }\nfunc main() {\n\tv := T{}\n\tp := &v.X // ERR\n\t_ = p\n}\n", []string{"interior pointer &v.X not supported"}},
		{"println-unsigned-ptr", "package main\ntype S struct{ U uint32 }\nfunc main() {\n\tprintln(&S{1}) // ERR\n}\n", []string{"println of *main.S not supported: it holds uint32"}},
		{"println-unsigned-box", "package main\nfunc main() {\n\tvar u uint32 = 1\n\tprintln(&u) // ERR\n}\n", []string{"println of *uint32 not supported"}},
		{"println-unsigned-map-elem", "package main\nfunc main() {\n\tprintln(map[int]uint32{1: 1}) // ERR\n}\n", []string{"println of map[int]uint32 not supported"}},
		{"println-unsigned-map-key", "package main\nfunc main() {\n\tprintln(map[uint64]int{1: 2}) // ERR\n}\n", []string{"println of map[uint64]int not supported"}},
		{"pointer-to-array", "package main\nfunc main() {\n\tvar a [2]int\n\tp := &a // ERR\n\t_ = p\n}\n", []string{"pointer to an array"}},
		{"array-any", "package main\nfunc main() {\n\tvar a [3]int\n\tvar x any = a // ERR\n\t_ = x\n}\n", []string{"array in interface not supported"}},
		{"array-send", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tactor.Send(actor.Self(), [2]int{}) // ERR\n}\n", []string{"array in interface not supported"}},
		{"array-slice-field", "package main\ntype S struct{ A [2]int }\nfunc main() {\n\tvar s S\n\tt := s.A[:] // ERR\n\t_ = t\n}\n", []string{"slice of an array that is not a variable not supported"}},
		{"array-slice-elem", "package main\nfunc main() {\n\tvar m [2][2]int\n\tt := m[1][:] // ERR\n\t_ = t\n}\n", []string{"slice of an array that is not a variable not supported"}},
		{"array-of-chan", "package main\nfunc main() {\n\tvar a [2]chan int // ERR\n\t_ = a\n}\n", []string{"array of channel"}},
		{"array-named-type", "package main\ntype G [4]int // ERR\nfunc main() {}\n", []string{"type definition"}},
		{"recover", "package main\nfunc main() {\n\tprintln(recover()) // ERR\n}\n", []string{"recover (a panic ends the actor: let its supervisor restart it) not supported"}},
		{"panic-func", "package main\nfunc main() {\n\tpanic(main) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"range-func", "package main\nfunc seq(yield func(int) bool) {}\nfunc main() {\n\tfor i := range seq { // ERR\n\t\t_ = i\n\t}\n}\n", []string{"range over func"}},
		{"range-int-assign", "package main\nfunc main() {\n\tvar i int\n\tfor i = range 3 { // ERR\n\t}\n\tprintln(i)\n}\n", []string{"range assigning to existing variables"}},
		{"string-sub", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s[0] - s[0], len(s))\n\tt := s[:1] + s[0:] // ok\n\tprintln(t)\n\tvar i int64\n\tprintln(s[i]) // ERR\n}\n", []string{"index of type int64"}},
		{"string-slice3", "package main\nfunc main() {\n\ts := \"abc\"\n\tprintln(s[0:1:2]) // ERR\n}\n", []string{"3-index"}},
		{"range-assign", "package main\nfunc main() {\n\tvar i int\n\tfor i = range \"ab\" { // ERR\n\t}\n\tprintln(i)\n}\n", []string{"range"}},
		{"conversion-string", "package main\nfunc main() {\n\tx := 65\n\ts := string(x) // ERR\n\t_ = s\n}\n", []string{"conversion"}},
		{"int64-mix", "package main\nfunc main() {\n\tvar a int64 = 1\n\tb := 2\n\tprintln(a + b) // ERR\n}\n", []string{"mismatched types int64 and int"}},
		{"int64-mix-assign", "package main\nfunc main() {\n\tvar a int64\n\tb := 2\n\ta = b // ERR\n\t_ = a\n}\n", []string{"cannot use b"}},
		{"switch-slice", "package main\nfunc main() {\n\ts := []int{1}\n\tswitch s { // ERR\n\tcase nil:\n\t}\n}\n", []string{"switch on a value of type []int not supported"}},
		{"goto", "package main\nfunc main() {\n\tgoto end // ERR\nend:\n\tfor {\n\t}\n}\n", []string{"goto statement not supported"}},
		{"label-block", "package main\nfunc main() {\nL: // ERR\n\t{\n\t\tprintln(1)\n\t}\n\tgoto L\n}\n", []string{"label on a statement other than for or switch not supported"}},
		{"bit-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s & s) // ERR\n}\n", []string{"operator &"}},
		{"bit-bool", "package main\nfunc main() {\n\tb := true\n\tprintln(b | b) // ERR\n}\n", []string{"operator |"}},
		{"shift-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(s << 1) // ERR\n}\n", []string{"shift"}},
		{"xor-string", "package main\nfunc main() {\n\ts := \"a\"\n\tprintln(^s) // ERR\n}\n", []string{"operator ^"}},
		{"andnot-assign-string", "package main\nfunc main() {\n\ts := \"a\"\n\ts &^= s // ERR\n}\n", []string{"operator &^"}},
		{"init", "package main\nfunc init() {} // ERR\nfunc main() {}\n", []string{"func init not supported"}},
		{"uintptr", "package main\nfunc main() {\n\tvar x uintptr = 1 // ERR\n\t_ = x\n}\n", []string{"uintptr"}},
		{"uint64-index", "package main\nfunc main() {\n\ts := []int{1}\n\tvar i uint64\n\tprintln(s[i]) // ERR\n}\n", []string{"index of type uint64"}},
		{"println-struct-uint32", "package main\ntype P struct{ A int; B []uint32 }\nfunc main() {\n\tprintln(P{}) // ERR\n}\n", []string{"println of main.P not supported", "uint32 or uint64"}},
		{"println-slice-uint64", "package main\nfunc main() {\n\tprintln([]uint64{1}) // ERR\n}\n", []string{"println of []uint64 not supported"}},
		{"multiret-println", "package main\nfunc f() (int, int) { return 1, 2 }\nfunc main() {\n\tprintln(f()) // ERR\n}\n", []string{"multiple values not supported"}},
		{"multiret-var", "package main\nfunc f() (int, int) { return 1, 2 }\nfunc main() {\n\tvar a, b = f() // ERR\n\tprintln(a, b)\n}\n", []string{"multiple values not supported"}},
		{"multiret-builtin-return", "package main\nimport \"gogo/actor\"\nfunc f() (any, bool) {\n\treturn actor.ReceiveTimeout(5) // ERR\n}\nfunc main() { a, b := f(); println(a, b) }\n", []string{"multiple values not supported"}},
		{"tworesult-single", "package main\nimport \"gogo/actor\"\nfunc main() {\n\ta, b := actor.Self() // ERR\n\t_, _ = a, b\n}\n", []string{"assignment mismatch"}},
		{"tworesult-var", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar m, ok = actor.ReceiveTimeout(5) // ERR\n\tprintln(m, ok)\n}\n", []string{"multiple values not supported"}},
		{"pid-literal-fields", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tp := actor.PID{1} // ERR\n\tprintln(p)\n}\n", []string{"implicit assignment to unexported field"}},
		{"ref-literal", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tr := actor.Ref{} // ERR\n\tprintln(r)\n}\n", []string{"composite literal of actor.Ref not supported"}},
		{"funcvalue-return-any", "package main\nfunc f() {}\nfunc g() any {\n\treturn f // ERR\n}\nfunc main() { _ = g() }\n", []string{"func value in interface not supported"}},
		{"funcvalue-anyfield", "package main\ntype B struct{ V any }\nfunc f() {}\nfunc main() {\n\tb := B{V: f} // ERR\n\t_ = b\n}\n", []string{"func value in interface not supported"}},
		{"import-unknown-lib", "package main\nimport \"gogo/nosuch\" // ERR\nfunc main() { nosuch.X() }\n", []string{`import "gogo/nosuch" not supported`}},
		{"fs-file-case", "package main\nimport (\n\t\"gogo/actor\"\n\t\"gogo/fs\"\n)\nfunc main() {\n\tvar x any = actor.Self()\n\tswitch x.(type) {\n\tcase fs.File: // ERR\n\t\tprintln(\"file\")\n\tcase actor.PID:\n\t}\n}\n", []string{"type switch case fs.File not supported; wrap it in a struct"}},
		{"port-open-var", "package main\nimport \"gogo/port\"\nfunc main() {\n\tvar p, ok = port.Open(\"gpio\", port.Host{}) // ERR\n\tprintln(p, ok)\n}\n", []string{"multiple values not supported"}},
		{"fs-readfile-single", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tb := fs.ReadFile(\"/flash/a\", 9) // ERR\n\t_ = b\n}\n", []string{"assignment mismatch"}},
		{"fs-size-var", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar f fs.File\n\tvar n, e = f.Size() // ERR\n\tprintln(n, e)\n}\n", []string{"multiple values not supported"}},
		{"fs-method-value", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar f fs.File\n\tg := f.Close // ERR\n\t_ = g\n}\n", []string{"selector other than a field read not supported"}},
		{"port-open-arg", "package main\nimport \"gogo/port\"\nfunc f(p any, ok bool) {}\nfunc main() {\n\tf(port.Open(\"gpio\", port.Host{})) // ERR\n}\n", []string{"cannot use port.Open"}},
		{"bytes-buffer-case", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tvar x any = bytes.NewBuffer(1)\n\tswitch x.(type) {\n\tcase bytes.Buffer: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"bytes-literal", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.Bytes{} // ERR\n\t_ = x\n}\n", []string{"composite literal of bytes.Bytes"}},
		{"buffer-literal", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tb := bytes.Buffer{} // ERR\n\t_ = b\n}\n", []string{"zero value of bytes.Buffer", "use bytes.NewBuffer"}},
		{"buffer-var-zero", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tvar b bytes.Buffer // ERR\n\tb.WriteByte(1)\n}\n", []string{"zero value of bytes.Buffer", "use bytes.NewBuffer"}},
		{"buffer-field-omitted", "package main\nimport \"gogo/bytes\"\ntype T struct {\n\tN int\n\tB bytes.Buffer\n}\nfunc main() {\n\tt := T{N: 1} // ERR\n\tprintln(t.N)\n}\n", []string{"use bytes.NewBuffer"}},
		{"buffer-field-empty-lit", "package main\nimport \"gogo/bytes\"\ntype T struct{ B bytes.Buffer }\nfunc main() {\n\tt := []T{{}} // ERR\n\t_ = t\n}\n", []string{"use bytes.NewBuffer"}},
		{"bytes-println", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tprintln(x) // ERR\n}\n", []string{"println of bytes.Bytes"}},
		{"bytes-compare", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tprintln(x == x) // ERR\n}\n", []string{"comparison of"}},
		{"bytes-methodvalue", "package main\nimport \"gogo/bytes\"\nfunc main() {\n\tx := bytes.FromString(\"a\")\n\tf := x.Len // ERR\n\t_ = f\n}\n", []string{"not supported"}},
		{"assert-slice", "package main\nfunc main() {\n\tvar x any = 1\n\t_ = x.([]int) // ERR\n}\n", []string{"type assertion to []int not supported"}},
		{"assert-file", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar x any = 1\n\t_ = x.(fs.File) // ERR\n}\n", []string{"type assertion to fs.File; wrap it in a struct not supported"}},
		{"assert-var", "package main\nfunc main() {\n\tvar x any = 1\n\tvar v, ok = x.(int) // ERR\n\tprintln(v, ok)\n}\n", []string{"multiple values not supported"}},
		{"assert-concrete", "package main\nfunc main() {\n\tx := 1\n\t_ = x.(int) // ERR\n}\n", []string{"not an interface"}},
		{"buffer-field-compare", "package main\nimport \"gogo/bytes\"\ntype T struct{ B bytes.Buffer }\nfunc main() {\n\ta := T{bytes.NewBuffer(1)}\n\tprintln(a == a) // ERR\n}\n", []string{"comparison of main.T not supported"}},
		{"iface-method-expr", "package main\ntype S interface{ M() int }\nfunc main() {\n\tf := S.M // ERR\n\t_ = f\n}\n", []string{"method expression on an interface type not supported"}},
		{"iface-bad-method", "package main\ntype S interface{ M() chan int } // ERR\nfunc main() {}\n", []string{"interface method M"}},
		{"iface-vm-bytes", "package main\nimport \"gogo/bytes\"\ntype L interface{ Len() int }\nfunc main() {\n\tvar l L = bytes.FromString(\"a\") // ERR\n\t_ = l\n}\n", []string{"bytes.Bytes as a main.L not supported", "builtin of the VM"}},
		{"iface-vm-arg", "package main\nimport \"gogo/bytes\"\ntype S interface{ String() string }\nfunc f(s S) {}\nfunc main() {\n\tf(bytes.FromString(\"a\")) // ERR\n}\n", []string{"bytes.Bytes as a main.S not supported"}},
		{"iface-vm-embedded", "package main\nimport \"gogo/bytes\"\ntype W interface{ WriteByte(c byte) }\ntype B struct{ bytes.Buffer }\nfunc main() {\n\tvar w W = B{} // ERR\n\t_ = w\n}\n", []string{"main.B as a main.W not supported"}},
		{"iface-vm-conv", "package main\nimport \"gogo/bytes\"\ntype L interface{ Len() int }\nfunc main() {\n\tl := L(bytes.FromString(\"a\")) // ERR\n\t_ = l\n}\n", []string{"bytes.Bytes as a main.L not supported"}},
		{"code-as-error", "package main\nimport \"gogo/fs\"\nfunc save() error {\n\treturn fs.Remove(\"/a\") // ERR\n}\nfunc main() { println(save() == nil) }\n", []string{"errors.Code result used as an error"}},
		{"code-as-error-var", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar err error = fs.Remove(\"/a\") // ERR\n\tprintln(err == nil)\n}\n", []string{"errors.Code result used as an error"}},
		{"code-as-error-assign", "package main\nimport \"gogo/fs\"\nfunc main() {\n\tvar err error\n\terr = fs.Remove(\"/a\") // ERR\n\tprintln(err == nil)\n}\n", []string{"errors.Code result used as an error"}},
		{"iface-typeset", "package main\ntype N interface{ ~int } // ERR\nfunc main() {}\n", []string{"interface with a type set not supported"}},
		{"alias-generic", "package main\ntype L[T any] = []T // ERR\nfunc main() {}\n", []string{"generic type not supported"}},
		{"alias-chan", "package main\ntype M = chan int // ERR\nfunc main() {}\n", []string{"channel not supported"}},
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

var zero = actor.PID{}

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
	actor.ReceiveTimeout(0) // a two-result builtin as a statement: results discarded
	actor.Whereis("x")
	(actor.ReceiveTimeout(0))
	z := actor.PID{} // the zero PID
	println(z == actor.PID{}, zero == z)
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
		{"global-write", "package main\nvar g int\nfunc main() { g = 1 }\n", "package-level variable g is read-only"},
		{"global-pointer", "package main\ntype T struct{ X int }\nvar g = &T{}\nfunc main() { println(g.X) }\n", "a pointer (its target would be read-only)"},
		{"global-call", "package main\nvar g = make([]int, 3)\nfunc main() { println(len(g)) }\n", "a call of make"},
		{"global-elem", "package main\nvar g = []int{1}\nfunc main() { g[0]++ }\n", "package-level variable g is read-only"},
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
	errs := checkSrc(t, "package main\nfunc f() (complex128, int, complex64) { return 0, 0, 0 }\nfunc g() (complex128, int) { return 0, 0 }\nfunc main() {}\n")
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

// fakeLibs is a library set with the real gogo/actor, gogo/port, gogo/bytes
// and gogo/errors plus extra
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
	esrc, err := lib.StubFS.ReadFile("errors/errors.gg")
	if err != nil {
		t.Fatal(err)
	}
	fsys := fstest.MapFS{"actor/actor.gg": {Data: src}, "port/port.gg": {Data: psrc}, "bytes/bytes.gg": {Data: bsrc}, "errors/errors.gg": {Data: esrc}}
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
		{"lib-global", "package lib\nvar g int\nfunc F() { g++ }\n", "lib/lib.gg:3:12: gogo: package-level variable g is read-only"},
		{"lib-subset", "package lib\nfunc F() {\n\tm := 1.5i\n\t_ = m\n}\n", "lib/lib.gg:3:7: gogo: complex not supported"},
		{"lib-type-error", "package lib\nfunc F() { undefined() }\n", "undefined"},
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
	var libs []string
	for _, l := range prog.Libs {
		if !prog.system[l.Pkg] {
			libs = append(libs, l.Pkg.Path())
		}
	}
	if len(libs) == 0 || libs[len(libs)-1] != "gogo/supervisor" {
		t.Fatalf("Libs = %v (system packages aside), want gogo/supervisor last", libs)
	}
	if errs := Check(prog); len(errs) != 0 {
		t.Fatalf("Check: %v", errs)
	}
}

// TestAcceptsBoxing: named basic values and bytes go into interfaces
// (boxed) and are type switch cases, nil is the nil interface value, and
// bool and string convert to and from their named types (all formerly
// rejected).
func TestAcceptsBoxing(t *testing.T) {
	for _, tc := range []struct{ name, src string }{
		{"variadic-any-byte", "package main\nfunc f(xs ...any) {}\nfunc main() {\n\tvar b byte = 1\n\tf(1, b)\n}\n"},
		{"slice-byte-any", "package main\nfunc main() {\n\tb := []byte(\"a\")\n\tvar x []any\n\tx = append(x, b[0])\n\t_ = x\n}\n"},
		{"namedbasic-any", "package main\ntype C int\nfunc main() {\n\tvar x any = C(1)\n\t_ = x\n}\n"},
		{"namedbasic-var", "package main\ntype C int\nfunc main() {\n\tvar c C = 1\n\tvar x any\n\tx = c\n\t_ = x\n}\n"},
		{"namedbasic-arg", "package main\ntype C bool\nfunc f(v any) {}\nfunc main() {\n\tvar c C\n\tf(c)\n}\n"},
		{"namedbasic-send", "package main\nimport \"gogo/actor\"\ntype C string\nfunc main() {\n\tvar c C = \"x\"\n\tactor.Send(actor.Self(), c)\n}\n"},
		{"namedbasic-field", "package main\ntype C int\ntype B struct{ V any }\nfunc main() {\n\tvar c C\n\tprintln(B{c})\n}\n"},
		{"namedbasic-return", "package main\ntype C int\nfunc f() any {\n\tvar c C\n\treturn c\n}\nfunc main() { println(f()) }\n"},
		{"namedbasic-case", "package main\ntype C int\nfunc main() {\n\tvar x any = 1\n\tswitch x.(type) {\n\tcase C:\n\t}\n}\n"},
		{"multiret-named-any", "package main\ntype C int\nfunc g() (C, int) { return 1, 2 }\nfunc f() (any, int) {\n\treturn g()\n}\nfunc main() { a, b := f(); println(a, b) }\n"},
		{"multiret-arg-any", "package main\ntype C int\nfunc g() (C, int) { return 1, 2 }\nfunc h(a any, b int) {}\nfunc main() {\n\th(g())\n}\n"},
		{"byte-any", "package main\nfunc main() {\n\tvar c byte = 3\n\tvar x any = c\n\t_ = x\n}\n"},
		{"byte-send", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 3\n\tactor.Send(actor.Self(), byte(x))\n}\n"},
		{"byte-case", "package main\nfunc main() {\n\tvar x any = 1\n\tswitch x.(type) {\n\tcase byte:\n\t}\n}\n"},
		{"nil-any", "package main\nfunc main() {\n\tvar x any = nil\n\t_ = x\n}\n"},
		{"conversion", "package main\ntype C bool\nfunc main() {\n\tx := true\n\ty := C(x)\n\t_ = y\n}\n"},
		{"conversion-named-string", "package main\ntype T string\nfunc main() {\n\tx := \"a\"\n\tt := T(x)\n\t_ = t\n}\n"},
		{"float-named-any", "package main\ntype C float64\nfunc main() {\n\tvar a any = C(1)\n\t_ = a\n}\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := compileProg(t, tc.src); err != nil {
				t.Fatal(err)
			}
		})
	}
}

// compileProg loads, checks and compiles src.
func compileProg(t *testing.T, src string) ([]byte, error) {
	t.Helper()
	prog, err := Load(writeProg(t, src))
	if err != nil {
		return nil, err
	}
	return Compile(prog)
}

// TestAcceptsEquality: == on interfaces and structs, and switches on
// them, by B_CMP (formerly rejected); a struct with a Buffer field does
// not compare.
func TestAcceptsEquality(t *testing.T) {
	for _, tc := range []struct{ name, src string }{
		{"switch-any", "package main\nfunc main() {\n\tvar x any = 1\n\tswitch x {\n\tcase 1:\n\t}\n}\n"},
		{"switch-struct", "package main\ntype P struct{ X int }\nfunc main() {\n\tp := P{1}\n\tswitch p {\n\tcase P{1}:\n\t}\n}\n"},
		{"switch-case-any", "package main\nfunc main() {\n\tx := 1\n\tvar a any = 1\n\tswitch x {\n\tcase a:\n\t}\n}\n"},
		{"any-compare-right-named", "package main\ntype C int\nfunc main() {\n\tvar c C = 3\n\tvar x any = 3\n\tprintln(c == x)\n}\n"},
		{"any-compare-right-const", "package main\nfunc main() {\n\tvar x any = 5\n\tprintln(5 == x)\n}\n"},
		{"any-compare-right-pid", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar x any = 5\n\tprintln(actor.Self() != x)\n}\n"},
		{"any-compare", "package main\nfunc main() {\n\tvar a, b any\n\tprintln(a == b)\n}\n"},
		{"struct-eq", "package main\ntype P struct{ X, Y int }\nfunc main() {\n\tprintln(P{1, 2} == P{1, 2}, P{} != P{1, 0})\n}\n"},
		{"any-nil", "package main\nfunc main() {\n\tvar x any\n\tprintln(x == nil, nil != x)\n}\n"},
		{"typeswitch-nil", "package main\nfunc main() {\n\tvar x any\n\tswitch x.(type) {\n\tcase nil:\n\t\tprintln(0)\n\t}\n}\n"},
		{"array-compare", "package main\nfunc main() {\n\ta, b := [2]int{}, [2]int{}\n\tprintln(a == b)\n}\n"},
		{"array-switch", "package main\nfunc main() {\n\ta := [2]int{}\n\tswitch a {\n\t}\n}\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := compileProg(t, tc.src); err != nil {
				t.Fatal(err)
			}
		})
	}
}

// TestAcceptsInterfaces: interface types, error and interface literals
// (formerly rejected).
func TestAcceptsInterfaces(t *testing.T) {
	for _, tc := range []struct{ name, src string }{
		{"method-interface", "package main\ntype S interface{ Area() int }\nfunc main() {}\n"},
		{"method-interface-param", "package main\nfunc f(s interface{ M() }) {}\nfunc main() {}\n"},
		{"error", "package main\nfunc f() error { return nil }\nfunc main() { f() }\n"},
		{"iface-literal", "package main\nfunc main() {\n\tvar x interface{ M() }\n\t_ = x\n}\n"},
		{"embedded", "package main\ntype N interface{ Name() string }\ntype S interface {\n\tN\n\tArea() int\n}\ntype R struct{ W int }\nfunc (r R) Name() string { return \"r\" }\nfunc (r R) Area() int { return r.W }\nfunc main() {\n\tvar s S = R{2}\n\tvar n N = s\n\tprintln(s.Area(), n.Name())\n}\n"},
		{"named-error", "package main\ntype E error\nfunc main() {\n\tvar e E\n\tprintln(e == nil)\n}\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := compileProg(t, tc.src); err != nil {
				t.Fatal(err)
			}
		})
	}
}
