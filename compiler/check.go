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
	c := &checker{p: p, seen: map[string]bool{}, okNil: map[*ast.Ident]bool{}}
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
	loops   int          // loops around the statement being checked, in its function
	seen    map[string]bool
	okNil   map[*ast.Ident]bool // nil idents that stand for a nil slice or a nil interface value
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
		case types.Int, types.Int64, types.Bool, types.String, types.UntypedInt, types.UntypedBool, types.UntypedString, types.UntypedRune,
			types.Float32, types.Float64, types.UntypedFloat,
			types.Int8, types.Int16, types.Int32, types.Uint, types.Uint8, types.Uint16, types.Uint32, types.Uint64:
			// Uint8 is byte, Int32 rune; 'x' literals behave as int.
			return ""
		case types.Complex64, types.Complex128, types.UntypedComplex:
			return "complex"
		case types.UntypedNil:
			return "nil"
		}
		return t.Name()
	case *types.Named:
		obj := t.Obj()
		if obj.Pkg() == nil {
			if obj.Name() == "error" {
				return ""
			}
			return obj.Name() // comparable
		}
		if c.p.compiled(obj.Pkg()) {
			return ""
		}
		return obj.Name()
	case *types.Pointer: // 15.6: to any type of the subset but an array
		if isArray(t.Elem()) {
			return "pointer to an array"
		}
		if msg := c.typeProblem(t.Elem()); msg != "" {
			return "pointer to " + msg
		}
		return ""
	case *types.Slice:
		if msg := c.typeProblem(t.Elem()); msg != "" {
			return "slice of " + msg
		}
		return ""
	case *types.Array:
		if msg := c.typeProblem(t.Elem()); msg != "" {
			return "array of " + msg
		}
		return ""
	case *types.Map:
		return "map"
	case *types.Chan:
		return "channel"
	case *types.Signature:
		// Any signature: its parameter and result types must be of the
		// subset (a named struct referring back to itself is fine).
		if t.TypeParams() != nil || t.Recv() != nil {
			return "func type " + types.TypeString(t, pkgName)
		}
		for _, tup := range []*types.Tuple{t.Params(), t.Results()} {
			for i := 0; i < tup.Len(); i++ {
				if c.typeProblem(tup.At(i).Type()) != "" {
					return "func type " + types.TypeString(t, pkgName)
				}
			}
		}
		return ""
	case *types.Interface:
		// any, or methods whose signatures are of the subset (embedded
		// interfaces add theirs); no type sets (constraints only).
		if !t.IsMethodSet() {
			return "interface with a type set"
		}
		for i := 0; i < t.NumMethods(); i++ {
			if c.typeProblem(types.NewSignatureType(nil, nil, nil, t.Method(i).Type().(*types.Signature).Params(), t.Method(i).Type().(*types.Signature).Results(), t.Method(i).Type().(*types.Signature).Variadic())) != "" {
				return "interface method " + t.Method(i).Name() + " of type " + types.TypeString(t.Method(i).Type(), pkgName)
			}
		}
		return ""
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
		if c.p.isBuiltin(c.p.Info.Defs[d.Name]) {
			return // the VM's: its body is never compiled
		}
		if d.Recv != nil && !c.recv(d.Recv) {
			return
		}
		if d.Type.TypeParams != nil {
			c.unsupported(d, "generic function")
			return
		}
		if d.Name.Name == "init" && d.Recv == nil { // a method may be called init
			c.unsupported(d, "func init") // would never be called
			return
		}
		c.fieldList(d.Type.Params)
		c.fieldList(d.Type.Results) // any number of results, named or not
		if d.Body == nil {
			c.unsupported(d, "function without body")
			return
		}
		var sig *types.Signature
		if fn, ok := c.p.Info.Defs[d.Name].(*types.Func); ok {
			sig = fn.Type().(*types.Signature)
			c.results = sig.Results()
		}
		c.capturedWrites(d.Body, sig)
		c.block(d.Body)
	default:
		c.unsupported(d, kindName(d))
	}
}

// recv checks the receiver of a method: a value receiver of a named
// struct or named basic type. It reports false if the method is not
// compiled.
func (c *checker) recv(fl *ast.FieldList) bool {
	f := fl.List[0]
	rt := c.p.Info.TypeOf(f.Type)
	if pt := pointerOf(rt); pt != nil { // a pointer receiver (15.6)
		rt = pt.Elem()
	}
	n, ok := types.Unalias(rt).(*types.Named)
	if !ok || n.TypeArgs().Len() > 0 {
		c.unsupported(f.Type, "receiver of type "+types.TypeString(c.p.Info.TypeOf(f.Type), pkgName))
		return false
	}
	// A type other than a struct or basic one is reported at its
	// declaration (see typeSpec).
	return structType(n) != nil || isBasic(n)
}

func (c *checker) fieldList(fl *ast.FieldList) {
	if fl == nil {
		return
	}
	for _, f := range fl.List {
		if ell, ok := f.Type.(*ast.Ellipsis); ok { // xs ...T: a []T parameter
			c.typeExpr(ell.Elt)
			continue
		}
		c.typeExpr(f.Type)
	}
}

func (c *checker) typeSpec(s *ast.TypeSpec) {
	if c.p.isBuiltin(c.p.Info.Defs[s.Name]) {
		return // a VM object (actor.PID, bytes.Bytes ...)
	}
	if s.TypeParams != nil {
		c.unsupported(s, "generic type")
		return
	}
	if s.Assign.IsValid() { // type A = T: another name for T, any type of the subset
		c.typeExpr(s.Type)
		return
	}
	if isIface(c.p.Info.TypeOf(s.Type)) { // type I interface{ ... }, type E error
		if msg := c.typeProblem(c.p.Info.TypeOf(s.Type)); msg != "" {
			c.unsupported(s, msg)
		}
		return
	}
	st, ok := s.Type.(*ast.StructType)
	if !ok {
		if tv, ok := c.p.Info.Types[s.Type]; ok && tv.IsType() && isBasic(tv.Type) {
			c.typeExpr(s.Type) // a named basic type: type C int
			return
		}
		c.unsupported(s, "type definition other than struct or an integer, float, bool or string type")
		return
	}
	for _, f := range st.Fields.List {
		if _, ptr := f.Type.(*ast.StarExpr); ptr && len(f.Names) == 0 {
			c.unsupported(f.Type, "embedded pointer field") // promoted fields would need a nil check per step
			continue
		}
		c.typeExpr(f.Type) // an embedded field too: a field named after its type
	}
}

// pkgName qualifies type names in messages by package name.
func pkgName(p *types.Package) string { return p.Name() }

// isFunc reports whether t is a func type (of any signature).
func isFunc(t types.Type) bool {
	_, ok := underlying(t).(*types.Signature)
	return ok
}

// funcValueCall reports whether call calls a func value: a variable or
// parameter, a struct field, an element, a call result or a literal of
// func type (compiled to CALLV), rather than a declared function, a
// builtin, a builtin function, a gogo/bytes method or a conversion.
func funcValueCall(info *types.Info, call *ast.CallExpr) bool {
	fun := ast.Unparen(call.Fun)
	if tv, ok := info.Types[fun]; ok && tv.IsType() {
		return false
	}
	switch fun := fun.(type) {
	case *ast.Ident:
		_, ok := info.Uses[fun].(*types.Var)
		return ok
	case *ast.SelectorExpr:
		sel := info.Selections[fun]
		return sel != nil && sel.Kind() == types.FieldVal
	}
	return isFunc(info.TypeOf(fun))
}

// underlying is t.Underlying(), or nil for a nil t.
func underlying(t types.Type) types.Type {
	if t == nil {
		return nil
	}
	return t.Underlying()
}

// isBasic reports whether t's underlying type is an integer type (of
// the subset: not uintptr), float32, float64, bool or string: what a
// named basic type may be.
func isBasic(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && (intKindOf(b).bits != 0 || b.Kind() == types.Bool || b.Kind() == types.String ||
		b.Kind() == types.Float32 || b.Kind() == types.Float64)
}

