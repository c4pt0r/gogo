package compiler

import (
	"go/ast"
	"go/types"
)

// B_CODE op codes (the first argument; CODE_OP_* in vm/module.h,
// cross-checked by TestCodeMatchC).
const (
	CodeLoad      = 0
	CodeSpawn     = 1
	CodeSpawnLink = 2
	CodeSpawnOpt  = 3
	CodeHold      = 4
	CodeRelease   = 5
	CodeMissing   = 6
	CodeMemUsed   = 7
)

// codeOps maps each gogo/code function (by stubName) to its B_CODE op code.
// Load and the Spawns have two results (twoResult).
var codeOps = map[string]int32{
	"code.Load": CodeLoad, "code.Spawn": CodeSpawn, "code.SpawnLink": CodeSpawnLink,
	"code.SpawnOpt": CodeSpawnOpt, "code.Hold": CodeHold, "code.Release": CodeRelease,
	"code.Missing": CodeMissing, "code.MemUsed": CodeMemUsed,
}

// codeCall compiles a call of the gogo/code function called name (stubName)
// into dst (and dst+1 for the two-result ones): B_CODE with the op code in
// the first register, then the arguments, converted to the parameter types
// (a Spawn arg to any).
func (f *fn) codeCall(call *ast.CallExpr, name string, dst uint8) {
	mark := f.next
	defer func() { f.next = mark }()
	op, ok := codeOps[name]
	if !ok {
		f.fail(call, "internal error: unexpected code call %s", name)
	}
	base := f.allocScalar(call)
	f.asm.EmitExt(OpLoadk, base, 0, 0, TermInt(op))
	n := uint8(1)
	if len(call.Args) > 0 {
		var params *types.Tuple
		if sig, ok := f.g.p.Info.TypeOf(call.Fun).(*types.Signature); ok {
			params = sig.Params()
		}
		_, k := f.argsAs(call, params) // right above the op
		n += k
	}
	f.asm.EmitExt(OpBuiltin, dst, base, n, BCode)
}
