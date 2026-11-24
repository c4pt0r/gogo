package compiler

import (
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"sort"
	"strings"
)

// Check verifies that p (package main and the libraries it imports) stays
// inside the gogo language subset. It is an allow-list walk: any node kind
// not handled explicitly is reported as "<kind> not supported". Errors are
// sorted by position.
func Check(p *Program) (errs []error) {
	c := &checker{p: p, seen: map[string]bool{}}
	defer func() {
		if r := recover(); r != nil {
			errs = append(c.errList(), fmt.Errorf("gogo: internal checker error: %v", r))
		}
	}()
	for _, pkg := range p.packages() {
		for _, f := range pkg.Files {
			c.file(f)
		}
	}
	sort.SliceStable(c.errs, func(i, j int) bool { return c.errs[i].pos < c.errs[j].pos })
	return c.errList()
}

// file checks the top-level declarations of f: constants, types and
// functions only.
func (c *checker) file(f *ast.File) {
	for _, d := range f.Decls {
		if gd, ok := d.(*ast.GenDecl); ok && gd.Tok == token.VAR {
			c.unsupported(gd, "global variable")
			continue
		}
		c.decl(d)
	}
}

type posErr struct {
	pos token.Pos
	err error
}

type checker struct {
	p       *Program
	errs    []posErr
	results *types.Tuple // results of the function being checked
	seen    map[string]bool
}

func (c *checker) fail(pos token.Pos, format string, args ...any) {
	p := c.p.Fset.Position(pos)
	msg := fmt.Sprintf(format, args...)
	key := fmt.Sprintf("%s:%d:%s", p.Filename, p.Line, msg) // one report per line and message
	if c.seen[key] {
		return
	}
	c.seen[key] = true
	c.errs = append(c.errs, posErr{pos, fmt.Errorf("%s: gogo: %s", p, msg)})
}

func (c *checker) unsupported(n ast.Node, what string) { c.fail(n.Pos(), "%s not supported", what) }

// kindName names an AST node kind for error messages.
func kindName(n ast.Node) string {
	switch n.(type) {
	case *ast.GoStmt:
		return "go statement"
	case *ast.DeferStmt:
		return "defer statement"
	case *ast.SelectStmt:
		return "select statement"
	case *ast.SendStmt:
		return "channel send"
	case *ast.RangeStmt:
		return "range statement"
	case *ast.SwitchStmt:
		return "switch statement"
	case *ast.LabeledStmt:
		return "labeled statement"
	case *ast.TypeAssertExpr:
		return "type assertion"
	case *ast.IndexExpr, *ast.IndexListExpr:
		return "index expression"
	case *ast.SliceExpr:
		return "slice expression"
	case *ast.StarExpr:
		return "pointer dereference"
	}
	return strings.TrimPrefix(fmt.Sprintf("%T", n), "*ast.")
}

// typeProblem names why t is outside the subset, or "" if t is allowed.
func (c *checker) typeProblem(t types.Type) string {
	switch t := types.Unalias(t).(type) {
	case *types.Basic:
		switch t.Kind() {
		case types.Int, types.Int64, types.Bool, types.String, types.UntypedInt, types.UntypedBool, types.UntypedString:
			return ""
		case types.Float32, types.Float64, types.UntypedFloat:
			return "float"
		case types.Complex64, types.Complex128, types.UntypedComplex:
			return "complex"
		case types.UntypedRune:
			return "rune"
		case types.UntypedNil:
			return "nil"
		}
		return t.Name()
	case *types.Named:
		obj := t.Obj()
		if obj.Pkg() == nil {
			return obj.Name() // error, comparable
		}
		if c.p.compiled(obj.Pkg()) || obj.Pkg().Path() == ActorPath {
			return ""
		}
		return obj.Name()
	case *types.Pointer:
		return "pointer"
	case *types.Slice:
		return "slice"
	case *types.Array:
		return "array"
	case *types.Map:
		return "map"
	case *types.Chan:
		return "channel"
	case *types.Signature:
		if isFunc(t) {
			return ""
		}
		return "func type " + types.TypeString(t, pkgName)
	case *types.Interface:
		if t.Empty() {
			return "" // any
		}
		return "interface"
	case *types.Struct:
		return "anonymous struct"
	case *types.Tuple:
		switch t.Len() {
		case 0:
			return ""
		case 1:
			return c.typeProblem(t.At(0).Type())
		}
		return "multiple values"
	}
	return fmt.Sprintf("type %s", t)
}

