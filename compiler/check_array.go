package compiler

import (
	"go/ast"
	"go/types"
)

// arrayLhs checks x, the array of an element assignment x[i] = v: a
// variable, or a field or an element on an assignment path (p.A[i],
// a[j][i], s[j][i]).
func (c *checker) arrayLhs(x ast.Expr) {
	switch x := ast.Unparen(x).(type) {
	case *ast.Ident:
	case *ast.SelectorExpr:
		c.fieldLhs(x)
	case *ast.IndexExpr:
		c.lhs(x)
	default:
		c.unsupported(x, "element assignment to "+kindName(x))
	}
}

// arraySlice checks the operand of a[lo:hi] on an array: it must be a
// local variable, whose array the slice shares. An array anywhere else (a
// field, an element) is never written in place (see codegen_array.go), so
// a slice of it could not see later writes as Go's does.
func (c *checker) arraySlice(e *ast.SliceExpr) bool {
	if id, ok := ast.Unparen(e.X).(*ast.Ident); ok {
		if v, ok := c.p.Info.Uses[id].(*types.Var); ok && !v.IsField() {
			return true
		}
	}
	c.fail(e.Pos(), "slice of an array that is not a variable not supported: copy the array into a variable first")
	return false
}

// arrayLit checks an array literal [N]T{...} or [...]T{...}: positional
// elements or constant index keys (go/types checks the keys); element
// literals may omit their type.
func (c *checker) arrayLit(e *ast.CompositeLit, at *types.Array) {
	for _, el := range e.Elts {
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			el = kv.Value
		}
		c.convert(at.Elem(), el)
		if lit, ok := el.(*ast.CompositeLit); ok && lit.Type == nil {
			c.compositeLitIn(lit, true)
			continue
		}
		c.expr(el)
	}
}
