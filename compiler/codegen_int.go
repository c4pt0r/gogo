package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
	"math"
)

// Sized integers. Every integer type lives in one of two representations
// (design.md, Values): the int one (a 31-bit immediate, or a K_INT box
// beyond) for the types of at most 32 bits, and the int64 one (an
// immediate, or a K_INT64 box) for int64 and uint64. An unsigned type is
// stored as the bit pattern of the signed type of its width: uint32(1<<32
// - 1) is the int -1, uint64(1<<64 - 1) the int64 -1. A type narrower than
// 32 bits holds its value (0 .. 255 for uint8, -128 .. 127 for int8), and
// arithmetic on it is truncated back after each operation (OP_CONV c=2..5).
// Equality, + - * & | ^ &^ << and negation are the signed ops at every
// width; division, remainder, ordering and >> differ for the unsigned
// types: a uint64 has its own ops (OP_DIVU64 .. OP_SHRU64), a uint32 is
// widened to an int64 (OP_CONV c=6, exact) for them, the result truncated
// back to 32 bits (OP_CONV c=0).

// intKind describes an integer type: its width in bits (0 for a type that
// is not an integer) and whether it is unsigned. An untyped integer
// constant (and rune) is an int.
type intKind struct {
	bits     int
	unsigned bool
}

// intKindOf is t's intKind, through named types.
func intKindOf(t types.Type) intKind {
	b, ok := underlying(t).(*types.Basic)
	if !ok {
		return intKind{}
	}
	switch b.Kind() {
	case types.Int, types.Int32, types.UntypedInt, types.UntypedRune:
		return intKind{32, false}
	case types.Int8:
		return intKind{8, false}
	case types.Int16:
		return intKind{16, false}
	case types.Int64:
		return intKind{64, false}
	case types.Uint8:
		return intKind{8, true}
	case types.Uint16:
		return intKind{16, true}
	case types.Uint, types.Uint32:
		return intKind{32, true}
	case types.Uint64:
		return intKind{64, true}
	}
	return intKind{}
}

// isWide reports whether t is a 64-bit integer type (int64, uint64 or a
// named type of one): the int64 representation and the *64 ops.
func isWide(t types.Type) bool { return intKindOf(t).bits == 64 }

// isUint32 reports whether t is uint32 or uint (or a named type of one).
func isUint32(t types.Type) bool { return intKindOf(t) == intKind{32, true} }

// isUint64 reports whether t is uint64 (or a named type of one).
func isUint64(t types.Type) bool { return intKindOf(t) == intKind{64, true} }

// truncMode is the OP_CONV mode that brings an int-representation value
// back into the range of a type narrower than 32 bits; false for others.
func truncMode(k intKind) (uint8, bool) {
	switch k {
	case intKind{8, true}:
		return ConvByte, true
	case intKind{16, true}:
		return ConvUint16, true
	case intKind{8, false}:
		return ConvInt8, true
	case intKind{16, false}:
		return ConvInt16, true
	}
	return 0, false
}

// constBits is the integer constant v as a value of type t in t's
// representation: the int32 bit pattern for a type of at most 32 bits,
// the int64 one for int64 and uint64. False if v does not fit t (go/types
// has already reported an overflow of a typed constant).
func constBits(v constant.Value, t types.Type) (int64, bool) {
	v = constant.ToInt(v)
	k := intKindOf(t)
	if k.bits == 0 {
		k = intKind{32, false}
	}
	if k.unsigned {
		u, ok := constant.Uint64Val(v)
		if !ok || k.bits < 64 && u >= 1<<k.bits {
			return 0, false
		}
		if k.bits == 32 {
			return int64(int32(uint32(u))), true
		}
		return int64(u), true
	}
	n, ok := constant.Int64Val(v)
	if !ok || k.bits < 64 && (n < -1<<(k.bits-1) || n >= 1<<(k.bits-1)) {
		return 0, false
	}
	return n, true
}

// trunc truncates register r, just computed by arithmetic on e (an
// expression or variable of a type narrower than 32 bits), back into the
// type's range. Nothing for any other type.
func (f *fn) trunc(e ast.Expr, r uint8) {
	if c, ok := truncMode(intKindOf(f.g.p.Info.TypeOf(e))); ok {
		f.asm.Emit(OpConv, r, r, c)
	}
}

// truncOp is trunc for a result of op: &, |, ^, &^ and >> of two values in
// a narrow type's range stay in it, and so do / and % of unsigned ones.
func (f *fn) truncOp(e ast.Expr, r uint8, op uint8) {
	switch op {
	case OpAnd, OpOr, OpXor, OpAndnot, OpShr:
		return
	case OpDiv, OpRem:
		if intKindOf(f.g.p.Info.TypeOf(e)).unsigned {
			return
		}
	}
	f.trunc(e, r)
}

