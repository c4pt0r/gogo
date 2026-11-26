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

/* A new heap object [hdr kind][lo] (words 2) or [hdr kind][lo][hi]
 * (words 3) into *out. May GC; false on OOM. */
static bool box_raw(Vm *vm, Actor *a, uint32_t kind, uint32_t words, uint64_t v, term *out) {
    uint32_t off = heap_alloc(vm, a, words, NULL, 0);
    if (off == UINT32_MAX) return false;
    a->mem[off] = hdr_make(words, kind);
    a->mem[off + 1] = (uint32_t)v;
    if (words == 3u) a->mem[off + 2] = (uint32_t)(v >> 32);
    *out = term_from_ref(off);
    return true;
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
 * -1, 0 or 1, and 2 if either is NaN; -0 equals 0. */
static int fcmp(uint64_t x, uint64_t y, bool is32) {
    uint64_t mag = MAG(is32);
    if ((x & mag) > INF(is32) || (y & mag) > INF(is32)) return 2;
    if (!((x | y) & mag)) return 0;
    int64_t p = x & SIGN(is32) ? -(int64_t)(x & mag) : (int64_t)x;   /* sign-magnitude to an order */
    int64_t q = y & SIGN(is32) ? -(int64_t)(y & mag) : (int64_t)y;
    return (p > q) - (p < q);
}

/* The float with raw bits u truncated toward zero to an int (32 bits) or,
 * with to64, an int64, on the bits: NaN gives 0 and a value beyond the
 * range the nearest end (C leaves both undefined; Go on arm64 does this). */
static int64_t ftrunc(uint64_t u, bool is32, bool to64) {
    int mbits = is32 ? 23 : 52, lim = to64 ? 63 : 31;
    uint32_t hi = is32 ? (uint32_t)u << 1 : (uint32_t)(u >> 32) << 1;   /* exponent at the top */
    int e = (int)(hi >> (is32 ? 24 : 21)) - (is32 ? 127 : 1023);
    uint64_t m = (u & (is32 ? 0x7FFFFFull : 0xFFFFFFFFFFFFFull)) | (is32 ? 0x800000ull : 0x10000000000000ull);
    bool neg = (u & SIGN(is32)) != 0;
    if ((u & MAG(is32)) > INF(is32) || e < 0) return 0;      /* NaN, |x| < 1 */
    if (e >= lim) return neg ? (to64 ? INT64_MIN : INT32_MIN) : (to64 ? INT64_MAX : INT32_MAX);
    uint64_t v = e >= mbits ? m << (e - mbits) : m >> (mbits - e);
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
            int64_t i = ftrunc(x, is32, rc == 3u);
            if (i >= TERM_INT_MIN && i <= TERM_INT_MAX) { R[ra] = term_from_int((int32_t)i); return R_NORMAL; }
            if (!box_raw(vm, a, rc == 2u ? K_INT : K_INT64, rc == 2u ? 2u : 3u, (uint64_t)i, &t)) return R_OUT_OF_MEMORY;
            goto store;
        }
        if (rc != 4u && rc != 5u) return R_BAD_CODE;   /* float64 (c=4) / float32 (c=5) of a float */
        if ((rc == 5u) == is32) { R[ra] = R[rb]; return R_NORMAL; }
        if (is32) {
            float f;
            uint32_t u = (uint32_t)x;
            memcpy(&f, &u, sizeof f);
            double d = f;
            memcpy(&r, &d, sizeof r);
        } else {
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
        switch (op) {
        case OP_EQF: b = c == 0; break;
        case OP_NEF: b = c != 0; break;
        case OP_LTF: b = c < 0; break;
        default:     b = c <= 0; break;      /* OP_LEF */
        }
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
    if (!box_raw(vm, a, K_FLOAT, is32 ? 2u : 3u, r, &t)) return R_OUT_OF_MEMORY;
store:
    ((term *)a->mem + a->fp)[ra] = t;
    return R_NORMAL;
}

#else
typedef int gogo_no_float;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
