// Package compiler compiles the gogo subset of Go to VM images.
package compiler

import (
	"bytes"
	"errors"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"go/types"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"unicode"

	"gogotools/lib"
)

// The system packages: gogo libraries that declare builtins (functions
// and types the VM implements, //gogo:builtin) or reserved types
// (//gogo:reserved N). Their paths are named here because the compiler
// maps their builtins by package and name (stubName, fsMethod,
// bytesMethod) and their reserved types by index (reservedTypes); which
// declaration is which is marked in the source (see marks).
const (
	// ActorPath: the actor API; its calls become builtins.
	ActorPath = "gogo/actor"
	// PortPath: the port (peripheral) API and the stream protocol types.
	PortPath = "gogo/port"
	// BytesPath: bytes.Bytes and bytes.Buffer, VM objects.
	BytesPath = "gogo/bytes"
	// NetPath: the network port configs, commands, events, Subscribe.
	NetPath = "gogo/net"
	// GpioPath: the GPIO port types and their mode and edge constants.
	GpioPath = "gogo/gpio"
	// FsPath: the storage API: calls become B_FS, File.Close the port
	// close builtin.
	FsPath = "gogo/fs"
	// KbdPath and PowerPath: the keyboard and battery port types.
	KbdPath   = "gogo/kbd"
	PowerPath = "gogo/power"
	// CodePath: the hot-loading API: calls become B_CODE.
	CodePath = "gogo/code"
	// ImuPath and IrPath: the IMU and infrared port types.
	ImuPath = "gogo/imu"
	IrPath  = "gogo/ir"
	// SysPath: sys.Time, sys.SetTime, sys.Random, sys.Config.
	SysPath = "gogo/sys"
	// AudioPath: the audio port types (the recording and playback
	// library is gogo/sound).
	AudioPath = "gogo/audio"
	// ScreenPath: the text screen port types (the text UI library is
	// gogo/tui).
	ScreenPath = "gogo/screen"
	// ErrorsPath: the shared error codes (errors.Code).
	ErrorsPath = "gogo/errors"
	// MathPath: the math library: calls become B_FLOAT (runtime/floatlib).
	MathPath = "gogo/math"
)

// StrconvPath is the import path of gogo/strconv, a library package whose
// float functions (FormatFloat, ParseFloat) the compiler implements itself
// (intrinsics; see stubName): they are B_FLOAT calls and their bodies are
// never compiled, so a program that imports strconv without them neither
// requires "float" nor needs the image format minor of floats.
const StrconvPath = "gogo/strconv"

// libPrefix starts the import path of every gogo library package.
const libPrefix = "gogo/"

// SourceExt is the file name extension of gogo source files: a package is
// the *.gg files of its directory (Go-subset source, parsed with go/parser).
const SourceExt = ".gg"

// Program is a parsed and type-checked gogo program: package main plus the
// library and user packages it imports.
type Program struct {
	Fset  *token.FileSet
	Files []*ast.File    // package main
	Pkg   *types.Package // package main
	Info  *types.Info    // type information of every compiled package
	Libs  []*Package     // compiled library and user packages, in dependency order
	// Optional holds the drivers named by package main's //gogo:optional
	// directives (see directives).
	Optional map[string]bool
	// Warnings lists the user packages found in more than one place of
	// the search (see resolve), one message each.
	Warnings []string
	// marks holds the //gogo:builtin and //gogo:reserved declarations of
	// the libraries (see loader.marks).
	marks
}

// marks are the per-declaration directives of the system packages:
// builtin holds the functions, methods and types the VM implements
// (//gogo:builtin: the compiler turns a call into a builtin or an
// instruction and never compiles the body; a builtin type is a VM object
// the checker does not look into); reserved the struct types with a
// fixed type index (//gogo:reserved N, the index in reservedTypes);
// system the packages holding either. A system package may hold library
// code too (functions with bodies, such as methods of its types): it is
// compiled only where the program uses it (see gen.funcIndex), so that
// importing a system package costs nothing by itself.
type marks struct {
	builtin  map[types.Object]bool
	reserved map[*types.TypeName]int
	system   map[*types.Package]bool
}

