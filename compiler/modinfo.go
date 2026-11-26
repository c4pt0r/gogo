package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
	"sort"
	"strings"
)

// anyPortWarning is the warning for a non-constant port.Open driver, after
// the call's position.
const anyPortWarning = "port.Open with a non-constant driver name: the image does not require it and the load does not check it; name the driver and mark it //gogo:optional if the program handles its absence"

// Export is one exported entry of package main.
type Export struct {
	Name string
	Fn   *types.Func
}

// Exports lists the entries a host can spawn by name: func main() as
// "main", and every exported func() or func(arg any) of package main, in
// source order.
func Exports(p *Program) []Export {
	var out []Export
	for _, f := range p.Files {
		for _, d := range f.Decls {
			fd, ok := d.(*ast.FuncDecl)
			if !ok || fd.Recv != nil {
				continue
			}
			fn, ok := p.Info.Defs[fd.Name].(*types.Func)
			if !ok {
				continue
			}
			sig := fn.Type().(*types.Signature)
			if sig.Results().Len() != 0 || sig.Variadic() || sig.TypeParams().Len() != 0 {
				continue
			}
			switch {
			case fn.Name() == "main" && sig.Params().Len() == 0:
			case ast.IsExported(fn.Name()) && (sig.Params().Len() == 0 || sig.Params().Len() == 1 && isEmptyIface(sig.Params().At(0).Type())):
			default:
				continue
			}
			out = append(out, Export{Name: fn.Name(), Fn: fn})
		}
	}
	return out
}

func isEmptyIface(t types.Type) bool {
	i, ok := t.Underlying().(*types.Interface)
	return ok && i.NumMethods() == 0 && i.IsMethodSet()
}

// Requires infers what the program needs from the board, by name, over
// package main and every compiled library: the driver of each constant
// port.Open (but those a library names in its //gogo:optional directive,
// for its own calls; see directivePrefix; package main may use it too),
// and "/sd" for constant gogo/fs paths under /sd. Imports alone require
// nothing. A non-constant driver name requires nothing, with a warning
// per call ("file:line: ..."). The names are sorted.
func Requires(p *Program) (names []string, warnings []string) {
	seen := map[string]bool{}
	add := func(n string) {
		if !seen[n] {
			seen[n] = true
			names = append(names, n)
		}
	}
	for _, pkg := range p.packages() {
		for _, f := range pkg.Files {
			ast.Inspect(f, func(n ast.Node) bool {
				call, ok := n.(*ast.CallExpr)
				if !ok {
					return true
				}
				var id *ast.Ident
				switch fun := ast.Unparen(call.Fun).(type) {
				case *ast.SelectorExpr:
					id = fun.Sel
				case *ast.Ident:
					id = fun
				}
				fn, _ := p.Info.Uses[id].(*types.Func)
				if fn == nil || fn.Pkg() == nil || fn.Type().(*types.Signature).Recv() != nil {
					return true
				}
				switch fn.Pkg().Path() {
				case PortPath:
					if fn.Name() != "Open" || len(call.Args) == 0 {
						return true
					}
					if s, ok := constString(p, call.Args[0]); ok {
						if !pkg.Optional[s] {
							add(s)
						}
					} else {
						warnings = append(warnings, p.Fset.Position(call.Pos()).String()+": "+anyPortWarning)
					}
				case FsPath:
					for _, a := range call.Args {
						if s, ok := constString(p, a); ok && (s == "/sd" || strings.HasPrefix(s, "/sd/")) {
							add("/sd")
						}
					}
				}
				return true
			})
		}
	}
	sort.Strings(names)
	return names, warnings
}

func constString(p *Program, e ast.Expr) (string, bool) {
	tv, ok := p.Info.Types[e]
	if !ok || tv.Value == nil || tv.Value.Kind() != constant.String {
		return "", false
	}
	return constant.StringVal(tv.Value), true
}
