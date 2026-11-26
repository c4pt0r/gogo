package compiler

import (
	"go/ast"
	"go/token"
	"go/types"
)

// Pointers (15.6). A pointer is a reference to a mutable struct of a
// synthetic type of its own, one per pointer type, whose type row carries
// StructMut: the only structs SETFIELD writes in place. For a pointer to a
// struct type T the target's fields are laid out as T's (the type "*T"
// has T's fields), so p.X is a GETFIELD and p.X = v a SETFIELD on the
// pointer itself, and *p is a copy of the fields into a new T (NEWSTRUCT),
// which keeps T a value: a copy taken before a write through p keeps the
// old fields. For any other target type the pointer is a box of one field
// (StructMut | StructBox). nil is the nil pointer: GETFIELD and SETFIELD
// of it end the actor with R_NIL. Pointers compare by identity (EQ, and
// B_CMP of a mutable struct). A send copies the target with the message,
// like a slice's array; within one message, pointers to one target stay
// one. There are no interior pointers: &s[i] and &x.F are rejected, as
// they would need a pointer into a slice's array or a struct field.
//
// A variable whose address is taken (&x, or x.M() of a pointer method M)
// lives in such a box for its whole life, and so does one that a func
// literal captures and that is assigned after its declaration (D7 of the
// language plan): its register holds the box, which closures capture, so
// they share the variable as in Go (closures otherwise capture by value).

// isPointer reports whether t's underlying type is a pointer.
func isPointer(t types.Type) bool {
	_, ok := underlying(t).(*types.Pointer)
	return ok
}

// pointerOf returns t's underlying pointer type, or nil.
func pointerOf(t types.Type) *types.Pointer {
	p, _ := underlying(t).(*types.Pointer)
	return p
}

// ptrStruct returns the named struct type a pointer type pt points to, or
// nil (a box of one field).
func ptrStruct(pt *types.Pointer) *types.Named { return structType(pt.Elem()) }

// ptrName names the target type of pointer type pt in the image: "*T"
// with T qualified as a function name is.
func (g *gen) ptrName(pt *types.Pointer) string {
	return "*" + types.TypeString(pt.Elem(), func(p *types.Package) string {
		if p == g.p.Pkg {
			return ""
		}
		return pkgPrefix(p)
	})
}

// ptrIndex returns the image type index of the target of pointer type pt,
// giving it an own type (flagged StructMut) when first used.
func (g *gen) ptrIndex(pt *types.Pointer) uint32 {
	key := g.ptrName(pt)
	if idx, ok := g.ptrIdx[key]; ok {
		return idx
	}
	if g.ptrIdx == nil {
		g.ptrIdx = map[string]uint32{}
	}
	var idx uint32
	if st := ptrStruct(pt); st != nil {
		idx = g.b.AddTypeFlags(key, uint32(st.Underlying().(*types.Struct).NumFields()), StructMut)
	} else {
		idx = g.b.AddTypeFlags(key, 1, StructMut|StructBox)
	}
	g.ptrIdx[key] = idx
	return idx
}

// derefInto loads *p into dst, p in register src of pointer type pt: a
// copy of a struct target's fields into a new struct (an allocation), or
// a box's one field.
func (f *fn) derefInto(n ast.Node, pt *types.Pointer, src, dst uint8) {
	st := ptrStruct(pt)
	if st == nil {
		f.asm.Emit(OpGetfield, dst, src, 0)
		return
	}
	nf := st.Underlying().(*types.Struct).NumFields()
	idx, ok := f.g.typeIndex(st)
	if !ok || nf > maxRegs {
		f.fail(n, "internal error: struct %s", st.Obj().Name())
	}
	mark := f.next
	base := f.allocN(n, nf)
	for i := 0; i < nf; i++ {
		f.asm.Emit(OpGetfield, base+uint8(i), src, uint8(i))
	}
	f.asm.EmitExt(OpNewstruct, dst, base, uint8(nf), idx)
	f.next = mark
}

