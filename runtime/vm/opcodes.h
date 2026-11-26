/* Bytecode definitions shared by the VM and the compiler.
 * Instruction word: op | a<<8 | b<<16 | c<<24. Ops marked +ext are
 * followed by one extra word. Every member has an explicit number because
 * the Go compiler parses this file.
 *
 * Reference comments: tools/bcdoc generates docs/bytecode.md from this file
 * (make bytecode-doc; make test fails when the doc is stale). Every Op and
 * Builtin member carries ONE structured comment on its own line, so merges
 * stay line-local:
 *     OP_X = n,  / *! OPERANDS | EFFECT | DESCRIPTION * /
 *     B_X  = n,  / *! ARGS -> RESULTS | DESCRIPTION * /
 * (spaces added inside the comment markers here so this text does not nest
 * a comment; the real marker is the opener, "!", the text, the closer). OPERANDS
 * lists the fields the op uses as name=role pairs (a=dst b=lhs c=rhs
 * ext=target), or - for none; EFFECT is register notation (ra is
 * register a); a field is separated from the next by " | ", so the text
 * itself must not contain " | " (write OR for bitwise or). For a builtin, ARGS
 * and RESULTS may contain " | " (alternatives): the description is
 * everything after the LAST " | ". Plain comments, including multi-line
 * ones above a member, are ignored. Whether an op takes an extension word
 * comes from op_has_ext below, not from the comment. Adding an op: pick
 * the next number, add the member with its comment, add it to op_has_ext
 * if it takes ext, move OP_LAST, give it a row in interp.c's op_info (its
 * ext word and register operands), run `make bytecode-doc`. Adding a
 * builtin: the same with B_LAST and a row in interp.c's builtin_info.
 */
#ifndef GOGO_OPCODES_H
#define GOGO_OPCODES_H

#include <stdint.h>
#include <stdbool.h>

