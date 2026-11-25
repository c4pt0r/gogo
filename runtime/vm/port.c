#include "port.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"
#include "alloc.h"
#include "evq.h"
#include "platform.h"
#include "config.h"
#include "bytes.h"
#include <string.h>

#if (GOGO_MAX_PORTS) < 1u || (GOGO_MAX_PORTS) > 16u
#error "GOGO_MAX_PORTS must be 1..16 (port PIDs use slots PORT_SLOT_BASE..0xFFFF)"
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

bool port_is(term pid) {
    return term_is_pid(pid) && term_pid_slot(pid) >= PORT_SLOT_BASE;
}

/* The open port pid names, or NULL (not a port PID, free or stale). */
static Port *port_of(Vm *vm, term pid) {
    if (!port_is(pid)) return NULL;
    uint32_t i = term_pid_slot(pid) - PORT_SLOT_BASE;
    if (i >= GOGO_MAX_PORTS) return NULL;
    Port *p = &vm->ports[i];
    return p->drv && p->id == pid ? p : NULL;
}

static const PortDriver *find_driver(const Vm *vm, const uint8_t *name, uint32_t len) {
    const PortDriver *const *l = vm->drivers ? vm->drivers : platform_port_drivers();
    for (; l && *l; l++) {
        if (strlen((*l)->name) == len && (len == 0 || memcmp((*l)->name, name, len) == 0)) return *l;
    }
    return NULL;
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
    while (i < GOGO_MAX_PORTS && vm->ports[i].drv) i++;
    if (i == GOGO_MAX_PORTS) return T_NIL;
    if (!actor_port_own(vm, owner)) return T_NIL;

    Port *p = &vm->ports[i];
    uint16_t gen = p->gen;
    memset(p, 0, sizeof *p);
    p->drv = drv;
    p->owner = owner->id;
    p->gen = gen;
    p->id = term_make_pid((uint16_t)(PORT_SLOT_BASE + i), gen);
    bool ok = (cfg != T_NIL ? drv->open_term(vm, owner, p, i, cfg) : drv->open(vm, p, i, f, n)) == 0;
    if (ok && (p->charge_words || p->room_64w)) {
        /* The driver's state for the port is the owner's; room_64w must
         * fit beside it (an event the owner could never take). */
        ok = (uint32_t)owner->mem_words + actor_charged_words(owner) + p->charge_words + p->room_64w * 64u <=
                 actor_max_words(owner) && actor_charge(vm, owner, p->charge_words);
        if (!ok && drv->close) drv->close(vm, p);
    }
    if (!ok) {
        /* Refused. Bump the generation anyway: anything the driver pushed
         * under it is stale. */
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
    if (!name_bytes(vm, owner, name, &nm, &len)) return T_NIL;
    const PortDriver *drv = find_driver(vm, nm, len);
    if (!drv) return T_NIL;
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

void port_close(Vm *vm, term pid) {
    Port *p = port_of(vm, pid);
    if (!p) return;
    if (p->drv->close) p->drv->close(vm, p);
    Actor *o = actor_lookup(vm, p->owner);
    if (o) {
        /* Release what the port pinned for o (o is alive here: an owner's
         * exit closes its ports before its handles go, signals.c). */
        for (uint32_t k = 0; k < PORT_MAX_HELD; k++) {
            if (p->held[k]) bytes_release(vm, o, p->held[k]);
        }
        actor_uncharge(o, p->charge_words);
        actor_port_disown(o);
    }
    memset(p->held, 0, sizeof p->held);
    if (p->flags & PORT_PENDING) vm->ports_pending--;
    p->flags = 0;
    p->drv = NULL;
    p->gen = (uint16_t)((p->gen + 1u) & GEN_MASK);
}

void port_close_owned(Vm *vm, term owner) {
    const Actor *a = actor_lookup(vm, owner);
    if (a && !actor_port_owns(a)) return;
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        Port *p = &vm->ports[i];
        if (p->drv && p->owner == owner) port_close(vm, p->id);
    }
}

void port_close_all(Vm *vm) {
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        Port *p = &vm->ports[i];
        if (p->drv) port_close(vm, p->id);
    }
}

