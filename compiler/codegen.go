package compiler

import (
	"errors"
	"fmt"
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
)

// maxRegs is the register limit of one function: register numbers are 8 bits.
const maxRegs = 255

// Compile checks p against the gogo subset and compiles it to an Image v1.
func Compile(p *Program) ([]byte, error) {
	if errs := Check(p); len(errs) > 0 {
		return nil, errors.Join(errs...)
	}
	g := &gen{
		p:       p,
		b:       NewImageBuilder(),
		funcIdx: map[*types.Func]uint32{},
		typeIdx: map[*types.TypeName]uint32{},
	}
	if err := g.collectTypes(); err != nil {
		return nil, err
	}

	// Index every top-level function of every package first so calls to
	// later ones resolve.
	var decls []*ast.FuncDecl
	for _, pkg := range p.packages() {
		for _, f := range pkg.Files {
			for _, d := range f.Decls {
				if fd, ok := d.(*ast.FuncDecl); ok {
					fn := p.Info.Defs[fd.Name].(*types.Func)
					g.funcIdx[fn] = uint32(len(g.funcs))
					g.funcs = append(g.funcs, &funcInfo{name: g.qualified(fn.Pkg(), fd.Name.Name)})
					decls = append(decls, fd)
				}
			}
		}
	}
	g.nameLits(decls)
	var errs []error
	for _, fd := range decls {
		fn := p.Info.Defs[fd.Name].(*types.Func)
		if err := g.compileFunc(g.funcs[g.funcIdx[fn]], g.declParams(fd.Type), fd.Body); err != nil {
			errs = append(errs, err)
		}
	}
	// Func literals, queued as they are compiled; compiling one may queue
	// more (nested literals).
	for len(g.pending) > 0 {
		c := g.pending[0]
		g.pending = g.pending[1:]
		if err := g.compileFunc(c.info, c.captures, c.lit.Body); err != nil {
			errs = append(errs, err)
		}
	}
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}

	for i, f := range g.funcs {
		idx := g.b.AddFunc(f.name, f.nParams, f.nRegs)
		if idx != uint32(i) {
			return nil, fmt.Errorf("gogo: internal error: function %s got index %d, want %d", f.name, idx, i)
		}
		g.b.SetFuncCode(idx, f.code)
	}
	g.b.SetEntry(g.funcIdx[p.Pkg.Scope().Lookup("main").(*types.Func)])
	return g.b.Bytes()
}

// funcInfo is one function of the image. Its index is its position in
// gen.funcs; code and register counts are filled in once compiled.
type funcInfo struct {
	name           string
	nParams, nRegs uint16
	code           []uint32
}

// gen holds the package-wide codegen state.
type gen struct {
	p       *Program
	b       *ImageBuilder
	funcs   []*funcInfo
	funcIdx map[*types.Func]uint32
	typeIdx map[*types.TypeName]uint32

	litNames map[*ast.FuncLit]string // hidden function name of each func literal
	pending  []closure               // closures still to compile
}

// qualified names a function or type of package pkg in the image: plain
// for package main, pkg.Name otherwise.
func (g *gen) qualified(pkg *types.Package, name string) string {
	if pkg == g.p.Pkg {
		return name
	}
	return pkg.Name() + "." + name
}

// structType returns the named struct type behind t, or nil. actor.PID and
// actor.Ref are structs in the stubs but a tagged term and an opaque heap
// object in the VM, so they are not.
func structType(t types.Type) *types.Named {
	n, ok := types.Unalias(t).(*types.Named)
	if !ok {
		return nil
	}
	if _, ok := n.Underlying().(*types.Struct); !ok {
		return nil
	}
	if isPID(n) || isRef(n) {
		return nil
	}
	return n
}

// isActorType reports whether t is the gogo/actor type called name.
func isActorType(t types.Type, name string) bool {
	n, ok := types.Unalias(t).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == ActorPath && n.Obj().Name() == name
}

func isPID(t types.Type) bool { return isActorType(t, "PID") }

func isRef(t types.Type) bool { return isActorType(t, "Ref") }

