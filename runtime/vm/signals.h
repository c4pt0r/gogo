/* Actor exit: the single exit path, deferred exits, links and exit
 * signals, and the emergency memory reserve (spec M2 §5.1.1, §5.1.4, §5.2,
 * §5.3). */
#ifndef GOGO_SIGNALS_H
#define GOGO_SIGNALS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"

/* Per-actor state most actors never need (pending exit, links, monitors,
 * charged words; registered names live in names.c, timers in Vm.timers).
 * Lazily allocated, owned by signals.c, reached through Actor.extra. */
typedef struct ActorExtra ActorExtra;

/* The only way an actor dies (normal return, Exit, runtime error, quota,
 * kill, cascade): prints the crash report unless code is R_NORMAL or
 * R_SHUTDOWN (" in <func>" when func_name is a const ref to the function's
 * name, nothing more for T_EMPTYSTR), closes the ports it owns
 * (port_close_owned; before any notification, so a restarted child can
 * reopen them), then signals every linked actor: a
 * trapping one gets its preallocated ExitMsg{From: a, Reason{code,
 * func_name}}; any other is marked to exit with the same code and func_name
 * unless code is R_NORMAL. Marked actors exit when next scheduled, so a
 * cascade is iterative, never recursive. Every monitoring actor gets its
 * preallocated DownMsg (sig_monitor). The timers targeting it are
 * cancelled (timer_cancel_actor). Then frees the actor, its queued
 * messages and its extra. a must not be on the run queue; it is gone when
 * this returns. Allocates nothing for the link and monitor notifications.
 * Guaranteed delivery of a preallocated notification is bounded by the
 * receiver's 16-bit mailbox counter: if the notification would overflow it,
 * it is dropped and the receiver exits with R_MAILBOX_OVERFLOW. */
void actor_exit(Vm *vm, Actor *a, uint32_t code, term func_name);

/* Deferred exit for an actor that is not running right now (mailbox
 * quota, kill, cascade), or for the running actor itself, which checks
 * after each builtin. Sets AF_EXIT_PENDING, makes a waiting actor
 * runnable, and from now on messages to it are dropped. When the scheduler
 * next runs it, it exits with `code` and `func_name` before executing any
 * instruction. The first mark wins if marked twice. If the pending code
 * cannot be stored even with the reserve, the actor exits with
 * R_OUT_OF_MEMORY instead (an actor with links already has the space). */
void actor_mark_exit(Vm *vm, Actor *a, uint32_t code, term func_name);

/* True if a is marked for exit; stores the code and function name. */
bool actor_exit_pending(const Actor *a, uint32_t *code, term *func_name);

/* Links a and the actor pid names, both ways; each side preallocates the
 * ExitMsg it will send the other when it exits. Linking oneself, or an
 * already linked pid, does nothing. A dead or stale pid is an immediate
 * NoProc exit signal to a (see sig_kill): an ExitMsg{From: pid,
 * Reason{NoProc, ""}} if a traps exits, else a is marked to exit with
 * R_NO_PROC. Each side's entry + ExitMsg block is charged to that side
 * (actor_charged_words) until unlink, delivery or exit. a's side must fit
 * a's limit; pid's side is charged even past its limit (links are
 * deduplicated, so this is at most one entry per linked pair, and linking
 * already ties the two actors' fates), and its next allocation then fails.
 * False if out of memory or over a's limit (nothing is linked or charged
 * then); the caller exits with R_OUT_OF_MEMORY. */
bool sig_link(Vm *vm, Actor *a, term pid);

/* Removes the link between a and pid on both sides, frees both
 * preallocated messages and releases both charges. Does nothing if they
 * are not linked. */
void sig_unlink(Vm *vm, Actor *a, term pid);

/* True if a has a link to pid. */
bool sig_is_linked(const Actor *a, term pid);

/* Sends an exit signal with `code` from `from` to the actor pid names (spec
 * §5.2): nothing for a dead pid or one already marked for exit; a target
 * trapping exits gets ExitMsg{From: from, Reason{code, ""}} (an ordinary
 * message: allocated now, subject to the receiver's quota) unless code is
 * R_KILLED; otherwise the target is marked to exit with `code`, except
 * that R_NORMAL does nothing to a non-trapping target other than `from`
 * itself (a non-trapping actor killing itself behaves like Exit). False if
 * the ExitMsg cannot be allocated; the caller exits with R_OUT_OF_MEMORY.
 * Unlike the link notifications, this ExitMsg is allocated now and is
 * subject to the target's quota. */
bool sig_kill(Vm *vm, Actor *from, term pid, uint32_t code);

