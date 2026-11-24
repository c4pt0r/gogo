package compiler

import (
	"fmt"
	"go/ast"
	"go/types"
)

// closure is a Spawn func literal compiled as a hidden function whose
// parameters are the variables it captures.
type closure struct {
	info     *funcInfo
	lit      *ast.FuncLit
	captures []*types.Var
}

// nameLits names every func literal main.<enclosing>.func<N>, N counting
// from 1 in source order within each top-level function (nested literals
// included).
func (g *gen) nameLits(decls []*ast.FuncDecl) {
	g.litNames = map[*ast.FuncLit]string{}
	for _, fd := range decls {
		n := 0
		ast.Inspect(fd.Body, func(node ast.Node) bool {
			if lit, ok := node.(*ast.FuncLit); ok {
				n++
				g.litNames[lit] = fmt.Sprintf("main.%s.func%d", fd.Name.Name, n)
			}
			return true
		})
	}
}

// captures lists the variables lit uses but does not declare, in order of
// first reference. They are copied by value into the spawned actor.
func (g *gen) captures(lit *ast.FuncLit) []*types.Var {
	var vars []*types.Var
	seen := map[*types.Var]bool{}
	ast.Inspect(lit.Body, func(node ast.Node) bool {
		id, ok := node.(*ast.Ident)
		if !ok {
			return true
		}
		v, ok := g.p.Info.Uses[id].(*types.Var)
		if !ok || v.IsField() || seen[v] || (v.Pos() >= lit.Pos() && v.Pos() < lit.End()) {
			return true
		}
		seen[v] = true
		vars = append(vars, v)
		return true
	})
	return vars
}

// actorCall compiles a call of gogo/actor function name into dst.
func (f *fn) actorCall(call *ast.CallExpr, name string, dst uint8) {
	switch name {
	case "Spawn":
		f.spawn(call, dst)
	case "Self":
		f.asm.EmitExt(OpBuiltin, dst, 0, 0, BSelf)
	case "Send":
		base, n := f.args(call)
		f.asm.EmitExt(OpBuiltin, dst, base, n, BSend)
	case "Receive":
		f.asm.EmitExt(OpBuiltin, dst, 0, 0, BReceive)
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
		f.fail(call, "internal error: unexpected actor.%s", name)
	}
}

// spawn compiles actor.Spawn(func(){...}): the captured variables go into
// consecutive registers, MKCLOSURE packs them with the hidden function, and
// BUILTIN SPAWN starts it.
func (f *fn) spawn(call *ast.CallExpr, dst uint8) {
	lit, ok := ast.Unparen(call.Args[0]).(*ast.FuncLit)
	if !ok {
		f.fail(call, "internal error: actor.Spawn of something other than a func literal")
	}
	caps := f.g.captures(lit)
	if len(caps) > maxRegs {
		f.fail(lit, "closure captures too many variables")
	}
	info := &funcInfo{name: f.g.litNames[lit]}
	idx := uint32(len(f.g.funcs))
	f.g.funcs = append(f.g.funcs, info)
	f.g.pending = append(f.g.pending, closure{info, lit, caps})

	base := f.allocN(lit, len(caps))
	for i, v := range caps {
		r, ok := f.locals[v]
		if !ok {
			f.fail(lit, "internal error: captured variable %s has no register", v.Name())
		}
		f.asm.Emit(OpMove, base+uint8(i), r, 0)
	}
	f.asm.EmitExt(OpMkclosure, dst, base, uint8(len(caps)), idx)
	f.asm.EmitExt(OpBuiltin, dst, dst, 1, BSpawn)
}

// typeSwitch compiles `switch m := actor.Receive().(type)`: the value goes
// into one register, which m names in every clause; each case is a
// TYPEIS/JMPF test, and default runs when no case matched.
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
	if st := structType(t); st != nil {
		if idx, ok := f.g.typeIdx[st.Obj()]; ok {
			return idx
		}
	}
	f.fail(te, "internal error: unexpected type switch case %s", types.TypeString(t, nil))
	return 0
}