// reservedTypes are the gogo/actor struct types. Every image starts its
// type table with them, in this order, so the VM can build these structs by
// fixed index (TYPE_* in vm/opcodes.h).
var reservedTypes = []struct {
	name   string
	fields []string
}{
	{"ProcInfo", []string{"Memory"}},
	{"Reason", []string{"Code", "Func"}},
	{"ExitMsg", []string{"From", "Reason"}},
	{"DownMsg", []string{"Ref", "From", "Reason"}},
	{"Request", []string{"From", "Ref", "Body"}},
	{"Options", []string{"MaxMemory"}},
}

// collectTypes emits the reserved actor types first (whether used or not),
// then gives the other struct types image type indices in order of first
// appearance in the package sources (libraries, then main; declarations
// and uses alike).
func (g *gen) collectTypes() error {
	var actorPkg *types.Package // whichever package imports it
	for _, pkg := range g.p.packages() {
		for _, imp := range pkg.Pkg.Imports() {
			if imp.Path() == ActorPath {
				actorPkg = imp
			}
		}
	}
	for _, rt := range reservedTypes {
		idx := g.b.AddType(rt.name, uint32(len(rt.fields)))
		if actorPkg == nil {
			continue
		}
		tn, ok := actorPkg.Scope().Lookup(rt.name).(*types.TypeName)
		if !ok {
			return fmt.Errorf("gogo: internal error: actor.%s missing", rt.name)
		}
		if st, ok := tn.Type().Underlying().(*types.Struct); !ok || st.NumFields() != len(rt.fields) {
			return fmt.Errorf("gogo: internal error: actor.%s does not match the reserved layout", rt.name)
		}
		g.typeIdx[tn] = idx
	}
	add := func(t types.Type) {
		n := structType(t)
		if n == nil {
			return
		}
		if _, ok := g.typeIdx[n.Obj()]; !ok {
			name := g.qualified(n.Obj().Pkg(), n.Obj().Name())
			g.typeIdx[n.Obj()] = g.b.AddType(name, uint32(n.Underlying().(*types.Struct).NumFields()))
		}
	}
	for _, pkg := range g.p.packages() {
		for _, f := range pkg.Files {
			ast.Inspect(f, func(node ast.Node) bool {
				if id, ok := node.(*ast.Ident); ok {
					if tn, ok := g.p.Info.ObjectOf(id).(*types.TypeName); ok {
						add(tn.Type())
					}
				}
				if e, ok := node.(ast.Expr); ok {
					if tv, ok := g.p.Info.Types[e]; ok {
						add(tv.Type)
					}
				}
				return true
			})
		}
	}
	return nil
}

// genError aborts the compilation of one function; compileFunc recovers it.
type genError struct{ err error }

// fn holds the state of the function being compiled.
//
// Registers: parameters are r0..r(n-1); each local gets the next register at
// its declaration and keeps it until its block ends; expression temporaries
// are allocated above the locals and released as soon as the expression that
// needed them has been emitted. nRegs is the high-water mark.
type fn struct {
	g      *gen
	info   *funcInfo
	asm    Asm
	locals map[*types.Var]uint8
	next   int // next free register
	nRegs  int

	breakTo    []Label // innermost last
	continueTo []Label
}

// declParams lists the parameters of a declared function; unnamed ones are
// nil (they get a register but no variable).
func (g *gen) declParams(ft *ast.FuncType) []*types.Var {
	var params []*types.Var
	for _, field := range ft.Params.List {
		if len(field.Names) == 0 {
			params = append(params, nil)
			continue
		}
		for _, name := range field.Names {
			v, _ := g.p.Info.Defs[name].(*types.Var)
			params = append(params, v)
		}
	}
	return params
}

