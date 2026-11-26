/* The float library: the B_FLOAT builtin behind gogo/math and
 * strconv.FormatFloat / ParseFloat (float_lib, declared in
 * runtime/vm/floats.h). It is not part of the VM core (tools/vmsize.sh does
 * not count it) and is built only with GOGO_FLOAT. It uses no libm: Sqrt,
 * Floor, Ceil, Trunc and the rounding functions work on the bits;
 * ParseFloat uses the C library's strtod / strtof after checking Go's
 * syntax itself.
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

/* strconv.AppendFloat(v, fmt, prec, is32 ? 32 : 64) for fmt 'b', 'e', 'E',
 * 'f', 'g' and 'G'; any other fmt gives '%' and fmt (Go also has 'x' and
 * 'X', which gogo leaves out). Writes to dst unless it is NULL and returns
 * the length either way. Uses about 1.4 KB of stack (an 800-byte digit
 * buffer and the bignums of float_digits). */
uint32_t floatlib_format(char *dst, double v, bool is32, char fmt, int prec);

#endif