func (c *checker) decl(d ast.Decl) {
	switch d := d.(type) {
	case *ast.GenDecl:
		switch d.Tok {
		case token.IMPORT: // validated by Load
		case token.CONST, token.VAR:
			for _, s := range d.Specs {
				c.valueSpec(s.(*ast.ValueSpec))
			}
		case token.TYPE:
			for _, s := range d.Specs {
				c.typeSpec(s.(*ast.TypeSpec))
			}
		default:
			c.unsupported(d, kindName(d))
		}
	case *ast.FuncDecl:
		if d.Recv != nil {
			c.unsupported(d, "method")
			return
		}
		if d.Type.TypeParams != nil {
			c.unsupported(d, "generic function")
			return
		}
		if d.Name.Name == "init" {
			c.unsupported(d, "func init") // would never be called
			return
		}
		c.fieldList(d.Type.Params)
		if d.Type.Results != nil {
			c.fieldList(d.Type.Results)
			n := 0
			for _, f := range d.Type.Results.List {
				n += max(1, len(f.Names))
				if len(f.Names) > 0 {
					c.unsupported(f, "named results")
				}
			}
			if n > 1 {
				c.unsupported(d.Type.Results, "multiple results")
			}
		}
		if d.Body == nil {
			c.unsupported(d, "function without body")
			return
		}
		if fn, ok := c.p.Info.Defs[d.Name].(*types.Func); ok {
			c.results = fn.Type().(*types.Signature).Results()
		}
		c.capturedWrites(d.Body)
		c.block(d.Body)
	default:
		c.unsupported(d, kindName(d))
	}
}

func (c *checker) fieldList(fl *ast.FieldList) {
	if fl == nil {
		return
	}
	for _, f := range fl.List {
		c.typeExpr(f.Type)
	}
}

func (c *checker) typeSpec(s *ast.TypeSpec) {
	if s.Assign.IsValid() {
		c.unsupported(s, "type alias")
		return
	}
	if s.TypeParams != nil {
		c.unsupported(s, "generic type")
		return
	}
	st, ok := s.Type.(*ast.StructType)
	if !ok {
		if tv, ok := c.p.Info.Types[s.Type]; ok && tv.IsType() && isBasic(tv.Type) {
			c.typeExpr(s.Type) // a named basic type: type C int
			return
		}
		c.unsupported(s, "type definition other than struct or int, int64, bool, string")
		return
	}
	for _, f := range st.Fields.List {
		if len(f.Names) == 0 {
			c.unsupported(f, "embedded field")
			continue
		}
		c.typeExpr(f.Type)
	}
}

// pkgName qualifies type names in messages by package name.
func pkgName(p *types.Package) string { return p.Name() }

// isFunc reports whether t is func(), the only func type of the subset.
func isFunc(t types.Type) bool {
	if t == nil {
		return false
	}
	sig, ok := t.Underlying().(*types.Signature)
	return ok && sig.Params().Len() == 0 && sig.Results().Len() == 0 && !sig.Variadic() && sig.TypeParams() == nil
}

// underlying is t.Underlying(), or nil for a nil t.
func underlying(t types.Type) types.Type {
	if t == nil {
		return nil
	}
	return t.Underlying()
}

