package compiler

import (
	"go/types"
	"strings"
	"testing"
)

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
