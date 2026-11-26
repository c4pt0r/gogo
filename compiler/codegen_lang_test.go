package compiler

import (
	"go/types"
	"strings"
	"testing"
)

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
