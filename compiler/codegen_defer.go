package compiler

import (
	"go/ast"
	"go/types"
)

// Static defer. The checker allows defer only outside loops, so each defer
// statement runs at most once per call and the deferred calls of a
// function are known at compile time: no run-time defer stack and no
// opcode. At the defer statement the function value, the receiver and the
// arguments are evaluated, as Go does, into registers reserved for that
// defer for the whole function; each return (and the end of a function
// without results) then compiles the deferred calls itself, last deferred
// first, reading those registers instead of the expressions (presets).
//
// A return knows statically which defers ran before it: one later in the
// source has not run (only a loop could jump back over it), one in an
// enclosing block has. A defer in a nested block (an if or a switch
// clause) that the return is outside of may or may not have run, so it
// has a flag register, false at entry and set at the defer statement, that
// the return tests.
//
// Deferred calls run on normal returns only. A crash or actor.Exit ends
// the actor without running them (ports and files close as the actor
// ends). With named results, a return with values sets the results before
// the deferred calls run, as in Go; a deferred call cannot change them
// (a func literal captures by value, and the checker rejects writes to
// captured variables).

// deferInfo is one defer statement of the function being compiled.
type deferInfo struct {
	stmt    *ast.DeferStmt
	exprs   []ast.Expr         // the expressions evaluated at the defer statement
	regs    map[ast.Expr]uint8 // their registers
	flag    int                // the "has run" flag register, -1 for a defer at the top level
	scope   int                // the block that holds the defer statement
	reached bool               // compiled: returns after it may run it
}

// deferState is the defer bookkeeping of one function.
type deferState struct {
	list    []*deferInfo
	byStmt  map[*ast.DeferStmt]*deferInfo
	scopes  []int // the blocks being compiled, innermost last
	nextID  int
	presets map[ast.Expr]uint8 // while compiling a deferred call
}

// deferredExprs lists what a defer statement evaluates: the func value (a
// func value call, a func literal included), the receiver of a method or
// a gogo/bytes or gogo/fs method call, and every argument that is not a
// constant.
func (g *gen) deferredExprs(call *ast.CallExpr) []ast.Expr {
	var out []ast.Expr
	if funcValueCall(g.p.Info, call) {
		out = append(out, call.Fun)
	} else if sel, ok := ast.Unparen(call.Fun).(*ast.SelectorExpr); ok {
		if s := g.p.Info.Selections[sel]; s != nil && s.Kind() == types.MethodVal {
			out = append(out, sel.X)
		}
	}
	for _, a := range call.Args {
		if g.p.Info.Types[a].Value == nil {
			out = append(out, a)
		}
	}
	return out
}

// setupDefers reserves the registers of the defer statements of body (not
// those of func literals in it, which are functions of their own) and
// clears their flags. Nothing for a function without defer.
func (f *fn) setupDefers(body *ast.BlockStmt) {
	top := map[ast.Stmt]bool{}
	for _, s := range body.List {
		top[s] = true
	}
	var list []*deferInfo
	ast.Inspect(body, func(n ast.Node) bool {
		switch n := n.(type) {
		case *ast.FuncLit:
			return false
		case *ast.DeferStmt:
			list = append(list, &deferInfo{stmt: n, flag: -1, exprs: f.g.deferredExprs(n.Call)})
		}
		return true
	})
	if len(list) == 0 {
		return
	}
	d := &deferState{list: list, byStmt: map[*ast.DeferStmt]*deferInfo{}, scopes: []int{0}, nextID: 1}
	for _, di := range list {
		d.byStmt[di.stmt] = di
		di.regs = map[ast.Expr]uint8{}
		for _, e := range di.exprs {
			di.regs[e] = f.alloc(e)
		}
		if !top[di.stmt] {
			di.flag = int(f.allocScalar(di.stmt))
			f.asm.EmitExt(OpLoadk, uint8(di.flag), 0, 0, TermFalse)
		}
	}
	f.defers = d
}

