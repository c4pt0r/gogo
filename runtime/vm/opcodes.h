/* Bytecode definitions shared by the VM and the compiler.
 * Instruction word: op | a<<8 | b<<16 | c<<24. Ops marked +ext are
 * followed by one extra word. Every member has an explicit number because
 * the Go compiler parses this file.
 */
#ifndef GOGO_OPCODES_H
#define GOGO_OPCODES_H

#include <stdint.h>
#include <stdbool.h>

enum Op {
    OP_NOP       = 0,
    OP_MOVE      = 1,   /* ra = rb */
    OP_LOADK     = 2,   /* ra = ext (raw term)                 +ext */
    OP_LOADI32   = 3,   /* ra = int32 ext (boxed if needed)    +ext */
    OP_ADD       = 4,
    OP_SUB       = 5,
    OP_MUL       = 6,
    OP_DIV       = 7,
    OP_REM       = 8,
    OP_NEG       = 9,   /* ra = -rb */
    OP_EQ        = 10,
    OP_NE        = 11,
    OP_LT        = 12,
    OP_LE        = 13,
    OP_NOT       = 14,
    OP_JMP       = 15,  /* pc = code + ext                     +ext */
    OP_JMPF      = 16,  /* jump if ra is false                 +ext */
    OP_JMPT      = 17,  /* jump if ra is true                  +ext */
    OP_CALL      = 18,  /* func ext, args rb..rb+c-1 -> ra     +ext */
    OP_RET       = 19,
    OP_NEWSTRUCT = 20,  /* ra = struct type ext, fields rb..   +ext */
    OP_GETFIELD  = 21,  /* ra = rb.fields[c] */
    OP_MKCLOSURE = 22,  /* ra = closure func ext, captures rb.. +ext */
    OP_TYPEIS    = 23,  /* ra = (dynamic type of rb == ext)    +ext */
    OP_BUILTIN   = 24,  /* ra = builtin ext(rb..rb+c-1)        +ext */
    OP_CONV      = 25,  /* c=0: ra = int(rb) (low 32 bits); c=1: ra = int64(rb);
                         * c=2: ra = byte(rb) = rb & 0xFF (rb an int or int64) */
    /* int64 ops. Operands are int64 values in either representation: a
     * small-int immediate or a K_INT64 box (heap or const area); results
     * are immediates when they fit in 31 bits, else boxed. The generic
     * ADD..LE / NEG above are int-only. */
    OP_ADD64     = 26,
    OP_SUB64     = 27,
    OP_MUL64     = 28,
    OP_DIV64     = 29,
    OP_REM64     = 30,
    OP_NEG64     = 31,  /* ra = -rb */
    OP_EQ64      = 32,
    OP_NE64      = 33,
    OP_LT64      = 34,
    OP_LE64      = 35,
    OP_BOX64     = 36,  /* ra = rb as a K_INT64 box (no-op if already boxed);
                         * an int64 inside `any` is always boxed */
    OP_CLEAR     = 37,  /* ra .. ra+b-1 = nil: dead registers stop being GC
                         * roots (b is the count, 0 = nothing) */
    OP_RETN      = 38,  /* return ra .. ra+b-1 into the caller's registers
                         * dst .. dst+b-1, dst = the CALL's or CALLV's ra
                         * (RET returns one) */
    OP_CALLV     = 39,  /* call the closure in rb with args rb+1..rb+c -> ra:
                         * its captures, then the args, are the callee's
                         * parameters; anything but a closure whose function
                         * takes captures+c parameters (nil) is badcode */
    /* Bitwise ops, Go semantics, int then int64 (as ADD / ADD64). SHL / SHR
     * take any integer count (either representation): a count >= the width
     * gives 0 (SHR: 0 or -1 by sign), a negative count crashes with
     * R_BAD_SHIFT. SHR is arithmetic. */
    OP_AND       = 40,
    OP_OR        = 41,
    OP_XOR       = 42,
    OP_ANDNOT    = 43,  /* ra = rb &^ rc */
    OP_SHL       = 44,
    OP_SHR       = 45,
    OP_AND64     = 46,
    OP_OR64      = 47,
    OP_XOR64     = 48,
    OP_ANDNOT64  = 49,
    OP_SHL64     = 50,
    OP_SHR64     = 51
};

