/* Per-actor heap allocation, stack frames and the copying GC. */
#ifndef GOGO_HEAP_H
#define GOGO_HEAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"
#include "module.h"
#include "opcodes.h"

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
#define heap_gc(vm, a, need_words, roots, n_roots) heap_collect(vm, a, need_words, roots, n_roots, false)

/* Like heap_gc, but sizes the block to exactly the live data plus
 * need_words (at least MIN_BLOCK_WORDS words), with no growth slack: for a
 * retry after a request failed against the limit, which counts the whole
 * block (off-heap Bytes, an allocation that the slack of an earlier growth
 * crowds out). The limit is read after the collection, so Bytes it
 * released count as free. With need_words > 0 it leaves the (compacted)
 * block as it is when even that cannot fit under the limit; with 0 it
 * always resizes to the live data, which may itself exceed the limit (the
 * block never drops below what is live). */
#define heap_gc_tight(vm, a, need_words, roots, n_roots) heap_collect(vm, a, need_words, roots, n_roots, true)

/* heap_gc (tight false) and heap_gc_tight (true): one entry, as they share
 * the collect, measure and resize steps. */
void heap_collect(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots, bool tight);

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
 * indexes into the object. *end is always the object's size (a kind's
 * terms run to its last word); the range is empty (*first = *end) for
 * K_STRING, K_INT, K_REF, K_INT64, K_BYTEARR, K_BYTES and K_FLOAT, whose
 * contents are raw, and for a K_SLICE of fewer than 5 words. Never word 1,
 * which is raw in every kind: type_id, func_idx, a slice's off, an array's
 * length, a buffer's length. Shared by the GC, message copy/relocation and
 * image validation so they always agree. */
void heap_obj_terms(const uint32_t *obj, uint32_t *first, uint32_t *end);

/* sizeof(Actor) + its table slot + the block + its charged words (queued
 * messages, pending SendAfter timers) in bytes. */
size_t actor_memory_bytes(const Actor *a);

/* ---- reading terms (interp.c, port.c) ---- */

/* obj_of, field_term, name_bytes and int_val are defined once, in heap.c,
 * not inline: at -Os every file that used an inline copy got its own
 * out-of-line one, and the firmware link relaxes each call to a 3-byte
 * call8, so one shared copy is smaller. struct_type stays inline (three
 * instructions). */

/* Pointer to the object a heap or const ref names, or NULL if t is neither
 * or points outside the heap / const area. A const ref resolves through the
 * module table (const_obj_of, module.h). */
const uint32_t *obj_of(const Vm *vm, const Actor *a, term t);

/* A term word read out of the object t names: a const ref inside a const
 * object is untagged in its image and gets the container's tag. */
term field_term(term t, term f);

/* The type word of the struct obj that t names, without its flags
 * (STRUCT_FLAGS): a const struct's word is untagged in its image; a module
 * type gets the module's tag. */
static inline uint32_t struct_type(term t, const uint32_t *obj) {
    uint32_t ty = obj[1] & ~STRUCT_FLAGS;
    return (term_is_const(t) && ty >= TYPE_OWN_BASE) ? ty | (t & MOD_TAG_MASK) : ty;
}

/* The bytes of a name argument: a string constant, a heap string or the
 * empty string. False for anything else. */
bool name_bytes(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len);

/* Value of a small int or boxed K_INT; false for anything else. */
bool int_val(const Vm *vm, const Actor *a, term t, int32_t *out);

/* A new heap box [hdr kind][lo] (words 2) or [hdr kind][lo][hi] (words
 * 3): a K_INT, K_INT64 or K_FLOAT holding v, into *out. May GC; false on
 * OOM. */
bool heap_box(Vm *vm, Actor *a, uint32_t kind, uint32_t words, uint64_t v, term *out);

#endif
