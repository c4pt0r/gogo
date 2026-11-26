package compiler

import (
	"go/ast"
	"go/types"
)

// Maps (15.6, D4 of the language plan) are a library: gogo/maps (loaded
// into every program that uses a map) holds the operations, on a header
// that is a struct of a type of the map type's own, flagged StructMut |
// StructMap, whose one field is the entries slice, sorted by key (see
// lib/maps). The compiler builds headers itself (make, a literal), and
// lowers m[k], v, ok := m[k], m[k] = v, m[k] op= v, delete, len, clear
// and range to calls of the library's functions. Keys and values go as
// they are: a key is converted to the map's key type (an interface key is
// boxed as any value going into an interface) and an int64 or uint64 key
// is boxed (BOX64), so that all the keys of a map have one representation
// and B_CMP orders them by value; a value read back is the map's value
// type's, with no conversion (an array is copied, as Go copies it). A map
// is a reference: assignment shares the header; a send copies it with the
// message.

// mapOf returns t's underlying map type, or nil.
func mapOf(t types.Type) *types.Map {
	m, _ := underlying(t).(*types.Map)
	return m
}

// isMap reports whether t's underlying type is a map.
func isMap(t types.Type) bool { return mapOf(t) != nil }

// mapName names map type mt in the image, as "map[K]V".
func (g *gen) mapName(mt *types.Map) string {
	return types.TypeString(mt, func(p *types.Package) string {
		if p == g.p.Pkg {
			return ""
		}
		return pkgPrefix(p)
	})
}

// mapIndex returns the image type index of the header of map type mt,
// giving it an own type (flagged StructMut | StructMap) when first used.
func (g *gen) mapIndex(mt *types.Map) uint32 {
	key := g.mapName(mt)
	if idx, ok := g.mapIdx[key]; ok {
		return idx
	}
	if g.mapIdx == nil {
		g.mapIdx = map[string]uint32{}
	}
	idx := g.b.AddTypeFlags(key, 1, StructMut|StructMap)
	g.mapIdx[key] = idx
	return idx
}

// mapsFunc returns the function index of gogo/maps' function name.
func (f *fn) mapsFunc(n ast.Node, name string) uint32 {
	for _, pkg := range f.g.p.Libs {
		if pkg.Pkg.Path() != MapsPath {
			continue
		}
		if fn, ok := pkg.Pkg.Scope().Lookup(name).(*types.Func); ok {
			if idx, ok := f.g.funcIndex(fn); ok {
				return idx
			}
		}
	}
	f.fail(n, "internal error: gogo/maps.%s not loaded", name)
	return 0
}

// mapKeyInto evaluates key e of map type mt into dst: converted to the key
// type, an int64 or uint64 boxed.
func (f *fn) mapKeyInto(e ast.Expr, mt *types.Map, dst uint8) {
	f.valueInto(e, mt.Key(), dst)
	if isWide(mt.Key()) {
		f.asm.Emit(OpBox64, dst, dst, 0)
	}
}

// mapCall emits a call of gogo/maps' function name with the arguments in
// args.. (consecutive registers), its results into dst...
func (f *fn) mapCall(n ast.Node, name string, dst, args, nargs uint8) {
	f.asm.EmitExt(OpCall, dst, args, nargs, f.mapsFunc(n, name))
}

// mapGetInto compiles m[k] (ix) into base (the value, or the value type's
// zero) and base+1 (whether k is there): two fresh registers.
func (f *fn) mapGetInto(ix *ast.IndexExpr, base uint8) {
	mt := mapOf(f.g.p.Info.TypeOf(ix.X))
	mark := f.next
	args := f.allocN(ix, 2)
	f.exprInto(ix.X, args)
	f.mapKeyInto(ix.Index, mt, args+1)
	f.mapCall(ix, "get", base, args, 2)
	f.next = mark
	okL, endL := f.asm.NewLabel(), f.asm.NewLabel()
	f.jump(OpJmpt, base+1, okL)
	f.zeroInto(ix, mt.Elem(), base)
	f.jump(OpJmp, 0, endL)
	f.bind(okL)
	if at := arrayType(mt.Elem()); at != nil { // a copy, as Go's m[k] is
		f.cloneArray(ix, at, base, base)
	}
	f.bind(endL)
}

