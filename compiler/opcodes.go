package compiler

import "fmt"

// Opcodes. Values mirror vm/opcodes.h (checked by TestOpcodesMatchC).
const (
	OpNop       uint8 = 0
	OpMove      uint8 = 1
	OpLoadk     uint8 = 2
	OpLoadi32   uint8 = 3
	OpAdd       uint8 = 4
	OpSub       uint8 = 5
	OpMul       uint8 = 6
	OpDiv       uint8 = 7
	OpRem       uint8 = 8
	OpNeg       uint8 = 9
	OpEq        uint8 = 10
	OpNe        uint8 = 11
	OpLt        uint8 = 12
	OpLe        uint8 = 13
	OpNot       uint8 = 14
	OpJmp       uint8 = 15
	OpJmpf      uint8 = 16
	OpJmpt      uint8 = 17
	OpCall      uint8 = 18
	OpRet       uint8 = 19
	OpNewstruct uint8 = 20
	OpGetfield  uint8 = 21
	OpMkclosure uint8 = 22
	OpTypeis    uint8 = 23
	OpBuiltin   uint8 = 24
	OpConv      uint8 = 25 // c=0: int(rb), c=1: int64(rb)

	// int64 ops: operands are immediates or K_INT64 boxes; the generic
	// ADD..LE / NEG are int-only.
	OpAdd64 uint8 = 26
	OpSub64 uint8 = 27
	OpMul64 uint8 = 28
	OpDiv64 uint8 = 29
	OpRem64 uint8 = 30
	OpNeg64 uint8 = 31
	OpEq64  uint8 = 32
	OpNe64  uint8 = 33
	OpLt64  uint8 = 34
	OpLe64  uint8 = 35
	OpBox64 uint8 = 36 // ra = rb as a K_INT64 box (every int64 inside any)

	OpClear uint8 = 37 // ra .. ra+b-1 = nil (dead registers stop being GC roots)
)

// Builtin ids.
const (
	BPrintln uint32 = 0
	BSpawn   uint32 = 1
	BSelf    uint32 = 2
	BSend    uint32 = 3
	BReceive uint32 = 4
	BMemory  uint32 = 5

	// Two-result builtins write ra and ra+1.
	BSpawnLink      uint32 = 6
	BSpawnOpt       uint32 = 7
	BLink           uint32 = 8
	BUnlink         uint32 = 9
	BTrapExit       uint32 = 10
	BExit           uint32 = 11
	BKill           uint32 = 12
	BMonitor        uint32 = 13
	BDemonitor      uint32 = 14
	BMakeRef        uint32 = 15
	BReceiveTimeout uint32 = 16
	BSendAfter      uint32 = 17
	BCancelTimer    uint32 = 18
	BSleep          uint32 = 19
	BNow            uint32 = 20
	BCall           uint32 = 21
	BReply          uint32 = 22
	BRegister       uint32 = 23
	BWhereis        uint32 = 24
	BUnregister     uint32 = 25
	BPortOpen       uint32 = 26 // driver string, cfg any -> pid, ok
	BPortClose      uint32 = 27 // pid -> nil
)

// Builtin type ids for TYPEIS; struct types use their type-table index.
const (
	TyInt    uint32 = 0x10000
	TyBool   uint32 = 0x10001
	TyString uint32 = 0x10002
	TyPid    uint32 = 0x10003
	TyRef    uint32 = 0x10004
	TyInt64  uint32 = 0x10005
)

// Reserved type-table indices of the gogo/actor and gogo/port struct types
// (see reservedTypes).
const (
	TypeProcinfo uint32 = 0
	TypeReason   uint32 = 1
	TypeExitmsg  uint32 = 2
	TypeDownmsg  uint32 = 3
	TypeRequest  uint32 = 4
	TypeOptions  uint32 = 5
	TypeGpio     uint32 = 6
	TypeWrite    uint32 = 7
	TypeEdge     uint32 = 8
)

// Heap object kinds.
const (
	KStruct  uint32 = 1
	KString  uint32 = 2
	KInt     uint32 = 3
	KClosure uint32 = 4
	KRef     uint32 = 5
	KInt64   uint32 = 6 // [hdr][lo][hi]; int64s outside 31 bits and every int64 in any
	KForward uint32 = 63
)

// Special terms.
const (
	TermNil   uint32 = 0x02
	TermFalse uint32 = 0x12
	TermTrue  uint32 = 0x22
	// TermEmptyStr is the empty string, the zero value of strings.
	TermEmptyStr uint32 = 0x32
	// TermZeroRef is the zero actor.Ref: a special TYPEIS sees as a Ref,
	// with serial 0, which no live ref has.
	TermZeroRef uint32 = 0x42
	// TermZeroPid is the zero actor.PID: the PID term of slot 0,
	// generation 0, which no actor or port ever gets.
	TermZeroPid uint32 = 0x06
)

// Int31Fits reports whether v is representable as a small int.
func Int31Fits(v int32) bool { return v >= -(1<<30) && v < 1<<30 }

// TermInt encodes a small int; it panics if v does not fit in 31 bits.
func TermInt(v int32) uint32 {
	if !Int31Fits(v) {
		panic(fmt.Sprintf("TermInt: %d does not fit in 31 bits", v))
	}
	return uint32(v)<<1 | 1
}

