/* Per-actor heap allocation, stack frames and the copying GC. */
#ifndef GOGO_HEAP_H
#define GOGO_HEAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"

#define HEAP_MAX_WORDS 65535u

/* Reserves `words` heap words and returns their word offset, or UINT32_MAX
 * if they cannot fit even after GC without the block growing past the
 * actor's limit: actor_max_words minus actor_charged_words (queued messages
 * and pending SendAfter timers), so heap block + charged words <= limit;
 * never past HEAP_MAX_WORDS. May GC, which moves objects: refs held outside
 * the stack must be passed in `roots` (updated in place). The caller must
 * fill in the object before the next allocation. */
uint32_t heap_alloc(Vm *vm, Actor *a, uint32_t words, term *roots, int n_roots);

/* Allocates a K_REF with the next serial of the VM-global counter and
 * returns it as a term; T_NIL if out of memory. May GC (like heap_alloc), so
 * the caller reloads pointers into the heap. */
term heap_new_ref(Vm *vm, Actor *a);

/* Copies everything reachable from the stack and `roots` into a new block
 * sized so that `need_words` more words fit if the limit allows (see
 * heap.c). At most two blocks of this actor exist at any time during it. */
void heap_gc(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots);

/* Pushes a frame of n_regs registers (all T_NIL) plus the saved caller fp
 * and ret_pc (-1 for an entry frame). May GC. False if out of memory or
 * over the limit (as for heap_alloc). */
bool heap_push_frame(Vm *vm, Actor *a, uint16_t n_regs, int32_t ret_pc);

/* Which words of the heap object at `obj` hold terms: [*first, *end) as
 * indexes into the object (empty range for K_STRING, K_INT, K_REF,
 * K_INT64, K_BYTEARR and K_BYTES, whose contents are raw; never word 1, which is
 * raw in every kind: type_id, func_idx, a slice's off, an array's length, a buffer's length;
 * end is always the object's size when the range is not empty). Shared by the
 * GC and by message copy/relocation so they always agree. */
void heap_obj_terms(const uint32_t *obj, uint32_t *first, uint32_t *end);

/* sizeof(Actor) + its table slot + the block + its charged words (queued
 * messages, pending SendAfter timers) in bytes. */
size_t actor_memory_bytes(const Actor *a);

/* ---- reading terms (interp.c, port.c) ---- */

/* Pointer to the object a heap or const ref names, or NULL if t is neither
 * or points outside the heap / const area. */
static inline const uint32_t *obj_of(const Vm *vm, const Actor *a, term t) {
    const uint32_t *base;
    uint32_t off, limit;
    if (term_is_ref(t)) {
        base = a->mem;
        off = term_ref_off(t);
        limit = a->hp;
    } else if (term_is_const(t)) {
        base = vm->img->consts;
        off = term_const_off(t);
        limit = vm->img->consts_words;
    } else {
        return NULL;
    }
    /* Every object has at least two words and lies wholly inside its area. */
    if (off >= limit || limit - off < 2u) return NULL;
    uint32_t words = hdr_words(base[off]);
    if (words < 2u || words > limit - off) return NULL;
    return base + off;
}

/* The bytes of a name argument: a string constant, a heap string or the
 * empty string. False for anything else. */
static inline bool name_bytes(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
    if (t == T_EMPTYSTR) {
        *p = NULL;
        *len = 0;
        return true;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_STRING || o[1] > (hdr_words(o[0]) - 2u) * 4u) return false;
    *p = (const uint8_t *)(o + 2);
    *len = o[1];
    return true;
}

/* Value of a small int or boxed K_INT; false for anything else. */
static inline bool int_val(const Vm *vm, const Actor *a, term t, int32_t *out) {
    if (term_is_int(t)) {
        *out = term_to_int(t);
        return true;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (o && hdr_kind(o[0]) == K_INT) {
        *out = (int32_t)o[1];
        return true;
    }
    return false;
}

#endif
