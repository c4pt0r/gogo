package compiler

import "fmt"

// Opcodes. Values mirror runtime/vm/opcodes.h (checked by TestOpcodesMatchC).
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
	OpConv      uint8 = 25 // c: see ConvInt ..

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
	OpRetn  uint8 = 38 // return ra .. ra+b-1 into the CALL's (or CALLV's) dst .. dst+b-1
	OpCallv uint8 = 39 // call the closure in rb with args rb+1..rb+c -> ra (captures, then args)

	OpAnd      uint8 = 40
	OpOr       uint8 = 41
	OpXor      uint8 = 42
	OpAndnot   uint8 = 43 // ra = rb &^ rc
	OpShl      uint8 = 44 // count: any integer; >= width gives 0, negative is badshift
	OpShr      uint8 = 45 // arithmetic
	OpAnd64    uint8 = 46
	OpOr64     uint8 = 47
	OpXor64    uint8 = 48
	OpAndnot64 uint8 = 49
	OpShl64    uint8 = 50
	OpShr64    uint8 = 51

	// Floating point (15.2): operands are K_FLOAT boxes of one width,
	// float32 or float64, which the op computes in.
	OpAddf  uint8 = 52
	OpSubf  uint8 = 53
	OpMulf  uint8 = 54
	OpDivf  uint8 = 55
	OpNegf  uint8 = 56
	OpEqf   uint8 = 57
	OpNef   uint8 = 58
	OpLtf   uint8 = 59
	OpLef   uint8 = 60
	OpConvf uint8 = 61 // c: see ConvF64Int ..

	// Unsigned 64-bit ops (15.3: sizedMinor): a uint64 is an int64
	// holding its bits; a uint32 is widened (ConvUint64) to use the int64
	// ops.
	OpDivu64 uint8 = 62
	OpRemu64 uint8 = 63
	OpLtu64  uint8 = 64
	OpLeu64  uint8 = 65
	OpShru64 uint8 = 66 // logical

	// References (15.6: refMinor).
	OpSetfield uint8 = 67 // rb.fields[c] = ra, in place: a heap struct of a mutable type (StructMut)
)

// OP_CONV c operands.
const (
	ConvInt    uint8 = 0 // int of an int64: the low 32 bits
	ConvInt64  uint8 = 1 // int64 of an int, sign-extended
	ConvByte   uint8 = 2 // uint8 of an int or int64: the low 8 bits
	ConvUint16 uint8 = 3 // (15.3) uint16: the low 16 bits
	ConvInt8   uint8 = 4 // (15.3) int8: the low 8 bits, sign-extended
	ConvInt16  uint8 = 5 // (15.3) int16: the low 16 bits, sign-extended
	ConvUint64 uint8 = 6 // (15.3) int64 of a uint32 (an int), zero-extended
)