// isCaseBasic reports whether t's underlying type is a type switch case
// type that TYPEIS tells apart: int, int64, float32, float64, bool or
// string (a narrower or unsigned integer cannot be in an interface).
func isCaseBasic(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && (b.Kind() == types.Int || b.Kind() == types.Int64 || b.Kind() == types.Bool || b.Kind() == types.String ||
		b.Kind() == types.Float32 || b.Kind() == types.Float64)
}

// basicKind is t's kind if t is an unnamed basic type, else types.Invalid.
func basicKind(t types.Type) types.BasicKind {
	if b, ok := types.Unalias(t).(*types.Basic); ok {
		return b.Kind()
	}
	return types.Invalid
}

// isIntConv reports whether conversion call is one the subset allows
// between numbers: T(x) for an integer or float type T (or a named type of
// one: fs.Err, errors.Code), where x is an integer, a float or an untyped
// constant (or of a named type of one).
func (c *checker) isIntConv(call *ast.CallExpr) bool {
	if len(call.Args) != 1 {
		return false
	}
	number := func(t types.Type) bool {
		switch basicKind(t.Underlying()) {
		case types.Float32, types.Float64, types.UntypedFloat:
			return true
		}
		return intKindOf(t).bits != 0
	}
	return number(c.p.Info.TypeOf(call.Fun)) && number(c.p.Info.TypeOf(call.Args[0]))
}

// isSameBasicConv reports whether conversion call is between a bool or
// string type and another of the same underlying type (type Name string:
// Name(s), string(n)): the value is the same.
func (c *checker) isSameBasicConv(call *ast.CallExpr) bool {
	if len(call.Args) != 1 {
		return false
	}
	to, from := underlying(c.p.Info.TypeOf(call.Fun)), underlying(c.p.Info.TypeOf(call.Args[0]))
	tb, ok1 := to.(*types.Basic)
	fb, ok2 := from.(*types.Basic)
	if !ok1 || !ok2 {
		return false
	}
	both := func(info types.BasicInfo) bool { return tb.Info()&info != 0 && fb.Info()&info != 0 }
	return both(types.IsBoolean) || (both(types.IsString) && basicKind(from) != types.UntypedRune && fb.Info()&types.IsInteger == 0)
}

// isByte reports whether t is byte (uint8), unnamed.
func isByte(t types.Type) bool { return basicKind(t) == types.Uint8 }

// anyType is the empty interface, any.
var anyType = types.Universe.Lookup("any").Type()

// isAny reports whether t is the empty interface.
func isAny(t types.Type) bool {
	if t == nil {
		return false
	}
	i, ok := types.Unalias(t).(*types.Interface)
	return ok && i.Empty()
}

// convert checks the implicit conversion of e to type target (an
// assignment, argument, result or field value). A func value cannot go
// into an interface (it has no type id).
func (c *checker) convert(target types.Type, e ast.Expr) {
	if isSlice(target) || isIface(target) || isPointer(target) {
		c.nilSlice(e) // the nil slice, the nil interface value, the nil pointer
	}
	if !isIface(target) {
		return
	}
	c.noFuncInAny(e)
	c.convertType(target, c.p.Info.TypeOf(e), e.Pos())
}

// codeAsError rejects the result of a call that returns an errors.Code
// (fs.WriteFile, code.Load's ...: fs.Err and code.Err are aliases)
// returned or stored as an error, as in `return fs.WriteFile(p, b)` in a
// function that returns error: errors.OK in an error is not nil, so
// success would read as a failure. Its callers are the return, var and
// assignment statements; an argument (errors.Is(fs.Remove(p),
// errors.NotFound), which compares), an explicit error(...) conversion
// and a Code held in a variable convert as any value does.
func (c *checker) codeAsError(target types.Type, e ast.Expr) {
	call, ok := ast.Unparen(e).(*ast.CallExpr)
	if !ok || c.p.Info.Types[call.Fun].IsType() || !types.Identical(target, errorType) || !isErrorsCode(c.p.Info.TypeOf(call)) {
		return // not a call, a conversion, or not a Code into an error
	}
	c.fail(e.Pos(), "errors.Code result used as an error: OK would be a non-nil error; check it against OK first")
}

// isErrorsCode reports whether t is gogo/errors' Code (fs.Err and code.Err
// included: they are aliases).
func isErrorsCode(t types.Type) bool {
	n, ok := types.Unalias(t).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == ErrorsPath && n.Obj().Name() == "Code"
}

// convertType checks the implicit conversion of a value of type src (one
// result of a multi-result call, or e in convert) to type target. A named
// basic value and a sized integer (byte, rune, uint32, ...) are boxed
// (codegen_iface.go).
func (c *checker) convertType(target, src types.Type, pos token.Pos) {
	if !isIface(target) {
		return
	}
	if isFunc(src) {
		c.fail(pos, "func value in interface not supported")
	}
	if isArray(src) {
		c.fail(pos, "array in interface not supported: use a slice or a struct with an array field")
	}
	c.vmMethods(target, src, pos)
	if it, ok := underlying(target).(*types.Interface); ok && isPointer(src) {
		ms := types.NewMethodSet(src)
		for i := 0; i < it.NumMethods(); i++ {
			m := it.Method(i)
			if sel := ms.Lookup(m.Pkg(), m.Name()); sel != nil && len(sel.Index()) > 1 && isPointer(sel.Obj().Type().(*types.Signature).Recv().Type()) {
				c.fail(pos, "%s as a %s not supported: its method %s has a pointer receiver and is promoted from an embedded field (no pointers into fields)",
					types.TypeString(src, pkgName), types.TypeString(target, pkgName), m.Name())
			}
		}
	}
}

// vmMethods rejects a value of concrete type src going into interface
// type target when one of the methods that implement target is a builtin
// of the VM's (bytes.Bytes, bytes.Buffer, fs.File and a struct embedding
// one): dispatch calls only compiled functions, so the value would not
// implement target at run time.
func (c *checker) vmMethods(target, src types.Type, pos token.Pos) {
	it, ok := underlying(target).(*types.Interface)
	if !ok || src == nil || isIface(src) || it.Empty() {
		return
	}
	if m := c.p.vmMethodFor(src, it); m != "" {
		c.fail(pos, "%s as a %s not supported: its method %s is a builtin of the VM, which interfaces cannot call",
			types.TypeString(src, pkgName), types.TypeString(target, pkgName), m)
	}
}

// vmMethodFor returns the name of a method of interface it that concrete
// type src gets from a VM builtin (its own, or promoted from an embedded
// bytes.Buffer, ...), or "".
func (p *Program) vmMethodFor(src types.Type, it *types.Interface) string {
	ms := types.NewMethodSet(src)
	for i := 0; i < it.NumMethods(); i++ {
		m := it.Method(i)
		if sel := ms.Lookup(m.Pkg(), m.Name()); sel != nil {
			if fn, _ := sel.Obj().(*types.Func); fn != nil && p.vmMethod(fn) {
				return m.Name()
			}
		}
	}
	return ""
}

// vmImplements reports whether concrete type src implements interface it
// only through a VM builtin method: at run time it does not (the checker
// keeps such values out of it; a type switch or an assertion says no).
func (p *Program) vmImplements(src types.Type, it *types.Interface) bool {
	return p.vmMethodFor(src, it) != ""
}

// vmMethod reports whether method fn is a builtin: marked itself, or a
// method of a type the VM implements.
func (p *Program) vmMethod(fn *types.Func) bool {
	if p.isBuiltin(fn) {
		return true
	}
	recv := fn.Type().(*types.Signature).Recv()
	if recv == nil {
		return false
	}
	n, ok := types.Unalias(recv.Type()).(*types.Named)
	return ok && p.isBuiltin(n.Obj())
}

