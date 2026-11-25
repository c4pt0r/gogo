/* Per-actor heap allocation, stack frames and the copying GC. */
#ifndef GOGO_HEAP_H
#define GOGO_HEAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"
#include "module.h"

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

/* Like heap_gc, but sizes the block to exactly the live data plus
 * need_words (at least MIN_BLOCK_WORDS words), with no growth slack: for a
 * retry after a request failed against the limit, which counts the whole
 * block (off-heap Bytes, an allocation that the slack of an earlier growth
 * crowds out). The limit is read after the collection, so Bytes it
 * released count as free. With need_words > 0 it leaves the (compacted)
 * block as it is when even that cannot fit under the limit; with 0 it
 * always resizes to the live data, which may itself exceed the limit (the
 * block never drops below what is live). */
void heap_gc_tight(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots);

/* For a non-fatal delivery (net data) refused because the receiver's
 * block, grown with slack, leaves no room under its limit: if a is waiting
 * in Receive (parked, every live term in its block, as gc_holders in
 * bytes.c relies on), collects it with heap_gc_tight. True if that freed
 * anything against its limit: the block shrank, or the sweep released
 * held Bytes (fewer charged words). Never called on the running actor. */
bool heap_reclaim_waiting(Vm *vm, Actor *a);

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
 * or points outside the heap / const area. A const ref resolves through the
 * module table (const_obj_of, module.h). */
static inline const uint32_t *obj_of(const Vm *vm, const Actor *a, term t) {
    if (term_is_const(t)) return const_obj_of(vm, t);
    if (!term_is_ref(t)) return NULL;
    uint32_t off = term_ref_off(t), limit = a->hp;
    /* Every object has at least two words and lies wholly inside the heap. */
    if (off >= limit || limit - off < 2u) return NULL;
    uint32_t words = hdr_words(a->mem[off]);
    if (words < 2u || words > limit - off) return NULL;
    return a->mem + off;
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
