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
    OP_CONV      = 25   /* c=0: ra = int(rb) (low 32 bits); c=1: ra = int64(rb) */
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
    B_UNREGISTER      = 25   /* name -> nil */
};

/* Type ids for TYPEIS; struct types use their type-table index. */
#define TY_INT    0x10000u
#define TY_BOOL   0x10001u
#define TY_STRING 0x10002u
#define TY_PID    0x10003u
#define TY_REF    0x10004u
#define TY_INT64  0x10005u

/* Reserved type-table indices: the compiler always emits the gogo/actor
 * struct types first, in this order (fields as in actor/actor.go). */
#define TYPE_PROCINFO 0   /* Memory int */
#define TYPE_REASON   1   /* Code int, Func string */
#define TYPE_EXITMSG  2   /* From PID, Reason Reason */
#define TYPE_DOWNMSG  3   /* Ref Ref, From PID, Reason Reason */
#define TYPE_REQUEST  4   /* From PID, Ref Ref, Body any */
#define TYPE_OPTIONS  5   /* MaxMemory int */

/* Exit reason codes (actor.Normal ...); user codes are >= 100. */
enum Reason {
    R_NORMAL           = 0,
    R_KILLED           = 1,
    R_OUT_OF_MEMORY    = 2,
    R_MAILBOX_OVERFLOW = 3,
    R_NO_PROC          = 4,
    R_DIV_ZERO         = 5,
    R_BAD_CODE         = 6,
    R_SHUTDOWN         = 7
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