// Package is one compiled package (a library, or main in packages()).
type Package struct {
	Pkg   *types.Package
	Files []*ast.File
	// Optional holds the drivers named by the package's //gogo:optional
	// directives (see directives).
	Optional map[string]bool
}

// packages lists every compiled package: the libraries, then main.
func (p *Program) packages() []*Package {
	return append(append([]*Package{}, p.Libs...), &Package{Pkg: p.Pkg, Files: p.Files, Optional: p.Optional})
}

// compiled reports whether pkg is compiled into the image (main, a
// library or a user package; a system package's builtins are not, see
// marks).
func (p *Program) compiled(pkg *types.Package) bool {
	if pkg == p.Pkg {
		return true
	}
	return p.isLib(pkg)
}

// isLib reports whether pkg is one of the compiled library packages.
func (p *Program) isLib(pkg *types.Package) bool {
	for _, l := range p.Libs {
		if l.Pkg == pkg {
			return true
		}
	}
	return false
}

// sizes makes int 32 bits, as on the target.
func sizes() types.Sizes { return &types.StdSizes{WordSize: 4, MaxAlign: 4} }

// loader parses and type-checks the packages a program imports: library
// packages from libs, a file system with one directory per package
// (gogo/<name> lives in <name>/*.gg), and user packages, every import path
// not under gogo/, from directories on disk (see resolve). It is also the
// importer of every package it checks; it serves only packages it has
// already loaded.
type loader struct {
	fset  *token.FileSet
	libs  fs.FS
	info  *types.Info
	pkgs  map[string]*types.Package // checked packages by import path
	order []*Package                // compiled packages (libraries and user packages) in load order
	// root is the build root as given (file names of user packages are
	// joined to it), rootName the base name of its absolute path: the
	// first element of the import paths it serves.
	root, rootName string
	path           []string // the search path after the root (Config.Path)
	warnings       []string // packages found in more than one place
	dirs           []pkgDir // the directories of the user packages resolved so far
	marks                   // the libraries' builtin and reserved declarations
}

// pkgDir is the directory of a resolved user package.
type pkgDir struct {
	path string
	info os.FileInfo
}

func (l *loader) Import(path string) (*types.Package, error) {
	if pkg := l.pkgs[path]; pkg != nil {
		return pkg, nil
	}
	return nil, fmt.Errorf("import %q not supported", path)
}

// parseFile parses one gogo source file. A first line starting with #!
// (an executable script's interpreter line) is read as an empty line, so
// line numbers stay those of the file.
func parseFile(fset *token.FileSet, name string, src []byte) (*ast.File, error) {
	if bytes.HasPrefix(src, []byte("#!")) {
		if i := bytes.IndexByte(src, '\n'); i >= 0 {
			src = src[i:]
		} else {
			src = nil
		}
	}
	return parser.ParseFile(fset, name, src, parser.ParseComments)
}

// parseLibDir parses the gogo source files of directory dir of the library
// set, in file name order.
func (l *loader) parseLibDir(dir string) ([]*ast.File, error) {
	names, err := fs.Glob(l.libs, dir+"/*"+SourceExt)
	if err != nil {
		return nil, err
	}
	var files []*ast.File
	for _, name := range names {
		src, err := fs.ReadFile(l.libs, name)
		if err != nil {
			return nil, err
		}
		f, err := parseFile(l.fset, name, src)
		if err != nil {
			return nil, err
		}
		files = append(files, f)
	}
	if len(files) == 0 {
		return nil, fmt.Errorf("gogo: library %s%s has no %s files", libPrefix, dir, SourceExt)
	}
	return files, nil
}

// isLib reports whether path names a library package: gogo/<name> with
// gogo source files in <name>/.
func (l *loader) isLib(path string) bool {
	name, ok := strings.CutPrefix(path, libPrefix)
	if !ok || name == "" || name == "main" || strings.ContainsAny(name, "/.*?[\\") {
		return false
	}
	names, err := fs.Glob(l.libs, name+"/*"+SourceExt)
	return err == nil && len(names) > 0
}

