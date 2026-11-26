package compiler

import (
	"go/types"
	"strings"
	"testing"
)

// TestDeferInlined: deferred calls are compiled at each return, last
// deferred first; a defer at the top level of the body needs no flag, one
// in a nested block is tested (JMPF) at the returns outside its block;
// a return before a defer does not run it.
func TestDeferInlined(t *testing.T) {
	dis := compileSrc(t, `package main

func a() {}
func b() {}
func c() {}

func f(n int) int {
	if n == 0 {
		return 0 // nothing deferred yet
	}
	defer a()
	if n > 1 {
		defer b()
		if n > 2 {
			return 2 // a and b, unconditionally
		}
	}
	defer c()
	return 1 // c, b if its flag is set, a
}

func g() {
	defer a()
	println("g") // the implicit return at the end runs a
}

func main() { println(f(3)); g() }
`)
	_, body := funcDis(t, dis, "f")
	code := parseIns(t, body)
	var calls []string
	for _, in := range code {
		switch in.op {
		case "CALL":
			calls = append(calls, in.args[strings.LastIndex(in.args, " ")+1:])
		case "RET":
			calls = append(calls, "|")
		}
	}
	if got, want := strings.Join(calls, " "), "| b a | c b a |"; got != want {
		t.Errorf("f: calls and returns %q, want %q\n%s", got, want, strings.Join(body, "\n"))
	}
	if n := countOp(code, "JMPF", ""); n != 4 { // 3 ifs, 1 flag test
		t.Errorf("f: %d JMPF, want 4\n%s", n, strings.Join(body, "\n"))
	}
	_, body = funcDis(t, dis, "g")
	if n := countOp(parseIns(t, body), "CALL", "a"); n != 1 {
		t.Errorf("g: %d calls of a, want 1\n%s", n, strings.Join(body, "\n"))
	}
}

// TestArrayCopies: an array value is copied (MAKE_SLICE + COPY) when it
// goes into a variable, an argument or a result, unless it is fresh (a
// literal or a call result); a = b copies into a's own array.
func TestArrayCopies(t *testing.T) {
	dis := compileSrc(t, `package main

func id(a [2]int) [2]int { return a }

func main() {
	a := [2]int{1, 2}
	b := a
	c := id([2]int{3, 4})
	a = c
	println(b[0], c[0], a[0])
}
`)
	_, body := funcDis(t, dis, "id")
	code := parseIns(t, body)
	if countOp(code, "BUILTIN", "COPY") != 1 {
		t.Errorf("id: want one COPY (the result)\n%s", strings.Join(body, "\n"))
	}
	_, body = funcDis(t, dis, "main")
	code = parseIns(t, body)
	// b := a copies; id's argument and result are fresh; a = c copies
	// into a.
	if n := countOp(code, "BUILTIN", "COPY"); n != 2 {
		t.Errorf("main: %d COPY, want 2\n%s", n, strings.Join(body, "\n"))
	}
}

// TestMethodTable: the method table of a type (the hook for interface
// dispatch) lists its own methods and the promoted ones, with the field
// path to the receiver; a shallower method wins.
func TestMethodTable(t *testing.T) {
	prog, err := Load(writeProg(t, `package main

type A struct{ N int }

func (A) Get() int  { return 1 }
func (A) Name() int { return 2 }

type B struct {
	X int
	A
}

func (B) Name() int { return 3 }

type C struct{ B }

func main() { println(C{}.Get(), C{}.Name()) }
`))
	if err != nil {
		t.Fatal(err)
	}
	g := &gen{p: prog}
	c := prog.Pkg.Scope().Lookup("C").Type().(*types.Named)
	var got []string
	for _, m := range g.methodTable(c) {
		s := funcName(m.fn) + "@"
		for _, st := range m.path {
			s += st.st.Obj().Name() + "." + st.st.Underlying().(*types.Struct).Field(st.idx).Name() + " "
		}
		got = append(got, strings.TrimSpace(s))
	}
	if want := "A.Get@C.B B.A|B.Name@C.B"; strings.Join(got, "|") != want {
		t.Errorf("methodTable(C) = %q, want %q", strings.Join(got, "|"), want)
	}
}
