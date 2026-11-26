package compiler

import (
	"fmt"
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
	"sort"
	"strings"
)

// Package-level variables (15.6, D2 of the language plan) are read-only
// and statically initialized: the compiler evaluates each initializer to
// a term at compile time and puts what it builds (structs, slices and
// their arrays, int64 and float boxes) in the image's constant area, in
// flash. A use of the variable loads that term (LOADK); nothing runs at
// start-up and no actor holds a copy. The checker rejects every write to
// one (checkGlobalWrite), and a write through an alias of a constant
// array (t := table; t[0] = 1) ends the actor with badcode: the VM
// refuses to write a constant array. An initializer may use constants,
// composite literals, conversions, other package-level variables and
// calls of functions whose body is a single return statement (such as
// errors.New), which are evaluated with their arguments bound
// (staticProblem lists what else is rejected).

// globalVar returns the package-level variable e names (an identifier, or
// pkg.V), or nil.
func globalVar(info *types.Info, e ast.Expr) *types.Var {
	var id *ast.Ident
	switch x := ast.Unparen(e).(type) {
	case *ast.Ident:
		id = x
	case *ast.SelectorExpr:
		if info.Selections[x] != nil {
			return nil
		}
		id = x.Sel
	default:
		return nil
	}
	v, ok := info.Uses[id].(*types.Var)
	if !ok || v.IsField() || v.Pkg() == nil || v.Parent() != v.Pkg().Scope() {
		return nil
	}
	return v
}

// globalInit returns the initializer of package-level variable v (nil for
// none: the zero value).
func (p *Program) globalInit(v *types.Var) ast.Expr {
	for _, pkg := range p.packages() {
		if pkg.Pkg != v.Pkg() {
			continue
		}
		for _, f := range pkg.Files {
			for _, d := range f.Decls {
				gd, ok := d.(*ast.GenDecl)
				if !ok || gd.Tok != token.VAR {
					continue
				}
				for _, s := range gd.Specs {
					vs := s.(*ast.ValueSpec)
					for i, n := range vs.Names {
						if p.Info.Defs[n] == v && i < len(vs.Values) {
							return vs.Values[i]
						}
					}
				}
			}
		}
	}
	return nil
}

// globalTerm returns the term of package-level variable v's value,
// evaluating its initializer when first used.
func (g *gen) globalTerm(n ast.Node, v *types.Var) uint32 {
	if t, ok := g.globals[v]; ok {
		return t
	}
	if g.globalsBusy[v] {
		g.staticFail(n, "initialization cycle through %s", v.Name())
	}
	if g.globals == nil {
		g.globals, g.globalsBusy = map[*types.Var]uint32{}, map[*types.Var]bool{}
	}
	g.globalsBusy[v] = true
	var t uint32
	if init := g.p.globalInit(v); init != nil {
		t = g.staticTerm(init, v.Type(), nil)
	} else {
		t = g.zeroTerm(n, v.Type())
	}
	delete(g.globalsBusy, v)
	g.globals[v] = t
	return t
}

// staticFail aborts with an error at n (recovered as a genError by the
// function being compiled).
func (g *gen) staticFail(n ast.Node, format string, args ...any) {
	panic(genError{fmt.Errorf("%s: gogo: %s", g.p.Fset.Position(n.Pos()), fmt.Sprintf(format, args...))})
}

// globalInto loads the value of package-level variable v into dst.
func (f *fn) globalInto(n ast.Node, v *types.Var, dst uint8) {
	f.asm.EmitExt(OpLoadk, dst, 0, 0, f.g.globalTerm(n, v))
}

