#include "port.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"
#include "alloc.h"
#include "evq.h"
#include "platform.h"
#include "config.h"
#include <string.h>

#if (GOGO_MAX_PORTS) < 1u || (GOGO_MAX_PORTS) > 16u
#error "GOGO_MAX_PORTS must be 1..16 (port PIDs use slots PORT_SLOT_BASE..0xFFFF)"
#endif

#define GEN_MASK 0xFFFu

static const PortDriver *const *override;

void port_set_drivers(const PortDriver *const *list) { override = list; }

static const PortDriver *const *drivers(void) {
    return override ? override : platform_port_drivers();
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

static const PortDriver *find_driver(const uint8_t *name, uint32_t len) {
    const PortDriver *const *l = drivers();
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

term port_open(Vm *vm, Actor *owner, term name, term cfg) {
    const uint8_t *nm;
    uint32_t len;
    if (!name_bytes(vm, owner, name, &nm, &len)) return T_NIL;
    const PortDriver *drv = find_driver(nm, len);
    if (!drv) return T_NIL;
    uint32_t type, n;
    int32_t f[PORT_MAX_FIELDS];
    if (!int_struct(vm, owner, cfg, &type, f, &n) || type != drv->cfg_type) return T_NIL;
    uint16_t i = 0;
    while (i < GOGO_MAX_PORTS && vm->ports[i].drv) i++;
    if (i == GOGO_MAX_PORTS) return T_NIL;
    if (!actor_port_own(owner)) return T_NIL;

    Port *p = &vm->ports[i];
    uint16_t gen = p->gen;
    memset(p, 0, sizeof *p);
    p->drv = drv;
    p->owner = owner->id;
    p->gen = gen;
    p->id = term_make_pid((uint16_t)(PORT_SLOT_BASE + i), gen);
    if (drv->open(p, i, f, n) != 0) {
        /* Refused. Bump the generation anyway: anything the driver pushed
         * under it is stale. */
        p->drv = NULL;
        p->gen = (uint16_t)((gen + 1u) & GEN_MASK);
        actor_port_disown(owner);
        return T_NIL;
    }
    return p->id;
}

void port_command(Vm *vm, const Actor *from, term pid, term msg) {
    Port *p = port_of(vm, pid);
    if (!p || !p->drv->command) return;
    uint32_t type, n;
    int32_t f[PORT_MAX_FIELDS];
    if (!int_struct(vm, from, msg, &type, f, &n)) return;
    p->drv->command(p, type, f, n);
}

void port_close(Vm *vm, term pid) {
    Port *p = port_of(vm, pid);
    if (!p) return;
    if (p->drv->close) p->drv->close(p);
    Actor *o = actor_lookup(vm, p->owner);
    if (o) actor_port_disown(o);
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
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        const Port *p = &vm->ports[i];
        if (p->drv && p->drv->can_event && p->drv->can_event(p)) return true;
    }
    return false;
}

bool port_next_deadline(const Vm *vm, uint64_t *deadline) {
    if (!vm->ports_pending) return false;
    bool have = false;
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        const Port *p = &vm->ports[i];
        if (!(p->flags & PORT_PENDING)) continue;
        uint64_t d = p->last_t + p->debounce_ms;
        if (!have || d < *deadline) *deadline = d;
        have = true;
    }
    return have;
}

/* ---- delivery ---- */

/* Sends the owner an event_type struct {a, b}: [hdr][type][a][b], then a
 * K_INT box for each field that does not fit a small int. Dropped if the
 * owner is gone or marked to exit, or if out of memory; over the owner's
 * quota it exits MailboxOverflow (msg_enqueue). */
static void deliver(Vm *vm, const Port *p, int32_t a, int32_t b) {
    Actor *o = actor_lookup(vm, p->owner);
    if (!o || o->state == A_EXITED || (o->flags & AF_EXIT_PENDING)) return;
    uint16_t words = 4;
    if (!term_int_fits(a)) words += 2;
    if (!term_int_fits(b)) words += 2;
    Msg *m = va_alloc(msg_alloc_bytes(words));
    if (!m) return;
    m->next = NULL;
    m->words = words;
    m->flags = 0;
    m->root = term_from_ref(0);
    m->data[0] = hdr_make(4, K_STRUCT);
    m->data[1] = p->drv->event_type;
    uint32_t at = 4;
    int32_t v[2] = { a, b };
    for (uint32_t i = 0; i < 2; i++) {
        if (term_int_fits(v[i])) {
            m->data[2 + i] = term_from_int(v[i]);
        } else {
            m->data[at] = hdr_make(2, K_INT);
            m->data[at + 1] = (uint32_t)v[i];
            m->data[2 + i] = term_from_ref(at);
            at += 2;
        }
    }
    msg_enqueue(vm, o, m, true);
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

void port_drain(Vm *vm, uint64_t now) {
    PortEvent e;
    while (evq_pop(&e)) {
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
}