// compileFunc compiles body into info; params get r0..r(n-1).
func (g *gen) compileFunc(info *funcInfo, params []*types.Var, body *ast.BlockStmt) (err error) {
	f := &fn{g: g, info: info, locals: map[*types.Var]uint8{}}
	defer func() {
		if r := recover(); r != nil {
			ge, ok := r.(genError)
			if !ok {
				err = fmt.Errorf("%s: internal compiler error: %v", g.p.Fset.Position(body.Pos()), r)
				return
			}
			err = ge.err
		}
	}()
	for _, v := range params {
		r := f.alloc(body)
		if v != nil {
			f.locals[v] = r
		}
	}
	info.nParams = uint16(f.next)
	f.stmts(body.List)
	if n := len(body.List); n == 0 || !isReturn(body.List[n-1]) {
		f.retNil()
	}
	code, aerr := f.asm.Finish()
	if aerr != nil {
		return fmt.Errorf("gogo: internal error in %s: %v", info.name, aerr)
	}
	info.code = code
	info.nRegs = uint16(f.nRegs)
	return nil
}

func isReturn(s ast.Stmt) bool {
	_, ok := s.(*ast.ReturnStmt)
	return ok
}

func (f *fn) fail(n ast.Node, format string, args ...any) {
	pos := f.g.p.Fset.Position(n.Pos())
	panic(genError{fmt.Errorf("%s: gogo: %s", pos, fmt.Sprintf(format, args...))})
}

func (f *fn) notYet(n ast.Node, what string) { f.fail(n, "not yet implemented: %s", what) }

// alloc reserves the next register; n is the node blamed if none is left.
func (f *fn) alloc(n ast.Node) uint8 {
	if f.next >= maxRegs {
		f.fail(n, "function %s needs more than %d registers", f.info.name, maxRegs)
	}
	r := uint8(f.next)
	f.next++
	f.nRegs = max(f.nRegs, f.next)
	return r
}

// allocN reserves n consecutive registers and returns the first.
func (f *fn) allocN(node ast.Node, n int) uint8 {
	base := uint8(f.next) // next <= maxRegs, so it fits
	for i := 0; i < n; i++ {
		f.alloc(node)
	}
	return base
}

func (f *fn) retNil() {
	mark := f.next
	r := f.alloc(nil)
	f.asm.EmitExt(OpLoadk, r, 0, 0, TermNil)
	f.asm.Emit(OpRet, r, 0, 0)
	f.next = mark
}

// ---- statements ----

func (f *fn) stmts(list []ast.Stmt) {
	for _, s := range list {
		f.stmt(s)
	}
}

// block compiles statements in a new scope: its locals die at its end.
func (f *fn) block(list []ast.Stmt) {
	mark := f.next
	f.stmts(list)
	f.next = mark
}

func (f *fn) stmt(s ast.Stmt) {
	switch s := s.(type) {
	case *ast.BlockStmt:
		f.block(s.List)
	case *ast.EmptyStmt:
	case *ast.ExprStmt:
		mark := f.next
		f.exprInto(s.X, f.alloc(s))
		f.next = mark
	case *ast.AssignStmt:
		f.assign(s)
	case *ast.IncDecStmt:
		op := OpAdd
		if s.Tok == token.DEC {
			op = OpSub
		}
		x := f.localOf(s.X)
		mark := f.next
		one := f.alloc(s)
		if isInt64(f.g.p.Info.TypeOf(s.X)) {
			f.int64Into(1, one)
		} else {
			f.asm.EmitExt(OpLoadk, one, 0, 0, TermInt(1))
		}
		f.asm.Emit(op, x, x, one)
		f.next = mark
	case *ast.DeclStmt:
		f.declStmt(s.Decl.(*ast.GenDecl))
	case *ast.ReturnStmt:
		if len(s.Results) == 0 {
			f.retNil()
			return
		}
		mark := f.next
		f.asm.Emit(OpRet, f.operand(s.Results[0]), 0, 0)
		f.next = mark
	case *ast.IfStmt:
		f.ifStmt(s)
	case *ast.ForStmt:
		f.forStmt(s)
	case *ast.BranchStmt:
		targets := f.breakTo
		if s.Tok == token.CONTINUE {
			targets = f.continueTo
		}
		if len(targets) == 0 {
			f.fail(s, "%s outside a loop", s.Tok)
		}
		f.asm.EmitJump(OpJmp, 0, targets[len(targets)-1])
	case *ast.TypeSwitchStmt:
		f.typeSwitch(s)
	default:
		f.fail(s, "internal error: unexpected statement %T", s)
	}
}

