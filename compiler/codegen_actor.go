package compiler

import (
	"fmt"
	"go/ast"
	"go/types"
)

// closure is a func literal compiled as a hidden function whose parameters
// are the variables it captures.
type closure struct {
	info     *funcInfo
	lit      *ast.FuncLit
	captures []*types.Var
}

// nameLits names every func literal <pkg>.<enclosing>.func<N> (main
// included: main.main.func1; <pkg> is pkgPrefix), N counting from 1 in source order within
// each top-level function (nested literals included).
func (g *gen) nameLits(decls []*ast.FuncDecl) {
	g.litNames = map[*ast.FuncLit]string{}
	for _, fd := range decls {
		pkg := pkgPrefix(g.p.Info.Defs[fd.Name].Pkg())
		n := 0
		ast.Inspect(fd.Body, func(node ast.Node) bool {
			if lit, ok := node.(*ast.FuncLit); ok {
				n++
				g.litNames[lit] = fmt.Sprintf("%s.%s.func%d", pkg, funcName(g.p.Info.Defs[fd.Name].(*types.Func)), n)
			}
			return true
		})
	}
}

// actorBuiltins maps each builtin function (by stubName: gogo/actor and
// gogo/port) to its BUILTIN id. The arguments go into consecutive
// registers; the two-result builtins (ReceiveTimeout, Call, Whereis,
// port.Open, sys.Config) write dst and dst+1, so they are only compiled by assign,
// which passes two fresh temporaries.
var actorBuiltins = map[string]uint32{
	"Spawn": BSpawn, "SpawnLink": BSpawnLink, "SpawnOpt": BSpawnOpt,
	"Self": BSelf, "Send": BSend, "Receive": BReceive,
	"Link": BLink, "Unlink": BUnlink, "TrapExit": BTrapExit, "Exit": BExit,
	"Kill": BKill, "Monitor": BMonitor, "Demonitor": BDemonitor,
	"MakeRef": BMakeRef, "ReceiveTimeout": BReceiveTimeout,
	"SendAfter": BSendAfter, "CancelTimer": BCancelTimer, "Sleep": BSleep,
	"Now": BNow, "Call": BCall, "Reply": BReply, "Register": BRegister,
	"Whereis": BWhereis, "Unregister": BUnregister,
	"port.open": BPortOpen, "port.Close": BPortClose, "port.Handoff": BPortHandoff,
	"net.Subscribe": BNetSubscribe, "sys.Config": BConfigGet,
	"sys.Time": BUnixTime, "sys.SetTime": BSetUnixTime, "sys.Random": BRandom,
	"bytes.NewBuffer": BBufNew, "bytes.FromString": BBytesFrom, "bytes.FromSlice": BBytesFrom,
}

// bytesMethods maps each gogo/bytes method (by bytesMethod) to its
// BUILTIN id; the receiver is its first argument.
var bytesMethods = map[string]uint32{
	"Buffer.WriteString": BBufWrite, "Buffer.Write": BBufWrite, "Buffer.WriteByte": BBufWrite,
	"Buffer.WriteBytes": BBufWrite, "Buffer.Len": BLen, "Buffer.Freeze": BBufFreeze,
	"Bytes.Len": BLen, "Bytes.At": BIndex, "Bytes.Slice": BSlice, "Bytes.String": BStrFrom,
	"Bytes.Equal": BBytesEqual, "Bytes.CopyTo": BBytesCopyto,
}

// bytesMethodCall compiles x.M(args) of a gogo/bytes method (name as
// bytesMethod gives it) into dst: BUILTIN with x and the arguments in
// consecutive registers.
func (f *fn) bytesMethodCall(call *ast.CallExpr, sel *ast.SelectorExpr, name string, dst uint8) {
	id, ok := bytesMethods[name]
	if !ok {
		f.fail(call, "internal error: unexpected method %s", name)
	}
	mark := f.next
	recv := f.alloc(sel.X)
	f.exprInto(sel.X, recv)
	_, n := f.args(call) // right above recv
	f.asm.EmitExt(OpBuiltin, dst, recv, n+1, id)
	f.next = mark
}

