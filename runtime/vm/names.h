/* Registered names: a VM-wide table mapping a name (any byte string, the
 * empty one included) to the PID of a live actor. An actor may hold several
 * names; a name belongs to one actor. The table lives in Vm.names. Name
 * bytes are copied into va_alloc'd entries, so they do not depend on the
 * heap or message they came from. An actor's names are dropped when it
 * exits (actor_exit), so a name in the table always belongs to a live
 * actor. Lookup is linear: M2 expects few names. */
#ifndef GOGO_NAMES_H
#define GOGO_NAMES_H

#include <stdint.h>
#include <stdbool.h>
#include "actor.h"

/* Registers name for pid, by actor `by` (the registrant). 1 = done; 0 =
 * refused (the name is taken, or pid is not a live actor); -1 = out of
 * memory. */
int names_register(Vm *vm, term by, const uint8_t *name, uint32_t len, term pid);

/* Looks name up; false if it is not registered. */
bool names_whereis(Vm *vm, const uint8_t *name, uint32_t len, term *pid);

/* Removes name if actor `by` registered it (docs/design.md §5.3: only the
 * registrant may; not the named actor, unless it registered itself);
 * otherwise, or if it is not registered, does nothing. */
void names_unregister(Vm *vm, term by, const uint8_t *name, uint32_t len);

/* Removes every name pid holds. */
void names_drop_actor(Vm *vm, term pid);

#endif