// mapIndexInto compiles the value of m[k] into dst.
func (f *fn) mapIndexInto(ix *ast.IndexExpr, dst uint8) {
	mark := f.next
	base := f.allocN(ix, 2)
	f.mapGetInto(ix, base)
	f.asm.Emit(OpMove, dst, base, 0)
	f.next = mark
}

// mapOpAssign compiles m[k] op= rhs and m[k]++ / m[k]-- (rhs nil): m and
// k are evaluated once, then rhs, then the entry is read, computed and
// set.
func (f *fn) mapOpAssign(ix *ast.IndexExpr, op uint8, rhs ast.Expr) {
	mt := mapOf(f.g.p.Info.TypeOf(ix.X))
	mark := f.next
	blk := f.allocN(ix, 3) // [m, k, v]: set's arguments
	f.exprInto(ix.X, blk)
	f.mapKeyInto(ix.Index, mt, blk+1)
	pre := -1
	if rhs != nil && !f.inert(rhs) {
		pre = int(f.rhsOperand(op, rhs))
	}
	res := f.allocN(ix, 2)
	f.mapCall(ix, "get", res, blk, 2)
	okL := f.asm.NewLabel()
	f.jump(OpJmpt, res+1, okL)
	f.zeroInto(ix, mt.Elem(), res)
	f.bind(okL)
	f.asm.Emit(OpMove, blk+2, res, 0)
	f.opInto(ix, op, blk+2, rhs, pre)
	f.mapCall(ix, "set", res, blk, 3)
	f.next = mark
}

// makeMapInto compiles make(map[K]V) and make(map[K]V, n) (n is
// evaluated and ignored, as a hint) into dst: a header with no entries.
func (f *fn) makeMapInto(call *ast.CallExpr, mt *types.Map, dst uint8) {
	mark := f.next
	if len(call.Args) > 1 {
		f.exprInto(call.Args[1], f.allocScalar(call))
	}
	f.next = mark
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNilSlice)
	f.asm.EmitExt(OpNewstruct, dst, dst, 1, f.g.mapIndex(mt))
}

// mapLitInto compiles a map literal into dst: a new header, then one set
// per entry, in source order (a later duplicate key wins, as a constant
// duplicate is a type error in Go anyway).
func (f *fn) mapLitInto(e *ast.CompositeLit, dst uint8) {
	mt := mapOf(f.g.p.Info.TypeOf(e))
	mark := f.next
	blk := f.allocN(e, 4) // [m, k, v, result]
	f.asm.EmitExt(OpLoadk, blk, 0, 0, TermNilSlice)
	f.asm.EmitExt(OpNewstruct, blk, blk, 1, f.g.mapIndex(mt))
	for _, el := range e.Elts {
		kv := el.(*ast.KeyValueExpr)
		f.mapKeyInto(kv.Key, mt, blk+1)
		f.valueInto(kv.Value, mt.Elem(), blk+2)
		f.mapCall(e, "set", blk+3, blk, 3)
	}
	f.asm.Emit(OpMove, dst, blk, 0)
	f.next = mark
}