// OP_CONVF c operands.
const (
	ConvF64Int uint8 = 0 // float64 of an int or int64
	ConvF32Int uint8 = 1 // float32 of an int or int64
	ConvIntF   uint8 = 2 // int of a float (toward zero; NaN 0, saturating)
	ConvInt64F uint8 = 3 // int64 of a float
	ConvF64F   uint8 = 4 // float64 of a float
	ConvF32F   uint8 = 5 // float32 of a float (rounded)
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

	// Sequences (runtime/vm/strs.c).
	BStrConcat     uint32 = 28 // a, b -> string
	BLen           uint32 = 29 // string | slice | Bytes | Buffer -> int
	BIndex         uint32 = 30 // string | slice | Bytes, i -> element (a byte as an int)
	BSetindex      uint32 = 31 // slice, i, v -> nil ([]byte stores v & 0xFF)
	BSlice         uint32 = 32 // string | slice | Bytes, lo, hi; every bound explicit; negative is badcode
	BMakeSlice     uint32 = 33 // elemkind (0 term, 1 byte), len, cap, zero -> slice
	BAppend        uint32 = 34 // slice, mode, zero, v... -> slice (see AppendTerm..)
	BCopy          uint32 = 35 // dst slice, src slice | string | Bytes -> n
	BCap           uint32 = 36 // slice -> int
	BStrFrom       uint32 = 37 // []byte | Bytes -> string
	BBytesliceFrom uint32 = 38 // string | Bytes -> []byte
	BStrCmp        uint32 = 39 // a, b -> -1 | 0 | 1

	// bytes.Buffer and bytes.Bytes (runtime/vm/strs.c, runtime/vm/bytes.c).
	BBufNew      uint32 = 40 // cap -> Buffer
	BBufWrite    uint32 = 41 // buf, string | []byte | Bytes | byte -> nil
	BBufFreeze   uint32 = 42 // buf -> Bytes; buf emptied
	BBytesFrom   uint32 = 43 // string | []byte -> Bytes (a copy)
	BBytesEqual  uint32 = 44 // Bytes, Bytes -> bool
	BBytesCopyto uint32 = 45 // Bytes, []byte -> n

	BPortHandoff  uint32 = 46 // port pid, to pid -> bool
	BNetSubscribe uint32 = 47 // -> bool
	BConfigGet    uint32 = 48 // key string -> value string, ok bool
	BFs           uint32 = 49 // op int, args... -> result(s): gogo/fs (runtime/vfs/builtin.c)
	BCode         uint32 = 50 // op int, args... -> result(s): gogo/code (runtime/vm/module.c)
	BUnixTime     uint32 = 51 // -> int64: sys.Time (vm_unix_time_ms)
	BSetUnixTime  uint32 = 52 // ms int64 -> bool: sys.SetTime (platform_set_unix_time_ms)
	BRandom       uint32 = 53 // -> int in [0, 2^30): sys.Random (platform_random)
	BFloat        uint32 = 54 // op int, args... -> result(s): gogo/math, strconv floats (runtime/floatlib)
	BPrintlnu     uint32 = 55 // (15.3) mask, values... -> nil: println, bit i marks value i a uint64

	// Since 15.4 (ifaceMinor).
	BPanic uint32 = 56 // v -> (does not return): panic(v), exit reason R_PANIC
	BCmp   uint32 = 57 // a, b [, x] -> -1 | 0 | 1: the order of two values (== on interfaces, structs; x for arrays)

	// 58-60 were kept for distributed actors (15.5), which needed none.
	BCallErr     uint32 = 58 // (15.7) pid, req, ms -> reply, code: actor.Call with a shared error code
	BRegisterErr uint32 = 59 // (15.7) name, pid -> code: actor.Register with a shared error code
	BLimit       uint32 = 60 // (15.8) pid -> int: actor.Limit, the actor's memory limit in bytes

	// Since 15.6 (refMinor).
	BSprint uint32 = 61 // mode, v -> string | nil: println's text of v (gogo/fmt)
)

// Off-heap Bytes limits: twins of runtime/vm/config.h's defaults, a board's
// and the host's (checked by TestConfigMatchC; the config key vm.handles
// changes the first two at VM start, up to the third).
const (
	BytesHandles      = 64    // GOGO_BYTES_HANDLES: handle table entries
	BytesPerActor     = 16    // GOGO_BYTES_PER_ACTOR: handles one actor may hold
	BytesHandlesMax   = 1024  // GOGO_BYTES_HANDLES_MAX: the most entries vm.handles may ask
	HostBytesHandles  = 4096  // the host's GOGO_BYTES_HANDLES
	HostBytesPerActor = 256   // the host's GOGO_BYTES_PER_ACTOR
	HostBytesMax      = 65535 // the host's GOGO_BYTES_HANDLES_MAX
	BytesInline       = 64    // GOGO_BYTES_INLINE: largest Freeze kept on the heap
)

// B_APPEND modes: bit 0 = byte elements, bit 1 = spread (v is a slice, or
// for bytes a slice or string, whose elements are appended).
const (
	AppendTerm        int32 = 0
	AppendByte        int32 = 1
	AppendTermSpread  int32 = 2
	AppendBytesSpread int32 = 3
)

// Builtin type ids for TYPEIS; struct types use their type-table index.
const (
	TyInt    uint32 = 0x10000
	TyBool   uint32 = 0x10001
	TyString uint32 = 0x10002
	TyPid    uint32 = 0x10003
	TyRef    uint32 = 0x10004
	TyInt64  uint32 = 0x10005
	TyBytes  uint32 = 0x10006 // bytes.Bytes: heap, off-heap or empty
	// 15.2: a 2-word and a 3-word K_FLOAT.
	TyFloat32 uint32 = 0x10007
	TyFloat64 uint32 = 0x10008
)

