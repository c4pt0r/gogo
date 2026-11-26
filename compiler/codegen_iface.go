package compiler

import (
	"fmt"
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
// value v) into dst: a struct in the const area, so no allocation. A box
// of a reserved type (an errors.Code) is built on the heap instead unless
// it is only compared (f.compared): a message may carry it to another
// module, which takes no struct out of this image's const area.
func (f *fn) constBox(e ast.Expr, v constant.Value, dst uint8) {
	t := f.g.p.Info.TypeOf(e)
	if idx := f.g.boxIndex(t); idx < TypeOwnBase && !f.compared {
		f.constInto(e, v, dst)
		f.asm.EmitExt(OpNewstruct, dst, dst, 1, idx)
		return
	}
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
	f.assertTest(e, dst, t)
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
	f.assertTest(e, base, base+1)
	t := f.g.p.Info.TypeOf(e.Type)
	okL, endL := f.asm.NewLabel(), f.asm.NewLabel()
	f.jump(OpJmpt, base+1, okL)
	f.zeroInto(e, t, base)
	f.jump(OpJmp, 0, endL)
	f.bind(okL)
	f.unbox(e, t, base)
	f.bind(endL)
}

// assertTest emits t = (the interface value in register x has type T, or
// implements T for an interface type T) for x.(T).
func (f *fn) assertTest(e *ast.TypeAssertExpr, x, t uint8) {
	if tt := f.g.p.Info.TypeOf(e.Type); isIface(tt) {
		f.ifaceTest(e, tt, x, t)
		return
	}
	f.typeTest(e.Type, x, t)
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
	if f.g.p.Info.Types[x].IsNil() {
		x, y = y, x
	}
	if xt := f.g.p.Info.TypeOf(x); f.g.p.Info.Types[y].IsNil() && isIface(xt) && !xt.Underlying().(*types.Interface).Empty() {
		// x == nil for a non-empty interface: never an int64 box (an
		// int64 has no methods), so EQ compares it with nil.
		mark := f.next
		r := f.operand(x)
		z := f.allocScalar(y)
		f.asm.EmitExt(OpLoadk, z, 0, 0, TermNil)
		f.asm.Emit(op, dst, r, z)
		f.next = mark
		return
	}
	mark := f.next
	base := f.allocN(x, int(cmpWidth(t)))
	f.comparedInto(x, t, base)
	f.comparedInto(y, t, base+1)
	f.cmpRegs(x, base, t, op, dst)
	f.next = mark
}

// comparedInto is valueInto for an operand of == that goes nowhere else:
// a boxed constant stays in the const area (constBox).
func (f *fn) comparedInto(e ast.Expr, t types.Type, dst uint8) {
	f.compared = f.g.p.Info.Types[e].Value != nil
	f.valueInto(e, t, dst)
	f.compared = false
}

// cmpWidth is the number of registers cmpRegs uses for values of type t.
func cmpWidth(t types.Type) uint8 {
	if isArray(t) {
		return 3
	}
	return 2
}

// cmpRegs emits dst = r == r+1 (op OpEq) or != (OpNe) by B_CMP, on two
// registers holding comparable values of type t; r+1 is clobbered. Two
// arrays pass a third register (r+2, allocated by the caller, its value
// unused): B_CMP compares slices at the top by their elements only then,
// as arrays; elsewhere a top-level slice is one held by an interface,
// which equals nothing.
func (f *fn) cmpRegs(n ast.Node, r uint8, t types.Type, op uint8, dst uint8) {
	f.asm.EmitExt(OpBuiltin, r, r, cmpWidth(t), BCmp)
	f.asm.EmitExt(OpLoadk, r+1, 0, 0, TermInt(0))
	f.asm.Emit(op, dst, r, r+1)
}

// Interfaces are dispatched in a closed world: the image holds every type
// whose values can be in its interface values (a module's own types never
// leave it, and the reserved types are the same everywhere), so a method
// call on an interface value calls a dispatcher, a function the compiler
// generates per interface method: a TYPEIS chain over the image's types
// that have that method, each calling its implementation. A type
// assertion or a type switch case to an interface type calls a similar
// chain that answers whether the value's type implements it.

// errorType is the universe interface error.
var errorType = types.Universe.Lookup("error").Type()

// concreteTypes lists the named non-interface types of every compiled
// package (libraries, system packages and main), in declaration order:
// the candidates of every dispatcher. The VM's own types (//gogo:builtin,
// whose methods are builtins) are not among them.
func (g *gen) concreteTypes() []types.Type {
	if g.concrete != nil {
		return g.concrete
	}
	g.concrete = []types.Type{}
	for _, pkg := range g.p.packages() {
		for _, f := range pkg.Files {
			for _, d := range f.Decls {
				gd, ok := d.(*ast.GenDecl)
				if !ok {
					continue
				}
				for _, s := range gd.Specs {
					ts, ok := s.(*ast.TypeSpec)
					if !ok || ts.Assign.IsValid() {
						continue
					}
					tn, ok := g.p.Info.Defs[ts.Name].(*types.TypeName)
					if !ok || g.p.isBuiltin(tn) || isIface(tn.Type()) {
						continue
					}
					if types.NewMethodSet(tn.Type()).Len() > 0 {
						g.concrete = append(g.concrete, tn.Type())
					}
				}
			}
		}
	}
	return g.concrete
}

// impl returns the method of concrete type t that implements interface
// method m (same name, as seen from m's package, and identical
// signature), its own or promoted from an embedded field (methodTable:
// with the field path to its receiver), or nil.
func (g *gen) impl(t types.Type, m *types.Func) *promotedMethod {
	n, ok := types.Unalias(t).(*types.Named)
	if !ok {
		return nil
	}
	for _, e := range g.methodTable(n) {
		if e.fn.Id() == m.Id() && types.Identical(e.fn.Type(), m.Type()) {
			return &e
		}
	}
	return nil
}

// ifaceName names a function generated for interface type t: name, with
// the qualifier of a function of t's package.
func (g *gen) ifaceName(t types.Type, name string) string {
	if n, ok := types.Unalias(t).(*types.Named); ok {
		if n.Obj().Pkg() == nil { // error
			return n.Obj().Name() + "." + name
		}
		return g.qualified(n.Obj().Pkg(), n.Obj().Name()+"."+name)
	}
	return types.TypeString(t, func(p *types.Package) string { return pkgPrefix(p) }) + "." + name
}

// methodFunc returns the function index that calls method m with its
// receiver: m's own, or, for a method promoted from an embedded field of
// interface type (m is the interface's method), its dispatcher.
func (g *gen) methodFunc(m *types.Func) (uint32, bool) {
	if recv := m.Type().(*types.Signature).Recv(); recv != nil && isIface(recv.Type()) {
		return g.dispatcher(m), true
	}
	return g.funcIndex(m)
}

// nilDeref is the panic of a method call on a nil interface value.
const nilDeref = "runtime error: invalid memory address or nil pointer dereference"

// dispatcher returns the function index of the dispatcher of interface
// method m (parameters: the interface value, then m's), generating it
// when first used: for each implementing type, TYPEIS, then a call of its
// method with the value (unboxed for a box) and the arguments, returning
// its results; a value of no such type (nil) panics.
func (g *gen) dispatcher(m *types.Func) uint32 {
	if idx, ok := g.dispIdx[m]; ok {
		return idx
	}
	sig := m.Type().(*types.Signature)
	info := &funcInfo{name: g.ifaceName(sig.Recv().Type(), m.Name())}
	idx := uint32(len(g.funcs))
	g.funcs = append(g.funcs, info)
	if g.dispIdx == nil {
		g.dispIdx = map[*types.Func]uint32{}
	}
	g.dispIdx[m] = idx
	np := 1 + sig.Params().Len()
	k := sig.Results().Len()
	t := uint8(np) // a scratch register for TYPEIS
	var a Asm
	for _, ct := range g.concreteTypes() {
		pm := g.impl(ct, m)
		if pm == nil {
			continue
		}
		id, ok := g.typeID(ct)
		fi, found := g.methodFunc(pm.fn)
		if !ok || !found {
			panic(genError{fmt.Errorf("gogo: internal error: %s: no type id or function for %s's %s", info.name, types.TypeString(ct, pkgName), m.Name())})
		}
		next := a.NewLabel()
		a.EmitExt(OpTypeis, t, 0, 0, id)
		a.EmitJump(OpJmpf, t, next)
		if boxed(ct) {
			a.Emit(OpGetfield, 0, 0, 0)
		}
		for _, step := range pm.path { // a promoted method: its embedded receiver
			a.Emit(OpGetfield, 0, 0, uint8(step.idx))
		}
		a.EmitExt(OpCall, 0, 0, uint8(np), fi)
		if k > 1 {
			a.Emit(OpRetn, 0, uint8(k), 0)
		} else {
			a.Emit(OpRet, 0, 0, 0)
		}
		a.Bind(next)
	}
	a.EmitExt(OpLoadk, t, 0, 0, TermConst(g.b.AddString(nilDeref)))
	a.EmitExt(OpBuiltin, t, t, 1, BPanic)
	a.Emit(OpRet, t, 0, 0)
	g.finishGenerated(info, &a, np, max(np+1, k))
	return idx
}

// implements returns the function index of the test "the dynamic type of
// parameter 0 implements interface type it", generating it when first
// used: a TYPEIS chain over the implementing types; for an empty
// interface, value != nil.
func (g *gen) implements(it types.Type) uint32 {
	for _, h := range g.isHelpers {
		if types.Identical(h.t, it) {
			return h.idx
		}
	}
	info := &funcInfo{name: g.ifaceName(it, "(is)")}
	idx := uint32(len(g.funcs))
	g.funcs = append(g.funcs, info)
	g.isHelpers = append(g.isHelpers, isHelper{it, idx})
	var a Asm
	iface := it.Underlying().(*types.Interface)
	if iface.Empty() {
		a.EmitExt(OpLoadk, 1, 0, 0, TermNil)
		a.EmitExt(OpBuiltin, 0, 0, 2, BCmp)
		a.EmitExt(OpLoadk, 1, 0, 0, TermInt(0))
		a.Emit(OpNe, 0, 0, 1)
		a.Emit(OpRet, 0, 0, 0)
		g.finishGenerated(info, &a, 1, 2)
		return idx
	}
	yes := a.NewLabel()
	for _, ct := range g.concreteTypes() {
		if !types.Implements(ct, iface) {
			continue
		}
		if id, ok := g.typeID(ct); ok {
			a.EmitExt(OpTypeis, 1, 0, 0, id)
			a.EmitJump(OpJmpt, 1, yes)
		}
	}
	a.EmitExt(OpLoadk, 1, 0, 0, TermFalse)
	a.Emit(OpRet, 1, 0, 0)
	a.Bind(yes)
	a.EmitExt(OpLoadk, 1, 0, 0, TermTrue)
	a.Emit(OpRet, 1, 0, 0)
	g.finishGenerated(info, &a, 1, 2)
	return idx
}

// isHelper is a generated implements test, by interface type.
type isHelper struct {
	t   types.Type
	idx uint32
}

// finishGenerated assembles a generated function.
func (g *gen) finishGenerated(info *funcInfo, a *Asm, nParams, nRegs int) {
	code, err := a.Finish()
	if err != nil {
		panic(genError{err})
	}
	info.code, info.nParams, info.nRegs = code, uint16(nParams), uint16(nRegs)
}

// ifaceTest emits t = (the interface value in register x implements
// interface type it): a call of its implements test.
func (f *fn) ifaceTest(n ast.Node, it types.Type, x, t uint8) {
	f.asm.Emit(OpMove, t, x, 0)
	f.asm.EmitExt(OpCall, t, t, 1, f.g.implements(it))
}

// methodIndex returns the function index that x.M (sel) calls: the
// method's own, or for an interface value its dispatcher.
func (f *fn) methodIndex(sel *ast.SelectorExpr, m *types.Func) uint32 {
	if f.g.p.ifaceMethod(sel) {
		return f.g.dispatcher(m)
	}
	return f.methodIdx(sel, m)
}

// panicValueInto leaves in register v the value panic prints for the
// value in v, of static type t: an error's Error() (as Go prints it);
// for an interface value that may hold an error, decided at run time.
func (f *fn) panicValue(n ast.Node, t types.Type, v uint8) {
	errIface := errorType.Underlying().(*types.Interface)
	switch {
	case isIface(t) && types.Implements(t, errIface):
		f.asm.EmitExt(OpCall, v, v, 1, f.g.dispatcher(errIface.Method(0)))
	case isIface(t):
		mark := f.next
		ok := f.allocScalar(n)
		f.ifaceTest(n, errorType, v, ok)
		skip := f.asm.NewLabel()
		f.jump(OpJmpf, ok, skip)
		f.asm.EmitExt(OpCall, v, v, 1, f.g.dispatcher(errIface.Method(0)))
		f.bind(skip)
		f.next = mark
	case types.Implements(t, errIface): // as an error, through the dispatcher
		f.toIface(n, t, v)
		f.asm.EmitExt(OpCall, v, v, 1, f.g.dispatcher(errIface.Method(0)))
	}
}