// checkImports rejects every import of f that allowed does not admit, and
// renamed imports.
func (l *loader) checkImports(f *ast.File, allowed func(string) bool, why string) error {
	for _, imp := range f.Imports {
		p, err := strconv.Unquote(imp.Path.Value)
		if err != nil || imp.Name != nil || !allowed(p) {
			what := "import " + imp.Path.Value
			if imp.Name != nil {
				what = "import " + imp.Name.Name + " " + imp.Path.Value
			}
			return fmt.Errorf("%s: gogo: %s not supported%s", l.fset.Position(imp.Pos()), what, why)
		}
	}
	return nil
}

// isUser reports whether import path names a user package: every path not
// under gogo/ (see resolve).
func isUser(path string) bool { return !strings.HasPrefix(path, libPrefix) }

// importable reports whether a user package or package main may import
// path: a library, or a user package (resolved when loaded).
func (l *loader) importable(path string) bool {
	return l.isLib(path) || isUser(path)
}

// loadPkg parses and type-checks package path, a library or a user
// package, first loading the packages it imports (depth first, so l.order
// is a dependency order). A library may import other libraries, a user package those and other user packages, without cycles;
// stack is the chain of packages being loaded, for the cycle error, and at
// the import that names path (token.NoPos for none).
func (l *loader) loadPkg(path string, stack []string, at token.Pos) error {
	if l.pkgs[path] != nil {
		return nil
	}
	for i, p := range stack {
		if p == path {
			return fmt.Errorf("%sgogo: import cycle %s", l.where(at), strings.Join(append(stack[i:], path), " -> "))
		}
	}
	stack = append(stack, path)
	var files []*ast.File
	var name string
	allowed := l.isLib
	if isUser(path) {
		dir, err := l.resolve(path, at)
		if err != nil {
			return err
		}
		if files, err = l.parseUserDir(dir); err != nil {
			return err
		}
		name = path[strings.LastIndex(path, "/")+1:]
		allowed = l.importable
		for _, f := range files {
			if f.Name.Name == "main" {
				return fmt.Errorf("%s: gogo: package main in %s: only the program's own directory can be package main", l.fset.Position(f.Name.Pos()), path)
			}
			if f.Name.Name != name {
				return fmt.Errorf("%s: gogo: package %s in %s (want package %s, the name of its directory)", l.fset.Position(f.Name.Pos()), f.Name.Name, path, name)
			}
		}
	} else {
		name = strings.TrimPrefix(path, libPrefix)
		var err error
		if files, err = l.parseLibDir(name); err != nil {
			return err
		}
		for _, f := range files {
			if f.Name.Name != name {
				return fmt.Errorf("%s: gogo: package %s in library %s (want package %s)", l.fset.Position(f.Name.Pos()), f.Name.Name, path, name)
			}
		}
	}
	for _, f := range files {
		if err := l.checkImports(f, allowed, ""); err != nil {
			return err
		}
	}
	for _, f := range files {
		for _, imp := range f.Imports {
			p, _ := strconv.Unquote(imp.Path.Value)
			if err := l.loadPkg(p, stack, imp.Path.Pos()); err != nil {
				return err
			}
		}
	}
	optional, err := l.directives(files)
	if err != nil {
		return err
	}
	pkg, err := l.check(path, files)
	if err != nil {
		return err
	}
	if err := l.marks.add(l, path, pkg, files); err != nil {
		return err
	}
	l.pkgs[path] = pkg
	l.order = append(l.order, &Package{Pkg: pkg, Files: files, Optional: optional})
	return nil
}

// usesError reports whether a checked package names the type error.
func usesError(info *types.Info) bool {
	e := types.Universe.Lookup("error")
	for _, obj := range info.Uses {
		if obj == e {
			return true
		}
	}
	return false
}

// where is the "file:line:col: " prefix of an error at pos, or "".
func (l *loader) where(pos token.Pos) string {
	if !pos.IsValid() {
		return ""
	}
	return l.fset.Position(pos).String() + ": "
}

