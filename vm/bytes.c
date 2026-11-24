/* Off-heap Bytes handles: table, holder lists, charging, C API. See
 * bytes.h for the model. */
#include "bytes.h"
#include "alloc.h"
#include "heap.h"
#include "signals.h"
#include "config.h"
#include <string.h>

#define NO_PARENT 0xFFFFu

/* Table indices fit Held.idx and Entry.parent (NO_PARENT excluded). */
typedef char bytes_config_ok[(GOGO_BYTES_HANDLES >= 1u && GOGO_BYTES_HANDLES <= 65535u &&
                              GOGO_BYTES_PER_ACTOR >= 1u) ? 1 : -1];

typedef struct Entry {
    uint32_t refcnt;          /* 0: free */
    uint32_t off;             /* into the root's block (0 for a root) */
    uint32_t len;
    uint16_t parent;          /* root index of a slice, NO_PARENT for a root */
    uint8_t *data;            /* the root's block (a slice shares it) */
} Entry;

#define TAB_BYTES ((size_t)GOGO_BYTES_HANDLES * sizeof(Entry))

/* Words an actor's holder list is charged while it exists. */
#define LIST_WORDS ((uint32_t)((sizeof(HeldBytes) + 3u) / 4u))

static Entry *tab;            /* GOGO_BYTES_HANDLES entries, NULL until the first handle */
static uint32_t tab_used;     /* live entries */

void bytes_reset(void) {
    tab = NULL;
    tab_used = 0;
}

uint32_t bytes_table_used(void) { return tab_used; }

/* Block size for len bytes (the allocator takes no zero-size blocks). */
static size_t block_bytes(uint32_t len) { return len ? (size_t)len : 1u; }

bool bytes_valid(term h) {
    if (!term_is_handle(h) || !tab) return false;
    uint32_t i = term_handle_idx(h);
    return i < GOGO_BYTES_HANDLES && tab[i].refcnt != 0;
}

static uint32_t root_of(uint32_t i) { return tab[i].parent == NO_PARENT ? i : tab[i].parent; }

/* A free slot (allocating the table on first use), or -1. The table is
 * never freed once made (until vm_init resets it): it is a fixed
 * TAB_BYTES, and keeping it avoids free/alloc churn on the arena when a
 * program makes and drops handles in a loop. */
static int32_t slot_find(void) {
    if (!tab) {
        tab = va_alloc(TAB_BYTES);
        if (!tab) return -1;
        memset(tab, 0, TAB_BYTES);
    }
    if (tab_used >= GOGO_BYTES_HANDLES) return -1;
    for (uint32_t i = 0; i < GOGO_BYTES_HANDLES; i++) {
        if (tab[i].refcnt == 0) return (int32_t)i;
    }
    return -1;
}

/* Drops one reference to entry i; the last one frees the entry (and the
 * block, for a root) and drops the slice's reference on its root. */
static void entry_unref(uint32_t i) {
    for (;;) {
        Entry *e = &tab[i];
        if (--e->refcnt) return;
        uint32_t parent = e->parent;
        if (parent == NO_PARENT) va_free(e->data, block_bytes(e->len));
        memset(e, 0, sizeof *e);
        tab_used--;
        if (parent == NO_PARENT) break;
        i = parent;
    }
}

/* ---- distinct-root sets ---- */

void bytes_set_init(BytesSet *s) { memset(s, 0, sizeof *s); }

void bytes_set_add(BytesSet *s, term h) {
    if (!bytes_valid(h)) return;
    uint32_t r = root_of(term_handle_idx(h));
    uint32_t bit = 1u << (r % 32u);
    if (s->seen[r / 32u] & bit) return;
    s->seen[r / 32u] |= bit;
    s->bytes += tab[r].len;
}

/* ---- holder lists ---- */

/* a's list, or NULL while it holds nothing. */
static HeldBytes *list_of(const Actor *a) {
    HeldBytes **slot = actor_held((Actor *)a, false);
    return slot ? *slot : NULL;
}

static void held_recount(HeldBytes *hb) {
    BytesSet s;
    bytes_set_init(&s);
    for (uint32_t k = 0; k < hb->n; k++) bytes_set_add(&s, term_from_handle(hb->e[k].idx));
    hb->bytes = s.bytes;
}

static int32_t held_find(const HeldBytes *hb, uint32_t i) {
    for (uint32_t k = 0; hb && k < hb->n; k++) {
        if (hb->e[k].idx == i) return (int32_t)k;
    }
    return -1;
}

/* Removes entry k of a's list *slot (its reference goes), freeing the list
 * (and its charge) when it empties, and recounts unless told not to (a
 * sweep recounts once at the end). */
static void held_remove(Actor *a, HeldBytes **slot, uint32_t k, bool recount) {
    HeldBytes *hb = *slot;
    uint32_t i = hb->e[k].idx;
    hb->e[k] = hb->e[hb->n - 1u];
    hb->n--;
    if (hb->n == 0) {
        va_free(hb, sizeof *hb);
        *slot = NULL;
        actor_uncharge(a, LIST_WORDS);
    } else if (recount) {
        entry_unref(i);
        held_recount(hb);
        return;
    }
    entry_unref(i);
}

