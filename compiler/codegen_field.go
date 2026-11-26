package compiler

import (
	"go/ast"
	"go/types"
)

// Field assignment. Structs are immutable in the VM, so x.F = v builds a
// new struct, a copy of x with F replaced (GETFIELD each other field,
// NEWSTRUCT), and stores it back to x. A path x.A.B.C = v rebuilds every
// struct on it, innermost first. The root of the path is a local variable
// or a slice element s[i]; an element is read with INDEX and written back
// with SETINDEX (the slice's array is shared, as in Go).

// fieldStep is one step on a field assignment path: a field selection, or
// an element of an array the path goes through (codegen_array.go).
type fieldStep struct {
	st  *types.Named // the struct type the field is selected from; nil for an element step
	idx int          // the field's index
	arr *types.Array // element step: the array's type
	ix  uint8        // element step: the register holding the index
}

// fieldTarget is a field assignment target whose operands have been
// evaluated: a root and the field path from it.
type fieldTarget struct {
	local int // the root variable's register, or -1
	elem  int // first register of the root element's [s, i, v] block, or -1
	path  []fieldStep
	typ   types.Type // the type of the last field
}

// isFieldTarget reports whether assignment target e is a field selector,
// or an element of an array that is not a local variable (p.A[i], a[i][j]:
// a path written by rebuilding, see arrayStep).
func (f *fn) isFieldTarget(e ast.Expr) bool {
	switch e := ast.Unparen(e).(type) {
	case *ast.SelectorExpr:
		s := f.g.p.Info.Selections[e]
		return s != nil && s.Kind() == types.FieldVal
	case *ast.IndexExpr:
		return f.arrayStep(e)
	}
	return false
}

// fieldTargetOf evaluates the operands of field assignment target e, as Go
// does before any assignment: an element root's slice and index go into a
// fresh block [s, i, v] (released by the caller); a variable root needs
// nothing; then the indices of the arrays on the path, in source order.
// A promoted field is a path down the embedded fields.
func (f *fn) fieldTargetOf(e ast.Expr) fieldTarget {
	t := fieldTarget{local: -1, elem: -1, typ: f.g.p.Info.TypeOf(e)}
	x := ast.Unparen(e)
	var indices []ast.Expr // of the element steps, outermost first
	for {
		if sel, ok := x.(*ast.SelectorExpr); ok {
			s := f.g.p.Info.Selections[sel]
			if s == nil || s.Kind() != types.FieldVal || s.Indirect() {
				f.fail(sel, "internal error: unexpected field assignment target")
			}
			steps := embedPath(f.g.p.Info.TypeOf(sel.X), s.Index())
			if steps == nil {
				f.fail(sel, "internal error: unexpected field assignment target")
			}
			t.path = append(steps, t.path...)
			x = ast.Unparen(sel.X)
			continue
		}
		if ix, ok := x.(*ast.IndexExpr); ok && f.arrayStep(ix) {
			t.path = append([]fieldStep{{arr: arrayType(f.g.p.Info.TypeOf(ix.X))}}, t.path...)
			indices = append(indices, ix.Index)
			x = ast.Unparen(ix.X)
			continue
		}
		break
	}
	switch x := x.(type) {
	case *ast.Ident:
		t.local = int(f.localOf(x))
	case *ast.IndexExpr:
		blk := f.allocN(x, 3)
		f.exprInto(x.X, blk)
		f.exprInto(x.Index, blk+1)
		t.elem = int(blk)
	default:
		f.fail(x, "internal error: unexpected field assignment root %T", x)
	}
	k := len(indices) - 1 // innermost first: source order
	for i := range t.path {
		if t.path[i].arr != nil {
			r := f.allocScalar(indices[k])
			f.exprInto(indices[k], r)
			t.path[i].ix = r
			k--
		}
	}
	return t
}

// storeField writes field target t: fill puts the last field's new value
// into dst, given the register of the struct that holds the field (for
// the old value).
func (f *fn) storeField(n ast.Node, t fieldTarget, fill func(dst, holder uint8)) {
	mark := f.next
	root := uint8(t.local)
	if t.elem >= 0 {
		blk := uint8(t.elem)
		root = blk + 2
		f.asm.EmitExt(OpBuiltin, root, blk, 2, BIndex)
	}
	f.rebuild(n, t.path, root, root, fill)
	if t.elem >= 0 {
		blk := uint8(t.elem)
		f.asm.EmitExt(OpBuiltin, blk, blk, 3, BSetindex)
	}
	f.next = mark
}

