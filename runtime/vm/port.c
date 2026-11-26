#include "port.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"
#include "alloc.h"
#include "evq.h"
#include "host.h"
#include "config.h"
#include "bytes.h"
#include "../include/gogo_errors.h"
#include <string.h>

#if (GOGO_MAX_PORTS) < 1u || (GOGO_MAX_PORTS) > PORT_MAX_CEILING
#error "GOGO_MAX_PORTS must be 1..PORT_MAX_CEILING (port PIDs use the slots 0x10000 - max .. 0xFFFF)"
#endif

#define GEN_MASK 0xFFFu

#define SOURCES_EACH(vm, q) for (const PortSrcSlot *q = (vm)->sources; q->s; q++)

void port_set_drivers(Vm *vm, const PortDriver *const *list) { vm->drivers = list; }

bool port_add_source(Vm *vm, const PortSource *s, void *ctx) {
    PortSrcSlot *q = vm->sources;
    while (q->s) q++;
    if (q == vm->sources + GOGO_MAX_SOURCES) return false;
    q->s = s;
    q->ctx = ctx;
    return true;
}

/* Order is not kept: the last source takes the freed slot. */
void port_remove_source(Vm *vm, const PortSource *s) {
    PortSrcSlot *q = vm->sources, *l;
    while (q->s && q->s != s) q++;
    if (!q->s) return;
    for (l = q; l[1].s; l++) {}
    *q = *l;
    l->s = NULL;
}

bool port_subscribe(Vm *vm, Actor *a) {
    SOURCES_EACH(vm, q) {
        if (q->s->subscribe) return q->s->subscribe(vm, q->ctx, a);
    }
    return false;
}

bool port_is(const Vm *vm, term pid) {
    return term_is_pid(pid) && term_pid_slot(pid) >= vm->port_base;
}

int vm_set_max_ports(Vm *vm, uint32_t n) {
    if (!n) n = GOGO_MAX_PORTS;
    if (n > PORT_MAX_CEILING || vm->port_n || vm->table_cap > PORT_SLOT_TOP - n) return -1;
    vm->port_max = (uint16_t)n;
    vm->port_base = (uint16_t)(PORT_SLOT_TOP - n);
    return 0;
}

/* The open port pid names, or NULL (not a port PID, free or stale). */
static Port *port_of(Vm *vm, term pid) {
    if (!port_is(vm, pid)) return NULL;
    uint32_t i = term_pid_slot(pid) - vm->port_base;
    if (i >= vm->port_n) return NULL;
    Port *p = port_at(vm, i);
    return p->drv && p->id == pid ? p : NULL;
}

/* One chunk more of the table (zeroed); false at the cap or out of
 * memory. The chunk list is allocated once, for the cap. */
static bool ports_grow(Vm *vm) {
    uint32_t n = vm->port_n, k = n / PORT_CHUNK, nchunks = (vm->port_max + PORT_CHUNK - 1u) / PORT_CHUNK;
    if (n >= vm->port_max) return false;
    if (!vm->port_chunk) {
        vm->port_chunk = nchunks == 1u ? &vm->port_chunk1 : va_alloc(vm, nchunks * sizeof *vm->port_chunk);
        if (!vm->port_chunk) return false;
    }
    Port *c = k ? va_alloc(vm, PORT_CHUNK * sizeof *c) : vm->port_first;   /* the first is in the Vm */
    if (!c) return false;
    if (k) memset(c, 0, PORT_CHUNK * sizeof *c);
    vm->port_chunk[k] = c;
    n += PORT_CHUNK;
    vm->port_n = (uint16_t)(n < vm->port_max ? n : vm->port_max);
    return true;
}

const void *port_name_find(const void *const *list, bool drv, const uint8_t *name, uint32_t len) {
    for (; list && *list; list++) {
        const char *n = drv ? *(const char *const *)*list : (const char *)*list;
        if (strlen(n) == len && (len == 0 || memcmp(n, name, len) == 0)) return *list;
    }
    return NULL;
}

