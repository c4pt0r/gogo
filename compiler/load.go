// Package compiler compiles the gogo subset of Go to VM images.
package compiler

import (
	"errors"
	"fmt"
	"go/ast"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"gogo"
)

// ActorPath is the import path of the actor API. Its package is a stub:
// the compiler replaces its calls with VM instructions.
const ActorPath = "gogo/actor"

// PortPath is the import path of the port (peripheral) API, a stub package
// like gogo/actor.
const PortPath = "gogo/port"

// BytesPath is the import path of the byte data API (bytes.Bytes,
// bytes.Buffer), a stub package like gogo/actor.
const BytesPath = "gogo/bytes"

// NetPath is the import path of the network API (port configs, commands,
// events, Subscribe), a stub package like gogo/port. Its source is in
// netapi/ (stubDir): net/ holds the C stack, and a Go package directory
// cannot hold C files without cgo.
const NetPath = "gogo/net"

// ConfigPath is the import path of the configuration store API
// (config.Get), a stub package like gogo/actor.
const ConfigPath = "gogo/config"

// FsPath is the import path of the storage API (fs.Open, fs.ReadFile, ...,
// fs.File's methods), a stub package like gogo/actor: calls become B_FS,
// File.Close the port close builtin. Its reserved types are Info and Space.
const FsPath = "gogo/fs"

// isStub reports whether path is one of the stub packages.
func isStub(path string) bool {
	return path == ActorPath || path == PortPath || path == BytesPath || path == NetPath || path == ConfigPath || path == FsPath
}

// stubDir is the directory of the library set holding stub package path.
func stubDir(path string) string {
	if path == NetPath {
		return "netapi"
	}
	return strings.TrimPrefix(path, libPrefix)
}

// libPrefix starts the import path of every gogo library package.
const libPrefix = "gogo/"

// Program is a parsed and type-checked gogo program: package main plus the
// library packages it imports.
type Program struct {
	Fset  *token.FileSet
	Files []*ast.File    // package main
	Pkg   *types.Package // package main
	Info  *types.Info    // type information of every compiled package
	Libs  []*Package     // compiled library packages, in dependency order
}

// Package is one compiled library package.
type Package struct {
	Pkg   *types.Package
	Files []*ast.File
}

// packages lists every compiled package: the libraries, then main.
func (p *Program) packages() []*Package {
	return append(append([]*Package{}, p.Libs...), &Package{Pkg: p.Pkg, Files: p.Files})
}

// compiled reports whether pkg is compiled into the image (main or a
// library), as opposed to the stub packages or the standard library.
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

// loader parses and type-checks library packages from libs, a file system
// with one directory per package: gogo/<name> lives in <name>/*.go. It is
// also the importer of every package it checks; it serves only packages it
// has already loaded, and the stub packages gogo/actor, gogo/port,
// gogo/bytes, gogo/net, gogo/config and gogo/fs.
type loader struct {
	fset  *token.FileSet
	libs  fs.FS
	info  *types.Info
	pkgs  map[string]*types.Package // checked packages by import path
	order []*Package                // compiled libraries in load order
}

func (l *loader) Import(path string) (*types.Package, error) {
	if pkg := l.pkgs[path]; pkg != nil {
		return pkg, nil
	}
	if !isStub(path) {
		return nil, fmt.Errorf("import %q not supported", path)
	}
	files, err := l.parseDir(stubDir(path))
	if err != nil {
		return nil, err
	}
	// gogo/port, gogo/net and gogo/fs import gogo/actor and gogo/bytes through l,
	// so every package sees the same actor.PID and bytes.Bytes.
	var imp types.Importer = importer.Default()
	if path == PortPath || path == NetPath || path == FsPath {
		imp = l
	}
	conf := types.Config{Sizes: sizes(), Importer: imp}
	pkg, err := conf.Check(path, l.fset, files, nil)
	if err != nil {
		return nil, err
	}
	l.pkgs[path] = pkg
	return pkg, nil
}

// parseDir parses the non-test Go files of directory dir of the library
// set, in file name order.
func (l *loader) parseDir(dir string) ([]*ast.File, error) {
	names, err := fs.Glob(l.libs, dir+"/*.go")
	if err != nil {
		return nil, err
	}
	var files []*ast.File
	for _, name := range names {
		if strings.HasSuffix(name, "_test.go") {
			continue
		}
		src, err := fs.ReadFile(l.libs, name)
		if err != nil {
			return nil, err
		}
		f, err := parser.ParseFile(l.fset, name, src, 0)
		if err != nil {
			return nil, err
		}
		files = append(files, f)
	}
	if len(files) == 0 {
		return nil, fmt.Errorf("gogo: library %s%s has no Go files", libPrefix, dir)
	}
	return files, nil
}

