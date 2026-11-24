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
 * Every object has at least two words, so a forwarded object can hold
 * [hdr K_FORWARD][new offset]. */
#include "heap.h"
#include "alloc.h"
#include <string.h>

#define MIN_BLOCK_WORDS 24u

typedef struct Copier {
    uint32_t *from, *to;
    uint32_t free;            /* next free word in to-space */
} Copier;

void heap_obj_terms(const uint32_t *obj, uint32_t *first, uint32_t *end) {
    uint32_t kind = hdr_kind(obj[0]);
    if (kind == K_STRUCT || kind == K_CLOSURE) {
        *first = 2;
        *end = hdr_words(obj[0]);
    } else {
        *first = *end = 0;
    }
}

/* Returns the to-space version of t, copying its object on first visit. */
static term forward(Copier *c, term t) {
    if (!term_is_ref(t)) return t;
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

/* Block size after a GC that left `live` words (heap + stack) and must make
 * room for `need` more: at least live*2 + need, at most HEAP_MAX_WORDS;
 * shrink (not below MIN_BLOCK_WORDS) when live data fills under a quarter.
 * A need that cannot fit within HEAP_MAX_WORDS is ignored, so the block is
 * sized for the live data and the caller reports out-of-memory. */
static uint32_t policy_words(uint32_t old_words, uint32_t live, uint32_t need) {
    if (need > HEAP_MAX_WORDS - live) need = 0;  /* live <= HEAP_MAX_WORDS */
    uint32_t want = live * 2u + need;            /* <= 3 * 65535, no overflow */
    if (want > HEAP_MAX_WORDS) want = HEAP_MAX_WORDS;
    if (old_words < want) return want;           /* grow */
    if (live < old_words / 4u) {                 /* mostly empty: shrink */
        uint32_t w = want > MIN_BLOCK_WORDS ? want : MIN_BLOCK_WORDS;
        return w < old_words ? w : old_words;
    }
    return old_words;
}

/* Moves the compacted heap [0, hp) and the stack into a block of new_words
 * words. Saved fps are top-relative, so only sp and fp need shifting. */
static bool resize_block(Actor *a, uint32_t new_words) {
    uint32_t old_words = a->mem_words;
    uint32_t stack_words = old_words - a->sp;
    uint32_t *mem = va_alloc((size_t)new_words * 4u);
    if (!mem) return false;
    memcpy(mem, a->mem, (size_t)a->hp * 4u);
    memcpy(mem + new_words - stack_words, a->mem + a->sp, (size_t)stack_words * 4u);
    va_free(a->mem, (size_t)old_words * 4u);
    a->mem = mem;
    a->mem_words = (uint16_t)new_words;
    a->sp = (uint16_t)(new_words - stack_words);
    a->fp = (uint16_t)((uint32_t)a->fp + new_words - old_words);
    return true;
}

void heap_gc(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots) {
    (void)vm;
    uint32_t words = a->mem_words;

    /* 1. Copy into a to-space of the same size; live data always fits.
     *    The stack keeps its offsets, its terms are forwarded in place. */
    Copier c;
    c.from = a->mem;
    c.to = va_alloc((size_t)words * 4u);
    c.free = 0;
    if (!c.to) return;    /* no memory to collect with; caller sees no room */
    memcpy(c.to + a->sp, c.from + a->sp, (size_t)(words - a->sp) * 4u);
    for (uint32_t i = a->sp; i < words; i++) c.to[i] = forward(&c, c.to[i]);
    for (int i = 0; i < n_roots; i++) roots[i] = forward(&c, roots[i]);

    /* 2. Cheney scan: forward the term words of each copied object. */
    for (uint32_t scan = 0; scan < c.free; ) {
        uint32_t h = c.to[scan];
        uint32_t first, end;
        heap_obj_terms(c.to + scan, &first, &end);
        for (uint32_t i = scan + first; i < scan + end; i++) c.to[i] = forward(&c, c.to[i]);
        scan += hdr_words(h);
    }

    va_free(a->mem, (size_t)words * 4u);
    a->mem = c.to;
    a->hp = (uint16_t)c.free;

    /* 3. Resize per policy. If that fails, keep the compacted block. */
    uint32_t live = c.free + (words - a->sp);
    uint32_t new_words = policy_words(words, live, need_words);
    if (new_words != words) resize_block(a, new_words);
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
    /* Queued messages are counted by mbox_words (maintained from Task 5). */
    return sizeof(Actor) + sizeof(uintptr_t) + (size_t)a->mem_words * 4u +
           (size_t)a->mbox_words * 4u;
}
