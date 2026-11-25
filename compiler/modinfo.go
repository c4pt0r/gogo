package compiler

import (
	"go/ast"
	"go/constant"
	"go/types"
	"strings"
)

// Capability bits of the image's requires word (v9; bits 4-6 added in v10, 7-8 in v11): what the host must
// provide for the module to run. Inferred by Requires.
const (
	CapNet     uint32 = 1
	CapKbd     uint32 = 2
	CapPower   uint32 = 4
	CapSD      uint32 = 8
	CapIMU     uint32 = 16
	CapIRTX    uint32 = 32
	CapIRRX    uint32 = 64
	CapMic     uint32 = 128
	CapSpk     uint32 = 256
	CapAnyPort uint32 = 1 << 31 // port.Open with a driver name not known at build time
)

// anyPortWarning is the warning for a non-constant port.Open driver.
const anyPortWarning = "port.Open with a non-constant driver name: requires any port"

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

// importCaps maps an import path to the capability it implies.
var importCaps = map[string]uint32{
	NetPath: CapNet, libPrefix + "dhcp": CapNet, libPrefix + "dns": CapNet, libPrefix + "http": CapNet,
	KbdPath: CapKbd, PowerPath: CapPower,
}

// driverCaps maps a constant port.Open driver name to its capability.
var driverCaps = map[string]uint32{
	"kbd": CapKbd, "power": CapPower, "imu": CapIMU, "ir_tx": CapIRTX, "ir_rx": CapIRRX, "mic": CapMic, "spk": CapSpk,
	"netif": CapNet, "udp": CapNet, "tcp": CapNet, "tcp_listen": CapNet,
}

// Requires infers the capabilities the program needs, over package main
// and every compiled library: imports, constant port.Open drivers and
// constant gogo/fs paths under /sd. A non-constant driver name requires
// any port, with a warning.
func Requires(p *Program) (bits uint32, warnings []string) {
	warned := false
	for _, pkg := range p.packages() {
		for _, imp := range pkg.Pkg.Imports() {
			bits |= importCaps[imp.Path()]
		}
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
						bits |= driverCaps[s]
					} else {
						bits |= CapAnyPort
						if !warned {
							warned = true
							warnings = append(warnings, anyPortWarning)
						}
					}
				case FsPath:
					for _, a := range call.Args {
						if s, ok := constString(p, a); ok && (s == "/sd" || strings.HasPrefix(s, "/sd/")) {
							bits |= CapSD
						}
					}
				}
				return true
			})
		}
	}
	return bits, warnings
}

func constString(p *Program, e ast.Expr) (string, bool) {
	tv, ok := p.Info.Types[e]
	if !ok || tv.Value == nil || tv.Value.Kind() != constant.String {
		return "", false
	}
	return constant.StringVal(tv.Value), true
}
