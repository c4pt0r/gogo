/* Floating point: values and ops (floats.h). */
#include "floats.h"

#if GOGO_FLOAT

#include <string.h>
#include "heap.h"
#include "opcodes.h"

/* ---- values ---- */

/* The raw bits of K_FLOAT object o (a float32's in the low half). */
static uint64_t raw(const uint32_t *o) {
    return hdr_words(o[0]) == 3u ? (uint64_t)o[1] | (uint64_t)o[2] << 32 : o[1];
}

/* The K_FLOAT object t names (heap or const area), or NULL. */
static const uint32_t *float_obj(const Vm *vm, const Actor *a, term t) {
    const uint32_t *o = obj_of(vm, a, t);
    return o && (o[0] == hdr_make(2, K_FLOAT) || o[0] == hdr_make(3, K_FLOAT)) ? o : NULL;
}

uint32_t float_type(const uint32_t *o) {
    return hdr_words(o[0]) == 2u ? TY_FLOAT32 : TY_FLOAT64;
}

/* ---- ops ---- */

/* The widths' bit layouts. */
#define MAG(is32)  ((is32) ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull)
#define INF(is32)  ((is32) ? 0x7F800000ull : 0x7FF0000000000000ull)
#define SIGN(is32) ((is32) ? 0x80000000ull : 0x8000000000000000ull)

/* x compared with y (raw bits of one width) in IEEE order, on the bits:
 * -1, 0 or 1, and 2 if either is NaN; -0 equals 0. A float32's bits go to
 * the top half, where sign and magnitude order as a float64's do; only
 * the infinity differs. */
static int fcmp(uint64_t x, uint64_t y, bool is32) {
    uint64_t mag = MAG(false), inf = INF(false);
    if (is32) {
        x <<= 32;
        y <<= 32;
        inf = INF(true) << 32;
    }
    if ((x & mag) > inf || (y & mag) > inf) return 2;
    if (!((x | y) & mag)) return 0;
    int64_t p = (int64_t)x < 0 ? -(int64_t)(x & mag) : (int64_t)x;   /* sign-magnitude to an order */
    int64_t q = (int64_t)y < 0 ? -(int64_t)(y & mag) : (int64_t)y;
    return (p > q) - (p < q);
}

/* The float64 bits of the float32 with bits u (exact; a NaN stays one). */
static uint64_t widen(uint32_t u) {
    float f;
    double d;
    uint64_t r;
    memcpy(&f, &u, sizeof f);
    d = f;
    memcpy(&r, &d, sizeof r);
    return r;
}

/* The float64 with raw bits u truncated toward zero to an int (32 bits)
 * or, with to64, an int64, on the bits: NaN gives 0 and a value beyond the
 * range the nearest end (C leaves both undefined; Go on arm64 does this).
 * A float32 is widened first (exact, so the result is the same). */
