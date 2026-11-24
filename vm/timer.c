#include "timer.h"
#include "alloc.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"

static Timer *timer_new(uint64_t deadline, uint64_t serial, term pid, Msg *msg) {
    Timer *t = va_alloc(sizeof *t);
    if (!t) return NULL;
    t->next = NULL;
    t->deadline = deadline;
    t->serial = serial;
    t->pid = pid;
    t->msg = msg;
    t->owner = T_NIL;
    t->charge = 0;
    return t;
}

/* Words a SendAfter timer holding m is charged to its creator: the queued
 * message footprint (as in mbox_words) plus the timer entry. */
static uint32_t send_charge(const Msg *m) {
    return msg_footprint_words(m->words) + (uint32_t)((sizeof(Timer) + 3u) / 4u);
}

/* Takes t's charge off its creator, if it still has one and is alive
 * (generation-checked lookup), and forgets the creator. Idempotent. */
static void uncharge(Vm *vm, Timer *t) {
    Actor *owner = actor_lookup(vm, t->owner);
    if (owner) actor_uncharge(owner, t->charge);
    t->owner = T_NIL;
    t->charge = 0;
}

static void timer_free(Vm *vm, Timer *t) {
    uncharge(vm, t);
    if (t->msg) msg_free(t->msg);
    va_free(t, sizeof *t);
}

/* Inserts t after every timer with a deadline <= its own, so timers with
 * equal deadlines fire in the order they were made. */
static void timer_insert(Vm *vm, Timer *t) {
    Timer **p = &vm->timers;
    while (*p && (*p)->deadline <= t->deadline) p = &(*p)->next;
    t->next = *p;
    *p = t;
}

bool timer_add_wakeup(Vm *vm, Actor *a, uint64_t deadline) {
    if (a->flags & AF_WAKEUP_ARMED) return true;   /* at most one per actor */
    Timer *t = timer_new(deadline, 0, a->id, NULL);
    if (!t) return false;
    timer_insert(vm, t);
    a->flags |= AF_WAKEUP_ARMED;
    return true;
}

void timer_cancel_wakeup(Vm *vm, Actor *a) {
    if (a->flags & AF_WAKEUP_ARMED) {
        for (Timer **p = &vm->timers; *p; p = &(*p)->next) {
            Timer *t = *p;
            if (t->pid == a->id && t->msg == NULL) {
                *p = t->next;
                timer_free(vm, t);
                break;
            }
        }
    }
    a->flags &= (uint8_t)~(AF_WAKEUP_ARMED | AF_TIMED_OUT);
}

term timer_add_send(Vm *vm, Actor *from, uint64_t deadline, term to_pid, term msg) {
    /* Copy first: msg_build leaves from's heap unchanged, while
     * heap_new_ref may GC and move msg. */
    Msg *m = msg_build(from, msg);
    if (!m) return T_NIL;
    uint32_t charge = send_charge(m);
    /* The pending timer counts against the creator's limit, like a queued
     * message against the receiver's. */
    if ((uint32_t)from->mem_words + actor_charged_words(from) + charge > actor_max_words(from)) {
        msg_free(m);
        return T_NIL;
    }
    Timer *t = timer_new(deadline, 0, to_pid, m);
    if (!t) {
        msg_free(m);
        return T_NIL;
    }
    if (!actor_charge(from, charge)) {
        timer_free(vm, t);
        return T_NIL;
    }
    t->owner = from->id;
    t->charge = charge;
    term ref = heap_new_ref(vm, from);     /* the block may only use what is left */
    if (ref == T_NIL) {
        timer_free(vm, t);                 /* releases the charge */
        return T_NIL;
    }
    const uint32_t *obj = from->mem + term_ref_off(ref);
    t->serial = (uint64_t)obj[1] | ((uint64_t)obj[2] << 32);
    timer_insert(vm, t);
    return ref;
}

bool timer_cancel(Vm *vm, uint64_t serial) {
    if (serial == 0) return false;          /* wakeups are not cancellable by ref */
    for (Timer **p = &vm->timers; *p; p = &(*p)->next) {
        Timer *t = *p;
        if (t->serial == serial) {
            *p = t->next;
            timer_free(vm, t);
            return true;
        }
    }
    return false;
}

void timer_cancel_actor(Vm *vm, const Actor *a) {
    Timer **p = &vm->timers;
    while (*p) {
        Timer *t = *p;
        if (t->pid == a->id) {
            *p = t->next;
            timer_free(vm, t);
        } else {
            if (t->owner == a->id) {       /* still fires; a's charge dies with a */
                t->owner = T_NIL;
                t->charge = 0;
            }
            p = &t->next;
        }
    }
}

bool timer_next(const Vm *vm, uint64_t *deadline) {
    if (!vm->timers) return false;
    *deadline = vm->timers->deadline;
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
    while (vm->timers && vm->timers->deadline <= now) {
        Timer *t = vm->timers;
        vm->timers = t->next;              /* unlinked before anything runs */
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