// multiCall returns e as a call of a compiled (main or library) function
// or of a func value with two or more results, else nil. Such a call may stand where its
// results are taken apart: `a, b := f()`, `return f()`, `g(f())` or a
// call statement.
func (c *checker) multiCall(e ast.Expr) *ast.CallExpr {
	call, ok := ast.Unparen(e).(*ast.CallExpr)
	if !ok {
		return nil
	}
	if tup, ok := c.p.Info.TypeOf(call).(*types.Tuple); !ok || tup.Len() < 2 {
		return nil
	}
	if funcValueCall(c.p.Info, call) {
		return call
	}
	var id *ast.Ident
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		id = fun
	case *ast.SelectorExpr:
		if c.p.Info.Selections[fun] != nil && c.p.method(fun) == nil {
			return nil
		}
		id = fun.Sel
	default:
		return nil
	}
	if fn, ok := c.p.Info.Uses[id].(*types.Func); !ok || !c.p.compiled(fn.Pkg()) || c.p.isBuiltin(fn) {
		return nil
	}
	return call
}

// multiConvert checks the conversion of each result of multi-result call
// to the matching type of targets (nil entries: no conversion).
func (c *checker) multiConvert(call *ast.CallExpr, targets []types.Type) {
	tup := c.p.Info.TypeOf(call).(*types.Tuple)
	for i := 0; i < tup.Len() && i < len(targets); i++ {
		c.convertType(targets[i], tup.At(i).Type(), call.Pos())
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
	switch e := e.(type) {
	case *ast.Ident, *ast.SelectorExpr, *ast.FuncType, *ast.InterfaceType: // their signatures were checked above
	case *ast.ArrayType:
		c.typeExpr(e.Elt) // []T, [N]T, [...]T
	case *ast.StarExpr:
		c.typeExpr(e.X) // *T
	case *ast.ParenExpr:
		c.typeExpr(e.X)
	default:
		c.unsupported(e, kindName(e))
	}
}

func (c *checker) valueSpec(s *ast.ValueSpec) {
	if len(s.Values) == 1 && len(s.Names) > 1 {
		c.unsupported(s.Values[0], "multiple values") // var a, b = f(): use a, b := f()
		return
	}
	if s.Type != nil {
		c.typeExpr(s.Type)
		if len(s.Values) == 0 && isBuffer(c.p.Info.TypeOf(s.Type)) {
			c.zeroBuffer(s)
		}
	}
	for _, v := range s.Values {
		if s.Type != nil {
			c.convert(c.p.Info.TypeOf(s.Type), v)
			c.codeAsError(c.p.Info.TypeOf(s.Type), v)
		}
		c.expr(v)
	}
}

// zeroBuffer reports a zero bytes.Buffer the compiler can see (spec M4
// §9.1): `var b bytes.Buffer` without a value, `bytes.Buffer{}`, or a
// struct literal leaving a bytes.Buffer field out. Its methods would fail
// at run time (badcode); zero Buffers the checker cannot see (a zero
// struct variable, a named result) still do.
func (c *checker) zeroBuffer(n ast.Node) {
	c.fail(n.Pos(), "zero value of bytes.Buffer not supported: use bytes.NewBuffer")
}

// bufferFieldsSet rejects a struct literal that leaves a bytes.Buffer
// field zero (positional literals list every field).
func (c *checker) bufferFieldsSet(e *ast.CompositeLit, st *types.Struct) {
	if len(e.Elts) > 0 {
		if _, keyed := e.Elts[0].(*ast.KeyValueExpr); !keyed {
			return
		}
	}
	set := map[string]bool{}
	for _, el := range e.Elts {
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			if id, ok := kv.Key.(*ast.Ident); ok {
				set[id.Name] = true
			}
		}
	}
	for i := 0; i < st.NumFields(); i++ {
		if f := st.Field(i); isBuffer(f.Type()) && !set[f.Name()] {
			c.zeroBuffer(e)
			return
		}
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
		c.call(call, c.multiCall(call) != nil) // results discarded
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
	case *ast.DeferStmt:
		c.deferStmt(s)
	case *ast.ReturnStmt:
		if c.results != nil && c.results.Len() > 1 && len(s.Results) == 1 {
			// return g() of a multi-result g
			call := c.multiCall(s.Results[0])
			if call == nil {
				c.unsupported(s.Results[0], "multiple values")
				return
			}
			c.multiConvert(call, tupleTypes(c.results))
			c.call(call, true)
			return
		}
		for i, r := range s.Results {
			if c.results != nil && i < c.results.Len() {
				c.convert(c.results.At(i).Type(), r)
				c.codeAsError(c.results.At(i).Type(), r)
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
		c.loops++
		c.block(s.Body)
		c.loops--
	case *ast.BranchStmt:
		if s.Tok != token.BREAK && s.Tok != token.CONTINUE && s.Tok != token.FALLTHROUGH {
			c.unsupported(s, strings.TrimSpace(s.Tok.String()+" statement"))
		}
	case *ast.LabeledStmt:
		switch s.Stmt.(type) {
		case *ast.ForStmt, *ast.RangeStmt, *ast.SwitchStmt, *ast.TypeSwitchStmt:
			c.stmt(s.Stmt)
		default:
			c.unsupported(s, "label on a statement other than for or switch")
			c.stmt(s.Stmt)
		}
	case *ast.SwitchStmt:
		c.switchStmt(s)
	case *ast.TypeSwitchStmt:
		c.typeSwitch(s)
	case *ast.RangeStmt:
		c.loops++
		c.rangeStmt(s)
		c.loops--
	default:
		c.unsupported(s, kindName(s))
	}
}

// capturedWrites rejects every write to a variable that a func literal in
// body captures. Closures copy their captures by value, which matches Go's
// by-reference capture only if the variable never changes after its
// declaration (its single assignment), in or out of the closure. A return
// with values writes every named result of its function (sig for body, the
// literal's own signature inside a func literal); a bare return writes none.
func (c *checker) capturedWrites(body *ast.BlockStmt, sig *types.Signature) {
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
	results := namedResults(c.p.Info, body, sig)
	deferred := map[*ast.FuncLit]bool{} // defer func() { ... }()
	ast.Inspect(body, func(n ast.Node) bool {
		if d, ok := n.(*ast.DeferStmt); ok {
			if lit, ok := ast.Unparen(d.Call.Fun).(*ast.FuncLit); ok {
				deferred[lit] = true
			}
		}
		return true
	})
	// write checks a write to e; inDefer: e is in a deferred func literal.
	write := func(e ast.Expr, inDefer bool) {
		// A field assignment x.F = v writes x, and so does an element
		// assignment x[i] = v to an array x (but s[i].F = v writes the
		// slice's shared array, not s).
		x, field, elem := ast.Unparen(e), false, false
		for {
			if ix, ok := x.(*ast.IndexExpr); ok && isArray(c.p.Info.TypeOf(ix.X)) {
				x, elem = ast.Unparen(ix.X), true
				continue
			}
			sel, ok := x.(*ast.SelectorExpr)
			if !ok || c.p.Info.Selections[sel] == nil {
				break
			}
			if isPointer(c.p.Info.TypeOf(sel.X)) {
				return // p.X = v writes p's target, not p
			}
			x, field = ast.Unparen(sel.X), true
		}
		// Uses, not Defs: the variables a := declares are not writes.
		if id, ok := x.(*ast.Ident); ok {
			if v, ok := c.p.Info.Uses[id].(*types.Var); ok && captured[v] {
				if results[v] && inDefer {
					c.fail(e.Pos(), "captured named result must not be changed: a deferred func literal captures it by value, so it cannot set the result")
				} else if results[v] {
					c.fail(e.Pos(), "captured named result must not be changed: closures capture by value")
				} else if elem {
					c.fail(e.Pos(), "captured variable must not be changed by element assignment")
				} else if field {
					c.fail(e.Pos(), "captured variable must not be changed by field assignment")
				} else {
					c.fail(id.Pos(), "captured variable must not be reassigned")
				}
			}
		}
	}
	var walk func(n ast.Node, sig *types.Signature, inDefer bool)
	walk = func(n ast.Node, sig *types.Signature, inDefer bool) {
		ast.Inspect(n, func(n ast.Node) bool {
			switch n := n.(type) {
			case *ast.FuncLit:
				s, _ := c.p.Info.TypeOf(n).(*types.Signature)
				walk(n.Body, s, deferred[n])
				return false
			case *ast.ReturnStmt:
				if len(n.Results) == 0 || sig == nil {
					break
				}
				for i := 0; i < sig.Results().Len(); i++ {
					if v := sig.Results().At(i); v.Name() != "" && captured[v] {
						c.fail(n.Pos(), "captured named result must not be set by return; use unnamed results")
						break
					}
				}
			case *ast.AssignStmt:
				for _, l := range n.Lhs {
					write(l, inDefer)
				}
			case *ast.IncDecStmt:
				write(n.X, inDefer)
			}
			return true
		})
	}
	walk(body, sig, false)
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
		c.fieldLhs(e)
	case *ast.IndexExpr:
		if isArray(c.p.Info.TypeOf(e.X)) { // a[i], p.A[i], s[j][i]: x is a variable or on a path
			c.arrayLhs(e.X)
			c.indexArg(e.Index)
			return
		}
		if !isSlice(c.p.Info.TypeOf(e.X)) {
			c.unsupported(e, "index assignment to "+types.TypeString(c.p.Info.TypeOf(e.X), nil))
			return
		}
		c.expr(e.X)
		c.indexArg(e.Index)
	case *ast.IndexListExpr:
		c.unsupported(e, "index assignment")
	case *ast.StarExpr: // *p = v (15.6)
		c.expr(e.X)
	default:
		c.unsupported(e, kindName(e))
	}
}

// fieldLhs checks a field assignment target x.F, where x is a local
// variable, a slice element s[i] or, in turn, a field of a struct (a.b.c).
func (c *checker) fieldLhs(e *ast.SelectorExpr) {
	sel := c.p.Info.Selections[e]
	if sel == nil || sel.Kind() != types.FieldVal {
		c.unsupported(e, "assignment to "+types.ExprString(e))
		return
	}
	if pt := pointerOf(c.p.Info.TypeOf(e.X)); pt != nil && structType(pt.Elem()) != nil {
		c.expr(e.X) // p.X = v through a pointer, any expression (15.6)
		return
	}
	if t := c.p.Info.TypeOf(e.X); structType(t) == nil {
		c.unsupported(e, "field assignment on "+types.TypeString(t, pkgName))
		return
	}
	switch x := ast.Unparen(e.X).(type) {
	case *ast.StarExpr: // (*p).X = v
		c.expr(x.X)
	case *ast.Ident:
	case *ast.SelectorExpr:
		c.fieldLhs(x)
	case *ast.IndexExpr:
		c.lhs(x)
	default:
		c.unsupported(e, "field assignment to "+kindName(x))
	}
}

func (c *checker) assign(s *ast.AssignStmt) {
	switch s.Tok {
	case token.ASSIGN, token.DEFINE, token.ADD_ASSIGN, token.SUB_ASSIGN, token.MUL_ASSIGN, token.QUO_ASSIGN, token.REM_ASSIGN,
		token.AND_ASSIGN, token.OR_ASSIGN, token.XOR_ASSIGN, token.AND_NOT_ASSIGN, token.SHL_ASSIGN, token.SHR_ASSIGN:
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
	if ta := commaOk(s.Lhs, s.Rhs); ta != nil && (s.Tok == token.ASSIGN || s.Tok == token.DEFINE) { // v, ok := x.(T)
		for _, l := range s.Lhs {
			c.lhs(l)
		}
		c.convertType(c.p.Info.TypeOf(s.Lhs[0]), c.p.Info.TypeOf(ta.Type), ta.Pos())
		c.typeAssert(ta)
		return
	}
	if len(s.Lhs) > 1 && len(s.Rhs) == 1 && (s.Tok == token.ASSIGN || s.Tok == token.DEFINE) {
		if call := c.multiCall(s.Rhs[0]); call != nil { // a, b := f()
			targets := make([]types.Type, len(s.Lhs))
			for i, l := range s.Lhs {
				c.lhs(l)
				targets[i] = c.p.Info.TypeOf(l)
			}
			c.multiConvert(call, targets)
			c.call(call, true)
			return
		}
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
			c.codeAsError(c.p.Info.TypeOf(s.Lhs[i]), r)
		}
		c.expr(r)
	}
}

// commaOk returns x.(T) if lhs, rhs is `v, ok = x.(T)`, else nil.
func commaOk(lhs, rhs []ast.Expr) *ast.TypeAssertExpr {
	if len(lhs) != 2 || len(rhs) != 1 {
		return nil
	}
	ta, _ := ast.Unparen(rhs[0]).(*ast.TypeAssertExpr)
	return ta
}

// twoResult lists the builtin functions that return two values (by stubName).
var twoResult = map[string]bool{"ReceiveTimeout": true, "Call": true, "Whereis": true, "port.open": true, "sys.Config": true,
	"fs.Open": true, "fs.ReadFile": true, "fs.Stat": true, "fs.ReadDir": true, "fs.Usage": true,
	"File.Read": true, "File.Size": true,
	"code.Load": true, "code.Spawn": true, "code.SpawnLink": true, "code.SpawnOpt": true,
	"strconv.parseFloat": true}

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
	if ta == nil || !isIface(c.p.Info.TypeOf(ta.X)) {
		c.unsupported(s, "type switch over a value other than an interface")
		return
	}
	c.expr(ta.X)
	for _, cl := range s.Body.List {
		cc := cl.(*ast.CaseClause)
		for _, t := range cc.List {
			if c.p.Info.Types[t].IsNil() {
				continue // case nil
			}
			if isFsFile(c.p.Info.TypeOf(t)) {
				c.unsupported(t, "type switch case fs.File not supported; wrap it in a struct")
			} else if !c.caseType(t) && !c.ifaceCase(t) {
				c.unsupported(t, "type switch case other than a struct, int, int64, byte, float32, float64, bool, string, a named type of those, a pointer, actor.PID, actor.Ref or bytes.Bytes")
			}
		}
		for _, b := range cc.Body {
			c.stmt(b)
		}
	}
}