const PortDriver *port_find_driver(const Vm *vm, const uint8_t *name, uint32_t len) {
    return port_name_find((const void *const *)(vm->drivers ? vm->drivers : vm->board->ports), true, name, len);
}

/* Decodes struct t (on a's heap or in the consts) into its type and int
 * fields; false unless it is a struct of at most PORT_MAX_FIELDS ints. */
static bool int_struct(const Vm *vm, const Actor *a, term t, uint32_t *type,
                       int32_t *f, uint32_t *n) {
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_STRUCT) return false;
    uint32_t nf = hdr_words(o[0]) - 2u;
    if (nf > PORT_MAX_FIELDS) return false;
    for (uint32_t i = 0; i < nf; i++) {
        if (!int_val(vm, a, o[2 + i], &f[i])) return false;
    }
    *type = o[1];
    *n = nf;
    return true;
}

/* Opens a port of drv for owner: with the decoded cfg fields f[0..n), or
 * through drv->open_term with the raw cfg when cfg is not T_NIL. */
static term open_in(Vm *vm, Actor *owner, const PortDriver *drv, const int32_t *f, uint32_t n, term cfg) {
    uint16_t i = 0;
    while (i < vm->port_n && port_at(vm, i)->drv) i++;
    vm->open_err = GOGO_E_TOOMANY;
    if (i == vm->port_n && (i >= vm->port_max || !ports_grow(vm))) {
        if (i < vm->port_max) vm->open_err = GOGO_E_NOMEMORY;
        return T_NIL;
    }
    vm->open_err = GOGO_E_NOMEMORY;   /* also the charge check's below */
    if (!actor_port_own(vm, owner)) return T_NIL;

    Port *p = port_at(vm, i);
    uint16_t gen = p->gen;
    memset(p, 0, sizeof *p);
    p->drv = drv;
    p->owner = owner->id;
    p->gen = gen;
    p->id = term_make_pid((uint16_t)(vm->port_base + i), gen);
    int rc = cfg != T_NIL ? drv->open_term(vm, owner, p, i, cfg) : drv->open(vm, p, i, f, n);
    bool ok = rc == 0;
    if (ok && (p->charge_words || p->room_64w)) {
        /* The driver's state for the port is the owner's; room_64w must
         * fit beside it (an event the owner could never take). */
        ok = actor_fits(owner, p->charge_words + p->room_64w * 64u) && actor_charge(vm, owner, p->charge_words);
        if (!ok && drv->close) drv->close(vm, p);
    }
    if (!ok) {
        /* Refused. Bump the generation anyway: anything the driver pushed
         * under it is stale. */
        if (rc) vm->open_err = rc > 0 ? (uint8_t)rc : GOGO_E_BUSY;
        p->drv = NULL;
        p->gen = (uint16_t)((gen + 1u) & GEN_MASK);
        actor_port_disown(owner);
        return T_NIL;
    }
    return p->id;
}

term port_open(Vm *vm, Actor *owner, term name, term cfg) {
    const uint8_t *nm;
    uint32_t len;
    vm->open_err = GOGO_E_NOTFOUND;   /* no driver of that name (or no name) */
    if (!name_bytes(vm, owner, name, &nm, &len)) return T_NIL;
    const PortDriver *drv = port_find_driver(vm, nm, len);
    if (!drv) return T_NIL;
    vm->open_err = GOGO_E_BADARG;     /* every cfg check's below */
    if (drv->open_term) {
        const uint32_t *o = obj_of(vm, owner, cfg);
        if (!o || hdr_kind(o[0]) != K_STRUCT || o[1] != drv->cfg_type) return T_NIL;
        return open_in(vm, owner, drv, NULL, 0, cfg);
    }
    uint32_t type, n;
    int32_t f[PORT_MAX_FIELDS];
    if (!int_struct(vm, owner, cfg, &type, f, &n) || type != drv->cfg_type) return T_NIL;
    return open_in(vm, owner, drv, f, n, T_NIL);
}

