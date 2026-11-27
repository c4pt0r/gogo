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
typedef char bytes_config_ok[(GOGO_BYTES_HANDLES >= 1u && GOGO_BYTES_HANDLES <= GOGO_BYTES_HANDLES_MAX &&
                              GOGO_BYTES_HANDLES_MAX <= 65535u && GOGO_BYTES_PER_ACTOR >= 1u) ? 1 : -1];

typedef struct BytesEntry {
    uint32_t refcnt;          /* 0: free */
    uint32_t off;             /* into the root's block (0 for a root) */
    uint32_t len;
    uint16_t parent;          /* root index of a slice, NO_PARENT for a root */
    uint8_t *data;            /* the root's block (a slice shares it) */
} Entry;

/* Both grow on demand, up to the VM's caps (vm.handles): the table from
 * TAB_FIRST entries (a board's whole default) by doubling, a holder list
 * from LIST_FIRST entries (what every list had before the caps were
 * parameters, so a holder's charge does not depend on them) likewise. */
#define TAB_FIRST 64u
#define LIST_FIRST 16u
#define TAB_BYTES(n) ((size_t)(n) * sizeof(Entry))

/* A holder list of room entries: its bytes, and the words it is charged
 * while it exists. */
#define LIST_BYTES(room) (sizeof(HeldBytes) + (size_t)(room) * sizeof(Held))
#define LIST_WORDS(room) ((uint32_t)((LIST_BYTES(room) + 3u) / 4u))

/* The table is vm->bytes_tab (vm->bytes_alloc entries, NULL until the
 * first handle; at most vm->bytes_cap) with vm->bytes_used live entries. */

void bytes_reset(Vm *vm) {
    vm->bytes_tab = NULL;
    vm->bytes_used = 0;
    vm->bytes_alloc = 0;
}

uint32_t bytes_table_used(const Vm *vm) { return vm->bytes_used; }

/* Block size for len bytes (the allocator takes no zero-size blocks). */
static size_t block_bytes(uint32_t len) { return len ? (size_t)len : 1u; }

bool bytes_valid(const Vm *vm, term h) {
    if (!term_is_handle(h) || !vm->bytes_tab) return false;
    uint32_t i = term_handle_idx(h);
    return i < vm->bytes_alloc && vm->bytes_tab[i].refcnt != 0;
}

static uint32_t root_of(const Vm *vm, uint32_t i) { return vm->bytes_tab[i].parent == NO_PARENT ? i : vm->bytes_tab[i].parent; }

uint32_t bytes_pinned_len(const Vm *vm, term h) {
    return bytes_valid(vm, h) ? vm->bytes_tab[root_of(vm, term_handle_idx(h))].len : 0u;
}

/* A free slot, or -1 at the cap or out of memory. A full table (none
 * yet: 0 of 0) doubles, from TAB_FIRST entries, up to vm->bytes_cap; it
 * is never shrunk or freed (until vm_init resets it), which avoids
 * free/alloc churn on the arena when a program makes and drops handles in
 * a loop. Only indices stay valid across it: callers hold no Entry
 * pointer over a slot_find. */
/* A copy of the block p (old bytes; NULL: none) in a new block of n bytes,
 * the rest zeroed; p is freed. NULL (p kept) if out of memory. */
static void *grow(Vm *vm, void *p, size_t old, size_t n) {
    uint8_t *q = va_alloc(vm, n);
    if (!q) return NULL;
    memset(q + old, 0, n - old);
    if (p) {
        memcpy(q, p, old);
        va_free(vm, p, old);
    }
    return q;
}

static int32_t slot_find(Vm *vm) {
    uint32_t have = vm->bytes_alloc;
    if (vm->bytes_used >= vm->bytes_cap) return -1;
    if (vm->bytes_used == have) {
        uint32_t n = have ? have * 2u : TAB_FIRST;
        if (n > vm->bytes_cap) n = vm->bytes_cap;
        Entry *t = grow(vm, vm->bytes_tab, TAB_BYTES(have), TAB_BYTES(n));
        if (!t) return -1;
        vm->bytes_tab = t;
        vm->bytes_alloc = (uint16_t)n;
        return (int32_t)have;                  /* the first new one */
    }
    for (uint32_t i = 0; i < have; i++) {
        if (vm->bytes_tab[i].refcnt == 0) return (int32_t)i;
    }
    return -1;
}