// Reserved type-table indices of the system packages' struct types (see
// reservedTypes).
const (
	TypeProcinfo uint32 = 0
	TypeReason   uint32 = 1
	TypeExitmsg  uint32 = 2
	TypeDownmsg  uint32 = 3
	TypeRequest  uint32 = 4
	TypeOptions  uint32 = 5
	// gogo/gpio
	TypeGpioConfig uint32 = 6
	TypeGpioWrite  uint32 = 7
	TypeGpioEdge   uint32 = 8
	// gogo/net
	TypeNetIfconfig  uint32 = 9
	TypeNetSetaddr   uint32 = 10
	TypeNetUp        uint32 = 11
	TypeNetDown      uint32 = 12
	TypeNetAddr      uint32 = 13
	TypeNetUdp       uint32 = 14
	TypeNetSendto    uint32 = 15
	TypeNetDatagram  uint32 = 16
	TypeNetDial      uint32 = 17
	TypeNetListen    uint32 = 18
	TypeNetShutdown  uint32 = 19
	TypeNetResume    uint32 = 20
	TypeNetConnected uint32 = 21
	TypeNetAccepted  uint32 = 22
	// gogo/fs
	TypeFsInfo  uint32 = 23
	TypeFsSpace uint32 = 24
	// gogo/kbd, gogo/power
	TypeKbdConfig    uint32 = 25
	TypeKbdKey       uint32 = 26
	TypePowerConfig  uint32 = 27
	TypePowerBattery uint32 = 28
	TypePowerRead    uint32 = 29
	// gogo/code
	TypeCodeModule uint32 = 30
	// gogo/port: the stream protocol
	TypeChunk  uint32 = 31
	TypeOut    uint32 = 32
	TypeDone   uint32 = 33
	TypeClosed uint32 = 34
	// gogo/imu, gogo/ir
	TypeImuConfig  uint32 = 35
	TypeImuGesture uint32 = 36
	TypeIrTx       uint32 = 37
	TypeIrRx       uint32 = 38
	// gogo/audio
	TypeAudioMic     uint32 = 39
	TypeAudioSpeaker uint32 = 40
	// net.DialTLS: cfg of the "tls" port
	TypeNetDialtls uint32 = 41
	// port.Host, port.Post: host-defined ports and host posts
	TypeHost uint32 = 42
	TypePost uint32 = 43
	// screen.Config (15.1): cfg of the "screen" port
	TypeScreenConfig uint32 = 44
	// gogo/errors (15.4): the box of an errors.Code, errors.New's error
	TypeErrorsCode        uint32 = 45
	TypeErrorsErrorstring uint32 = 46
	// net.NodeLink, net.NodeEvent (15.5): cfg and events of the "node" port
	TypeNetNodelink  uint32 = 47
	TypeNetNodeevent uint32 = 48
	// TypeReservedCount is how many reserved types there are
	// (TYPE_RESERVED_COUNT in runtime/vm/opcodes.h; TestReservedTypeCount
	// checks len(reservedTypes)).
	TypeReservedCount uint32 = 49
	// TypeOwnBase is the index of an image's first own struct type
	// (TYPE_OWN_BASE): fixed, whatever the reserved count.
	TypeOwnBase uint32 = 128
)

// Heap object kinds.
const (
	KStruct  uint32 = 1
	KString  uint32 = 2
	KInt     uint32 = 3
	KClosure uint32 = 4
	KRef     uint32 = 5
	KInt64   uint32 = 6  // [hdr][lo][hi]; int64s outside 31 bits and every int64 in any
	KSlice   uint32 = 7  // [hdr][off][len][cap][array]
	KArray   uint32 = 8  // [hdr][n][elements]
	KBytearr uint32 = 9  // [hdr][nbytes][bytes]
	KBytes   uint32 = 10 // [hdr][nbytes][bytes], immutable: a heap bytes.Bytes
	KBuf     uint32 = 11 // [hdr][len][array]: a bytes.Buffer
	KFloat   uint32 = 12 // 15.2: [hdr][bits] float32, [hdr][lo][hi] float64
	KForward uint32 = 63
)