/* Monitors. Makes a fresh Ref on a's heap and returns it
 * (it may GC: the caller reloads its register pointer); T_NIL if out of
 * memory, and the caller exits with R_OUT_OF_MEMORY. When the actor pid
 * names exits, for whatever reason (Normal included, never cascaded),
 * a receives a DownMsg{Ref, From: pid, Reason{code, func}}: the block is
 * preallocated here, queued without the quota, and counted in mbox_words.
 * Both monitor entries and the DownMsg block are charged to a (the
 * watcher) until Demonitor, delivery or either side's exit; if that would
 * take a past its limit, nothing is made and T_NIL is returned (a exits
 * with R_OUT_OF_MEMORY), so a Monitor loop cannot exhaust the machine.
 * A dead or stale pid gets a DownMsg{Reason{NoProc, ""}} queued to a at
 * once, allocated now (the quota applies; failing to allocate it is also
 * T_NIL). Monitoring oneself returns a Ref and never delivers. Monitors die
 * with either side: neither a watcher that exited nor a target that
 * exited leaves a dangling entry or an unfreed block. Several monitors of
 * one actor deliver newest first. */
term sig_monitor(Vm *vm, Actor *a, term pid);

/* Drops the monitor whose Ref has this serial, frees its preallocated
 * DownMsg and releases a's charge for it. An
 * unknown, already fired or foreign serial does nothing. A DownMsg already
 * queued in a's mailbox stays there (as in Erlang, without the flush
 * option); once Demonitor returns, none is queued for that ref. */
void sig_demonitor(Vm *vm, Actor *a, uint64_t serial);

/* TrapExit: turns exit signals other than Killed into ExitMsg messages. */
void sig_set_trap(Actor *a, bool on);

/* Words charged to a besides its heap block: its queued messages
 * (mbox_words, including the off-heap Bytes they carry) plus
 * ActorExtra.charged_words (the pending SendAfter timers it created, its
 * monitors, its link sides) plus its held off-heap Bytes (held_bytes,
 * rounded up to words; bytes.h). Heap block + charged words
 * must stay within actor_max_words; this is the single place limit checks
 * read. */
uint32_t actor_charged_words(const Actor *a);

/* Where a's extra keeps its off-heap Bytes holder list (bytes.h; the list
 * is NULL while a holds none). actor_held: NULL when a has no extra.
 * actor_held_new: the extra is allocated if needed (NULL if it cannot be). */
HeldBytes **actor_held(Actor *a);
HeldBytes **actor_held_new(Vm *vm, Actor *a);

/* ActorExtra.charged_words alone (0 without an extra); for tests. */
uint32_t actor_extra_charged(const Actor *a);

/* Charges / releases words for resources a holds outside its heap
 * (timer.c: pending SendAfter timers). actor_charge allocates the extra
 * block on first use; false if out of memory (nothing is charged). The
 * caller checks the limit. */
bool actor_charge(Vm *vm, Actor *a, uint32_t words);
void actor_uncharge(Actor *a, uint32_t words);

/* Frees a->extra, if any. Called by actor_free. After actor_exit its lists
 * are empty; for an actor freed without exiting, it first removes the
 * peers' entries naming a and their charges (no notifications). */
void actor_extra_free(Vm *vm, Actor *a);

/* Emergency reserve: vm_run grabs GOGO_RESERVE_BYTES at start (false if it
 * cannot) and releases it when it returns. Exit-path allocations use
 * signal_alloc: if the allocator fails, the reserve is released and the
 * allocation retried once; the reserve is re-acquired after the exit (or
 * mark) completes, when memory is available again. */
bool vm_reserve_init(Vm *vm);
void vm_reserve_release(Vm *vm);
void *signal_alloc(Vm *vm, size_t bytes);

/* Counts the SendAfter timers pending for a (timer.c bookkeeping). _ref
 * allocates a's extra if needed and returns false if it cannot (nothing
 * counted); _unref undoes one; _has says whether any is pending. */
bool actor_timer_ref(Vm *vm, Actor *a);
void actor_timer_unref(Actor *a);
bool actor_timer_has(const Actor *a);
/* Likewise for pending timers charged to a as their creator. */
bool actor_timer_own(Vm *vm, Actor *a);
void actor_timer_disown(Actor *a);
bool actor_timer_owns(const Actor *a);
/* Likewise for the open ports a owns (port.c): _own is false if the extra
 * cannot be allocated (nothing counted). */
bool actor_port_own(Vm *vm, Actor *a);
void actor_port_disown(Actor *a);
bool actor_port_owns(const Actor *a);

#endif
