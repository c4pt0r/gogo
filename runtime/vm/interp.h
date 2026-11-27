/* Bytecode interpreter and scheduler. */
#ifndef GOGO_INTERP_H
#define GOGO_INTERP_H

#include "actor.h"

enum { RUN_YIELD, RUN_WAIT, RUN_EXIT };

/* Runs `a` until it yields (reductions used up), blocks, or exits, saving
 * pc/fp/sp in the actor. On RUN_EXIT the actor has exited through
 * actor_exit and is already freed. */
int interp_run(Vm *vm, Actor *a);

/* Starts function f of module slot `mod` in a new actor of that module
 * (counted in its live actors). With caps, val is a closure in src's heap
 * whose captures become r0..; else val (copied out of src's heap like a
 * message; with src NULL an immediate) becomes r0 if f takes a parameter.
 * The caller checks f's arity. max_words: the child's limit (0: default);
 * link: SpawnLink. Returns R_NORMAL and sets *pid, or the reason the spawn
 * failed (R_OUT_OF_MEMORY, R_BAD_CODE; R_BAD_MSG / R_BAD_CODE when the
 * cross-module rules refuse the copy for module mod, msg_build). The
 * spawner's heap does not move. */
uint32_t vm_spawn_fn(Vm *vm, Actor *src, uint8_t mod, uint32_t f, term val, bool caps,
                     gogo_words_t max_words, bool link, term *pid);

/* The child limit SpawnOpt asks for: Options{MaxMemory} in bytes, as
 * words (0 or less: the default; rounded down but at least 1; at most
 * HEAP_MAX_WORDS). False if opts is not an actor.Options struct. */
bool vm_spawn_opt_words(const Vm *vm, const Actor *a, term opts, gogo_words_t *out);

/* Running a program. A host either calls vm_run, or drives the scheduler
 * itself: vm_start once, then vm_step until it returns VM_DONE (waiting
 * between steps while it returns VM_IDLE), then vm_stop. All on the
 * thread that owns the Vm.
 *
 * Latched wake, for a host that steps the Vm itself: events arrive from
 * ISRs and other threads (evq_push, net RX) and call host->wake, so such
 * a host MUST still provide wake, and its own wait MUST be latched: a wake
 * since the host last consumed the latch makes the wait return at once.
 * The host consumes (clears) the latch before it calls vm_step, never
 * between a VM_IDLE result and its wait; then a wake that lands after
 * vm_step's final check is never slept through. */

/* vm_step's results. */
enum { VM_RUNNABLE, VM_IDLE, VM_DONE };
/* Vm.run_state: before vm_start, between vm_start and vm_stop, after. */
enum { VM_ST_NEW, VM_ST_RUN, VM_ST_DONE };

/* Grabs the emergency reserve, starts module 0's "main" export the way
 * code.Spawn starts any entry (module_spawn), sets the time base (vm_now
 * restarts at 0) and empties the event ring. Returns 0; 1 if module 0 has
 * no "main" export ("gogo: image has no main" on stderr), the reserve or
 * the main actor cannot be allocated; 1 without a message if the Vm has
 * been started before (a Vm runs once). */
int vm_start(Vm *vm);

/* Runs scheduler passes: each delivers queued events and due source work,
 * fires due timers and runs at most one time slice of the next runnable
 * actor. budget: scheduler passes to run (a pass that runs no actor, only
 * a drain, counts too), 0 = until VM_IDLE or VM_DONE. Returns
 *  VM_RUNNABLE: the budget ran out and work remains (an actor is
 *               runnable, or events, source work or a timer are due);
 *  VM_IDLE:     nothing is runnable, no event is queued, no source and no
 *               timer is due: wait (latched, above) until vm_next_deadline
 *               or a wake;
 *  VM_DONE:     no actor is runnable, no timer is pending and no port or
 *               source is live (every actor exited or is blocked for
 *               good), or the Vm is not started. Call vm_stop. */
/* Not re-entrant: a hook that vm_step calls (write, alloc, ...) must not
 * call vm_step, vm_start or vm_stop. Boards never do; the embedding API
 * refuses it (GOGO_EBUSY) with a flag of its own, so the core pays
 * nothing for a guard. */
int vm_step(Vm *vm, uint32_t budget);

/* The earliest pending timer or port/source deadline, as an absolute
 * vm_now value (the Vm's epoch; vm_host_ms converts it to the host's
 * now_ms epoch). It may already have passed by the time the host reads
 * its clock: step again at once (compare before subtracting). False if
 * there is none: only a wake can make work. */
bool vm_next_deadline(const Vm *vm, uint64_t *abs_vm_ms);

/* A vm_now value as a host->now_ms value. */
static inline uint64_t vm_host_ms(const Vm *vm, uint64_t vm_ms) { return vm_ms + vm->t0; }

/* Closes every open port (output drivers always get close) and releases
 * the reserve; the Vm cannot be started again. Safe to repeat. */
void vm_stop(Vm *vm);

/* vm_start, then vm_step(vm, 0), waiting in host->wait while it returns
 * VM_IDLE, until VM_DONE; then vm_stop. Returns 0 then; 1 if vm_start
 * fails, or at once (no message) if the host has no wait hook: such a
 * host drives vm_step itself. */
int vm_run(Vm *vm);

#endif