/* Adds entry i to a's list with the given pin / heap reachability, or
 * updates a's existing entry for it. A new list entry takes a reference.
 * False (nothing changed) if the list is full or memory runs out. */
static bool held_add(Actor *a, uint32_t i, bool pin, bool heap) {
    HeldBytes **slot = actor_held(a, true);
    if (!slot) return false;
    HeldBytes *hb = *slot;
    int32_t k = held_find(hb, i);
    if (k >= 0) {
        if (pin) {
            if (hb->e[k].pins == UINT8_MAX) return false;
            hb->e[k].pins++;
        }
        if (heap) hb->e[k].flags |= HELD_HEAP;
        return true;
    }
    if (!hb) {
        hb = va_alloc(sizeof *hb);
        if (!hb) return false;
        if (!actor_charge(a, LIST_WORDS)) {   /* the list counts against a */
            va_free(hb, sizeof *hb);
            return false;
        }
        hb->n = 0;
        hb->bytes = 0;
        *slot = hb;
    }
    if (hb->n >= GOGO_BYTES_PER_ACTOR) return false;
    hb->e[hb->n].idx = (uint16_t)i;
    hb->e[hb->n].pins = pin ? 1u : 0u;
    hb->e[hb->n].flags = heap ? HELD_HEAP : 0u;
    hb->n++;
    tab[i].refcnt++;
    held_recount(hb);
    return true;
}

/* True if a may take one more list entry whose root is r (r is charged
 * unless a already holds something of that root; the list itself is
 * charged when it is made), within its limit. */
static bool room_for(const Actor *a, uint32_t r, uint32_t root_len) {
    const HeldBytes *hb = list_of(a);
    uint32_t n = hb ? hb->n : 0u, held = hb ? hb->bytes : 0u;
    if (n >= GOGO_BYTES_PER_ACTOR) return false;
    for (uint32_t k = 0; k < n; k++) {
        if (root_of(hb->e[k].idx) == r) return true;   /* no new charge */
    }
    uint64_t words = (uint64_t)a->mem_words + actor_charged_words(a) - bytes_words(held) +
                     bytes_words(held + root_len < held ? UINT32_MAX : held + root_len) +
                     (hb ? 0u : LIST_WORDS);
    return words <= actor_max_words(a);
}

uint32_t bytes_held_bytes(const Actor *a) {
    const HeldBytes *hb = list_of(a);
    return hb ? hb->bytes : 0u;
}

uint32_t bytes_list_words(const Actor *a) { return list_of(a) ? 0u : LIST_WORDS; }

uint32_t bytes_held_count(const Actor *a) {
    const HeldBytes *hb = list_of(a);
    return hb ? hb->n : 0u;
}

/* ---- making entries ---- */

/* One attempt at a new root of len bytes held by a. */
static term try_new(Actor *a, uint32_t len, bool pin, uint8_t **data) {
    if (!room_for(a, NO_PARENT, len)) return T_NIL;
    int32_t s = slot_find();
    if (s < 0) return T_NIL;
    uint8_t *d = va_alloc(block_bytes(len));
    if (!d) return T_NIL;
    Entry *e = &tab[s];
    e->refcnt = 0;
    e->off = 0;
    e->len = len;
    e->parent = NO_PARENT;
    e->data = d;
    tab_used++;
    e->refcnt = 1;                        /* while held_add runs */
    bool ok = held_add(a, (uint32_t)s, pin, !pin);
    entry_unref((uint32_t)s);             /* the list's reference stays, if any */
    if (!ok) return T_NIL;
    *data = d;
    return term_from_handle((uint32_t)s);
}

/* One attempt at the slice [lo, lo + len) of the live handle h. */
static term try_sub(Actor *a, term h, uint32_t lo, uint32_t len) {
    uint32_t i = term_handle_idx(h), r = root_of(i);
    if (!room_for(a, r, tab[r].len)) return T_NIL;
    int32_t s = slot_find();
    if (s < 0) return T_NIL;              /* the table is live: h is in it */
    Entry *e = &tab[s];
    e->off = tab[i].off + lo;
    e->len = len;
    e->parent = (uint16_t)r;
    e->data = tab[r].data;
    e->refcnt = 1;                        /* while held_add runs */
    tab_used++;
    tab[r].refcnt++;
    bool ok = held_add(a, (uint32_t)s, false, true);
    entry_unref((uint32_t)s);
    return ok ? term_from_handle((uint32_t)s) : T_NIL;
}

/* The table is full: collects every other actor that holds handles, so
 * entries only an idle actor's stale registers or heap still name (an
 * actor blocked in Receive never collects; a received handle stays on its
 * list until its next GC) go back to the table. The caller a was just
 * collected by its own retry and is skipped. Safe because no other actor
 * is executing: a parked actor (waiting or runnable) keeps every live term
 * in its own block (registers are stack words; the interpreter re-derives
 * its register pointer from a->mem and a->fp when it resumes), and the
 * collection keeps the accounting exact (the sweep uncharges what it
 * drops). */