// TermConst encodes a reference to the const-area object at word offset off.
func TermConst(off uint32) uint32 { return off<<4 | 0xA }

// HdrMake builds a heap object header.
func HdrMake(words, kind uint32) uint32 { return words<<6 | kind }

// OpHasExt reports whether op is followed by one extra word.
func OpHasExt(op uint8) bool {
	switch op {
	case OpLoadk, OpLoadi32, OpJmp, OpJmpf, OpJmpt, OpCall, OpNewstruct,
		OpMkclosure, OpTypeis, OpBuiltin:
		return true
	}
	return false
}

var opNames = [...]string{"NOP", "MOVE", "LOADK", "LOADI32", "ADD", "SUB", "MUL",
	"DIV", "REM", "NEG", "EQ", "NE", "LT", "LE", "NOT", "JMP", "JMPF", "JMPT",
	"CALL", "RET", "NEWSTRUCT", "GETFIELD", "MKCLOSURE", "TYPEIS", "BUILTIN",
	"CONV", "ADD64", "SUB64", "MUL64", "DIV64", "REM64", "NEG64", "EQ64", "NE64",
	"LT64", "LE64", "BOX64", "CLEAR"}

var builtinNames = [...]string{"PRINTLN", "SPAWN", "SELF", "SEND", "RECEIVE", "MEMORY",
	"SPAWN_LINK", "SPAWN_OPT", "LINK", "UNLINK", "TRAP_EXIT", "EXIT", "KILL",
	"MONITOR", "DEMONITOR", "MAKE_REF", "RECEIVE_TIMEOUT", "SEND_AFTER",
	"CANCEL_TIMER", "SLEEP", "NOW", "CALL", "REPLY", "REGISTER", "WHEREIS",
	"UNREGISTER", "PORT_OPEN", "PORT_CLOSE"}

// goConstants lists every exported Go twin of a C constant, by Go name.
var goConstants = map[string]uint32{
	"OpNop": uint32(OpNop), "OpMove": uint32(OpMove), "OpLoadk": uint32(OpLoadk),
	"OpLoadi32": uint32(OpLoadi32), "OpAdd": uint32(OpAdd), "OpSub": uint32(OpSub),
	"OpMul": uint32(OpMul), "OpDiv": uint32(OpDiv), "OpRem": uint32(OpRem),
	"OpNeg": uint32(OpNeg), "OpEq": uint32(OpEq), "OpNe": uint32(OpNe),
	"OpLt": uint32(OpLt), "OpLe": uint32(OpLe), "OpNot": uint32(OpNot),
	"OpJmp": uint32(OpJmp), "OpJmpf": uint32(OpJmpf), "OpJmpt": uint32(OpJmpt),
	"OpCall": uint32(OpCall), "OpRet": uint32(OpRet), "OpNewstruct": uint32(OpNewstruct),
	"OpGetfield": uint32(OpGetfield), "OpMkclosure": uint32(OpMkclosure),
	"OpTypeis": uint32(OpTypeis), "OpBuiltin": uint32(OpBuiltin), "OpConv": uint32(OpConv),
	"OpAdd64": uint32(OpAdd64), "OpSub64": uint32(OpSub64), "OpMul64": uint32(OpMul64),
	"OpDiv64": uint32(OpDiv64), "OpRem64": uint32(OpRem64), "OpNeg64": uint32(OpNeg64),
	"OpEq64": uint32(OpEq64), "OpNe64": uint32(OpNe64), "OpLt64": uint32(OpLt64),
	"OpLe64": uint32(OpLe64), "OpBox64": uint32(OpBox64), "OpClear": uint32(OpClear),
	"BPrintln": BPrintln, "BSpawn": BSpawn, "BSelf": BSelf, "BSend": BSend,
	"BReceive": BReceive, "BMemory": BMemory,
	"BSpawnLink": BSpawnLink, "BSpawnOpt": BSpawnOpt, "BLink": BLink,
	"BUnlink": BUnlink, "BTrapExit": BTrapExit, "BExit": BExit, "BKill": BKill,
	"BMonitor": BMonitor, "BDemonitor": BDemonitor, "BMakeRef": BMakeRef,
	"BReceiveTimeout": BReceiveTimeout, "BSendAfter": BSendAfter,
	"BCancelTimer": BCancelTimer, "BSleep": BSleep, "BNow": BNow, "BCall": BCall,
	"BReply": BReply, "BRegister": BRegister, "BWhereis": BWhereis,
	"BUnregister": BUnregister, "BPortOpen": BPortOpen, "BPortClose": BPortClose,
	"TyInt": TyInt, "TyBool": TyBool, "TyString": TyString, "TyPid": TyPid,
	"TyRef": TyRef, "TyInt64": TyInt64,
	"TypeProcinfo": TypeProcinfo, "TypeReason": TypeReason, "TypeExitmsg": TypeExitmsg,
	"TypeDownmsg": TypeDownmsg, "TypeRequest": TypeRequest, "TypeOptions": TypeOptions,
	"TypeGpio": TypeGpio, "TypeWrite": TypeWrite, "TypeEdge": TypeEdge,
}
