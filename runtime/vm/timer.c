#include "timer.h"
#include "alloc.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"
#include <string.h>

static Timer *timer_new(uint64_t deadline, uint64_t serial, term pid, Msg *msg) {
    Timer *t = va_alloc(sizeof *t);
    if (!t) return NULL;
    t->seq = 0;
    t->deadline = deadline;
    t->serial = serial;
    t->pid = pid;
    t->msg = msg;
    t->owner = T_NIL;
    t->charge = 0;
    t->counted = false;
    return t;
}

/* Words a SendAfter timer holding m is charged to its creator: the queued
 * message charge (as in mbox_words, off-heap Bytes included) plus the
 * timer entry. */
static uint32_t send_charge(const Msg *m) {
    return msg_charge_words(m) + (uint32_t)((sizeof(Timer) + 3u) / 4u);
}

/* Takes t's charge off its creator, if it still has one and is alive
 * (generation-checked lookup), and forgets the creator. Idempotent. */
static void uncharge(Vm *vm, Timer *t) {
    Actor *owner = actor_lookup(vm, t->owner);
    if (owner) {
        actor_uncharge(owner, t->charge);
        actor_timer_disown(owner);
    }
    t->owner = T_NIL;
    t->charge = 0;
}

static void timer_free(Vm *vm, Timer *t) {
    uncharge(vm, t);
    if (t->counted) {                      /* no longer pending for its target */
        Actor *to = actor_lookup(vm, t->pid);
        if (to) actor_timer_unref(to);
        t->counted = false;
    }
    if (t->msg) msg_free(t->msg);
    va_free(t, sizeof *t);
}

/* ---- the heap: Vm.timers[0..ntimers), min at [0] ---- */

static bool timer_less(const Timer *x, const Timer *y) {
    return x->deadline < y->deadline || (x->deadline == y->deadline && x->seq < y->seq);
}

