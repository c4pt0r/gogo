package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
)

// Interface values. A value in an interface is the value itself, as a
// term the VM can tell apart from every other type's (TYPEIS): an int, a
// string, a struct of its own type id, ... Two kinds of value would be
// indistinguishable and are boxed on the way in, into a struct of one
// field whose type stands for theirs: a value of a named non-struct type
// (type Code int: a box of type Code, so it is neither an int nor another
// named int) and a byte (a box of type "byte", so it is not an int). An
// int64 goes in as a K_INT64 box (BOX64), so it is not an int either.
// Taking a value out (a type assertion, a type switch case) unboxes it.

// isIface reports whether t is an interface type (any included).
func isIface(t types.Type) bool {
	_, ok := underlying(t).(*types.Interface)
	return ok
}

// boxed reports whether a value of concrete type t is boxed in an
// interface: a named type whose underlying type is basic, or a sized
// integer (byte, rune, int8, uint32, uint64, ...), whose value alone an
// int or int64 could not be told apart from.
func boxed(t types.Type) bool {
	if n, ok := types.Unalias(t).(*types.Named); ok {
		_, basic := n.Underlying().(*types.Basic)
		return basic
	}
	k := basicKind(t)
	return k != types.Int && k != types.Int64 && k != types.UntypedInt && intKindOf(t).bits != 0
}

// boxName is the image type name of the box of type t (boxed): the
// qualified name of a named type, else the integer type's own ("byte",
// "int32" for a rune too, "uint64", ...).
func (g *gen) boxName(t types.Type) string {
	if n, ok := types.Unalias(t).(*types.Named); ok {
		return g.qualified(n.Obj().Pkg(), n.Obj().Name())
	}
	switch k := basicKind(t); k {
	case types.Uint8:
		return "byte"
	case types.UntypedRune:
		return "int32"
	default:
		return types.Typ[k].Name()
	}
}

// boxIndex returns the image type index of the box of type t (boxed),
// giving it an own type index (one field) when first used, unless its
// type is a reserved one (its declaration's //gogo:reserved).
func (g *gen) boxIndex(t types.Type) uint32 {
	if n, ok := types.Unalias(t).(*types.Named); ok {
		if idx, ok := g.typeIdx[n.Obj()]; ok {
			g.b.UseType(idx)
			return idx
		}
	}
	key := g.boxName(t)
	if idx, ok := g.boxIdx[key]; ok {
		return idx
	}
	if g.boxIdx == nil {
		g.boxIdx = map[string]uint32{}
	}
	idx := g.b.AddType(key, 1)
	g.boxIdx[key] = idx
	return idx
}

// typeID returns the TYPEIS id of concrete type t: a TY_* id for the
// basic types, PIDs, Refs and Bytes, the type index of a struct type or
// of a box. False for a type without one (a slice, a func, a Buffer).
func (g *gen) typeID(t types.Type) (uint32, bool) {
	switch {
	case isPID(t):
		return TyPid, true
	case isRef(t):
		return TyRef, true
	case isBytes(t):
		return TyBytes, true
	case boxed(t):
		return g.boxIndex(t), true
	}
	if b, ok := types.Unalias(t).(*types.Basic); ok {
		switch b.Kind() {
		case types.Int:
			return TyInt, true
		case types.Int64:
			return TyInt64, true
		case types.Float32:
			return TyFloat32, true
		case types.Float64:
			return TyFloat64, true
		case types.Bool:
			return TyBool, true
		case types.String:
			return TyString, true
		}
	}
	if st := structType(t); st != nil {
		return g.typeIndex(st)
	}
	return 0, false
}

// toIface converts the value in register r, of concrete type src, to an
// interface value in place: a box for a boxed type, a K_INT64 box for an
// int64; nothing for any other type.
func (f *fn) toIface(n ast.Node, src types.Type, r uint8) {
	switch {
	case boxed(src):
		f.asm.EmitExt(OpNewstruct, r, r, 1, f.g.boxIndex(src))
	case isInt64(src):
		f.asm.Emit(OpBox64, r, r, 0)
	}
}

// convertsToIface reports whether a value of type src going to a
// destination of type target is converted: target is an interface and
// src a concrete type that is boxed in one (named basic, byte, int64).
func convertsToIface(src, target types.Type) bool {
	return target != nil && isIface(target) && src != nil && !isIface(src) && (boxed(src) || isInt64(src))
}

// constBox loads the boxed constant e (of a boxed type with constant
// value v) into dst: a struct in the const area, so no allocation.
func (f *fn) constBox(e ast.Expr, v constant.Value, dst uint8) {
	t := f.g.p.Info.TypeOf(e)
	var field uint32
	switch v.Kind() {
	case constant.Bool:
		field = TermFalse
		if constant.BoolVal(v) {
			field = TermTrue
		}
	case constant.String:
		field = TermConst(f.g.b.AddString(constant.StringVal(v)))
	case constant.Float:
		x, _ := constant.Float64Val(v)
		if basicKind(t.Underlying()) == types.Float32 {
			field = TermConst(f.g.b.AddFloat32(float32(x)))
		} else {
			field = TermConst(f.g.b.AddFloat64(x))
		}
	case constant.Int:
		n, exact := constant.Int64Val(v)
		switch k := intKindOf(t); {
		case k.unsigned && k.bits == 64:
			u, ok := constant.Uint64Val(v)
			n, exact = int64(u), ok // the bits of the value, as a uint64 is kept
		case k.unsigned && n >= 1<<31 && n < 1<<32:
			n = int64(int32(uint32(n))) // a uint32's bits, as an int
		}
		switch {
		case !exact || (!isWide(t) && (n < -1<<31 || n >= 1<<31)):
			f.fail(e, "constant %s overflows %s", v, t)
		case n >= -(1<<30) && n < 1<<30:
			field = TermInt(int32(n))
		case isWide(t):
			field = TermConst(f.g.b.AddInt64(n))
		default: // an int outside 31 bits: computed (LOADI32 boxes it as a K_INT)
			f.intInto(int32(n), dst)
			f.toIface(e, t, dst)
			return
		}
	default:
		f.fail(e, "constant %s not supported", v)
	}
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.b.AddStruct(f.g.boxIndex(t), []uint32{field})))
}

