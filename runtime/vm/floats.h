/* Floating point (GOGO_FLOAT, config.h): float32 and float64 values, the
 * float ops (OP_ADDF .. OP_CONVF), and the float -> decimal conversion that
 * println and strconv.FormatFloat share. docs/design.md §5.5.
 *
 * Values. Every float is a K_FLOAT box (term.h), on the heap or in a const
 * area, never an immediate: [hdr][bits] (2 words) is a float32,
 * [hdr][lo][hi] (3 words) a float64. The size is the type, so TYPEIS tells
 * float32 from float64 and an op computes in its operands' width: float32
 * ops run in single precision (the ESP32-S3's FPU), float64 ops in double
 * (software on the ESP32-S3). The compiler never mixes widths in one op.
 *
 * Decimal conversion. float_digits produces the decimal digits of a
 * positive finite value exactly, with a small bignum (Steele & White /
 * Burger & Dybvig): the shortest digits that read back as the same value
 * (closest to it among the shortest), or a number of digits correctly
 * rounded (half to even), as Go's strconv does. println prints a float as
 * Go 1.27's println does, strconv.FormatFloat(v, 'g', -1, width) at the
 * value's own width (1.5, 1e+21, -0, NaN, +Inf); floatlib_format
 * (runtime/floatlib) is the rest of FormatFloat. No libc printf. */
#ifndef GOGO_FLOAT_H
#define GOGO_FLOAT_H

#include "config.h"

#if GOGO_FLOAT

#include <stdint.h>
#include <stdbool.h>
#include "actor.h"
#include "out.h"

/* The value of the K_FLOAT that t names (heap or const area), widened to
 * double (exact for a float32), and whether it is a float32. False for
 * anything else. */
bool float_val(const Vm *vm, const Actor *a, term t, double *d, bool *is32);

/* A new heap K_FLOAT holding d (rounded to float32 when is32). May GC (the
 * caller reloads register pointers). False on OOM. */
bool float_box(Vm *vm, Actor *a, double d, bool is32, term *out);

/* Runs the float instruction w (OP_ADDF .. OP_CONVF) on a's top frame,
 * which has nregs registers (it checks the operands against them).
 * Returns R_NORMAL, R_BAD_CODE or R_OUT_OF_MEMORY. May GC. */
uint32_t float_exec(Vm *vm, Actor *a, uint32_t w, uint32_t nregs);

/* d truncated toward zero to an int (32 bits) or, with to64, an int64:
 * NaN gives 0, a value beyond the range the nearest end (as Go on arm64;
 * Go leaves it to the implementation). */
int64_t float_trunc(double d, bool to64);

/* TYPEIS id of heap object o, a K_FLOAT: TY_FLOAT32 or TY_FLOAT64. */
uint32_t float_type(const uint32_t *o);

/* Prints the K_FLOAT o as println does. */
void out_float(Out *o, const uint32_t *obj);

/* float_digits modes. */
enum {
    FD_SHORTEST = 0,   /* the shortest digits that read back as v (n unused) */
    FD_DIGITS   = 1,   /* n >= 1 significant digits, rounded half to even */
    FD_FRAC     = 2    /* digits up to n >= 0 places after the point */
};

/* Decimal digits of v (positive and finite; read at float32 width if
 * is32): ASCII digits into buf (at most cap of them, cap >= 17; a double
 * has at most 767 significant digits, so 800 always suffices), value
 * 0.d1d2... x 10^*dp. Trailing zeros are dropped. Returns the digit count
 * (0 when FD_FRAC rounds v to zero). */
int float_digits(double v, bool is32, int mode, int n, char *buf, int cap, int *dp);

/* B_FLOAT (gogo/math, strconv.FormatFloat and ParseFloat): op code in
 * register rb, then the arguments; results into ra (and ra+1 for the
 * two-result ops, which check it against nregs). Not part of the VM core:
 * runtime/floatlib/floatlib.c, linked into every build that has
 * GOGO_FLOAT. Returns R_NORMAL, R_BAD_CODE or R_OUT_OF_MEMORY; may GC. */
uint32_t float_lib(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs);

#endif /* GOGO_FLOAT */
#endif