// switchStmt checks an expression switch: tagged (`switch x { case 1, 2: }`)
// or tagless (`switch { case x > 0: }`), with an optional init statement.
// The tag and every case value must be comparable with ==; a case value
// goes into an interface tag as in an assignment, and the other way round.
func (c *checker) switchStmt(s *ast.SwitchStmt) {
	if s.Init != nil {
		c.stmt(s.Init)
	}
	if s.Tag != nil {
		if t := c.p.Info.TypeOf(s.Tag); !c.comparable(t) {
			c.unsupported(s.Tag, "switch on a value of type "+types.TypeString(t, pkgName))
			return
		}
		c.expr(s.Tag)
	}
	for _, cl := range s.Body.List {
		cc := cl.(*ast.CaseClause)
		for _, e := range cc.List {
			if t := c.p.Info.TypeOf(e); s.Tag != nil && !c.comparable(t) && !(c.p.Info.Types[e].IsNil() && isIface(c.p.Info.TypeOf(s.Tag))) {
				c.unsupported(e, "switch case of type "+types.TypeString(t, pkgName))
				continue
			}
			if s.Tag != nil {
				tt := c.p.Info.TypeOf(s.Tag)
				c.convert(tt, e) // nil, a boxed value into an interface tag
				c.convertType(c.p.Info.TypeOf(e), tt, s.Tag.Pos())
			}
			c.expr(e)
		}
		for _, b := range cc.Body {
			c.stmt(b)
		}
	}
}

