package compiler

import (
	"go/ast"
	"go/types"
)

// B_FS op codes (the first argument; FS_* in runtime/vfs/vfs.h, cross-checked by
// TestFsMatchC).
const (
	FsOpen      = 0
	FsRead      = 1
	FsWrite     = 2
	FsSync      = 3
	FsSize      = 4
	FsReadFile  = 5
	FsWriteFile = 6
	FsStat      = 7
	FsReadDir   = 8
	FsMkdir     = 9
	FsRemove    = 10
	FsRename    = 11
	FsUsage     = 12
	FsWriteAt   = 13
	FsOpenErr   = 14 // (15.4) -> code: why the last refused port open failed (port.Open)
)

// fsOps maps each gogo/fs function (stubName) and method (fsMethod) except
// File.Close to its B_FS op code.
var fsOps = map[string]int32{
	"fs.Open": FsOpen, "File.Read": FsRead, "File.Write": FsWrite, "File.Sync": FsSync,
	"File.Size": FsSize, "fs.ReadFile": FsReadFile, "fs.WriteFile": FsWriteFile,
	"fs.Stat": FsStat, "fs.ReadDir": FsReadDir, "fs.Mkdir": FsMkdir,
	"fs.Remove": FsRemove, "fs.Rename": FsRename, "fs.Usage": FsUsage, "File.WriteAt": FsWriteAt,
	"port.openErr": FsOpenErr,
}

// fsCall compiles a call of the gogo/fs function or method called name
// (stubName / fsMethod) into dst (and dst+1 for the two-result ones): B_FS
// with the op code in the first register, then the receiver (methods) and
// the arguments. File.Close is the port close builtin.
func (f *fn) fsCall(call *ast.CallExpr, name string, dst uint8) {
	mark := f.next
	defer func() { f.next = mark }()
	sel, _ := ast.Unparen(call.Fun).(*ast.SelectorExpr)
	if name == "File.Close" {
		recv := f.alloc(sel.X)
		f.exprInto(sel.X, recv)
		f.asm.EmitExt(OpBuiltin, dst, recv, 1, BPortClose)
		return
	}
	op, ok := fsOps[name]
	if !ok {
		f.fail(call, "internal error: unexpected fs call %s", name)
	}
	base := f.allocScalar(call)
	f.asm.EmitExt(OpLoadk, base, 0, 0, TermInt(op))
	n := uint8(1)
	if len(name) > 5 && name[:5] == "File." {
		r := f.alloc(sel.X)
		f.exprInto(sel.X, r)
		n++
	}
	if len(call.Args) > 0 {
		var params *types.Tuple
		if sig, ok := f.g.p.Info.TypeOf(call.Fun).(*types.Signature); ok {
			params = sig.Params()
		}
		_, k := f.argsAs(call, params) // right above the receiver
		n += k
	}
	f.asm.EmitExt(OpBuiltin, dst, base, n, BFs)
}