enum Op {
    OP_NOP       = 0,   /*! - | - | Do nothing. */
    OP_MOVE      = 1,   /*! a=dst b=src | ra = rb | Copy a register. */
    OP_LOADK     = 2,   /*! a=dst ext=term | ra = ext | Load a raw term: small int, special immediate or constant-area ref (checked against the const map when executed). */
    OP_LOADI32   = 3,   /*! a=dst ext=int32 | ra = int32(ext) | Load a 32-bit integer, boxed as an int64 when it does not fit an immediate. */
    OP_ADD       = 4,   /*! a=dst b=lhs c=rhs | ra = rb + rc | int addition (int only; int64 is ADD64). */
    OP_SUB       = 5,   /*! a=dst b=lhs c=rhs | ra = rb - rc | int subtraction. */
    OP_MUL       = 6,   /*! a=dst b=lhs c=rhs | ra = rb * rc | int multiplication. */
    OP_DIV       = 7,   /*! a=dst b=lhs c=rhs | ra = rb / rc | int division, truncated; a zero divisor crashes with R_DIV_ZERO. */
    OP_REM       = 8,   /*! a=dst b=lhs c=rhs | ra = rb % rc | int remainder; a zero divisor crashes with R_DIV_ZERO. */
    OP_NEG       = 9,   /*! a=dst b=src | ra = -rb | int negation. */
    OP_EQ        = 10,  /*! a=dst b=lhs c=rhs | ra = rb == rc | Equality of ints, bools, strings (by content), PIDs and Refs; an int64 box is badcode (use EQ64). */
    OP_NE        = 11,  /*! a=dst b=lhs c=rhs | ra = rb != rc | Inequality, as EQ. */
    OP_LT        = 12,  /*! a=dst b=lhs c=rhs | ra = rb < rc | int less-than. */
    OP_LE        = 13,  /*! a=dst b=lhs c=rhs | ra = rb <= rc | int less-or-equal. */
    OP_NOT       = 14,  /*! a=dst b=src | ra = !rb | Boolean negation. */
    OP_JMP       = 15,  /*! ext=target | pc = code + ext | Unconditional jump to an absolute code word index. */
    OP_JMPF      = 16,  /*! a=cond ext=target | if !ra: pc = code + ext | Jump if ra is false. */
    OP_JMPT      = 17,  /*! a=cond ext=target | if ra: pc = code + ext | Jump if ra is true. */
    OP_CALL      = 18,  /*! a=dst b=args c=nargs ext=func | ra = func ext(rb .. rb+c-1) | Call a function by index; c must equal its parameter count. The result returns into ra via RET or RETN. */
    OP_RET       = 19,  /*! a=src | return ra | Return one value into the caller's CALL or CALLV destination register. */
    OP_NEWSTRUCT = 20,  /*! a=dst b=fields c=nfields ext=type | ra = struct ext{rb .. rb+c-1} | Allocate a struct of type ext (reserved index or TYPE_OWN_BASE + n); c must equal the type's field count. The struct's type word carries its row's flags (15.6: mutable, map, box; term.h). */
    OP_GETFIELD  = 21,  /*! a=dst b=struct c=index | ra = rb.fields[c] | Read a field; past the end of an older image's reserved struct it reads int 0, of an own struct it is badcode; nil crashes with R_NIL (15.6). */
    OP_MKCLOSURE = 22,  /*! a=dst b=captures c=ncaptures ext=func | ra = closure func ext{rb .. rb+c-1} | Allocate a closure of function ext capturing registers rb .. rb+c-1. */
    OP_TYPEIS    = 23,  /*! a=dst b=value ext=type | ra = (dynamic type of rb == ext) | Type test: ext is a TY_* id or a struct type id. */
    OP_BUILTIN   = 24,  /*! a=dst b=args c=nargs ext=builtin | ra = builtin ext(rb .. rb+c-1) | Call a builtin (B_*). Two-result builtins also write ra+1. */
    OP_CONV      = 25,  /*! a=dst b=src c=mode | ra = conv_c(rb) | Integer conversion: c=0 int(rb) of an int64 (low 32 bits); c=1 int64(rb) of an int (sign-extended); c=2 uint8(rb), c=3 uint16(rb), c=4 int8(rb), c=5 int16(rb) of an int or int64: the low 8 or 16 bits, zero- or sign-extended (15.3 for c=3..6); c=6 int64(uint32(rb)) of an int (zero-extended). */
    /* int64 ops. Operands are int64 values in either representation: a
     * small-int immediate or a K_INT64 box (heap or const area); results
     * are immediates when they fit in 31 bits, else boxed. The generic
     * ADD..LE / NEG above are int-only. */
    OP_ADD64     = 26,  /*! a=dst b=lhs c=rhs | ra = rb + rc | int64 addition. */
    OP_SUB64     = 27,  /*! a=dst b=lhs c=rhs | ra = rb - rc | int64 subtraction. */
    OP_MUL64     = 28,  /*! a=dst b=lhs c=rhs | ra = rb * rc | int64 multiplication. */
    OP_DIV64     = 29,  /*! a=dst b=lhs c=rhs | ra = rb / rc | int64 division; a zero divisor crashes with R_DIV_ZERO. */
    OP_REM64     = 30,  /*! a=dst b=lhs c=rhs | ra = rb % rc | int64 remainder; a zero divisor crashes with R_DIV_ZERO. */
    OP_NEG64     = 31,  /*! a=dst b=src | ra = -rb | int64 negation. */
    OP_EQ64      = 32,  /*! a=dst b=lhs c=rhs | ra = rb == rc | int64 equality. */
    OP_NE64      = 33,  /*! a=dst b=lhs c=rhs | ra = rb != rc | int64 inequality. */
    OP_LT64      = 34,  /*! a=dst b=lhs c=rhs | ra = rb < rc | int64 less-than. */
    OP_LE64      = 35,  /*! a=dst b=lhs c=rhs | ra = rb <= rc | int64 less-or-equal. */
    OP_BOX64     = 36,  /*! a=dst b=src | ra = box64(rb) | Make rb a K_INT64 box (no-op if already boxed); an int64 inside `any` is always boxed. */
    OP_CLEAR     = 37,  /*! a=first b=count | ra .. ra+b-1 = nil | Set dead registers to nil so they stop being GC roots (b = 0 does nothing). */
    OP_RETN      = 38,  /*! a=first b=count | return ra .. ra+b-1 | Return b values into the caller's registers dst .. dst+b-1, dst = the CALL's or CALLV's ra (RET returns one). */
    OP_CALLV     = 39,  /*! a=dst b=closure c=nargs | ra = rb(rb+1 .. rb+c) | Call the closure in rb with args rb+1 .. rb+c: its captures, then the args, are the callee's parameters; anything but a closure whose function takes captures+c parameters (nil) is badcode. */
    /* Bitwise ops, Go semantics, int then int64 (as ADD / ADD64). SHL / SHR
     * take any integer count (either representation): a count >= the width
     * gives 0 (SHR: 0 or -1 by sign), a negative count crashes with
     * R_BAD_SHIFT. SHR is arithmetic. */
    OP_AND       = 40,  /*! a=dst b=lhs c=rhs | ra = rb & rc | int bitwise and. */
    OP_OR        = 41,  /*! a=dst b=lhs c=rhs | ra = rb OR rc | int bitwise or. */
    OP_XOR       = 42,  /*! a=dst b=lhs c=rhs | ra = rb ^ rc | int bitwise xor. */
    OP_ANDNOT    = 43,  /*! a=dst b=lhs c=rhs | ra = rb &^ rc | int bit clear. */
    OP_SHL       = 44,  /*! a=dst b=value c=count | ra = rb << rc | int left shift; a count >= the width gives 0, a negative count crashes with R_BAD_SHIFT. */
    OP_SHR       = 45,  /*! a=dst b=value c=count | ra = rb >> rc | int arithmetic right shift; a count >= the width gives 0 or -1 by sign, a negative count crashes with R_BAD_SHIFT. */
    OP_AND64     = 46,  /*! a=dst b=lhs c=rhs | ra = rb & rc | int64 bitwise and. */
    OP_OR64      = 47,  /*! a=dst b=lhs c=rhs | ra = rb OR rc | int64 bitwise or. */
    OP_XOR64     = 48,  /*! a=dst b=lhs c=rhs | ra = rb ^ rc | int64 bitwise xor. */
    OP_ANDNOT64  = 49,  /*! a=dst b=lhs c=rhs | ra = rb &^ rc | int64 bit clear. */
    OP_SHL64     = 50,  /*! a=dst b=value c=count | ra = rb << rc | int64 left shift (count as SHL; the count may be either representation). */
    OP_SHR64     = 51,  /*! a=dst b=value c=count | ra = rb >> rc | int64 arithmetic right shift (count as SHR). */
    /* Floating point (15.2, GOGO_FLOAT; floats.h). Operands are K_FLOAT
     * boxes (heap or const area) of one width, float32 or float64: the op
     * computes in that width (IEEE 754, round to nearest even, as Go) and
     * boxes the result; mixed widths or a non-float are badcode. Division
     * by zero gives an infinity or NaN, never a crash. A VM built without
     * GOGO_FLOAT refuses images that use them (requirement "float"). */
    OP_ADDF      = 52,  /*! a=dst b=lhs c=rhs | ra = rb + rc | float addition. */
    OP_SUBF      = 53,  /*! a=dst b=lhs c=rhs | ra = rb - rc | float subtraction. */
    OP_MULF      = 54,  /*! a=dst b=lhs c=rhs | ra = rb * rc | float multiplication. */
    OP_DIVF      = 55,  /*! a=dst b=lhs c=rhs | ra = rb / rc | float division; a zero divisor gives +Inf, -Inf or NaN. */
    OP_NEGF      = 56,  /*! a=dst b=src | ra = -rb | float negation (flips the sign bit: -0 from 0). */
    OP_EQF       = 57,  /*! a=dst b=lhs c=rhs | ra = rb == rc | float equality (NaN equals nothing; -0 == 0). */
    OP_NEF       = 58,  /*! a=dst b=lhs c=rhs | ra = rb != rc | float inequality (true if either is NaN). */
    OP_LTF       = 59,  /*! a=dst b=lhs c=rhs | ra = rb < rc | float less-than (false if either is NaN). */
    OP_LEF       = 60,  /*! a=dst b=lhs c=rhs | ra = rb <= rc | float less-or-equal (false if either is NaN). */
    OP_CONVF     = 61,  /*! a=dst b=src c=mode | ra = conv_c(rb) | Float conversion: c=0 float64(rb), c=1 float32(rb) of an int or int64; c=2 int(rb), c=3 int64(rb) of a float, truncated toward zero, NaN giving 0 and out of range the nearest end; c=4 float64(rb), c=5 float32(rb) of a float (rounded to nearest even). */
    /* Unsigned 64-bit ops (15.3). A uint64 is an int64 holding its bits,
     * so + - * & OR ^ &^ <<, negation and equality are the int64 ops; these
     * are the ones that differ. A uint32 (and uint) is an int holding its
     * bits: the compiler widens it with CONV c=6 and uses the int64 ops
     * (signed or not, they agree on 0 .. 2^32-1). */
    OP_DIVU64    = 62,  /*! a=dst b=lhs c=rhs | ra = rb / rc | uint64 division; a zero divisor crashes with R_DIV_ZERO. */
    OP_REMU64    = 63,  /*! a=dst b=lhs c=rhs | ra = rb % rc | uint64 remainder; a zero divisor crashes with R_DIV_ZERO. */
    OP_LTU64     = 64,  /*! a=dst b=lhs c=rhs | ra = rb < rc | uint64 less-than. */
    OP_LEU64     = 65,  /*! a=dst b=lhs c=rhs | ra = rb <= rc | uint64 less-or-equal. */
    OP_SHRU64    = 66,  /*! a=dst b=value c=count | ra = rb >> rc | uint64 logical right shift (count as SHR: a count >= 64 gives 0). */
    /* References (15.6): pointers and maps are structs of a type whose
     * row is flagged mutable (STRUCT_MUT, term.h); only those are
     * written in place. */
    OP_SETFIELD  = 67   /*! a=src b=struct c=index | rb.fields[c] = ra | Write a field of a heap struct of a mutable type (a pointer's target, a map header); nil crashes with R_NIL, anything else (a const or immutable struct, past the end) is badcode. */
};
#define OP_LAST OP_SETFIELD   /* the highest op: interp.c's op_info has a row for each up to it */