// validImportPath reports whether path is a usable user import path:
// slash-separated elements of letters, digits, '_', '-' and '.', none
// empty, none starting with '.'.
func validImportPath(path string) bool {
	if path == "" {
		return false
	}
	for _, elem := range strings.Split(path, "/") {
		if elem == "" || elem[0] == '.' {
			return false
		}
		for _, r := range elem {
			if !unicode.IsLetter(r) && !unicode.IsDigit(r) && r != '_' && r != '-' && r != '.' {
				return false
			}
		}
	}
	return true
}

// hasSources reports whether dir is a directory holding gogo source files.
func hasSources(dir string) bool {
	names, err := filepath.Glob(filepath.Join(dir, "*"+SourceExt))
	if err != nil {
		return false
	}
	for _, n := range names {
		if fi, err := os.Stat(n); err == nil && !fi.IsDir() {
			return true
		}
	}
	return false
}

// resolve finds the directory of user package path. The candidates, in
// order: <root>/<rest> when the first element of path is the root's name
// (import "app/util" in the build root app/), then <dir>/<path> for each
// dir of the search path. The first one holding .gg files wins; when more
// do, a warning names them all. at is the import, for the error.
func (l *loader) resolve(path string, at token.Pos) (string, error) {
	if !validImportPath(path) {
		return "", fmt.Errorf("%sgogo: import %q: bad import path (want slash-separated names like \"app/util\")", l.where(at), path)
	}
	last := path[strings.LastIndex(path, "/")+1:]
	if !token.IsIdentifier(last) || last == "_" {
		return "", fmt.Errorf("%sgogo: import %q: %q cannot be a package name (its directory's name must be a Go identifier, like %q)", l.where(at), path, last, "util")
	}
	// base is the directory the import path's elements (from rel on) are
	// names in, for checkCase.
	type candidate struct{ dir, from, base, rel string }
	var cands []candidate
	first, rest, _ := strings.Cut(path, "/")
	if first == l.rootName {
		if rest == "" {
			return "", fmt.Errorf("%sgogo: import %q: that is the program's own directory (package main), which cannot be imported", l.where(at), path)
		}
		cands = append(cands, candidate{filepath.Join(l.root, filepath.FromSlash(rest)), "build root", l.root, rest})
	}
	for _, d := range l.path {
		cands = append(cands, candidate{filepath.Join(d, filepath.FromSlash(path)), "search path", d, path})
	}
	// The places holding the package; a candidate that is the same
	// directory as an earlier one (the root inside a search path
	// directory, a symlink) is the same place.
	var found []string
	var foundInfo []os.FileInfo
	for _, c := range cands {
		if !hasSources(c.dir) {
			continue
		}
		fi, err := os.Stat(c.dir)
		if err != nil {
			continue
		}
		same := false
		for _, f := range foundInfo {
			same = same || os.SameFile(f, fi)
		}
		if same {
			continue
		}
		if err := checkCase(c.base, c.rel); err != nil {
			return "", fmt.Errorf("%sgogo: import %q: %v", l.where(at), path, err)
		}
		found = append(found, c.dir)
		foundInfo = append(foundInfo, fi)
	}
	if len(found) > 1 {
		l.warnings = append(l.warnings, fmt.Sprintf("package %q is in %d places; using the first: %s", path, len(found), strings.Join(found, ", ")))
	}
	if len(found) > 0 {
		for _, d := range l.dirs {
			if os.SameFile(d.info, foundInfo[0]) {
				return "", fmt.Errorf("%sgogo: import %q is the directory %s, already imported as %q: use one import path", l.where(at), path, found[0], d.path)
			}
		}
		l.dirs = append(l.dirs, pkgDir{path, foundInfo[0]})
		return found[0], nil
	}
	var b strings.Builder
	fmt.Fprintf(&b, "%sgogo: import %q: package not found; searched:", l.where(at), path)
	if first != l.rootName {
		fmt.Fprintf(&b, "\n\t%s (build root: serves only imports starting with %q)", l.root, l.rootName+"/")
	}
	for _, c := range cands {
		why := "no such directory"
		if fi, err := os.Stat(c.dir); err == nil {
			why = "no " + SourceExt + " files"
			if !fi.IsDir() {
				why = "not a directory"
			}
		}
		fmt.Fprintf(&b, "\n\t%s (%s: %s)", c.dir, c.from, why)
	}
	if len(l.path) == 0 {
		b.WriteString("\n\t(the search path is empty)")
	}
	if l.isLib(libPrefix + path) {
		fmt.Fprintf(&b, "\n(did you mean %q?)", libPrefix+path)
	}
	return "", errors.New(b.String())
}