// isBasic reports whether t's underlying type is int, int64, bool or string.
func isBasic(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && (b.Kind() == types.Int || b.Kind() == types.Int64 || b.Kind() == types.Bool || b.Kind() == types.String)
}

// basicKind is t's kind if t is an unnamed basic type, else types.Invalid.
func basicKind(t types.Type) types.BasicKind {
	if b, ok := types.Unalias(t).(*types.Basic); ok {
		return b.Kind()
	}
	return types.Invalid
}

// isIntConv reports whether conversion call is one the subset allows:
// int64(x) or int(x) where x is an int, an int64 or an untyped constant.
func (c *checker) isIntConv(call *ast.CallExpr) bool {
	to := basicKind(c.p.Info.TypeOf(call.Fun))
	if (to != types.Int && to != types.Int64) || len(call.Args) != 1 {
		return false
	}
	switch basicKind(c.p.Info.TypeOf(call.Args[0])) {
	case types.Int, types.Int64, types.UntypedInt:
		return true
	}
	return false
}

// isAny reports whether t is the empty interface.
func isAny(t types.Type) bool {
	if t == nil {
		return false
	}
	i, ok := types.Unalias(t).(*types.Interface)
	return ok && i.Empty()
}

// convert checks the implicit conversion of e to type target (an
// assignment, argument, result or field value). A value of a named basic
// type cannot go into an interface: at run time it is indistinguishable
// from its underlying type. Nor can a func value.
func (c *checker) convert(target types.Type, e ast.Expr) {
	if !isAny(target) {
		return
	}
	c.noFuncInAny(e)
	if n, ok := types.Unalias(c.p.Info.TypeOf(e)).(*types.Named); ok {
		if _, basic := n.Underlying().(*types.Basic); basic {
			c.fail(e.Pos(), "named basic type in interface not supported")
		}
	}
}

// noFuncInAny rejects e, which goes into an interface, if it is a func value.
func (c *checker) noFuncInAny(e ast.Expr) {
	if isFunc(c.p.Info.TypeOf(e)) {
		c.fail(e.Pos(), "func value in interface not supported")
	}
}

// typeExpr checks an expression that denotes a type.
func (c *checker) typeExpr(e ast.Expr) {
	tv, ok := c.p.Info.Types[e]
	if !ok || !tv.IsType() {
		c.unsupported(e, kindName(e))
		return
	}
	if msg := c.typeProblem(tv.Type); msg != "" {
		c.unsupported(e, msg)
		return
	}
	switch e.(type) {
	case *ast.Ident, *ast.SelectorExpr, *ast.FuncType: // FuncType: func(), checked above
	default:
		c.unsupported(e, kindName(e))
	}
}

func (c *checker) valueSpec(s *ast.ValueSpec) {
	if s.Type != nil {
		c.typeExpr(s.Type)
	}
	for _, v := range s.Values {
		if s.Type != nil {
			c.convert(c.p.Info.TypeOf(s.Type), v)
		}
		c.expr(v)
	}
}

func (c *checker) block(b *ast.BlockStmt) {
	for _, s := range b.List {
		c.stmt(s)
	}
}

