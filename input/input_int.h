/* input/ internals shared by kbd.c and source.c. */
#ifndef GOGO_INPUT_INT_H
#define GOGO_INPUT_INT_H

#include "input.h"
#include "opcodes.h"
#include "actor.h"

/* Deadlines inside input/ are 32-bit ms (wrapping, compared as signed
 * deltas) to save RAM; reached() and deadline_abs() convert from and to the
 * scheduler's 64-bit clock. */
static inline bool reached(uint64_t now, uint32_t t) { return (int32_t)((uint32_t)now - t) >= 0; }
static inline uint64_t deadline_abs(uint64_t now, uint32_t t) {
    int32_t d = (int32_t)(t - (uint32_t)now);
    return d <= 0 ? now : now + (uint32_t)d;
}

bool input_failed(void);                     /* input_install found no source slot */
bool kbd_is_open(void), power_is_open(void);
bool kbd_work(uint64_t now);                 /* a raw event waits, or a repeat is due */
bool kbd_next_deadline(uint64_t now, uint64_t *d);   /* the next auto-repeat or error retry */
void kbd_drain(Vm *vm, uint64_t now);

bool power_work(uint64_t now);                /* a sample is due, wanted (Read) or flagged */
bool power_next_deadline(uint64_t now, uint64_t *d);       /* the next scheduled sample */
void power_drain(Vm *vm, uint64_t now);
void dropped_inc(void);                      /* counts an event dropped for input_dropped() */

#endif