// isLib reports whether path names a compiled library package: gogo/<name>
// with Go files in <name>/, other than the stub packages.
func (l *loader) isLib(path string) bool {
	name, ok := strings.CutPrefix(path, libPrefix)
	if !ok || isStub(path) || name == "" || name == "main" || strings.ContainsAny(name, "/.*?[\\") {
		return false
	}
	names, err := fs.Glob(l.libs, name+"/*.go")
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

// loadLib parses and type-checks library path, first loading the libraries
// it imports (depth first, so l.order is a dependency order). A library may
// import the stub packages and other libraries, without cycles; stack is
// the chain of libraries being loaded, for the cycle error.
func (l *loader) loadLib(path string, stack []string) error {
	if l.pkgs[path] != nil {
		return nil
	}
	for i, p := range stack {
		if p == path {
			return fmt.Errorf("gogo: import cycle %s", strings.Join(append(stack[i:], path), " -> "))
		}
	}
	stack = append(stack, path)
	name := strings.TrimPrefix(path, libPrefix)
	files, err := l.parseDir(name)
	if err != nil {
		return err
	}
	allowed := func(p string) bool { return isStub(p) || l.isLib(p) }
	for _, f := range files {
		if f.Name.Name != name {
			return fmt.Errorf("%s: gogo: package %s in library %s (want package %s)", l.fset.Position(f.Name.Pos()), f.Name.Name, path, name)
		}
		if err := l.checkImports(f, allowed, ""); err != nil {
			return err
		}
	}
	for _, f := range files {
		for _, imp := range f.Imports {
			if p, _ := strconv.Unquote(imp.Path.Value); !isStub(p) {
				if err := l.loadLib(p, stack); err != nil {
					return err
				}
			}
		}
	}
	pkg, err := l.check(path, files)
	if err != nil {
		return err
	}
	l.pkgs[path] = pkg
	l.order = append(l.order, &Package{Pkg: pkg, Files: files})
	return nil
}

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

// Load parses and type-checks the package main in dir, with the libraries
// embedded in the compiler.
func Load(dir string) (*Program, error) { return LoadWith(dir, gogo.StubFS) }

// LoadWith is Load with the library set libs (see loader).
func LoadWith(dir string, libs fs.FS) (*Program, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	fset := token.NewFileSet()
	var files []*ast.File
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".go") || strings.HasSuffix(name, "_test.go") {
			continue
		}
		f, err := parser.ParseFile(fset, filepath.Join(dir, name), nil, 0)
		if err != nil {
			return nil, err
		}
		if f.Name.Name != "main" {
			return nil, fmt.Errorf("%s: gogo: package %s not supported (want package main)", fset.Position(f.Name.Pos()), f.Name.Name)
		}
		files = append(files, f)
	}
	if len(files) == 0 {
		return nil, fmt.Errorf("%s: no Go files", dir)
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
		pkgs: map[string]*types.Package{},
	}
	allowed := func(p string) bool { return isStub(p) || l.isLib(p) }
	var libPaths []string
	for _, f := range files {
		if err := l.checkImports(f, allowed, ""); err != nil {
			return nil, err
		}
		for _, imp := range f.Imports {
			if p, _ := strconv.Unquote(imp.Path.Value); !isStub(p) {
				libPaths = append(libPaths, p)
			}
		}
	}
	sort.Strings(libPaths)
	for _, p := range libPaths {
		if err := l.loadLib(p, nil); err != nil {
			return nil, err
		}
	}

	pkg, err := l.check("main", files)
	if err != nil {
		return nil, err
	}
	if fn, ok := pkg.Scope().Lookup("main").(*types.Func); !ok || fn.Type().(*types.Signature).Params().Len() != 0 || fn.Type().(*types.Signature).Results().Len() != 0 {
		return nil, fmt.Errorf("%s: gogo: package main needs a func main()", dir)
	}
	sort.SliceStable(files, func(i, j int) bool { return fset.File(files[i].Pos()).Name() < fset.File(files[j].Pos()).Name() })
	return &Program{Fset: fset, Files: files, Pkg: pkg, Info: l.info, Libs: l.order}, nil
}