// localOf returns the register of the local variable e names.
func (f *fn) localOf(e ast.Expr) uint8 {
	id, ok := ast.Unparen(e).(*ast.Ident)
	if !ok {
		f.fail(e, "internal error: assignment target is not a variable")
	}
	v, ok := f.g.p.Info.ObjectOf(id).(*types.Var)
	r, found := f.locals[v]
	if !ok || !found {
		f.fail(e, "internal error: unknown variable %s", id.Name)
	}
	return r
}

var assignOps = map[token.Token]uint8{
	token.ADD_ASSIGN: OpAdd, token.SUB_ASSIGN: OpSub, token.MUL_ASSIGN: OpMul,
	token.QUO_ASSIGN: OpDiv, token.REM_ASSIGN: OpRem,
}

func (f *fn) assign(s *ast.AssignStmt) {
	if op, ok := assignOps[s.Tok]; ok { // x op= e
		x := f.localOf(s.Lhs[0])
		mark := f.next
		f.asm.Emit(op, x, x, f.operand(s.Rhs[0]))
		f.next = mark
		return
	}

	// Registers of the targets; -1 discards (blank identifier). Variables
	// declared here get their register before the right-hand sides are
	// evaluated, so the temporaries sit above them.
	targets := make([]int, len(s.Lhs))
	var newVars []*types.Var
	var newRegs []uint8
	for i, l := range s.Lhs {
		id, _ := ast.Unparen(l).(*ast.Ident)
		switch {
		case id != nil && id.Name == "_":
			targets[i] = -1
		case s.Tok == token.DEFINE && id != nil && f.g.p.Info.Defs[id] != nil:
			r := f.alloc(id)
			newVars = append(newVars, f.g.p.Info.Defs[id].(*types.Var))
			newRegs = append(newRegs, r)
			targets[i] = int(r)
		default:
			targets[i] = int(f.localOf(l))
		}
	}
	mark := f.next
	if len(s.Lhs) == 2 && len(s.Rhs) == 1 {
		// a, b := actor.X(...): the builtin writes two fresh temporaries,
		// above every live local, which then move to the targets.
		call, ok := ast.Unparen(s.Rhs[0]).(*ast.CallExpr)
		if !ok {
			f.fail(s, "internal error: two-value assignment from a non-call")
		}
		base := f.allocN(s, 2)
		f.twoResultCall(call, base)
		for i, t := range targets {
			if t >= 0 {
				f.asm.Emit(OpMove, uint8(t), base+uint8(i), 0)
			}
		}
	} else if len(s.Lhs) == 1 && len(newVars) == 1 {
		// A fresh variable cannot be read by its own initializer.
		f.exprInto(s.Rhs[0], uint8(targets[0]))
	} else {
		// Evaluate every right-hand side before assigning any (a, b = b, a).
		base := f.allocN(s, len(s.Rhs))
		for i, r := range s.Rhs {
			f.exprInto(r, base+uint8(i))
		}
		for i, t := range targets {
			if t >= 0 {
				f.asm.Emit(OpMove, uint8(t), base+uint8(i), 0)
			}
		}
	}
	f.next = mark
	for i, v := range newVars {
		f.locals[v] = newRegs[i]
	}
}

func (f *fn) declStmt(d *ast.GenDecl) {
	if d.Tok != token.VAR {
		return // constants are folded at their uses
	}
	for _, spec := range d.Specs {
		vs := spec.(*ast.ValueSpec)
		var vars []*types.Var
		var regs []uint8
		for _, name := range vs.Names {
			v, _ := f.g.p.Info.Defs[name].(*types.Var)
			vars = append(vars, v)
			regs = append(regs, f.alloc(name))
		}
		mark := f.next
		for i, name := range vs.Names {
			if len(vs.Values) > 0 {
				f.exprInto(vs.Values[i], regs[i])
			} else {
				f.zeroInto(name, vars[i].Type(), regs[i])
			}
		}
		f.next = mark
		for i, v := range vars {
			if v != nil && v.Name() != "_" {
				f.locals[v] = regs[i]
			}
		}
	}
}