// staticTerm evaluates e, which goes into a destination of type t (nil:
// e's own), to a term: an immediate, or a ref into the constant area.
// env binds the parameters of a function being evaluated inline.
func (g *gen) staticTerm(e ast.Expr, t types.Type, env map[*types.Var]uint32) uint32 {
	info := g.p.Info
	et := info.TypeOf(e)
	if t == nil {
		t = et
	}
	if tv := info.Types[e]; tv.IsNil() {
		if isSlice(t) {
			return TermNilSlice
		}
		return TermNil
	}
	if tv := info.Types[e]; tv.Value != nil {
		return g.staticConst(e, tv.Value, et, t)
	}
	if isIface(t) && !isIface(et) { // a concrete value into an interface
		return g.staticBox(e, et, g.staticTerm(e, et, env))
	}
	switch x := ast.Unparen(e).(type) {
	case *ast.Ident:
		if v, ok := info.Uses[x].(*types.Var); ok {
			if term, ok := env[v]; ok {
				return term
			}
			if globalVar(info, x) != nil {
				return g.globalTerm(x, v)
			}
		}
	case *ast.SelectorExpr:
		if v := globalVar(info, x); v != nil {
			return g.globalTerm(x, v)
		}
		if s := info.Selections[x]; s != nil && s.Kind() == types.FieldVal && len(s.Index()) == 1 && !isPointer(info.TypeOf(x.X)) {
			if obj := g.staticTerm(x.X, nil, env); obj&0xF == 0xA && g.b.consts[obj>>4]&63 == KStruct {
				return g.b.consts[obj>>4+2+uint32(s.Index()[0])] // a field of a constant struct
			}
		}
	case *ast.CompositeLit:
		return g.staticLit(x, env)
	case *ast.CallExpr:
		if tv := info.Types[x.Fun]; tv.IsType() { // a conversion
			to := tv.Type
			arg := x.Args[0]
			if isSlice(to) && isString(info.TypeOf(arg)) { // []byte("...")
				if v := info.Types[arg].Value; v != nil {
					return g.b.AddTermSlice(nil, []byte(constant.StringVal(v)), true)
				}
			}
			return g.staticTerm(arg, to, env)
		}
		if fn, decl := g.staticFunc(x); fn != nil {
			sig := fn.Type().(*types.Signature)
			inner := map[*types.Var]uint32{}
			for i, a := range x.Args {
				p := sig.Params().At(i)
				inner[p] = g.staticTerm(a, p.Type(), env)
			}
			res := decl.Body.List[0].(*ast.ReturnStmt).Results[0]
			return g.staticTerm(res, sig.Results().At(0).Type(), inner)
		}
	}
	g.staticFail(e, "package-level variable initializer %s is not static", types.ExprString(e))
	return 0
}

// staticFunc returns the function call calls and its declaration if its
// body is a single return of one value (inlined by staticTerm), else nil.
func (g *gen) staticFunc(call *ast.CallExpr) (*types.Func, *ast.FuncDecl) {
	var id *ast.Ident
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		id = fun
	case *ast.SelectorExpr:
		if g.p.Info.Selections[fun] != nil {
			return nil, nil
		}
		id = fun.Sel
	default:
		return nil, nil
	}
	fn, ok := g.p.Info.Uses[id].(*types.Func)
	if !ok || g.p.isBuiltin(fn) || call.Ellipsis.IsValid() {
		return nil, nil
	}
	sig := fn.Type().(*types.Signature)
	if sig.Variadic() || sig.Results().Len() != 1 || sig.Params().Len() != len(call.Args) {
		return nil, nil
	}
	for _, pkg := range g.p.packages() {
		if pkg.Pkg != fn.Pkg() {
			continue
		}
		for _, f := range pkg.Files {
			for _, d := range f.Decls {
				if fd, ok := d.(*ast.FuncDecl); ok && g.p.Info.Defs[fd.Name] == fn && fd.Body != nil && len(fd.Body.List) == 1 {
					if r, ok := fd.Body.List[0].(*ast.ReturnStmt); ok && len(r.Results) == 1 {
						return fn, fd
					}
				}
			}
		}
	}
	return nil, nil
}