// rebuild builds into dst a copy of the struct in src with the field
// path[0] replaced: by fill's value if it is the last step, else by the
// rebuilt struct of the rest of the path. dst may be src. An element step
// copies the array instead (rebuildElem).
func (f *fn) rebuild(n ast.Node, path []fieldStep, src, dst uint8, fill func(dst, holder uint8)) {
	step := path[0]
	if step.arr != nil {
		f.rebuildElem(n, path, src, dst, fill)
		return
	}
	nf := step.st.Underlying().(*types.Struct).NumFields()
	if nf > maxRegs {
		f.fail(n, "struct %s has too many fields", step.st.Obj().Name())
	}
	idx, ok := f.g.typeIdx[step.st.Obj()]
	if !ok {
		f.fail(n, "internal error: struct type %s has no index", step.st.Obj().Name())
	}
	mark := f.next
	base := f.allocN(n, nf)
	for j := 0; j < nf; j++ {
		if j != step.idx {
			f.asm.Emit(OpGetfield, base+uint8(j), src, uint8(j))
		}
	}
	slot := base + uint8(step.idx)
	if len(path) == 1 {
		fill(slot, src)
	} else {
		f.loadStep(n, step, src, slot)
		f.rebuild(n, path[1:], slot, slot, fill)
	}
	f.asm.EmitExt(OpNewstruct, dst, base, uint8(nf), idx)
	f.next = mark
}

// fieldAssign compiles x.F = rhs (a single target). With a variable root
// rhs is compiled straight into the new struct's field register: nothing
// it calls can change a local variable. With an element root rhs is
// evaluated first, since a call in it may change the element (a closure
// sharing the slice), unless it is inert.
func (f *fn) fieldAssign(l, rhs ast.Expr) {
	mark := f.next
	t := f.fieldTargetOf(l)
	if t.local >= 0 || f.inert(rhs) {
		f.storeField(l, t, func(dst, _ uint8) { f.valueInto(rhs, t.typ, dst) })
	} else {
		v := f.alloc(rhs)
		f.valueInto(rhs, t.typ, v)
		f.storeField(l, t, func(dst, _ uint8) { f.asm.Emit(OpMove, dst, v, 0) })
	}
	f.next = mark
}

// fieldOpAssign compiles x.F op= rhs, and x.F++ / x.F-- (rhs nil). The
// operands of the target are evaluated once; with an element root rhs is
// evaluated before the element is read (see fieldAssign).
func (f *fn) fieldOpAssign(l ast.Expr, op uint8, rhs ast.Expr) {
	mark := f.next
	t := f.fieldTargetOf(l)
	pre := -1
	if rhs != nil && t.elem >= 0 && !f.inert(rhs) {
		pre = int(f.rhsOperand(op, rhs))
	}
	f.storeField(l, t, func(v, holder uint8) {
		f.loadStep(l, t.path[len(t.path)-1], holder, v)
		f.opInto(l, op, v, rhs, pre)
	})
	f.next = mark
}

// opInto computes v = v op rhs in register v, which holds a value of e's
// type; rhs nil is the constant 1 (++ and --). pre, if not -1, is the
// register rhs was already evaluated into. += on strings concatenates.
func (f *fn) opInto(e ast.Expr, op uint8, v uint8, rhs ast.Expr, pre int) {
	mark := f.next
	switch {
	case rhs == nil:
		one := f.allocScalar(e)
		f.oneInto(e, one)
		f.arith(e, op, v, v, one)
	case op == OpAdd && isString(f.g.p.Info.TypeOf(e)):
		y := f.into(rhs)
		if pre >= 0 {
			y = f.moveFrom(uint8(pre))
		}
		f.seqCall(e, v, BStrConcat, f.moveFrom(v), y)
	default:
		r := uint8(pre)
		if pre < 0 {
			r = f.rhsOperand(op, rhs)
		}
		f.arith(e, op, v, v, r)
	}
	f.next = mark
}

// inert reports whether evaluating e cannot change anything: a constant
// or a local variable.
func (f *fn) inert(e ast.Expr) bool {
	if f.g.p.Info.Types[e].Value != nil {
		return true
	}
	_, ok := f.localReg(e)
	return ok
}
