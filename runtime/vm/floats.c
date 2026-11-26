/* Floating point: values, ops, and float -> decimal (floats.h). */
#include "floats.h"

#if GOGO_FLOAT

#include <string.h>
#include "heap.h"
#include "opcodes.h"

/* ---- values ---- */

static uint64_t f64_bits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* The value of K_FLOAT object o (2 or 3 words), widened to double. */
static double obj_float(const uint32_t *o, bool *is32) {
    double d;
    *is32 = hdr_words(o[0]) == 2u;
    if (*is32) {
        float f;
        memcpy(&f, &o[1], sizeof f);
        return f;
    }
    uint64_t u = (uint64_t)o[1] | (uint64_t)o[2] << 32;
    memcpy(&d, &u, sizeof d);
    return d;
}

bool float_val(const Vm *vm, const Actor *a, term t, double *d, bool *is32) {
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_FLOAT || (hdr_words(o[0]) != 2u && hdr_words(o[0]) != 3u)) return false;
    *d = obj_float(o, is32);
    return true;
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

bool float_box(Vm *vm, Actor *a, double d, bool is32, term *out) {
    return box_raw(vm, a, K_FLOAT, is32 ? 2u : 3u, is32 ? f32_bits((float)d) : f64_bits(d), out);
}

uint32_t float_type(const uint32_t *o) {
    return hdr_words(o[0]) == 2u ? TY_FLOAT32 : TY_FLOAT64;
}

/* ---- ops ---- */

/* x op y in single precision: the ESP32-S3 has a float32 FPU, so this
 * path stays in hardware there. */
static float arith_f32(uint32_t op, float x, float y) {
    switch (op) {
    case OP_ADDF: return x + y;
    case OP_SUBF: return x - y;
    case OP_MULF: return x * y;
    default:      return x / y;
    }
}

static double arith_f64(uint32_t op, double x, double y) {
    switch (op) {
    case OP_ADDF: return x + y;
    case OP_SUBF: return x - y;
    case OP_MULF: return x * y;
    default:      return x / y;
    }
}

/* x compared with y in IEEE order on their bits (no soft-float calls):
 * -1, 0 or 1, and 2 if either is NaN; -0 equals 0. Exact for widened
 * float32 values too. */
static int fcmp(double x, double y) {
    const uint64_t mag = 0x7FFFFFFFFFFFFFFFull, inf = 0x7FF0000000000000ull;
    uint64_t u = f64_bits(x), v = f64_bits(y);
    if ((u & mag) > inf || (v & mag) > inf) return 2;
    if (!((u | v) & mag)) return 0;
    int64_t p = u >> 63 ? -(int64_t)(u & mag) : (int64_t)u;   /* sign-magnitude to an order */
    int64_t q = v >> 63 ? -(int64_t)(v & mag) : (int64_t)v;
    return (p > q) - (p < q);
}

int64_t float_trunc(double d, bool to64) {
    /* NaN gives 0 and a value beyond the range the nearest end (C leaves
     * both undefined), as Go on arm64. */
    double lim = to64 ? 9223372036854775808.0 : 2147483648.0;
    if (d != d) return 0;
    if (d >= lim) return to64 ? INT64_MAX : INT32_MAX;
    if (d <= -lim) return to64 ? INT64_MIN : INT32_MIN;   /* -lim itself is the minimum */
    return (int64_t)d;
}