// staticConst is the term of constant v of type et going into type t.
func (g *gen) staticConst(n ast.Node, v constant.Value, et, t types.Type) uint32 {
	if isIface(t) && !isIface(et) {
		return g.staticBox(n, et, g.staticConst(n, v, et, et))
	}
	if isFloat(et) {
		x, _ := constant.Float64Val(constant.ToFloat(v))
		if basicKind(et.Underlying()) == types.Float32 {
			return TermConst(g.b.AddFloat32(float32(x)))
		}
		return TermConst(g.b.AddFloat64(x))
	}
	if v.Kind() == constant.Float {
		v = constant.ToInt(v)
	}
	switch v.Kind() {
	case constant.Bool:
		if constant.BoolVal(v) {
			return TermTrue
		}
		return TermFalse
	case constant.String:
		if s := constant.StringVal(v); s != "" {
			return TermConst(g.b.AddString(s))
		}
		return TermEmptyStr
	case constant.Int:
		n64, ok := constBits(v, et)
		if !ok {
			g.staticFail(n, "constant %s overflows %s", v, et)
		}
		switch {
		case n64 >= -(1<<30) && n64 < 1<<30:
			return TermInt(int32(n64))
		case isWide(et):
			return TermConst(g.b.AddInt64(n64))
		default:
			return TermConst(g.b.AddInt32(int32(n64)))
		}
	}
	g.staticFail(n, "constant %s not supported", v)
	return 0
}

// staticBox is the term of a value of concrete type et (term x) in an
// interface: a constant box for a boxed type, a K_INT64 for an int64.
func (g *gen) staticBox(n ast.Node, et types.Type, x uint32) uint32 {
	switch {
	case boxed(et):
		return TermConst(g.b.AddStruct(g.boxIndex(et), []uint32{x}))
	case isInt64(et) && x&1 == 1: // an immediate: box it
		return TermConst(g.b.AddInt64(int64(int32(x) >> 1)))
	}
	return x
}

// staticLit is the term of composite literal e: a constant struct, or a
// constant slice (an array is a slice whose length is its capacity).
func (g *gen) staticLit(e *ast.CompositeLit, env map[*types.Var]uint32) uint32 {
	t := g.p.Info.TypeOf(e)
	if st := structType(t); st != nil {
		s := st.Underlying().(*types.Struct)
		fields := make([]uint32, s.NumFields())
		set := make([]bool, len(fields))
		for i, el := range e.Elts {
			j, val := i, el
			if kv, ok := el.(*ast.KeyValueExpr); ok {
				val = kv.Value
				for k := 0; k < s.NumFields(); k++ {
					if s.Field(k).Name() == kv.Key.(*ast.Ident).Name {
						j = k
					}
				}
			}
			fields[j], set[j] = g.staticTerm(val, s.Field(j).Type(), env), true
		}
		for i := range fields {
			if !set[i] {
				fields[i] = g.zeroTerm(e, s.Field(i).Type())
			}
		}
		idx, _ := g.typeIndex(st)
		return TermConst(g.b.AddStruct(idx, fields))
	}
	if mt := mapOf(t); mt != nil {
		return g.staticMap(e, mt, env)
	}
	var elem types.Type
	n := len(e.Elts)
	if at := arrayType(t); at != nil {
		elem, n = at.Elem(), int(at.Len())
	} else if sl, ok := underlying(t).(*types.Slice); ok {
		elem = sl.Elem()
	} else {
		g.staticFail(e, "package-level variable of type %s not supported", types.TypeString(t, pkgName))
	}
	vals := map[int]ast.Expr{}
	k := 0
	for _, el := range e.Elts {
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			i, _ := constant.Int64Val(g.p.Info.Types[kv.Key].Value)
			k = int(i)
			el = kv.Value
		}
		vals[k] = el
		k++
		n = max(n, k)
	}
	if isByte(elem) {
		bs := make([]byte, n)
		for i, el := range vals {
			v, _ := constant.Int64Val(g.p.Info.Types[el].Value)
			bs[i] = byte(v)
		}
		return g.b.AddTermSlice(nil, bs, true)
	}
	terms := make([]uint32, n)
	for i := range terms {
		if el, ok := vals[i]; ok {
			terms[i] = g.staticTerm(el, elem, env)
		} else {
			terms[i] = g.zeroTerm(e, elem)
		}
	}
	return g.b.AddTermSlice(terms, nil, false)
}

