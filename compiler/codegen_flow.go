package compiler

import (
	"go/ast"
	"go/token"
)

// Control flow: expression switches.

// switchStmt compiles an expression switch. The tag (if any) is evaluated
// once; then each case value, in source order, is compared with it (EQ, or
// EQ64 for int64; EQ compares strings by content), or for a tagless switch
// evaluated as a condition, and a true result jumps to its clause's body. When no case matches, the default
// body runs, or the switch ends. The bodies follow in source order, so a
// fallthrough at the end of one simply runs into the next.
func (f *fn) switchStmt(s *ast.SwitchStmt) {
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
	f.bindEnd(endL)
	f.next = scope
}
