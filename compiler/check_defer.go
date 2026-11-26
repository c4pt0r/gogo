package compiler

import (
	"go/ast"
	"go/types"
)

// deferStmt checks `defer f(args)`. Deferred calls are static: the
// compiler inlines them at every return (codegen_defer.go), so a defer
// must run at most once per call of its function: not inside a loop (a
// func literal is a function of its own). The call is checked like a call
// statement. Its arguments are evaluated at the defer statement, so an
// argument that is a call with several results, which would need one
// register per result, is rejected.
func (c *checker) deferStmt(s *ast.DeferStmt) {
	if c.loops > 0 {
		c.fail(s.Pos(), "defer inside a loop not supported: deferred calls are static (move the loop body into a function)")
		return
	}
	call := s.Call
	if len(call.Args) == 1 && c.multiCall(call.Args[0]) != nil {
		c.unsupported(call.Args[0], "deferred call with a multi-value argument")
		return
	}
	c.call(call, c.multiCall(call) != nil) // results discarded
}

// namedResults returns the named results of sig and of every func literal
// in body.
func namedResults(info *types.Info, body *ast.BlockStmt, sig *types.Signature) map[*types.Var]bool {
	out := map[*types.Var]bool{}
	add := func(sig *types.Signature) {
		if sig == nil {
			return
		}
		for i := 0; i < sig.Results().Len(); i++ {
			if v := sig.Results().At(i); v.Name() != "" {
				out[v] = true
			}
		}
	}
	add(sig)
	ast.Inspect(body, func(n ast.Node) bool {
		if lit, ok := n.(*ast.FuncLit); ok {
			s, _ := info.TypeOf(lit).(*types.Signature)
			add(s)
		}
		return true
	})
	return out
}
