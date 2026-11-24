/* Timers (spec M2 §5.3): ReceiveTimeout / Sleep wakeups and SendAfter.
 *
 * All timers live in one VM-global list, Vm.timers, sorted by deadline
 * (ties keep insertion order). Deadlines are absolute platform_now_ms()
 * values, 64-bit, so they never wrap. Entries are va_alloc'd and freed
 * exactly once: when they fire, when they are cancelled, or when the
 * actor they target exits (timer_cancel_actor, called by actor_exit).
 * Timers name actors by PID and look them up (generation-checked) when they
 * fire, so a timer never holds an Actor pointer.
 *
 * A wakeup belongs to the actor it wakes; an actor has at most one. Its
 * state is two Actor.flags bits: AF_WAKEUP_ARMED while the entry is in the
 * list, AF_TIMED_OUT once it has fired (until the builtin consumes it).
 *
 * A SendAfter timer targets `to`; like Erlang's send_after to a pid, it is
 * cancelled when the target exits, not when its creator does. Until it
 * fires or is cancelled, its message footprint plus the entry are charged
 * to the creator (actor_charged_words), so one actor piling up timers hits
 * its own limit instead of exhausting memory others need. The charge is
 * released when the timer fires (delivered or dropped), is cancelled, or
 * the creator exits (the creator is then forgotten, and the timer still
 * fires for its target). */
#ifndef GOGO_TIMER_H
#define GOGO_TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include "term.h"
#include "actor.h"

struct Timer {
    Timer *next;
    uint64_t deadline;        /* platform_now_ms() value it fires at */
    uint64_t serial;          /* SendAfter: its Ref's serial; wakeup: 0 */
    term pid;                 /* the actor it targets (to wake / to send to) */
    Msg *msg;                 /* SendAfter: the copied message; wakeup: NULL */
    term owner;               /* SendAfter: its creator while charged, else T_NIL */
    uint32_t charge;          /* words charged to owner (actor_charge) */
};

/* Arms a's wakeup for `deadline` and sets AF_WAKEUP_ARMED. If one is
 * already armed, does nothing (and returns true). False if out of memory. */
bool timer_add_wakeup(Vm *vm, Actor *a, uint64_t deadline);

/* Removes a's wakeup if armed, and clears AF_WAKEUP_ARMED and AF_TIMED_OUT. */
void timer_cancel_wakeup(Vm *vm, Actor *a);

/* SendAfter: copies msg out of from's heap now and queues it for to_pid at
 * `deadline`, charging it to from. Returns a fresh Ref on from's heap naming
 * the timer (may GC: the caller reloads its register pointer), or T_NIL if
 * out of memory or the charge would take from past its limit (nothing is
 * left behind; the caller exits with R_OUT_OF_MEMORY). A dead or stale to_pid is allowed: the timer
 * fires into nothing. */
term timer_add_send(Vm *vm, Actor *from, uint64_t deadline, term to_pid, term msg);

/* Cancels the SendAfter timer whose Ref has this serial and frees its
 * message. False if there is none (fired, cancelled, or never a timer). */
bool timer_cancel(Vm *vm, uint64_t serial);

/* Frees every timer that targets a: its wakeup and SendAfter timers to it.
 * Timers a created for others stay, no longer charged to anyone. Called by
 * actor_exit. */
void timer_cancel_actor(Vm *vm, const Actor *a);

/* The earliest deadline; false if no timer is pending. */
bool timer_next(const Vm *vm, uint64_t *deadline);

/* Fires, in deadline order, every timer due at `now` (deadline <= now).
 * A SendAfter message goes through the receiver's quota (msg_enqueue); a
 * dead, stale or exiting target drops it. A wakeup sets AF_TIMED_OUT,
 * clears AF_WAKEUP_ARMED and makes the actor runnable if it is waiting
 * (an actor already runnable is not queued twice). */
void timer_fire_due(Vm *vm, uint64_t now);

#endif