// checkCase reports an error unless every element of the slash path rel
// is, letter case included, the name of a directory entry under base: on
// a file system that ignores case (macOS by default) a path that differs
// only in case would be found here, and not on Linux.
func checkCase(base, rel string) error {
	dir := base
	for _, elem := range strings.Split(rel, "/") {
		entries, err := os.ReadDir(dir)
		if err != nil {
			return nil // found by Stat; nothing to compare with
		}
		exact, other := false, ""
		for _, e := range entries {
			if e.Name() == elem {
				exact = true
				break
			}
			if strings.EqualFold(e.Name(), elem) {
				other = e.Name()
			}
		}
		if !exact && other != "" {
			return fmt.Errorf("the directory is %s on disk: import paths must match directory names, letter case included", filepath.Join(dir, other))
		}
		dir = filepath.Join(dir, elem)
	}
	return nil
}

// realName returns the base name of directory dir as its parent lists it
// (the letter case on disk, whatever case dir was typed in).
func realName(dir string) string {
	name := filepath.Base(dir)
	fi, err := os.Stat(dir)
	if err != nil {
		return name
	}
	entries, err := os.ReadDir(filepath.Dir(dir))
	if err != nil {
		return name
	}
	for _, e := range entries {
		if e.Name() == name {
			return name
		}
	}
	for _, e := range entries {
		if strings.EqualFold(e.Name(), name) {
			if efi, err := os.Stat(filepath.Join(filepath.Dir(dir), e.Name())); err == nil && os.SameFile(efi, fi) {
				return e.Name()
			}
		}
	}
	return name
}

// parseUserDir parses the gogo source files of directory dir (a user
// package), in file name order.
func (l *loader) parseUserDir(dir string) ([]*ast.File, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var files []*ast.File
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), SourceExt) {
			continue
		}
		name := filepath.Join(dir, e.Name())
		src, err := os.ReadFile(name)
		if err != nil {
			return nil, err
		}
		f, err := parseFile(l.fset, name, src)
		if err != nil {
			return nil, err
		}
		files = append(files, f)
	}
	return files, nil
}

// directivePrefix starts every gogo source directive. There are four:
//
//	//gogo:optional name...
//
// a comment line of a compiled package, a library or package main (any
// file; conventionally the last line of the package doc comment, after a
// blank // line, as gofmt places directives) naming port drivers the
// package opens only when a call asks for them at run time, and whose
// absence (port.Open's error) it handles as an ordinary failure.
// Requires then adds no requirement for that package's own constant
// port.Open of those drivers; importers and the other packages are
// unaffected. Any driver name is accepted (a board's or a host port's).
//
//	//gogo:ondemand
//
// a comment line in a file (of any compiled package; meant for libraries)
// whose top-level functions are compiled into an image only when the rest
// of the program refers to them, directly or through another such
// function (onDemandUsed in codegen.go). It takes no arguments, and the
// file may declare no methods. Without it every function of a compiled
// package is in the image, used or not.
//
//	//gogo:builtin
//	//gogo:reserved N
//
// the last line of the doc comment of one declaration of a gogo/ library
// (see marks): a function, method or type the VM implements, and a struct
// type with the fixed reserved index N.
const directivePrefix = "//gogo:"

// onDemandDirective marks a file whose functions are compiled only when
// used (see directivePrefix).
const onDemandDirective = directivePrefix + "ondemand"