enum Builtin {
    B_PRINTLN = 0,
    B_SPAWN   = 1,
    B_SELF    = 2,
    B_SEND    = 3,
    B_RECEIVE = 4,
    B_MEMORY  = 5,
    /* Two-result builtins write ra and ra+1. */
    B_SPAWN_LINK      = 6,   /* closure -> pid */
    B_SPAWN_OPT       = 7,   /* closure, Options struct -> pid */
    B_LINK            = 8,   /* pid -> nil */
    B_UNLINK          = 9,   /* pid -> nil */
    B_TRAP_EXIT       = 10,  /* bool -> nil */
    B_EXIT            = 11,  /* code (does not return) */
    B_KILL            = 12,  /* pid, code -> nil */
    B_MONITOR         = 13,  /* pid -> ref */
    B_DEMONITOR       = 14,  /* ref -> nil */
    B_MAKE_REF        = 15,  /* -> ref */
    B_RECEIVE_TIMEOUT = 16,  /* ms -> msg, ok */
    B_SEND_AFTER      = 17,  /* ms, pid, msg -> ref */
    B_CANCEL_TIMER    = 18,  /* ref -> bool */
    B_SLEEP           = 19,  /* ms -> nil */
    B_NOW             = 20,  /* -> int64 */
    B_CALL            = 21,  /* pid, req, ms -> reply, ok */
    B_REPLY           = 22,  /* Request struct, reply -> nil */
    B_REGISTER        = 23,  /* name, pid -> bool */
    B_WHEREIS         = 24,  /* name -> pid, ok */
    B_UNREGISTER      = 25,  /* name -> nil */
    B_PORT_OPEN       = 26,  /* driver string, cfg any -> pid, ok */
    B_PORT_CLOSE      = 27,  /* pid -> nil */
    /* Sequences (strs.c): the generic ones dispatch on the kind of their
     * first argument. Out-of-range -> badcode, allocation failure -> OOM. */
    B_STR_CONCAT      = 28,  /* a, b -> string */
    B_LEN             = 29,  /* string | slice | Bytes | Buffer -> int */
    B_INDEX           = 30,  /* string | slice | Bytes, i -> element; a byte
                              * (of a string, []byte or Bytes) as an int */
    B_SETINDEX        = 31,  /* slice, i, v -> nil; a []byte stores v & 0xFF */
    B_SLICE           = 32,  /* string | slice | Bytes, lo, hi -> the same kind; every
                              * bound is explicit, any negative or out-of-range
                              * one is badcode (a slice's hi may reach cap) */
    B_MAKE_SLICE      = 33,  /* elemkind (0 term, 1 byte), len, cap, zero -> slice;
                              * every element is `zero` (0 for bytes) */
    B_APPEND          = 34,  /* slice, mode, zero, v... -> slice. mode 0: append the
                              * terms v...; 1: the bytes v...; 2: every element of
                              * the term slice v (one v); 3: every byte of the
                              * []byte or string v (one v). Growth is decided once
                              * for all of them: if they do not all fit in cap, a
                              * new array (x2, min 4, spare slots = zero) gets
                              * them and the old one is left untouched */
    B_COPY            = 35,  /* dst slice, src slice | string | Bytes -> n copied */
    B_CAP             = 36,  /* slice -> int */
    B_STR_FROM        = 37,  /* []byte | Bytes -> string (a copy) */
    B_BYTESLICE_FROM  = 38,  /* string | Bytes -> []byte (a copy) */
    B_STR_CMP         = 39,  /* a, b -> -1 | 0 | 1 */
    /* bytes.Buffer and bytes.Bytes (strs.c, handles in bytes.c). A Buffer
     * builtin on anything but a Buffer (the zero Buffer is nil) is badcode. */
    B_BUF_NEW         = 40,  /* cap -> Buffer */
    B_BUF_WRITE       = 41,  /* buf, string | []byte | Bytes | byte (an int) -> nil */
    B_BUF_FREEZE      = 42,  /* buf -> Bytes (a copy: heap K_BYTES up to
                              * GOGO_BYTES_INLINE bytes, else off-heap); buf
                              * is emptied and stays usable */
    B_BYTES_FROM      = 43,  /* string | []byte -> Bytes (a heap copy, any size) */
    B_BYTES_EQUAL     = 44,  /* Bytes, Bytes -> bool (same contents) */
    B_BYTES_COPYTO    = 45,  /* Bytes, []byte -> n copied */
    B_PORT_HANDOFF    = 46,  /* port pid, to pid -> bool (port.h port_handoff) */
    B_NET_SUBSCRIBE   = 47,  /* -> bool: net.Subscribe (port.h port_subscribe) */
    B_CONFIG_GET      = 48,  /* key string -> value string, ok bool: sys.Config
                              * (host->config_get; "wifi." keys hidden) */
    B_FS              = 49,  /* op int, args... -> result(s): gogo/fs, run by
                              * the Vm's fs_builtin hook (actor.h; vfs/builtin.c) */
    B_CODE            = 50,  /* op int, args... -> result(s): gogo/code
                              * (module_builtin, module.h; CODE_OP_*) */
    B_UNIX_TIME       = 51,  /* -> int64: sys.Time, vm_unix_time_ms (0: unknown) */
    B_SET_UNIX_TIME   = 52,  /* ms int64 -> bool: sys.SetTime, vm_set_unix_time_ms (false: below the build-time floor) */
    B_RANDOM          = 53   /* -> int in [0, 2^30): sys.Random, vm_random */
};

/* Type ids for TYPEIS; struct types use their type-table index. */
#define TY_INT    0x10000u
#define TY_BOOL   0x10001u
#define TY_STRING 0x10002u
#define TY_PID    0x10003u
#define TY_REF    0x10004u
#define TY_INT64  0x10005u
#define TY_BYTES  0x10006u   /* heap K_BYTES, off-heap handle, T_EMPTYBYTES */
/* 0x10007 - 0x100FF: reserved for later basic types (term.h). */

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
/* How many reserved types this VM knows (0 .. TYPE_RESERVED_COUNT - 1):
 * they are global across modules. The one constant to bump with a new
 * reserved type (compiler: len(reservedTypes)); a new one is an image
 * format minor (image.h). */
#define TYPE_RESERVED_COUNT 45u
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
    TYPE_NF2(2, 0), /* 44 screen.Config (15.1) */ \
}
/* The first own type: an image names its own struct types from here up
 * (image.h), whatever reserved types it or the VM knows, and a struct of
 * an own type carries its module tag in its type word (term.h). Fixed
 * forever: reserved types stay below it. */
#define TYPE_OWN_BASE 128u

/* Exit reason codes (actor.Normal ...); user codes are >= 100. */
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
    R_BAD_MSG          = 9    /* a value that may not cross modules (msg.h) */
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