static void sift_up(Timer **h, uint32_t i) {
    Timer *t = h[i];
    while (i > 0) {
        uint32_t p = (i - 1u) / 2u;
        if (!timer_less(t, h[p])) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = t;
}

static void sift_down(Timer **h, uint32_t n, uint32_t i) {
    Timer *t = h[i];
    for (;;) {
        uint32_t c = 2u * i + 1u;
        if (c >= n) break;
        if (c + 1u < n && timer_less(h[c + 1u], h[c])) c++;
        if (!timer_less(h[c], t)) break;
        h[i] = h[c];
        i = c;
    }
    h[i] = t;
}

/* Makes room for one more entry. False if out of memory. */
static bool heap_reserve(Vm *vm) {
    if (vm->ntimers < vm->timers_cap) return true;
    uint32_t cap = vm->timers_cap ? vm->timers_cap * 2u : 8u;
    Timer **h = va_alloc((size_t)cap * sizeof(Timer *));
    if (!h) return false;
    if (vm->ntimers) memcpy(h, vm->timers, (size_t)vm->ntimers * sizeof(Timer *));
    va_free(vm->timers, (size_t)vm->timers_cap * sizeof(Timer *));   /* NULL, 0 when first */
    vm->timers = h;
    vm->timers_cap = cap;
    return true;
}

/* Frees the array once nothing is pending. */
static void heap_trim(Vm *vm) {
    if (vm->ntimers == 0 && vm->timers) {
        va_free(vm->timers, (size_t)vm->timers_cap * sizeof(Timer *));
        vm->timers = NULL;
        vm->timers_cap = 0;
    }
}

/* Adds t; heap_reserve must have succeeded. Later insertions sort after
 * equal deadlines (seq), so ties fire in the order they were made. */
static void timer_insert(Vm *vm, Timer *t) {
    t->seq = vm->timer_seq++;
    vm->timers[vm->ntimers] = t;
    sift_up(vm->timers, vm->ntimers++);
}

/* Unlinks and returns the entry at index i. */
static Timer *heap_remove(Vm *vm, uint32_t i) {
    Timer **h = vm->timers;
    Timer *t = h[i];
    Timer *last = h[--vm->ntimers];
    if (i < vm->ntimers) {
        h[i] = last;
        if (i > 0 && timer_less(last, h[(i - 1u) / 2u])) sift_up(h, i);
        else sift_down(h, vm->ntimers, i);
    }
    heap_trim(vm);
    return t;
}

uint32_t timer_pending(const Vm *vm) { return vm->ntimers; }

bool timer_add_wakeup(Vm *vm, Actor *a, uint64_t deadline) {
    if (a->flags & AF_WAKEUP_ARMED) return true;   /* at most one per actor */
    if (!heap_reserve(vm)) return false;
    Timer *t = timer_new(deadline, 0, a->id, NULL);
    if (!t) { heap_trim(vm); return false; }
    timer_insert(vm, t);
    a->flags |= AF_WAKEUP_ARMED;
    return true;
}

void timer_cancel_wakeup(Vm *vm, Actor *a) {
    if (a->flags & AF_WAKEUP_ARMED) {
        for (uint32_t i = 0; i < vm->ntimers; i++) {
            const Timer *t = vm->timers[i];
            if (t->pid == a->id && t->msg == NULL) {
                timer_free(vm, heap_remove(vm, i));
                break;
            }
        }
    }
    a->flags &= (uint8_t)~(AF_WAKEUP_ARMED | AF_TIMED_OUT);
}

term timer_add_send(Vm *vm, Actor *from, uint64_t deadline, term to_pid, term msg, uint32_t *why) {
    /* Copy first: msg_build leaves from's heap unchanged, while
     * heap_new_ref may GC and move msg. */
    const Actor *dst = actor_lookup(vm, to_pid);
    Msg *m = msg_build(vm, from, msg, (dst ? dst : from)->mod, why);
    if (!m) return T_NIL;
    uint32_t charge = send_charge(m);
    /* The pending timer counts against the creator's limit, like a queued
     * message against the receiver's. */
    if ((uint32_t)from->mem_words + actor_charged_words(from) + charge > actor_max_words(from)) {
        msg_free(m);
        return T_NIL;
    }
    /* Everything that can fail but the Ref comes first, so inserting
     * cannot fail afterwards. */
    if (!heap_reserve(vm)) {
        msg_free(m);
        return T_NIL;
    }
    Timer *t = timer_new(deadline, 0, to_pid, m);
    if (!t) {
        msg_free(m);
        heap_trim(vm);
        return T_NIL;
    }
    Actor *to = actor_lookup(vm, to_pid);
    if (to && to->state != A_EXITED) {
        if (!actor_timer_ref(to)) {
            timer_free(vm, t);
            heap_trim(vm);
            return T_NIL;
        }
        t->counted = true;
    }
    if (!actor_charge(from, charge)) {
        timer_free(vm, t);
        heap_trim(vm);
        return T_NIL;
    }
    if (!actor_timer_own(from)) {
        timer_free(vm, t);
        heap_trim(vm);
        return T_NIL;
    }
    t->owner = from->id;
    t->charge = charge;
    term ref = heap_new_ref(vm, from);     /* the block may only use what is left */
    if (ref == T_NIL) {
        timer_free(vm, t);                 /* releases the charge */
        heap_trim(vm);
        return T_NIL;
    }
    const uint32_t *obj = from->mem + term_ref_off(ref);
    t->serial = (uint64_t)obj[1] | ((uint64_t)obj[2] << 32);
    timer_insert(vm, t);
    return ref;
}

bool timer_cancel(Vm *vm, uint64_t serial) {
    if (serial == 0) return false;          /* wakeups are not cancellable by ref */
    for (uint32_t i = 0; i < vm->ntimers; i++) {
        if (vm->timers[i]->serial == serial) {
            timer_free(vm, heap_remove(vm, i));
            return true;
        }
    }
    return false;
}

void timer_cancel_actor(Vm *vm, const Actor *a) {
    /* Nothing targets a unless it has a wakeup armed or counted SendAfters;
     * nothing is charged to it unless its extra counts timers it created. */
    bool targeted = (a->flags & AF_WAKEUP_ARMED) || actor_timer_has(a);
    bool owner = actor_timer_owns(a);
    if (!targeted && !owner) return;
    Timer **h = vm->timers;
    uint32_t j = 0;
    for (uint32_t i = 0; i < vm->ntimers; i++) {
        vm->timer_scanned++;
        Timer *t = h[i];
        if (targeted && t->pid == a->id) {
            timer_free(vm, t);
        } else {
            if (t->owner == a->id) {       /* still fires; a's charge dies with a */
                t->owner = T_NIL;
                t->charge = 0;
            }
            h[j++] = t;
        }
    }
    vm->ntimers = j;
    for (uint32_t i = j / 2u; i-- > 0;) sift_down(h, j, i);
    heap_trim(vm);
}

bool timer_next(const Vm *vm, uint64_t *deadline) {
    if (!vm->ntimers) return false;
    *deadline = vm->timers[0]->deadline;
    return true;
}

/* Wakes the actor t targets, if it is still alive. */
static void fire_wakeup(Vm *vm, const Timer *t) {
    Actor *a = actor_lookup(vm, t->pid);
    if (!a || a->state == A_EXITED) return;
    a->flags = (uint8_t)((a->flags & ~AF_WAKEUP_ARMED) | AF_TIMED_OUT);
    if (a->state == A_WAITING) {
        a->state = A_RUNNABLE;
        runq_push(vm, a);
    }
}

void timer_fire_due(Vm *vm, uint64_t now) {
    while (vm->ntimers && vm->timers[0]->deadline <= now) {
        Timer *t = heap_remove(vm, 0);     /* unlinked before anything runs */
        if (t->msg) {
            /* Released first: a creator sending to itself must not have
             * the message counted twice by its quota. */
            uncharge(vm, t);
            Actor *to = actor_lookup(vm, t->pid);
            if (to && to->state != A_EXITED && !(to->flags & AF_EXIT_PENDING)) {
                msg_enqueue(vm, to, t->msg, true);   /* owns it now, even if dropped */
                t->msg = NULL;
            }
        } else {
            fire_wakeup(vm, t);
        }
        timer_free(vm, t);
    }
}
