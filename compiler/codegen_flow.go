package compiler

import (
	"go/ast"
	"go/token"
	"go/types"
)

// Control flow: expression switches, labeled statements and range over an
// integer.

// jumpTargets are the labels a labeled statement's break and continue jump
// to; a switch has no continue target.
type jumpTargets struct {
	brk, cont Label
	loop      bool
}

// takeLabel returns the label of the statement being compiled ("" if
// none) and clears it, so the statements inside do not see it.
func (f *fn) takeLabel() string {
	l := f.label
	f.label = ""
	return l
}

// setLabel records the break target (and, for a loop, the continue
// target) of the statement labeled label; nothing if label is "".
func (f *fn) setLabel(label string, brk Label, cont ...Label) {
	if label == "" {
		return
	}
	if f.labeled == nil {
		f.labeled = map[string]jumpTargets{}
	}
	t := jumpTargets{brk: brk}
	if len(cont) > 0 {
		t.cont, t.loop = cont[0], true
	}
	f.labeled[label] = t
}

// dropLabel forgets label once its statement is compiled.
func (f *fn) dropLabel(label string) {
	if label != "" {
		delete(f.labeled, label)
	}
}

// labeledBranch compiles `break L` and `continue L`.
func (f *fn) labeledBranch(s *ast.BranchStmt) {
	t, ok := f.labeled[s.Label.Name]
	if !ok || (s.Tok == token.CONTINUE && !t.loop) {
		f.fail(s, "internal error: %s %s has no target", s.Tok, s.Label.Name)
	}
	l := t.brk
	if s.Tok == token.CONTINUE {
		l = t.cont
	}
	f.jump(OpJmp, 0, l)
}

// switchStmt compiles an expression switch. The tag (if any) is evaluated
// once; then each case value, in source order, is compared with it (EQ, or
// EQ64 for int64; EQ compares strings by content), or for a tagless switch
// evaluated as a condition, and a true result jumps to its clause's body. When no case matches, the default
// body runs, or the switch ends. The bodies follow in source order, so a
// fallthrough at the end of one simply runs into the next.
func (f *fn) switchStmt(s *ast.SwitchStmt) {
	label := f.takeLabel()
	scope := f.next
	if s.Init != nil {
		f.stmt(s.Init)
	}
	// A local tag is compared in place: case values are expressions, which
	// cannot assign it (closures cannot write their captures).
	tagMark := f.next
	var tag uint8
	if s.Tag != nil {
		tag = f.operand(s.Tag)
	}
	clauses := s.Body.List
	bodyL := make([]Label, len(clauses))
	for i := range bodyL {
		bodyL[i] = f.asm.NewLabel()
	}
	endL := f.asm.NewLabel()
	def := -1
	for i, cl := range clauses {
		cc := cl.(*ast.CaseClause)
		if cc.List == nil {
			def = i
			continue
		}
		for _, e := range cc.List {
			mark := f.next
			if s.Tag == nil {
				f.jump(OpJmpt, f.operand(e), bodyL[i])
			} else if tt, et := f.g.p.Info.TypeOf(s.Tag), f.g.p.Info.TypeOf(e); needsCmp(tt) || needsCmp(et) {
				// tag == e by B_CMP: both as values of one type (an
				// interface one's), in two fresh registers.
				ct := cmpType(tt, et)
				r := f.allocN(e, 2)
				f.asm.Emit(OpMove, r, tag, 0)
				if convertsToIface(tt, ct) {
					f.toIface(e, tt, r)
				}
				f.valueInto(e, ct, r+1)
				f.cmpRegs(e, r, OpEq, r)
				f.jump(OpJmpt, r, bodyL[i])
			} else {
				t := f.allocScalar(e) // EQ results are bools
				f.asm.Emit(f.typedOp(OpEq, s.Tag), t, tag, f.operand(e))
				f.jump(OpJmpt, t, bodyL[i])
			}
			f.next = mark
		}
	}
	if def >= 0 {
		f.jump(OpJmp, 0, bodyL[def])
	} else {
		f.jump(OpJmp, 0, endL)
	}
	f.next = tagMark // the tag is dead in the bodies

	f.breakTo = append(f.breakTo, endL)
	f.setLabel(label, endL)
	for i, cl := range clauses {
		cc := cl.(*ast.CaseClause)
		f.bind(bodyL[i])
		body := cc.Body
		fall := false
		if n := len(body); n > 0 {
			if br, ok := body[n-1].(*ast.BranchStmt); ok && br.Tok == token.FALLTHROUGH {
				body, fall = body[:n-1], true
			}
		}
		f.block(body)
		if !fall && i < len(clauses)-1 {
			f.jump(OpJmp, 0, endL)
		}
	}
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
	f.dropLabel(label)
	f.bindEnd(endL)
	f.next = scope
}

// rangeInt compiles `for i := range n` over an integer n: i runs from 0 to
// n-1 (no pass when n <= 0). n is evaluated once and the count kept in a
// hidden register, so the body may reassign n and i without changing the
// loop; i is a fresh variable, set at the top of each pass.
func (f *fn) rangeInt(s *ast.RangeStmt) {
	label := f.takeLabel()
	scope := f.next
	xt := f.g.p.Info.TypeOf(s.X)
	wide := isWide(xt)
	n := f.alloc(s)
	f.exprInto(s.X, n)
	idx := f.alloc(s)
	loadInt := func(v int32, r uint8) {
		if wide {
			f.int64Into(int64(v), r)
		} else {
			f.asm.EmitExt(OpLoadk, r, 0, 0, TermInt(v))
		}
	}
	loadInt(0, idx)
	var key uint8
	hasKey := false
	if id, _ := s.Key.(*ast.Ident); id != nil && id.Name != "_" {
		v, ok := f.g.p.Info.Defs[id].(*types.Var)
		if !ok {
			f.fail(id, "internal error: range variable %s is not a definition", id.Name)
		}
		key, hasKey = f.alloc(id), true
		f.locals[v] = key
	}

	topL, contL, endL := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(topL)
	mark := f.next
	more := f.allocScalar(s)
	f.arith(s.X, OpLt, more, idx, n) // unsigned for an unsigned n
	f.jump(OpJmpf, more, endL)
	f.next = mark
	if hasKey {
		f.asm.Emit(OpMove, key, idx, 0)
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
	loadInt(1, one)
	f.asm.Emit(f.typedOp(OpAdd, s.X), idx, idx, one) // below n: never leaves the type's range
	f.next = mark
	f.clearDead() // the body's locals die before the next iteration
	f.jump(OpJmp, 0, topL)
	f.bindEnd(endL)
	f.next = scope
}
