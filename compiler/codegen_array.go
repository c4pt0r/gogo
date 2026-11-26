package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
)

// Arrays. An array [N]T is a slice of length and capacity N (a header over
// a mutable backing array, as make([]T, N) builds), so indexing, len,
// range, slicing and printing are the slice ones. What makes it a value
// is where the compiler copies it:
//
//   - The array of a local variable (a parameter, a named result, a range
//     variable) belongs to that variable alone. a[i] = v writes it in
//     place, a[:] shares it (as Go's slice of an addressable array does),
//     and a = b copies b's elements into it (COPY), so those slices see
//     the new values, as in Go.
//   - Every other array (a struct field, an element of a slice or of
//     another array) is never written in place: a write through it, such
//     as p.A[i] = v or a[i][j] = v, copies that array, sets the element
//     and stores the copy back along the path (rebuildElem), the way a
//     field write rebuilds a struct. So struct copies and array copies may
//     share inner arrays: none of them ever changes.
//   - An array value that goes anywhere (a variable, an argument, a result,
//     a field, an element) is copied unless it is a fresh array: a literal
//     or a call result (which its function copied already). See copyArray.
//
// Slicing an array that is not a local variable is rejected (the slice
// would see a copy, not later writes), and so are == on arrays (it needs
// a comparison builtin) and arrays in interfaces.

// arrayType returns t's underlying array type, or nil.
func arrayType(t types.Type) *types.Array {
	a, _ := underlying(t).(*types.Array)
	return a
}

// isArray reports whether t's underlying type is an array.
func isArray(t types.Type) bool { return arrayType(t) != nil }

// freshArray reports whether e, of an array type, evaluates to a new array
// that nothing else holds: a composite literal or a call (a function
// copies the arrays it returns). A conversion is not a call: [N]T(x) of an
// array, or of a slice (Go 1.20), would be x's own array.
func (g *gen) freshArray(e ast.Expr) bool {
	switch e := ast.Unparen(e).(type) {
	case *ast.CompositeLit:
		return true
	case *ast.CallExpr:
		return !g.p.Info.Types[e.Fun].IsType()
	}
	return false
}

// localArray reports whether e names a local variable of an array type:
// an array written in place.
func (f *fn) localArray(e ast.Expr) bool {
	if !isArray(f.g.p.Info.TypeOf(e)) {
		return false
	}
	_, ok := f.localReg(e)
	return ok
}

// arrayStep reports whether index expression ix selects an element of an
// array that is not a local variable: an element step of a field
// assignment path (fieldTargetOf), written by rebuildElem.
func (f *fn) arrayStep(ix *ast.IndexExpr) bool {
	return isArray(f.g.p.Info.TypeOf(ix.X)) && !f.localArray(ix.X)
}

// copiesArray reports whether e, going into a destination of type target,
// must be copied: an array that is not fresh.
func (f *fn) copiesArray(e ast.Expr, target types.Type) bool {
	return isArray(target) && !f.g.freshArray(e) && !f.isPreset(e)
}

// copyArray copies the array in dst (e's value, going into a destination
// of type target) if it must be (copiesArray). Nothing for other types.
func (f *fn) copyArray(e ast.Expr, target types.Type, dst uint8) {
	if f.copiesArray(e, target) {
		f.cloneArray(e, arrayType(target), dst, dst)
	}
}

// arrayLen is the length of array type at, which must fit a small int.
func (f *fn) arrayLen(n ast.Node, at *types.Array) int32 {
	if at.Len() < 0 || at.Len() >= 1<<30 {
		f.fail(n, "array of %d elements too large", at.Len())
	}
	return int32(at.Len())
}

// cloneArray builds into dst a new array of type at holding the elements
// of the array in src (MAKE_SLICE, COPY); dst may be src.
func (f *fn) cloneArray(n ast.Node, at *types.Array, src, dst uint8) {
	mark := f.next
	tmp := dst
	if dst == src {
		tmp = f.alloc(n)
	}
	size := f.arrayLen(n, at)
	nilF := func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermNil) } // every element is copied
	f.seqCall(n, tmp, BMakeSlice, f.loadInt(elemKind(at.Elem())), f.loadInt(size), f.loadInt(size), nilF)
	cnt := f.allocScalar(n)
	f.seqCall(n, cnt, BCopy, f.moveFrom(tmp), f.moveFrom(src))
	if tmp != dst {
		f.asm.Emit(OpMove, dst, tmp, 0)
	}
	f.next = mark
}

// zeroArray builds the zero value of array type at into dst. Every
// element is the same zero value: a struct or an inner array is never
// written in place, so they can share it.
func (f *fn) zeroArray(n ast.Node, at *types.Array, dst uint8) {
	size := f.arrayLen(n, at)
	f.seqCall(n, dst, BMakeSlice, f.loadInt(elemKind(at.Elem())), f.loadInt(size), f.loadInt(size), f.zeroFill(n, at.Elem()))
}

