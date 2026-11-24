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
// included: main.main.func1), N counting from 1 in source order within
// each top-level function (nested literals included).
func (g *gen) nameLits(decls []*ast.FuncDecl) {
	g.litNames = map[*ast.FuncLit]string{}
	for _, fd := range decls {
		pkg := g.p.Info.Defs[fd.Name].Pkg().Name()
		n := 0
		ast.Inspect(fd.Body, func(node ast.Node) bool {
			if lit, ok := node.(*ast.FuncLit); ok {
				n++
				g.litNames[lit] = fmt.Sprintf("%s.%s.func%d", pkg, fd.Name.Name, n)
			}
			return true
		})
	}
}

// actorBuiltins maps each gogo/actor function to its BUILTIN id. The
// arguments go into consecutive registers; the two-result builtins
// (ReceiveTimeout, Call, Whereis) write dst and dst+1, so they are only
// compiled by assign, which passes two fresh temporaries.
var actorBuiltins = map[string]uint32{
	"Spawn": BSpawn, "SpawnLink": BSpawnLink, "SpawnOpt": BSpawnOpt,
	"Self": BSelf, "Send": BSend, "Receive": BReceive,
	"Link": BLink, "Unlink": BUnlink, "TrapExit": BTrapExit, "Exit": BExit,
	"Kill": BKill, "Monitor": BMonitor, "Demonitor": BDemonitor,
	"MakeRef": BMakeRef, "ReceiveTimeout": BReceiveTimeout,
	"SendAfter": BSendAfter, "CancelTimer": BCancelTimer, "Sleep": BSleep,
	"Now": BNow, "Call": BCall, "Reply": BReply, "Register": BRegister,
	"Whereis": BWhereis, "Unregister": BUnregister,
}

// actorCall compiles a call of gogo/actor function name into dst.
func (f *fn) actorCall(call *ast.CallExpr, name string, dst uint8) {
	switch name {
	case "Info": // ProcInfo{Memory: MEMORY(p)}
		st := structType(f.g.p.Info.TypeOf(call))
		if st == nil {
			f.fail(call, "internal error: actor.Info result is not a struct")
		}
		idx, ok := f.g.typeIdx[st.Obj()]
		if !ok {
			f.fail(call, "internal error: ProcInfo has no type index")
		}
		base, n := f.args(call)
		f.asm.EmitExt(OpBuiltin, base, base, n, BMemory)
		f.asm.EmitExt(OpNewstruct, dst, base, 1, idx)
	default:
		id, ok := actorBuiltins[name]
		if !ok {
			f.fail(call, "internal error: unexpected actor.%s", name)
		}
		if twoResult[name] {
			f.fail(call, "internal error: actor.%s outside a two-value assignment", name)
		}
		base, n := f.args(call)
		f.asm.EmitExt(OpBuiltin, dst, base, n, id)
	}
}

// twoResultCall compiles a two-result builtin call (ReceiveTimeout, Call,
// Whereis) whose results go to dst and dst+1, two fresh temporaries.
func (f *fn) twoResultCall(call *ast.CallExpr, dst uint8) {
	sel, ok := ast.Unparen(call.Fun).(*ast.SelectorExpr)
	var name string
	if ok {
		if obj, ok := f.g.p.Info.Uses[sel.Sel].(*types.Func); ok && obj.Pkg() != nil && obj.Pkg().Path() == ActorPath {
			name = obj.Name()
		}
	}
	if !twoResult[name] {
		f.fail(call, "internal error: two-value assignment from something other than a two-result builtin")
	}
	mark := f.next
	base, n := f.args(call)
	f.asm.EmitExt(OpBuiltin, dst, base, n, actorBuiltins[name])
	f.next = mark
}

// closureInto compiles func literal lit into dst: the captured variables
// go into consecutive registers and MKCLOSURE packs them with the hidden
// function, whose parameters they become.
func (f *fn) closureInto(lit *ast.FuncLit, dst uint8) {
	caps := litCaptures(f.g.p.Info, lit)
	if len(caps) > maxRegs {
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

// funcValueInto compiles the top-level function fn (of type func()) used as
// a value into dst: a closure without captures.
func (f *fn) funcValueInto(n ast.Node, fn *types.Func, dst uint8) {
	idx, ok := f.g.funcIdx[fn]
	if !ok {
		f.fail(n, "internal error: unknown function %s", fn.Name())
	}
	f.asm.EmitExt(OpMkclosure, dst, 0, 0, idx)
}

// typeSwitch compiles `switch m := x.(type)` for any expression x of type
// any: x is evaluated once into one register, which m names in every
// clause; each case is a TYPEIS/JMPF test, and default runs when no case
// matched.
func (f *fn) typeSwitch(s *ast.TypeSwitchStmt) {
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
	var def *ast.CaseClause
	for _, stmt := range s.Body.List {
		cc := stmt.(*ast.CaseClause)
		if cc.List == nil {
			def = cc
			continue
		}
		bodyL, nextL := f.asm.NewLabel(), f.asm.NewLabel()
		mark := f.next
		t := f.alloc(cc)
		for i, te := range cc.List {
			f.asm.EmitExt(OpTypeis, t, m, 0, f.typeID(te))
			if i == len(cc.List)-1 {
				f.asm.EmitJump(OpJmpf, t, nextL)
			} else {
				f.asm.EmitJump(OpJmpt, t, bodyL)
			}
		}
		f.next = mark
		f.asm.Bind(bodyL)
		f.caseBody(cc, m, endL)
		f.asm.EmitJump(OpJmp, 0, endL)
		f.asm.Bind(nextL)
	}
	if def != nil {
		f.caseBody(def, m, endL)
	}
	f.asm.Bind(endL)
	f.next = scope
}

// caseBody compiles a clause with its binder (if any) on register m;
// break leaves the switch.
func (f *fn) caseBody(cc *ast.CaseClause, m uint8, endL Label) {
	if v, ok := f.g.p.Info.Implicits[cc].(*types.Var); ok {
		f.locals[v] = m
	}
	f.breakTo = append(f.breakTo, endL)
	f.block(cc.Body)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
}

// typeID is the TYPEIS operand for case type te.
func (f *fn) typeID(te ast.Expr) uint32 {
	t := f.g.p.Info.TypeOf(te)
	if isPID(t) {
		return TyPid
	}
	if isRef(t) {
		return TyRef
	}
	if b, ok := types.Unalias(t).(*types.Basic); ok {
		switch b.Kind() {
		case types.Int:
			return TyInt
		case types.Int64:
			return TyInt64
		case types.Bool:
			return TyBool
		case types.String:
			return TyString
		}
	}
	if st := structType(t); st != nil {
		if idx, ok := f.g.typeIdx[st.Obj()]; ok {
			return idx
		}
	}
	f.fail(te, "internal error: unexpected type switch case %s", types.TypeString(t, nil))
	return 0
}