term port_open_with(Vm *vm, Actor *owner, const PortDriver *drv, const int32_t *f, uint32_t n) {
    if (!drv->open) return T_NIL;                /* an open_term-only driver */
    return open_in(vm, owner, drv, f, n, T_NIL);
}

void port_command(Vm *vm, const Actor *from, term pid, term msg) {
    Port *p = port_of(vm, pid);
    if (!p) return;
    if (p->drv->command_term) {
        p->drv->command_term(vm, from, p, msg);
        return;
    }
    if (!p->drv->command) return;
    uint32_t type, n;
    int32_t f[PORT_MAX_FIELDS];
    if (!int_struct(vm, from, msg, &type, f, &n)) return;
    p->drv->command(vm, p, type, f, n);
}

#if GOGO_DEBUG
/* The ports holding a pending debounce value: what ports_pending counts. */
static uint32_t pending_ports(const Vm *vm) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < vm->port_n; i++) n += (port_at(vm, i)->flags & PORT_PENDING) != 0;
    return n;
}
#endif

/* Takes from o (NULL: gone) what it holds as p's owner: the first k
 * pinned Bytes, the charge and the port count. */
static void owner_drop(Vm *vm, const Port *p, Actor *o, uint32_t k) {
    if (!o) return;
    while (k--) {
        if (p->held[k]) bytes_release(vm, o, p->held[k]);
    }
    actor_uncharge(o, p->charge_words);
    actor_port_disown(o);
}

void port_close(Vm *vm, term pid) {
    Port *p = port_of(vm, pid);
    if (!p) return;
    if (p->drv->close) p->drv->close(vm, p);
    /* The owner is alive here: an owner's exit closes its ports before
     * its handles go (signals.c). */
    owner_drop(vm, p, actor_lookup(vm, p->owner), PORT_MAX_HELD);
    memset(p->held, 0, sizeof p->held);
    if (p->flags & PORT_PENDING) vm->ports_pending--;
    p->flags = 0;
    GOGO_ASSERT(vm->ports_pending == pending_ports(vm));
    p->drv = NULL;
    p->gen = (uint16_t)((p->gen + 1u) & GEN_MASK);
}

/* Closes every open port of owner (T_NIL: every open port). */
static void close_where(Vm *vm, term owner) {
    for (uint32_t i = 0; i < vm->port_n; i++) {
        Port *p = port_at(vm, i);
        if (p->drv && (owner == T_NIL || p->owner == owner)) port_close(vm, p->id);
    }
}

void port_close_owned(Vm *vm, term owner) {
    const Actor *a = actor_lookup(vm, owner);
    if (a && !actor_port_owns(a)) return;
    close_where(vm, owner);
}

void port_close_all(Vm *vm) { close_where(vm, T_NIL); }

bool port_any_live(const Vm *vm) {
    SOURCES_EACH(vm, q) {
        if (q->s->any_live(vm, q->ctx)) return true;
    }
    for (uint32_t i = 0; i < vm->port_n; i++) {
        const Port *p = port_at(vm, i);
        if (p->drv && p->drv->can_event && p->drv->can_event(vm, p)) return true;
    }
    return false;
}

bool port_work_pending(const Vm *vm) {
    return vm->ports_pending || port_due_now(vm);
}

bool port_due_now(const Vm *vm) {
    if (!evq_empty(vm)) return true;
    SOURCES_EACH(vm, q) {
        if (q->s->pending(vm, q->ctx)) return true;
    }
    return false;
}