// caseType reports whether t is an allowed type switch case or type
// assertion type: a struct type, int, int64, byte, bool, string, a named
// type of those, actor.PID, actor.Ref or bytes.Bytes (not bytes.Buffer,
// which has no type id).
func (c *checker) caseType(t ast.Expr) bool {
	tv, ok := c.p.Info.Types[t]
	if !ok || !tv.IsType() {
		return false
	}
	switch ty := types.Unalias(tv.Type).(type) {
	case *types.Pointer: // *T (15.6)
		return c.typeProblem(ty) == ""
	case *types.Basic:
		return isCaseBasic(ty) || intKindOf(ty).bits != 0 // a sized integer is boxed
	case *types.Named:
		if isFsFile(ty) {
			return false // the same term tag as actor.PID: the case would match every PID
		}
		if isPID(ty) || isRef(ty) || isBytes(ty) {
			return true
		}
		if isBuffer(ty) {
			return false
		}
		switch ty.Underlying().(type) {
		case *types.Struct, *types.Basic:
			return c.typeProblem(ty) == ""
		}
		return false
	}
	return false
}

// actorFunc returns the stubName of the builtin function (gogo/actor, port,
// bytes, net, config or fs) a call invokes, or the fsMethod name of a
// gogo/fs method call, or "".
func (c *checker) actorFunc(call *ast.CallExpr) string {
	if id, ok := ast.Unparen(call.Fun).(*ast.Ident); ok { // a system package's own builtin
		fn, _ := c.p.Info.Uses[id].(*types.Func)
		return c.p.stubName(fn)
	}
	sel, ok := ast.Unparen(call.Fun).(*ast.SelectorExpr)
	if !ok {
		return ""
	}
	if c.p.Info.Selections[sel] != nil {
		return fsMethod(c.p.Info, sel)
	}
	fn, _ := c.p.Info.Uses[sel.Sel].(*types.Func)
	return c.p.stubName(fn)
}

// stubName names a builtin function (//gogo:builtin): "Spawn" for
// actor.Spawn, "port.Open" for port.Open; "" for any other function.
func (p *Program) stubName(fn *types.Func) string {
	if fn == nil || fn.Pkg() == nil || !p.isBuiltin(fn) {
		return ""
	}
	if sig, ok := fn.Type().(*types.Signature); ok && sig.Recv() != nil {
		return "" // a method: see bytesMethod
	}
	switch fn.Pkg().Path() {
	case ActorPath:
		return fn.Name()
	case PortPath:
		return "port." + fn.Name()
	case BytesPath:
		return "bytes." + fn.Name()
	case NetPath:
		return "net." + fn.Name()
	case SysPath:
		return "sys." + fn.Name()
	case FsPath:
		return "fs." + fn.Name()
	case CodePath:
		return "code." + fn.Name()
	case MathPath:
		return "math." + fn.Name()
	case StrconvPath:
		return "strconv." + fn.Name()
	}
	return ""
}

// fsMethod names the gogo/fs method a selector calls, as "File.Read"; ""
// for anything else.
func fsMethod(info *types.Info, sel *ast.SelectorExpr) string {
	s := info.Selections[sel]
	if s == nil || s.Kind() != types.MethodVal {
		return ""
	}
	fn, ok := s.Obj().(*types.Func)
	if !ok || fn.Pkg() == nil || fn.Pkg().Path() != FsPath || !isFsFile(s.Recv()) {
		return ""
	}
	return "File." + fn.Name()
}

// bytesMethod names the gogo/bytes method a selector calls, as
// "Buffer.WriteString" or "Bytes.Len"; "" for anything else. These are the
// only method calls of the subset: the compiler turns each
// into a builtin.
func bytesMethod(info *types.Info, sel *ast.SelectorExpr) string {
	s := info.Selections[sel]
	if s == nil || s.Kind() != types.MethodVal {
		return ""
	}
	fn, ok := s.Obj().(*types.Func)
	if !ok || fn.Pkg() == nil || fn.Pkg().Path() != BytesPath {
		return ""
	}
	recv := s.Recv()
	switch {
	case isBytes(recv):
		return "Bytes." + fn.Name()
	case isBuffer(recv):
		return "Buffer." + fn.Name()
	}
	return ""
}

// tupleTypes lists the types of t's members.
func tupleTypes(t *types.Tuple) []types.Type {
	out := make([]types.Type, t.Len())
	for i := range out {
		out[i] = t.At(i).Type()
	}
	return out
}

