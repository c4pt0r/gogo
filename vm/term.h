/* 32-bit tagged term representation. See the "Term tags" table in the design.
 *
 *   ...1  small int (31-bit signed)   (v << 1) | 1
 *   ..00  heap ref                    word_offset << 2
 *   0010  special                     T_NIL, T_FALSE, T_TRUE, T_EMPTYSTR
 *   0110  PID                         (gen << 20) | (slot << 4) | 0x6
 *   1010  const ref                   const_word_offset << 4 | 0xA
 *   1110  off-heap handle             reserved (M2)
 */
#ifndef GOGO_TERM_H
#define GOGO_TERM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint32_t term;

#define T_NIL   ((term)0x02)
#define T_FALSE ((term)0x12)
#define T_TRUE  ((term)0x22)
/* The empty string: zero value of string-typed vars and fields. */
#define T_EMPTYSTR ((term)0x32)

/* Heap object kinds. */
enum {
    K_STRUCT  = 1,
    K_STRING  = 2,
    K_INT     = 3,
    K_CLOSURE = 4,
    K_REF     = 5,   /* [hdr][serial_lo][serial_hi] */
    K_INT64   = 6,   /* [hdr][lo][hi]; every int64 value, even small ones */
    K_FORWARD = 63
};

#define TERM_INT_MIN (-1073741824)
#define TERM_INT_MAX 1073741823

/* Small ints. */
static inline bool term_is_int(term t) { return (t & 1u) != 0; }
static inline bool term_int_fits(int32_t v) { return v >= TERM_INT_MIN && v <= TERM_INT_MAX; }
static inline term term_from_int(int32_t v) { return ((uint32_t)v << 1) | 1u; }
static inline int32_t term_to_int(term t) { return (int32_t)t >> 1; }

/* Heap references: word offset into the actor's memory block. */
static inline bool term_is_ref(term t) { return (t & 3u) == 0; }
static inline term term_from_ref(uint32_t off) { return off << 2; }
static inline uint32_t term_ref_off(term t) { return t >> 2; }

/* PIDs: 16-bit slot, 12-bit generation. */
static inline bool term_is_pid(term t) { return (t & 0xFu) == 0x6u; }
static inline term term_make_pid(uint16_t slot, uint16_t gen) {
    return ((uint32_t)(gen & 0xFFFu) << 20) | ((uint32_t)slot << 4) | 0x6u;
}
static inline uint16_t term_pid_slot(term t) { return (uint16_t)((t >> 4) & 0xFFFFu); }
static inline uint16_t term_pid_gen(term t) { return (uint16_t)(t >> 20); }

/* Const-area references: word offset into the image's const area. */
static inline bool term_is_const(term t) { return (t & 0xFu) == 0xAu; }
static inline term term_from_const(uint32_t off) { return (off << 4) | 0xAu; }
static inline uint32_t term_const_off(term t) { return t >> 4; }

static inline term term_from_bool(bool b) { return b ? T_TRUE : T_FALSE; }

/* Object header: (total_words << 6) | kind; total_words includes the header. */
static inline uint32_t hdr_make(uint32_t words, uint32_t kind) { return (words << 6) | kind; }
static inline uint32_t hdr_words(uint32_t h) { return h >> 6; }
static inline uint32_t hdr_kind(uint32_t h) { return h & 63u; }

#endif