enum Builtin {
    B_PRINTLN = 0,  /*! values... -> nil | Print the values, space-separated, with a newline, to the console. */
    B_SPAWN   = 1,  /*! closure -> pid | Start a new actor running the closure. */
    B_SELF    = 2,  /*! -> pid | The calling actor's PID. */
    B_SEND    = 3,  /*! pid, msg -> nil | Copy msg into the target's mailbox (a value that may not cross modules is R_BAD_MSG). */
    B_RECEIVE = 4,  /*! -> msg | Block until a message arrives and take it. */
    B_MEMORY  = 5,  /*! pid -> int | The memory the local actor pid holds, in bytes (the actor, its table slot, its block, its queued messages and timers); 0 for anything else. actor.Info puts it in a ProcInfo. */
    /* Two-result builtins write ra and ra+1. */
    B_SPAWN_LINK      = 6,   /*! closure -> pid | Spawn and link atomically. */
    B_SPAWN_OPT       = 7,   /*! closure, Options struct -> pid | Spawn with actor.Options (MaxMemory). */
    B_LINK            = 8,   /*! pid -> nil | Link to another actor. */
    B_UNLINK          = 9,   /*! pid -> nil | Remove a link. */
    B_TRAP_EXIT       = 10,  /*! bool -> nil | Turn exit trapping on or off: linked exits arrive as ExitMsg. */
    B_EXIT            = 11,  /*! code -> (does not return) | Terminate the calling actor with an exit code. */
    B_KILL            = 12,  /*! pid, code -> nil | Terminate another actor with a code. */
    B_MONITOR         = 13,  /*! pid -> ref | Monitor an actor: its end arrives as DownMsg. */
    B_DEMONITOR       = 14,  /*! ref -> nil | Cancel a monitor. */
    B_MAKE_REF        = 15,  /*! -> ref | A fresh unique reference. */
    B_RECEIVE_TIMEOUT = 16,  /*! ms -> msg, ok | Receive with a timeout in milliseconds; ok is false on timeout. */
    B_SEND_AFTER      = 17,  /*! ms, pid, msg -> ref | Send after a delay; the ref cancels it. */
    B_CANCEL_TIMER    = 18,  /*! ref -> bool | Cancel a send-after timer; true if it had not fired. */
    B_SLEEP           = 19,  /*! ms -> nil | Suspend the calling actor. */
    B_NOW             = 20,  /*! -> int64 | Monotonic milliseconds. */
    B_CALL            = 21,  /*! pid, req, ms -> reply, ok | Request/reply with a timeout (actor.Call). */
    B_REPLY           = 22,  /*! Request struct, reply -> nil | Answer an actor.Request. */
    B_REGISTER        = 23,  /*! name, pid -> bool | Register a name for a PID. */
    B_WHEREIS         = 24,  /*! name -> pid, ok | Look up a registered name. */
    B_UNREGISTER      = 25,  /*! name -> nil | Remove a registered name. */
    B_PORT_OPEN       = 26,  /*! driver string, cfg any -> pid, ok | Open a port driver (port.Open). */
    B_PORT_CLOSE      = 27,  /*! pid -> nil | Close a port. */
    /* Sequences (strs.c): the generic ones dispatch on the kind of their
     * first argument. Out-of-range -> badcode, allocation failure -> OOM. */
    B_STR_CONCAT      = 28,  /*! a, b -> string | Concatenate two strings. */
    B_LEN             = 29,  /*! string | slice | Bytes | Buffer -> int | Length of a sequence. */
    B_INDEX           = 30,  /*! string | slice | Bytes, i -> element | Element i; a byte (of a string, []byte or Bytes) as an int. */
    B_SETINDEX        = 31,  /*! slice, i, v -> nil | Store an element; a []byte stores v & 0xFF. */
    B_SLICE           = 32,  /*! string | slice | Bytes, lo, hi -> the same kind | Sub-sequence; every bound is explicit, any negative or out-of-range one is badcode (a slice's hi may reach cap). */
    B_MAKE_SLICE      = 33,  /*! elemkind, len, cap, zero -> slice | New slice; elemkind 0 term, 1 byte; every element is `zero` (0 for bytes). */
    B_APPEND          = 34,  /*! slice, mode, zero, v... -> slice | Append. Mode 0: the terms v...; 1: the bytes v...; 2: every element of the term slice v (one v); 3: every byte of the []byte or string v (one v). Growth is decided once for all of them: if they do not all fit in cap, a new array (x2, min 4, spare slots = zero) gets them and the old one is left untouched. */
    B_COPY            = 35,  /*! dst slice, src slice | string | Bytes -> n | Copy elements; n is the count copied. */
    B_CAP             = 36,  /*! slice -> int | Capacity of a slice. */
    B_STR_FROM        = 37,  /*! []byte | Bytes -> string | String from bytes (a copy). */
    B_BYTESLICE_FROM  = 38,  /*! string | Bytes -> []byte | Byte slice from a string or Bytes (a copy). */
    B_STR_CMP         = 39,  /*! a, b -> -1 | 0 | 1 | Compare two strings. */
    /* bytes.Buffer and bytes.Bytes (strs.c, handles in bytes.c). A Buffer
     * builtin on anything but a Buffer (the zero Buffer is nil) is badcode. */
    B_BUF_NEW         = 40,  /*! cap -> Buffer | New bytes.Buffer with an initial capacity. */
    B_BUF_WRITE       = 41,  /*! buf, string | []byte | Bytes | byte (an int) -> nil | Append to a Buffer. */
    B_BUF_FREEZE      = 42,  /*! buf -> Bytes | Freeze into Bytes (a copy: heap K_BYTES up to GOGO_BYTES_INLINE bytes, else off-heap); buf is emptied and stays usable. */
    B_BYTES_FROM      = 43,  /*! string | []byte -> Bytes | Bytes from a string or slice (a heap copy, any size). */
    B_BYTES_EQUAL     = 44,  /*! Bytes, Bytes -> bool | Same contents. */
    B_BYTES_COPYTO    = 45,  /*! Bytes, []byte -> n | Copy Bytes into a slice; n is the count copied. */
    B_PORT_HANDOFF    = 46,  /*! port pid, to pid -> bool | Hand a port's events to another actor (port.h port_handoff). */
    B_NET_SUBSCRIBE   = 47,  /*! -> bool | net.Subscribe: receive network events (port.h port_subscribe). */
    B_CONFIG_GET      = 48,  /*! key string -> value string, ok bool | sys.Config (host->config_get; "wifi." keys hidden). */
    B_FS              = 49,  /*! op int, args... -> result(s) | gogo/fs, run by the Vm's fs_builtin hook (actor.h; vfs/builtin.c). */
    B_CODE            = 50,  /*! op int, args... -> result(s) | gogo/code (module_builtin, module.h; CODE_OP_*). */
    B_UNIX_TIME       = 51,  /*! -> int64 | sys.Time: Unix milliseconds, vm_unix_time_ms (0: unknown). */
    B_SET_UNIX_TIME   = 52,  /*! ms int64 -> bool | sys.SetTime, vm_set_unix_time_ms (false: below the build-time floor). */
    B_RANDOM          = 53,  /*! -> int | sys.Random: an int in [0, 2^30), vm_random. */
    B_FLOAT           = 54,  /*! op int, args... -> result(s) | (15.2, GOGO_FLOAT) gogo/math and strconv.FormatFloat / ParseFloat (float_lib, runtime/floatlib; FLOAT_OP_*). */
    B_PRINTLNU        = 55,  /*! mask, values... -> nil | (15.3) println with unsigned values: bit i of the int mask (i < 30) marks value i a uint64 (an int64's bits), printed unsigned. */
    /* Since 15.4 (the language's abstraction phase). */
    B_PANIC           = 56,  /*! v -> (does not return) | panic(v): print "panic: " and v (as println prints it) to stderr, then end the calling actor with R_PANIC. Deferred calls do not run. */
    B_CMP             = 57,  /*! a, b [, x [, mode]] -> -1 | 0 | 1 | 2 | A total order of two values, for == and switch on interfaces, structs and arrays: kinds first, then ints, int64s and floats by value (a NaN equals nothing; -0 == 0), Refs by serial, strings and Bytes by bytes, structs of one type field by field (a struct of a mutable type, a pointer or a map, by identity: 15.6), arrays (slices) element by element, anything else by term identity (a func, a Buffer). A third argument (its value unused) says a and b are arrays; without it a slice at the top, one held by an interface, equals nothing, itself included. Nesting past 64 levels, other than through a struct's last field or an array's last element, is badcode, and so is a compare that visits more than 65536 terms (values sharing subvalues). Does not preempt. A fourth argument (15.6, gogo/maps), a mode, makes it a total order: a NaN equals a NaN and sorts before every other float, and a value it cannot order (a func, a Buffer, a pointer or a map at any depth, or with mode 2 a slice at the top) gives 2. */
    /* 58-60 were kept for distributed actors (15.5), which needed no
     * builtin (remote PIDs go through the VmDist hooks, actor.h). From
     * 15.7 58 is actor.Call with an error; 59-60 are free for a later
     * minor. */
    B_CALL_ERR        = 58,  /*! pid, req, ms -> reply, code | (15.7) actor.Call: B_CALL, with why it failed in place of ok, a shared error code (gogo_errors.h): 0 replied, GOGO_E_TIMEOUT no reply within ms (the server died meanwhile, or ms <= 0 and no reply yet), GOGO_E_NOPROC no live local actor (dead, never was, the zero PID, a port), GOGO_E_NOCONNECTION a remote PID the VmDist hook could not send to (its node is not connected, the PID names nothing any more, the table of remote Calls in flight is full) or whose node went away while waiting. */
    B_RESERVED_59     = 59,  /*! - -> - | Unused (badcode); free for a later minor. */
    B_RESERVED_60     = 60,  /*! - -> - | Unused (badcode); free for a later minor. */
    /* Since 15.6 (references, gogo/fmt). */
    B_SPRINT          = 61   /*! mode, v -> string | nil | The text println prints for v, without the newline (a box as its value, a pointer's target after &, a map as map[k:v ...]): mode 0 returns it as a new string, mode 1 writes it to the console (gogo/fmt). */
};
#define B_LAST B_SPRINT   /* the highest builtin: interp.c's builtin_info has a row for each up to it */