// storeDeref compiles *p = v, p in register ptr of pointer type pt and v
// in register val: a struct target gets each of v's fields (SETFIELD per
// field), a box its one field.
func (f *fn) storeDeref(n ast.Node, pt *types.Pointer, ptr, val uint8) {
	st := ptrStruct(pt)
	if st == nil {
		f.asm.Emit(OpSetfield, val, ptr, 0)
		return
	}
	nf := st.Underlying().(*types.Struct).NumFields()
	mark := f.next
	t := f.alloc(n)
	for i := 0; i < nf; i++ {
		f.asm.Emit(OpGetfield, t, val, uint8(i))
		f.asm.Emit(OpSetfield, t, ptr, uint8(i))
	}
	f.next = mark
}

// newPtrInto builds into dst a new target of pointer type pt holding a
// copy of the value in register val (val may be dst).
func (f *fn) newPtrInto(n ast.Node, pt *types.Pointer, val, dst uint8) {
	idx := f.g.ptrIndex(pt)
	st := ptrStruct(pt)
	if st == nil {
		f.asm.EmitExt(OpNewstruct, dst, val, 1, idx)
		return
	}
	nf := st.Underlying().(*types.Struct).NumFields()
	mark := f.next
	base := f.allocN(n, nf)
	for i := 0; i < nf; i++ {
		f.asm.Emit(OpGetfield, base+uint8(i), val, uint8(i))
	}
	f.asm.EmitExt(OpNewstruct, dst, base, uint8(nf), idx)
	f.next = mark
}

// addrInto compiles &e into dst: &T{...} builds a new target from the
// literal (its fields go straight into the target for a struct T), &x is
// the box of variable x.
func (f *fn) addrInto(e *ast.UnaryExpr, dst uint8) {
	pt := pointerOf(f.g.p.Info.TypeOf(e))
	switch x := ast.Unparen(e.X).(type) {
	case *ast.CompositeLit:
		if st := ptrStruct(pt); st != nil {
			f.structLitAs(x, f.g.ptrIndex(pt), dst)
			return
		}
		f.exprInto(x, dst)
		f.newPtrInto(e, pt, dst, dst)
		return
	case *ast.Ident:
		v, _ := f.g.p.Info.Uses[x].(*types.Var)
		r, ok := f.locals[v]
		if !ok || !f.g.boxed[v] || !f.inBox[v] {
			f.fail(e, "internal error: &%s of a variable without a box", x.Name)
		}
		if r != dst {
			f.asm.Emit(OpMove, dst, r, 0)
		}
		return
	}
	f.fail(e, "internal error: unexpected &%T", e.X)
}

// newInto compiles new(T) into dst: a new target holding T's zero value.
func (f *fn) newInto(call *ast.CallExpr, dst uint8) {
	pt := pointerOf(f.g.p.Info.TypeOf(call))
	if st := ptrStruct(pt); st != nil {
		f.newStructAs(call, st, f.g.ptrIndex(pt), nil, dst)
		return
	}
	f.zeroInto(call, pt.Elem(), dst)
	f.newPtrInto(call, pt, dst, dst)
}

// starInto compiles *p (a read) into dst.
func (f *fn) starInto(e *ast.StarExpr, dst uint8) {
	mark := f.next
	p := f.operand(e.X)
	f.derefInto(e, pointerOf(f.g.p.Info.TypeOf(e.X)), p, dst)
	f.next = mark
}

// ---- variables in boxes ----

// varPtr is the pointer type of the box of variable v.
func varPtr(v *types.Var) *types.Pointer { return types.NewPointer(v.Type()) }

// isBoxedVar reports whether e names a variable that lives in a box.
func (f *fn) isBoxedVar(e ast.Expr) (*types.Var, bool) {
	id, ok := ast.Unparen(e).(*ast.Ident)
	if !ok {
		return nil, false
	}
	v, ok := f.g.p.Info.ObjectOf(id).(*types.Var)
	return v, ok && f.g.boxed[v]
}

// boxVar puts the value of v in register r into a new box, in place, if v
// lives in one: at its declaration, binding or (a loop variable) each
// iteration.
func (f *fn) boxVar(n ast.Node, v *types.Var, r uint8) {
	if v != nil && f.g.boxed[v] {
		f.newPtrInto(n, varPtr(v), r, r)
		f.markBoxed(v)
	}
}

// markBoxed records that v's register holds its box (boxVar, or a
// closure's capture of it): localOf refuses a boxed variable that was not
// put into one, an internal error rather than a GETFIELD on a plain value.
func (f *fn) markBoxed(v *types.Var) {
	if f.inBox == nil {
		f.inBox = map[*types.Var]bool{}
	}
	f.inBox[v] = true
}

