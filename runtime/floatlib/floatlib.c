/* The float library: B_FLOAT and println's floats (floatlib.h); the
 * decimal conversions are in decimal.c. */
#include "floats.h"

#if GOGO_FLOAT

#include <string.h>
#include "floatlib.h"
#include "heap.h"
#include "opcodes.h"

static uint64_t bits_of(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static double of_bits(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

#define SIGN_BIT 0x8000000000000000ull

static double fl_trunc(double x) {
    uint64_t u = bits_of(x);
    int e = (int)(u >> 52 & 0x7FFu) - 1023;
    if (e >= 52) return x;                          /* integral, Inf or NaN */
    if (e < 0) return of_bits(u & SIGN_BIT);        /* |x| < 1: a signed zero */
    return of_bits(u & ~((1ull << (52 - e)) - 1u));
}

static double fl_floor(double x) {
    double t = fl_trunc(x);
    return x < 0 && t != x ? t - 1 : t;
}

static double fl_ceil(double x) {
    double t = fl_trunc(x);
    return x > 0 && t != x ? t + 1 : t;
}

static double fl_copysign(double f, double s) {
    return of_bits((bits_of(f) & ~SIGN_BIT) | (bits_of(s) & SIGN_BIT));
}

/* Half away from zero (even: half to even). x - trunc(x) is exact. */
static double fl_round(double x, int even) {
    double t = fl_trunc(x);
    if (t == x || x != x) return x;
    double d = x - t;
    if (d < 0) d = -d;
    if (d > 0.5 || (d == 0.5 && (!even || (int64_t)t % 2 != 0))) t += fl_copysign(1, x);
    return t;
}

/* Correctly rounded square root, one result bit per step (no libm). */
static double fl_sqrt(double x) {
    uint64_t u = bits_of(x);
    if (x != x || x == 0 || u == 0x7FF0000000000000ull) return x;   /* NaN, +-0, +Inf */
    if (u & SIGN_BIT) return of_bits(0x7FF8000000000001ull);        /* negative: NaN */
    int e = (int)(u >> 52);
    uint64_t m = u & 0xFFFFFFFFFFFFFull;
    if (e == 0) {                                   /* subnormal: normalize */
        e = 1;
        while (!(m & 1ull << 52)) { m <<= 1; e--; }
    } else {
        m |= 1ull << 52;
    }
    e -= 1023;                                       /* x = m x 2^(e-52) */
    if (e & 1) { m <<= 1; e--; }                     /* even exponent, m < 2^54 */
    /* q = floor(sqrt(m x 2^54)) in [2^53, 2^54), digit by digit over the
     * bit pairs of m x 2^54: the result's 53 bits and one to round with. */
    uint64_t q = 0, r = 0;
    for (int i = 53; i >= 0; i--) {
        uint64_t two = 2 * i >= 54 ? m >> (2 * i - 54) & 3u : 0u;
        r = r << 2 | two;
        uint64_t t = q << 2 | 1u;
        if (r >= t) { r -= t; q = q << 1 | 1u; }
        else q <<= 1;
    }
    /* The last bit rounds (an exact half would need m x 2^54 to be an odd
     * square). */
    q = (q >> 1) + (q & 1u);
    if (q >> 53) { q >>= 1; e += 2; }
    return of_bits((uint64_t)(e / 2 + 1023) << 52 | (q & 0xFFFFFFFFFFFFFull));
}

/* ---- values ---- */

/* The float64 that t names (a 3-word K_FLOAT, heap or const area): every
 * float argument of B_FLOAT is one (the compiler converts). */
static bool f64_val(const Vm *vm, const Actor *a, term t, double *d) {
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || o[0] != hdr_make(3, K_FLOAT)) return false;
    *d = of_bits((uint64_t)o[1] | (uint64_t)o[2] << 32);
    return true;
}

/* A new heap float64 holding d. May GC; false on OOM. */
static bool f64_box(Vm *vm, Actor *a, double d, term *out) {
    uint32_t off = heap_alloc(vm, a, 3, NULL, 0);
    if (off == UINT32_MAX) return false;
    uint64_t u = bits_of(d);
    a->mem[off] = hdr_make(3, K_FLOAT);
    a->mem[off + 1] = (uint32_t)u;
    a->mem[off + 2] = (uint32_t)(u >> 32);
    *out = term_from_ref(off);
    return true;
}

/* ---- println ---- */

void floatlib_print(Out *o, const uint32_t *obj) {
    char t[32];
    bool is32 = hdr_words(obj[0]) == 2u;
    double v;
    if (is32) {
        float f;
        memcpy(&f, &obj[1], sizeof f);
        v = f;
    } else {
        v = of_bits((uint64_t)obj[1] | (uint64_t)obj[2] << 32);
    }
    out_bytes(o, t, floatlib_format(t, v, is32, 'g', -1));
}

/* ---- B_FLOAT ---- */

uint32_t float_lib(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs) {
    term *R = (term *)a->mem + a->fp;
    int32_t op, i, prec, bitsz;
    double x = 0, y = 0, r;
    bool b;
    term t;
    if (rc < 1u || !int_val(vm, a, R[rb], &op)) return R_BAD_CODE;
    const term *arg = R + rb + 1;
    uint32_t nargs = rc - 1u;
    /* Ops on float64 arguments first: read them all. */
    static const uint8_t nf[] = {1, 1, 1, 1, 1, 1, 1, 2, 1, 0, 0, 1, 1, 2, 2, 1, 0};
    if (op < 0 || (uint32_t)op >= sizeof nf || nargs < nf[op]) return R_BAD_CODE;
    if (nf[op] >= 1 && !f64_val(vm, a, arg[0], &x)) return R_BAD_CODE;
    if (nf[op] >= 2 && !f64_val(vm, a, arg[1], &y)) return R_BAD_CODE;
    switch (op) {
    case FLOAT_OP_SQRT:      r = fl_sqrt(x); break;
    case FLOAT_OP_FLOOR:     r = fl_floor(x); break;
    case FLOAT_OP_CEIL:      r = fl_ceil(x); break;
    case FLOAT_OP_TRUNC:     r = fl_trunc(x); break;
    case FLOAT_OP_ROUND:     r = fl_round(x, 0); break;
    case FLOAT_OP_ROUNDEVEN: r = fl_round(x, 1); break;
    case FLOAT_OP_ABS:       r = of_bits(bits_of(x) & ~SIGN_BIT); break;
    case FLOAT_OP_COPYSIGN:  r = fl_copysign(x, y); break;
    case FLOAT_OP_NAN:       r = of_bits(0x7FF8000000000001ull); break;   /* Go's NaN */
    case FLOAT_OP_INF:
        if (nargs != 1u || !int_val(vm, a, arg[0], &i)) return R_BAD_CODE;
        r = of_bits(i >= 0 ? 0x7FF0000000000000ull : 0xFFF0000000000000ull);
        break;
    case FLOAT_OP_MAX: case FLOAT_OP_MIN: {
        bool max = op == FLOAT_OP_MAX;
        double inf = of_bits(max ? 0x7FF0000000000000ull : 0xFFF0000000000000ull);
        if (x == inf || y == inf) r = inf;
        else if (x != x || y != y) r = of_bits(0x7FF8000000000001ull);
        else if (x == 0 && x == y) r = ((bits_of(x) & SIGN_BIT) != 0) == max ? y : x;
        else r = (max ? x > y : x < y) ? x : y;
        break;
    }
    case FLOAT_OP_SIGNBIT: case FLOAT_OP_ISNAN: case FLOAT_OP_ISINF:
        if (op == FLOAT_OP_SIGNBIT) b = (bits_of(x) & SIGN_BIT) != 0;
        else if (op == FLOAT_OP_ISNAN) b = x != x;
        else {
            if (nargs != 2u || !int_val(vm, a, arg[1], &i)) return R_BAD_CODE;
            b = x - x != 0 && x == x && (i == 0 || (i > 0) == (x > 0));
        }
        R[ra] = term_from_bool(b);
        return R_NORMAL;
    case FLOAT_OP_FORMAT: {
        int32_t fmt;
        if (nargs != 4u || !int_val(vm, a, arg[1], &fmt) || !int_val(vm, a, arg[2], &prec) ||
            !int_val(vm, a, arg[3], &bitsz) || (bitsz != 32 && bitsz != 64)) return R_BAD_CODE;
        uint32_t max = (HEAP_MAX_WORDS - 2u) * 4u;
        if (prec > (int32_t)max) return R_OUT_OF_MEMORY;           /* cannot fit any heap */
        uint32_t len = floatlib_format(NULL, x, bitsz == 32, (char)fmt, prec);
        if (len > max) return R_OUT_OF_MEMORY;
        uint32_t words = 2u + (len + 3u) / 4u;
        uint32_t off = heap_alloc(vm, a, words, NULL, 0);
        if (off == UINT32_MAX) return R_OUT_OF_MEMORY;
        a->mem[off] = hdr_make(words, K_STRING);
        a->mem[off + 1] = len;
        a->mem[off + words - 1u] = 0;
        floatlib_format((char *)(a->mem + off + 2), x, bitsz == 32, (char)fmt, prec);
        ((term *)a->mem + a->fp)[ra] = len ? term_from_ref(off) : T_EMPTYSTR;
        return R_NORMAL;
    }
    case FLOAT_OP_PARSE: {
        const uint8_t *p;
        uint32_t len;
        if (nargs != 2u || ra + 1u >= nregs || !int_val(vm, a, arg[1], &bitsz) ||
            (bitsz != 32 && bitsz != 64) || !name_bytes(vm, a, arg[0], &p, &len)) return R_BAD_CODE;
        b = floatlib_parse(p, len, bitsz == 32, &r) == FP_OK;
        if (!f64_box(vm, a, r, &t)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        R[ra] = t;
        R[ra + 1] = term_from_bool(b);
        return R_NORMAL;
    }
    default:
        return R_BAD_CODE;
    }
    if (nf[op] != nargs && op != FLOAT_OP_INF) return R_BAD_CODE;
    if (!f64_box(vm, a, r, &t)) return R_OUT_OF_MEMORY;
    ((term *)a->mem + a->fp)[ra] = t;
    return R_NORMAL;
}

#else
typedef int gogo_no_float;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