/* Type ids for TYPEIS; struct types use their type-table index. */
#define TY_INT    0x10000u
#define TY_BOOL   0x10001u
#define TY_STRING 0x10002u
#define TY_PID    0x10003u
#define TY_REF    0x10004u
#define TY_INT64  0x10005u
#define TY_BYTES  0x10006u   /* heap K_BYTES, off-heap handle, T_EMPTYBYTES */
#define TY_FLOAT32 0x10007u  /* (15.2) a 2-word K_FLOAT */
#define TY_FLOAT64 0x10008u  /* (15.2) a 3-word K_FLOAT */
/* 0x10009 - 0x100FF: reserved for later basic types (term.h). */

/* Reserved type-table indices: the compiler always emits the gogo/actor
 * struct types first, in this order (fields as in lib/actor/actor.gg). */
#define TYPE_PROCINFO 0   /* Memory int */
#define TYPE_REASON   1   /* Code int, Func string */
#define TYPE_EXITMSG  2   /* From PID, Reason Reason */
#define TYPE_DOWNMSG  3   /* Ref Ref, From PID, Reason Reason */
#define TYPE_REQUEST  4   /* From PID, Ref Ref, Body any */
#define TYPE_OPTIONS  5   /* MaxMemory int */
/* Then the gogo/gpio struct types (fields as in lib/gpio/gpio.gg). */
#define TYPE_GPIO_CONFIG 6   /* Pin, Mode, Edge, DebounceMs int: cfg of the "gpio" port */
#define TYPE_GPIO_WRITE  7   /* Value int: gpio command */
#define TYPE_GPIO_EDGE   8   /* Pin int, Value int: gpio event */
/* Then the gogo/net struct types (fields as in lib/net/net.gg). IPv4
 * addresses are ints (a.b.c.d = a<<24|b<<16|c<<8|d). */
