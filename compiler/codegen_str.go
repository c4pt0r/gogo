package compiler

import (
	"go/ast"
	"go/token"
	"go/types"
)

// seqCall emits BUILTIN id with its arguments in fresh consecutive
// registers, each filled by the matching function, and the result in dst.
func (f *fn) seqCall(n ast.Node, dst uint8, id uint32, args ...func(r uint8)) {
	mark := f.next
	base := f.allocN(n, len(args))
	for i, g := range args {
		g(base + uint8(i))
	}
	f.asm.EmitExt(OpBuiltin, dst, base, uint8(len(args)), id)
	f.next = mark
}

// into returns the argument filler that compiles e into the register.
func (f *fn) into(e ast.Expr) func(r uint8) {
	return func(r uint8) { f.exprInto(e, r) }
}

// loadInt returns the filler that loads the small constant v.
func (f *fn) loadInt(v int32) func(r uint8) {
	return func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermInt(v)) }
}

// moveFrom returns the filler that copies register src.
func (f *fn) moveFrom(src uint8) func(r uint8) {
	return func(r uint8) { f.asm.Emit(OpMove, r, src, 0) }
}

// strBinary compiles the string operators that are not plain EQ/NE:
// concatenation and ordering. False if e is neither.
func (f *fn) strBinary(e *ast.BinaryExpr, dst uint8) bool {
	if !isString(f.g.p.Info.TypeOf(e.X)) {
		return false
	}
	switch e.Op {
	case token.ADD:
		f.seqCall(e, dst, BStrConcat, f.into(e.X), f.into(e.Y))
	case token.LSS, token.LEQ, token.GTR, token.GEQ:
		// cmp(x, y) is -1, 0 or 1: compare it with 0. Operands are
		// evaluated in source order.
		cmp := f.allocScalar(e)
		f.seqCall(e, cmp, BStrCmp, f.into(e.X), f.into(e.Y))
		zero := f.allocScalar(e)
		f.asm.EmitExt(OpLoadk, zero, 0, 0, TermInt(0))
		switch e.Op {
		case token.LSS:
			f.asm.Emit(OpLt, dst, cmp, zero)
		case token.LEQ:
			f.asm.Emit(OpLe, dst, cmp, zero)
		case token.GTR:
			f.asm.Emit(OpLt, dst, zero, cmp)
		default:
			f.asm.Emit(OpLe, dst, zero, cmp)
		}
	default:
		return false
	}
	return true
}

// indexInto compiles s[i] (a string's byte as an int-valued byte).
func (f *fn) indexInto(e *ast.IndexExpr, dst uint8) {
	f.seqCall(e, dst, BIndex, f.into(e.X), f.into(e.Index))
}

// sliceInto compiles s[lo:hi]; a missing lo is 0 and a missing hi is the
// length, computed by LEN from the operand's copy (two registers below),
// so that no user value can mean "to the end".
func (f *fn) sliceInto(e *ast.SliceExpr, dst uint8) {
	lo := f.loadInt(0)
	hi := func(r uint8) { f.seqCall(e, r, BLen, f.moveFrom(r-2)) }
	if e.Low != nil {
		lo = f.into(e.Low)
	}
	if e.High != nil {
		hi = f.into(e.High)
	}
	f.seqCall(e, dst, BSlice, f.into(e.X), lo, hi)
}

// rangeStmt compiles `for i, c := range s` over a string's bytes (or a
// slice's elements; over an integer: rangeInt). The string is copied into
// a hidden register and the byte index kept in another, so the body may
// reassign s, i and c without changing the loop.
func (f *fn) rangeStmt(s *ast.RangeStmt) {
	if isInteger(f.g.p.Info.TypeOf(s.X)) {
		f.rangeInt(s)
		return
	}
	if isMap(f.g.p.Info.TypeOf(s.X)) {
		f.rangeMap(s)
		return
	}
	label := f.takeLabel()
	scope := f.next
	str := f.alloc(s)
	f.exprInto(s.X, str)
	f.rangeArray(s, str)
	n := f.allocScalar(s)
	f.seqCall(s, n, BLen, f.moveFrom(str))
	idx := f.allocScalar(s)
	f.asm.EmitExt(OpLoadk, idx, 0, 0, TermInt(0))

	// Loop variables: fresh registers, assigned at the top of each pass.
	declare := func(e ast.Expr) (uint8, bool) {
		id, _ := e.(*ast.Ident)
		if id == nil || id.Name == "_" {
			return 0, false
		}
		v, ok := f.g.p.Info.Defs[id].(*types.Var)
		if !ok {
			f.fail(id, "internal error: range variable %s is not a definition", id.Name)
		}
		r := f.alloc(id)
		f.locals[v] = r
		return r, true
	}
	var key, val uint8
	var hasKey, hasVal bool
	if s.Key != nil {
		key, hasKey = declare(s.Key)
	}
	if s.Value != nil {
		val, hasVal = declare(s.Value)
	}

	topL, contL, endL := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(topL)
	mark := f.next
	more := f.allocScalar(s)
	f.asm.Emit(OpLt, more, idx, n)
	f.jump(OpJmpf, more, endL)
	f.next = mark
	if hasKey {
		f.asm.Emit(OpMove, key, idx, 0)
		f.boxVar(s, f.rangeVar(s.Key), key) // a fresh box each pass (Go 1.22)
	}
	if hasVal {
		f.seqCall(s, val, BIndex, f.moveFrom(str), f.moveFrom(idx))
		f.rangeElem(s, val)
		f.boxVar(s, f.rangeVar(s.Value), val)
	}
	f.breakTo = append(f.breakTo, endL)
	f.continueTo = append(f.continueTo, contL)
	f.setLabel(label, endL, contL)
	f.block(s.Body.List)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
	f.continueTo = f.continueTo[:len(f.continueTo)-1]
	f.dropLabel(label)
	f.bind(contL)
	one := f.allocScalar(s)
	f.asm.EmitExt(OpLoadk, one, 0, 0, TermInt(1))
	f.asm.Emit(OpAdd, idx, idx, one)
	f.next = mark
	f.clearDead() // the body's locals die before the next iteration
	f.jump(OpJmp, 0, topL)
	f.bindEnd(endL)
	f.next = scope
}