// actorCall compiles a call of the builtin function called name (stubName)
// into dst.
func (f *fn) actorCall(call *ast.CallExpr, name string, dst uint8) {
	switch name {
	case "Info": // ProcInfo{Memory: MEMORY(p)}
		st := structType(f.g.p.Info.TypeOf(call))
		if st == nil {
			f.fail(call, "internal error: actor.Info result is not a struct")
		}
		idx, ok := f.g.typeIndex(st)
		if !ok {
			f.fail(call, "internal error: ProcInfo has no type index")
		}
		base, n := f.args(call)
		f.asm.EmitExt(OpBuiltin, base, base, n, BMemory)
		f.asm.EmitExt(OpNewstruct, dst, base, 1, idx)
	case "sys.Args":
		f.sysArgs(call, dst)
	default:
		id, ok := actorBuiltins[name]
		if !ok {
			f.fail(call, "internal error: unexpected builtin function %s", name)
		}
		if twoResult[name] {
			f.fail(call, "internal error: %s outside a two-value assignment", name)
		}
		base, n := f.args(call)
		f.asm.EmitExt(OpBuiltin, dst, base, n, id)
	}
}

// twoResultCall compiles a two-result builtin call (ReceiveTimeout, Call,
// Whereis, port.Open, sys.Config and the gogo/fs ones) whose results go to dst and dst+1, two fresh
// temporaries.
func (f *fn) twoResultCall(call *ast.CallExpr, dst uint8) {
	name := f.stubOf(call)
	if !twoResult[name] {
		f.fail(call, "internal error: two-value assignment from something other than a two-result builtin")
	}
	if _, isFs := fsOps[name]; isFs {
		f.fsCall(call, name, dst)
		return
	}
	if _, isCode := codeOps[name]; isCode {
		f.codeCall(call, name, dst)
		return
	}
	if _, isFloat := floatOps[name]; isFloat {
		f.floatCall(call, name, dst)
		return
	}
	mark := f.next
	base, n := f.args(call)
	f.asm.EmitExt(OpBuiltin, dst, base, n, actorBuiltins[name])
	f.next = mark
}

// closureInto compiles func literal lit into dst: the captured variables
// go into consecutive registers and MKCLOSURE packs them with the hidden
// function, whose leading parameters they become (its own parameters
// follow; see OP_CALLV).
func (f *fn) closureInto(lit *ast.FuncLit, dst uint8) {
	caps := litCaptures(f.g.p.Info, lit)
	if len(caps)+len(f.g.declParams(lit.Type)) > maxRegs {
		f.fail(lit, "closure captures too many variables")
	}
	info := &funcInfo{name: f.g.litNames[lit]}
	idx := uint32(len(f.g.funcs))
	f.g.funcs = append(f.g.funcs, info)
	f.g.pending = append(f.g.pending, closure{info, lit, caps})

	mark := f.next
	base := f.allocN(lit, len(caps))
	for i, v := range caps {
		r, ok := f.locals[v]
		if !ok {
			f.fail(lit, "internal error: captured variable %s has no register", v.Name())
		}
		f.asm.Emit(OpMove, base+uint8(i), r, 0)
	}
	f.asm.EmitExt(OpMkclosure, dst, base, uint8(len(caps)), idx)
	f.next = mark
}

// funcValueInto compiles the top-level function fn (of any signature) used
// as a value into dst: a closure without captures.
func (f *fn) funcValueInto(n ast.Node, fn *types.Func, dst uint8) {
	idx, ok := f.g.funcIndex(fn)
	if !ok {
		f.fail(n, "internal error: unknown function %s", fn.Name())
	}
	f.asm.EmitExt(OpMkclosure, dst, 0, 0, idx)
}

// methodIdx returns the function index of method m (declared in a
// compiled package, so indexed by Compile).
func (f *fn) methodIdx(n ast.Node, m *types.Func) uint32 {
	idx, ok := f.g.funcIndex(m)
	if !ok {
		f.fail(n, "internal error: unknown method %s", funcName(m))
	}
	return idx
}

