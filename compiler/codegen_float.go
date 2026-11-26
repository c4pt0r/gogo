package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
)

// Floating point: float32 and float64 values are K_FLOAT boxes (heap or
// const area) of their own width; the float ops compute in the width of
// their operands, so float32 and float64 share OpAddf .. OpConvf.

// B_FLOAT op codes (the first argument; FLOAT_OP_* in
// runtime/floatlib/floatlib.h, cross-checked by TestFloatOpsMatchC).
const (
	FloatSqrt      = 0
	FloatFloor     = 1
	FloatCeil      = 2
	FloatTrunc     = 3
	FloatRound     = 4
	FloatRoundEven = 5
	FloatAbs       = 6
	FloatCopysign  = 7
	FloatSignbit   = 8
	FloatInf       = 9
	FloatNaN       = 10
	FloatIsNaN     = 11
	FloatIsInf     = 12
	FloatMax       = 13
	FloatMin       = 14
	FloatFormat    = 15
	FloatParse     = 16
)

// floatOps maps each gogo/math function and strconv intrinsic (stubName)
// to its B_FLOAT op code. strconv.ParseFloat has two results (twoResult).
var floatOps = map[string]int32{
	"math.Sqrt": FloatSqrt, "math.Floor": FloatFloor, "math.Ceil": FloatCeil,
	"math.Trunc": FloatTrunc, "math.Round": FloatRound, "math.RoundToEven": FloatRoundEven,
	"math.Abs": FloatAbs, "math.Copysign": FloatCopysign, "math.Signbit": FloatSignbit,
	"math.Inf": FloatInf, "math.NaN": FloatNaN, "math.IsNaN": FloatIsNaN, "math.IsInf": FloatIsInf,
	"math.Max": FloatMax, "math.Min": FloatMin,
	"strconv.FormatFloat": FloatFormat, "strconv.parseFloat": FloatParse,
}

// floatCall compiles a call of a gogo/math function or a strconv float
// intrinsic (name as stubName gives it) into dst (and dst+1 for
// ParseFloat): B_FLOAT with the op code in the first register, then the
// arguments converted to the parameter types.
func (f *fn) floatCall(call *ast.CallExpr, name string, dst uint8) {
	mark := f.next
	defer func() { f.next = mark }()
	op, ok := floatOps[name]
	if !ok {
		f.fail(call, "internal error: unexpected float call %s", name)
	}
	if op == FloatInf || op == FloatNaN || op == FloatParse {
		f.g.b.MakesFloat() // the others take floats, which come from somewhere that makes them
	}
	base := f.allocScalar(call)
	f.asm.EmitExt(OpLoadk, base, 0, 0, TermInt(op))
	var params *types.Tuple
	if sig, ok := f.g.p.Info.TypeOf(call.Fun).(*types.Signature); ok {
		params = sig.Params()
	}
	_, n := f.argsAs(call, params) // right above the op code
	f.asm.EmitExt(OpBuiltin, dst, base, n+1, BFloat)
}

// isFloat reports whether t's underlying type is float32 or float64 (or
// an untyped float constant).
func isFloat(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&types.IsFloat != 0
}

// isFloat32 reports whether t's underlying type is float32.
func isFloat32(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Kind() == types.Float32
}

// floatOpsOf maps each generic arithmetic/compare op to its float twin.
var floatOpsOf = map[uint8]uint8{
	OpAdd: OpAddf, OpSub: OpSubf, OpMul: OpMulf, OpDiv: OpDivf,
	OpNeg: OpNegf, OpEq: OpEqf, OpNe: OpNef, OpLt: OpLtf, OpLe: OpLef,
}

// floatInto loads the float constant v as a value of type t (float32 or
// float64) into dst: a const-area K_FLOAT (no allocation).
func (f *fn) floatInto(v constant.Value, t types.Type, dst uint8) {
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.floatConst(v, t)))
}

// floatConst is the const-area offset of the K_FLOAT holding constant v
// as a value of type t. go/types has rounded a typed constant to its type
// already; Float64Val / Float32Val round an exact value to nearest.
func (g *gen) floatConst(v constant.Value, t types.Type) uint32 {
	v = constant.ToFloat(v)
	if isFloat32(t) {
		x, _ := constant.Float32Val(v)
		return g.b.AddFloat32(x)
	}
	x, _ := constant.Float64Val(v)
	return g.b.AddFloat64(x)
}

// floatConversion compiles a conversion to or from a float type (the
// checker admits any integer or float type on either side) into dst. A
// uint32 is converted through its int64 value, a uint64 by
// uint64ToFloat; to a type narrower than 32 bits through int (as Go on
// arm64: byte(300.7) is 44), to uint32 through int64 (uint32(-1.0) is
// 4294967295, uint32(1e10) 1410065408), to uint64 by floatToUint64.
func (f *fn) floatConversion(call *ast.CallExpr, to, from types.Type, dst uint8) {
	var c uint8
	switch {
	case isFloat(to) && isFloat(from):
		if isFloat32(to) == isFloat32(from) {
			f.exprInto(call.Args[0], dst) // a named float type: the same value
			return
		}
		c = ConvF64F
		if isFloat32(to) {
			c = ConvF32F
		}
	case isFloat(to): // of an integer
		c = ConvF64Int
		if isFloat32(to) {
			c = ConvF32Int
		}
	case isWide(to), isUint32(to):
		c = ConvInt64F
	default: // int and the narrower types
		c = ConvIntF
	}
	mark := f.next
	x := f.operand(call.Args[0])
	switch {
	case isUint64(from) && isFloat(to):
		f.uint64ToFloat(call, c, dst, x)
	case isUint64(to):
		f.floatToUint64(call, from, dst, x)
	case isUint32(from) && isFloat(to):
		w := f.alloc(call)
		f.asm.Emit(OpConv, w, x, ConvUint64)
		f.asm.Emit(OpConvf, dst, w, c)
	default:
		f.asm.Emit(OpConvf, dst, x, c)
		if isUint32(to) {
			f.asm.Emit(OpConv, dst, dst, ConvInt) // the low 32 bits
		}
	}
	f.next = mark
	f.trunc(call, dst) // a narrow type: int(f), then its low bits
}

// oneInto loads the constant 1 of e's type (int, int64, byte or a float
// type) into dst, for x++ and x-- on a variable, an element or a field:
// an immediate, or a const-area float (never a heap object).
func (f *fn) oneInto(e ast.Expr, dst uint8) {
	switch t := f.g.p.Info.TypeOf(e); {
	case isFloat(t):
		f.floatInto(constant.MakeInt64(1), t, dst)
	case isWide(t):
		f.int64Into(1, dst)
	default:
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermInt(1))
	}
}
