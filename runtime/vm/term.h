/* 32-bit tagged term representation. See docs/design.md §5.5.2.
 *
 *   ...1  small int (31-bit signed)   (v << 1) | 1
 *   ..00  heap ref                    word_offset << 2
 *   0010  special                     T_NIL, T_FALSE, T_TRUE, T_EMPTYSTR, T_ZEROREF,
 *                                     T_NILSLICE, T_EMPTYBYTES
 *   0110  PID                         (gen << 20) | (slot << 4) | 0x6; T_ZEROPID
 *   1010  const ref                   tag | const_word_offset << 4 | 0xA (below)
 *   1110  off-heap Bytes handle       (index << 4) | 0xE (bytes.h)
 *
 * Module tags (hot loading, module.h; docs/design.md §5.5.3). Every term or raw
 * word that names something in a module's image carries the module's tag in
 * its top 11 bits: generation (low 8 bits of the slot's generation) in
 * 31..24, slot in 23..21.
 *   const ref        gen:8 | slot:3 | offset:17 | 1010
 *   K_CLOSURE word 1 gen:8 | slot:3 | func index:21
 *   K_STRUCT word 1  gen:8 | slot:3 | 0:5 | type index:16 for a module's own
 *                    types (index >= TYPE_OWN_BASE); a reserved type
 *                    (system packages, opcodes.h) is its bare index: global,
 *                    the same in every module
 * Images hold untagged refs (tag 0: offset < CONST_MAX_WORDS, as before);
 * the VM ORs the module's tag in where an image word becomes a runtime term
 * (LOADK, MKCLOSURE, NEWSTRUCT, a crash's function name, a field read out of
 * a const object). Slot 0 is the boot image (module 0), which is never
 * unloaded, so its generation stays 0 and its tag is 0: a module-0 ref is
 * bit for bit the image ref. Every read through a tagged word checks the
 * slot's current generation (module_of_tag); a mismatch is `badcode`.
 *
 * Reserved for the language phases still to come (interfaces, pointers,
 * maps), so that they need an image format minor, not a major (image.h):
 *   - heap kinds 12-15 (K_*, below; image_load refuses a const object of
 *     an unknown kind);
 *   - the specials from 0x72 up (T_*: only 0x02-0x62 are given);
 *   - the TYPEIS ids 0x10007-0x100FF beside TY_* (opcodes.h);
 *   - bits 16-20 of a K_STRUCT's type word (0 in every v15 image), for
 *     per-type flags.
 * Nothing in a v15 compiler or VM produces any of them; the image
 * version check, not the loader, keeps them from an older VM.
 *
 * Residual alias bound: a tag carries 8 generation bits, so a stale term
 * resolves again (to whatever module then sits in its slot) only after
 * exactly 256 more loads into that same slot while the term survives.
 * module_load picks free slots round-robin, so with the 7 non-boot slots
 * free that takes about 7 x 256 = 1792 loads. Even then the result is
 * memory-safe (a const ref must still name an object start of the newer
 * module; a closure runs only through that module's checked function
 * table), but it is the wrong module's data or code, not `badcode`. */
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
/* The zero actor.Ref: a special, so it is an immediate (never scanned by
 * the GC, copied verbatim into messages) that TYPEIS reports as TY_REF and
 * whose serial counts as 0. Live refs are K_REF heap objects with serials
 * from 1 up (Vm.next_ref starts at 1), so it equals no live ref.
 * CancelTimer and Demonitor on it do nothing. Prints as <ref 0>. */
#define T_ZEROREF ((term)0x42)
/* The zero actor.PID: the PID term of slot 0, generation 0. actor.c never
 * gives slot 0 generation 0 (it starts at 1 and skips 0 on wrap) and port
 * PIDs use slots >= PORT_SLOT_BASE, so no live actor or port has it: Send
 * drops, Link/Monitor see a dead PID (NoProc). Prints as <pid 0>. */