// call checks a call; allowTwo admits several results: those of a
// two-result builtin or a multi-result function (multiCall) whose caller
// has checked the context around it.
func (c *checker) call(call *ast.CallExpr, allowTwo bool) {
	if tv, ok := c.p.Info.Types[call.Fun]; ok && tv.IsType() {
		if isIface(tv.Type) && len(call.Args) == 1 { // any(x), error(e), I(x): as an assignment
			c.typeExpr(call.Fun)
			c.convert(tv.Type, call.Args[0])
			c.expr(call.Args[0])
			return
		}
		if !c.isIntConv(call) && !c.isBytesConv(call) && !isPIDConv(c.p.Info, call) && !c.isSameBasicConv(call) {
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
	if call.Ellipsis.IsValid() && !c.isBuiltin(call, "append") {
		if sig, ok := underlying(c.p.Info.TypeOf(call.Fun)).(*types.Signature); !ok || !sig.Variadic() {
			c.unsupported(call, "variadic call")
			return
		}
	}
	if funcValueCall(c.p.Info, call) { // f(x), s.F(x), hs[i](x), mk()(x)
		c.expr(call.Fun)
		c.args(call, 0)
		return
	}
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		switch obj := c.p.Info.Uses[fun].(type) {
		case *types.Builtin:
			switch obj.Name() {
			case "len", "cap", "append", "make", "copy":
				c.seqBuiltin(call, obj.Name())
				return
			case "new": // new(T) (15.6)
				c.typeExpr(call.Args[0])
				if isArray(c.p.Info.TypeOf(call.Args[0])) {
					c.unsupported(call, "pointer to an array")
				}
				return
			case "panic": // panic(v): v goes into an any
				c.convert(anyType, call.Args[0])
				c.expr(call.Args[0])
				return
			case "recover":
				c.unsupported(call, "recover (a panic ends the actor: let its supervisor restart it)")
				return
			case "println":
			default:
				c.unsupported(call, "builtin "+obj.Name())
				return
			}
			for _, a := range call.Args {
				c.noFuncInAny(a)
				if c.multiCall(a) != nil {
					c.unsupported(a, "multiple values") // println(f())
					return
				}
				if t := c.p.Info.TypeOf(a); isBytes(t) || isBuffer(t) {
					c.unsupported(a, "println of "+types.TypeString(t, pkgName)+" (use String())")
					return
				}
				if t := c.p.Info.TypeOf(a); holdsUnsigned(t, nil) {
					c.fail(a.Pos(), "println of %s not supported: it holds uint32 or uint64 values, which would print signed; print them one by one", types.TypeString(t, pkgName))
					return
				}
			}
		case *types.Func:
			if !c.p.compiled(obj.Pkg()) || (c.p.isBuiltin(obj) && c.p.stubName(obj) == "") {
				c.unsupported(call, "call of "+obj.Name())
				return
			}
		default:
			c.unsupported(call, "call of "+fun.Name)
			return
		}
	case *ast.SelectorExpr:
		if c.p.method(fun) != nil { // x.M(args), T.M(x, args)
			c.methodRecv(fun)
			c.args(call, 0)
			return
		}
		if bytesMethod(c.p.Info, fun) != "" { // b.WriteString(s), x.Len() ...
			c.expr(fun.X)
			c.args(call, 0)
			return
		}
		if fsMethod(c.p.Info, fun) != "" { // f.Read(n), f.Close() ...
			c.expr(fun.X)
			c.args(call, 0)
			return
		}
		if c.stubPromoted(fun) {
			return
		}
		if c.actorFunc(call) == "" && !c.libFunc(fun) {
			c.unsupported(call, "method call") // field calls: funcValueCall
			return
		}
	default:
		c.unsupported(call, "call") // func values: funcValueCall
		return
	}
	c.args(call, 0)
}

// method returns the method sel names, as x.M (a method value or call)
// or T.M (a method expression), if it is declared in a compiled package
// and not a builtin, else nil. The builtin methods (gogo/bytes, gogo/fs)
// are compiled by bytesMethod and fsMethod.
func (p *Program) method(sel *ast.SelectorExpr) *types.Func {
	s := p.Info.Selections[sel]
	if s == nil || (s.Kind() != types.MethodVal && s.Kind() != types.MethodExpr) {
		return nil
	}
	fn, ok := s.Obj().(*types.Func)
	if !ok || (!p.compiled(fn.Pkg()) && !isIface(s.Recv())) || p.isBuiltin(fn) {
		return nil
	}
	return fn
}

// ifaceMethod reports whether sel selects a method of an interface value
// (x.M with x of interface type: dispatched at run time).
func (p *Program) ifaceMethod(sel *ast.SelectorExpr) bool {
	s := p.Info.Selections[sel]
	return s != nil && s.Kind() == types.MethodVal && isIface(s.Recv())
}

// methodRecv checks the operand of a method selector: the receiver
// expression of x.M. The type T of a method expression T.M or (T).M was
// checked at the method's declaration. A method expression of a promoted
// method (B.M for a method M of B's embedded A) would need a function of
// its own that takes a B: it is rejected, and so is I.M of an interface I.
func (c *checker) methodRecv(sel *ast.SelectorExpr) {
	s := c.p.Info.Selections[sel]
	fn, _ := s.Obj().(*types.Func)
	ptrRecv := fn != nil && isPointer(fn.Type().(*types.Signature).Recv().Type())
	if s.Kind() == types.MethodVal {
		c.expr(sel.X)
		if !ptrRecv || isIface(s.Recv()) {
			return
		}
		// A pointer method: on a pointer, or on a local variable (&x). A
		// slice element, a field or a promoted method would need a
		// pointer into it (no interior pointers).
		_, isVar := ast.Unparen(sel.X).(*ast.Ident)
		switch {
		case len(s.Index()) > 1:
			c.fail(sel.Pos(), "method %s with a pointer receiver promoted from an embedded field not supported: it needs a pointer to the field (no interior pointers); call it on the field of a pointer, or embed a value whose methods have value receivers", fn.Name())
		case !isPointer(c.p.Info.TypeOf(sel.X)) && (!isVar || c.isGlobal(sel.X)):
			c.fail(sel.Pos(), "method %s has a pointer receiver: %s needs a pointer to it (no interior pointers: &s[i], &v.F); use a pointer (a []*T, a *T field) or a local variable", fn.Name(), types.ExprString(sel.X))
		}
		return
	}
	if isIface(s.Recv()) {
		c.unsupported(sel, "method expression on an interface type")
	} else if len(s.Index()) > 1 {
		c.unsupported(sel, "method expression of a promoted method")
	} else if isPointer(s.Recv()) && !ptrRecv {
		c.unsupported(sel, "method expression (*T).M of a value method")
	}
}

// addrOf checks &x (15.6): &T{...} of a composite literal (not an array),
// or &v of a local variable (not an array), which then lives in a box. An
// interior pointer, &s[i] or &v.F, is rejected: it would point into a
// slice's array or a struct field.
func (c *checker) addrOf(e *ast.UnaryExpr) {
	switch x := ast.Unparen(e.X).(type) {
	case *ast.CompositeLit:
		if isArray(c.p.Info.TypeOf(x)) {
			c.unsupported(e, "pointer to an array")
			return
		}
		c.compositeLit(x)
		return
	case *ast.Ident:
		v, ok := c.p.Info.Uses[x].(*types.Var)
		switch {
		case !ok:
		case c.isGlobal(x):
			c.fail(e.Pos(), "address of package-level variable %s not supported: package-level variables are read-only", x.Name)
		case isArray(v.Type()):
			c.unsupported(e, "pointer to an array")
		default:
			c.expr(x)
		}
		return
	}
	c.fail(e.Pos(), "interior pointer %s not supported: gogo has no pointers into a slice element or a struct field; use a []*T or a *T field, or s[i].F = v", types.ExprString(e))
}

// isGlobal reports whether e names a package-level variable.
func (c *checker) isGlobal(e ast.Expr) bool {
	id, ok := ast.Unparen(e).(*ast.Ident)
	if !ok {
		return false
	}
	v, ok := c.p.Info.Uses[id].(*types.Var)
	return ok && !v.IsField() && v.Pkg() != nil && v.Parent() == v.Pkg().Scope()
}

// libFunc reports whether sel names a function of a compiled library
// package (lib.F).
func (c *checker) libFunc(sel *ast.SelectorExpr) bool {
	fn, ok := c.p.Info.Uses[sel.Sel].(*types.Func)
	return ok && c.p.Info.Selections[sel] == nil && c.p.isLib(fn.Pkg()) && !c.p.isBuiltin(fn)
}

// args checks the arguments of a call of a declared function from index
// first on, including their conversion to the parameter types.
func (c *checker) args(call *ast.CallExpr, first int) {
	sig, _ := underlying(c.p.Info.TypeOf(call.Fun)).(*types.Signature)
	if first == 0 && len(call.Args) == 1 && sig != nil && !sig.Variadic() {
		if inner := c.multiCall(call.Args[0]); inner != nil { // f(g())
			c.multiConvert(inner, tupleTypes(sig.Params()))
			c.call(inner, true)
			return
		}
	}
	for i := first; i < len(call.Args); i++ {
		a := call.Args[i]
		if t := paramType(sig, i, call.Ellipsis.IsValid()); t != nil {
			c.convert(t, a)
		}
		c.expr(a)
	}
}

// paramType is the type argument i of a call of a function of signature
// sig goes into, or nil (sig nil, or no such parameter). The arguments of
// a variadic function from its last parameter on are elements of that
// parameter's slice, unless the call spreads a slice into it (spread).
func paramType(sig *types.Signature, i int, spread bool) types.Type {
	if sig == nil {
		return nil
	}
	n := sig.Params().Len()
	if sig.Variadic() && i >= n-1 {
		last := sig.Params().At(n - 1).Type()
		if spread {
			return last
		}
		return sliceElem(last)
	}
	if i < n {
		return sig.Params().At(i).Type()
	}
	return nil
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
			if !c.okNil[e] {
				c.unsupported(e, "nil")
			}
		case *types.Func: // a top-level function as a func value
			if msg := c.typeProblem(obj.Type()); msg != "" {
				c.unsupported(e, msg)
			}
		default:
			c.unsupported(e, kindName(e)+" "+obj.Name())
		}
	case *ast.BasicLit:
		switch e.Kind {
		case token.INT, token.STRING, token.CHAR, token.FLOAT:
		default:
			c.unsupported(e, strings.ToLower(e.Kind.String())+" literal")
		}
	case *ast.ParenExpr:
		c.expr(e.X)
	case *ast.FuncLit: // its type was checked above
		saved, loops := c.results, c.loops
		c.results, c.loops = c.p.Info.TypeOf(e).(*types.Signature).Results(), 0
		c.block(e.Body)
		c.results, c.loops = saved, loops
	case *ast.CompositeLit:
		c.compositeLit(e)
	case *ast.SelectorExpr:
		if obj, ok := c.p.Info.Uses[e.Sel].(*types.Const); ok && obj.Pkg() != nil && c.p.isLib(obj.Pkg()) {
			return // actor.Normal, port.Output etc.
		}
		if fn, _ := c.p.Info.Uses[e.Sel].(*types.Func); fn != nil && fn.Pkg() != nil && fn.Pkg().Path() == StrconvPath && c.p.stubName(fn) != "" {
			c.unsupported(e, fn.Pkg().Name()+"."+fn.Name()+" as a func value")
			return
		}
		if c.libFunc(e) {
			if fn, _ := c.p.Info.Uses[e.Sel].(*types.Func); c.p.stubName(fn) != "" {
				c.unsupported(e, fn.Pkg().Name()+"."+fn.Name()+" as a func value")
			}
			return // lib.F as a func value; its type was checked above
		}
		if c.p.method(e) != nil {
			c.methodRecv(e) // x.M, a method value, or T.M, a method expression
			return
		}
		if c.stubPromoted(e) {
			return
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
		if e.Op == token.AND {
			c.addrOf(e)
			return
		}
		if e.Op != token.SUB && e.Op != token.NOT && e.Op != token.XOR {
			c.unsupported(e, "operator "+e.Op.String())
			return
		}
		c.expr(e.X)
	case *ast.StarExpr: // *p (15.6)
		c.expr(e.X)
	case *ast.BinaryExpr:
		c.binary(e)
	case *ast.IndexExpr:
		c.indexExpr(e)
	case *ast.SliceExpr:
		c.sliceExpr(e)
	case *ast.TypeAssertExpr:
		c.typeAssert(e)
	default:
		c.unsupported(e, kindName(e))
	}
}

