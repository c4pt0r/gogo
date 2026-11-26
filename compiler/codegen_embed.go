package compiler

import (
	"go/ast"
	"go/types"
	"sort"
)

// Embedded fields. An embedded field is an ordinary field of the struct,
// named after its type (`struct{ A; Y int }` has fields A and Y). go/types
// resolves a promoted selector x.F or x.M to a path of field indices
// (types.Selection.Index), applying Go's depth and ambiguity rules (an
// ambiguous selector is a type error), so promotion is a chain of GETFIELDs
// down the embedded fields: no opcode of its own.

// embedPath lists the field steps from a value of type t down the field
// indices index (a Selection's path without its last element for a method,
// with it for a field).
func embedPath(t types.Type, index []int) []fieldStep {
	steps := make([]fieldStep, 0, len(index))
	for _, i := range index {
		st := structType(t)
		if st == nil {
			return nil
		}
		steps = append(steps, fieldStep{st: st, idx: i})
		t = st.Underlying().(*types.Struct).Field(i).Type()
	}
	return steps
}

// fieldPathInto loads into dst the value at the field path index from the
// struct in register src: one GETFIELD per step (the first reads src, the
// others dst).
func (f *fn) fieldPathInto(n ast.Node, index []int, src, dst uint8) {
	if len(index) == 0 {
		if src != dst {
			f.asm.Emit(OpMove, dst, src, 0)
		}
		return
	}
	for _, i := range index {
		if i > 255 {
			f.fail(n, "internal error: field index %d", i)
		}
		f.asm.Emit(OpGetfield, dst, src, uint8(i))
		src = dst
	}
}

// promotedMethod is a method of a struct type reached through its embedded
// fields: the function to call and the field path from the outer value to
// the receiver it expects.
type promotedMethod struct {
	name string
	fn   *types.Func
	path []fieldStep // empty for the type's own methods
}

// methodTable lists the methods in the method set of named type t, its own
// and the promoted ones, sorted by name, each with the field path to its
// receiver. It is the hook for interface dispatch: a dispatcher for method
// M on t calls fn with the receiver loaded along path (fieldPathInto), so
// promoted methods need no wrapper functions. Methods of packages that are
// not compiled (the stubs) are left out.
//
// For a pointer type *T (15.6) the table holds T's methods of both
// receiver kinds and those promoted through embedded fields (the path
// starts at the pointer's target, laid out as T), except a promoted
// method with a pointer receiver, which would need a pointer into a
// field: no candidate (the checker rejects such a *T in an interface).
func (g *gen) methodTable(t types.Type) []promotedMethod {
	ms := types.NewMethodSet(t)
	elem := t
	if pt := pointerOf(t); pt != nil {
		elem = pt.Elem()
	}
	var out []promotedMethod
	for i := 0; i < ms.Len(); i++ {
		sel := ms.At(i)
		fn, ok := sel.Obj().(*types.Func)
		if !ok || !g.p.compiled(fn.Pkg()) {
			continue
		}
		idx := sel.Index()
		if _, ptr := fn.Type().(*types.Signature).Recv().Type().(*types.Pointer); ptr && (!isPointer(t) || len(idx) > 1) {
			continue // (Indirect is also true for a method of an embedded interface)
		}
		out = append(out, promotedMethod{name: fn.Name(), fn: fn, path: embedPath(elem, idx[:len(idx)-1])})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].name < out[j].name })
	return out
}