func (c *checker) stmt(s ast.Stmt) {
	switch s := s.(type) {
	case *ast.BlockStmt:
		c.block(s)
	case *ast.EmptyStmt:
	case *ast.ExprStmt:
		call, ok := ast.Unparen(s.X).(*ast.CallExpr)
		if !ok {
			c.unsupported(s, "expression statement")
			return
		}
		c.call(call, false)
	case *ast.AssignStmt:
		c.assign(s)
	case *ast.IncDecStmt:
		c.lhs(s.X)
	case *ast.DeclStmt:
		gd, ok := s.Decl.(*ast.GenDecl)
		if !ok || (gd.Tok != token.VAR && gd.Tok != token.CONST) {
			c.unsupported(s, "local type declaration")
			return
		}
		c.decl(gd)
	case *ast.ReturnStmt:
		if len(s.Results) > 1 {
			c.unsupported(s, "multiple results")
			return
		}
		for _, r := range s.Results {
			if c.results != nil && c.results.Len() == 1 {
				c.convert(c.results.At(0).Type(), r)
			}
			c.expr(r)
		}
	case *ast.IfStmt:
		if s.Init != nil {
			c.stmt(s.Init)
		}
		c.expr(s.Cond)
		c.block(s.Body)
		if s.Else != nil {
			c.stmt(s.Else)
		}
	case *ast.ForStmt:
		if s.Init != nil {
			c.stmt(s.Init)
		}
		if s.Cond != nil {
			c.expr(s.Cond)
		}
		if s.Post != nil {
			c.stmt(s.Post)
		}
		c.block(s.Body)
	case *ast.BranchStmt:
		if (s.Tok != token.BREAK && s.Tok != token.CONTINUE) || s.Label != nil {
			c.unsupported(s, strings.TrimSpace(s.Tok.String()+" statement"))
		}
	case *ast.TypeSwitchStmt:
		c.typeSwitch(s)
	default:
		c.unsupported(s, kindName(s))
	}
}

// capturedWrites rejects every write to a variable that a func literal in
// body captures. Closures copy their captures by value, which matches Go's
// by-reference capture only if the variable never changes after its
// declaration (its single assignment), in or out of the closure.
func (c *checker) capturedWrites(body *ast.BlockStmt) {
	captured := map[*types.Var]bool{}
	ast.Inspect(body, func(n ast.Node) bool {
		if lit, ok := n.(*ast.FuncLit); ok {
			for _, v := range litCaptures(c.p.Info, lit) {
				captured[v] = true
			}
		}
		return true
	})
	if len(captured) == 0 {
		return
	}
	write := func(e ast.Expr) {
		// Uses, not Defs: the variables a := declares are not writes.
		if id, ok := ast.Unparen(e).(*ast.Ident); ok {
			if v, ok := c.p.Info.Uses[id].(*types.Var); ok && captured[v] {
				c.fail(id.Pos(), "captured variable must not be reassigned")
			}
		}
	}
	ast.Inspect(body, func(n ast.Node) bool {
		switch n := n.(type) {
		case *ast.AssignStmt:
			for _, l := range n.Lhs {
				write(l)
			}
		case *ast.IncDecStmt:
			write(n.X)
		}
		return true
	})
}

// litCaptures lists the variables lit uses but does not declare, in order
// of first reference (nested literals included). A closure copies them by
// value.
func litCaptures(info *types.Info, lit *ast.FuncLit) []*types.Var {
	var vars []*types.Var
	seen := map[*types.Var]bool{}
	ast.Inspect(lit.Body, func(node ast.Node) bool {
		id, ok := node.(*ast.Ident)
		if !ok {
			return true
		}
		v, ok := info.Uses[id].(*types.Var)
		if !ok || v.IsField() || seen[v] || (v.Pos() >= lit.Pos() && v.Pos() < lit.End()) {
			return true
		}
		seen[v] = true
		vars = append(vars, v)
		return true
	})
	return vars
}

// lhs reports an unsupported assignment target.
func (c *checker) lhs(e ast.Expr) {
	switch e := e.(type) {
	case *ast.Ident: // writes to captured variables: see capturedWrites
	case *ast.ParenExpr:
		c.lhs(e.X)
	case *ast.SelectorExpr:
		c.unsupported(e, "field assignment")
	case *ast.IndexExpr, *ast.IndexListExpr:
		c.unsupported(e, "index assignment")
	case *ast.StarExpr:
		c.unsupported(e, "pointer assignment")
	default:
		c.unsupported(e, kindName(e))
	}
}