#define TYPE_NET_IFCONFIG  9   /* (no fields): cfg of the "netif" port */
#define TYPE_NET_SETADDR  10   /* IP, Mask, Gateway, DNS int */
#define TYPE_NET_UP       11   /* IP, Gateway, DNS int */
#define TYPE_NET_DOWN     12   /* (no fields) */
#define TYPE_NET_ADDR     13   /* IP, Port int */
#define TYPE_NET_UDP      14   /* Port int: cfg of the "udp" port */
#define TYPE_NET_SENDTO   15   /* Addr Addr, Data bytes.Bytes */
#define TYPE_NET_DATAGRAM 16   /* From Addr, Data bytes.Bytes */
#define TYPE_NET_DIAL      17   /* IP, Port, TimeoutMs int: cfg of the "tcp" port */
#define TYPE_NET_LISTEN    18   /* Port int: cfg of the "tcp_listen" port */
#define TYPE_NET_SHUTDOWN  19   /* (no fields): tcp command, send FIN */
#define TYPE_NET_RESUME    20   /* (no fields): tcp command, unpause an accepted connection */
#define TYPE_NET_CONNECTED 21   /* (no fields): tcp event */
#define TYPE_NET_ACCEPTED  22   /* Conn PID: tcp_listen event */
/* Then the gogo/fs struct types (fields as in lib/fs/fs.gg). */
#define TYPE_FS_INFO  23   /* Size int, IsDir bool: fs.Stat */
#define TYPE_FS_SPACE 24   /* Used, Total int: fs.Usage */
/* Then the gogo/kbd and gogo/power struct types (lib/kbd/kbd.gg, lib/power/power.gg). */
#define TYPE_KBD_CONFIG    25   /* RepeatDelayMs, RepeatMs int: port.Open("kbd") */
#define TYPE_KBD_KEY       26   /* Code, Char, Mods int; Down bool: key event */
#define TYPE_POWER_CONFIG  27   /* PeriodMs int: port.Open("power") */
#define TYPE_POWER_BATTERY 28   /* MilliVolts, Pct, Charging, External int */
#define TYPE_POWER_READ    29   /* (no fields): ask for a Battery now */
/* Then the gogo/code struct type (lib/code/code.gg). */
#define TYPE_CODE_MODULE   30   /* H int, missing string: a module handle (module_builtin) */
/* Then the byte-stream protocol of every port that streams (lib/port/port.gg):
 * From is the port's PID. */
