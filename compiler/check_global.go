package compiler

import (
	"go/ast"
	"go/token"
	"go/types"
)

// Package-level variables (15.6): read-only and statically initialized
// (codegen_global.go).

// globalDecl checks a package-level var declaration: a type of the
// subset, and initializers the compiler can evaluate (staticProblem).
func (c *checker) globalDecl(gd *ast.GenDecl) {
	for _, spec := range gd.Specs {
		s := spec.(*ast.ValueSpec)
		if len(s.Values) == 1 && len(s.Names) > 1 {
			c.unsupported(s.Values[0], "multiple values") // var a, b = f()
			continue
		}
		for i, n := range s.Names {
			v, _ := c.p.Info.Defs[n].(*types.Var)
			if v == nil {
				continue
			}
			if msg := c.typeProblem(v.Type()); msg != "" {
				c.unsupported(n, msg)
				continue
			}
			if isBuffer(v.Type()) {
				c.fail(n.Pos(), "package-level variable of type bytes.Buffer not supported: it is read-only")
				continue
			}
			if i < len(s.Values) {
				val := s.Values[i]
				c.convert(v.Type(), val)
				if why := c.staticProblem(val, 0); why != "" {
					c.fail(val.Pos(), "package-level variable %s: %s (package-level variables are read-only and set when the program is built)", n.Name, why)
				}
			}
		}
	}
}

// staticProblem says why the compiler cannot evaluate e when it builds the
// program, or "": constants, nil, package-level variables, composite
// literals (no map yet), conversions and calls of functions whose body is
// a single return of one value, whose arguments are static too (depth:
// such calls being evaluated, at most 8 deep).
func (c *checker) staticProblem(e ast.Expr, depth int) string {
	info := c.p.Info
	tv := info.Types[e]
	if tv.Value != nil || tv.IsNil() {
		return ""
	}
	switch x := ast.Unparen(e).(type) {
	case *ast.Ident:
		if globalVar(info, x) != nil {
			return ""
		}
		if v, ok := info.Uses[x].(*types.Var); ok && depth > 0 && !v.IsField() {
			return "" // a parameter of a function being evaluated
		}
		if _, ok := info.Uses[x].(*types.Func); ok {
			return "a func value"
		}
	case *ast.SelectorExpr:
		if globalVar(info, x) != nil {
			return ""
		}
		if s := info.Selections[x]; s != nil && s.Kind() == types.FieldVal && len(s.Index()) == 1 && !isPointer(info.TypeOf(x.X)) {
			return c.staticProblem(x.X, depth)
		}
		if _, ok := info.Uses[x.Sel].(*types.Func); ok {
			return "a func value"
		}
	case *ast.CompositeLit:
		t := info.TypeOf(x)
		if isPID(t) && len(x.Elts) == 0 { // actor.PID{}: the zero PID
			return ""
		}
		if isBuffer(t) || isPID(t) || isRef(t) || isBytes(t) {
			return "a composite literal of " + types.TypeString(t, pkgName)
		}
		if mt := mapOf(t); mt != nil { // keys of a basic type, constant: sorted when built
			if b, ok := underlying(mt.Key()).(*types.Basic); !ok || b.Info()&(types.IsInteger|types.IsFloat|types.IsString|types.IsBoolean) == 0 {
				return "a map with keys of type " + types.TypeString(mt.Key(), pkgName) + " (a package-level map's keys are constants of an integer, float, bool or string type)"
			}
			for _, el := range x.Elts {
				kv := el.(*ast.KeyValueExpr)
				if info.Types[kv.Key].Value == nil {
					return "a map key that is not a constant"
				}
				if why := c.staticProblem(kv.Value, depth); why != "" {
					return why
				}
			}
			return ""
		}
		if !isArray(t) && !isSlice(t) && structType(t) == nil {
			return "a composite literal of " + types.TypeString(t, pkgName)
		}
		byteElems := isByteSlice(t) || (isArray(t) && isByte(arrayType(t).Elem()))
		for _, el := range x.Elts {
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				el = kv.Value
				if isArray(t) || isSlice(t) {
					if info.Types[kv.Key].Value == nil {
						return "an element index that is not a constant"
					}
				}
			}
			if byteElems && info.Types[el].Value == nil {
				return "a byte that is not a constant"
			}
			if why := c.staticProblem(el, depth); why != "" {
				return why
			}
		}
		return ""
	case *ast.UnaryExpr:
		if x.Op == token.AND {
			return "a pointer (its target would be read-only)"
		}
	case *ast.CallExpr:
		if tv := info.Types[x.Fun]; tv.IsType() && len(x.Args) == 1 {
			if isSlice(tv.Type) && info.Types[x.Args[0]].Value == nil {
				return "a conversion that is not of a constant"
			}
			return c.staticProblem(x.Args[0], depth)
		}
		if depth >= 8 {
			return "calls nested too deeply"
		}
		if fn, decl := (&gen{p: c.p}).staticFunc(x); fn != nil {
			for _, a := range x.Args {
				if why := c.staticProblem(a, depth); why != "" {
					return why
				}
			}
			return c.staticProblem(decl.Body.List[0].(*ast.ReturnStmt).Results[0], depth+1)
		}
		if id, ok := ast.Unparen(x.Fun).(*ast.Ident); ok {
			if _, ok := info.Uses[id].(*types.Builtin); ok {
				return "a call of " + id.Name
			}
		}
		return "a call of a function whose body is not a single return statement"
	}
	return types.ExprString(e) + " is not static"
}

// globalWrite rejects (and reports true for) a write to a package-level
// variable: the variable itself, an element of its slice, array or map,
// or a field on a path from it that does not go through a pointer.
func (c *checker) globalWrite(e ast.Expr) bool {
	x := ast.Unparen(e)
	for {
		switch y := x.(type) {
		case *ast.SelectorExpr:
			if v := globalVar(c.p.Info, y); v != nil {
				c.fail(e.Pos(), "package-level variable %s is read-only", v.Name())
				return true
			}
			if s := c.p.Info.Selections[y]; s == nil || isPointer(c.p.Info.TypeOf(y.X)) {
				return false
			}
			x = ast.Unparen(y.X)
			continue
		case *ast.IndexExpr:
			x = ast.Unparen(y.X)
			continue
		case *ast.Ident:
			if v := globalVar(c.p.Info, y); v != nil {
				c.fail(e.Pos(), "package-level variable %s is read-only", v.Name())
				return true
			}
		}
		return false
	}
}
