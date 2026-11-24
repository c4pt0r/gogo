package compiler

import (
	"go/ast"
	"go/types"
)

// Slices: a slice value is the nil slice (TermNilSlice) or a header over a
// mutable backing array (vm/strs.c). Element reads, len and slicing use the
// generic sequence builtins (codegen_str.go); this file adds make, append,
// cap, copy, slice literals and element writes.

// seqBuiltin compiles the predeclared len, cap, append, make or copy into
// dst. False for any other builtin (println).
func (f *fn) seqBuiltin(call *ast.CallExpr, name string, dst uint8) bool {
	switch name {
	case "len":
		f.seqCall(call, dst, BLen, f.into(call.Args[0]))
	case "cap":
		f.seqCall(call, dst, BCap, f.into(call.Args[0]))
	case "copy":
		f.seqCall(call, dst, BCopy, f.into(call.Args[0]), f.into(call.Args[1]))
	case "make":
		f.makeInto(call, dst)
	case "append":
		f.appendInto(call, dst)
	default:
		return false
	}
	return true
}

// elemKind is MAKE_SLICE's element kind for slices of elem: 1 for bytes
// (a packed byte array), else 0.
func elemKind(elem types.Type) int32 {
	if isByte(elem) {
		return 1
	}
	return 0
}

// zeroFill returns the filler that loads the zero value of elem: the value
// a new array's spare elements get (bytes are always 0).
func (f *fn) zeroFill(n ast.Node, elem types.Type) func(r uint8) {
	if isByte(elem) {
		return f.loadInt(0)
	}
	return func(r uint8) { f.zeroInto(n, elem, r) }
}

// makeInto compiles make([]T, n) and make([]T, n, c).
func (f *fn) makeInto(call *ast.CallExpr, dst uint8) {
	elem := sliceElem(f.g.p.Info.TypeOf(call.Args[0]))
	capF := func(r uint8) { f.asm.Emit(OpMove, r, r-1, 0) } // cap = len
	if len(call.Args) == 3 {
		capF = f.into(call.Args[2])
	}
	f.seqCall(call, dst, BMakeSlice, f.loadInt(elemKind(elem)), f.into(call.Args[1]), capF, f.zeroFill(call, elem))
}

// sliceLit compiles []T{a, b, ...}: MAKE_SLICE of the exact length, then
// one SETINDEX per element, in a temporary moved to dst at the end (dst
// may be a variable the elements read).
func (f *fn) sliceLit(e *ast.CompositeLit, dst uint8) {
	elem := sliceElem(f.g.p.Info.TypeOf(e))
	n := int32(len(e.Elts))
	mark := f.next
	tmp := f.alloc(e)
	nilF := func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermNil) } // every slot is set below
	f.seqCall(e, tmp, BMakeSlice, f.loadInt(elemKind(elem)), f.loadInt(n), f.loadInt(n), nilF)
	if n > 0 {
		blk := f.allocN(e, 3)
		for i, el := range e.Elts {
			f.asm.Emit(OpMove, blk, tmp, 0)
			f.asm.EmitExt(OpLoadk, blk+1, 0, 0, TermInt(int32(i)))
			f.valueInto(el, elem, blk+2)
			f.asm.EmitExt(OpBuiltin, blk, blk, 3, BSetindex)
		}
	}
	f.asm.Emit(OpMove, dst, tmp, 0)
	f.next = mark
}

// appendZero returns the filler for APPEND's zero argument; the slice is
// two registers below it, n the number of values appended. A struct zero
// value is a new heap object, so it is built only when the append is about
// to grow the array (len + n > cap); otherwise the register holds nil,
// which the VM then does not read.
func (f *fn) appendZero(node ast.Node, elem types.Type, n int, spread bool) func(r uint8) {
	if structType(elem) == nil || spread {
		return f.zeroFill(node, elem)
	}
	return func(r uint8) {
		mark := f.next
		f.asm.EmitExt(OpLoadk, r, 0, 0, TermNil)
		l, c := f.allocScalar(node), f.allocScalar(node)
		f.seqCall(node, l, BLen, f.moveFrom(r-2))
		f.seqCall(node, c, BCap, f.moveFrom(r-2))
		k := f.allocScalar(node)
		f.asm.EmitExt(OpLoadk, k, 0, 0, TermInt(int32(n)))
		f.asm.Emit(OpAdd, l, l, k)
		f.asm.Emit(OpLe, l, l, c) // len + n <= cap: no growth
		skip := f.asm.NewLabel()
		f.jump(OpJmpt, l, skip)
		f.zeroInto(node, elem, r)
		f.bind(skip)
		f.next = mark
	}
}

// appendInto compiles append(s, v...), append(s, a, b, ...) and append(s)
// as one APPEND [s, mode, zero, values...]: the VM decides growth once
// for all the values, as Go does (if they do not all fit, none is written
// into the old array). Arguments are evaluated in source order.
func (f *fn) appendInto(call *ast.CallExpr, dst uint8) {
	elem := sliceElem(f.g.p.Info.TypeOf(call.Args[0]))
	bytes := isByte(elem)
	if len(call.Args) == 1 {
		f.exprInto(call.Args[0], dst)
		return
	}
	if len(call.Args)+2 > maxRegs {
		f.fail(call, "too many arguments")
	}
	spread := call.Ellipsis.IsValid()
	mode := AppendTerm
	switch {
	case spread && bytes:
		mode = AppendBytesSpread
	case spread:
		mode = AppendTermSpread
	case bytes:
		mode = AppendByte
	}
	fill := []func(r uint8){f.into(call.Args[0]), f.loadInt(mode),
		f.appendZero(call, elem, len(call.Args)-1, spread)}
	for _, a := range call.Args[1:] {
		if spread {
			fill = append(fill, f.into(a))
		} else {
			fill = append(fill, func(r uint8) { f.valueInto(a, elem, r) })
		}
	}
	f.seqCall(call, dst, BAppend, fill...)
}

// indexOpAssign compiles s[i] op= rhs, and s[i]++ / s[i]-- (rhs nil): s
// and i are evaluated once.
func (f *fn) indexOpAssign(ix *ast.IndexExpr, op uint8, rhs ast.Expr) {
	mark := f.next
	blk := f.allocN(ix, 3) // [s, i, v]
	f.exprInto(ix.X, blk)
	f.exprInto(ix.Index, blk+1)
	v := blk + 2
	f.asm.EmitExt(OpBuiltin, v, blk, 2, BIndex)
	et := f.g.p.Info.TypeOf(ix)
	switch {
	case rhs == nil:
		one := f.allocScalar(ix)
		if isInt64(et) {
			f.int64Into(1, one)
		} else {
			f.asm.EmitExt(OpLoadk, one, 0, 0, TermInt(1))
		}
		f.asm.Emit(f.typedOp(op, ix), v, v, one)
	case op == OpAdd && isString(et):
		f.seqCall(ix, v, BStrConcat, f.moveFrom(v), f.into(rhs))
	default:
		f.asm.Emit(f.typedOp(op, ix), v, v, f.operand(rhs))
	}
	f.truncByte(ix, v)
	f.asm.EmitExt(OpBuiltin, blk, blk, 3, BSetindex)
	f.next = mark
}