/* Drops one reference to entry i; the last one frees the entry (and the
 * block, for a root) and drops the slice's reference on its root. */
static void entry_unref(Vm *vm, uint32_t i) {
    for (;;) {
        Entry *e = &vm->bytes_tab[i];
        if (--e->refcnt) return;
        uint32_t parent = e->parent;
        if (parent == NO_PARENT) va_free(vm, e->data, block_bytes(e->len));
        memset(e, 0, sizeof *e);
        vm->bytes_used--;
        if (parent == NO_PARENT) break;
        i = parent;
    }
}

/* ---- distinct-root sets ---- */

void bytes_set_init(const Vm *vm, BytesSet *s) {
    memset(s->seen, 0, (vm->bytes_alloc + 31u) / 32u * 4u);
    s->bytes = 0;
}

void bytes_set_add(const Vm *vm, BytesSet *s, term h) {
    if (!bytes_valid(vm, h)) return;
    uint32_t r = root_of(vm, term_handle_idx(h));
    uint32_t bit = 1u << (r % 32u);
    if (s->seen[r / 32u] & bit) return;
    s->seen[r / 32u] |= bit;
    s->bytes += vm->bytes_tab[r].len;
}

/* ---- holder lists ---- */

/* a's list, or NULL while it holds nothing. */
static HeldBytes *list_of(const Actor *a) {
    HeldBytes **slot = actor_held((Actor *)a);
    return slot ? *slot : NULL;
}