#define T_ZEROPID ((term)0x06)
/* The nil slice, the zero value of every slice type: length and capacity
 * 0. Only `s == nil` compares slices; an empty non-nil slice is a K_SLICE
 * of capacity 0. Prints as []. */
#define T_NILSLICE ((term)0x52)
/* The empty bytes.Bytes, its zero value: length 0, never allocated. */
#define T_EMPTYBYTES ((term)0x62)
/* Specials from 0x72 up: reserved (top). */

/* Heap object kinds. */
enum {
    K_STRUCT  = 1,
    K_STRING  = 2,
    K_INT     = 3,
    K_CLOSURE = 4,
    K_REF     = 5,   /* [hdr][serial_lo][serial_hi] */
    K_INT64   = 6,   /* [hdr][lo][hi]; int64 values outside 31 bits, and every int64 inside `any` */
    K_SLICE   = 7,   /* [hdr][off][len][cap][array]: raw off/len/cap; array is a
                      * K_ARRAY or K_BYTEARR ref, T_NIL when cap is 0 */
    K_ARRAY   = 8,   /* [hdr][n][elements: n terms], mutable */
    K_BYTEARR = 9,   /* [hdr][nbytes][bytes, zero-padded], mutable, not scanned */
    K_BYTES   = 10,  /* [hdr][nbytes][bytes, zero-padded], immutable, not scanned:
                      * a heap bytes.Bytes (any size) */
    K_BUF     = 11,  /* [hdr][len][array]: a bytes.Buffer; raw len, array a
                      * K_BYTEARR ref (its capacity), T_NIL when cap is 0 */
    /* 12-15: reserved for interface values, pointers and maps (top). */
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

/* Module tags (see the top of this file). */
#define MOD_TAG_SHIFT 21u
#define MOD_TAG_MASK  0xFFE00000u
#define MOD_SLOT_MASK 7u               /* slot bits: at most 8 modules */
#define MOD_GEN_MASK  255u             /* generation bits carried in a tag */
static inline uint32_t mod_tag_make(uint32_t slot, uint32_t gen) {
    return ((gen & MOD_GEN_MASK) << 24) | ((slot & MOD_SLOT_MASK) << MOD_TAG_SHIFT);
}
static inline uint32_t mod_tag_slot(uint32_t w) { return (w >> MOD_TAG_SHIFT) & MOD_SLOT_MASK; }
static inline uint32_t mod_tag_gen(uint32_t w) { return w >> 24; }

/* Index limits left by the tag: const word offsets (17 bits: 512 KB of
 * constants per image; the ESP image partition is 1 MB), function indexes
 * (21), struct type indexes (16: TYPEIS uses 0x10000+ for the basic types,
 * opcodes.h). image_load and the compiler check them. */
#define CONST_MAX_WORDS (1u << 17)
#define FUNC_MAX        (1u << 21)
#define TYPE_MAX        (1u << 16)

/* Const-area references: tag | word offset into that module's const area.
 * term_from_const makes the untagged (image, module 0) form. */
static inline bool term_is_const(term t) { return (t & 0xFu) == 0xAu; }
static inline term term_from_const(uint32_t off) { return (off << 4) | 0xAu; }
static inline uint32_t term_const_off(term t) { return (t >> 4) & (CONST_MAX_WORDS - 1u); }

/* Off-heap Bytes handles: index into the handle table (bytes.h). */
static inline bool term_is_handle(term t) { return (t & 0xFu) == 0xEu; }
static inline term term_from_handle(uint32_t idx) { return (idx << 4) | 0xEu; }
static inline uint32_t term_handle_idx(term t) { return t >> 4; }

static inline term term_from_bool(bool b) { return b ? T_TRUE : T_FALSE; }

/* Object header: (total_words << 6) | kind; total_words includes the header. */
static inline uint32_t hdr_make(uint32_t words, uint32_t kind) { return (words << 6) | kind; }
static inline uint32_t hdr_words(uint32_t h) { return h >> 6; }
static inline uint32_t hdr_kind(uint32_t h) { return h & 63u; }

#endif