// Per-type flags (15.6) in bits 16-18 of a type-table row's field-count
// word, which NEWSTRUCT copies into the struct's type word (term.h).
const (
	StructMut uint32 = 0x10000 // a reference: SETFIELD writes it, B_CMP compares it by identity
	StructMap uint32 = 0x20000 // a map header (with StructMut): prints as map[k:v ...]
	StructBox uint32 = 0x40000 // a box: prints as its one field
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
	// TermNilSlice is the nil slice, the zero value of every slice type.
	TermNilSlice uint32 = 0x52
	// TermEmptyBytes is the empty bytes.Bytes, its zero value.
	TermEmptyBytes uint32 = 0x62
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
	"LT64", "LE64", "BOX64", "CLEAR", "RETN", "CALLV",
	"AND", "OR", "XOR", "ANDNOT", "SHL", "SHR", "AND64", "OR64", "XOR64", "ANDNOT64",
	"SHL64", "SHR64", "ADDF", "SUBF", "MULF", "DIVF", "NEGF", "EQF", "NEF", "LTF", "LEF", "CONVF",
	"DIVU64", "REMU64", "LTU64", "LEU64", "SHRU64", "SETFIELD"}

var builtinNames = [...]string{"PRINTLN", "SPAWN", "SELF", "SEND", "RECEIVE", "MEMORY",
	"SPAWN_LINK", "SPAWN_OPT", "LINK", "UNLINK", "TRAP_EXIT", "EXIT", "KILL",
	"MONITOR", "DEMONITOR", "MAKE_REF", "RECEIVE_TIMEOUT", "SEND_AFTER",
	"CANCEL_TIMER", "SLEEP", "NOW", "CALL", "REPLY", "REGISTER", "WHEREIS",
	"UNREGISTER", "PORT_OPEN", "PORT_CLOSE", "STR_CONCAT", "LEN", "INDEX", "SETINDEX",
	"SLICE", "MAKE_SLICE", "APPEND", "COPY", "CAP", "STR_FROM", "BYTESLICE_FROM",
	"STR_CMP", "BUF_NEW", "BUF_WRITE", "BUF_FREEZE", "BYTES_FROM", "BYTES_EQUAL",
	"BYTES_COPYTO", "PORT_HANDOFF", "NET_SUBSCRIBE", "CONFIG_GET", "FS", "CODE", "UNIX_TIME", "SET_UNIX_TIME", "RANDOM", "FLOAT", "PRINTLNU",
	"PANIC", "CMP", "CALL_ERR", "REGISTER_ERR", "LIMIT", "SPRINT"}

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
	"OpRetn": uint32(OpRetn), "OpCallv": uint32(OpCallv),
	"OpAnd": uint32(OpAnd), "OpOr": uint32(OpOr), "OpXor": uint32(OpXor),
	"OpAndnot": uint32(OpAndnot), "OpShl": uint32(OpShl), "OpShr": uint32(OpShr),
	"OpAnd64": uint32(OpAnd64), "OpOr64": uint32(OpOr64), "OpXor64": uint32(OpXor64),
	"OpAndnot64": uint32(OpAndnot64), "OpShl64": uint32(OpShl64), "OpShr64": uint32(OpShr64),
	"OpAddf": uint32(OpAddf), "OpSubf": uint32(OpSubf), "OpMulf": uint32(OpMulf), "OpDivf": uint32(OpDivf),
	"OpNegf": uint32(OpNegf), "OpEqf": uint32(OpEqf), "OpNef": uint32(OpNef), "OpLtf": uint32(OpLtf),
	"OpLef": uint32(OpLef), "OpConvf": uint32(OpConvf),
	"OpDivu64": uint32(OpDivu64), "OpRemu64": uint32(OpRemu64),
	"OpLtu64": uint32(OpLtu64), "OpLeu64": uint32(OpLeu64), "OpShru64": uint32(OpShru64),
	"OpSetfield": uint32(OpSetfield),
	"BPrintln":   BPrintln, "BSpawn": BSpawn, "BSelf": BSelf, "BSend": BSend,
	"BReceive": BReceive, "BMemory": BMemory,
	"BSpawnLink": BSpawnLink, "BSpawnOpt": BSpawnOpt, "BLink": BLink,
	"BUnlink": BUnlink, "BTrapExit": BTrapExit, "BExit": BExit, "BKill": BKill,
	"BMonitor": BMonitor, "BDemonitor": BDemonitor, "BMakeRef": BMakeRef,
	"BReceiveTimeout": BReceiveTimeout, "BSendAfter": BSendAfter,
	"BCancelTimer": BCancelTimer, "BSleep": BSleep, "BNow": BNow, "BCall": BCall,
	"BReply": BReply, "BRegister": BRegister, "BWhereis": BWhereis,
	"BUnregister": BUnregister, "BPortOpen": BPortOpen, "BPortClose": BPortClose,
	"BStrConcat": BStrConcat, "BLen": BLen, "BIndex": BIndex, "BSlice": BSlice,
	"BStrCmp": BStrCmp, "BSetindex": BSetindex, "BMakeSlice": BMakeSlice,
	"BAppend": BAppend, "BCopy": BCopy, "BCap": BCap, "BStrFrom": BStrFrom,
	"BBytesliceFrom": BBytesliceFrom, "BBufNew": BBufNew, "BBufWrite": BBufWrite,
	"BBufFreeze": BBufFreeze, "BBytesFrom": BBytesFrom, "BBytesEqual": BBytesEqual,
	"BBytesCopyto": BBytesCopyto, "BPortHandoff": BPortHandoff, "BNetSubscribe": BNetSubscribe,
	"BConfigGet": BConfigGet, "BFs": BFs, "BCode": BCode, "BUnixTime": BUnixTime, "BSetUnixTime": BSetUnixTime, "BRandom": BRandom, "BFloat": BFloat, "BPrintlnu": BPrintlnu,
	"BPanic": BPanic, "BCmp": BCmp, "BCallErr": BCallErr, "BRegisterErr": BRegisterErr, "BLimit": BLimit,
	"BSprint": BSprint,
	"TyInt":   TyInt, "TyBool": TyBool, "TyString": TyString, "TyPid": TyPid,
	"TyRef": TyRef, "TyInt64": TyInt64, "TyBytes": TyBytes, "TyFloat32": TyFloat32, "TyFloat64": TyFloat64,
	"TypeProcinfo": TypeProcinfo, "TypeReason": TypeReason, "TypeExitmsg": TypeExitmsg,
	"TypeDownmsg": TypeDownmsg, "TypeRequest": TypeRequest, "TypeOptions": TypeOptions,
	"TypeGpioConfig": TypeGpioConfig, "TypeGpioWrite": TypeGpioWrite, "TypeGpioEdge": TypeGpioEdge,
	"TypeNetIfconfig": TypeNetIfconfig, "TypeNetSetaddr": TypeNetSetaddr, "TypeNetUp": TypeNetUp,
	"TypeNetDown": TypeNetDown, "TypeNetAddr": TypeNetAddr, "TypeNetUdp": TypeNetUdp,
	"TypeNetSendto": TypeNetSendto, "TypeNetDatagram": TypeNetDatagram,
	"TypeNetDial": TypeNetDial, "TypeNetListen": TypeNetListen, "TypeNetShutdown": TypeNetShutdown,
	"TypeNetResume": TypeNetResume, "TypeNetConnected": TypeNetConnected, "TypeNetAccepted": TypeNetAccepted,
	"TypeFsInfo": TypeFsInfo, "TypeFsSpace": TypeFsSpace,
	"TypeKbdConfig": TypeKbdConfig, "TypeKbdKey": TypeKbdKey, "TypePowerConfig": TypePowerConfig, "TypePowerBattery": TypePowerBattery, "TypePowerRead": TypePowerRead,
	"TypeCodeModule": TypeCodeModule, "TypeChunk": TypeChunk, "TypeOut": TypeOut, "TypeDone": TypeDone, "TypeClosed": TypeClosed,
	"TypeImuConfig": TypeImuConfig, "TypeImuGesture": TypeImuGesture, "TypeIrTx": TypeIrTx, "TypeIrRx": TypeIrRx,
	"TypeAudioMic": TypeAudioMic, "TypeAudioSpeaker": TypeAudioSpeaker, "TypeNetDialtls": TypeNetDialtls,
	"TypeHost": TypeHost, "TypePost": TypePost, "TypeScreenConfig": TypeScreenConfig,
	"TypeErrorsCode": TypeErrorsCode, "TypeErrorsErrorstring": TypeErrorsErrorstring,
	"TypeNetNodelink": TypeNetNodelink, "TypeNetNodeevent": TypeNetNodeevent,
	"TypeReservedCount": TypeReservedCount, "TypeOwnBase": TypeOwnBase,
}
