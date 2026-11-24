package compiler

import (
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
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
		{"funclit", "package main\nfunc main() {\n\tf := func() {} // ERR\n\tf()\n}\n", []string{"func literal", "only allowed as the argument of actor.Spawn"}},
		{"import", "package main\nimport \"fmt\" // ERR\nfunc main() { fmt.Println() }\n", []string{`import "fmt"`}},
		{"fieldassign", "package main\ntype P struct{ X int }\nfunc main() {\n\tp := P{}\n\tp.X = 1 // ERR\n}\n", []string{"field assignment"}},
		{"pointer", "package main\nfunc main() {\n\tx := 1\n\tp := &x // ERR\n\t_ = p\n}\n", []string{"pointer"}},
		{"array", "package main\nfunc main() {\n\tvar a [3]int // ERR\n\t_ = a\n}\n", []string{"array"}},
		{"panic", "package main\nfunc main() {\n\tpanic(1) // ERR\n}\n", []string{"panic"}},
		{"range", "package main\nfunc main() {\n\tfor i := range 3 { // ERR\n\t\t_ = i\n\t}\n}\n", []string{"range"}},
		{"stringop", "package main\nfunc main() {\n\ts := \"a\"\n\tt := s + \"b\" // ERR\n\tprintln(t)\n}\n", []string{"string"}},
		{"conversion", "package main\nfunc main() {\n\tx := int(3) // ERR\n\t_ = x\n}\n", []string{"conversion"}},
		{"funcvalue", "package main\nfunc f() {}\nfunc main() {\n\tg := f // ERR\n\t_ = g\n}\n", []string{"func value"}},
		{"switch", "package main\nfunc main() {\n\tswitch 1 { // ERR\n\tcase 1:\n\t}\n}\n", []string{"switch"}},
		{"error", "package main\nfunc f() error { return nil } // ERR\nfunc main() { f() }\n", []string{"error"}},
		{"shift", "package main\nfunc main() {\n\tx := 1\n\ty := x << 2 // ERR\n\t_ = y\n}\n", []string{"operator <<"}},
		{"namedresult", "package main\nfunc f() (r int) { // ERR\n\tr = 5\n\treturn\n}\nfunc main() { println(f()) }\n", []string{"named results not supported"}},
		{"init", "package main\nfunc init() {} // ERR\nfunc main() {}\n", []string{"func init not supported"}},
		{"intcast", "package main\nfunc main() {\n\tvar x int32 = 1 // ERR\n\t_ = x\n}\n", []string{"int32"}},
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
		{"iface-default-copy", hdr + "func main() {\n\tswitch m := actor.Receive().(type) {\n\tcase P:\n\t\tprintln(m.X)\n\tdefault:\n\t\ty := m\n\t\tprintln(y)\n\t}\n}\n", "interface value"},
		{"iface-default-println", hdr + "func main() {\n\tswitch m := actor.Receive().(type) {\n\tcase P:\n\t\tprintln(m.X)\n\tdefault:\n\t\tprintln(m)\n\t}\n}\n", "interface value"},
		{"iface-multi-case", hdr + "func main() {\n\tswitch m := actor.Receive().(type) {\n\tcase P, Q:\n\t\tprintln(m)\n\t}\n}\n", "interface value"},
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