uint32_t float_exec(Vm *vm, Actor *a, uint32_t w, uint32_t nregs) {
    uint32_t op = OP_OP(w), ra = OP_A(w), rb = OP_B(w), rc = OP_C(w);
    term *R = (term *)a->mem + a->fp;
    if (ra >= nregs || rb >= nregs || (op != OP_NEGF && op != OP_CONVF && rc >= nregs)) return R_BAD_CODE;
    double x, y, r;
    bool is32, y32, b;
    term t;
    if (op >= OP_ADDF && op <= OP_DIVF) {
        /* float32 arithmetic in single precision without going through
         * double (each conversion is a software call on the ESP32-S3). */
        const uint32_t *p = obj_of(vm, a, R[rb]), *q = obj_of(vm, a, R[rc]);
        if (p && q && p[0] == hdr_make(2, K_FLOAT) && q[0] == p[0]) {
            float fx, fy, fr;
            memcpy(&fx, &p[1], sizeof fx);
            memcpy(&fy, &q[1], sizeof fy);
            fr = arith_f32(op, fx, fy);
            if (!box_raw(vm, a, K_FLOAT, 2u, f32_bits(fr), &t)) return R_OUT_OF_MEMORY;
            goto store;
        }
    }
    if (op == OP_CONVF && rc <= 1u) {        /* float64 (c=0) / float32 (c=1) of an int or int64 */
        int32_t v;
        int64_t i;
        const uint32_t *o = obj_of(vm, a, R[rb]);
        if (int_val(vm, a, R[rb], &v)) i = v;
        else if (o && hdr_kind(o[0]) == K_INT64 && hdr_words(o[0]) == 3u) i = (int64_t)((uint64_t)o[1] | (uint64_t)o[2] << 32);
        else return R_BAD_CODE;
        is32 = rc == 1u;
        r = is32 ? (double)(float)i : (double)i;   /* to float32 directly: no double rounding */
        goto box;
    }
    if (!float_val(vm, a, R[rb], &x, &is32)) return R_BAD_CODE;
    if (op == OP_CONVF) {
        if (rc == 2u || rc == 3u) {          /* int (c=2) / int64 (c=3) of a float */
            int64_t i = float_trunc(x, rc == 3u);
            if (i >= TERM_INT_MIN && i <= TERM_INT_MAX) { R[ra] = term_from_int((int32_t)i); return R_NORMAL; }
            if (!box_raw(vm, a, rc == 2u ? K_INT : K_INT64, rc == 2u ? 2u : 3u, (uint64_t)i, &t)) return R_OUT_OF_MEMORY;
            goto store;
        }
        if (rc != 4u && rc != 5u) return R_BAD_CODE;   /* float64 (c=4) / float32 (c=5) of a float */
        if ((rc == 5u) == is32) { R[ra] = R[rb]; return R_NORMAL; }
        r = x;
        is32 = !is32;
    } else if (op == OP_NEGF) {
        r = -x;
    } else {
        if (!float_val(vm, a, R[rc], &y, &y32) || y32 != is32) return R_BAD_CODE;
        if (op < OP_EQF) {
            r = arith_f64(op, x, y);         /* float32 operands took the path above */
            goto box;
        }
        int c = fcmp(x, y);
        switch (op) {
        case OP_EQF: b = c == 0; break;
        case OP_NEF: b = c != 0; break;
        case OP_LTF: b = c < 0; break;
        default:     b = c <= 0; break;      /* OP_LEF */
        }
        R[ra] = term_from_bool(b);
        return R_NORMAL;
    }
box:
    if (!float_box(vm, a, r, is32, &t)) return R_OUT_OF_MEMORY;
store:
    ((term *)a->mem + a->fp)[ra] = t;
    return R_NORMAL;
}

/* ---- float -> decimal ---- */

/* Unsigned big integers, little-endian 32-bit limbs. The largest value the
 * conversion holds is below 10 x 2^1076 (a float64's scaled denominator
 * times 10), 1080 bits: 36 limbs leave room. */
#define BN_LIMBS 36
typedef struct Bn {
    uint32_t n;                 /* limbs in use; w[n..] are not read */
    uint32_t w[BN_LIMBS];
} Bn;

static void bn_set(Bn *a, uint64_t v) {
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
    a->n = a->w[1] ? 2u : a->w[0] ? 1u : 0u;
}

static void bn_mul(Bn *a, uint32_t m) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        carry += (uint64_t)a->w[i] * m;
        a->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry && a->n < BN_LIMBS) a->w[a->n++] = (uint32_t)carry;
}

/* a *= 10^k */
static void bn_mul_pow10(Bn *a, int k) {
    for (; k >= 9; k -= 9) bn_mul(a, 1000000000u);
    static const uint32_t p10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (k > 0) bn_mul(a, p10[k]);
}