#define TYPE_CHUNK       31   /* From PID; Seq, TimeMs, Dropped int; Data bytes.Bytes: input */
#define TYPE_OUT         32   /* Data bytes.Bytes: output command */
#define TYPE_DONE        33   /* From PID, Seq int: an Out finished */
#define TYPE_CLOSED      34   /* From PID, Reason int: the port ended by itself */
/* Then the gogo/imu and gogo/ir types. */
#define TYPE_IMU_CONFIG  35   /* RateHz, Gestures, BatchMs int: port.Open("imu") */
#define TYPE_IMU_GESTURE 36   /* Kind, Value int: gesture event */
#define TYPE_IR_TX       37   /* CarrierHz, DutyPct int: port.Open("ir_tx") */
#define TYPE_IR_RX       38   /* GapUs int: port.Open("ir_rx") */
/* Then the gogo/audio types. */
#define TYPE_AUDIO_MIC     39   /* RateHz, FrameMs, GainDb int: port.Open("mic") */
#define TYPE_AUDIO_SPEAKER 40   /* RateHz, VolumePct int: port.Open("speaker") */
/* Then the tls port config. */
#define TYPE_NET_DIALTLS   41   /* IP, Port, TimeoutMs int, Host string: cfg of the "tls" port */
/* Then the embedding types (lib/port/port.gg). */
#define TYPE_HOST          42   /* Arg int, Data bytes.Bytes: cfg of a host-defined port */
#define TYPE_POST          43   /* From PID, Kind int, Data bytes.Bytes: a host post, host-port command or event */
/* Then (15.1) the gogo/screen type (lib/screen/screen.gg). */
#define TYPE_SCREEN_CONFIG 44   /* Cols, Rows int: port.Open("screen") */
/* Then (15.4) the gogo/errors types (lib/errors/errors.gg): the errors
 * that may cross modules. */