// zeroTerm is the term of the zero value of type t.
func (g *gen) zeroTerm(n ast.Node, t types.Type) uint32 {
	switch {
	case structType(t) != nil:
		st := structType(t)
		s := st.Underlying().(*types.Struct)
		fields := make([]uint32, s.NumFields())
		for i := range fields {
			fields[i] = g.zeroTerm(n, s.Field(i).Type())
		}
		idx, _ := g.typeIndex(st)
		return TermConst(g.b.AddStruct(idx, fields))
	case isArray(t):
		at := arrayType(t)
		if isByte(at.Elem()) {
			return g.b.AddTermSlice(nil, make([]byte, at.Len()), true)
		}
		terms := make([]uint32, at.Len())
		for i := range terms {
			terms[i] = g.zeroTerm(n, at.Elem())
		}
		return g.b.AddTermSlice(terms, nil, false)
	case isPID(t):
		return TermZeroPid
	case isRef(t):
		return TermZeroRef
	case isSlice(t):
		return TermNilSlice
	case isBytes(t):
		return TermEmptyBytes
	case isIface(t) || isFunc(t) || isPointer(t):
		return TermNil
	case isFloat(t):
		return g.staticConst(n, constant.MakeInt64(0), t, t)
	case isBool(t):
		return TermFalse
	case isString(t):
		return TermEmptyStr
	case intKindOf(t).bits != 0:
		return TermInt(0)
	}
	g.staticFail(n, "zero value of %s in a package-level variable not supported", types.TypeString(t, pkgName))
	return 0
}

// staticMap is the term of map literal e of a package-level variable: a
// constant header (flagged StructMut | StructMap, so it prints as a map
// and SETFIELD refuses it) whose entries are sorted at compile time in
// B_CMP's order, which is known here for the keys staticProblem allows:
// constants of an integer, float, bool or string type.
func (g *gen) staticMap(e *ast.CompositeLit, mt *types.Map, env map[*types.Var]uint32) uint32 {
	type entry struct {
		key  constant.Value
		k, v uint32
	}
	var es []entry
	for _, el := range e.Elts {
		kv := el.(*ast.KeyValueExpr)
		key := g.p.Info.Types[kv.Key].Value
		k := g.staticTerm(kv.Key, mt.Key(), env)
		if isWide(mt.Key()) && k&1 == 1 { // a map's int64 keys are boxed
			k = TermConst(g.b.AddInt64(int64(int32(k) >> 1)))
		}
		es = append(es, entry{key, k, g.staticTerm(kv.Value, mt.Elem(), env)})
	}
	unsigned := intKindOf(mt.Key()).unsigned
	less := func(a, b constant.Value) bool {
		switch a.Kind() {
		case constant.String:
			return strings.Compare(constant.StringVal(a), constant.StringVal(b)) < 0
		case constant.Bool:
			return !constant.BoolVal(a) && constant.BoolVal(b)
		case constant.Float:
			x, _ := constant.Float64Val(a)
			y, _ := constant.Float64Val(b)
			return x < y
		}
		if isFloat(mt.Key()) {
			x, _ := constant.Float64Val(constant.ToFloat(a))
			y, _ := constant.Float64Val(constant.ToFloat(b))
			return x < y
		}
		x, _ := constBits(a, mt.Key())
		y, _ := constBits(b, mt.Key())
		if unsigned && !isWide(mt.Key()) { // a uint32's bits, as an int
			return int32(uint32(x)) < int32(uint32(y))
		}
		return x < y
	}
	sort.SliceStable(es, func(i, j int) bool { return less(es[i].key, es[j].key) })
	terms := make([]uint32, 0, 2*len(es))
	for _, x := range es {
		terms = append(terms, x.k, x.v)
	}
	entries := g.b.AddTermSlice(terms, nil, false)
	return TermConst(g.b.AddStruct(g.mapIndex(mt)|StructMut|StructMap, []uint32{entries}))
}