static void bn_shl(Bn *a, uint32_t k) {
    uint32_t limbs = k / 32u, bits = k % 32u;
    if (!a->n) return;
    uint32_t n = a->n + limbs + 1u;
    if (n > BN_LIMBS) n = BN_LIMBS;
    for (uint32_t i = n; i-- > 0;) {
        uint32_t hi = i >= limbs && i - limbs < a->n ? a->w[i - limbs] : 0u;
        uint32_t lo = i >= limbs + 1u && i - limbs - 1u < a->n ? a->w[i - limbs - 1u] : 0u;
        a->w[i] = bits ? hi << bits | lo >> (32u - bits) : hi;
    }
    a->n = n;
    while (a->n && !a->w[a->n - 1u]) a->n--;
}

static int bn_cmp(const Bn *a, const Bn *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (uint32_t i = a->n; i-- > 0;)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* d = a + b */
static void bn_add(Bn *d, const Bn *a, const Bn *b) {
    const Bn *l = a->n >= b->n ? a : b, *s = l == a ? b : a;
    uint64_t c = 0;
    uint32_t i;
    for (i = 0; i < l->n; i++) {
        c += (uint64_t)l->w[i] + (i < s->n ? s->w[i] : 0u);
        d->w[i] = (uint32_t)c;
        c >>= 32;
    }
    d->n = l->n;
    if (c && d->n < BN_LIMBS) d->w[d->n++] = (uint32_t)c;
}

/* a -= b (a >= b) */
static void bn_sub(Bn *a, const Bn *b) {
    int64_t c = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        c += (int64_t)a->w[i] - (i < b->n ? (int64_t)b->w[i] : 0);
        a->w[i] = (uint32_t)c;
        c = c < 0 ? -1 : 0;
    }
    while (a->n && !a->w[a->n - 1u]) a->n--;
}

/* The next digit: r = 10 r, then the quotient r / s (0..9; r < 10 s) and r
 * keeps the remainder. */
static int bn_digit(Bn *r, const Bn *s) {
    int d = 0;
    bn_mul(r, 10);
    while (bn_cmp(r, s) >= 0) {
        bn_sub(r, s);
        d++;
    }
    return d;
}

/* 2r compared with s: the remainder's distance to half a unit. */
static int bn_half(const Bn *r, const Bn *s) {
    Bn t;
    bn_add(&t, r, r);
    return bn_cmp(&t, s);
}

