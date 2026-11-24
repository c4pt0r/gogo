/* Bytecode interpreter and scheduler. */
#ifndef GOGO_INTERP_H
#define GOGO_INTERP_H

#include "actor.h"

enum { RUN_YIELD, RUN_WAIT, RUN_EXIT };

/* Runs `a` until it yields (reductions used up), blocks, or exits, saving
 * pc/fp/sp in the actor. On RUN_EXIT the actor has exited through
 * actor_exit and is already freed. */
int interp_run(Vm *vm, Actor *a);

/* Grabs the emergency reserve, spawns the image's entry function as the
 * first actor and runs the scheduler until the run queue is empty. Returns
 * 0 then; 2 if the entry function has too many registers; 1 if the
 * reserve or the main actor cannot be allocated. */
int vm_run(Vm *vm);

#endif
