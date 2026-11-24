/* Per-actor heap and Cheney copying GC.
 *
 * Block layout: heap objects in [0, hp), free words in [hp, sp), stack
 * frames in [sp, mem_words). Every stack word is a valid term.
 *
 * GC roots are the stack words and the caller's `roots`. Only heap refs
 * (low bits 00) are followed; small ints, specials, PIDs and const refs are
 * left alone. Inside objects, which words are terms depends on the kind:
 *   K_STRUCT   [hdr][type_id raw][fields: terms]
 *   K_CLOSURE  [hdr][func_idx raw][captures: terms]
 *   K_STRING   [hdr][byte_len][bytes]   all raw, not scanned
 *   K_INT      [hdr][int32]             raw, not scanned
 *   K_REF      [hdr][serial lo][hi]     raw, not scanned
 *   K_INT64    [hdr][lo][hi]            raw, not scanned
 *   K_SLICE    [hdr][off][len][cap][array term]
 *   K_ARRAY    [hdr][n raw][elements: terms]
 *   K_BYTEARR  [hdr][nbytes][bytes]     raw, not scanned
 *   K_BYTES    [hdr][nbytes][bytes]     raw, not scanned
 *   K_BUF      [hdr][len][array term]
 * Word 1 is never a term (message copy borrows it, see msg.c), and a
 * kind's term words always run to its last word (msg.c's walk resumes a
 * parent with end = its size). Every object has at least two words, so a forwarded object can hold
 * [hdr K_FORWARD][new offset].
 *
 * Off-heap Bytes handles (bytes.h) are immediates to the copy, but every
 * one the stack, the roots or a live object holds is marked on the actor's
 * holder list; after a successful collection the unmarked, unpinned ones
 * are released (bytes_gc_sweep). */
#include "heap.h"
#include "alloc.h"
#include "signals.h"
#include "bytes.h"
#include <string.h>

#define MIN_BLOCK_WORDS 24u

typedef struct Copier {
    uint32_t *from, *to;
    uint32_t free;            /* next free word in to-space */
    HeldBytes **held;         /* the actor's holder list, NULL if empty */
} Copier;

void heap_obj_terms(const uint32_t *obj, uint32_t *first, uint32_t *end) {
    uint32_t kind = hdr_kind(obj[0]);
    if (kind == K_STRUCT || kind == K_CLOSURE || kind == K_ARRAY) {
        *first = 2;
        *end = hdr_words(obj[0]);
    } else if (kind == K_SLICE) {
        *first = 4;
        *end = 5;
    } else if (kind == K_BUF) {
        *first = 2;
        *end = 3;
    } else {
        *first = *end = 0;
    }
}

/* Returns the to-space version of t, copying its object on first visit. */
static term forward(Copier *c, term t) {
    if (!term_is_ref(t)) {
        if (c->held && term_is_handle(t)) bytes_gc_mark(c->held, t);
        return t;
    }
    uint32_t off = term_ref_off(t);
    uint32_t h = c->from[off];
    if (hdr_kind(h) == K_FORWARD) return term_from_ref(c->from[off + 1]);

    uint32_t words = hdr_words(h);
    uint32_t dst = c->free;
    memcpy(c->to + dst, c->from + off, words * 4u);
    c->free += words;
    c->from[off] = hdr_make(words, K_FORWARD);
    c->from[off + 1] = dst;
    return term_from_ref(dst);
}

/* The largest block this actor may have: its limit minus the words its
 * queued messages and pending SendAfter timers take (heap block + charged
 * words <= limit). */
static uint32_t block_cap(const Actor *a) {
    uint32_t limit = actor_max_words(a);
    uint32_t charged = actor_charged_words(a);
    if (limit > HEAP_MAX_WORDS) limit = HEAP_MAX_WORDS;
    return limit > charged ? limit - charged : 0u;
}

/* Block size for `live` words (heap + stack) plus room for `need` more:
 * at least live*2 + need, at most cap; shrink (not below MIN_BLOCK_WORDS)
 * when live data fills under a quarter of old_words. A need that cannot fit
 * within cap is ignored, so the block is sized for the live data and the
 * caller reports out-of-memory. Never returns less than live. */
static uint32_t policy_words(uint32_t old_words, uint32_t live, uint32_t need,
                             uint32_t cap) {
    if (cap < live) cap = live;                  /* already over: keep live */
    if (need > cap - live) need = 0;
    uint32_t want = live * 2u + need;            /* <= 3 * 65535, no overflow */
    if (want > cap) want = cap;
    if (old_words < want) return want;           /* grow */
    if (live < old_words / 4u) {                 /* mostly empty: shrink */
        uint32_t w = want > MIN_BLOCK_WORDS ? want : MIN_BLOCK_WORDS;
        return w < old_words ? w : old_words;
    }
    return old_words;
}

/* One Cheney pass from a's block into a new block of new_words, which must
 * hold hp + stack words (an upper bound of the live data). The heap is
 * compacted to the bottom, the stack moves to the top; saved fps are
 * top-relative, so only sp and fp shift. The old block is freed after the
 * copy, so at most two blocks exist at once. False (nothing changed) if the
 * new block cannot be allocated. */
