/* The float library: B_FLOAT (floatlib.h). */
#include "floats.h"

#if GOGO_FLOAT

#include <stdlib.h>
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

/* ---- strconv ---- */

/* Appends n copies of c (dst NULL: counts only). */
static uint32_t put(char *dst, uint32_t at, char c, int n) {
    for (; n > 0; n--, at++)
        if (dst) dst[at] = c;
    return at;
}

/* Go's strconv fmtEFG: lays out digits s[0..nd) (0.s x 10^dp) as fmt
 * 'e', 'E', 'f', 'g' or 'G' with precision prec (shortest: the digits are
 * the shortest form), '-' first if neg. Writes to dst unless it is NULL;
 * returns the length either way. */
static uint32_t efg(char *dst, bool neg, const char *s, int dp, int nd, int prec, char fmt, bool shortest) {
    uint32_t at = 0;
    if (fmt == 'g' || fmt == 'G') {
        int eprec = prec;
        if (eprec > nd && nd >= dp) eprec = nd;
        if (shortest) eprec = 6;       /* %e is used if the exponent is < -4 or >= 6 */
        int exp = dp - 1;
        if (exp < -4 || exp >= eprec) {
            if (prec > nd) prec = nd;
            prec--;
            fmt = (char)(fmt + 'e' - 'g');
        } else {
            if (prec > dp) prec = nd;
            prec = prec - dp > 0 ? prec - dp : 0;
            fmt = 'f';
        }
    }
    if (neg) at = put(dst, at, '-', 1);
    if (fmt == 'e' || fmt == 'E') {
        at = put(dst, at, nd ? s[0] : '0', 1);
        if (prec > 0) {
            at = put(dst, at, '.', 1);
            int i;
            for (i = 1; i < nd && i <= prec; i++) at = put(dst, at, s[i], 1);
            at = put(dst, at, '0', prec + 1 - i);
        }
        at = put(dst, at, fmt, 1);
        int exp = nd ? dp - 1 : 0;
        at = put(dst, at, exp < 0 ? '-' : '+', 1);
        if (exp < 0) exp = -exp;
        if (exp >= 100) at = put(dst, at, (char)('0' + exp / 100), 1);
        at = put(dst, at, (char)('0' + exp / 10 % 10), 1);
        return put(dst, at, (char)('0' + exp % 10), 1);
    }
    if (fmt != 'f') {
        at = put(dst, at, '%', 1);
        return put(dst, at, fmt, 1);
    }
    if (dp > 0) {
        for (int i = 0; i < dp; i++) at = put(dst, at, i < nd ? s[i] : '0', 1);
    } else {
        at = put(dst, at, '0', 1);
    }
    if (prec > 0) {
        at = put(dst, at, '.', 1);
        for (int i = 0; i < prec; i++) {
            int j = dp + i;               /* digit index of this place */
            at = put(dst, at, j >= 0 && j < nd ? s[j] : '0', 1);
        }
    }
    return at;
}


/* The digit buffer: a float64 has at most 767 significant digits. */
#define DIGITS_CAP 800