static int64_t ftrunc(uint64_t u, bool to64) {
    int lim = to64 ? 63 : 31;
    int e = (int)((uint32_t)(u >> 32) << 1 >> 21) - 1023;
    uint64_t m = (u & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    bool neg = (int64_t)u < 0;
    if ((u & MAG(false)) > INF(false) || e < 0) return 0;      /* NaN, |x| < 1 */
    if (e >= lim) return neg ? (to64 ? INT64_MIN : INT32_MIN) : (to64 ? INT64_MAX : INT32_MAX);
    uint64_t v = e >= 52 ? m << (e - 52) : m >> (52 - e);
    return neg ? -(int64_t)v : (int64_t)v;
}

uint32_t float_exec(Vm *vm, Actor *a, uint32_t w, uint32_t nregs) {
    uint32_t op = OP_OP(w), ra = OP_A(w), rb = OP_B(w), rc = OP_C(w);
    term *R = (term *)a->mem + a->fp;
    if (ra >= nregs || rb >= nregs || (op != OP_NEGF && op != OP_CONVF && rc >= nregs)) return R_BAD_CODE;
    const uint32_t *p = float_obj(vm, a, R[rb]);
    uint64_t x, r;
    bool is32;
    term t;
    if (op == OP_CONVF && rc <= 1u) {        /* float64 (c=0) / float32 (c=1) of an int or int64 */
        int32_t v;
        int64_t i;
        const uint32_t *o = obj_of(vm, a, R[rb]);
        if (int_val(vm, a, R[rb], &v)) i = v;
        else if (o && o[0] == hdr_make(3, K_INT64)) i = (int64_t)((uint64_t)o[1] | (uint64_t)o[2] << 32);
        else return R_BAD_CODE;
        is32 = rc == 1u;
        if (is32) {                          /* directly: through double could round twice */
            float f = (float)i;
            uint32_t u;
            memcpy(&u, &f, sizeof u);
            r = u;
        } else {
            double d = (double)i;
            memcpy(&r, &d, sizeof r);
        }
        goto box;
    }
    if (!p) return R_BAD_CODE;
    is32 = hdr_words(p[0]) == 2u;
    x = raw(p);
    if (op == OP_NEGF) {                     /* the sign bit, as Go's negation (NaN too) */
        r = x ^ SIGN(is32);
        goto box;
    }
    if (op == OP_CONVF) {
        if (rc == 2u || rc == 3u) {          /* int (c=2) / int64 (c=3) of a float */
            int64_t i = ftrunc(is32 ? widen((uint32_t)x) : x, rc == 3u);
            if (i >= TERM_INT_MIN && i <= TERM_INT_MAX) { R[ra] = term_from_int((int32_t)i); return R_NORMAL; }
            if (!heap_box(vm, a, rc == 2u ? K_INT : K_INT64, rc == 2u ? 2u : 3u, (uint64_t)i, &t)) return R_OUT_OF_MEMORY;
            goto store;
        }
        if (rc != 4u && rc != 5u) return R_BAD_CODE;   /* float64 (c=4) / float32 (c=5) of a float */
        if ((rc == 5u) == is32) { R[ra] = R[rb]; return R_NORMAL; }
        if (is32) r = widen((uint32_t)x);
        else {
            double d;
            memcpy(&d, &x, sizeof d);
            float f = (float)d;
            uint32_t u;
            memcpy(&u, &f, sizeof u);
            r = u;
        }
        is32 = !is32;
        goto box;
    }
    const uint32_t *q = float_obj(vm, a, R[rc]);
    if (!q || q[0] != p[0]) return R_BAD_CODE;           /* both of one width */
    uint64_t y = raw(q);
    if (op >= OP_EQF) {
        int c = fcmp(x, y, is32);
        bool b;
        /* bit c + 1 (c: -1, 0, 1, 2 for NaN) of the nibble of EQF, NEF,
         * LTF, LEF (57..60, consecutive in the image format): 2, D, 1, 3 */
        b = 0x31D2u >> ((op - OP_EQF) * 4u + (uint32_t)(c + 1)) & 1u;
        R[ra] = term_from_bool(b);
        return R_NORMAL;
    }
    if (is32) {                              /* single precision: the ESP32-S3's FPU */
        float fx, fy, fr;
        uint32_t ux = (uint32_t)x, uy = (uint32_t)y, ur;
        memcpy(&fx, &ux, sizeof fx);
        memcpy(&fy, &uy, sizeof fy);
        switch (op) {
        case OP_ADDF: fr = fx + fy; break;
        case OP_SUBF: fr = fx - fy; break;
        case OP_MULF: fr = fx * fy; break;
        default:      fr = fx / fy; break;
        }
        memcpy(&ur, &fr, sizeof ur);
        r = ur;
    } else {
        double dx, dy, dr;
        memcpy(&dx, &x, sizeof dx);
        memcpy(&dy, &y, sizeof dy);
        switch (op) {
        case OP_ADDF: dr = dx + dy; break;
        case OP_SUBF: dr = dx - dy; break;
        case OP_MULF: dr = dx * dy; break;
        default:      dr = dx / dy; break;
        }
        memcpy(&r, &dr, sizeof r);
    }
box:
    if (!heap_box(vm, a, K_FLOAT, is32 ? 2u : 3u, r, &t)) return R_OUT_OF_MEMORY;
store:
    ((term *)a->mem + a->fp)[ra] = t;
    return R_NORMAL;
}

int float_order(const uint32_t *x, const uint32_t *y) {
    return fcmp(raw(x), raw(y), hdr_words(x[0]) == 2u);
}

#else
typedef int gogo_no_float;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