func (c *checker) assign(s *ast.AssignStmt) {
	switch s.Tok {
	case token.ASSIGN, token.DEFINE, token.ADD_ASSIGN, token.SUB_ASSIGN, token.MUL_ASSIGN, token.QUO_ASSIGN, token.REM_ASSIGN:
	default:
		c.unsupported(s, "operator "+s.Tok.String())
		return
	}
	if call := c.twoResultCall(s); call != nil {
		for _, l := range s.Lhs {
			c.lhs(l)
		}
		c.call(call, true)
		return
	}
	if len(s.Lhs) != len(s.Rhs) {
		c.unsupported(s, "multi-value assignment")
		return
	}
	for _, l := range s.Lhs {
		c.lhs(l)
	}
	for i, r := range s.Rhs {
		if s.Tok == token.ASSIGN || s.Tok == token.DEFINE {
			c.convert(c.p.Info.TypeOf(s.Lhs[i]), r)
		}
		c.expr(r)
	}
}

// twoResult lists the gogo/actor functions that return two values.
var twoResult = map[string]bool{"ReceiveTimeout": true, "Call": true, "Whereis": true}

// twoResultCall returns the call of `a, b := actor.X(...)` or
// `a, b = actor.X(...)` when X is a two-result builtin, else nil.
func (c *checker) twoResultCall(s *ast.AssignStmt) *ast.CallExpr {
	if len(s.Lhs) != 2 || len(s.Rhs) != 1 || (s.Tok != token.ASSIGN && s.Tok != token.DEFINE) {
		return nil
	}
	call, ok := ast.Unparen(s.Rhs[0]).(*ast.CallExpr)
	if !ok || !twoResult[c.actorFunc(call)] {
		return nil
	}
	return call
}

func (c *checker) typeSwitch(s *ast.TypeSwitchStmt) {
	if s.Init != nil {
		c.unsupported(s.Init, "type switch initializer")
		return
	}
	var ta *ast.TypeAssertExpr
	switch a := s.Assign.(type) {
	case *ast.ExprStmt:
		ta, _ = a.X.(*ast.TypeAssertExpr)
	case *ast.AssignStmt:
		if len(a.Rhs) == 1 {
			ta, _ = a.Rhs[0].(*ast.TypeAssertExpr)
		}
	}
	if ta == nil || !isAny(c.p.Info.TypeOf(ta.X)) {
		c.unsupported(s, "type switch over a value other than any")
		return
	}
	c.expr(ta.X)
	for _, cl := range s.Body.List {
		cc := cl.(*ast.CaseClause)
		for _, t := range cc.List {
			if !c.caseType(t) {
				c.unsupported(t, "type switch case other than a struct, int, int64, bool, string, actor.PID or actor.Ref")
			}
		}
		for _, b := range cc.Body {
			c.stmt(b)
		}
	}
}

// caseType reports whether t is an allowed type switch case: a struct type,
// int, int64, bool, string, actor.PID or actor.Ref.
func (c *checker) caseType(t ast.Expr) bool {
	tv, ok := c.p.Info.Types[t]
	if !ok || !tv.IsType() {
		return false
	}
	switch ty := types.Unalias(tv.Type).(type) {
	case *types.Basic:
		return isBasic(ty)
	case *types.Named:
		if isPID(ty) || isRef(ty) {
			return true
		}
		_, isStruct := ty.Underlying().(*types.Struct)
		return isStruct && c.typeProblem(ty) == ""
	}
	return false
}

// actorFunc returns the name of the gogo/actor function a call invokes, or "".
func (c *checker) actorFunc(call *ast.CallExpr) string {
	sel, ok := ast.Unparen(call.Fun).(*ast.SelectorExpr)
	if !ok || c.p.Info.Selections[sel] != nil {
		return ""
	}
	if fn, ok := c.p.Info.Uses[sel.Sel].(*types.Func); ok && fn.Pkg() != nil && fn.Pkg().Path() == ActorPath {
		return fn.Name()
	}
	return ""
}

