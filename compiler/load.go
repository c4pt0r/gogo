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

	"gogotools/lib"
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
// lib/net/.
const NetPath = "gogo/net"

// GpioPath is the import path of the GPIO port types (gpio.Config,
// gpio.Write, gpio.Edge) and their mode and edge constants, a stub package
// with only types and constants; the types are reserved.
const GpioPath = "gogo/gpio"

// FsPath is the import path of the storage API (fs.Open, fs.ReadFile, ...,
// fs.File's methods), a stub package like gogo/actor: calls become B_FS,
// File.Close the port close builtin. Its reserved types are Info and Space.
const FsPath = "gogo/fs"

// KbdPath is the import path of the keyboard API (kbd.Config, kbd.Key, key
// codes), a stub package with only types and constants. Its reserved types
// are Config and Key.
const KbdPath = "gogo/kbd"

// PowerPath is the import path of the battery API (power.Config,
// power.Battery, power.Read), a stub package with only types. All three are
// reserved types.
const PowerPath = "gogo/power"

// CodePath is the import path of the hot-loading API (code.Load,
// code.Spawn, ...), a stub package like gogo/fs: calls become B_CODE. Its
// reserved type is Module.
const CodePath = "gogo/code"

// ImuPath and IrPath are the import paths of the IMU and infrared APIs
// (imu.Config, imu.Gesture; ir.TX, ir.RX), stub packages with only types and
// constants. Their types are reserved.
const (
	ImuPath = "gogo/imu"
	IrPath  = "gogo/ir"
)

// SysPath is the import path of the system API (sys.Time, sys.SetTime,
// sys.Random, sys.Config), a stub package like gogo/actor: calls become
// B_UNIX_TIME, B_SET_UNIX_TIME, B_RANDOM and B_CONFIG_GET.
const SysPath = "gogo/sys"

// AudioPath is the import path of the audio port types (audio.Mic,
// audio.Speaker), a stub package with only types and constants; the types
// are reserved. The recording and playback library is gogo/sound.
const AudioPath = "gogo/audio"

// ScreenPath is the import path of the text screen port types
// (screen.Config) and its operation constants, a stub package; the type is
// reserved. The text UI library is gogo/tui.
const ScreenPath = "gogo/screen"

// isStub reports whether path is one of the stub packages.
func isStub(path string) bool {
	return path == ActorPath || path == PortPath || path == BytesPath || path == NetPath || path == GpioPath || path == FsPath || path == KbdPath || path == PowerPath || path == CodePath || path == ImuPath || path == IrPath || path == AudioPath || path == SysPath || path == ScreenPath
}

// stubDir is the directory of the library set holding stub package path:
// gogo/<name> lives in <name>/ (lib/<name>/ in the repository).
func stubDir(path string) string {
	return strings.TrimPrefix(path, libPrefix)
}

// libPrefix starts the import path of every gogo library package.
const libPrefix = "gogo/"

// SourceExt is the file name extension of gogo source files: a package is
// the *.gg files of its directory (Go-subset source, parsed with go/parser).
const SourceExt = ".gg"

// Program is a parsed and type-checked gogo program: package main plus the
// library packages it imports.
type Program struct {
	Fset  *token.FileSet
	Files []*ast.File    // package main
	Pkg   *types.Package // package main
	Info  *types.Info    // type information of every compiled package
	Libs  []*Package     // compiled library packages, in dependency order
	// Optional holds the drivers named by package main's //gogo:optional
	// directives (see directives).
	Optional map[string]bool
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
// with one directory per package: gogo/<name> lives in <name>/*.gg. It is
// also the importer of every package it checks; it serves only packages it
// has already loaded, and the stub packages (isStub).
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
	// gogo/port, gogo/net, gogo/fs and gogo/code import gogo/actor and gogo/bytes
	// through l, so every package sees the same actor.PID and bytes.Bytes.
	var imp types.Importer = importer.Default()
	if path == PortPath || path == NetPath || path == FsPath || path == CodePath {
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

// parseDir parses the gogo source files of directory dir of the library
// set, in file name order.
func (l *loader) parseDir(dir string) ([]*ast.File, error) {
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
		f, err := parser.ParseFile(l.fset, name, src, parser.ParseComments)
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

// isLib reports whether path names a compiled library package: gogo/<name>
// with gogo source files in <name>/, other than the stub packages.
func (l *loader) isLib(path string) bool {
	name, ok := strings.CutPrefix(path, libPrefix)
	if !ok || isStub(path) || name == "" || name == "main" || strings.ContainsAny(name, "/.*?[\\") {
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
	optional, err := l.directives(files)
	if err != nil {
		return err
	}
	pkg, err := l.check(path, files)
	if err != nil {
		return err
	}
	l.pkgs[path] = pkg
	l.order = append(l.order, &Package{Pkg: pkg, Files: files, Optional: optional})
	return nil
}

// directivePrefix starts every gogo source directive. The one directive
// is
//
//	//gogo:optional name...
//
// a comment line of a compiled package, a library or package main (any
// file; conventionally the last line of the package doc comment, after a
// blank // line, as gofmt places directives) naming port drivers the
// package opens only when a call asks for them at run time, and whose
// absence (port.Open's ok=false) it handles as an ordinary failure.
// Requires then adds no requirement for that package's own constant
// port.Open of those drivers; importers and the other packages are
// unaffected. Any driver name is accepted (a board's or a host port's).
const directivePrefix = "//gogo:"

// directives parses the //gogo: directives of one package's files and
// returns the drivers named by //gogo:optional. An unknown directive or an
// empty list is an error.
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
				names, ok := strings.CutPrefix(rest, "optional")
				if !ok || names != "" && names[0] != ' ' && names[0] != '\t' {
					return nil, fmt.Errorf("%s: gogo: unknown directive %s (want //gogo:optional)", pos, strings.Fields(c.Text)[0])
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
func Load(dir string) (*Program, error) { return LoadWith(dir, lib.StubFS) }

// LoadWith is Load with the library set libs (see loader).
func LoadWith(dir string, libs fs.FS) (*Program, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	fset := token.NewFileSet()
	var files []*ast.File
	goFiles := 0
	for _, e := range entries {
		name := e.Name()
		if !e.IsDir() && strings.HasSuffix(name, ".go") {
			goFiles++
		}
		if e.IsDir() || !strings.HasSuffix(name, SourceExt) {
			continue
		}
		f, err := parser.ParseFile(fset, filepath.Join(dir, name), nil, parser.ParseComments)
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
	optional, err := l.directives(files)
	if err != nil {
		return nil, err
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
	// main is optional (every image is a module); when present it is func main().
	if obj := pkg.Scope().Lookup("main"); obj != nil {
		if fn, ok := obj.(*types.Func); !ok || fn.Type().(*types.Signature).Params().Len() != 0 || fn.Type().(*types.Signature).Results().Len() != 0 {
			return nil, fmt.Errorf("%s: gogo: main must be func main()", dir)
		}
	}
	sort.SliceStable(files, func(i, j int) bool { return fset.File(files[i].Pos()).Name() < fset.File(files[j].Pos()).Name() })
	return &Program{Fset: fset, Files: files, Pkg: pkg, Info: l.info, Libs: l.order, Optional: optional}, nil
}
