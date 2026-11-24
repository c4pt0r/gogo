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
 * if they cannot fit even after GC within HEAP_MAX_WORDS. May GC, which
 * moves objects: refs held outside the stack must be passed in `roots`
 * (updated in place). The caller must fill in the object before the next
 * allocation. */
uint32_t heap_alloc(Vm *vm, Actor *a, uint32_t words, term *roots, int n_roots);

/* Copies everything reachable from the stack and `roots` into a new block
 * sized so that `need_words` more words fit if possible (see heap.c). */
void heap_gc(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots);

/* Pushes a frame of n_regs registers (all T_NIL) plus the saved caller fp
 * and ret_pc (-1 for an entry frame). May GC. False if out of memory. */
bool heap_push_frame(Vm *vm, Actor *a, uint16_t n_regs, int32_t ret_pc);

/* Which words of the heap object at `obj` hold terms: [*first, *end) as
 * indexes into the object (empty range for K_STRING and K_INT, whose
 * contents are raw, and for the raw type_id / func_idx word). Shared by the
 * GC and by message copy/relocation so they always agree. */
void heap_obj_terms(const uint32_t *obj, uint32_t *first, uint32_t *end);

/* sizeof(Actor) + its table slot + the block + queued message bytes. */
size_t actor_memory_bytes(const Actor *a);

#endif