// assertMsg is the panic value of a failed x.(T).
func (f *fn) assertMsg(e *ast.TypeAssertExpr) string {
	q := func(p *types.Package) string { return pkgPrefix(p) }
	return "interface conversion: " + types.TypeString(f.g.p.Info.TypeOf(e.X), q) + " is not " + types.TypeString(f.g.p.Info.TypeOf(e.Type), q)
}

// typeTest emits t = (the interface value in register x has type T), T
// the type of type expression te.
func (f *fn) typeTest(te ast.Expr, x, t uint8) {
	id, ok := f.g.typeID(f.g.p.Info.TypeOf(te))
	if !ok {
		f.fail(te, "internal error: type %s has no type id", types.TypeString(f.g.p.Info.TypeOf(te), nil))
	}
	f.asm.EmitExt(OpTypeis, t, x, 0, id)
}

// unbox turns the interface value in register r, known to be of concrete
// type t, into a value of type t in place: the field of a box; an int64,
// boxed in every interface, back in the representation its arithmetic
// and equality expect outside one (an immediate when it fits).
func (f *fn) unbox(n ast.Node, t types.Type, r uint8) {
	switch {
	case boxed(t):
		f.asm.Emit(OpGetfield, r, r, 0)
	case isWide(t):
		mark := f.next
		z := f.allocScalar(n)
		f.asm.EmitExt(OpLoadi32, z, 0, 0, 0)
		f.asm.Emit(OpAdd64, r, r, z)
		f.next = mark
	}
}

// assertInto compiles x.(T) into dst: x's value if it has type T,
// unboxed, else a panic.
func (f *fn) assertInto(e *ast.TypeAssertExpr, dst uint8) {
	mark := f.next
	defer func() { f.next = mark }()
	f.exprInto(e.X, dst)
	t := f.allocScalar(e)
	f.typeTest(e.Type, dst, t)
	okL := f.asm.NewLabel()
	f.jump(OpJmpt, t, okL)
	f.panicMsg(e, f.assertMsg(e))
	f.bind(okL)
	f.unbox(e, f.g.p.Info.TypeOf(e.Type), dst)
}

// assertOkInto compiles v, ok := x.(T) into base (v) and base+1 (ok), two
// fresh registers: v is x's value, unboxed, if it has type T, else T's
// zero value.
func (f *fn) assertOkInto(e *ast.TypeAssertExpr, base uint8) {
	f.exprInto(e.X, base)
	f.typeTest(e.Type, base, base+1)
	t := f.g.p.Info.TypeOf(e.Type)
	okL, endL := f.asm.NewLabel(), f.asm.NewLabel()
	f.jump(OpJmpt, base+1, okL)
	f.zeroInto(e, t, base)
	f.jump(OpJmp, 0, endL)
	f.bind(okL)
	f.unbox(e, t, base)
	f.bind(endL)
}

// panicMsg panics with the constant string msg.
func (f *fn) panicMsg(n ast.Node, msg string) {
	mark := f.next
	r := f.allocScalar(n)
	f.asm.EmitExt(OpLoadk, r, 0, 0, TermConst(f.g.b.AddString(msg)))
	f.asm.EmitExt(OpBuiltin, r, r, 1, BPanic)
	f.next = mark
}

// Equality of interface values and structs: B_CMP, the VM's order of two
// values (types first, then values; structs field by field), compared
// with 0. The other comparable types use EQ (EQ64 for int64).

// needsCmp reports whether == on values of type t is a B_CMP: an
// interface, an array (element by element), or a struct (not a PID or a
// Ref, which are terms).
func needsCmp(t types.Type) bool {
	return isIface(t) || isArray(t) || (structType(t) != nil && !isPID(t))
}

// cmpType is the type the operands of x == y are compared as: an
// interface one's (the other operand is converted to it, as in Go), else
// x's.
func cmpType(xt, yt types.Type) types.Type {
	if isIface(yt) && !isIface(xt) {
		return yt
	}
	return xt
}

// cmpInto emits dst = x == y (op OpEq) or x != y (OpNe) by B_CMP, x and y
// converted to type t.
func (f *fn) cmpInto(x, y ast.Expr, t types.Type, op uint8, dst uint8) {
	mark := f.next
	base := f.allocN(x, 2)
	f.valueInto(x, t, base)
	f.valueInto(y, t, base+1)
	f.cmpRegs(x, base, op, dst)
	f.next = mark
}

// cmpRegs emits dst = r == r+1 (op OpEq) or != (OpNe) by B_CMP, on two
// registers holding comparable values of one type; r+1 is clobbered.
func (f *fn) cmpRegs(n ast.Node, r uint8, op uint8, dst uint8) {
	f.asm.EmitExt(OpBuiltin, r, r, 2, BCmp)
	f.asm.EmitExt(OpLoadk, r+1, 0, 0, TermInt(0))
	f.asm.Emit(op, dst, r, r+1)
}