#define TYPE_ERRORS_CODE        45   /* the box of an errors.Code (an int) in an interface */
#define TYPE_ERRORS_ERRORSTRING 46   /* s string: errors.New's error */
/* Then (15.5) the "node" port's types (lib/net/net.gg, runtime/dist/). */
#define TYPE_NET_NODELINK  47   /* Self, Peer, Relay int; Agent PID; Name string; Key Bytes; Conn PID; Hello Bytes: port.Open("node") */
#define TYPE_NET_NODEEVENT 48   /* From PID; Kind, Node int; Agent PID; Name string: node port event */
/* How many reserved types this VM knows (0 .. TYPE_RESERVED_COUNT - 1):
 * they are global across modules. The one constant to bump with a new
 * reserved type (compiler: len(reservedTypes)); a new one is an image
 * format minor (image.h). */
#define TYPE_RESERVED_COUNT 49u
/* Field counts of the reserved types this VM knows, in index order, two
 * per byte (TYPE_NF2(even, odd)). An image's reserved row may declare at
 * most this many (image_load): fields are only ever appended, of type
 * int, 0 meaning the default, so a struct of an older image has a prefix
 * of the fields, and GETFIELD past its end reads int 0. A new reserved
 * type or an appended field is an image format minor (image.h).
 * TestReservedFieldsMatchC checks this table against compiler.reservedTypes.
 * These readers check a struct's exact size today; when their type gains a
 * field, relax each to "at least" and read the new field only when present:
 * net.SendTo and net.Addr (net/udp.c), port.Out (stream/stream.c),
 * port.Post (embed/hostport.c), net.SetAddr (net/ports.c), actor.Options
 * (vm_spawn_opt_words, interp.c), actor.Request (B_REPLY, interp.c) and
 * code.Module (module_obj, module.c). */
