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
                     uint16_t max_words, bool link, term *pid);

/* The child limit SpawnOpt asks for: Options{MaxMemory} in bytes, as
 * words (0 or less: the default; rounded down but at least 1; at most
 * HEAP_MAX_WORDS). False if opts is not an actor.Options struct. */
bool vm_spawn_opt_words(const Vm *vm, const Actor *a, term opts, uint16_t *out);

/* Grabs the emergency reserve, starts module 0's "main" export the way
 * code.Spawn starts any entry (module_spawn) and runs the scheduler until
 * the run queue is empty. Returns 0 then; 1 if module 0 has no "main"
 * export ("gogo: image has no main" on stderr), or the reserve or the main
 * actor cannot be allocated. */
int vm_run(Vm *vm);

#endif
