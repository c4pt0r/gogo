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

// Compile checks p against the gogo subset and compiles it to an image.
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
	// later ones resolve: all but the builtins, those of the system
	// packages, which get an index when first used (funcIndex), and a
	// //gogo:ondemand file's, which are compiled only if used.
	used := onDemandUsed(p)
	var decls, all []*ast.FuncDecl
	g.lazy = map[*types.Func]*ast.FuncDecl{}
	for _, pkg := range p.packages() {
		for _, f := range pkg.Files {
			onDemand := isOnDemand(f)
			for _, d := range f.Decls {
				fd, ok := d.(*ast.FuncDecl)
				if !ok {
					continue
				}
				fn := p.Info.Defs[fd.Name].(*types.Func)
				switch {
				case p.isBuiltin(fn) || isIntrinsic(p.Info, fd):
					continue
				case p.system[pkg.Pkg]:
					g.lazy[fn] = fd
				case onDemand && !used[fn]:
				default:
					g.addFunc(fn)
					decls = append(decls, fd)
				}
				all = append(all, fd)
			}
		}
	}
	g.nameLits(all)
	var errs []error
	for _, fd := range decls {
		if err := g.compileDecl(fd); err != nil {
			errs = append(errs, err)
		}
	}
	// Func literals and the system packages' functions in use, queued as
	// they are compiled; compiling one may queue more.
	for len(g.pending) > 0 || len(g.used) > 0 {
		if len(g.used) > 0 {
			fd := g.used[0]
			g.used = g.used[1:]
			if err := g.compileDecl(fd); err != nil {
				errs = append(errs, err)
			}
			continue
		}
		c := g.pending[0]
		g.pending = g.pending[1:]
		// The captures are the leading parameters, the literal's own
		// parameters follow (CALLV passes them in that order).
		params := append(append([]*types.Var{}, c.captures...), g.declParams(c.lit.Type)...)
		sig := p.Info.TypeOf(c.lit).(*types.Signature)
		if err := g.compileFunc(c.info, params, sig.Results(), c.lit.Body); err != nil {
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
	// Entry: func main, or function 0 in a package without one (a module
	// the host loads and spawns by export name).
	if m, ok := p.Pkg.Scope().Lookup("main").(*types.Func); ok {
		g.b.SetEntry(g.funcIdx[m])
	}
	names, _ := Requires(p)
	g.b.SetRequires(names)
	for _, e := range Exports(p) {
		g.b.AddExport(e.Name, g.funcIdx[e.Fn])
	}
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

	// lazy holds the functions of the system packages (marks), indexed
	// and compiled only once used; used queues those indexed but not yet
	// compiled.
	lazy map[*types.Func]*ast.FuncDecl
	used []*ast.FuncDecl

	boxIdx map[string]uint32 // own box types (boxIndex), by image name

	concrete  []types.Type           // concreteTypes, once computed
	dispIdx   map[*types.Func]uint32 // dispatchers, by interface method
	isHelpers []isHelper             // implements tests, by interface type
}

// addFunc gives declared function fn the next function index.
func (g *gen) addFunc(fn *types.Func) uint32 {
	idx := uint32(len(g.funcs))
	g.funcIdx[fn] = idx
	g.funcs = append(g.funcs, &funcInfo{name: g.qualified(fn.Pkg(), funcName(fn))})
	return idx
}

// funcIndex returns the function index of declared function fn (not a
// builtin). A function of a system package gets its index, and is queued
// for compilation, here, when first used.
func (g *gen) funcIndex(fn *types.Func) (uint32, bool) {
	if idx, ok := g.funcIdx[fn]; ok {
		return idx, true
	}
	fd, ok := g.lazy[fn]
	if !ok {
		return 0, false
	}
	g.used = append(g.used, fd)
	return g.addFunc(fn), true
}

// compileDecl compiles declared function fd (indexed already).
func (g *gen) compileDecl(fd *ast.FuncDecl) error {
	fn := g.p.Info.Defs[fd.Name].(*types.Func)
	params := g.declParams(fd.Type)
	if fd.Recv != nil { // the receiver is parameter 0
		params = append(g.fieldVars(fd.Recv), params...)
	}
	return g.compileFunc(g.funcs[g.funcIdx[fn]], params, fn.Type().(*types.Signature).Results(), fd.Body)
}

// qualified names a function or type of package pkg in the image: plain
// for package main, pkgPrefix.name otherwise.
func (g *gen) qualified(pkg *types.Package, name string) string {
	if pkg == g.p.Pkg {
		return name
	}
	return pkgPrefix(pkg) + "." + name
}

// funcName names a declared function in the image, without its package:
// its name, or Type.Method for a method.
func funcName(fn *types.Func) string {
	if recv := fn.Type().(*types.Signature).Recv(); recv != nil {
		if n, ok := types.Unalias(recv.Type()).(*types.Named); ok {
			return n.Obj().Name() + "." + fn.Name()
		}
	}
	return fn.Name()
}

// pkgPrefix names package pkg in the image's function and type names: a
// library by its package name (strings.Index), a user package by its
// import path (app/a/util.Mk), so that two user packages of one name, or
// a user package named like a library, stay apart in crash reports,
// actor.Reason.Func and gogo dis.
func pkgPrefix(pkg *types.Package) string {
	if isUser(pkg.Path()) && pkg.Path() != "main" {
		return pkg.Path()
	}
	return pkg.Name()
}

// structType returns the named struct type behind t, or nil. actor.PID,
// actor.Ref, bytes.Bytes and bytes.Buffer are structs in the system packages but
// tagged terms and opaque heap objects in the VM, so they are not.
func structType(t types.Type) *types.Named {
	n, ok := types.Unalias(t).(*types.Named)
	if !ok {
		return nil
	}
	if _, ok := n.Underlying().(*types.Struct); !ok {
		return nil
	}
	if isPID(n) || isRef(n) || isBytes(n) || isBuffer(n) {
		return nil
	}
	return n
}

// isActorType reports whether t is the gogo/actor type called name.
func isActorType(t types.Type, name string) bool {
	n, ok := types.Unalias(t).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == ActorPath && n.Obj().Name() == name
}

// isFsFile reports whether t is fs.File, a PID in the VM.
func isFsFile(t types.Type) bool {
	n, ok := types.Unalias(t).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == FsPath && n.Obj().Name() == "File"
}

// isPID reports whether t is actor.PID or fs.File (a port PID).
func isPID(t types.Type) bool { return isActorType(t, "PID") || isFsFile(t) }

// isPIDConv reports whether call converts between actor.PID and fs.File,
// which share their representation.
func isPIDConv(info *types.Info, call *ast.CallExpr) bool {
	return len(call.Args) == 1 && isPID(info.TypeOf(call.Fun)) && isPID(info.TypeOf(call.Args[0]))
}

func isRef(t types.Type) bool { return isActorType(t, "Ref") }

// isBytesType reports whether t is the gogo/bytes type called name.
func isBytesType(t types.Type, name string) bool {
	n, ok := types.Unalias(t).(*types.Named)
	return ok && n.Obj().Pkg() != nil && n.Obj().Pkg().Path() == BytesPath && n.Obj().Name() == name
}

func isBytes(t types.Type) bool { return isBytesType(t, "Bytes") }

func isBuffer(t types.Type) bool { return isBytesType(t, "Buffer") }

// reservedTypes are the struct types of the system packages (gogo/actor,
// gogo/gpio, gogo/net, gogo/fs, gogo/kbd, gogo/power, gogo/code, gogo/port,
// gogo/imu, gogo/ir, gogo/audio, gogo/screen). The VM builds and recognizes
// these structs by fixed index, their position here (TYPE_* in
// runtime/vm/opcodes.h, field counts in TYPE_NFIELDS there). An image's
// type table starts with them, in this order, up to the highest one the
// program uses; own types are numbered from TypeOwnBase after them.
//
// Within a major format they only grow: a new type goes at the end and an
// existing type may gain fields only at the end, of type int, where 0
// means the default (a VM reading an older image's struct sees 0, and an
// old VM refuses an image whose struct has more fields than it knows).
// Each such change is a format minor: add the type's image name to
// typeMinor (image.go).
var reservedTypes = []struct {
	pkg    string
	name   string
	fields []string
}{
	{ActorPath, "ProcInfo", []string{"Memory"}},
	{ActorPath, "Reason", []string{"Code", "Func"}},
	{ActorPath, "ExitMsg", []string{"From", "Reason"}},
	{ActorPath, "DownMsg", []string{"Ref", "From", "Reason"}},
	{ActorPath, "Request", []string{"From", "Ref", "Body"}},
	{ActorPath, "Options", []string{"MaxMemory"}},
	{GpioPath, "Config", []string{"Pin", "Mode", "Edge", "DebounceMs"}},
	{GpioPath, "Write", []string{"Value"}},
	{GpioPath, "Edge", []string{"Pin", "Value"}},
	{NetPath, "IfConfig", nil},
	{NetPath, "SetAddr", []string{"IP", "Mask", "Gateway", "DNS"}},
	{NetPath, "Up", []string{"IP", "Gateway", "DNS"}},
	{NetPath, "Down", nil},
	{NetPath, "Addr", []string{"IP", "Port"}},
	{NetPath, "UDP", []string{"Port"}},
	{NetPath, "SendTo", []string{"Addr", "Data"}},
	{NetPath, "Datagram", []string{"From", "Data"}},
	{NetPath, "Dial", []string{"IP", "Port", "TimeoutMs"}},
	{NetPath, "Listen", []string{"Port"}},
	{NetPath, "Shutdown", nil},
	{NetPath, "Resume", nil},
	{NetPath, "Connected", nil},
	{NetPath, "Accepted", []string{"Conn"}},
	{FsPath, "Info", []string{"Size", "IsDir"}},
	{FsPath, "Space", []string{"Used", "Total"}},
	{KbdPath, "Config", []string{"RepeatDelayMs", "RepeatMs"}},
	{KbdPath, "Key", []string{"Code", "Char", "Mods", "Down"}},
	{PowerPath, "Config", []string{"PeriodMs"}},
	{PowerPath, "Battery", []string{"MilliVolts", "Pct", "Charging", "External"}},
	{PowerPath, "Read", nil},
	{CodePath, "Module", []string{"H", "missing"}},
	{PortPath, "Chunk", []string{"From", "Seq", "TimeMs", "Dropped", "Data"}},
	{PortPath, "Out", []string{"Data"}},
	{PortPath, "Done", []string{"From", "Seq"}},
	{PortPath, "Closed", []string{"From", "Reason"}},
	{ImuPath, "Config", []string{"RateHz", "Gestures", "BatchMs"}},
	{ImuPath, "Gesture", []string{"Kind", "Value"}},
	{IrPath, "TX", []string{"CarrierHz", "DutyPct"}},
	{IrPath, "RX", []string{"GapUs"}},
	{AudioPath, "Mic", []string{"RateHz", "FrameMs", "GainDb"}},
	{AudioPath, "Speaker", []string{"RateHz", "VolumePct"}},
	{NetPath, "DialTLS", []string{"IP", "Port", "TimeoutMs", "Host"}},
	{PortPath, "Host", []string{"Arg", "Data"}},
	{PortPath, "Post", []string{"From", "Kind", "Data"}},
	{ScreenPath, "Config", []string{"Cols", "Rows"}}, // 15.1
	// 15.4: the errors that may cross modules. Code is an int: its row is
	// the box an interface holds it in, of one field.
	{ErrorsPath, "Code", []string{"value"}},
	{ErrorsPath, "errorString", []string{"s"}},
}

// reservedName is the image name of reserved type i: the bare name for
// gogo/actor types, else the package's last element and the name
// ("port.Chunk"). typeMinor (image.go) is keyed by it.
func reservedName(i int) string {
	rt := reservedTypes[i]
	switch rt.pkg {
	case ActorPath:
		return rt.name
	case PortPath:
		return "port." + rt.name
	case GpioPath:
		return "gpio." + rt.name
	case NetPath:
		return "net." + rt.name
	case FsPath:
		return "fs." + rt.name
	case KbdPath:
		return "kbd." + rt.name
	case PowerPath:
		return "power." + rt.name
	case CodePath:
		return "code." + rt.name
	case ImuPath:
		return "imu." + rt.name
	case IrPath:
		return "ir." + rt.name
	case AudioPath:
		return "audio." + rt.name
	case ScreenPath:
		return "screen." + rt.name
	case ErrorsPath:
		return "errors." + rt.name
	}
	panic("reservedName: package " + rt.pkg)
}

// collectTypes emits the reserved types first (whether used or not), then
// gives the other struct types image type indices in order of first
// appearance in the package sources (libraries, then main; declarations
// and uses alike).
func (g *gen) collectTypes() error {
	byIdx := map[int]*types.TypeName{}
	for tn, i := range g.p.reserved {
		if i >= len(reservedTypes) || reservedTypes[i].pkg != tn.Pkg().Path() || reservedTypes[i].name != tn.Name() {
			return fmt.Errorf("gogo: internal error: %s.%s is //gogo:reserved %d, which the compiler does not know as that", tn.Pkg().Path(), tn.Name(), i)
		}
		byIdx[i] = tn
	}
	loaded := map[string]*types.Package{}
	for _, pkg := range g.p.Libs {
		loaded[pkg.Pkg.Path()] = pkg.Pkg
	}
	for i, rt := range reservedTypes {
		name := reservedName(i)
		idx := g.b.AddReservedType(name, uint32(len(rt.fields)))
		tn := byIdx[i]
		if tn == nil {
			if pkg := loaded[rt.pkg]; pkg != nil && pkg.Scope().Lookup(rt.name) != nil {
				return fmt.Errorf("gogo: internal error: %s is not marked //gogo:reserved %d", name, i)
			}
			continue
		}
		_, box := tn.Type().Underlying().(*types.Basic) // the box of a named basic type: one field
		if st, ok := tn.Type().Underlying().(*types.Struct); (!ok || st.NumFields() != len(rt.fields)) && !(box && len(rt.fields) == 1) {
			return fmt.Errorf("gogo: internal error: %s does not match the reserved layout", name)
		}
		g.typeIdx[tn] = idx
	}
	add := func(t types.Type) {
		if n := structType(t); n != nil {
			g.typeIndex(n)
		}
	}
	for _, pkg := range g.p.packages() {
		if g.p.system[pkg.Pkg] {
			continue // its types count where the program uses them
		}
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

// typeIndex returns the image type index of named struct type n, giving
// an own type the next index when first seen (collectTypes sees them in
// source order; a system package's are added as its code uses them). A
// reserved type it returns gets its row in the image.
func (g *gen) typeIndex(n *types.Named) (uint32, bool) {
	if idx, ok := g.typeIdx[n.Obj()]; ok {
		g.b.UseType(idx)
		return idx, true
	}
	if _, ok := n.Underlying().(*types.Struct); !ok {
		return 0, false
	}
	idx := g.b.AddType(g.qualified(n.Obj().Pkg(), n.Obj().Name()), uint32(n.Underlying().(*types.Struct).NumFields()))
	g.typeIdx[n.Obj()] = idx
	return idx, true
}

// genError aborts the compilation of one function; compileFunc recovers it.
type genError struct{ err error }

// fn holds the state of the function being compiled.
//
// Registers: parameters are r0..r(n-1); each local gets the next register at
// its declaration and keeps it until its block ends; expression temporaries
// are allocated above the locals and released as soon as the expression that
// needed them has been emitted. nRegs is the high-water mark.
//
// Dead registers: a register at or above next is dead, but the VM's GC
// scans every register of a frame, so a dead register still holding a heap
// value would keep it alive. dirty is a high-water mark: registers in
// [next, dirty) may hold such stale values. alloc raises it (allocScalar,
// for values that are never heap objects, does not); clear points emit
// CLEAR next, dirty-next and lower it to next. The clear points are the
// end of every statement (which covers the temporaries it released, a
// discarded call result and, after a block or a loop, its locals) and
// every backward jump (so a loop body's locals die before the next
// iteration). Jumps carry dirty to their label (labelDirty) and binding a
// label merges it in, so locals and temporaries live where a break,
// continue or a conditional jump left are cleared at the next clear point
// on every path. Registers below next are never cleared: they are the
// locals of the enclosing scopes, live by construction.
type fn struct {
	g       *gen
	info    *funcInfo
	asm     Asm
	locals  map[*types.Var]uint8
	next    int // next free register
	nRegs   int
	result  types.Type   // the result type if there is exactly one, else nil
	results []types.Type // the result types
	named   int          // first register of the named results, -1 if unnamed

	breakTo    []Label // innermost last
	continueTo []Label
	label      string                 // the label of the statement about to be compiled, "" if none
	labeled    map[string]jumpTargets // the labeled statements being compiled, by label

	dirty      int           // see the type comment
	labelDirty map[Label]int // dirty at the jumps to each label, merged
	boundAt    int           // code position of the last label bound (-1: none)
	lastClear  int           // code position of the last CLEAR emitted (-1: none)

	defers   *deferState // the function's defer statements, nil if none (codegen_defer.go)
	compared bool        // the value being compiled is only compared (constBox)
}

// declParams lists the parameters of a declared function; unnamed ones are
// nil (they get a register but no variable).
func (g *gen) declParams(ft *ast.FuncType) []*types.Var {
	return g.fieldVars(ft.Params)
}

// fieldVars lists the variables a parameter or receiver list declares;
// unnamed ones are nil.
func (g *gen) fieldVars(fl *ast.FieldList) []*types.Var {
	var params []*types.Var
	for _, field := range fl.List {
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

// compileFunc compiles body into info; params get r0..r(n-1); results are
// the function's results (nil if none). Named results get the registers
// after the parameters, initialized to their zero values, so a bare return
// returns them in place.
func (g *gen) compileFunc(info *funcInfo, params []*types.Var, results *types.Tuple, body *ast.BlockStmt) (err error) {
	f := &fn{g: g, info: info, locals: map[*types.Var]uint8{}, named: -1,
		labelDirty: map[Label]int{}, boundAt: -1, lastClear: -1}
	if results != nil {
		f.results = tupleTypes(results)
		if len(f.results) == 1 {
			f.result = f.results[0]
		}
	}
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
	if results != nil && results.Len() > 0 && results.At(0).Name() != "" {
		f.named = f.next
		for i := 0; i < results.Len(); i++ {
			v := results.At(i)
			r := f.alloc(body)
			f.zeroInto(body, v.Type(), r)
			if v.Name() != "_" {
				f.locals[v] = r
			}
		}
		f.clearDead()
	}
	f.setupDefers(body)
	f.stmts(body.List)
	if n := len(body.List); n == 0 || !isReturn(body.List[n-1]) {
		if len(f.results) == 0 { // else the end is unreachable
			f.runDefers()
		}
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
// The register may come to hold a heap value: it is cleared once dead.
func (f *fn) alloc(n ast.Node) uint8 {
	r := f.allocScalar(n)
	f.dirty = max(f.dirty, f.next)
	return r
}

// allocScalar reserves the next register for a value that is never a heap
// object (see scalar). It is not marked dirty: if it held a heap value
// earlier in the statement, dirty already covers it.
func (f *fn) allocScalar(n ast.Node) uint8 {
	if f.next >= maxRegs {
		f.fail(n, "function %s needs more than %d registers", f.info.name, maxRegs)
	}
	r := uint8(f.next)
	f.next++
	f.nRegs = max(f.nRegs, f.next)
	return r
}

// scalar reports whether e's value is never a heap object: a bool, a PID
// (immediates), no value (a call without result yields nil), or a constant
// that is an immediate or lives in the const area (not an int outside 31
// bits, which LOADI32 boxes on the heap).
func (f *fn) scalar(e ast.Expr) bool {
	tv := f.g.p.Info.Types[e]
	if tv.IsNil() { // the nil slice, a special
		return true
	}
	if tv.Value != nil {
		if tv.Value.Kind() == constant.Int && !isWide(tv.Type) && !isFloat(tv.Type) {
			n, ok := constBits(tv.Value, tv.Type)
			return ok && n >= -(1<<30) && n < 1<<30
		}
		return true
	}
	if tup, ok := tv.Type.(*types.Tuple); ok && tup.Len() == 0 {
		return true
	}
	if b, ok := underlying(tv.Type).(*types.Basic); ok && b.Info()&types.IsBoolean != 0 {
		return true
	}
	return isPID(tv.Type)
}

// allocFor reserves a register for e's value: scalar when e is.
func (f *fn) allocFor(e ast.Expr) uint8 {
	if f.scalar(e) {
		return f.allocScalar(e)
	}
	return f.alloc(e)
}

// clearDead is a clear point: CLEAR the registers in [next, dirty). A
// CLEAR right after the previous one (no label in between) is merged into
// it when the ranges touch.
func (f *fn) clearDead() {
	if f.dirty <= f.next {
		f.dirty = f.next
		return
	}
	a, n := f.next, f.dirty-f.next
	f.dirty = f.next
	if pa, pn, ok := f.trailingClear(); ok && a <= pa+pn && pa <= a+n {
		lo, hi := min(a, pa), max(a+n, pa+pn)
		f.asm.SetLast(makeWord(OpClear, uint8(lo), uint8(hi-lo), 0))
		return
	}
	f.asm.Emit(OpClear, uint8(a), uint8(n), 0)
	f.lastClear = f.asm.Len() - 1
}

// trailingClear returns the range of the CLEAR that is the last
// instruction emitted, if it is one and no label is bound after it (the
// label's jumpers skip it, so it must not be widened or moved; a label
// bound before it is fine).
func (f *fn) trailingClear() (a, n int, ok bool) {
	w, ok := f.asm.Last()
	if !ok || f.lastClear != f.asm.Len()-1 || f.boundAt == f.asm.Len() {
		return 0, 0, false
	}
	return int(w >> 8 & 0xFF), int(w >> 16 & 0xFF), true
}

// dropTrailingClear removes a CLEAR that is the last instruction emitted.
func (f *fn) dropTrailingClear() (a, n int, ok bool) {
	if a, n, ok = f.trailingClear(); ok {
		f.asm.DropLast()
		f.lastClear = -1
	}
	return a, n, ok
}

// jump emits a jump to l, carrying dirty to it. After an unconditional
// jump the code is unreachable until a label: nothing is stale there.
func (f *fn) jump(op uint8, ra uint8, l Label) {
	f.labelDirty[l] = max(f.labelDirty[l], f.dirty)
	f.asm.EmitJump(op, ra, l)
	if op == OpJmp {
		f.dirty = 0
	}
}

// bind binds l here, merging the dirty marks of the jumps to it.
func (f *fn) bind(l Label) {
	f.asm.Bind(l)
	f.boundAt = f.asm.Len()
	f.dirty = max(f.dirty, f.labelDirty[l])
}

// bindEnd is bind for a label that a clear point follows with no code in
// between (the end of an if, for or type switch statement). A CLEAR just
// before the label is moved after it (into dirty), so it merges with the
// clear the label's jumpers need instead of being emitted twice.
func (f *fn) bindEnd(l Label) {
	if a, _, ok := f.trailingClear(); ok && a >= f.next {
		_, n, _ := f.dropTrailingClear()
		f.dirty = max(f.dirty, a+n)
	}
	f.asm.Bind(l)
	f.boundAt = f.asm.Len()
	f.dirty = max(f.dirty, f.labelDirty[l])
}

// allocN reserves n consecutive registers and returns the first.
func (f *fn) allocN(node ast.Node, n int) uint8 {
	base := uint8(f.next) // next <= maxRegs, so it fits
	for i := 0; i < n; i++ {
		f.alloc(node)
	}
	return base
}

// retNil returns nil. A CLEAR just before it is dropped: returning frees
// the frame, and LOADK nil allocates nothing.
func (f *fn) retNil() {
	f.dropTrailingClear()
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
	f.scopeIn()
	f.stmts(list)
	f.scopeOut()
	f.next = mark
}

// stmt compiles s followed by a clear point (see fn). After a return the
// code is unreachable until a label, so nothing is cleared.
func (f *fn) stmt(s ast.Stmt) {
	f.stmtCode(s)
	if _, ok := s.(*ast.ReturnStmt); ok {
		f.dirty = 0 // unreachable until a label
		return
	}
	f.clearDead()
}

func (f *fn) stmtCode(s ast.Stmt) {
	switch s := s.(type) {
	case *ast.BlockStmt:
		f.block(s.List)
	case *ast.EmptyStmt:
	case *ast.ExprStmt:
		mark := f.next
		if n := tupleLen(f.g.p.Info.TypeOf(s.X)); n > 1 { // results discarded
			f.multiCallInto(s.X, f.allocN(s, n), nil)
		} else {
			f.exprInto(s.X, f.allocFor(s.X))
		}
		f.next = mark
	case *ast.AssignStmt:
		f.assign(s)
	case *ast.IncDecStmt:
		op := OpAdd
		if s.Tok == token.DEC {
			op = OpSub
		}
		if ix, ok := ast.Unparen(s.X).(*ast.IndexExpr); ok && !f.isFieldTarget(ix) { // s[i]++
			f.indexOpAssign(ix, op, nil)
			return
		}
		if f.isFieldTarget(s.X) { // x.F++
			f.fieldOpAssign(s.X, op, nil)
			return
		}
		x := f.localOf(s.X)
		mark := f.next
		one := f.allocScalar(s)
		f.oneInto(s.X, one)
		f.arith(s.X, op, x, x, one)
		f.next = mark
	case *ast.DeclStmt:
		f.declStmt(s.Decl.(*ast.GenDecl))
	case *ast.DeferStmt:
		f.deferStmt(s)
	case *ast.ReturnStmt:
		k := len(f.results)
		switch {
		case len(s.Results) == 0 && f.named >= 0: // bare return of named results
			f.runDefers()
			f.retNamed(s, k)
			return
		case len(s.Results) == 0:
			f.runDefers()
			f.retNil()
			return
		case f.named >= 0 && f.defers != nil:
			f.retToNamed(s) // the results are set before the deferred calls run
			return
		case k > 1:
			// return a, b (each converted to its result type) or return
			// g() of a multi-result g: the values go to k consecutive
			// fresh registers.
			mark := f.next
			base := f.allocN(s, k)
			if len(s.Results) == 1 {
				f.multiCallInto(s.Results[0], base, f.results)
			} else {
				for i, res := range s.Results {
					f.valueInto(res, f.results[i], base+uint8(i))
				}
			}
			f.runDefers()
			f.ret(base, k)
			f.next = mark
			return
		}
		mark := f.next
		res := s.Results[0]
		var r uint8
		if f.boxes(res, f.result) || f.copiesArray(res, f.result) {
			r = f.alloc(res)
			f.valueInto(res, f.result, r)
		} else {
			r = f.operand(res)
		}
		f.runDefers()
		f.asm.Emit(OpRet, r, 0, 0)
		f.next = mark
	case *ast.IfStmt:
		f.ifStmt(s)
	case *ast.ForStmt:
		f.forStmt(s)
	case *ast.BranchStmt:
		if s.Label != nil {
			f.labeledBranch(s)
			return
		}
		if s.Tok == token.FALLTHROUGH {
			f.fail(s, "internal error: fallthrough outside a switch clause end")
		}
		targets := f.breakTo
		if s.Tok == token.CONTINUE {
			targets = f.continueTo
		}
		if len(targets) == 0 {
			f.fail(s, "%s outside a loop", s.Tok)
		}
		f.jump(OpJmp, 0, targets[len(targets)-1])
	case *ast.LabeledStmt:
		f.label = s.Label.Name
		f.stmtCode(s.Stmt)
		f.label = ""
	case *ast.SwitchStmt:
		f.switchStmt(s)
	case *ast.TypeSwitchStmt:
		f.typeSwitch(s)
	case *ast.RangeStmt:
		f.rangeStmt(s)
	default:
		f.fail(s, "internal error: unexpected statement %T", s)
	}
}

// ret returns the k values in registers r..r+k-1: RET for one, RETN for
// several.
func (f *fn) ret(r uint8, k int) {
	if k == 1 {
		f.asm.Emit(OpRet, r, 0, 0)
	} else {
		f.asm.Emit(OpRetn, r, uint8(k), 0)
	}
}

// tupleLen is the number of values of a multi-value type t (a call of a
// function with several results), else 0.
func tupleLen(t types.Type) int {
	if tup, ok := t.(*types.Tuple); ok {
		return tup.Len()
	}
	return 0
}

// multiCallInto compiles call, a call with several results (a function
// with several results or a two-result builtin), so that result i lands
// in base+i; base.. are fresh consecutive registers above every live
// local. Result i is converted to targets[i] (nil, or a nil entry: no
// conversion): an int64 going into any is boxed.
func (f *fn) multiCallInto(e ast.Expr, base uint8, targets []types.Type) {
	if ta, ok := ast.Unparen(e).(*ast.TypeAssertExpr); ok { // v, ok := x.(T)
		f.assertOkInto(ta, base)
		if len(targets) > 0 && convertsToIface(f.g.p.Info.TypeOf(ta.Type), targets[0]) {
			f.toIface(e, f.g.p.Info.TypeOf(ta.Type), base)
		}
		return
	}
	call, ok := ast.Unparen(e).(*ast.CallExpr)
	if !ok {
		f.fail(e, "internal error: multi-value expression is not a call")
	}
	if twoResult[f.stubOf(call)] {
		f.twoResultCall(call, base)
	} else {
		f.call(call, base) // CALL's ra is base: RETN fills base..
	}
	tup := f.g.p.Info.TypeOf(call).(*types.Tuple)
	for i := 0; i < tup.Len() && i < len(targets); i++ {
		if convertsToIface(tup.At(i).Type(), targets[i]) {
			f.toIface(call, tup.At(i).Type(), base+uint8(i))
		}
	}
}

// stubOf returns the stubName of the builtin function call invokes, or "".
func (f *fn) stubOf(call *ast.CallExpr) string {
	if sel, ok := ast.Unparen(call.Fun).(*ast.SelectorExpr); ok {
		obj, _ := f.g.p.Info.Uses[sel.Sel].(*types.Func)
		if name := f.g.p.stubName(obj); name != "" {
			return name
		}
		return fsMethod(f.g.p.Info, sel)
	}
	return ""
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
	token.AND_ASSIGN: OpAnd, token.OR_ASSIGN: OpOr, token.XOR_ASSIGN: OpXor,
	token.AND_NOT_ASSIGN: OpAndnot, token.SHL_ASSIGN: OpShl, token.SHR_ASSIGN: OpShr,
}

func (f *fn) assign(s *ast.AssignStmt) {
	if ix, ok := ast.Unparen(s.Lhs[0]).(*ast.IndexExpr); ok && s.Tok != token.ASSIGN && s.Tok != token.DEFINE && !f.isFieldTarget(ix) {
		f.indexOpAssign(ix, assignOps[s.Tok], s.Rhs[0]) // s[i] op= e
		return
	}
	if s.Tok != token.ASSIGN && s.Tok != token.DEFINE && f.isFieldTarget(s.Lhs[0]) {
		f.fieldOpAssign(s.Lhs[0], assignOps[s.Tok], s.Rhs[0]) // x.F op= e
		return
	}
	if len(s.Lhs) == 1 && len(s.Rhs) == 1 && f.isFieldTarget(s.Lhs[0]) {
		f.fieldAssign(s.Lhs[0], s.Rhs[0]) // x.F = e
		return
	}
	if len(s.Lhs) == 1 && len(s.Rhs) == 1 && s.Tok == token.ASSIGN && f.assignArray(s.Lhs[0], s.Rhs[0]) {
		return // a = e of an array variable: copied in place
	}
	if s.Tok == token.ADD_ASSIGN && isString(f.g.p.Info.TypeOf(s.Lhs[0])) { // s += e
		x := f.localOf(s.Lhs[0])
		mark := f.next
		f.seqCall(s, x, BStrConcat, f.moveFrom(x), f.into(s.Rhs[0]))
		f.next = mark
		return
	}
	if op, ok := assignOps[s.Tok]; ok { // x op= e
		x := f.localOf(s.Lhs[0])
		mark := f.next
		f.arith(s.Lhs[0], op, x, x, f.rhsOperand(op, s.Rhs[0]))
		f.next = mark
		return
	}

	// Registers of the targets; -1 discards (blank identifier). Variables
	// declared here get their register before the right-hand sides are
	// evaluated, so the temporaries sit above them. An element target
	// s[i] gets three registers [s, i, v], s and i evaluated now (Go
	// evaluates the index operands on the left first), v filled at the
	// assignment; index[i] is its first register, or -1. A field target
	// x.F has its operands evaluated now too (fields[i]; see
	// fieldTargetOf) and is rebuilt at the assignment.
	targets := make([]int, len(s.Lhs))
	inPlace := make([]bool, len(s.Lhs)) // an array variable: its elements are copied over
	index := make([]int, len(s.Lhs))
	fields := make([]*fieldTarget, len(s.Lhs))
	var newVars []*types.Var
	var newRegs []uint8
	mark := f.next
	for i, l := range s.Lhs {
		index[i] = -1
		id, _ := ast.Unparen(l).(*ast.Ident)
		if f.isFieldTarget(l) { // x.F: its operands are evaluated now
			t := f.fieldTargetOf(l)
			fields[i] = &t
			continue
		}
		if ix, ok := ast.Unparen(l).(*ast.IndexExpr); ok {
			blk := f.allocN(l, 3)
			f.exprInto(ix.X, blk)
			f.exprInto(ix.Index, blk+1)
			index[i] = int(blk)
			targets[i] = int(blk + 2)
			continue
		}
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
			inPlace[i] = isArray(f.g.p.Info.TypeOf(l))
		}
	}
	if s.Tok == token.DEFINE {
		mark = f.next // the new locals stay (:= has no element targets)
	}
	// store assigns target i from register r.
	store := func(i int, r uint8) {
		switch {
		case fields[i] != nil:
			f.storeField(s.Lhs[i], *fields[i], func(dst, _ uint8) { f.asm.Emit(OpMove, dst, r, 0) })
		case index[i] >= 0:
			blk := uint8(index[i])
			if r != blk+2 {
				f.asm.Emit(OpMove, blk+2, r, 0)
			}
			f.asm.EmitExt(OpBuiltin, blk, blk, 3, BSetindex)
		case inPlace[i]:
			f.copyElems(s.Lhs[i], uint8(targets[i]), r)
		case targets[i] >= 0 && uint8(targets[i]) != r:
			f.asm.Emit(OpMove, uint8(targets[i]), r, 0)
		}
	}
	if len(s.Lhs) > 1 && len(s.Rhs) == 1 {
		// a, b := f(...) or actor.X(...): the call writes fresh
		// temporaries, above every live local, which then move to the
		// targets.
		lhsTypes := make([]types.Type, len(s.Lhs))
		for i, l := range s.Lhs {
			lhsTypes[i] = f.g.p.Info.TypeOf(l)
		}
		base := f.allocN(s, len(s.Lhs))
		f.multiCallInto(s.Rhs[0], base, lhsTypes)
		for i := range targets {
			store(i, base+uint8(i))
		}
	} else if len(s.Lhs) == 1 && (len(newVars) == 1 || index[0] >= 0) {
		// A fresh variable cannot be read by its own initializer; an
		// element's value goes straight into its v register.
		f.valueInto(s.Rhs[0], f.g.p.Info.TypeOf(s.Lhs[0]), uint8(targets[0]))
		store(0, uint8(targets[0]))
	} else {
		// Evaluate every right-hand side before assigning any (a, b = b, a).
		base := f.allocN(s, len(s.Rhs))
		for i, r := range s.Rhs {
			f.valueInto(r, f.g.p.Info.TypeOf(s.Lhs[i]), base+uint8(i))
		}
		for i := range targets {
			store(i, base+uint8(i))
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
				var t types.Type
				if vars[i] != nil {
					t = vars[i].Type()
				}
				f.valueInto(vs.Values[i], t, regs[i])
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
	f.jump(OpJmpf, f.operand(s.Cond), elseL)
	f.next = mark
	f.block(s.Body.List)
	if s.Else == nil {
		f.bindEnd(elseL)
	} else {
		endL := f.asm.NewLabel()
		f.jump(OpJmp, 0, endL)
		f.bind(elseL)
		f.stmt(s.Else)
		f.bindEnd(endL)
	}
	f.next = scope
}

func (f *fn) forStmt(s *ast.ForStmt) {
	label := f.takeLabel()
	scope := f.next
	if s.Init != nil {
		f.stmt(s.Init)
	}
	topL, contL, endL := f.asm.NewLabel(), f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(topL)
	if s.Cond != nil {
		mark := f.next
		f.jump(OpJmpf, f.operand(s.Cond), endL)
		f.next = mark
	}
	f.breakTo = append(f.breakTo, endL)
	f.continueTo = append(f.continueTo, contL)
	f.setLabel(label, endL, contL)
	f.block(s.Body.List)
	f.breakTo = f.breakTo[:len(f.breakTo)-1]
	f.continueTo = f.continueTo[:len(f.continueTo)-1]
	f.dropLabel(label)
	if s.Post != nil {
		f.bind(contL)
		f.stmt(s.Post)
	} else {
		f.bindEnd(contL)
	}
	f.clearDead() // the body's locals die before the next iteration
	f.jump(OpJmp, 0, topL)
	f.bindEnd(endL)
	f.next = scope
}

// ---- expressions ----

// operand returns a register holding e's value: a local's own register, or
// a fresh temporary that the caller releases.
func (f *fn) operand(e ast.Expr) uint8 {
	if r, ok := f.localReg(e); ok {
		return r
	}
	r := f.allocFor(e)
	f.exprInto(e, r)
	return r
}

// localReg returns the register of the local variable e names, if it is one.
func (f *fn) localReg(e ast.Expr) (uint8, bool) {
	if r, ok := f.preset(e); ok {
		return r, true
	}
	if id, ok := ast.Unparen(e).(*ast.Ident); ok {
		if v, ok := f.g.p.Info.Uses[id].(*types.Var); ok {
			if r, ok := f.locals[v]; ok {
				return r, true
			}
		}
	}
	return 0, false
}

// boxes reports whether e, going into a destination of type target, is
// converted to an interface value (nil is the nil interface value, not
// the nil slice): an int64 (small int64 values are
// immediates the VM cannot tell from ints, so inside an interface an int64
// is always a K_INT64 box, BOX64), a named non-struct value or a byte
// (boxed in a struct of their type, see codegen_iface.go).
func (f *fn) boxes(e ast.Expr, target types.Type) bool {
	return convertsToIface(f.g.p.Info.TypeOf(e), target) || (target != nil && isIface(target) && f.g.p.Info.Types[e].IsNil())
}

// valueInto compiles e into dst as a value of type target (the type of the
// variable, parameter, result or field it is assigned to; nil if unknown,
// e.g. the blank identifier), converting a value that goes into an
// interface (boxes). A constant is boxed at compile time, in the const
// area.
func (f *fn) valueInto(e ast.Expr, target types.Type, dst uint8) {
	if target != nil && isIface(target) && f.g.p.Info.Types[e].IsNil() { // the nil interface value
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNil)
		return
	}
	if !f.boxes(e, target) {
		f.exprInto(e, dst)
		f.copyArray(e, target, dst)
		return
	}
	if t := f.g.p.Info.TypeOf(e); boxed(t) {
		if tv := f.g.p.Info.Types[e]; tv.Value != nil {
			f.constBox(e, tv.Value, dst)
			return
		}
		mark := f.next
		r := f.operand(e) // a local in place, else a temporary
		f.asm.EmitExt(OpNewstruct, dst, r, 1, f.g.boxIndex(t))
		f.next = mark
		return
	}
	if tv := f.g.p.Info.Types[e]; tv.Value != nil {
		n, exact := constant.Int64Val(tv.Value)
		if !exact {
			f.fail(e, "constant %s overflows int64", tv.Value)
		}
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.b.AddInt64(n)))
		return
	}
	if r, ok := f.localReg(e); ok {
		f.asm.Emit(OpBox64, dst, r, 0)
		return
	}
	f.exprInto(e, dst)
	f.asm.Emit(OpBox64, dst, dst, 0)
}

// int64Ops maps each generic arithmetic/compare op to its int64 twin.
var int64Ops = map[uint8]uint8{
	OpAdd: OpAdd64, OpSub: OpSub64, OpMul: OpMul64, OpDiv: OpDiv64, OpRem: OpRem64,
	OpAnd: OpAnd64, OpOr: OpOr64, OpXor: OpXor64, OpAndnot: OpAndnot64,
	OpShl: OpShl64, OpShr: OpShr64,
	OpNeg: OpNeg64, OpEq: OpEq64, OpNe: OpNe64, OpLt: OpLt64, OpLe: OpLe64,
}

// typedOp returns op, or its int64, uint64 or float twin when operand x
// has static type int64, uint64 or float32 / float64 (or a named type of
// one): the generic ops are int-only. (A uint32's division, remainder,
// ordering and >> are not one op: see arith.)
func (f *fn) typedOp(op uint8, x ast.Expr) uint8 {
	t := f.g.p.Info.TypeOf(x)
	if opf, ok := floatOpsOf[op]; ok && isFloat(t) {
		return opf
	}
	if opu, ok := uint64Ops[op]; ok && isUint64(t) {
		return opu
	}
	if op64, ok := int64Ops[op]; ok && isWide(t) {
		return op64
	}
	return op
}

var binaryOps = map[token.Token]uint8{
	token.ADD: OpAdd, token.SUB: OpSub, token.MUL: OpMul, token.QUO: OpDiv, token.REM: OpRem,
	token.EQL: OpEq, token.NEQ: OpNe, token.LSS: OpLt, token.LEQ: OpLe,
	token.AND: OpAnd, token.OR: OpOr, token.XOR: OpXor, token.AND_NOT: OpAndnot,
	token.SHL: OpShl, token.SHR: OpShr,
}

// exprInto compiles e so that its value ends up in dst. Temporaries it
// allocates are released before it returns.
func (f *fn) exprInto(e ast.Expr, dst uint8) {
	if tv := f.g.p.Info.Types[e]; tv.Value != nil {
		f.constInto(e, tv.Value, dst)
		return
	}
	if r, ok := f.preset(e); ok { // evaluated at its defer statement
		if r != dst {
			f.asm.Emit(OpMove, dst, r, 0)
		}
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
		if _, ok := f.g.p.Info.Uses[e].(*types.Nil); ok { // the checker admits nil slices only
			f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNilSlice)
			return
		}
		if r := f.localOf(e); r != dst {
			f.asm.Emit(OpMove, dst, r, 0)
		}
	case *ast.UnaryExpr:
		if e.Op == token.XOR { // ^x == x XOR -1
			x := f.operand(e.X)
			m := f.allocScalar(e)
			if isWide(f.g.p.Info.TypeOf(e.X)) {
				f.int64Into(-1, m)
			} else {
				f.asm.EmitExt(OpLoadi32, m, 0, 0, uint32(0xffffffff))
			}
			f.asm.Emit(f.typedOp(OpXor, e.X), dst, x, m)
			f.trunc(e, dst)
			return
		}
		op := f.typedOp(OpNeg, e.X)
		if e.Op == token.NOT {
			op = OpNot
		}
		f.asm.Emit(op, dst, f.operand(e.X), 0)
		f.trunc(e, dst)
	case *ast.BinaryExpr:
		f.binaryInto(e, dst)
	case *ast.SelectorExpr:
		if m := f.g.p.method(e); m != nil {
			f.methodValueInto(e, m, dst)
			return
		}
		if fn, ok := f.g.p.Info.Uses[e.Sel].(*types.Func); ok {
			f.funcValueInto(e, fn, dst) // lib.F
			return
		}
		sel := f.g.p.Info.Selections[e]
		if sel == nil || sel.Kind() != types.FieldVal || sel.Indirect() {
			f.fail(e, "internal error: unexpected selector")
		}
		f.fieldPathInto(e, sel.Index(), f.operand(e.X), dst) // promoted: down the embedded fields
	case *ast.IndexExpr:
		f.indexInto(e, dst)
	case *ast.SliceExpr:
		f.sliceInto(e, dst)
	case *ast.CompositeLit:
		if isSlice(f.g.p.Info.TypeOf(e)) {
			f.sliceLit(e, dst)
			return
		}
		if isArray(f.g.p.Info.TypeOf(e)) {
			f.arrayLit(e, dst)
			return
		}
		f.structLit(e, dst)
	case *ast.FuncLit:
		f.closureInto(e, dst)
	case *ast.CallExpr:
		f.call(e, dst)
	case *ast.TypeAssertExpr:
		f.assertInto(e, dst)
	default:
		f.fail(e, "internal error: unexpected expression %T", e)
	}
}

func (f *fn) binaryInto(e *ast.BinaryExpr, dst uint8) {
	if e.Op == token.EQL || e.Op == token.NEQ {
		if xt, yt := f.g.p.Info.TypeOf(e.X), f.g.p.Info.TypeOf(e.Y); needsCmp(xt) || needsCmp(yt) {
			f.cmpInto(e.X, e.Y, cmpType(xt, yt), binaryOps[e.Op], dst)
			return
		}
	}
	if f.strBinary(e, dst) {
		return
	}
	switch e.Op {
	case token.LAND, token.LOR: // short circuit: skip Y when X decides
		jump := OpJmpf
		if e.Op == token.LOR {
			jump = OpJmpt
		}
		endL := f.asm.NewLabel()
		f.exprInto(e.X, dst)
		f.jump(jump, dst, endL)
		f.exprInto(e.Y, dst)
		f.bind(endL)
	case token.GTR: // x > y  ==  y < x
		x, y := f.operand(e.X), f.operand(e.Y)
		f.arith(e.X, OpLt, dst, y, x)
	case token.GEQ: // x >= y  ==  y <= x
		x, y := f.operand(e.X), f.operand(e.Y)
		f.arith(e.X, OpLe, dst, y, x)
	default:
		op, ok := binaryOps[e.Op]
		if !ok {
			f.fail(e, "internal error: unexpected operator %s", e.Op)
		}
		x := f.operand(e.X)
		y := f.rhsOperand(op, e.Y)
		f.arith(e.X, op, dst, x, y)
	}
}

// constInto loads the constant v (of e's type) into dst.
func (f *fn) constInto(e ast.Expr, v constant.Value, dst uint8) {
	if t := f.g.p.Info.TypeOf(e); isFloat(t) {
		f.floatInto(v, t, dst)
		return
	}
	if v.Kind() == constant.Float { // an integral float constant of an integer type
		v = constant.ToInt(v)
	}
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
		t := f.g.p.Info.TypeOf(e)
		n, ok := constBits(v, t)
		if !ok {
			f.fail(e, "constant %s overflows %s", v, t)
		}
		if isWide(t) {
			f.int64Into(n, dst)
			return
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

// int64Into loads the int64 n into dst: an immediate (LOADI32, no
// allocation) when it fits in 31 bits, else a const-area K_INT64 box.
func (f *fn) int64Into(n int64, dst uint8) {
	if n >= -(1<<30) && n < 1<<30 {
		f.asm.EmitExt(OpLoadi32, dst, 0, 0, uint32(int32(n)))
		return
	}
	f.asm.EmitExt(OpLoadk, dst, 0, 0, TermConst(f.g.b.AddInt64(n)))
}

// isBool reports whether t's underlying type is bool (or untyped bool).
func isBool(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Info()&types.IsBoolean != 0
}

// isInt64 reports whether t's underlying type is int64.
func isInt64(t types.Type) bool {
	b, ok := underlying(t).(*types.Basic)
	return ok && b.Kind() == types.Int64
}

// conversion compiles a conversion between numeric types, string(b) or
// []byte(s) (the only conversions the checker allows) into dst.
func (f *fn) conversion(call *ast.CallExpr, dst uint8) {
	to, from := f.g.p.Info.TypeOf(call.Fun), f.g.p.Info.TypeOf(call.Args[0])
	switch {
	case isIface(to): // any(x), I(x): as an assignment
		f.valueInto(call.Args[0], to, dst)
		return
	case isPID(to): // actor.PID <-> fs.File: the same term
		f.exprInto(call.Args[0], dst)
		return
	case isString(to) && isString(from), isBool(to): // Name(s), string(n), Flag(b): the same value
		f.exprInto(call.Args[0], dst)
		return
	case isString(to): // string(b []byte)
		f.seqCall(call, dst, BStrFrom, f.into(call.Args[0]))
		return
	case isSlice(to): // []byte(s)
		f.seqCall(call, dst, BBytesliceFrom, f.into(call.Args[0]))
		return
	}
	if isFloat(to) || isFloat(from) {
		f.floatConversion(call, to, from, dst)
		return
	}
	c, ok := intConvMode(to, from)
	if !ok { // the same term
		f.exprInto(call.Args[0], dst)
		return
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
	if at := arrayType(t); at != nil {
		f.zeroArray(n, at, dst)
		return
	}
	switch {
	case isPID(t): // typed: no actor or port has it (runtime/vm/term.h)
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermZeroPid)
		return
	case isRef(t): // typed: no live ref has serial 0 (runtime/vm/term.h)
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermZeroRef)
		return
	case isSlice(t):
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNilSlice)
		return
	case isBytes(t): // empty
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermEmptyBytes)
		return
	case isIface(t) || isFunc(t) || isBuffer(t):
		// The zero Buffer is nil: unusable (its methods are badcode).
		// A nil any; a nil func, which Spawn rejects (the spawner crashes
		// with badcode).
		f.asm.EmitExt(OpLoadk, dst, 0, 0, TermNil)
		return
	}
	b, ok := t.Underlying().(*types.Basic)
	switch {
	case ok && b.Info()&types.IsFloat != 0: // a const-area 0.0 of t's width
		f.floatInto(constant.MakeInt64(0), t, dst)
	case ok && isWide(b):
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
	idx, ok := f.g.typeIndex(st)
	if !ok {
		f.fail(n, "internal error: struct type %s has no index", st.Obj().Name())
	}
	mark := f.next
	base := f.allocN(n, nf)
	set := make([]bool, nf)
	for _, v := range vals {
		f.valueInto(v.val, fields.Field(v.field).Type(), base+uint8(v.field))
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

// args evaluates call arguments into consecutive fresh registers, as
// values of the callee's parameter types (an int64 passed as `any` is
// boxed): user and library functions and the gogo/actor builtins alike.
func (f *fn) args(call *ast.CallExpr) (base uint8, n uint8) {
	var params *types.Tuple
	if sig, ok := f.g.p.Info.TypeOf(call.Fun).(*types.Signature); ok {
		if sig.Variadic() {
			return f.variadicArgs(call, sig)
		}
		params = sig.Params()
	}
	return f.argsAs(call, params)
}

// variadicArgs is args for a call of a variadic function: the fixed
// arguments go into consecutive fresh registers, as args does, and the
// last register gets the variadic parameter's slice. The caller builds it
// from the remaining arguments (a new slice, or the nil slice when there
// are none), or passes the slice of f(s...) as it is, as Go does.
func (f *fn) variadicArgs(call *ast.CallExpr, sig *types.Signature) (base uint8, n uint8) {
	np := sig.Params().Len()
	if np > maxRegs || len(call.Args) > maxRegs {
		f.fail(call, "too many arguments")
	}
	fixed := call.Args[:np-1]
	base = uint8(f.next)
	for _, a := range fixed {
		f.allocFor(a)
	}
	last := f.alloc(call)
	for i, a := range fixed {
		f.valueInto(a, sig.Params().At(i).Type(), base+uint8(i))
	}
	rest := call.Args[np-1:]
	switch {
	case call.Ellipsis.IsValid(): // f(xs, s...)
		f.exprInto(rest[0], last)
	case len(rest) == 0:
		f.asm.EmitExt(OpLoadk, last, 0, 0, TermNilSlice)
	default:
		f.sliceOf(call, sliceElem(sig.Params().At(np-1).Type()), rest, last)
	}
	return base, uint8(np)
}

// argsAs is args with the parameter types given (nil: no conversions).
func (f *fn) argsAs(call *ast.CallExpr, params *types.Tuple) (base uint8, n uint8) {
	if len(call.Args) > maxRegs {
		f.fail(call, "too many arguments")
	}
	if len(call.Args) == 1 {
		if n := tupleLen(f.g.p.Info.TypeOf(call.Args[0])); n > 1 { // f(g())
			var targets []types.Type
			if params != nil {
				targets = tupleTypes(params)
			}
			base = f.allocN(call, n)
			f.multiCallInto(call.Args[0], base, targets)
			return base, uint8(n)
		}
	}
	// One register per argument, consecutive; scalar ones (constants,
	// bools, PIDs) are not marked dirty.
	base = uint8(f.next)
	for _, a := range call.Args {
		f.allocFor(a)
	}
	for i, a := range call.Args {
		var t types.Type
		if params != nil && i < params.Len() {
			t = params.At(i).Type()
		}
		f.valueInto(a, t, base+uint8(i))
	}
	return base, uint8(len(call.Args))
}

func (f *fn) call(call *ast.CallExpr, dst uint8) {
	if tv := f.g.p.Info.Types[call.Fun]; tv.IsType() {
		f.conversion(call, dst)
		return
	}
	if funcValueCall(f.g.p.Info, call) {
		f.callValue(call, dst)
		return
	}
	switch fun := ast.Unparen(call.Fun).(type) {
	case *ast.Ident:
		switch obj := f.g.p.Info.Uses[fun].(type) {
		case *types.Builtin: // println, panic and the sequence builtins
			if f.seqBuiltin(call, obj.Name(), dst) {
				return
			}
			if obj.Name() == "panic" {
				f.panicCall(call, dst)
				return
			}
			if f.printlnUnsigned(call, dst) {
				return
			}
			base, n := f.argsAs(call, nil) // prints an int64 in either representation
			f.asm.EmitExt(OpBuiltin, dst, base, n, BPrintln)
			return
		case *types.Func:
			idx, ok := f.g.funcIndex(obj)
			if !ok {
				f.fail(call, "internal error: unknown function %s", obj.Name())
			}
			base, n := f.args(call)
			f.asm.EmitExt(OpCall, dst, base, n, idx)
			return
		}
	case *ast.SelectorExpr:
		if m := f.g.p.method(fun); m != nil {
			f.methodCall(call, fun, m, dst)
			return
		}
		if name := bytesMethod(f.g.p.Info, fun); name != "" {
			f.bytesMethodCall(call, fun, name, dst)
			return
		}
		if name := fsMethod(f.g.p.Info, fun); name != "" {
			f.fsCall(call, name, dst)
			return
		}
		obj, ok := f.g.p.Info.Uses[fun.Sel].(*types.Func)
		if name := f.g.p.stubName(obj); ok && name != "" {
			if _, isFs := fsOps[name]; isFs {
				f.fsCall(call, name, dst)
				return
			}
			if _, isCode := codeOps[name]; isCode {
				f.codeCall(call, name, dst)
				return
			}
			if _, isFloat := floatOps[name]; isFloat {
				f.floatCall(call, name, dst)
				return
			}
			f.actorCall(call, name, dst)
			return
		}
		if idx, found := f.g.funcIndex(obj); ok && found { // lib.F(...)
			base, n := f.args(call)
			f.asm.EmitExt(OpCall, dst, base, n, idx)
			return
		}
	}
	f.fail(call, "internal error: unexpected call")
}

// panicCall compiles panic(v): B_PANIC prints v as println would, on
// stderr after "panic: ", and ends the actor with reason actor.Panic. It
// does not return.
func (f *fn) panicCall(call *ast.CallExpr, dst uint8) {
	mark := f.next
	v := f.alloc(call)
	t := f.g.p.Info.TypeOf(call.Args[0])
	if isIface(t) || types.Implements(t, errorType.Underlying().(*types.Interface)) {
		f.exprInto(call.Args[0], v)
		f.panicValue(call, t, v)
	} else {
		f.valueInto(call.Args[0], anyType, v)
	}
	f.asm.EmitExt(OpBuiltin, dst, v, 1, BPanic)
	f.next = mark
}

// callValue compiles a call of a func value into dst: the closure goes into
// a fresh register and the arguments right above it, then CALLV (whose
// results land in dst.., as CALL's do). A nil func value is badcode.
func (f *fn) callValue(call *ast.CallExpr, dst uint8) {
	mark := f.next
	clo := f.alloc(call.Fun)
	f.exprInto(call.Fun, clo)
	base, n := f.args(call)
	if n > 0 && base != clo+1 {
		f.fail(call, "internal error: call arguments not above the func value")
	}
	f.asm.Emit(OpCallv, dst, clo, n)
	f.next = mark
}

// onDemandUsed returns the functions of //gogo:ondemand files that the
// program refers to: from any other file, or from the body of an on-demand
// function it refers to (transitively). A reference is any use of the
// function's name: a call, or a func value.
func onDemandUsed(p *Program) map[*types.Func]bool {
	decls := map[*types.Func]*ast.FuncDecl{}
	var others []*ast.File
	for _, pkg := range p.packages() {
		for _, f := range pkg.Files {
			if !isOnDemand(f) {
				others = append(others, f)
				continue
			}
			for _, d := range f.Decls {
				if fd, ok := d.(*ast.FuncDecl); ok {
					decls[p.Info.Defs[fd.Name].(*types.Func)] = fd
				}
			}
		}
	}
	used := map[*types.Func]bool{}
	if len(decls) == 0 {
		return used
	}
	var visit func(n ast.Node)
	visit = func(n ast.Node) {
		ast.Inspect(n, func(n ast.Node) bool {
			if id, ok := n.(*ast.Ident); ok {
				if fn, ok := p.Info.Uses[id].(*types.Func); ok && decls[fn] != nil && !used[fn] {
					used[fn] = true
					visit(decls[fn])
				}
			}
			return true
		})
	}
	for _, f := range others {
		visit(f)
	}
	return used
}