// ifaceCase reports whether t names an interface type of the subset (a
// type switch case or assertion type: does the value implement it).
func (c *checker) ifaceCase(t ast.Expr) bool {
	tv, ok := c.p.Info.Types[t]
	return ok && tv.IsType() && isIface(tv.Type) && c.typeProblem(tv.Type) == ""
}

// typeAssert checks x.(T): x an interface value, T a type a type switch
// may name.
func (c *checker) typeAssert(e *ast.TypeAssertExpr) {
	if e.Type == nil {
		c.unsupported(e, "x.(type) outside a type switch")
		return
	}
	if isFsFile(c.p.Info.TypeOf(e.Type)) {
		c.unsupported(e.Type, "type assertion to fs.File; wrap it in a struct")
		return
	}
	if !c.caseType(e.Type) && !c.ifaceCase(e.Type) {
		c.unsupported(e.Type, "type assertion to "+types.TypeString(c.p.Info.TypeOf(e.Type), pkgName))
		return
	}
	c.expr(e.X)
}

func (c *checker) compositeLit(e *ast.CompositeLit) {
	c.compositeLitIn(e, false)
}

// compositeLitIn checks a composite literal; elided: it is an element of a
// slice literal, whose type it may omit (`[]Point{{1, 2}}`).
func (c *checker) compositeLitIn(e *ast.CompositeLit, elided bool) {
	if e.Type == nil && !elided {
		c.unsupported(e, "composite literal without type")
		return
	}
	if e.Type != nil {
		c.typeExpr(e.Type)
	} else if msg := c.typeProblem(c.p.Info.TypeOf(e)); msg != "" {
		c.unsupported(e, msg)
		return
	}
	t := c.p.Info.TypeOf(e)
	if sl, ok := underlying(t).(*types.Slice); ok {
		c.sliceLit(e, sl)
		return
	}
	if at := arrayType(t); at != nil {
		c.arrayLit(e, at)
		return
	}
	if isBuffer(t) {
		c.zeroBuffer(e)
		return
	}
	if isPID(t) || isRef(t) || isBytes(t) {
		c.unsupported(e, "composite literal of "+types.TypeString(t, func(p *types.Package) string { return p.Name() }))
		return
	}
	st, _ := underlying(t).(*types.Struct)
	if st != nil {
		c.bufferFieldsSet(e, st)
	}
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
		token.AND, token.OR, token.XOR, token.AND_NOT, token.SHL, token.SHR,
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
			if c.nilCompare(e) {
				return
			}
			// Both sides: Go converts a non-interface operand to the other
			// side's interface type (boxing it, see convert).
			yt := c.p.Info.TypeOf(e.Y)
			for _, t := range []types.Type{xt, yt} {
				if !c.comparable(t) {
					c.unsupported(e, "comparison of "+types.TypeString(t, nil))
					return
				}
			}
			c.convertType(xt, yt, e.Y.Pos())
			c.convertType(yt, xt, e.X.Pos())
		case token.LAND, token.LOR:
		default:
			if isString(xt) && (e.Op == token.ADD || e.Op == token.LSS || e.Op == token.LEQ || e.Op == token.GTR || e.Op == token.GEQ) {
				break // concatenation and ordering of strings
			}
			if b, ok := underlying(xt).(*types.Basic); !ok || b.Info()&(types.IsInteger|types.IsFloat) == 0 {
				c.unsupported(e, "operator "+e.Op.String()+" on "+types.TypeString(xt, nil))
				return
			}
		}
	}
	c.expr(e.X)
	c.expr(e.Y)
}

// comparable reports whether == is allowed on t: int, int64, byte,
// float32, float64, bool, string (or a named type of them), actor.PID,
// fs.File, actor.Ref, an interface, an array of a comparable type, or a
// struct whose fields are all comparable (go/types has rejected slices and funcs; a Bytes field
// compares by content; a Buffer does not compare).
func (c *checker) comparable(t types.Type) bool {
	if isPID(t) || isRef(t) || isIface(t) || isPointer(t) { // a pointer by identity
		return true
	}
	if a := arrayType(t); a != nil { // element by element
		return isBytes(a.Elem()) || c.comparable(a.Elem())
	}
	if st := structType(t); st != nil {
		s := st.Underlying().(*types.Struct)
		for i := 0; i < s.NumFields(); i++ {
			if ft := s.Field(i).Type(); !isBytes(ft) && !c.comparable(ft) {
				return false
			}
		}
		return true
	}
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&(types.IsInteger|types.IsFloat|types.IsBoolean|types.IsString) != 0
}

func (c *checker) errList() []error {
	out := make([]error, len(c.errs))
	for i, e := range c.errs {
		out[i] = e.err
	}
	return out
}

// isString reports whether t's underlying type is string (or untyped string).
func isString(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&types.IsString != 0
}

// indexArg checks an index or slice bound: an integer of at most 32
// bits or an untyped integer constant (an int64 or uint64 may not fit the
// VM's int index; a uint32 from 2^31 is a negative int there, out of
// range as in Go).
func (c *checker) indexArg(e ast.Expr) {
	switch t := c.p.Info.TypeOf(e); {
	case intKindOf(t).bits != 0 && intKindOf(t).bits <= 32:
		c.expr(e)
	default:
		c.unsupported(e, "index of type "+types.TypeString(c.p.Info.TypeOf(e), nil))
	}
}

// indexExpr checks s[i] on a string (a byte), a slice or an array.
func (c *checker) indexExpr(e *ast.IndexExpr) {
	if xt := c.p.Info.TypeOf(e.X); !isString(xt) && !isSlice(xt) && !isArray(xt) {
		c.unsupported(e, "index of "+types.TypeString(xt, nil))
		return
	}
	c.expr(e.X)
	c.indexArg(e.Index)
}

// sliceExpr checks s[a:b], s[a:] and s[:b] on a string, a slice or an
// array variable.
func (c *checker) sliceExpr(e *ast.SliceExpr) {
	if isArray(c.p.Info.TypeOf(e.X)) && !c.arraySlice(e) {
		return
	}
	if xt := c.p.Info.TypeOf(e.X); !isString(xt) && !isSlice(xt) && !isArray(xt) {
		c.unsupported(e, "slice of "+types.TypeString(xt, nil))
		return
	}
	if e.Slice3 {
		c.unsupported(e, "3-index slice")
		return
	}
	c.expr(e.X)
	for _, b := range []ast.Expr{e.Low, e.High} {
		if b != nil {
			c.indexArg(b)
		}
	}
}