bool port_any_live(const Vm *vm) {
    SOURCES_EACH(vm, q) {
        if (q->s->any_live(vm, q->ctx)) return true;
    }
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        const Port *p = &vm->ports[i];
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
    for (uint32_t i = 0; vm->ports_pending && i < GOGO_MAX_PORTS; i++) {
        const Port *p = &vm->ports[i];
        d = p->last_t + p->debounce_ms;
        if ((p->flags & PORT_PENDING) && d < best) best = d;
    }
    *deadline = best;                        /* meaningful only when true */
    return best != UINT64_MAX;
}

/* ---- delivery ---- */

/* A message holding struct `type` with n int fields (bit i of bool_mask:
 * field i is a bool): [hdr][type][f0..fn-1], then a K_INT box for each
 * field that does not fit a small int. NULL when out of memory. */
static Msg *ints_msg(Vm *vm, uint32_t type, const int32_t *v, uint32_t n, uint32_t bool_mask) {
    uint32_t words = 2u + n, i;
    for (i = 0; i < n; i++) {
        if (!(bool_mask >> i & 1u) && !term_int_fits(v[i])) words += 2u;
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
        term t = v[i] ? T_TRUE : T_FALSE;
        if (!(bool_mask >> i & 1u)) {
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
                       uint32_t bool_mask) {
    Actor *o = port_owner(vm, p);
    Msg *m = o ? ints_msg(vm, type, v, n, bool_mask) : NULL;
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

/* The event's 32-bit timestamp as a platform_now_ms value, taking it to be
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
        if (e.port >= GOGO_MAX_PORTS) continue;
        Port *p = &vm->ports[e.port];
        if (!p->drv || p->gen != e.gen) continue;   /* closed since: stale */
        if (!p->debounce_ms) {
            deliver(vm, p, e.a, e.b);
            continue;
        }
        uint64_t t = event_time(e.time_ms, now);
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
        for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) flush_due(vm, &vm->ports[i], now);
    }
    SOURCES_EACH(vm, q) {
        if (q->s->pending(vm, q->ctx)) q->s->drain(vm, q->ctx, now);
    }
}

/* ---- ownership (M5) ---- */

Actor *port_owner(Vm *vm, const Port *p) {
    Actor *o = actor_lookup(vm, p->owner);
    return o && o->state != A_EXITED && !(o->flags & AF_EXIT_PENDING) ? o : NULL;
}

bool port_handoff(Vm *vm, Actor *from, term pid, term to) {
    Port *p = port_of(vm, pid);
    if (!p || p->owner != from->id) return false;
    Actor *t = port_is(to) ? NULL : actor_lookup(vm, to);
    if (!t || t->state == A_EXITED || (t->flags & AF_EXIT_PENDING)) return false;
    if (t != from) {
        /* t must fit the charge plus what holding the pinned Bytes adds
         * (each root t does not hold yet, at its full length, and t's
         * holder list if it has none). */
        uint32_t k;
        uint64_t need = (uint64_t)p->charge_words + bytes_extra_words(vm, t, p->held, PORT_MAX_HELD);
        if ((uint64_t)t->mem_words + actor_charged_words(t) + need > actor_max_words(t)) return false;
        if (!actor_port_own(vm, t)) return false;
        if (p->charge_words && !actor_charge(vm, t, p->charge_words)) {
            actor_port_disown(t);
            return false;
        }
        for (k = 0; k < PORT_MAX_HELD; k++) {
            if (p->held[k] && !bytes_retain(vm, t, p->held[k])) break;
        }
        if (k < PORT_MAX_HELD) {         /* undo */
            while (k--) if (p->held[k]) bytes_release(vm, t, p->held[k]);
            actor_uncharge(t, p->charge_words);
            actor_port_disown(t);
            return false;
        }
        for (k = 0; k < PORT_MAX_HELD; k++) {
            if (p->held[k]) bytes_release(vm, from, p->held[k]);
        }
        actor_uncharge(from, p->charge_words);
        actor_port_disown(from);
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
