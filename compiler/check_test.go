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

	"gogo"
)

func writeProg(t *testing.T, src string) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "main.go"), []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
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
		{"method", "package main\ntype P struct{ X int }\nfunc (p P) M() {} // ERR\nfunc main() {}\n", []string{"method"}},
		{"slice", "package main\nfunc main() {\n\ts := []int{1} // ERR\n\t_ = s\n}\n", []string{"slice"}},
		{"call-funcvalue", "package main\nfunc main() {\n\tf := func() {}\n\tf() // ERR\n}\n", []string{"calling a func value not supported"}},
		{"call-funcfield", "package main\ntype J struct{ R func() }\nfunc main() {\n\tj := J{}\n\tj.R() // ERR\n}\n", []string{"calling a func value not supported"}},
		{"call-funclit", "package main\nfunc main() {\n\tfunc() {}() // ERR\n}\n", []string{"calling a func value not supported"}},
		{"funclit-params", "package main\nfunc main() {\n\tf := func(x int) {} // ERR\n\t_ = f\n}\n", []string{"func type func(x int) not supported"}},
		{"funcparam-sig", "package main\nfunc run(f func() int) {} // ERR\nfunc main() {}\n", []string{"func type func() int not supported"}},
		{"funcvalue-any", "package main\nfunc f() {}\nfunc main() {\n\tvar x any = f // ERR\n\t_ = x\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-send", "package main\nimport \"gogo/actor\"\nfunc f() {}\nfunc main() {\n\tactor.Send(actor.Self(), f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-println", "package main\nfunc f() {}\nfunc main() {\n\tprintln(f) // ERR\n}\n", []string{"func value in interface not supported"}},
		{"funcvalue-compare", "package main\nfunc f() {}\nfunc main() {\n\tg := f\n\tprintln(g == f) // ERR\n}\n", []string{"compared to nil"}},
		{"funcvalue-actor", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tg := actor.Self // ERR\n\t_ = g\n}\n", []string{"func type func() actor.PID not supported"}},
		{"import", "package main\nimport \"fmt\" // ERR\nfunc main() { fmt.Println() }\n", []string{`import "fmt"`}},
		{"fieldassign", "package main\ntype P struct{ X int }\nfunc main() {\n\tp := P{}\n\tp.X = 1 // ERR\n}\n", []string{"field assignment"}},
		{"pointer", "package main\nfunc main() {\n\tx := 1\n\tp := &x // ERR\n\t_ = p\n}\n", []string{"pointer"}},
		{"array", "package main\nfunc main() {\n\tvar a [3]int // ERR\n\t_ = a\n}\n", []string{"array"}},
		{"panic", "package main\nfunc main() {\n\tpanic(1) // ERR\n}\n", []string{"panic"}},
		{"range", "package main\nfunc main() {\n\tfor i := range 3 { // ERR\n\t\t_ = i\n\t}\n}\n", []string{"range"}},
		{"stringop", "package main\nfunc main() {\n\ts := \"a\"\n\tt := s + \"b\" // ERR\n\tprintln(t)\n}\n", []string{"string"}},
		{"conversion", "package main\ntype C int\nfunc main() {\n\tx := C(3) // ERR\n\t_ = x\n}\n", []string{"conversion"}},
		{"conversion-string", "package main\nfunc main() {\n\tx := 65\n\ts := string(x) // ERR\n\t_ = s\n}\n", []string{"conversion"}},
		{"conversion-named-int64", "package main\ntype T int64\nfunc main() {\n\tx := 1\n\tt := T(x) // ERR\n\t_ = t\n}\n", []string{"conversion"}},
		{"int64-mix", "package main\nfunc main() {\n\tvar a int64 = 1\n\tb := 2\n\tprintln(a + b) // ERR\n}\n", []string{"mismatched types int64 and int"}},
		{"int64-mix-assign", "package main\nfunc main() {\n\tvar a int64\n\tb := 2\n\ta = b // ERR\n\t_ = a\n}\n", []string{"cannot use b"}},
		{"int32-conv", "package main\nfunc main() {\n\tx := 1\n\ty := int32(x) // ERR\n\t_ = y\n}\n", []string{"int32"}},
		{"funcvalue-sig", "package main\nfunc f(x int) {}\nfunc main() {\n\tg := f // ERR\n\t_ = g\n}\n", []string{"func type func(x int) not supported"}},
		{"switch", "package main\nfunc main() {\n\tswitch 1 { // ERR\n\tcase 1:\n\t}\n}\n", []string{"switch"}},
		{"error", "package main\nfunc f() error { return nil } // ERR\nfunc main() { f() }\n", []string{"error"}},
		{"shift", "package main\nfunc main() {\n\tx := 1\n\ty := x << 2 // ERR\n\t_ = y\n}\n", []string{"operator <<"}},
		{"namedresult", "package main\nfunc f() (r int) { // ERR\n\tr = 5\n\treturn\n}\nfunc main() { println(f()) }\n", []string{"named results not supported"}},
		{"init", "package main\nfunc init() {} // ERR\nfunc main() {}\n", []string{"func init not supported"}},
		{"intcast", "package main\nfunc main() {\n\tvar x int32 = 1 // ERR\n\t_ = x\n}\n", []string{"int32"}},
		{"namedbasic-any", "package main\ntype C int\nfunc main() {\n\tvar x any = C(1) // ERR\n\t_ = x\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-var", "package main\ntype C int\nfunc main() {\n\tvar c C = 1\n\tvar x any\n\tx = c // ERR\n\t_ = x\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-arg", "package main\ntype C bool\nfunc f(v any) {}\nfunc main() {\n\tvar c C\n\tf(c) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-send", "package main\nimport \"gogo/actor\"\ntype C string\nfunc main() {\n\tvar c C = \"x\"\n\tactor.Send(actor.Self(), c) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-field", "package main\ntype C int\ntype B struct{ V any }\nfunc main() {\n\tvar c C\n\tprintln(B{c}) // ERR\n}\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-return", "package main\ntype C int\nfunc f() any {\n\tvar c C\n\treturn c // ERR\n}\nfunc main() { println(f()) }\n", []string{"named basic type in interface not supported"}},
		{"namedbasic-case", "package main\ntype C int\nfunc main() {\n\tvar x any = 1\n\tswitch x.(type) {\n\tcase C: // ERR\n\t}\n}\n", []string{"type switch case"}},
		{"tworesult-user", "package main\nfunc main() {\n\ta, b := f() // ERR\n\tprintln(a, b)\n}\nfunc f() (int, int) { return 1, 2 }\n", []string{"multi-value assignment not supported"}},
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
		{"call-funcresult", "package main\nfunc f() {}\nfunc g() func() { return f }\nfunc main() {\n\tg()() // ERR\n}\n", []string{"calling a func value not supported"}},
		{"captured-assign-nested", "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 1\n\tactor.Spawn(func() {\n\t\tf := func() {\n\t\t\tx = 2 // ERR\n\t\t}\n\t\tactor.Spawn(f)\n\t})\n\tprintln(x)\n}\n", []string{"captured variable"}},
		{"captured-opassign-before", "package main\nfunc main() {\n\tx := 1\n\tx += 1 // ERR\n\tf := func() { println(x) }\n\t_ = f\n}\n", []string{"captured variable must not be reassigned"}},
		{"import-unknown-lib", "package main\nimport \"gogo/nosuch\" // ERR\nfunc main() { nosuch.X() }\n", []string{`import "gogo/nosuch" not supported`}},
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

func TestLoadRequiresMain(t *testing.T) {
	if _, err := Load(writeProg(t, "package main\nfunc other() {}\n")); err == nil {
		t.Fatal("expected error for missing func main")
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

func TestRejectsReview(t *testing.T) {
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
}
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
	src, err := gogo.StubFS.ReadFile("actor/actor.go")
	if err != nil {
		t.Fatal(err)
	}
	fsys := fstest.MapFS{"actor/actor.go": {Data: src}}
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
		{"lib-imports-fmt", "package lib\nimport \"fmt\"\nfunc F() { fmt.Println() }\n", `lib/lib.go:2:8: gogo: import "fmt" not supported`},
		{"lib-imports-lib", "package lib\nimport \"gogo/lib\"\nfunc F() { lib.F() }\n", `import "gogo/lib" not supported`},
		{"lib-renamed-actor", "package lib\nimport a \"gogo/actor\"\nfunc F() { a.Self() }\n", `import a "gogo/actor" not supported`},
		{"lib-wrong-package", "package other\nfunc F() {}\n", "package other"},
		{"lib-global", "package lib\nvar g int\nfunc F() { println(g) }\n", "lib/lib.go:2:1: gogo: global variable not supported"},
		{"lib-subset", "package lib\nfunc F() {\n\tm := map[int]int{}\n\t_ = m\n}\n", "lib/lib.go:3:7: gogo: map not supported"},
		{"lib-type-error", "package lib\nfunc F() { undefined() }\n", "undefined"},
		{"lib-captured-reassign", "package lib\nimport \"gogo/actor\"\nfunc F() {\n\tx := 1\n\tx = 2\n\tactor.Spawn(func() { println(x) })\n}\n", "lib/lib.go:5:2: gogo: captured variable must not be reassigned"},
		{"lib-ok", "package lib\nimport \"gogo/actor\"\nfunc F() { actor.Spawn(func() { println(1) }) }\n", ""},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := loadErr(t, useLib, fakeLibs(t, map[string]string{"lib/lib.go": tc.lib}))
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