// directives parses the //gogo: directives of one package's files and
// returns the drivers named by //gogo:optional. An unknown directive or an
// empty list is an error; where //gogo:builtin and //gogo:reserved stand
// is checked by marks.add.
func (l *loader) directives(files []*ast.File) (map[string]bool, error) {
	var optional map[string]bool
	for _, f := range files {
		for _, g := range f.Comments {
			for _, c := range g.List {
				rest, ok := strings.CutPrefix(c.Text, directivePrefix)
				if !ok {
					continue
				}
				pos := l.fset.Position(c.Pos())
				if c.Text == onDemandDirective {
					if err := onDemandMethods(f); err != nil {
						return nil, fmt.Errorf("%s: gogo: %v", l.fset.Position(err.(onDemandErr).pos), err)
					}
					continue
				}
				if name, _ := declDirective(c.Text); name != "" {
					continue
				}
				names, ok := strings.CutPrefix(rest, "optional")
				if !ok || names != "" && names[0] != ' ' && names[0] != '\t' {
					return nil, fmt.Errorf("%s: gogo: unknown directive %s (want //gogo:optional, //gogo:ondemand, //gogo:builtin or //gogo:reserved)", pos, strings.Fields(c.Text)[0])
				}
				fields := strings.Fields(names)
				if len(fields) == 0 {
					return nil, fmt.Errorf("%s: gogo: //gogo:optional names no driver", pos)
				}
				for _, d := range fields {
					if optional == nil {
						optional = map[string]bool{}
					}
					optional[d] = true
				}
			}
		}
	}
	return optional, nil
}

// declDirective returns the name of the per-declaration directive comment
// line text ("builtin" or "reserved") and its arguments, or "".
func declDirective(text string) (name string, args []string) {
	rest, ok := strings.CutPrefix(text, directivePrefix)
	if !ok {
		return "", nil
	}
	fields := strings.Fields(rest)
	if len(fields) == 0 || (fields[0] != "builtin" && fields[0] != "reserved") {
		return "", nil
	}
	return fields[0], fields[1:]
}

// add records the //gogo:builtin and //gogo:reserved N declarations of
// package path (checked as pkg from files). Each must be the last line of
// a declaration's doc comment: builtin on a function, a method or a type,
// reserved N on a struct type, in a gogo/ library only. A builtin
// function's body (panic("gogo only"), for the host toolchain) is never
// compiled.
func (m *marks) add(l *loader, path string, pkg *types.Package, files []*ast.File) error {
	attached := map[token.Pos]bool{}
	mark := func(doc *ast.CommentGroup, obj types.Object, isType bool) error {
		if doc == nil || obj == nil {
			return nil
		}
		c := doc.List[len(doc.List)-1]
		name, args := declDirective(c.Text)
		if name == "" {
			return nil
		}
		attached[c.Pos()] = true
		pos := l.fset.Position(c.Pos())
		if isUser(path) {
			return fmt.Errorf("%s: gogo: //gogo:%s is for the gogo/ libraries only", pos, name)
		}
		if m.builtin == nil {
			m.builtin, m.reserved, m.system = map[types.Object]bool{}, map[*types.TypeName]int{}, map[*types.Package]bool{}
		}
		m.system[pkg] = true
		switch {
		case name == "builtin" && len(args) == 0:
			m.builtin[obj] = true
			return nil
		case name == "reserved" && len(args) == 1 && isType:
			n, err := strconv.Atoi(args[0])
			if err == nil && n >= 0 {
				m.reserved[obj.(*types.TypeName)] = n
				return nil
			}
		}
		return fmt.Errorf("%s: gogo: bad directive %s (want //gogo:builtin, or //gogo:reserved N on a type)", pos, c.Text)
	}
	for _, f := range files {
		for _, d := range f.Decls {
			switch d := d.(type) {
			case *ast.FuncDecl:
				if err := mark(d.Doc, l.info.Defs[d.Name], false); err != nil {
					return err
				}
			case *ast.GenDecl:
				for _, s := range d.Specs {
					ts, ok := s.(*ast.TypeSpec)
					if !ok {
						continue
					}
					doc := ts.Doc
					if doc == nil && len(d.Specs) == 1 {
						doc = d.Doc
					}
					if err := mark(doc, l.info.Defs[ts.Name], true); err != nil {
						return err
					}
				}
			}
		}
		for _, g := range f.Comments {
			for _, c := range g.List {
				if name, _ := declDirective(c.Text); name != "" && !attached[c.Pos()] {
					return fmt.Errorf("%s: gogo: //gogo:%s must be the last line of a declaration's doc comment", l.fset.Position(c.Pos()), name)
				}
			}
		}
	}
	return nil
}

