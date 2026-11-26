/* Floating point (GOGO_FLOAT, config.h): float32 and float64 values and
 * the float ops (OP_ADDF .. OP_CONVF). docs/design.md §5.2, §5.5.
 *
 * Values. Every float is a K_FLOAT box (term.h), on the heap or in a const
 * area, never an immediate: [hdr][bits] (2 words) is a float32,
 * [hdr][lo][hi] (3 words) a float64. The size is the type, so TYPEIS tells
 * float32 from float64 and an op computes in its operands' width: float32
 * ops run in single precision (the ESP32-S3's FPU), float64 ops in double
 * (software on the ESP32-S3). The compiler never mixes widths in one op.
 *
 * Decimal conversion is not in the VM core: println prints a float through
 * floatlib_print, strconv's FormatFloat and ParseFloat and gogo/math are
 * B_FLOAT (float_lib); both are in runtime/floatlib, which every build
 * with GOGO_FLOAT links. The core keeps the values and the ops; all their
 * integer conversions and negation work on the bits. */
#ifndef GOGO_FLOAT_H
#define GOGO_FLOAT_H

#include "config.h"

#if GOGO_FLOAT

#include <stdint.h>
#include <stdbool.h>
#include "actor.h"
#include "out.h"

/* Runs the float instruction w (OP_ADDF .. OP_CONVF) on a's top frame,
 * which has nregs registers (it checks the operands against them).
 * Returns R_NORMAL, R_BAD_CODE or R_OUT_OF_MEMORY. May GC. */
uint32_t float_exec(Vm *vm, Actor *a, uint32_t w, uint32_t nregs);

/* TYPEIS id of heap object o, a K_FLOAT: TY_FLOAT32 or TY_FLOAT64. */
uint32_t float_type(const uint32_t *o);

/* The order of K_FLOAT objects x and y of one width, as OP_LTF and OP_EQF
 * see it: -1, 0 or 1, 2 if either is a NaN; -0 == 0 (B_CMP). */
int float_order(const uint32_t *x, const uint32_t *y);

/* Prints the K_FLOAT object obj as Go 1.27's println does:
 * strconv.FormatFloat(v, 'g', -1, width) at its own width (1.5, 1e+21,
 * -0, NaN, +Inf). In runtime/floatlib (floatlib.c); about 0.9 KB of stack
 * (the shortest-digit bignums). */
void floatlib_print(Out *o, const uint32_t *obj);

/* B_FLOAT (gogo/math, strconv.FormatFloat and ParseFloat): op code in
 * register rb, then the arguments; results into ra (and ra+1 for the
 * two-result ops, which check it against nregs). Not part of the VM core:
 * runtime/floatlib/floatlib.c, linked into every build that has
 * GOGO_FLOAT. Returns R_NORMAL, R_BAD_CODE or R_OUT_OF_MEMORY; may GC. */
uint32_t float_lib(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs);

#endif /* GOGO_FLOAT */
#endif