int float_digits(double v, bool is32, int mode, int n, char *buf, int cap, int *dp) {
    uint64_t f;
    int e, p, minexp;
    if (is32) {
        uint32_t u = f32_bits((float)v);
        uint32_t be = u >> 23 & 0xFFu;
        f = u & 0x7FFFFFu;
        p = 24;
        minexp = -149;
        if (be) f |= 1u << 23;
        e = be ? (int)be - 150 : minexp;
    } else {
        uint64_t u = f64_bits(v);
        uint32_t be = (uint32_t)(u >> 52) & 0x7FFu;
        f = u & 0xFFFFFFFFFFFFFull;
        p = 53;
        minexp = -1074;
        if (be) f |= 1ull << 52;
        e = be ? (int)be - 1075 : minexp;
    }
    /* v = f x 2^e. r / s is v scaled; mp and mm are half the gaps to the
     * neighbours above and below, on the same scale (Burger & Dybvig). The
     * gap below is half as wide at a power of two. */
    int unequal = f == 1ull << (p - 1) && e > minexp;
    Bn r, s, mp, mm;
    if (e >= 0) {
        bn_set(&r, f);
        bn_shl(&r, (uint32_t)(e + 1 + unequal));
        bn_set(&s, 2u << unequal);
        bn_set(&mp, 1);
        bn_shl(&mp, (uint32_t)(e + unequal));
        bn_set(&mm, 1);
        bn_shl(&mm, (uint32_t)e);
    } else {
        bn_set(&r, f << (1 + unequal));
        bn_set(&s, 1);
        bn_shl(&s, (uint32_t)(1 - e + unequal));
        bn_set(&mp, 1u << unequal);
        bn_set(&mm, 1);
    }
    /* k: v < 10^k. Start from floor(log10(2^floor(log2 v))) (1233/4096 is
     * just under log10 2), never above the true k, and correct upward. */
    int bits = 0;
    for (uint64_t t = f; t; t >>= 1) bits++;
    int e2 = e + bits - 1;
    int k = e2 >= 0 ? e2 * 1233 / 4096 : -((-e2 * 1233 + 4095) / 4096);
    if (k >= 0) bn_mul_pow10(&s, k);
    else {
        bn_mul_pow10(&r, -k);
        bn_mul_pow10(&mp, -k);
        bn_mul_pow10(&mm, -k);
    }
    /* Shortest: an even mantissa reads back from a boundary too (round
     * half to even), so the boundaries are inclusive then. */
    int even = (f & 1u) == 0;
    for (;;) {
        Bn t;
        int c;
        if (mode == FD_SHORTEST) {
            bn_add(&t, &r, &mp);
            c = bn_cmp(&t, &s);
            if (c < 0 || (c == 0 && !even)) break;
        } else if (bn_cmp(&r, &s) < 0) {
            break;
        }
        bn_mul(&s, 10);
        k++;
    }
    *dp = k;
    int nd = 0;
    if (mode == FD_SHORTEST) {
        for (;;) {
            Bn t;
            int d = bn_digit(&r, &s);
            bn_mul(&mp, 10);
            bn_mul(&mm, 10);
            int c1 = bn_cmp(&r, &mm);
            bn_add(&t, &r, &mp);
            int c2 = bn_cmp(&t, &s);
            int low = c1 < 0 || (c1 == 0 && even);
            int high = c2 > 0 || (c2 == 0 && even);
            if (!low && !high && nd < cap - 1) {
                buf[nd++] = (char)('0' + d);
                continue;
            }
            if (low && high) {
                int h = bn_half(&r, &s);
                if (h > 0 || (h == 0 && (d & 1))) d++;
            } else if (high) {
                d++;
            }
            buf[nd++] = (char)('0' + d);
            return nd;
        }
    }
    int want = mode == FD_FRAC ? k + n : n;
    if (want > cap) want = cap;
    if (want < 0) return 0;
    while (nd < want && r.n) buf[nd++] = (char)('0' + bn_digit(&r, &s));
    /* Round what is left (r / s of a unit of the last digit) half to even;
     * with no digit at all, against 0. */
    if (r.n) {
        int h = bn_half(&r, &s);
        if (h > 0 || (h == 0 && nd && ((buf[nd - 1] - '0') & 1))) {
            int i = nd - 1;
            while (i >= 0 && buf[i] == '9') i--;
            if (i < 0) {
                buf[0] = '1';
                nd = 1;
                *dp = k + 1;
            } else {
                buf[i]++;
                nd = i + 1;
            }
        }
    }
    while (nd && buf[nd - 1] == '0') nd--;
    return nd;
}

void out_float(Out *o, const uint32_t *obj) {
    char d[24], t[32];
    bool is32;
    double v = obj_float(obj, &is32);
    if (v != v) { out_str(o, "NaN"); return; }
    bool neg = f64_bits(v) >> 63 != 0;
    if (neg) v = -v;
    if (v - v != 0) { out_str(o, neg ? "-Inf" : "+Inf"); return; }   /* inf - inf is NaN */
    /* Go's FormatFloat(v, 'g', -1): the shortest digits, as %e when the
     * exponent is < -4 or >= 6 (d.ddde+XX), else as %f. */
    int dp = 1, nd = 0, n = 0;
    if (v != 0) nd = float_digits(v, is32, FD_SHORTEST, 0, d, (int)sizeof d, &dp);
    if (neg) t[n++] = '-';
    int exp = dp - 1;
    if (exp < -4 || exp >= 6) {
        t[n++] = d[0];
        if (nd > 1) t[n++] = '.';
        for (int i = 1; i < nd; i++) t[n++] = d[i];
        t[n++] = 'e';
        t[n++] = exp < 0 ? '-' : '+';
        if (exp < 0) exp = -exp;
        if (exp >= 100) t[n++] = (char)('0' + exp / 100);
        t[n++] = (char)('0' + exp / 10 % 10);
        t[n++] = (char)('0' + exp % 10);
    } else {
        int i = 0;
        if (dp <= 0) {                            /* 0.000ddd */
            t[n++] = '0';
            t[n++] = '.';
            for (i = dp; i < 0; i++) t[n++] = '0';
        }
        for (; i < dp || i < nd; i++) {           /* ddd00 or dd.ddd */
            if (i == dp && dp > 0) t[n++] = '.';
            t[n++] = i < nd ? d[i] : '0';
        }
    }
    out_bytes(o, t, (size_t)n);
}

#else
typedef int gogo_no_float;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