// uint64Ops maps the generic ops that differ for unsigned operands to
// their uint64 twins.
var uint64Ops = map[uint8]uint8{
	OpDiv: OpDivu64, OpRem: OpRemu64, OpLt: OpLtu64, OpLe: OpLeu64, OpShr: OpShru64,
}

// arith emits dst = x op y for operands of e's type (op one of the
// generic ops OpAdd .. OpShr, OpEq, OpNe, OpLt, OpLe; y is a shift count
// from countOperand for a shift), and truncates the result to e's type
// when it is an integer narrower than 32 bits. x and y may be dst.
func (f *fn) arith(e ast.Expr, op uint8, dst, x, y uint8) {
	t := f.g.p.Info.TypeOf(e)
	if _, ok := uint64Ops[op]; ok && isUint32(t) {
		// Widen to int64 (zero-extended): on 0 .. 2^32-1 the signed int64
		// op is the unsigned one.
		mark := f.next
		wx := f.alloc(e)
		f.asm.Emit(OpConv, wx, x, ConvUint64)
		wy := y
		if op != OpShr {
			wy = f.alloc(e)
			f.asm.Emit(OpConv, wy, y, ConvUint64)
		}
		f.asm.Emit(int64Ops[op], dst, wx, wy)
		if op != OpLt && op != OpLe {
			f.asm.Emit(OpConv, dst, dst, ConvInt)
		}
		f.next = mark
		return
	}
	f.asm.Emit(f.typedOp(op, e), dst, x, y)
	if op != OpEq && op != OpNe && op != OpLt && op != OpLe {
		f.truncOp(e, dst, op)
	}
}

// rhsOperand evaluates the right operand of op: a shift count goes
// through countOperand.
func (f *fn) rhsOperand(op uint8, e ast.Expr) uint8 {
	if op == OpShl || op == OpShr {
		return f.countOperand(e)
	}
	return f.operand(e)
}

// countOperand evaluates the shift count e into a register the shift ops
// read right: any integer, where a negative one crashes with badshift and
// one at or above the width shifts every bit out. An unsigned count is
// never negative: a uint32 is widened to an int64, a uint64 at or above
// 2^63 (an int64 below 0) becomes 64, a constant beyond 64 is 64.
func (f *fn) countOperand(e ast.Expr) uint8 {
	t := f.g.p.Info.TypeOf(e)
	if tv := f.g.p.Info.Types[e]; tv.Value != nil {
		if u, ok := constant.Uint64Val(constant.ToInt(tv.Value)); ok && u > 64 {
			r := f.allocScalar(e)
			f.asm.EmitExt(OpLoadk, r, 0, 0, TermInt(64))
			return r
		}
		return f.operand(e)
	}
	k := intKindOf(t)
	if !k.unsigned || k.bits < 32 {
		return f.operand(e)
	}
	x := f.operand(e)
	r := f.alloc(e)
	if k.bits == 32 {
		f.asm.Emit(OpConv, r, x, ConvUint64)
		return r
	}
	// r = x <u 64 ? x : 64
	c := f.allocScalar(e)
	done := f.asm.NewLabel()
	f.asm.Emit(OpMove, r, x, 0)
	f.asm.EmitExt(OpLoadk, c, 0, 0, TermInt(64))
	f.asm.Emit(OpLtu64, c, r, c)
	f.jump(OpJmpt, c, done)
	f.asm.EmitExt(OpLoadk, r, 0, 0, TermInt(64))
	f.bind(done)
	return r
}

// intConvMode is the OP_CONV mode of a conversion from integer type from
// to integer type to; false if the value stays the same term. Within the
// int representation the bits stay unless to is narrower than 32 bits and
// from's range does not fit in to's; into the int64 one a 32-bit value is
// sign- or zero-extended; out of it to the low 32 bits (then truncated
// further).
func intConvMode(to, from types.Type) (uint8, bool) {
	tk, fk := intKindOf(to), intKindOf(from)
	switch {
	case tk.bits == 64 && fk.bits == 64, tk.bits == 64 && fk.bits < 32, tk.bits == 32 && fk.bits <= 32:
		// the same term: int64 <-> uint64, a narrow value widened (an
		// immediate of either representation), int <-> uint32
		return 0, false
	case tk.bits == 64:
		if fk.unsigned {
			return ConvUint64, true
		}
		return ConvInt64, true
	case tk.bits == 32: // of an int64 or uint64
		return ConvInt, true
	case tk == fk || fk.bits < tk.bits && (fk.unsigned || !tk.unsigned):
		return 0, false // a narrow type, in range already
	}
	return truncMode(tk)
}

