/* The float library: the B_FLOAT builtin behind gogo/math and
 * strconv.FormatFloat / ParseFloat (float_lib) and println's floats
 * (floatlib_print), both declared in runtime/vm/floats.h. It is not part
 * of the VM core (tools/vmsize.sh does not count it) and is built only
 * with GOGO_FLOAT. It uses no libm and no libc conversion: Sqrt, Floor,
 * Ceil, Trunc and the rounding functions work on the bits, and the
 * decimal conversions both ways are exact, on one small bignum
 * (decimal.c).
 *
 * B_FLOAT's first argument is one of these op codes (the compiler's
 * floatOps, compiler/codegen_float.go, checked by TestFloatOpsMatchC);
 * every float argument and result is a float64. */
#ifndef GOGO_FLOATLIB_H
#define GOGO_FLOATLIB_H

enum {
    FLOAT_OP_SQRT      = 0,   /* x -> math.Sqrt(x), correctly rounded */
    FLOAT_OP_FLOOR     = 1,   /* x -> math.Floor(x) */
    FLOAT_OP_CEIL      = 2,   /* x -> math.Ceil(x) */
    FLOAT_OP_TRUNC     = 3,   /* x -> math.Trunc(x) */
    FLOAT_OP_ROUND     = 4,   /* x -> math.Round(x): half away from zero */
    FLOAT_OP_ROUNDEVEN = 5,   /* x -> math.RoundToEven(x) */
    FLOAT_OP_ABS       = 6,   /* x -> math.Abs(x) */
    FLOAT_OP_COPYSIGN  = 7,   /* f, sign -> math.Copysign(f, sign) */
    FLOAT_OP_SIGNBIT   = 8,   /* x -> bool: math.Signbit(x) */
    FLOAT_OP_INF       = 9,   /* sign int -> math.Inf(sign) */
    FLOAT_OP_NAN       = 10,  /* -> math.NaN() */
    FLOAT_OP_ISNAN     = 11,  /* x -> bool */
    FLOAT_OP_ISINF     = 12,  /* x, sign int -> bool: math.IsInf(x, sign) */
    FLOAT_OP_MAX       = 13,  /* x, y -> math.Max(x, y) */
    FLOAT_OP_MIN       = 14,  /* x, y -> math.Min(x, y) */
    FLOAT_OP_FORMAT    = 15,  /* f, fmt byte, prec, bitSize int -> string: strconv.FormatFloat */
    FLOAT_OP_PARSE     = 16   /* s string, bitSize int -> f, ok: strconv.ParseFloat (two results) */
};

#include <stdbool.h>
#include <stdint.h>

/* floatlib_digits modes. */
enum {
    FD_SHORTEST = 0,   /* the shortest digits that read back as v (n unused) */
    FD_DIGITS   = 1,   /* n >= 1 significant digits, rounded half to even */
    FD_FRAC     = 2    /* digits up to n >= 0 places after the point */
};

/* Decimal digits of v (positive and finite; read at float32 width if
 * is32), exactly, with a small bignum (Steele & White / Burger & Dybvig):
 * ASCII digits into buf (at most cap of them, cap >= 17; a double has at
 * most 767 significant digits, so 800 always suffices), value 0.d1d2...
 * x 10^*dp. Trailing zeros are dropped. Returns the digit count (0 when
 * FD_FRAC rounds v to zero). About 0.75 KB of stack. */
int floatlib_digits(double v, bool is32, int mode, int n, char *buf, int cap, int *dp);

/* strconv.AppendFloat(v, fmt, prec, is32 ? 32 : 64) for fmt 'b', 'e', 'E',
 * 'f', 'g' and 'G'; any other fmt gives '%' and fmt (Go also has 'x' and
 * 'X', which gogo leaves out). Writes to dst unless it is NULL and returns
 * the length either way. Stack: about 0.75 KB for the shortest form
 * (prec < 0, println), 1.6 KB with a precision (an 800-byte digit buffer
 * besides). */
uint32_t floatlib_format(char *dst, double v, bool is32, char fmt, int prec);

/* floatlib_parse results. */
enum { FP_OK = 0, FP_RANGE = 1, FP_SYNTAX = -1 };

/* strconv.ParseFloat(s, is32 ? 32 : 64), correctly rounded (ties to even)
 * for any input, without strtod: FP_OK with the value in *out (a float32
 * widened when is32), FP_RANGE (beyond the largest finite value: *out is
 * +-Inf, Go's ErrRange) or FP_SYNTAX (*out is 0). Go's syntax: decimal or
 * 0x hexadecimal with a p exponent, underscores between digits, inf /
 * infinity with a sign, nan without one, any case. At most 800
 * significant digits are kept exactly (more become a sticky digit, which
 * keeps the rounding exact). About 1 KB of stack. */
int floatlib_parse(const uint8_t *s, uint32_t n, bool is32, double *out);

#endif