// rangeStmt checks `for i, c := range s` (also with one variable, blanks, or
// none) over a string or a slice, and `for i := range n` over an integer. A
// string ranges over bytes, not runes: i is the byte index and c the byte
// value (typed rune, an int32, as in Go; gogo's rune is an int).
func (c *checker) rangeStmt(s *ast.RangeStmt) {
	if xt := c.p.Info.TypeOf(s.X); !isString(xt) && !isSlice(xt) && !isArray(xt) && !isInteger(xt) {
		c.unsupported(s, "range over "+types.TypeString(c.p.Info.TypeOf(s.X), nil))
		return
	}
	if s.Tok == token.ASSIGN {
		c.unsupported(s, "range assigning to existing variables")
		return
	}
	for _, v := range []ast.Expr{s.Key, s.Value} {
		if v == nil {
			continue
		}
		if _, ok := v.(*ast.Ident); !ok {
			c.unsupported(v, kindName(v)+" as range variable")
			return
		}
	}
	c.expr(s.X)
	c.block(s.Body)
}

// isInteger reports whether t's underlying type is an integer type (or an
// untyped integer constant): `for i := range n`.
func isInteger(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&types.IsInteger != 0
}

// isSlice reports whether t's underlying type is a slice.
func isSlice(t types.Type) bool {
	_, ok := underlying(t).(*types.Slice)
	return ok
}

// sliceElem is the element type of slice type t, or nil.
func sliceElem(t types.Type) types.Type {
	if sl, ok := underlying(t).(*types.Slice); ok {
		return sl.Elem()
	}
	return nil
}

// isByteSlice reports whether t is a slice of bytes.
func isByteSlice(t types.Type) bool { return isByte(sliceElem(t)) }

// nilSlice admits e if it is nil going into a slice-typed or
// interface-typed destination: the nil slice, the nil interface value.
func (c *checker) nilSlice(e ast.Expr) {
	if id, ok := ast.Unparen(e).(*ast.Ident); ok {
		if _, isNil := c.p.Info.Uses[id].(*types.Nil); isNil {
			c.okNil[id] = true
		}
	}
}

// nilCompare checks s == nil, nil == s and the != forms for a slice s (the
// only comparison of slices) or an interface value s. False if e is not
// one.
func (c *checker) nilCompare(e *ast.BinaryExpr) bool {
	isNil := func(x ast.Expr) bool {
		id, ok := ast.Unparen(x).(*ast.Ident)
		if !ok {
			return false
		}
		_, n := c.p.Info.Uses[id].(*types.Nil)
		return n
	}
	x, y := e.X, e.Y
	if isNil(x) {
		x, y = y, x
	}
	if !isNil(y) || (!isSlice(c.p.Info.TypeOf(x)) && !isIface(c.p.Info.TypeOf(x)) && !isPointer(c.p.Info.TypeOf(x))) {
		return false
	}
	c.nilSlice(y)
	c.expr(x)
	c.expr(y)
	return true
}

// isBuiltin reports whether call calls the predeclared function name.
func (c *checker) isBuiltin(call *ast.CallExpr, name string) bool {
	id, ok := ast.Unparen(call.Fun).(*ast.Ident)
	if !ok {
		return false
	}
	b, ok := c.p.Info.Uses[id].(*types.Builtin)
	return ok && b.Name() == name
}

// isBytesConv reports whether call is string(b) of a []byte b or []byte(s)
// of a string s (both copy).
func (c *checker) isBytesConv(call *ast.CallExpr) bool {
	if len(call.Args) != 1 {
		return false
	}
	to, from := c.p.Info.TypeOf(call.Fun), c.p.Info.TypeOf(call.Args[0])
	if basicKind(to) == types.String {
		return isByteSlice(from)
	}
	_, unnamed := types.Unalias(to).(*types.Slice)
	return unnamed && isByteSlice(to) && isString(from)
}

// seqBuiltin checks len, cap, append, make and copy.
func (c *checker) seqBuiltin(call *ast.CallExpr, name string) {
	argType := func(i int) types.Type { return c.p.Info.TypeOf(call.Args[i]) }
	switch name {
	case "len":
		if len(call.Args) != 1 || (!isString(argType(0)) && !isSlice(argType(0)) && !isArray(argType(0))) {
			c.unsupported(call, "len of anything but a string, a slice or an array")
			return
		}
		c.expr(call.Args[0])
	case "cap":
		if len(call.Args) != 1 || (!isSlice(argType(0)) && !isArray(argType(0))) {
			c.unsupported(call, "cap of anything but a slice or an array")
			return
		}
		c.expr(call.Args[0])
	case "append":
		st := argType(0)
		c.convert(st, call.Args[0])
		c.expr(call.Args[0])
		if call.Ellipsis.IsValid() {
			if len(call.Args) != 2 {
				c.unsupported(call, "append with ... and more than one value")
				return
			}
			c.convert(st, call.Args[1])
			c.expr(call.Args[1])
			return
		}
		for _, a := range call.Args[1:] {
			c.convert(sliceElem(st), a)
			c.expr(a)
		}
	case "make":
		if !isSlice(argType(0)) {
			c.unsupported(call, "make of anything but a slice")
			return
		}
		c.typeExpr(call.Args[0])
		for _, a := range call.Args[1:] {
			c.indexArg(a)
		}
	case "copy":
		c.expr(call.Args[0])
		c.expr(call.Args[1])
	}
}

// sliceLit checks a slice literal []T{a, b, ...}: positional elements only;
// element literals may omit their type.
func (c *checker) sliceLit(e *ast.CompositeLit, sl *types.Slice) {
	for _, el := range e.Elts {
		if _, ok := el.(*ast.KeyValueExpr); ok {
			c.unsupported(el, "keyed slice element")
			continue
		}
		c.convert(sl.Elem(), el)
		if lit, ok := el.(*ast.CompositeLit); ok && lit.Type == nil {
			c.compositeLitIn(lit, true)
			continue
		}
		c.expr(el)
	}
}

// stubPromoted reports (and rejects) x.M where M is a method of a gogo
// package type (bytes.Buffer, bytes.Bytes, fs.File) promoted from an
// embedded field: those methods are builtins compiled for a receiver of
// their own type only.
func (c *checker) stubPromoted(sel *ast.SelectorExpr) bool {
	s := c.p.Info.Selections[sel]
	if s == nil || s.Kind() != types.MethodVal || len(s.Index()) < 2 {
		return false
	}
	fn, ok := s.Obj().(*types.Func)
	if !ok || fn.Pkg() == nil || !c.p.isBuiltin(fn) {
		return false
	}
	c.fail(sel.Pos(), "method %s of an embedded %s type not supported: call it on the field", fn.Name(), fn.Pkg().Path())
	return true
}

// holdsUnsigned reports whether t is a struct, slice or array type with a
// uint32, uint or uint64 inside (at any depth): println prints such a
// value's fields and elements without their static types, so these would
// print as their signed bit patterns. (A bare one is printed unsigned:
// printlnUnsigned.)
func holdsUnsigned(t types.Type, seen map[types.Type]bool) bool {
	if seen[t] {
		return false
	}
	if seen == nil {
		seen = map[types.Type]bool{}
	}
	seen[t] = true
	switch u := t.Underlying().(type) {
	case *types.Struct:
		for i := 0; i < u.NumFields(); i++ {
			ft := u.Field(i).Type()
			if k := intKindOf(ft); k.unsigned && k.bits >= 32 || holdsUnsigned(ft, seen) {
				return true
			}
		}
	case *types.Slice:
		if k := intKindOf(u.Elem()); k.unsigned && k.bits >= 32 || holdsUnsigned(u.Elem(), seen) {
			return true
		}
	case *types.Array:
		if k := intKindOf(u.Elem()); k.unsigned && k.bits >= 32 || holdsUnsigned(u.Elem(), seen) {
			return true
		}
	}
	return false
}