bool port_next_deadline(const Vm *vm, uint64_t *deadline) {
    uint64_t best = UINT64_MAX, d;
    SOURCES_EACH(vm, q) {
        if (q->s->next_deadline && q->s->next_deadline(vm, q->ctx, &d) && d < best) best = d;
    }
    for (uint32_t i = 0; vm->ports_pending && i < vm->port_n; i++) {
        const Port *p = port_at(vm, i);
        d = p->last_t + p->debounce_ms;
        if ((p->flags & PORT_PENDING) && d < best) best = d;
    }
    *deadline = best;                        /* meaningful only when true */
    return best != UINT64_MAX;
}

/* ---- delivery ---- */

/* A message holding struct `type` with n fields: ints, but where bit i of
 * raw_mask is set v[i] is a term put as it is (a bool's T_TRUE or
 * T_FALSE, a PID): [hdr][type][f0..fn-1], then a K_INT box for each int
 * that does not fit a small int. NULL when out of memory. */
static Msg *ints_msg(Vm *vm, uint32_t type, const int32_t *v, uint32_t n, uint32_t raw_mask) {
    uint32_t words = 2u + n, i;
    for (i = 0; i < n; i++) {
        if (!(raw_mask >> i & 1u) && !term_int_fits(v[i])) words += 2u;
    }
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    m->data[0] = hdr_make(2u + n, K_STRUCT);
    m->data[1] = type;
    words = 2u + n;                          /* where the next box goes */
    for (i = 0; i < n; i++) {
        term t = (term)v[i];
        if (!(raw_mask >> i & 1u)) {
            t = term_from_int(v[i]);
            if (!term_int_fits(v[i])) {
                m->data[words] = hdr_make(2, K_INT);
                m->data[words + 1] = (uint32_t)v[i];
                t = term_from_ref(words);
                words += 2u;
            }
        }
        m->data[2 + i] = t;
    }
    return m;
}

bool port_deliver_ints(Vm *vm, const Port *p, uint32_t type, const int32_t *v, uint32_t n,
                       uint32_t raw_mask) {
    Actor *o = port_owner(vm, p);
    Msg *m = o ? ints_msg(vm, type, v, n, raw_mask) : NULL;
    if (!m) return false;
    if (msg_try_enqueue(vm, o, m)) return true;
    msg_free(vm, m);
    return false;
}

/* The event_type struct {a, b} to the owner; dropped if the owner is gone
 * or out of memory, fatal over its quota (msg_enqueue). */
static void deliver(Vm *vm, const Port *p, int32_t a, int32_t b) {
    Actor *o = port_owner(vm, p);
    int32_t v[2] = { a, b };
    Msg *m = o ? ints_msg(vm, p->drv->event_type, v, 2, 0) : NULL;
    if (m) msg_enqueue(vm, o, m, true);
}

static void deliver_at(Vm *vm, Port *p, int32_t a, int32_t b, uint64_t t) {
    p->flags |= PORT_DELIVERED;
    p->last_b = b;
    p->last_t = t;
    deliver(vm, p, a, b);
}

/* Ends p's debounce window if it is over by time t: the pending value goes
 * out if it differs from the last delivered one. */
static void flush_due(Vm *vm, Port *p, uint64_t t) {
    if (!(p->flags & PORT_PENDING)) return;
    uint64_t end = p->last_t + p->debounce_ms;
    if (end > t) return;
    p->flags &= (uint8_t)~PORT_PENDING;
    vm->ports_pending--;
    if (p->pend_b != p->last_b) deliver_at(vm, p, p->pend_a, p->pend_b, end);
}

/* The event's 32-bit timestamp (made vm_now time by the caller) as a vm_now value, taking it to be
 * at most ~24 days before now (a timestamp after now counts as now). */
static uint64_t event_time(uint32_t ms, uint64_t now) {
    uint32_t back = (uint32_t)now - ms;
    if ((int32_t)back < 0) return now;
    return back > now ? 0u : now - back;
}

/* Pops at most GOGO_EVQ_CAP events per call, so a producer that keeps the ring
 * full cannot hold the scheduler here; the rest wait for the next pass (the
 * scheduler does not idle while evq_empty(vm) is false). */