// arrayLit compiles [N]T{...} and [...]T{...}: the zero array (or, when
// every element is given, an unfilled one), then one SETINDEX per element,
// in source order, in a temporary moved to dst at the end. An element may
// have a constant index key ([5]int{2: 1, 4: 7}); one without a key
// follows the previous one.
func (f *fn) arrayLit(e *ast.CompositeLit, dst uint8) {
	at := arrayType(f.g.p.Info.TypeOf(e))
	size := f.arrayLen(e, at)
	mark := f.next
	tmp := f.alloc(e)
	keyed := false
	for _, el := range e.Elts {
		if _, ok := el.(*ast.KeyValueExpr); ok {
			keyed = true
		}
	}
	if keyed || int32(len(e.Elts)) < size {
		f.zeroArray(e, at, tmp)
	} else {
		nilF := func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermNil) }
		f.seqCall(e, tmp, BMakeSlice, f.loadInt(elemKind(at.Elem())), f.loadInt(size), f.loadInt(size), nilF)
	}
	if len(e.Elts) > 0 {
		blk := f.allocN(e, 3)
		i := int64(0)
		for _, el := range e.Elts {
			val := el
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				k, exact := constant.Int64Val(f.g.p.Info.Types[kv.Key].Value)
				if !exact {
					f.fail(kv.Key, "internal error: array index is not a constant")
				}
				i, val = k, kv.Value
			}
			f.asm.Emit(OpMove, blk, tmp, 0)
			f.asm.EmitExt(OpLoadk, blk+1, 0, 0, TermInt(int32(i)))
			f.valueInto(val, at.Elem(), blk+2)
			f.asm.EmitExt(OpBuiltin, blk, blk, 3, BSetindex)
			i++
		}
	}
	f.asm.Emit(OpMove, dst, tmp, 0)
	f.next = mark
}

// loadStep loads into dst the field or element that path step s selects
// from the struct or array in holder.
func (f *fn) loadStep(n ast.Node, s fieldStep, holder, dst uint8) {
	if s.arr == nil {
		f.asm.Emit(OpGetfield, dst, holder, uint8(s.idx))
		return
	}
	f.seqCall(n, dst, BIndex, f.moveFrom(holder), f.moveFrom(s.ix))
}

// rebuildElem is rebuild for an element step: dst gets a copy of the array
// in src with element path[0].ix replaced, by fill's value if it is the
// last step, else by the rebuilt value of the rest of the path. dst may be
// src.
func (f *fn) rebuildElem(n ast.Node, path []fieldStep, src, dst uint8, fill func(dst, holder uint8)) {
	step := path[0]
	mark := f.next
	blk := f.allocN(n, 3) // [copy, i, v] for SETINDEX
	f.cloneArray(n, step.arr, src, blk)
	f.asm.Emit(OpMove, blk+1, step.ix, 0)
	if len(path) == 1 {
		fill(blk+2, src)
	} else {
		f.loadStep(n, step, src, blk+2)
		f.rebuild(n, path[1:], blk+2, blk+2, fill)
	}
	f.asm.EmitExt(OpBuiltin, blk+1, blk, 3, BSetindex) // its nil result into the index: blk keeps the copy
	f.asm.Emit(OpMove, dst, blk, 0)
	f.next = mark
}

// assignArray compiles a = e for a local variable a of an array type: e's
// elements are copied into a's array (COPY), so the slices of a see them,
// as in Go. False for any other assignment.
func (f *fn) assignArray(lhs, rhs ast.Expr) bool {
	if !f.localArray(lhs) {
		return false
	}
	a := f.localOf(lhs)
	mark := f.next
	src := f.operand(rhs)
	f.copyElems(lhs, a, src)
	f.next = mark
	return true
}

// copyElems copies the elements of the array in src into the array in dst
// (two arrays of one type).
func (f *fn) copyElems(n ast.Node, dst, src uint8) {
	mark := f.next
	cnt := f.allocScalar(n)
	f.seqCall(n, cnt, BCopy, f.moveFrom(dst), f.moveFrom(src))
	f.next = mark
}

// retNamed returns the k named results (a bare return). An array result
// is copied: the variable's array may be shared by a slice of it.
func (f *fn) retNamed(n ast.Node, k int) {
	copies := false
	for i, t := range f.results {
		copies = copies || isArray(t) || f.namedBoxed(i) != nil
	}
	if !copies {
		f.ret(uint8(f.named), k)
		return
	}
	mark := f.next
	base := f.allocN(n, k)
	for i, t := range f.results {
		r := uint8(f.named + i)
		if at := arrayType(t); at != nil {
			f.cloneArray(n, at, r, base+uint8(i))
		} else if v := f.namedBoxed(i); v != nil { // a named result in a box (D7)
			f.boxedLoad(n, v, r, base+uint8(i))
		} else {
			f.asm.Emit(OpMove, base+uint8(i), r, 0)
		}
	}
	f.ret(base, k)
	f.next = mark
}

// namedBoxed returns named result i if it lives in a box, else nil.
func (f *fn) namedBoxed(i int) *types.Var {
	for v, r := range f.locals {
		if int(r) == f.named+i && f.g.boxed[v] && f.isResult(v) {
			return v
		}
	}
	return nil
}

// isResult reports whether v is a named result of the function being
// compiled.
func (f *fn) isResult(v *types.Var) bool {
	r, ok := f.locals[v]
	return ok && f.named >= 0 && int(r) >= f.named && int(r) < f.named+len(f.results)
}

// rangeArray prepares a range over an array: when the loop has a value
// variable and ranges over a local variable's array, it ranges over a
// copy (in register x), as Go does, so writes in the body do not show.
func (f *fn) rangeArray(s *ast.RangeStmt, x uint8) {
	if s.Value == nil || !f.localArray(s.X) {
		return
	}
	if id, ok := s.Value.(*ast.Ident); ok && id.Name == "_" {
		return
	}
	f.cloneArray(s, arrayType(f.g.p.Info.TypeOf(s.X)), x, x)
}

// rangeElem copies the range value in register v when it is an array: the
// loop variable's own array.
func (f *fn) rangeElem(s *ast.RangeStmt, v uint8) {
	if id, ok := s.Value.(*ast.Ident); ok {
		if at := arrayType(f.g.p.Info.TypeOf(id)); at != nil {
			f.cloneArray(s, at, v, v)
		}
	}
}
