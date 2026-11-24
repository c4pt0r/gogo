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
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gogo"
)

// ActorPath is the import path of the actor API, the only import allowed.
const ActorPath = "gogo/actor"

// Program is a parsed and type-checked gogo program.
type Program struct {
	Fset  *token.FileSet
	Files []*ast.File
	Pkg   *types.Package
	Info  *types.Info
}

// sizes makes int 32 bits, as on the target.
func sizes() types.Sizes { return &types.StdSizes{WordSize: 4, MaxAlign: 4} }

// stubImporter serves only gogo/actor, type-checked from the embedded stubs.
type stubImporter struct {
	fset *token.FileSet
	pkg  *types.Package
}

func (s *stubImporter) Import(path string) (*types.Package, error) {
	if path != ActorPath {
		return nil, fmt.Errorf("import %q not supported", path)
	}
	if s.pkg != nil {
		return s.pkg, nil
	}
	entries, err := gogo.StubFS.ReadDir("actor")
	if err != nil {
		return nil, err
	}
	var files []*ast.File
	for _, e := range entries {
		src, err := gogo.StubFS.ReadFile("actor/" + e.Name())
		if err != nil {
			return nil, err
		}
		f, err := parser.ParseFile(s.fset, "actor/"+e.Name(), src, 0)
		if err != nil {
			return nil, err
		}
		files = append(files, f)
	}
	conf := types.Config{Sizes: sizes(), Importer: importer.Default()}
	pkg, err := conf.Check(ActorPath, s.fset, files, nil)
	if err != nil {
		return nil, err
	}
	s.pkg = pkg
	return pkg, nil
}

// Load parses and type-checks the package main in dir.
func Load(dir string) (*Program, error) {
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
	for _, f := range files {
		for _, imp := range f.Imports {
			if imp.Path.Value != `"`+ActorPath+`"` || imp.Name != nil {
				what := "import " + imp.Path.Value
				if imp.Name != nil {
					what = "import " + imp.Name.Name + " " + imp.Path.Value
				}
				return nil, fmt.Errorf("%s: gogo: %s not supported", fset.Position(imp.Pos()), what)
			}
		}
	}

	info := &types.Info{
		Types:      map[ast.Expr]types.TypeAndValue{},
		Defs:       map[*ast.Ident]types.Object{},
		Uses:       map[*ast.Ident]types.Object{},
		Selections: map[*ast.SelectorExpr]*types.Selection{},
		Implicits:  map[ast.Node]types.Object{},
	}
	var errs []error
	conf := types.Config{
		Sizes:    sizes(),
		Importer: &stubImporter{fset: fset},
		Error:    func(err error) { errs = append(errs, err) },
	}
	pkg, _ := conf.Check("main", fset, files, info)
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}
	if fn, ok := pkg.Scope().Lookup("main").(*types.Func); !ok || fn.Type().(*types.Signature).Params().Len() != 0 || fn.Type().(*types.Signature).Results().Len() != 0 {
		return nil, fmt.Errorf("%s: gogo: package main needs a func main()", dir)
	}
	sort.SliceStable(files, func(i, j int) bool { return fset.File(files[i].Pos()).Name() < fset.File(files[j].Pos()).Name() })
	return &Program{Fset: fset, Files: files, Pkg: pkg, Info: info}, nil
}
