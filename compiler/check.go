package compiler

import (
	"fmt"
	"go/ast"
	"go/token"
	"go/types"
	"sort"
	"strings"
)

// Check verifies that p stays inside the M1 language subset. It is an
// allow-list walk: any node kind not handled explicitly is reported as
// "<kind> not supported". Errors are sorted by position.
func Check(p *Program) (errs []error) {
	c := &checker{p: p, seen: map[string]bool{}}
	defer func() {
		if r := recover(); r != nil {
			errs = append(c.errList(), fmt.Errorf("gogo: internal checker error: %v", r))
		}
	}()
	for _, f := range p.Files {
		for _, d := range f.Decls {
			if gd, ok := d.(*ast.GenDecl); ok && gd.Tok == token.VAR {
				c.unsupported(gd, "global variable")
				continue
			}
			c.decl(d)
		}
	}
	sort.SliceStable(c.errs, func(i, j int) bool { return c.errs[i].pos < c.errs[j].pos })
	return c.errList()
}

type posErr struct {
	pos token.Pos
	err error
}

type checker struct {
	p    *Program
	errs []posErr
	lit  *ast.FuncLit // innermost Spawn closure being checked
	seen map[string]bool
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
		case types.Int, types.Bool, types.String, types.UntypedInt, types.UntypedBool, types.UntypedString:
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
		if obj.Pkg() == c.p.Pkg || obj.Pkg().Path() == ActorPath {
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
		return "func value"
	case *types.Interface:
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
		c.unsupported(s, "type definition other than struct")
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
	if _, ok := e.(*ast.Ident); !ok {
		if _, ok := e.(*ast.SelectorExpr); !ok {
			c.unsupported(e, kindName(e))
		}
	}
}

func (c *checker) valueSpec(s *ast.ValueSpec) {
	if s.Type != nil {
		c.typeExpr(s.Type)
	}
	for _, v := range s.Values {
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

// captured rejects writes to variables declared outside the current Spawn
// closure: captures are copied by value, so such a write would be lost.
func (c *checker) captured(id *ast.Ident) {
	v, ok := c.p.Info.ObjectOf(id).(*types.Var)
	if !ok || c.lit == nil {
		return
	}
	if v.Pos() < c.lit.Pos() || v.Pos() >= c.lit.End() {
		c.unsupported(id, "assignment to captured variable")
	}
}

// lhs reports an unsupported assignment target.
func (c *checker) lhs(e ast.Expr) {
	switch e := e.(type) {
	case *ast.Ident:
		c.captured(e)
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
	if len(s.Lhs) != len(s.Rhs) {
		c.unsupported(s, "multi-value assignment")
		return
	}
	for _, l := range s.Lhs {
		c.lhs(l)
	}
	for _, r := range s.Rhs {
		c.expr(r)
	}
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
	var recv *ast.CallExpr
	if ta != nil {
		recv, _ = ast.Unparen(ta.X).(*ast.CallExpr)
	}
	if recv == nil || c.actorFunc(recv) != "Receive" {
		c.unsupported(s, "type switch over anything but actor.Receive()")
		return
	}
	c.call(recv, true)
	for _, cl := range s.Body.List {
		cc := cl.(*ast.CaseClause)
		for _, t := range cc.List {
			tv, ok := c.p.Info.Types[t]
			named, isNamed := types.Unalias(tv.Type).(*types.Named)
			if !ok || !tv.IsType() || !isNamed {
				c.unsupported(t, "type switch case other than a struct type")
				continue
			}
			if _, isStruct := named.Underlying().(*types.Struct); !isStruct || c.typeProblem(named) != "" {
				c.unsupported(t, "type switch case other than a struct type")
			}
		}
		for _, b := range cc.Body {
			c.stmt(b)
		}
	}
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

func (c *checker) call(call *ast.CallExpr, allowIface bool) {
	if tv, ok := c.p.Info.Types[call.Fun]; ok && tv.IsType() {
		c.unsupported(call, "conversion")
		return
	}
	if tv, ok := c.p.Info.Types[call]; ok && !allowIface {
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
		case *types.Func:
			if obj.Pkg() != c.p.Pkg {
				c.unsupported(call, "call of "+obj.Name())
				return
			}
		default:
			c.unsupported(call, "call of func value")
			return
		}
	case *ast.SelectorExpr:
		if c.actorFunc(call) == "" {
			c.unsupported(call, "method call")
			return
		}
		if c.actorFunc(call) == "Spawn" && len(call.Args) == 1 {
			if lit, ok := ast.Unparen(call.Args[0]).(*ast.FuncLit); ok {
				saved := c.lit
				c.lit = lit
				c.block(lit.Body)
				c.lit = saved
				return
			}
		}
	default:
		c.expr(call.Fun)
		return
	}
	for _, a := range call.Args {
		c.arg(call, a)
	}
}

// arg checks a call argument. actor.Send's message is converted to any, so
// the interface type recorded for an untyped constant is not a problem.
func (c *checker) arg(call *ast.CallExpr, a ast.Expr) {
	if c.actorFunc(call) == "Send" && len(call.Args) == 2 && a == call.Args[1] {
		if tv, ok := c.p.Info.Types[a]; ok && tv.Value != nil {
			return
		}
	}
	if id, ok := ast.Unparen(a).(*ast.Ident); ok && c.actorFunc(call) == "Send" && len(call.Args) == 2 && a == call.Args[1] {
		if _, isVar := c.p.Info.Uses[id].(*types.Var); isVar {
			return // forwarding a received message is allowed
		}
	}
	c.expr(a)
}

func (c *checker) expr(e ast.Expr) {
	if tv, ok := c.p.Info.Types[e]; ok {
		if tv.IsType() {
			c.typeExpr(e)
			return
		}
		if _, isIdent := e.(*ast.Ident); !isIdent {
			if _, isCall := e.(*ast.CallExpr); !isCall {
				if _, isLit := e.(*ast.FuncLit); !isLit {
					if msg := c.typeProblem(tv.Type); msg != "" {
						c.unsupported(e, msg)
						return
					}
				}
			}
		}
	}
	switch e := e.(type) {
	case *ast.Ident:
		switch obj := c.p.Info.Uses[e].(type) {
		case *types.Var:
			if c.typeProblem(obj.Type()) == "interface" {
				c.unsupported(e, "interface value")
			}
		case *types.Const, nil:
		case *types.Nil:
			c.unsupported(e, "nil")
		case *types.Func:
			c.unsupported(e, "func value")
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
	case *ast.FuncLit:
		c.fail(e.Pos(), "func literal only allowed as the argument of actor.Spawn")
	case *ast.CompositeLit:
		if e.Type == nil {
			c.unsupported(e, "composite literal without type")
			return
		}
		c.typeExpr(e.Type)
		for _, el := range e.Elts {
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				c.expr(kv.Value)
			} else {
				c.expr(el)
			}
		}
	case *ast.SelectorExpr:
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
			if !c.comparable(xt) {
				c.unsupported(e, "comparison of "+types.TypeString(xt, nil))
				return
			}
		case token.LAND, token.LOR:
		default:
			if b, ok := types.Unalias(xt).(*types.Basic); !ok || b.Info()&types.IsInteger == 0 {
				c.unsupported(e, "operator "+e.Op.String()+" on "+types.TypeString(xt, nil))
				return
			}
		}
	}
	c.expr(e.X)
	c.expr(e.Y)
}

// comparable reports whether == is allowed on t: int, bool or actor.PID.
func (c *checker) comparable(t types.Type) bool {
	switch t := types.Unalias(t).(type) {
	case *types.Basic:
		return t.Info()&(types.IsInteger|types.IsBoolean) != 0
	case *types.Named:
		return t.Obj().Pkg() != nil && t.Obj().Pkg().Path() == ActorPath && t.Obj().Name() == "PID"
	}
	return false
}

func (c *checker) errList() []error {
	out := make([]error, len(c.errs))
	for i, e := range c.errs {
		out[i] = e.err
	}
	return out
}