// mapBuiltin compiles len(m), delete(m, k) and clear(m) into dst; false
// if call is none of them.
func (f *fn) mapBuiltin(call *ast.CallExpr, name string, dst uint8) bool {
	if len(call.Args) == 0 || !isMap(f.g.p.Info.TypeOf(call.Args[0])) {
		return false
	}
	mt := mapOf(f.g.p.Info.TypeOf(call.Args[0]))
	mark := f.next
	defer func() { f.next = mark }()
	switch name {
	case "len", "clear":
		r := f.alloc(call)
		f.exprInto(call.Args[0], r)
		fn := "length"
		if name == "clear" {
			fn = "clearAll"
		}
		f.mapCall(call, fn, dst, r, 1)
	case "delete":
		r := f.allocN(call, 2)
		f.exprInto(call.Args[0], r)
		f.mapKeyInto(call.Args[1], mt, r+1)
		f.mapCall(call, "del", dst, r, 2)
	default:
		return false
	}
	return true
}

// rangeMap compiles `for k, v := range m`: the map is evaluated once;
// each pass calls next with the index and key the last one left, which
// gives the next entry in key order (or -1), so the body may insert and
// delete (see gogo/maps' next).
func (f *fn) rangeMap(s *ast.RangeStmt) {
	label := f.takeLabel()
	scope := f.next
	mt := mapOf(f.g.p.Info.TypeOf(s.X))
	st := f.allocN(s, 3) // [m, i, prev]: next's arguments
	f.exprInto(s.X, st)
	f.asm.EmitExt(OpLoadk, st+1, 0, 0, TermInt(0))
	f.asm.EmitExt(OpLoadk, st+2, 0, 0, TermNil)
	declare := func(e ast.Expr) (*types.Var, uint8, bool) {
		id, _ := e.(*ast.Ident)
		if id == nil || id.Name == "_" {
			return nil, 0, false
		}
		v, ok := f.g.p.Info.Defs[id].(*types.Var)
		if !ok {
			f.fail(id, "internal error: range variable %s is not a definition", id.Name)
		}
		r := f.alloc(id)
		f.locals[v] = r
		return v, r, true
	}
	var kv, vv *types.Var
	var key, val uint8
	var hasKey, hasVal bool
	if s.Key != nil {
		kv, key, hasKey = declare(s.Key)
	}
	if s.Value != nil {
		vv, val, hasVal = declare(s.Value)
	}
	topL, endL := f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(topL)
	mark := f.next
	res := f.allocN(s, 3) // [i, k, v]
	f.mapCall(s, "next", res, st, 3)
	zero := f.allocScalar(s)
	f.asm.EmitExt(OpLoadk, zero, 0, 0, TermInt(0))
	f.asm.Emit(OpLt, zero, res, zero)
	f.jump(OpJmpt, zero, endL)
	f.asm.Emit(OpMove, st+1, res, 0)
	f.asm.Emit(OpMove, st+2, res+1, 0)
	if hasKey {
		f.asm.Emit(OpMove, key, res+1, 0)
		if isWide(mt.Key()) {
			f.unbox(s, mt.Key(), key) // the plain representation outside the map
		} else if at := arrayType(mt.Key()); at != nil {
			f.cloneArray(s, at, key, key)
		}
		f.boxVar(s, kv, key)
	}
	if hasVal {
		f.asm.Emit(OpMove, val, res+2, 0)
		if at := arrayType(mt.Elem()); at != nil {
			f.cloneArray(s, at, val, val)
		}
		f.boxVar(s, vv, val)
	}
	f.next = mark
	f.breakTo = append(f.breakTo, endL)
	f.continueTo = append(f.continueTo, topL)
	f.setLabel(label, endL, topL)
	f.block(s.Body.List)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
	f.continueTo = f.continueTo[:len(f.continueTo)-1]
	f.dropLabel(label)
	f.clearDead() // the body's locals die before the next iteration
	f.jump(OpJmp, 0, topL)
	f.bindEnd(endL)
	f.next = scope
}

// isMapIndex reports whether e is m[k] of a map m.
func (f *fn) isMapIndex(e ast.Expr) (*ast.IndexExpr, bool) {
	ix, ok := ast.Unparen(e).(*ast.IndexExpr)
	return ix, ok && isMap(f.g.p.Info.TypeOf(ix.X))
}