// rangeVar is the variable a range clause's key or value expression e
// declares, or nil.
func (f *fn) rangeVar(e ast.Expr) *types.Var {
	id, _ := e.(*ast.Ident)
	if id == nil {
		return nil
	}
	v, _ := f.g.p.Info.Defs[id].(*types.Var)
	return v
}

// boxedLoad loads the value of boxed variable v (its box in register box)
// into dst.
func (f *fn) boxedLoad(n ast.Node, v *types.Var, box, dst uint8) {
	f.derefInto(n, varPtr(v), box, dst)
}

// findBoxed returns the variables of the program that live in a box: those
// whose address is taken (&x, or x.M() or x.M of a method with a pointer
// receiver on a variable x of the named type), and those a func literal
// captures that are written after their declaration (an assignment, ++ or
// --, a field write, or a return with values for a named result). An
// array variable is never boxed: it is written in place already. A
// three-clause for loop's variable that only its post statement writes
// needs no box: each closure of an iteration captures its value, which is
// the iteration's (Go 1.22's per-iteration loop variables).
func findBoxed(p *Program) map[*types.Var]bool {
	boxed := map[*types.Var]bool{}
	info := p.Info
	varOf := func(e ast.Expr) *types.Var {
		id, ok := ast.Unparen(e).(*ast.Ident)
		if !ok {
			return nil
		}
		v, _ := info.Uses[id].(*types.Var)
		if v == nil || v.IsField() || v.Parent() == v.Pkg().Scope() {
			return nil // not a local (a package-level variable)
		}
		return v
	}
	for _, pkg := range p.packages() {
		for _, file := range pkg.Files {
			for _, d := range file.Decls {
				fd, ok := d.(*ast.FuncDecl)
				if !ok || fd.Body == nil {
					continue
				}
				captured := map[*types.Var]bool{}
				written := map[*types.Var]bool{}
				postOnly := map[*types.Var]bool{} // loop variables written by their post statement
				// root is the variable a write to e writes: x of x, x.F,
				// x.F.G (not through a pointer or a slice element).
				root := func(e ast.Expr) *types.Var {
					x := ast.Unparen(e)
					for {
						sel, ok := x.(*ast.SelectorExpr)
						if !ok || info.Selections[sel] == nil || isPointer(info.TypeOf(sel.X)) {
							break
						}
						x = ast.Unparen(sel.X)
					}
					return varOf(x)
				}
				var walk func(n ast.Node, sig *types.Signature)
				walk = func(n ast.Node, sig *types.Signature) {
					ast.Inspect(n, func(n ast.Node) bool {
						switch n := n.(type) {
						case *ast.FuncLit:
							for _, v := range litCaptures(info, n) {
								captured[v] = true
							}
							s, _ := info.TypeOf(n).(*types.Signature)
							walk(n.Body, s)
							return false
						case *ast.UnaryExpr:
							if n.Op == token.AND {
								if v := varOf(n.X); v != nil {
									boxed[v] = true
								}
							}
						case *ast.SelectorExpr:
							if s := info.Selections[n]; s != nil && (s.Kind() == types.MethodVal) && !isIface(s.Recv()) {
								if fn, ok := s.Obj().(*types.Func); ok && isPointer(fn.Type().(*types.Signature).Recv().Type()) && !isPointer(info.TypeOf(n.X)) {
									if v := varOf(n.X); v != nil {
										boxed[v] = true
									}
								}
							}
						case *ast.ForStmt:
							if as, ok := n.Init.(*ast.AssignStmt); ok && as.Tok == token.DEFINE && n.Post != nil {
								for _, l := range as.Lhs {
									if id, ok := l.(*ast.Ident); ok {
										if v, ok := info.Defs[id].(*types.Var); ok {
											postOnly[v] = true
										}
									}
								}
								// The post statement's writes are not writes (see above).
								if n.Cond != nil {
									walk(n.Cond, sig)
								}
								walk(n.Init, sig)
								walk(n.Body, sig)
								for _, w := range postWrites(n.Post) {
									if v := root(w); v != nil && !postOnly[v] {
										written[v] = true
									}
								}
								ast.Inspect(n.Post, func(m ast.Node) bool {
									if lit, ok := m.(*ast.FuncLit); ok {
										walk(lit, sig)
										return false
									}
									return true
								})
								return false
							}
						case *ast.AssignStmt:
							for _, l := range n.Lhs {
								if v := root(l); v != nil {
									written[v] = true
								}
							}
						case *ast.IncDecStmt:
							if v := root(n.X); v != nil {
								written[v] = true
							}
						case *ast.ReturnStmt:
							if len(n.Results) > 0 && sig != nil {
								for i := 0; i < sig.Results().Len(); i++ {
									if v := sig.Results().At(i); v.Name() != "" {
										written[v] = true
									}
								}
							}
						}
						return true
					})
				}
				var sig *types.Signature
				if fn, ok := info.Defs[fd.Name].(*types.Func); ok {
					sig = fn.Type().(*types.Signature)
				}
				walk(fd.Body, sig)
				for v := range captured {
					if written[v] && !isArray(v.Type()) {
						boxed[v] = true
					}
				}
			}
		}
	}
	return boxed
}