// call checks a call; allowTwo admits the two results of a two-result
// builtin (the caller has checked the assignment around it).
func (c *checker) call(call *ast.CallExpr, allowTwo bool) {
	if tv, ok := c.p.Info.Types[call.Fun]; ok && tv.IsType() {
		if !c.isIntConv(call) {
			c.unsupported(call, "conversion")
			return
		}
		c.expr(call.Args[0])
		return
	}
	if tv, ok := c.p.Info.Types[call]; ok && !allowTwo {
		if msg := c.typeProblem(tv.Type); msg != "" {
			c.unsupported(call, msg)
			return
		}
	}
	if call.Ellipsis.IsValid() {
		c.unsupported(call, "variadic call")
		return
	}
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		switch obj := c.p.Info.Uses[fun].(type) {
		case *types.Builtin:
			if obj.Name() != "println" {
				c.unsupported(call, "builtin "+obj.Name())
				return
			}
			for _, a := range call.Args {
				c.noFuncInAny(a)
			}
		case *types.Func:
			if !c.p.compiled(obj.Pkg()) {
				c.unsupported(call, "call of "+obj.Name())
				return
			}
		default:
			c.fail(call.Pos(), "calling a func value not supported")
			return
		}
	case *ast.SelectorExpr:
		if c.actorFunc(call) == "" && !c.libFunc(fun) {
			if sel := c.p.Info.Selections[fun]; sel != nil && sel.Kind() == types.FieldVal {
				c.fail(call.Pos(), "calling a func value not supported")
			} else {
				c.unsupported(call, "method call")
			}
			return
		}
	default:
		c.fail(call.Pos(), "calling a func value not supported")
		return
	}
	c.args(call, 0)
}

// libFunc reports whether sel names a function of a compiled library
// package (lib.F).
func (c *checker) libFunc(sel *ast.SelectorExpr) bool {
	fn, ok := c.p.Info.Uses[sel.Sel].(*types.Func)
	return ok && c.p.Info.Selections[sel] == nil && c.p.isLib(fn.Pkg())
}

// args checks the arguments of a call of a declared function from index
// first on, including their conversion to the parameter types.
func (c *checker) args(call *ast.CallExpr, first int) {
	sig, _ := c.p.Info.TypeOf(call.Fun).(*types.Signature)
	for i := first; i < len(call.Args); i++ {
		a := call.Args[i]
		if sig != nil && !sig.Variadic() && i < sig.Params().Len() {
			c.convert(sig.Params().At(i).Type(), a)
		}
		c.expr(a)
	}
}

func (c *checker) expr(e ast.Expr) {
	if tv, ok := c.p.Info.Types[e]; ok {
		if tv.IsType() {
			c.typeExpr(e)
			return
		}
		if _, isIdent := e.(*ast.Ident); !isIdent {
			if _, isCall := e.(*ast.CallExpr); !isCall {
				if msg := c.typeProblem(tv.Type); msg != "" {
					c.unsupported(e, msg)
					return
				}
			}
		}
	}
	switch e := e.(type) {
	case *ast.Ident:
		switch obj := c.p.Info.Uses[e].(type) {
		case *types.Var:
			if msg := c.typeProblem(obj.Type()); msg != "" {
				c.unsupported(e, msg)
			}
		case *types.Const, nil:
		case *types.Nil:
			c.unsupported(e, "nil")
		case *types.Func: // a top-level function as a func() value
			if msg := c.typeProblem(obj.Type()); msg != "" {
				c.unsupported(e, msg)
			}
		default:
			c.unsupported(e, kindName(e)+" "+obj.Name())
		}
	case *ast.BasicLit:
		switch e.Kind {
		case token.INT, token.STRING:
		case token.FLOAT:
			c.unsupported(e, "float literal")
		default:
			c.unsupported(e, strings.ToLower(e.Kind.String())+" literal")
		}
	case *ast.ParenExpr:
		c.expr(e.X)
	case *ast.FuncLit: // its type, func(), was checked above
		saved := c.results
		c.results = nil
		c.block(e.Body)
		c.results = saved
	case *ast.CompositeLit:
		c.compositeLit(e)
	case *ast.SelectorExpr:
		if obj, ok := c.p.Info.Uses[e.Sel].(*types.Const); ok && obj.Pkg() != nil && (obj.Pkg().Path() == ActorPath || c.p.isLib(obj.Pkg())) {
			return // actor.Normal etc.
		}
		if c.libFunc(e) {
			return // lib.F as a func() value; its type was checked above
		}
		sel := c.p.Info.Selections[e]
		if sel == nil || sel.Kind() != types.FieldVal {
			c.unsupported(e, "selector other than a field read")
			return
		}
		c.expr(e.X)
	case *ast.CallExpr:
		c.call(e, false)
	case *ast.UnaryExpr:
		if e.Op != token.SUB && e.Op != token.NOT {
			c.unsupported(e, "operator "+e.Op.String())
			return
		}
		c.expr(e.X)
	case *ast.BinaryExpr:
		c.binary(e)
	default:
		c.unsupported(e, kindName(e))
	}
}