// scopeIn and scopeOut bracket a block: a defer in it has run at the
// returns inside it that follow it.
func (f *fn) scopeIn() {
	if d := f.defers; d != nil {
		d.scopes = append(d.scopes, d.nextID)
		d.nextID++
	}
}

func (f *fn) scopeOut() {
	if d := f.defers; d != nil {
		d.scopes = d.scopes[:len(d.scopes)-1]
	}
}

// deferStmt compiles a defer statement: its function value, receiver and
// arguments go into its registers (an array argument is copied, as Go
// evaluates it here), and its flag is set.
func (f *fn) deferStmt(s *ast.DeferStmt) {
	di := f.defers.byStmt[s]
	for _, e := range di.exprs {
		r := di.regs[e]
		f.exprInto(e, r)
		if t := f.g.p.Info.TypeOf(e); isArray(t) && !freshArray(e) {
			f.cloneArray(e, arrayType(t), r, r)
		}
	}
	if di.flag >= 0 {
		f.asm.EmitExt(OpLoadk, uint8(di.flag), 0, 0, TermTrue)
	}
	di.scope = f.defers.scopes[len(f.defers.scopes)-1]
	di.reached = true
}

// runDefers compiles the deferred calls due at a return here, last
// deferred first: each defer already compiled, unconditionally if its
// block encloses this point, else if its flag is set. Their results are
// discarded. Nothing in a function without defer.
func (f *fn) runDefers() {
	d := f.defers
	if d == nil {
		return
	}
	open := map[int]bool{}
	for _, id := range d.scopes {
		open[id] = true
	}
	for i := len(d.list) - 1; i >= 0; i-- {
		di := d.list[i]
		if !di.reached {
			continue
		}
		var skip Label
		cond := !open[di.scope]
		if cond {
			if di.flag < 0 {
				f.fail(di.stmt, "internal error: defer without a flag")
			}
			skip = f.asm.NewLabel()
			f.jump(OpJmpf, uint8(di.flag), skip)
		}
		d.presets = di.regs
		call := di.stmt.Call
		mark := f.next
		if n := tupleLen(f.g.p.Info.TypeOf(call)); n > 1 {
			f.multiCallInto(call, f.allocN(call, n), nil)
		} else {
			f.exprInto(call, f.allocFor(call))
		}
		f.next = mark
		d.presets = nil
		if cond {
			f.bind(skip)
		}
	}
}

// preset returns the register e was evaluated into at its defer statement,
// while the deferred call that contains it is compiled.
func (f *fn) preset(e ast.Expr) (uint8, bool) {
	if f.defers == nil || f.defers.presets == nil {
		return 0, false
	}
	r, ok := f.defers.presets[e]
	return r, ok
}

// isPreset reports whether e is preset (see preset).
func (f *fn) isPreset(e ast.Expr) bool {
	_, ok := f.preset(e)
	return ok
}

// retToNamed compiles return with values in a function with named results
// and defers, as Go does: the values are evaluated, then stored into the
// named results (an array result by copying its elements into the
// result's own array, which a slice may share), then the deferred calls
// run, then the named results are returned.
func (f *fn) retToNamed(s *ast.ReturnStmt) {
	k := len(f.results)
	mark := f.next
	base := f.allocN(s, k)
	if len(s.Results) == 1 && k > 1 {
		f.multiCallInto(s.Results[0], base, f.results)
	} else {
		for i, res := range s.Results {
			f.valueInto(res, f.results[i], base+uint8(i))
		}
	}
	for i, t := range f.results {
		r := uint8(f.named + i)
		if isArray(t) {
			f.copyElems(s, r, base+uint8(i))
		} else {
			f.asm.Emit(OpMove, r, base+uint8(i), 0)
		}
	}
	f.runDefers()
	f.retNamed(s, k)
	f.next = mark
}