// postWrites lists the targets a for loop's post statement writes.
func postWrites(s ast.Stmt) []ast.Expr {
	switch s := s.(type) {
	case *ast.AssignStmt:
		return s.Lhs
	case *ast.IncDecStmt:
		return []ast.Expr{s.X}
	}
	return nil
}

// derefTarget reports whether assignment target e is written through a
// pointer as a whole: *p, or a variable that lives in a box.
func (f *fn) derefTarget(e ast.Expr) bool {
	if _, ok := ast.Unparen(e).(*ast.StarExpr); ok {
		return true
	}
	_, ok := f.isBoxedVar(e)
	return ok
}

// derefType is the pointer type through which target e (derefTarget) is
// written.
func (f *fn) derefType(e ast.Expr) *types.Pointer {
	if st, ok := ast.Unparen(e).(*ast.StarExpr); ok {
		return pointerOf(f.g.p.Info.TypeOf(st.X))
	}
	v, _ := f.isBoxedVar(e)
	return varPtr(v)
}

// derefPtr returns a register holding the pointer target e (derefTarget)
// is written through: p of *p evaluated into a fresh register (the
// caller releases it), or a boxed variable's own register.
func (f *fn) derefPtr(e ast.Expr) uint8 {
	if st, ok := ast.Unparen(e).(*ast.StarExpr); ok {
		r := f.alloc(st)
		f.exprInto(st.X, r)
		return r
	}
	return f.localOf(e)
}

// derefOpAssign compiles *p op= rhs and *p++ / *p-- (rhs nil), and the
// same on a variable in a box: the pointer is evaluated once, rhs (unless
// inert) before the target is read, as for an element (fieldOpAssign).
func (f *fn) derefOpAssign(e ast.Expr, op uint8, rhs ast.Expr) {
	mark := f.next
	pt := f.derefType(e)
	ptr := f.derefPtr(e)
	pre := -1
	if rhs != nil && !f.inert(rhs) {
		pre = int(f.rhsOperand(op, rhs))
	}
	v := f.alloc(e)
	f.derefInto(e, pt, ptr, v)
	f.opInto(e, op, v, rhs, pre)
	f.storeDeref(e, pt, ptr, v)
	f.next = mark
}

// rebox gives each boxed variable a three-clause for loop declares a new
// box, a copy of its current value, before the post statement: Go 1.22's
// per-iteration loop variables, so a closure or pointer of one iteration
// keeps that iteration's variable.
func (f *fn) rebox(s *ast.ForStmt) {
	as, ok := s.Init.(*ast.AssignStmt)
	if !ok || as.Tok != token.DEFINE {
		return
	}
	for _, l := range as.Lhs {
		id, _ := l.(*ast.Ident)
		v, _ := f.g.p.Info.Defs[id].(*types.Var)
		if v == nil || !f.g.boxed[v] {
			continue
		}
		r := f.locals[v]
		pt := varPtr(v)
		if ptrStruct(pt) != nil { // a target laid out as the struct: copy its fields
			f.newPtrInto(s, pt, r, r)
			continue
		}
		mark := f.next
		t := f.alloc(s)
		f.asm.Emit(OpGetfield, t, r, 0)
		f.newPtrInto(s, pt, t, r)
		f.next = mark
	}
}