static void gc_holders(Vm *vm, Actor *a) {
    for (uint32_t i = 0; i < vm->table_cap; i++) {
        uintptr_t slot = vm->table[i];
        if (slot & 1u) continue;          /* a free slot */
        Actor *o = (Actor *)slot;
        if (o == a || o->state == A_EXITED || !list_of(o)) continue;
        heap_gc(vm, o, 0, NULL, 0);
    }
}

static bool tab_full(void) { return tab && tab_used >= GOGO_BYTES_HANDLES; }

term bytes_make(Vm *vm, Actor *a, uint32_t len, uint8_t **data) {
    term h = try_new(a, len, false, data);
    if (h == T_NIL) {
        heap_gc(vm, a, 0, NULL, 0);       /* drops what a no longer reaches */
        h = try_new(a, len, false, data);
    }
    if (h == T_NIL && tab_full()) {
        gc_holders(vm, a);
        h = try_new(a, len, false, data);
    }
    return h;
}

term bytes_sub(Vm *vm, Actor *a, term h, uint32_t lo, uint32_t len) {
    if (!bytes_valid(h)) return T_NIL;
    term s = try_sub(a, h, lo, len);
    if (s == T_NIL) {
        heap_gc(vm, a, 0, NULL, 0);       /* h is in a register: it stays */
        if (!bytes_valid(h)) return T_NIL;
        s = try_sub(a, h, lo, len);
    }
    if (s == T_NIL && tab_full()) {
        gc_holders(vm, a);                /* a holds h: it stays */
        s = try_sub(a, h, lo, len);
    }
    return s;
}

/* ---- C API ---- */

term bytes_new(Vm *vm, Actor *owner, uint32_t len, uint8_t **data) {
    (void)vm;
    uint8_t *d;
    term h = try_new(owner, len, true, &d);
    if (h == T_NIL) return T_NIL;
    memset(d, 0, len);
    *data = d;
    return h;
}

bool bytes_retain(Vm *vm, Actor *holder, term h) {
    (void)vm;
    return bytes_valid(h) && held_add(holder, term_handle_idx(h), true, false);
}

void bytes_release(Vm *vm, Actor *holder, term h) {
    (void)vm;
    HeldBytes **slot = actor_held(holder, false);
    HeldBytes *hb = slot ? *slot : NULL;
    int32_t k = bytes_valid(h) ? held_find(hb, term_handle_idx(h)) : -1;
    if (k < 0 || hb->e[k].pins == 0) return;
    hb->e[k].pins--;
    if (hb->e[k].pins == 0 && !(hb->e[k].flags & HELD_HEAP)) held_remove(holder, slot, (uint32_t)k, true);
}

const uint8_t *bytes_data(term h, uint32_t *len) {
    if (!bytes_valid(h)) return NULL;
    const Entry *e = &tab[term_handle_idx(h)];
    *len = e->len;
    return e->data + e->off;
}

bool bytes_view(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
    if (t == T_EMPTYBYTES) {
        *p = NULL;
        *len = 0;
        return true;
    }
    if (term_is_handle(t)) {
        *p = bytes_data(t, len);
        return *p != NULL;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_BYTES || o[1] > (hdr_words(o[0]) - 2u) * 4u) return false;
    *p = (const uint8_t *)(o + 2);
    *len = o[1];
    return true;
}

void bytes_actor_exit(Vm *vm, Actor *a) {
    (void)vm;
    HeldBytes **slot = actor_held(a, false);
    while (slot && *slot) held_remove(a, slot, (*slot)->n - 1u, false);
}

/* ---- GC ---- */

HeldBytes **bytes_gc_begin(Actor *a) {
    HeldBytes **slot = actor_held(a, false);
    if (!slot || !*slot) return NULL;
    HeldBytes *hb = *slot;
    for (uint32_t k = 0; k < hb->n; k++) hb->e[k].flags &= (uint8_t)~HELD_MARK;
    return slot;
}

void bytes_gc_mark(HeldBytes **slot, term h) {
    HeldBytes *hb = *slot;
    int32_t k = held_find(hb, term_handle_idx(h));
    if (k >= 0) hb->e[k].flags |= HELD_MARK;
}

void bytes_gc_sweep(Actor *a, HeldBytes **slot) {
    bool changed = false;
    for (uint32_t k = (*slot)->n; k-- > 0; ) {
        Held *e = &(*slot)->e[k];
        e->flags = (e->flags & HELD_MARK) ? HELD_HEAP : 0u;
        if (!e->flags && !e->pins) {
            held_remove(a, slot, k, false);
            changed = true;
            if (!*slot) return;          /* emptied and freed */
        }
    }
    if (changed) held_recount(*slot);
}

/* ---- messages ---- */

void bytes_msg_ref(term h) {
    if (bytes_valid(h)) tab[term_handle_idx(h)].refcnt++;
}

void bytes_msg_unref(term h) {
    if (bytes_valid(h)) entry_unref(term_handle_idx(h));
}

bool bytes_adopt(Actor *a, term h) {
    return bytes_valid(h) && held_add(a, term_handle_idx(h), false, true);
}