static void held_recount(const Vm *vm, HeldBytes *hb) {
    BytesSet s;
    bytes_set_init(vm, &s);
    for (uint32_t k = 0; k < hb->n; k++) bytes_set_add(vm, &s, term_from_handle(hb->e[k].idx));
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
static void held_remove(Vm *vm, Actor *a, HeldBytes **slot, uint32_t k, bool recount) {
    HeldBytes *hb = *slot;
    uint32_t i = hb->e[k].idx;
    hb->e[k] = hb->e[hb->n - 1u];
    hb->n--;
    if (hb->n == 0) {
        uint32_t room = hb->room;
        va_free(vm, hb, LIST_BYTES(room));
        *slot = NULL;
        actor_uncharge(a, LIST_WORDS(room));
    } else if (recount) {
        entry_unref(vm, i);
        held_recount(vm, hb);
        return;
    }
    entry_unref(vm, i);
}

/* The room a's list hb (NULL: none yet) has after one more entry. */
static uint32_t list_room(const Vm *vm, const HeldBytes *hb) {
    uint32_t r = !hb ? LIST_FIRST : hb->n < hb->room ? hb->room : hb->room * 2u;
    return r < vm->bytes_per_actor ? r : vm->bytes_per_actor;
}

/* Words one more entry would add to a's charge for its list: a first
 * list's, a grown list's more, else 0. */
static uint32_t list_more_words(const Vm *vm, const HeldBytes *hb) {
    return LIST_WORDS(list_room(vm, hb)) - (hb ? LIST_WORDS(hb->room) : 0u);
}

/* Adds entry i to a's list with the given pin / heap reachability, or
 * updates a's existing entry for it. A new list entry takes a reference.
 * False (nothing changed) if the list is full or memory runs out. */
static bool held_add(Vm *vm, Actor *a, uint32_t i, bool pin, bool heap) {
    HeldBytes **slot = actor_held_new(vm, a);
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
    if (hb && hb->n >= vm->bytes_per_actor) return false;
    uint32_t room = list_room(vm, hb), more = list_more_words(vm, hb);
    if (!hb || room != hb->room) {        /* a first list, or a full one doubled */
        if (!actor_charge(vm, a, more)) return false;   /* the list counts against a */
        HeldBytes *nb = grow(vm, hb, hb ? LIST_BYTES(hb->room) : 0u, LIST_BYTES(room));
        if (!nb) {
            actor_uncharge(a, more);
            return false;
        }
        nb->room = (uint16_t)room;
        *slot = hb = nb;
    }
    hb->e[hb->n].idx = (uint16_t)i;
    hb->e[hb->n].pins = pin ? 1u : 0u;
    hb->e[hb->n].flags = heap ? HELD_HEAP : 0u;
    hb->n++;
    vm->bytes_tab[i].refcnt++;
    held_recount(vm, hb);
    return true;
}

/* True if a may take one more list entry whose root is r (r is charged
 * unless a already holds something of that root; the list itself is
 * charged when it is made), within its limit. */
static bool room_for(const Vm *vm, const Actor *a, uint32_t r, uint32_t root_len) {
    const HeldBytes *hb = list_of(a);
    uint32_t n = hb ? hb->n : 0u, held = hb ? hb->bytes : 0u;
    if (n >= vm->bytes_per_actor) return false;
    for (uint32_t k = 0; k < n; k++) {
        if (root_of(vm, hb->e[k].idx) == r) return true;   /* no new charge */
    }
    uint64_t words = (uint64_t)a->mem_words + actor_charged_words(a) - bytes_words(held) +
                     bytes_words(held + root_len < held ? UINT32_MAX : held + root_len) +
                     list_more_words(vm, hb);
    return words <= actor_max_words(a);
}

uint32_t bytes_extra_words(const Vm *vm, const Actor *a, const term *hs, uint32_t n) {
    const HeldBytes *hb = list_of(a);
    uint32_t held = hb ? hb->bytes : 0u;
    uint64_t total = held;
    BytesSet s;
    bytes_set_init(vm, &s);
    for (uint32_t k = 0; hb && k < hb->n; k++) bytes_set_add(vm, &s, term_from_handle(hb->e[k].idx));
    bool any = false;
    for (uint32_t j = 0; j < n; j++) {
        if (!hs[j] || !bytes_valid(vm, hs[j])) continue;
        any = true;
        uint32_t before = s.bytes;
        bytes_set_add(vm, &s, hs[j]);
        total += s.bytes - before;        /* a root not seen yet: its full length */
    }
    if (!any) return 0u;
    uint64_t w = (total + 3u) / 4u - bytes_words(held) + list_more_words(vm, hb);
    return w > UINT32_MAX ? UINT32_MAX : (uint32_t)w;
}

uint32_t bytes_held_bytes(const Actor *a) {
    const HeldBytes *hb = list_of(a);
    return hb ? hb->bytes : 0u;
}

uint32_t bytes_list_words(const Vm *vm, const Actor *a) { return list_more_words(vm, list_of(a)); }

uint32_t bytes_held_count(const Actor *a) {
    const HeldBytes *hb = list_of(a);
    return hb ? hb->n : 0u;
}

/* ---- making entries ---- */

/* One attempt at a new entry held by a: a root of len bytes (h is T_NIL;
 * pinned if pin; *data its block), or the slice [lo, lo + len) of the
 * live handle h. */
static term try_entry(Vm *vm, Actor *a, term h, uint32_t lo, uint32_t len, bool pin, uint8_t **data) {
    uint32_t i = 0, r = NO_PARENT;
    uint8_t *d;
    if (h != T_NIL) {
        i = term_handle_idx(h);
        r = root_of(vm, i);
    }
    if (!room_for(vm, a, r, r == NO_PARENT ? len : vm->bytes_tab[r].len)) return T_NIL;
    int32_t s = slot_find(vm);
    if (s < 0) return T_NIL;
    if (r == NO_PARENT) {
        d = va_alloc(vm, block_bytes(len));
        if (!d) return T_NIL;
        lo = 0;
    } else {
        d = vm->bytes_tab[r].data;
        lo += vm->bytes_tab[i].off;
        vm->bytes_tab[r].refcnt++;
    }
    Entry *e = &vm->bytes_tab[s];
    e->off = lo;
    e->len = len;
    e->parent = (uint16_t)r;
    e->data = d;
    e->refcnt = 1;                        /* while held_add runs */
    vm->bytes_used++;
    bool ok = held_add(vm, a, (uint32_t)s, pin, !pin);
    entry_unref(vm, (uint32_t)s);             /* the list's reference stays, if any */
    if (!ok) return T_NIL;
    *data = d;
    return term_from_handle((uint32_t)s);
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

static bool tab_full(const Vm *vm) { return vm->bytes_tab && vm->bytes_used >= vm->bytes_cap; }   /* at the cap */

/* try_entry, collecting a's heap (it drops what a no longer reaches, and
 * the slack) and then, if the table is full, the other holders' before
 * trying again. h, when given, is in a register of a, which holds it: it
 * stays. */
static term make_entry(Vm *vm, Actor *a, term h, uint32_t lo, uint32_t len, uint8_t **data) {
    for (int k = 0; ; k++) {
        if (h != T_NIL && !bytes_valid(vm, h)) return T_NIL;
        term s = try_entry(vm, a, h, lo, len, false, data);
        if (s != T_NIL || k == 2) return s;
        if (k == 0) heap_gc_tight(vm, a, 0, NULL, 0);
        else if (tab_full(vm)) gc_holders(vm, a);
        else return s;
    }
}

term bytes_make(Vm *vm, Actor *a, uint32_t len, uint8_t **data) {
    return make_entry(vm, a, T_NIL, 0, len, data);
}

term bytes_sub(Vm *vm, Actor *a, term h, uint32_t lo, uint32_t len) {
    uint8_t *d;
    if (h == T_NIL) return T_NIL; /* as before make_entry: nil has no bytes to share */
    return make_entry(vm, a, h, lo, len, &d);
}

/* ---- C API ---- */

term bytes_new(Vm *vm, Actor *owner, uint32_t len, uint8_t **data) {
    uint8_t *d;
    term h = try_entry(vm, owner, T_NIL, 0, len, true, &d);
    if (h == T_NIL) return T_NIL;
    memset(d, 0, len);
    *data = d;
    return h;
}

bool bytes_retain(Vm *vm, Actor *holder, term h) {
    return bytes_valid(vm, h) && held_add(vm, holder, term_handle_idx(h), true, false);
}

void bytes_release(Vm *vm, Actor *holder, term h) {
    HeldBytes **slot = actor_held(holder);
    HeldBytes *hb = slot ? *slot : NULL;
    int32_t k = bytes_valid(vm, h) ? held_find(hb, term_handle_idx(h)) : -1;
    if (k < 0 || hb->e[k].pins == 0) return;
    hb->e[k].pins--;
    if (hb->e[k].pins == 0 && !(hb->e[k].flags & HELD_HEAP)) held_remove(vm, holder, slot, (uint32_t)k, true);
}

const uint8_t *bytes_data(const Vm *vm, term h, uint32_t *len) {
    if (!bytes_valid(vm, h)) return NULL;
    const Entry *e = &vm->bytes_tab[term_handle_idx(h)];
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
        *p = bytes_data(vm, t, len);
        return *p != NULL;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_BYTES || o[1] > (hdr_words(o[0]) - 2u) * 4u) return false;
    *p = (const uint8_t *)(o + 2);
    *len = o[1];
    return true;
}

void bytes_actor_exit(Vm *vm, Actor *a) {
    HeldBytes **slot = actor_held(a);
    while (slot && *slot) held_remove(vm, a, slot, (*slot)->n - 1u, false);
}

/* ---- GC ---- */

HeldBytes **bytes_gc_begin(Actor *a) {
    HeldBytes **slot = actor_held(a);
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

void bytes_gc_sweep(Vm *vm, Actor *a, HeldBytes **slot) {
    bool changed = false;
    for (uint32_t k = (*slot)->n; k-- > 0; ) {
        Held *e = &(*slot)->e[k];
        e->flags = (e->flags & HELD_MARK) ? HELD_HEAP : 0u;
        if (!e->flags && !e->pins) {
            held_remove(vm, a, slot, k, false);
            changed = true;
            if (!*slot) return;          /* emptied and freed */
        }
    }
    if (changed) held_recount(vm, *slot);
}

/* ---- messages ---- */

void bytes_msg_ref(Vm *vm, term h) {
    if (bytes_valid(vm, h)) vm->bytes_tab[term_handle_idx(h)].refcnt++;
}

void bytes_msg_unref(Vm *vm, term h) {
    if (bytes_valid(vm, h)) entry_unref(vm, term_handle_idx(h));
}

bool bytes_adopt(Vm *vm, Actor *a, term h) {
    return bytes_valid(vm, h) && held_add(vm, a, term_handle_idx(h), false, true);
}