// methodCall compiles a call of method m into dst. A method is a function
// whose parameter 0 is the receiver: x.M(args) passes x in the register
// right below the arguments (a variadic method's slice is built above
// it, as for a function), and T.M(x, args), a method expression, is a
// plain call of that function.
func (f *fn) methodCall(call *ast.CallExpr, sel *ast.SelectorExpr, m *types.Func, dst uint8) {
	idx := f.methodIndex(sel, m)
	mark := f.next
	defer func() { f.next = mark }()
	if f.g.p.Info.Selections[sel].Kind() == types.MethodExpr {
		base, n := f.args(call)
		f.asm.EmitExt(OpCall, dst, base, n, idx)
		return
	}
	recv := f.receiver(sel)
	base, n := f.args(call) // right above recv
	if n > 0 && base != recv+1 {
		f.fail(call, "internal error: call arguments not above the receiver")
	}
	if int(n)+1 > maxRegs {
		f.fail(call, "too many arguments")
	}
	f.asm.EmitExt(OpCall, dst, recv, n+1, idx)
}

// receiver evaluates the receiver of x.M into a fresh register (for a
// method promoted from an embedded field, that field of x). A value
// receiver gets x's value: structs are immutable, so the method cannot
// change the caller's copy.
func (f *fn) receiver(sel *ast.SelectorExpr) uint8 {
	s := f.g.p.Info.Selections[sel]
	if s.Indirect() {
		f.fail(sel, "internal error: indirect method")
	}
	recv := f.allocFor(sel.X)
	f.exprInto(sel.X, recv)
	if idx := s.Index(); !isIface(s.Recv()) { // an interface's method set is flat: no field path
		f.fieldPathInto(sel, idx[:len(idx)-1], recv, recv) // a promoted method: its embedded receiver
	}
	return recv
}

// methodValueInto compiles x.M, a method value, into dst: a closure of
// method m capturing x, which becomes the method's parameter 0 when the
// closure is called (see OP_CALLV). T.M, a method expression, is the
// method's function itself: a closure without captures.
func (f *fn) methodValueInto(sel *ast.SelectorExpr, m *types.Func, dst uint8) {
	idx := f.methodIndex(sel, m)
	if f.g.p.Info.Selections[sel].Kind() == types.MethodExpr {
		f.asm.EmitExt(OpMkclosure, dst, 0, 0, idx)
		return
	}
	mark := f.next
	recv := f.receiver(sel)
	f.asm.EmitExt(OpMkclosure, dst, recv, 1, idx)
	f.next = mark
}

// typeSwitch compiles `switch m := x.(type)` for any expression x of type
// any: x is evaluated once into one register, which m names in every
// clause; each case is a TYPEIS/JMPF test, and default runs when no case
// matched.
func (f *fn) typeSwitch(s *ast.TypeSwitchStmt) {
	label := f.takeLabel()
	var x ast.Expr
	switch a := s.Assign.(type) {
	case *ast.ExprStmt:
		x = a.X.(*ast.TypeAssertExpr).X
	case *ast.AssignStmt:
		x = a.Rhs[0].(*ast.TypeAssertExpr).X
	}
	scope := f.next
	m := f.alloc(s)
	f.exprInto(x, m)

	endL := f.asm.NewLabel()
	f.setLabel(label, endL)
	defer f.dropLabel(label)
	var def *ast.CaseClause
	for _, stmt := range s.Body.List {
		cc := stmt.(*ast.CaseClause)
		if cc.List == nil {
			def = cc
			continue
		}
		bodyL, nextL := f.asm.NewLabel(), f.asm.NewLabel()
		mark := f.next
		t := f.allocScalar(cc) // TYPEIS results are bools
		for i, te := range cc.List {
			if f.g.p.Info.Types[te].IsNil() { // case nil: m == nil
				r := f.allocN(te, 2)
				f.asm.Emit(OpMove, r, m, 0)
				f.asm.EmitExt(OpLoadk, r+1, 0, 0, TermNil)
				f.cmpRegs(te, r, OpEq, t)
			} else if tt := f.g.p.Info.TypeOf(te); isIface(tt) {
				f.ifaceTest(te, tt, m, t)
			} else {
				f.typeTest(te, m, t)
			}
			if i == len(cc.List)-1 {
				f.jump(OpJmpf, t, nextL)
			} else {
				f.jump(OpJmpt, t, bodyL)
			}
		}
		f.next = mark
		f.bind(bodyL)
		f.caseBody(cc, m, endL)
		f.jump(OpJmp, 0, endL)
		f.bind(nextL)
	}
	if def != nil {
		f.caseBody(def, m, endL)
	}
	f.bindEnd(endL)
	f.next = scope
}

