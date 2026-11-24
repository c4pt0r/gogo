/* Bytecode interpreter and scheduler. */
#ifndef GOGO_INTERP_H
#define GOGO_INTERP_H

#include "actor.h"

enum { RUN_YIELD, RUN_WAIT, RUN_EXIT };

/* Runs `a` until it yields (reductions used up), blocks, or exits, saving
 * pc/fp/sp in the actor. On RUN_EXIT the caller frees the actor. */
int interp_run(Vm *vm, Actor *a);

/* Spawns the image's entry function as the first actor and runs the
 * scheduler until the run queue is empty. Returns 0. */
int vm_run(Vm *vm);

#endif