// isBuiltin reports whether obj is a //gogo:builtin declaration.
func (m *marks) isBuiltin(obj types.Object) bool { return m.builtin[obj] }

// check type-checks one compiled package, recording into l.info.
func (l *loader) check(path string, files []*ast.File) (*types.Package, error) {
	var errs []error
	conf := types.Config{
		Sizes:    sizes(),
		Importer: l,
		Error:    func(err error) { errs = append(errs, err) },
	}
	pkg, _ := conf.Check(path, l.fset, files, l.info)
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}
	return pkg, nil
}

// Config says how Load finds a program's packages.
type Config struct {
	// Libs is the library set (see loader); nil means the libraries
	// embedded in the compiler.
	Libs fs.FS
	// Path is the package search path: the directories searched, in
	// order, for a user package the build root does not hold (see
	// resolve). Load's is SearchPath().
	Path []string
}

// SearchPath is the default package search path: the directories of
// $GOGOPATH (separated by the OS's list separator, ':' or ';' on Windows),
// or $HOME/gogo when GOGOPATH is unset.
func SearchPath() []string {
	home, _ := os.UserHomeDir()
	gp, set := os.LookupEnv("GOGOPATH")
	return searchPath(gp, set, home)
}

func searchPath(gogopath string, set bool, home string) []string {
	if !set {
		if home == "" {
			return nil
		}
		return []string{filepath.Join(home, "gogo")}
	}
	return SplitPath(gogopath)
}

// SplitPath splits a search path list (GOGOPATH, gogo build -path) into
// its directories, made absolute; empty elements are dropped.
func SplitPath(list string) []string {
	var dirs []string
	for _, d := range filepath.SplitList(list) {
		if d == "" {
			continue
		}
		if abs, err := filepath.Abs(d); err == nil {
			d = abs
		}
		dirs = append(dirs, d)
	}
	return dirs
}

// Load parses and type-checks the package main in dir, with the libraries
// embedded in the compiler and user packages found from dir and
// SearchPath().
func Load(dir string) (*Program, error) { return Config{Path: SearchPath()}.Load(dir) }

// LoadWith is Load with the library set libs (see loader).
func LoadWith(dir string, libs fs.FS) (*Program, error) {
	return Config{Libs: libs, Path: SearchPath()}.Load(dir)
}

// Load parses and type-checks the package main in dir (the build root),
// and every package it imports.
func (c Config) Load(dir string) (*Program, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var names []string
	goFiles := 0
	for _, e := range entries {
		name := e.Name()
		if !e.IsDir() && strings.HasSuffix(name, ".go") {
			goFiles++
		}
		if !e.IsDir() && strings.HasSuffix(name, SourceExt) {
			names = append(names, name)
		}
	}
	return c.load(dir, names, goFiles)
}

// LoadFile is Load of a program of one file (gogo run x.gg): package main
// is that file alone, and the build root, where user packages are found,
// is its directory, whose other .gg files are not read.
func (c Config) LoadFile(file string) (*Program, error) {
	return c.load(filepath.Dir(file), []string{filepath.Base(file)}, 0)
}