// caseBody compiles a clause with its binder (if any) on register m;
// break leaves the switch. In a clause of one concrete type the binder has
// that type: m is unboxed in place (no later clause tests it).
func (f *fn) caseBody(cc *ast.CaseClause, m uint8, endL Label) {
	if v, ok := f.g.p.Info.Implicits[cc].(*types.Var); ok {
		f.locals[v] = m
		if len(cc.List) == 1 && !isIface(v.Type()) && f.uses(cc.Body, v) {
			f.unbox(cc, v.Type(), m)
		}
	}
	f.breakTo = append(f.breakTo, endL)
	f.block(cc.Body)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
}

// uses reports whether the statements body refer to variable v.
func (f *fn) uses(body []ast.Stmt, v *types.Var) bool {
	found := false
	for _, s := range body {
		ast.Inspect(s, func(n ast.Node) bool {
			if id, ok := n.(*ast.Ident); ok && f.g.p.Info.Uses[id] == v {
				found = true
			}
			return !found
		})
	}
	return found
}

// ArgsKey is the configuration key under which a host serves the program's
// arguments (sys.Args): each argument followed by a NUL byte, so empty
// arguments survive and no arguments is an unset key. A NUL cannot be in
// an argument, and the key cannot be set by `gogo config`.
const ArgsKey = "\x00args"

// sysArgs compiles sys.Args() inline, with no builtin of its own (so no
// VM code and no image minor): it reads ArgsKey with CONFIG_GET and splits
// the value at the NULs into a new []string, as
//
//	s, _ := sys.Config(ArgsKey)
//	a := []string{}
//	for i, start := 0, 0; i < len(s); i++ {
//		if s[i] == 0 { a = append(a, s[start:i]); start = i + 1 }
//	}
func (f *fn) sysArgs(call *ast.CallExpr, dst uint8) {
	mark := f.next
	s := f.allocN(call, 2) // s, ok
	f.seqCall(call, s, BConfigGet, func(r uint8) {
		f.asm.EmitExt(OpLoadk, r, 0, 0, TermConst(f.g.b.AddString(ArgsKey)))
	})
	out := f.alloc(call)
	f.seqCall(call, out, BMakeSlice, f.loadInt(0), f.loadInt(0), f.loadInt(0),
		func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermEmptyStr) })
	n, i, start, one, zero, t := f.allocScalar(call), f.allocScalar(call), f.allocScalar(call),
		f.allocScalar(call), f.allocScalar(call), f.allocScalar(call)
	f.seqCall(call, n, BLen, f.moveFrom(s))
	f.asm.EmitExt(OpLoadk, i, 0, 0, TermInt(0))
	f.asm.EmitExt(OpLoadk, start, 0, 0, TermInt(0))
	f.asm.EmitExt(OpLoadk, one, 0, 0, TermInt(1))
	f.asm.EmitExt(OpLoadk, zero, 0, 0, TermInt(0))
	loop, next, end := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(loop)
	f.asm.Emit(OpLt, t, i, n)
	f.jump(OpJmpf, t, end)
	f.seqCall(call, t, BIndex, f.moveFrom(s), f.moveFrom(i))
	f.asm.Emit(OpEq, t, t, zero)
	f.jump(OpJmpf, t, next)
	sub := f.alloc(call)
	f.seqCall(call, sub, BSlice, f.moveFrom(s), f.moveFrom(start), f.moveFrom(i))
	f.seqCall(call, out, BAppend, f.moveFrom(out), f.loadInt(AppendTerm),
		func(r uint8) { f.asm.EmitExt(OpLoadk, r, 0, 0, TermEmptyStr) }, f.moveFrom(sub))
	f.asm.Emit(OpAdd, start, i, one)
	f.bind(next)
	f.asm.Emit(OpAdd, i, i, one)
	f.jump(OpJmp, 0, loop)
	f.bindEnd(end)
	f.asm.Emit(OpMove, dst, out, 0)
	f.next = mark
}