func (f *fn) ifStmt(s *ast.IfStmt) {
	scope := f.next
	if s.Init != nil {
		f.stmt(s.Init)
	}
	elseL := f.asm.NewLabel()
	mark := f.next
	f.asm.EmitJump(OpJmpf, f.operand(s.Cond), elseL)
	f.next = mark
	f.block(s.Body.List)
	if s.Else == nil {
		f.asm.Bind(elseL)
	} else {
		endL := f.asm.NewLabel()
		f.asm.EmitJump(OpJmp, 0, endL)
		f.asm.Bind(elseL)
		f.stmt(s.Else)
		f.asm.Bind(endL)
	}
	f.next = scope
}

func (f *fn) forStmt(s *ast.ForStmt) {
	scope := f.next
	if s.Init != nil {
		f.stmt(s.Init)
	}
	topL, contL, endL := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.asm.Bind(topL)
	if s.Cond != nil {
		mark := f.next
		f.asm.EmitJump(OpJmpf, f.operand(s.Cond), endL)
		f.next = mark
	}
	f.breakTo = append(f.breakTo, endL)
	f.continueTo = append(f.continueTo, contL)
	f.block(s.Body.List)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
	f.continueTo = f.continueTo[:len(f.continueTo)-1]
	f.asm.Bind(contL)
	if s.Post != nil {
		f.stmt(s.Post)
	}
	f.asm.EmitJump(OpJmp, 0, topL)
	f.asm.Bind(endL)
	f.next = scope
}

// ---- expressions ----

// operand returns a register holding e's value: a local's own register, or
// a fresh temporary that the caller releases.
func (f *fn) operand(e ast.Expr) uint8 {
	if id, ok := ast.Unparen(e).(*ast.Ident); ok {
		if v, ok := f.g.p.Info.Uses[id].(*types.Var); ok {
			if r, ok := f.locals[v]; ok {
				return r
			}
		}
	}
	r := f.alloc(e)
	f.exprInto(e, r)
	return r
}

var binaryOps = map[token.Token]uint8{
	token.ADD: OpAdd, token.SUB: OpSub, token.MUL: OpMul, token.QUO: OpDiv, token.REM: OpRem,
	token.EQL: OpEq, token.NEQ: OpNe, token.LSS: OpLt, token.LEQ: OpLe,
}

// exprInto compiles e so that its value ends up in dst. Temporaries it
// allocates are released before it returns.
func (f *fn) exprInto(e ast.Expr, dst uint8) {
	if tv := f.g.p.Info.Types[e]; tv.Value != nil {
		f.constInto(e, tv.Value, dst)
		return
	}
	mark := f.next
	defer func() { f.next = mark }()

	switch e := e.(type) {
	case *ast.ParenExpr:
		f.exprInto(e.X, dst)
	case *ast.Ident:
		if fn, ok := f.g.p.Info.Uses[e].(*types.Func); ok {
			f.funcValueInto(e, fn, dst)
			return
		}
		if r := f.localOf(e); r != dst {
			f.asm.Emit(OpMove, dst, r, 0)
		}
	case *ast.UnaryExpr:
		op := OpNeg
		if e.Op == token.NOT {
			op = OpNot
		}
		f.asm.Emit(op, dst, f.operand(e.X), 0)
	case *ast.BinaryExpr:
		f.binaryInto(e, dst)
	case *ast.SelectorExpr:
		if fn, ok := f.g.p.Info.Uses[e.Sel].(*types.Func); ok {
			f.funcValueInto(e, fn, dst) // lib.F
			return
		}
		sel := f.g.p.Info.Selections[e]
		if sel == nil || sel.Kind() != types.FieldVal || len(sel.Index()) != 1 {
			f.fail(e, "internal error: unexpected selector")
		}
		f.asm.Emit(OpGetfield, dst, f.operand(e.X), uint8(sel.Index()[0]))
	case *ast.CompositeLit:
		f.structLit(e, dst)
	case *ast.FuncLit:
		f.closureInto(e, dst)
	case *ast.CallExpr:
		f.call(e, dst)
	default:
		f.fail(e, "internal error: unexpected expression %T", e)
	}
}