// load is Load of the files names of dir as package main; goFiles counts
// the .go files of dir, for the error when there are no .gg files.
func (c Config) load(dir string, names []string, goFiles int) (*Program, error) {
	libs := c.Libs
	if libs == nil {
		libs = lib.StubFS
	}
	fset := token.NewFileSet()
	var files []*ast.File
	for _, name := range names {
		path := filepath.Join(dir, name)
		src, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		f, err := parseFile(fset, path, src)
		if err != nil {
			return nil, err
		}
		if f.Name.Name != "main" {
			return nil, fmt.Errorf("%s: gogo: package %s not supported (want package main)", fset.Position(f.Name.Pos()), f.Name.Name)
		}
		files = append(files, f)
	}
	if len(files) == 0 {
		if goFiles > 0 {
			return nil, fmt.Errorf("%s: no %s files, only .go files: gogo source files end in %s (rename main.go to main%s)", dir, SourceExt, SourceExt, SourceExt)
		}
		return nil, fmt.Errorf("%s: no %s files", dir, SourceExt)
	}
	absRoot, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}

	l := &loader{
		fset: fset,
		libs: libs,
		info: &types.Info{
			Types:      map[ast.Expr]types.TypeAndValue{},
			Defs:       map[*ast.Ident]types.Object{},
			Uses:       map[*ast.Ident]types.Object{},
			Selections: map[*ast.SelectorExpr]*types.Selection{},
			Implicits:  map[ast.Node]types.Object{},
		},
		pkgs:     map[string]*types.Package{},
		root:     dir,
		rootName: realName(absRoot),
		path:     c.Path,
	}
	optional, err := l.directives(files)
	if err != nil {
		return nil, err
	}
	type pkgImport struct {
		path string
		at   token.Pos
	}
	var imports []pkgImport
	for _, f := range files {
		if err := l.checkImports(f, l.importable, ""); err != nil {
			return nil, err
		}
		for _, imp := range f.Imports {
			p, _ := strconv.Unquote(imp.Path.Value)
			imports = append(imports, pkgImport{p, imp.Path.Pos()})
		}
	}
	sort.SliceStable(imports, func(i, j int) bool { return imports[i].path < imports[j].path })
	for _, imp := range imports {
		if err := l.loadPkg(imp.path, nil, imp.at); err != nil {
			return nil, err
		}
	}

	pkg, err := l.check("main", files)
	if err != nil {
		return nil, err
	}
	// A program that uses error loads gogo/errors (nothing of it is
	// compiled unless used): an error it receives may be one of the
	// errors' reserved types, whose methods its dispatchers must know.
	if l.pkgs[ErrorsPath] == nil && usesError(l.info) {
		if err := l.loadPkg(ErrorsPath, nil, token.NoPos); err != nil {
			return nil, err
		}
	}
	// main is optional (every image is a module); when present it is func main().
	if obj := pkg.Scope().Lookup("main"); obj != nil {
		if fn, ok := obj.(*types.Func); !ok || fn.Type().(*types.Signature).Params().Len() != 0 || fn.Type().(*types.Signature).Results().Len() != 0 {
			return nil, fmt.Errorf("%s: gogo: main must be func main()", dir)
		}
	}
	sort.SliceStable(files, func(i, j int) bool { return fset.File(files[i].Pos()).Name() < fset.File(files[j].Pos()).Name() })
	if err := l.marks.add(l, "main", pkg, files); err != nil {
		return nil, err
	}
	return &Program{Fset: fset, Files: files, Pkg: pkg, Info: l.info, Libs: l.order, Optional: optional, Warnings: l.warnings, marks: l.marks}, nil
}

// onDemandErr is a method declared in a //gogo:ondemand file.
type onDemandErr struct{ pos token.Pos }

func (onDemandErr) Error() string { return "a //gogo:ondemand file may declare no methods" }

// onDemandMethods reports the first method declared in f (a
// //gogo:ondemand file), or nil.
func onDemandMethods(f *ast.File) error {
	for _, d := range f.Decls {
		if fd, ok := d.(*ast.FuncDecl); ok && fd.Recv != nil {
			return onDemandErr{fd.Pos()}
		}
	}
	return nil
}

// isOnDemand reports whether f carries //gogo:ondemand.
func isOnDemand(f *ast.File) bool {
	for _, g := range f.Comments {
		for _, c := range g.List {
			if c.Text == onDemandDirective {
				return true
			}
		}
	}
	return false
}