uint32_t floatlib_format(char *dst, double v, bool is32, char fmt, int prec) {
    char digits[DIGITS_CAP];
    const char *special = NULL;
    if (is32) v = (float)v;
    bool neg = (bits_of(v) & SIGN_BIT) != 0;
    if (v != v) special = "NaN";
    else if (v - v != 0) special = neg ? "-Inf" : "+Inf";
    if (special) {
        uint32_t n = (uint32_t)strlen(special);
        if (dst) memcpy(dst, special, n);
        return n;
    }
    if (neg) v = -v;
    if (fmt == 'b') {   /* mantissa "p" exponent, both decimal */
        uint64_t u = bits_of(v), m;
        int e;
        if (is32) {
            float f = (float)v;
            uint32_t w;
            memcpy(&w, &f, sizeof w);
            m = w & 0x7FFFFFu;
            e = (int)(w >> 23);
            if (e) m |= 1u << 23;
            e = (e ? e : 1) - 150;
        } else {
            m = u & 0xFFFFFFFFFFFFFull;
            e = (int)(u >> 52);
            if (e) m |= 1ull << 52;
            e = (e ? e : 1) - 1075;
        }
        char t[48];
        int n = 0, k = 0;
        if (neg) t[n++] = '-';
        char d[24];
        do { d[k++] = (char)('0' + m % 10u); m /= 10u; } while (m);
        while (k) t[n++] = d[--k];
        t[n++] = 'p';
        t[n++] = e < 0 ? '-' : '+';
        unsigned ue = (unsigned)(e < 0 ? -e : e);
        do { d[k++] = (char)('0' + ue % 10u); ue /= 10u; } while (ue);
        while (k) t[n++] = d[--k];
        if (dst) memcpy(dst, t, (size_t)n);
        return (uint32_t)n;
    }
    int dp = 0, nd = 0;
    if (v == 0) return efg(dst, neg, digits, 0, 0, prec, fmt, prec < 0);
    if (prec < 0) {
        nd = float_digits(v, is32, FD_SHORTEST, 0, digits, DIGITS_CAP, &dp);
        switch (fmt) {
        case 'e': case 'E': prec = nd - 1; break;
        case 'f': prec = nd - dp > 0 ? nd - dp : 0; break;
        case 'g': case 'G': prec = nd; break;
        }
        return efg(dst, neg, digits, dp, nd, prec, fmt, true);
    }
    switch (fmt) {
    case 'e': case 'E':
        nd = float_digits(v, is32, FD_DIGITS, prec + 1, digits, DIGITS_CAP, &dp);
        break;
    case 'f':
        nd = float_digits(v, is32, FD_FRAC, prec, digits, DIGITS_CAP, &dp);
        break;
    case 'g': case 'G':
        if (prec == 0) prec = 1;
        nd = float_digits(v, is32, FD_DIGITS, prec, digits, DIGITS_CAP, &dp);
        break;
    }
    return efg(dst, neg, digits, dp, nd, prec, fmt, false);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Go's ParseFloat syntax, without underscores: [+-] then decimal digits
 * with an optional point and e exponent, 0x hex digits with an optional
 * point and a required p exponent, or inf / infinity (any case); "nan"
 * (any case) without a sign. */
static bool float_syntax(const uint8_t *s, uint32_t n) {
    uint32_t i = 0, digits = 0;
    if (n == 3 && lower(s[0]) == 'n' && lower(s[1]) == 'a' && lower(s[2]) == 'n') return true;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (n - i == 3 || n - i == 8) {
        static const char inf[] = "infinity";
        uint32_t j = 0;
        while (j < n - i && lower(s[i + j]) == inf[j]) j++;
        if (j == n - i) return true;
    }
    bool hex = n - i > 2 && s[i] == '0' && lower(s[i + 1]) == 'x';
    if (hex) i += 2;
    bool point = false;
    for (; i < n; i++) {
        int c = lower(s[i]);
        if (c == '.' && !point) point = true;
        else if ((c >= '0' && c <= '9') || (hex && c >= 'a' && c <= 'f')) digits++;
        else break;
    }
    if (!digits) return false;
    if (i == n) return !hex;
    if (lower(s[i]) != (hex ? 'p' : 'e')) return false;
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (i == n) return false;
    for (; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* The longest string ParseFloat reads (longer ones give ok = false). */
#define PARSE_MAX 512u

static uint32_t parse_float(const Vm *vm, const Actor *a, term st, int32_t bits, double *out, bool *ok) {
    const uint8_t *p;
    uint32_t n;
    char buf[PARSE_MAX + 1u];
    if (!name_bytes(vm, a, st, &p, &n) || (bits != 32 && bits != 64)) return R_BAD_CODE;
    *out = 0;
    *ok = false;
    if (n == 0 || n > PARSE_MAX || !float_syntax(p, n)) return R_NORMAL;
    memcpy(buf, p, n);
    buf[n] = 0;
    double d = bits == 32 ? (double)strtof(buf, NULL) : strtod(buf, NULL);
    /* Overflow gives +-Inf and false, as Go's ErrRange; underflow is fine. */
    bool inf = d - d != 0 && d == d;
    *ok = !inf || lower(buf[n - 1]) == 'f' || lower(buf[n - 1]) == 'y';
    *out = d;
    return R_NORMAL;
}

/* ---- B_FLOAT ---- */

uint32_t float_lib(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs) {
    term *R = (term *)a->mem + a->fp;
    int32_t op, i, prec, bitsz;
    double x = 0, y = 0, r;
    bool is32, b;
    term t;
    if (rc < 1u || !int_val(vm, a, R[rb], &op)) return R_BAD_CODE;
    const term *arg = R + rb + 1;
    uint32_t nargs = rc - 1u;
    /* Ops on float64 arguments first: read them all. */
    static const uint8_t nf[] = {1, 1, 1, 1, 1, 1, 1, 2, 1, 0, 0, 1, 1, 2, 2, 1, 0};
    if (op < 0 || (uint32_t)op >= sizeof nf || nargs < nf[op]) return R_BAD_CODE;
    if (nf[op] >= 1 && (!float_val(vm, a, arg[0], &x, &is32) || is32)) return R_BAD_CODE;
    if (nf[op] >= 2 && (!float_val(vm, a, arg[1], &y, &is32) || is32)) return R_BAD_CODE;
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
        if (nargs != 2u || ra + 1u >= nregs || !int_val(vm, a, arg[1], &bitsz)) return R_BAD_CODE;
        uint32_t why = parse_float(vm, a, arg[0], bitsz, &r, &b);
        if (why != R_NORMAL) return why;
        if (!float_box(vm, a, r, false, &t)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        R[ra] = t;
        R[ra + 1] = term_from_bool(b);
        return R_NORMAL;
    }
    default:
        return R_BAD_CODE;
    }
    if (nf[op] != nargs && op != FLOAT_OP_INF) return R_BAD_CODE;
    if (!float_box(vm, a, r, false, &t)) return R_OUT_OF_MEMORY;
    ((term *)a->mem + a->fp)[ra] = t;
    return R_NORMAL;
}

#else
typedef int gogo_no_float;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