static bool copy_into(Actor *a, uint32_t new_words, term *roots, int n_roots) {
    uint32_t old_words = a->mem_words;
    uint32_t stack_words = old_words - a->sp;
    uint32_t new_sp = new_words - stack_words;
    Copier c;
    c.from = a->mem;
    c.to = va_alloc((size_t)new_words * 4u);
    c.free = 0;
    c.held = bytes_gc_begin(a);
    if (!c.to) return false;

    /* 1. Roots: the stack (forwarded in its new place) and the caller's. */
    memcpy(c.to + new_sp, c.from + a->sp, (size_t)stack_words * 4u);
    for (uint32_t i = new_sp; i < new_words; i++) c.to[i] = forward(&c, c.to[i]);
    for (int i = 0; i < n_roots; i++) roots[i] = forward(&c, roots[i]);

    /* 2. Cheney scan: forward the term words of each copied object. */
    for (uint32_t scan = 0; scan < c.free; ) {
        uint32_t h = c.to[scan];
        uint32_t first, end;
        heap_obj_terms(c.to + scan, &first, &end);
        for (uint32_t i = scan + first; i < scan + end; i++) c.to[i] = forward(&c, c.to[i]);
        scan += hdr_words(h);
    }

    if (c.held) bytes_gc_sweep(a, c.held);   /* what nothing reached goes */
    va_free(a->mem, (size_t)old_words * 4u);
    a->mem = c.to;
    a->mem_words = (uint16_t)new_words;
    a->hp = (uint16_t)c.free;
    a->sp = (uint16_t)new_sp;
    a->fp = (uint16_t)((uint32_t)a->fp + new_words - old_words);
    return true;
}

/* Copies the compacted heap [0, hp) and the stack into a block of new_words
 * (no tracing needed: everything in the block is live). */
static void resize_block(Actor *a, uint32_t new_words) {
    uint32_t old_words = a->mem_words;
    uint32_t stack_words = old_words - a->sp;
    uint32_t *mem = va_alloc((size_t)new_words * 4u);
    if (!mem) return;                            /* keep the compacted block */
    memcpy(mem, a->mem, (size_t)a->hp * 4u);
    memcpy(mem + new_words - stack_words, a->mem + a->sp, (size_t)stack_words * 4u);
    va_free(a->mem, (size_t)old_words * 4u);
    a->mem = mem;
    a->mem_words = (uint16_t)new_words;
    a->sp = (uint16_t)(new_words - stack_words);
    a->fp = (uint16_t)((uint32_t)a->fp + new_words - old_words);
}

/* Spec M2 §5.1.3: at most two blocks of this actor exist at once.
 *
 * used = hp + stack is an upper bound of the live data. When the final
 * size can be decided from it, one pass copies straight into the final
 * block: growth is certain when need cannot fit in the old block even if
 * the whole heap is garbage (stack + need > old), and a shrink is safe when
 * used < old / 4. Otherwise (the common case: the heap is full, maybe of
 * garbage) copy into a block of the same size, which always holds the live
 * data, and only then, knowing the exact live size, resize if the policy
 * says so. The resize allocates after the old block is freed, so this path
 * also never holds more than two blocks. */
void heap_gc(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots) {
    (void)vm;
    uint32_t old_words = a->mem_words;
    uint32_t stack_words = old_words - a->sp;
    uint32_t used = (uint32_t)a->hp + stack_words;
    uint32_t cap = block_cap(a);
    uint32_t target = policy_words(old_words, used, need_words, cap);

    bool grow_certain = target > old_words && stack_words + need_words > old_words &&
                        used <= cap && need_words <= cap - used;  /* target >= used + need */
    if (grow_certain || target < old_words) {
        if (copy_into(a, target, roots, n_roots)) return;
    }
    if (!copy_into(a, old_words, roots, n_roots)) return;  /* no memory to collect with */

    uint32_t live = (uint32_t)a->hp + stack_words;
    uint32_t new_words = policy_words(old_words, live, need_words, cap);
    if (new_words != old_words) resize_block(a, new_words);
}

uint32_t heap_alloc(Vm *vm, Actor *a, uint32_t words, term *roots, int n_roots) {
    if ((uint32_t)(a->sp - a->hp) < words) {
        heap_gc(vm, a, words, roots, n_roots);
        if ((uint32_t)(a->sp - a->hp) < words) return UINT32_MAX;
    }
    uint32_t off = a->hp;
    a->hp = (uint16_t)(off + words);
    return off;
}

bool heap_push_frame(Vm *vm, Actor *a, uint16_t n_regs, int32_t ret_pc) {
    uint32_t size = (uint32_t)n_regs + 2u;
    if ((uint32_t)(a->sp - a->hp) < size) {
        heap_gc(vm, a, size, NULL, 0);
        if ((uint32_t)(a->sp - a->hp) < size) return false;
    }
    /* Top-relative caller fp; 0 means "no caller" (entry frame). */
    int32_t saved_fp = a->sp == a->mem_words ? 0 : (int32_t)(a->mem_words - a->fp);
    uint32_t fp = a->sp - size;
    for (uint32_t i = 0; i < n_regs; i++) a->mem[fp + i] = T_NIL;
    a->mem[fp + n_regs] = term_from_int(saved_fp);
    a->mem[fp + n_regs + 1] = term_from_int(ret_pc);
    a->sp = a->fp = (uint16_t)fp;
    return true;
}

size_t actor_memory_bytes(const Actor *a) {
    /* Queued messages and pending SendAfter timers it created. */
    return sizeof(Actor) + sizeof(uintptr_t) + (size_t)a->mem_words * 4u +
           (size_t)actor_charged_words(a) * 4u;
}

term heap_new_ref(Vm *vm, Actor *a) {
    uint32_t off = heap_alloc(vm, a, 3, NULL, 0);
    if (off == UINT32_MAX) return T_NIL;
    a->mem[off] = hdr_make(3, K_REF);
    a->mem[off + 1] = (uint32_t)vm->next_ref;
    a->mem[off + 2] = (uint32_t)(vm->next_ref >> 32);
    vm->next_ref++;
    return term_from_ref(off);
}