func (c *checker) compositeLit(e *ast.CompositeLit) {
	if e.Type == nil {
		c.unsupported(e, "composite literal without type")
		return
	}
	c.typeExpr(e.Type)
	t := c.p.Info.TypeOf(e)
	if isPID(t) || isRef(t) {
		c.unsupported(e, "composite literal of "+types.TypeString(t, func(p *types.Package) string { return p.Name() }))
		return
	}
	st, _ := underlying(t).(*types.Struct)
	for i, el := range e.Elts {
		val := el
		var field types.Type
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			val = kv.Value
			if id, ok := kv.Key.(*ast.Ident); ok {
				if v, ok := c.p.Info.Uses[id].(*types.Var); ok {
					field = v.Type()
				}
			}
		} else if st != nil && i < st.NumFields() {
			field = st.Field(i).Type()
		}
		c.convert(field, val)
		c.expr(val)
	}
}

func (c *checker) binary(e *ast.BinaryExpr) {
	isConst := c.p.Info.Types[e].Value != nil // constant expressions are folded
	switch e.Op {
	case token.ADD, token.SUB, token.MUL, token.QUO, token.REM,
		token.EQL, token.NEQ, token.LSS, token.LEQ, token.GTR, token.GEQ,
		token.LAND, token.LOR:
	default:
		if !isConst {
			c.unsupported(e, "operator "+e.Op.String())
			return
		}
	}
	if !isConst {
		xt := c.p.Info.TypeOf(e.X)
		switch e.Op {
		case token.EQL, token.NEQ:
			// Both sides: Go converts a non-interface operand to the other
			// side's interface type, which is not in the subset.
			for _, t := range []types.Type{xt, c.p.Info.TypeOf(e.Y)} {
				if !c.comparable(t) {
					c.unsupported(e, "comparison of "+types.TypeString(t, nil))
					return
				}
			}
		case token.LAND, token.LOR:
		default:
			if b, ok := underlying(xt).(*types.Basic); !ok || b.Info()&types.IsInteger == 0 {
				c.unsupported(e, "operator "+e.Op.String()+" on "+types.TypeString(xt, nil))
				return
			}
		}
	}
	c.expr(e.X)
	c.expr(e.Y)
}

// comparable reports whether == is allowed on t: int, int64, bool (or a named
// type of them), actor.PID or actor.Ref.
func (c *checker) comparable(t types.Type) bool {
	if isPID(t) || isRef(t) {
		return true
	}
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&(types.IsInteger|types.IsBoolean) != 0
}

func (c *checker) errList() []error {
	out := make([]error, len(c.errs))
	for i, e := range c.errs {
		out[i] = e.err
	}
	return out
}