#define TYPE_NF2(a, b) ((uint8_t)((a) | (b) << 4))
#define TYPE_NFIELDS { \
    TYPE_NF2(1, 2), /* 0 ProcInfo, Reason */ \
    TYPE_NF2(2, 3), /* 2 ExitMsg, DownMsg */ \
    TYPE_NF2(3, 1), /* 4 Request, Options */ \
    TYPE_NF2(4, 1), /* 6 gpio.Config, gpio.Write */ \
    TYPE_NF2(2, 0), /* 8 gpio.Edge, net.IfConfig */ \
    TYPE_NF2(4, 3), /* 10 net.SetAddr, net.Up */ \
    TYPE_NF2(0, 2), /* 12 net.Down, net.Addr */ \
    TYPE_NF2(1, 2), /* 14 net.UDP, net.SendTo */ \
    TYPE_NF2(2, 3), /* 16 net.Datagram, net.Dial */ \
    TYPE_NF2(1, 0), /* 18 net.Listen, net.Shutdown */ \
    TYPE_NF2(0, 0), /* 20 net.Resume, net.Connected */ \
    TYPE_NF2(1, 2), /* 22 net.Accepted, fs.Info */ \
    TYPE_NF2(2, 2), /* 24 fs.Space, kbd.Config */ \
    TYPE_NF2(4, 1), /* 26 kbd.Key, power.Config */ \
    TYPE_NF2(4, 0), /* 28 power.Battery, power.Read */ \
    TYPE_NF2(2, 5), /* 30 code.Module, port.Chunk */ \
    TYPE_NF2(1, 2), /* 32 port.Out, port.Done */ \
    TYPE_NF2(2, 3), /* 34 port.Closed, imu.Config */ \
    TYPE_NF2(2, 2), /* 36 imu.Gesture, ir.TX */ \
    TYPE_NF2(1, 3), /* 38 ir.RX, audio.Mic */ \
    TYPE_NF2(2, 4), /* 40 audio.Speaker, net.DialTLS */ \
    TYPE_NF2(2, 3), /* 42 port.Host, port.Post */ \
    TYPE_NF2(2, 1), /* 44 screen.Config (15.1), errors.Code (15.4) */ \
    TYPE_NF2(1, 8), /* 46 errors.errorString (15.4), net.NodeLink (15.5) */ \
    TYPE_NF2(5, 0), /* 48 net.NodeEvent (15.5) */ \
}
/* The first own type: an image names its own struct types from here up
 * (image.h), whatever reserved types it or the VM knows, and a struct of
 * an own type carries its module tag in its type word (term.h). Fixed
 * forever: reserved types stay below it. */
#define TYPE_OWN_BASE 128u

/* Exit reason codes (actor.Normal ...). 10..63 are kept for the VM;
 * 64..99 are library failures, actor.Failed (64) + a GOGO_E_* code
 * (gogo_errors.h), which the VM treats like user codes; user codes are
 * >= 100. */
enum Reason {
    R_NORMAL           = 0,
    R_KILLED           = 1,
    R_OUT_OF_MEMORY    = 2,
    R_MAILBOX_OVERFLOW = 3,
    R_NO_PROC          = 4,
    R_DIV_ZERO         = 5,
    R_BAD_CODE         = 6,
    R_SHUTDOWN         = 7,
    R_BAD_SHIFT        = 8,
    R_BAD_MSG          = 9,   /* a value that may not cross modules or nodes (msg.h, runtime/dist/) */
    R_PANIC            = 10,  /* panic(v), a failed type assertion, a method call on a nil interface (15.4) */
    R_NO_CONNECTION    = 11,  /* the node of a linked or monitored remote actor became unreachable (15.5) */
    R_NIL              = 12,  /* a nil pointer dereference, a write to a nil map: GETFIELD or SETFIELD of nil (15.6) */
    R_FAILED           = 64   /* + a GOGO_E_* code: a library failure (no VM code uses it) */
};

/* Instruction decoders. */
#define OP_OP(w) ((uint32_t)(w) & 0xFFu)
#define OP_A(w)  (((uint32_t)(w) >> 8) & 0xFFu)
#define OP_B(w)  (((uint32_t)(w) >> 16) & 0xFFu)
#define OP_C(w)  (((uint32_t)(w) >> 24) & 0xFFu)
#define OP_MAKE(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))

/* True for ops followed by one extra word. */
static inline bool op_has_ext(uint32_t op) {
    switch (op) {
    case OP_LOADK: case OP_LOADI32: case OP_JMP: case OP_JMPF: case OP_JMPT:
    case OP_CALL: case OP_NEWSTRUCT: case OP_MKCLOSURE: case OP_TYPEIS:
    case OP_BUILTIN:
        return true;
    default:
        return false;
    }
}

#endif