void port_drain(Vm *vm, uint64_t now) {
    PortEvent e;
    for (uint32_t n = 0; n < GOGO_EVQ_CAP && evq_pop(vm, &e); n++) {
        if (e.port >= vm->port_n) continue;
        Port *p = port_at(vm, e.port);
        if (!p->drv || p->gen != e.gen) continue;   /* closed since: stale */
        if (!p->debounce_ms) {
            deliver(vm, p, e.a, e.b);
            continue;
        }
        uint64_t t = event_time(e.time_ms - (uint32_t)vm->t0, now);
        flush_due(vm, p, t);
        if (!(p->flags & PORT_DELIVERED) || t >= p->last_t + p->debounce_ms) {
            deliver_at(vm, p, e.a, e.b, t);
        } else {
            if (!(p->flags & PORT_PENDING)) {
                p->flags |= PORT_PENDING;
                vm->ports_pending++;
            }
            p->pend_a = e.a;
            p->pend_b = e.b;
        }
    }
    if (vm->ports_pending) {
        for (uint32_t i = 0; i < vm->port_n; i++) flush_due(vm, port_at(vm, i), now);
    }
    GOGO_ASSERT(vm->ports_pending == pending_ports(vm));
    SOURCES_EACH(vm, q) {
        if (q->s->pending(vm, q->ctx)) q->s->drain(vm, q->ctx, now);
    }
}

/* ---- ownership (M5) ---- */

Actor *port_owner(Vm *vm, const Port *p) {
    return actor_lookup_live(vm, p->owner);
}

bool port_handoff(Vm *vm, Actor *from, term pid, term to) {
    Port *p = port_of(vm, pid);
    if (!p || p->owner != from->id) return false;
    Actor *t = actor_lookup_live(vm, to);   /* NULL for a port PID too */
    if (!t) return false;
    if (t != from) {
        /* t must fit the charge plus what holding the pinned Bytes adds
         * (each root t does not hold yet, at its full length, and t's
         * holder list if it has none). */
        uint32_t k;
        if (!actor_fits(t, p->charge_words + bytes_extra_words(vm, t, p->held, PORT_MAX_HELD))) return false;
        if (!actor_port_own(vm, t)) return false;
        (void)actor_charge(vm, t, p->charge_words);   /* t has its extra now: cannot fail */
        for (k = 0; k < PORT_MAX_HELD; k++) {
            if (p->held[k] && !bytes_retain(vm, t, p->held[k])) break;
        }
        if (k < PORT_MAX_HELD) {
            owner_drop(vm, p, t, k);     /* undo */
            return false;
        }
        owner_drop(vm, p, from, PORT_MAX_HELD);
        p->owner = t->id;
    }
    if (p->drv->handoff) p->drv->handoff(vm, p);
    return true;
}

/* ---- port-held Bytes (M5) ---- */

static term *held_slot(Port *p, term h) {
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) {
        if (p->held[k] == h) return &p->held[k];
    }
    return NULL;
}

term port_bytes_new(Vm *vm, Port *p, uint32_t len, uint8_t **data) {
    term *s = held_slot(p, 0);
    Actor *o = port_owner(vm, p);
    if (!s || !o) return T_NIL;
    term h = bytes_new(vm, o, len, data);
    if (h != T_NIL) *s = h;
    return h;
}

bool port_bytes_retain(Vm *vm, Port *p, term h) {
    term *s = held_slot(p, 0);
    Actor *o = port_owner(vm, p);
    if (!s || !o || !h || !bytes_retain(vm, o, h)) return false;
    *s = h;
    return true;
}

void port_bytes_release(Vm *vm, Port *p, term h) {
    term *s = h ? held_slot(p, h) : NULL;
    if (!s) return;
    *s = 0;
    Actor *o = actor_lookup(vm, p->owner);
    if (o) bytes_release(vm, o, h);
}