func (f *fn) binaryInto(e *ast.BinaryExpr, dst uint8) {
	switch e.Op {
	case token.LAND, token.LOR: // short circuit: skip Y when X decides
		jump := OpJmpf
		if e.Op == token.LOR {
			jump = OpJmpt
		}
		endL := f.asm.NewLabel()
		f.exprInto(e.X, dst)
		f.asm.EmitJump(jump, dst, endL)
		f.exprInto(e.Y, dst)
		f.asm.Bind(endL)
	case token.GTR: // x > y  ==  y < x
		x, y := f.operand(e.X), f.operand(e.Y)
		f.asm.Emit(OpLt, dst, y, x)
	case token.GEQ: // x >= y  ==  y <= x
		x, y := f.operand(e.X), f.operand(e.Y)
		f.asm.Emit(OpLe, dst, y, x)
	default:
		op, ok := binaryOps[e.Op]
		if !ok {
			f.fail(e, "internal error: unexpected operator %s", e.Op)
		}
		x, y := f.operand(e.X), f.operand(e.Y)
		f.asm.Emit(op, dst, x, y)
	}
}

// constInto loads the constant v (of e's type) into dst.
func (f *fn) constInto(e ast.Expr, v constant.Value, dst uint8) {
	switch v.Kind() {
	case constant.Bool:
		t := TermFalse
		if constant.BoolVal(v) {
			t = TermTrue
		}
		f.asm.EmitExt(OpLoadk, dst, 0, 0, t)
	case constant.String:
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.b.AddString(constant.StringVal(v))))
	case constant.Int:
		n, exact := constant.Int64Val(v)
		if isInt64(f.g.p.Info.TypeOf(e)) {
			if !exact {
				f.fail(e, "constant %s overflows int64", v)
			}
			f.int64Into(n, dst)
			return
		}
		if !exact || n < -1<<31 || n >= 1<<31 {
			f.fail(e, "constant %s overflows int", v)
		}
		f.intInto(int32(n), dst)
	default:
		f.fail(e, "constant %s not supported", v)
	}
}

func (f *fn) intInto(n int32, dst uint8) {
	if Int31Fits(n) {
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermInt(n))
	} else {
		f.asm.EmitExt(OpLoadi32, dst, 0, 0, uint32(n))
	}
}

// int64Into loads the int64 n into dst: int64 values are always boxed, so
// it is a const-area K_INT64 object.
func (f *fn) int64Into(n int64, dst uint8) {
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.b.AddInt64(n)))
}

// isInt64 reports whether t's underlying type is int64.
func isInt64(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Kind() == types.Int64
}

// conversion compiles int(x) or int64(x) (the only conversions the checker
// allows) into dst; converting to x's own type is a plain copy.
func (f *fn) conversion(call *ast.CallExpr, dst uint8) {
	to, from := f.g.p.Info.TypeOf(call.Fun), f.g.p.Info.TypeOf(call.Args[0])
	if isInt64(to) == isInt64(from) {
		f.exprInto(call.Args[0], dst)
		return
	}
	c := uint8(0)
	if isInt64(to) {
		c = 1
	}
	mark := f.next
	f.asm.Emit(OpConv, dst, f.operand(call.Args[0]), c)
	f.next = mark
}

// zeroInto loads the zero value of t into dst.
func (f *fn) zeroInto(n ast.Node, t types.Type, dst uint8) {
	if st := structType(t); st != nil {
		f.newStruct(n, st, nil, dst)
		return
	}
	if isPID(t) || isRef(t) || isAny(t) || isFunc(t) {
		// No actor has the zero PID and no ref has serial 0; a nil any; a
		// nil func, which Spawn rejects (the spawner crashes with badcode).
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNil)
		return
	}
	b, ok := t.Underlying().(*types.Basic)
	switch {
	case ok && b.Kind() == types.Int64:
		f.int64Into(0, dst)
	case ok && b.Info()&types.IsInteger != 0:
		f.intInto(0, dst)
	case ok && b.Info()&types.IsBoolean != 0:
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermFalse)
	case ok && b.Info()&types.IsString != 0:
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermEmptyStr)
	default:
		f.fail(n, "zero value of %s not supported", t)
	}
}

