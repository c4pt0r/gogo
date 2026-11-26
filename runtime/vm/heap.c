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
 *   K_FLOAT    [hdr][bits] / [hdr][lo][hi]  raw, not scanned (float32 / float64)
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

/* One pass from a's block into a new block of new_words, which must hold
 * hp + stack words. With `trace`, a Cheney copy (an upper bound of the live
 * data): the heap is compacted to the bottom. Without, the block is already
 * compacted (everything in [0, hp) is live) and is copied as it is: a
 * resize. Either way the stack moves to the top; saved fps are
 * top-relative, so only sp and fp shift. The old block is freed after the
 * copy, so at most two blocks exist at once. False (nothing changed) if the
 * new block cannot be allocated. */
static bool copy_into(Vm *vm, Actor *a, uint32_t new_words, term *roots, int n_roots, bool trace) {
    uint32_t old_words = a->mem_words;
    uint32_t stack_words = old_words - a->sp;
    uint32_t new_sp = new_words - stack_words;
    Copier c;
    c.from = a->mem;
    c.to = va_alloc(vm, (size_t)new_words * 4u);
    c.free = a->hp;
    if (!c.to) return false;

    memcpy(c.to + new_sp, c.from + a->sp, (size_t)stack_words * 4u);
    if (trace) {
        c.free = 0;
        c.held = bytes_gc_begin(a);
        /* 1. Roots: the stack (forwarded in its new place) and the caller's. */
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

        if (c.held) bytes_gc_sweep(vm, a, c.held);   /* what nothing reached goes */
    } else {
        memcpy(c.to, c.from, (size_t)c.free * 4u);   /* compacted: everything is live */
    }
    va_free(vm, a->mem, (size_t)old_words * 4u);
    a->mem = c.to;
    a->mem_words = (uint16_t)new_words;
    a->hp = (uint16_t)c.free;
    a->sp = (uint16_t)new_sp;
    a->fp = (uint16_t)((uint32_t)a->fp + new_words - old_words);
    return true;
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
        if (copy_into(vm, a, target, roots, n_roots, true)) return;
    }
    if (!copy_into(vm, a, old_words, roots, n_roots, true)) return;  /* no memory to collect with */

    uint32_t live = (uint32_t)a->hp + stack_words;
    uint32_t new_words = policy_words(old_words, live, need_words, cap);
    if (new_words != old_words) copy_into(vm, a, new_words, NULL, 0, false);
}

void heap_gc_tight(Vm *vm, Actor *a, uint32_t need_words, term *roots, int n_roots) {
    (void)vm;
    uint32_t old_words = a->mem_words;
    if (!copy_into(vm, a, old_words, roots, n_roots, true)) return;  /* no memory to collect with */
    uint32_t live = (uint32_t)a->hp + (old_words - a->sp);
    uint32_t cap = block_cap(a);                 /* after the sweep released Bytes */
    uint32_t want = live + need_words;           /* <= 2 * 65535, no overflow */
    if (need_words && want > cap) return;        /* cannot fit: keep the compacted block */
    if (want < MIN_BLOCK_WORDS && MIN_BLOCK_WORDS <= old_words) want = MIN_BLOCK_WORDS;
    if (want != old_words) copy_into(vm, a, want, NULL, 0, false);
}

bool heap_reclaim_waiting(Vm *vm, Actor *a) {
    if (a->state != A_WAITING) return false;
    uint32_t before = (uint32_t)a->mem_words + actor_charged_words(a);
    heap_gc_tight(vm, a, 0, NULL, 0);
    return (uint32_t)a->mem_words + actor_charged_words(a) < before;
}

/* Room for `words` free words: a normal GC (sized with growth slack), and
 * if that fails against the limit, a tight one (a request near the limit
 * fits once the slack is gone). */
static bool make_room(Vm *vm, Actor *a, uint32_t words, term *roots, int n_roots) {
    GOGO_ASSERT(a->hp <= a->sp && a->sp <= a->mem_words && a->fp >= a->sp);
    if ((uint32_t)(a->sp - a->hp) >= words) return true;
    heap_gc(vm, a, words, roots, n_roots);
    if ((uint32_t)(a->sp - a->hp) >= words) return true;
    heap_gc_tight(vm, a, words, roots, n_roots);
    GOGO_ASSERT(a->hp <= a->sp && a->sp <= a->mem_words);
    return (uint32_t)(a->sp - a->hp) >= words;
}

uint32_t heap_alloc(Vm *vm, Actor *a, uint32_t words, term *roots, int n_roots) {
    if (!make_room(vm, a, words, roots, n_roots)) return UINT32_MAX;
    uint32_t off = a->hp;
    a->hp = (uint16_t)(off + words);
    return off;
}

bool heap_push_frame(Vm *vm, Actor *a, uint16_t n_regs, int32_t ret_pc) {
    uint32_t size = (uint32_t)n_regs + 2u;
    if (!make_room(vm, a, size, NULL, 0)) return false;
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

bool heap_box(Vm *vm, Actor *a, uint32_t kind, uint32_t words, uint64_t v, term *out) {
    uint32_t off = heap_alloc(vm, a, words, NULL, 0);
    if (off == UINT32_MAX) return false;
    a->mem[off] = hdr_make(words, kind);
    a->mem[off + 1] = (uint32_t)v;
    if (words == 3u) a->mem[off + 2] = (uint32_t)(v >> 32);
    *out = term_from_ref(off);
    return true;
}

/* ---- reading terms (heap.h) ---- */

const uint32_t *obj_of(const Vm *vm, const Actor *a, term t) {
    if (term_is_const(t)) return const_obj_of(vm, t);
    if (!term_is_ref(t)) return NULL;
    uint32_t off = term_ref_off(t), limit = a->hp;
    /* Every object has at least two words and lies wholly inside the heap. */
    if (off >= limit || limit - off < 2u) return NULL;
    uint32_t words = hdr_words(a->mem[off]);
    if (words < 2u || words > limit - off) return NULL;
    return a->mem + off;
}

term field_term(term t, term f) {
    return (term_is_const(t) && term_is_const(f)) ? f | (t & MOD_TAG_MASK) : f;
}

bool name_bytes(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
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

bool int_val(const Vm *vm, const Actor *a, term t, int32_t *out) {
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