// uint64ToFloat compiles float32(x) or float64(x) (mode ConvF32Int or
// ConvF64Int) of the uint64 in register x into dst. Below 2^63 the int64
// conversion is the one; from 2^63 (a negative int64) half of x, its low
// bit kept as a sticky bit so that it rounds once, is converted and
// doubled (exact).
func (f *fn) uint64ToFloat(n ast.Node, mode uint8, dst, x uint8) {
	mark := f.next
	c, h, l := f.allocScalar(n), f.alloc(n), f.alloc(n)
	hi, end := f.asm.NewLabel(), f.asm.NewLabel()
	f.asm.EmitExt(OpLoadk, c, 0, 0, TermInt(0))
	f.asm.Emit(OpLt64, c, x, c)
	f.jump(OpJmpt, c, hi)
	f.asm.Emit(OpConvf, dst, x, mode)
	f.jump(OpJmp, 0, end)
	f.bind(hi)
	f.asm.EmitExt(OpLoadk, c, 0, 0, TermInt(1))
	f.asm.Emit(OpShru64, h, x, c)
	f.asm.Emit(OpAnd64, l, x, c)
	f.asm.Emit(OpOr64, h, h, l)
	f.asm.Emit(OpConvf, dst, h, mode)
	f.asm.Emit(OpAddf, dst, dst, dst)
	f.bind(end)
	f.next = mark
}

// floatToUint64 compiles uint64(x) of the float (of type ft) in register x
// into dst, as Go on arm64 does: truncated toward zero, NaN and negative
// values give 0, values from 2^64 the maximum. Below 2^63 it is the int64
// conversion (0 if negative); from 2^63 the int64 conversion of x - 2^63
// with the top bit set (saturating at the int64 maximum, which gives
// 2^64-1).
func (f *fn) floatToUint64(n ast.Node, ft types.Type, dst, x uint8) {
	mark := f.next
	k, c := f.alloc(n), f.allocScalar(n)
	big, nan, end := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.floatInto(constant.MakeFloat64(1<<63), ft, k)
	f.asm.Emit(OpLtf, c, x, k)
	f.jump(OpJmpf, c, big) // NaN too
	f.asm.Emit(OpConvf, dst, x, ConvInt64F)
	f.asm.EmitExt(OpLoadk, c, 0, 0, TermInt(0))
	f.asm.Emit(OpLt64, c, dst, c)
	f.jump(OpJmpf, c, end)
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermInt(0))
	f.jump(OpJmp, 0, end)
	f.bind(big)
	f.asm.Emit(OpNef, c, x, x)
	f.jump(OpJmpt, c, nan)
	f.asm.Emit(OpSubf, k, x, k)
	f.asm.Emit(OpConvf, dst, k, ConvInt64F)
	f.int64Into(math.MinInt64, k)
	f.asm.Emit(OpXor64, dst, dst, k)
	f.jump(OpJmp, 0, end)
	f.bind(nan)
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermInt(0))
	f.bind(end)
	f.next = mark
}

// printlnUnsigned compiles println(args...) with a uint32 or uint64
// argument into dst: B_PRINTLNU with the mask of the uint64 arguments
// first (a uint64 prints from its int64 bits), each uint32 widened to its
// int64 value. False if no argument is unsigned (B_PRINTLN does).
func (f *fn) printlnUnsigned(call *ast.CallExpr, dst uint8) bool {
	mask, any := int32(0), false
	for i, a := range call.Args {
		switch t := f.g.p.Info.TypeOf(a); {
		case isUint64(t):
			if i >= 30 {
				f.fail(a, "println: a uint64 after the 30th argument is not supported")
			}
			mask |= 1 << i
			any = true
		case isUint32(t):
			any = true
		}
	}
	if !any {
		return false
	}
	m := f.allocScalar(call)
	f.asm.EmitExt(OpLoadk, m, 0, 0, TermInt(mask))
	base, n := f.argsAs(call, nil)
	for i, a := range call.Args {
		if isUint32(f.g.p.Info.TypeOf(a)) {
			r := base + uint8(i)
			f.asm.Emit(OpConv, r, r, ConvUint64)
			f.dirty = max(f.dirty, int(r)+1) // may be a heap box now
		}
	}
	f.asm.EmitExt(OpBuiltin, dst, m, n+1, BPrintlnu)
	return true
}