func (f *fn) structLit(e *ast.CompositeLit, dst uint8) {
	st := structType(f.g.p.Info.TypeOf(e))
	if st == nil {
		f.fail(e, "internal error: composite literal of non-struct type")
	}
	fields := st.Underlying().(*types.Struct)
	var vals []fieldVal
	for i, el := range e.Elts {
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			name := kv.Key.(*ast.Ident).Name
			for j := 0; j < fields.NumFields(); j++ {
				if fields.Field(j).Name() == name {
					vals = append(vals, fieldVal{j, kv.Value})
				}
			}
		} else {
			vals = append(vals, fieldVal{i, el})
		}
	}
	f.newStruct(e, st, vals, dst)
}

// fieldVal is a struct literal value and the index of the field it sets.
type fieldVal struct {
	field int
	val   ast.Expr
}

// newStruct builds a struct of type st into dst. The values are evaluated in
// the order given (source order, as Go requires); fields without a value get
// their zero value.
func (f *fn) newStruct(n ast.Node, st *types.Named, vals []fieldVal, dst uint8) {
	fields := st.Underlying().(*types.Struct)
	nf := fields.NumFields()
	if nf > maxRegs {
		f.fail(n, "struct %s has too many fields", st.Obj().Name())
	}
	idx, ok := f.g.typeIdx[st.Obj()]
	if !ok {
		f.fail(n, "internal error: struct type %s has no index", st.Obj().Name())
	}
	mark := f.next
	base := f.allocN(n, nf)
	set := make([]bool, nf)
	for _, v := range vals {
		f.exprInto(v.val, base+uint8(v.field))
		set[v.field] = true
	}
	for i := 0; i < nf; i++ {
		if !set[i] {
			f.zeroInto(n, fields.Field(i).Type(), base+uint8(i))
		}
	}
	f.asm.EmitExt(OpNewstruct, dst, base, uint8(nf), idx)
	f.next = mark
}

// args evaluates call arguments into consecutive fresh registers.
func (f *fn) args(call *ast.CallExpr) (base uint8, n uint8) {
	if len(call.Args) > maxRegs {
		f.fail(call, "too many arguments")
	}
	base = f.allocN(call, len(call.Args))
	for i, a := range call.Args {
		f.exprInto(a, base+uint8(i))
	}
	return base, uint8(len(call.Args))
}

func (f *fn) call(call *ast.CallExpr, dst uint8) {
	if tv := f.g.p.Info.Types[call.Fun]; tv.IsType() {
		f.conversion(call, dst)
		return
	}
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		switch obj := f.g.p.Info.Uses[fun].(type) {
		case *types.Builtin: // println, the only builtin the checker allows
			base, n := f.args(call)
			f.asm.EmitExt(OpBuiltin, dst, base, n, BPrintln)
			return
		case *types.Func:
			idx, ok := f.g.funcIdx[obj]
			if !ok {
				f.fail(call, "internal error: unknown function %s", obj.Name())
			}
			base, n := f.args(call)
			f.asm.EmitExt(OpCall, dst, base, n, idx)
			return
		}
	case *ast.SelectorExpr:
		obj, ok := f.g.p.Info.Uses[fun.Sel].(*types.Func)
		if ok && obj.Pkg() != nil && obj.Pkg().Path() == ActorPath {
			f.actorCall(call, obj.Name(), dst)
			return
		}
		if idx, found := f.g.funcIdx[obj]; ok && found { // lib.F(...)
			base, n := f.args(call)
			f.asm.EmitExt(OpCall, dst, base, n, idx)
			return
		}
	}
	f.fail(call, "internal error: unexpected call")
}
