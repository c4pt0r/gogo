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
    OP_BUILTIN   = 24   /* ra = builtin ext(rb..rb+c-1)        +ext */
};

enum Builtin {
    B_PRINTLN = 0,
    B_SPAWN   = 1,
    B_SELF    = 2,
    B_SEND    = 3,
    B_RECEIVE = 4,
    B_MEMORY  = 5
};

/* Type ids for TYPEIS; struct types use their type-table index. */
#define TY_INT    0x10000u
#define TY_BOOL   0x10001u
#define TY_STRING 0x10002u
#define TY_PID    0x10003u

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
