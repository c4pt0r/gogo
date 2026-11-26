/* Exact conversions between floats and decimal text (floatlib.h): the
 * digits of a float (shortest, or correctly rounded to a precision), Go's
 * %e / %f / %g layout, and parsing, all on one small bignum. No libc
 * printf, strtod or libm. */
#include "floats.h"

#if GOGO_FLOAT

#include <string.h>
#include "floatlib.h"

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

/* ---- big integers ---- */

/* Unsigned big integers, little-endian 32-bit limbs in caller storage (cap
 * limbs). The callers size them so that nothing overflows (see each). */
typedef struct Bn {
    uint32_t n, cap;            /* limbs in use (w[n..] are not read), limbs there are */
    uint32_t *w;
} Bn;

#define BN(name, limbs) uint32_t name##_w[limbs]; Bn name = {0, (limbs), name##_w}

static void bn_set(Bn *a, uint64_t v) {
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
    a->n = a->w[1] ? 2u : a->w[0] ? 1u : 0u;
}

/* a = a * m + c */
static void bn_mul_add(Bn *a, uint32_t m, uint32_t c) {
    uint64_t carry = c;
    for (uint32_t i = 0; i < a->n; i++) {
        carry += (uint64_t)a->w[i] * m;
        a->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry && a->n < a->cap) a->w[a->n++] = (uint32_t)carry;
    GOGO_ASSERT(!carry || a->w[a->n - 1u] == (uint32_t)carry);
}

/* a *= 10^k (k >= 0) */
static void bn_mul_pow10(Bn *a, int k) {
    static const uint32_t p10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    for (; k >= 9; k -= 9) bn_mul_add(a, 1000000000u, 0);
    if (k > 0) bn_mul_add(a, p10[k], 0);
}

static void bn_shl(Bn *a, uint32_t k) {
    uint32_t limbs = k / 32u, bits = k % 32u;
    if (!a->n) return;
    uint32_t n = a->n + limbs + 1u;
    GOGO_ASSERT(n <= a->cap || (n == a->cap + 1u && !(bits && a->w[a->n - 1u] >> (32u - bits))));
    if (n > a->cap) n = a->cap;
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

/* d = a + b (d may be a or b) */
static void bn_add(Bn *d, const Bn *a, const Bn *b) {
    const Bn *l = a->n >= b->n ? a : b, *s = l == a ? b : a;
    uint64_t c = 0;
    uint32_t i, n = l->n;
    for (i = 0; i < n; i++) {
        c += (uint64_t)l->w[i] + (i < s->n ? s->w[i] : 0u);
        d->w[i] = (uint32_t)c;
        c >>= 32;
    }
    d->n = n;
    if (c && d->n < d->cap) d->w[d->n++] = (uint32_t)c;
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

static uint32_t bn_bits(const Bn *a) {
    if (!a->n) return 0;
    uint32_t b = 32u * (a->n - 1u);
    for (uint32_t t = a->w[a->n - 1u]; t; t >>= 1) b++;
    return b;
}

/* 2r compared with s, without a temporary. */
static int bn_cmp2(const Bn *r, const Bn *s) {
    uint32_t n = r->n + 1u > s->n ? r->n + 1u : s->n;
    for (uint32_t i = n; i-- > 0;) {
        uint32_t hi = i < r->n ? r->w[i] : 0u, lo = i >= 1u && i - 1u < r->n ? r->w[i - 1u] : 0u;
        uint32_t x = hi << 1 | lo >> 31, y = i < s->n ? s->w[i] : 0u;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

/* The next digit: r = 10 r, then the quotient r / s (0..9; r < 10 s) and r
 * keeps the remainder. */
static int bn_digit(Bn *r, const Bn *s) {
    int d = 0;
    bn_mul_add(r, 10, 0);
    while (bn_cmp(r, s) >= 0) {
        bn_sub(r, s);
        d++;
    }
    return d;
}

/* ---- float -> digits ---- */

/* The format side's bignums: the largest value is below 10 x 2^1076 (a
 * float64's scaled denominator times 10), 1080 bits. */
#define FMT_LIMBS 36

int floatlib_digits(double v, bool is32, int mode, int n, char *buf, int cap, int *dp) {
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
    BN(r, FMT_LIMBS);
    BN(s, FMT_LIMBS);
    BN(mp, FMT_LIMBS);
    BN(mm, FMT_LIMBS);
    BN(t, FMT_LIMBS);
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
    for (uint64_t x = f; x; x >>= 1) bits++;
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
        int c;
        if (mode == FD_SHORTEST) {
            bn_add(&t, &r, &mp);
            c = bn_cmp(&t, &s);
            if (c < 0 || (c == 0 && !even)) break;
        } else if (bn_cmp(&r, &s) < 0) {
            break;
        }
        bn_mul_add(&s, 10, 0);
        k++;
    }
    *dp = k;
    int nd = 0;
    if (mode == FD_SHORTEST) {
        for (;;) {
            int d = bn_digit(&r, &s);
            bn_mul_add(&mp, 10, 0);
            bn_mul_add(&mm, 10, 0);
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
                int h = bn_cmp2(&r, &s);
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
        int h = bn_cmp2(&r, &s);
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

/* ---- layout ---- */

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

/* ---- FormatFloat ---- */

/* The digit buffer of a fixed precision: a float64 has at most 767
 * significant digits. */
#define DIGITS_CAP 800

/* FormatFloat with prec >= 0 (positive finite v): the 800-byte buffer
 * only on this path, so println and the shortest forms do not carry it. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint32_t format_fixed(char *dst, bool neg, double v, bool is32, char fmt, int prec) {
    char digits[DIGITS_CAP];
    int dp = 0, nd = 0;
    switch (fmt) {
    case 'e': case 'E':
        nd = floatlib_digits(v, is32, FD_DIGITS, prec + 1, digits, DIGITS_CAP, &dp);
        break;
    case 'f':
        nd = floatlib_digits(v, is32, FD_FRAC, prec, digits, DIGITS_CAP, &dp);
        break;
    case 'g': case 'G':
        if (prec == 0) prec = 1;
        nd = floatlib_digits(v, is32, FD_DIGITS, prec, digits, DIGITS_CAP, &dp);
        break;
    }
    return efg(dst, neg, digits, dp, nd, prec, fmt, false);
}

/* fmt 'b': the mantissa, "p", the binary exponent, both decimal. */
static uint32_t format_b(char *dst, bool neg, double v, bool is32) {
    uint64_t m;
    int e;
    if (is32) {
        uint32_t w = f32_bits((float)v);
        m = w & 0x7FFFFFu;
        e = (int)(w >> 23);
        if (e) m |= 1u << 23;
        e = (e ? e : 1) - 150;
    } else {
        uint64_t u = f64_bits(v);
        m = u & 0xFFFFFFFFFFFFFull;
        e = (int)(u >> 52);
        if (e) m |= 1ull << 52;
        e = (e ? e : 1) - 1075;
    }
    char t[48], d[24];
    int n = 0, k = 0;
    if (neg) t[n++] = '-';
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

uint32_t floatlib_format(char *dst, double v, bool is32, char fmt, int prec) {
    if (is32) v = (float)v;
    uint64_t u = f64_bits(v);
    bool neg = u >> 63 != 0;
    if ((u << 1) > 0xFFE0000000000000ull || (u << 1) == 0xFFE0000000000000ull) {   /* NaN, Inf */
        const char *t = (u << 1) > 0xFFE0000000000000ull ? "NaN" : neg ? "-Inf" : "+Inf";
        uint32_t n = (uint32_t)strlen(t);
        if (dst) memcpy(dst, t, n);
        return n;
    }
    if (neg) v = -v;
    if (fmt == 'b') return format_b(dst, neg, v, is32);
    if (v == 0) return efg(dst, neg, "", 0, 0, prec, fmt, prec < 0);
    if (prec >= 0) return format_fixed(dst, neg, v, is32, fmt, prec);
    char digits[24];
    int dp;
    int nd = floatlib_digits(v, is32, FD_SHORTEST, 0, digits, (int)sizeof digits, &dp);
    switch (fmt) {
    case 'e': case 'E': prec = nd - 1; break;
    case 'f': prec = nd - dp > 0 ? nd - dp : 0; break;
    case 'g': case 'G': prec = nd; break;
    }
    return efg(dst, neg, digits, dp, nd, prec, fmt, true);
}

/* ---- ParseFloat ---- */

/* The parse side's bignums. At most PARSE_DIGITS significant digits are
 * kept (more nonzero ones become one more digit 1, a sticky bit: a
 * float64 halfway point has at most 767 significant digits, so the
 * rounding is still exact), and a value is scaled only once it is known
 * to lie within about 10^-325 .. 10^310: the denominator is at most
 * 10^(801 + 325) shifted by under 2^4, 3745 bits. */
#define PARSE_DIGITS 800
#define PARSE_LIMBS 118

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool is_digit(int c, bool hex) {
    return (c >= '0' && c <= '9') || (hex && lower(c) >= 'a' && lower(c) <= 'f');
}

/* Go's underscoreOK: an underscore only between digits (a base prefix
 * counts as a digit). */
static bool underscore_ok(const uint8_t *s, uint32_t n) {
    int saw = '^';
    uint32_t i = 0;
    bool hex = false;
    if (n >= 1 && (s[0] == '-' || s[0] == '+')) { s++; n--; }
    if (n >= 2 && s[0] == '0' && (lower(s[1]) == 'b' || lower(s[1]) == 'o' || lower(s[1]) == 'x')) {
        i = 2;
        saw = '0';
        hex = lower(s[1]) == 'x';
    }
    for (; i < n; i++) {
        if (is_digit(s[i], hex)) { saw = '0'; continue; }
        if (s[i] == '_') {
            if (saw != '0') return false;
            saw = '_';
            continue;
        }
        if (saw == '_') return false;
        saw = '!';
    }
    return saw != '_';
}

/* Rounds r / s x 2^e2 (r, s > 0; e2 roughly the binary exponent) to the
 * nearest float32 or float64, ties to even, into *bits. False on
 * overflow (*bits is then Inf). r and s are scratch. */
static bool round_ratio(Bn *r, Bn *s, int e2, bool is32, uint64_t *bits) {
    int p = is32 ? 24 : 53, emin = is32 ? -126 : -1022, emax = is32 ? 127 : 1023;
    uint64_t inf = is32 ? 0x7F800000ull : 0x7FF0000000000000ull;
    int d = (int)bn_bits(r) - (int)bn_bits(s);  /* r / s in (2^(d-1), 2^(d+1)) */
    e2 += d;
    if (e2 > emax + 1) { *bits = inf; return false; }
    if (e2 < emin - p - 1) { *bits = 0; return true; }
    if (d >= 0) bn_shl(s, (uint32_t)d);
    else bn_shl(r, (uint32_t)-d);
    if (bn_cmp(r, s) < 0) { bn_shl(r, 1); e2--; }   /* now 1 <= r / s < 2 */
    if (e2 > emax) { *bits = inf; return false; }
    int nbits = e2 >= emin ? p : p - (emin - e2);   /* fewer for a subnormal */
    uint64_t m = 0;
    if (nbits == 0) {
        m = bn_cmp(r, s) > 0;                  /* in [half, 1) of the least subnormal: a tie goes to 0 */
    } else if (nbits > 0) {
        m = 1;
        bn_sub(r, s);
        for (int i = 1; i < nbits; i++) {
            bn_shl(r, 1);
            m <<= 1;
            if (bn_cmp(r, s) >= 0) { bn_sub(r, s); m |= 1u; }
        }
        int c = bn_cmp2(r, s);
        if (c > 0 || (c == 0 && (m & 1u))) m++;
    }
    if (e2 < emin) { *bits = m; return true; }  /* subnormal; a carry makes it the least normal */
    if (m >> p) { m >>= 1; e2++; }
    if (e2 > emax) { *bits = inf; return false; }
    *bits = (uint64_t)(e2 + (is32 ? 127 : 1023)) << (p - 1) | (m & ((1ull << (p - 1)) - 1u));
    return true;
}

int floatlib_parse(const uint8_t *s, uint32_t n, bool is32, double *out) {
    uint32_t i = 0;
    bool neg = false, under = false;
    uint64_t bits = 0;
    *out = 0;
    for (uint32_t j = 0; j < n; j++) under = under || s[j] == '_';
    if (under && !underscore_ok(s, n)) return FP_SYNTAX;
    if (n == 3 && lower(s[0]) == 'n' && lower(s[1]) == 'a' && lower(s[2]) == 'n') {
        bits = is32 ? 0x7FC00000u : 0x7FF8000000000001ull;
        goto done;
    }
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (n - i == 3 || n - i == 8) {
        static const char inf[] = "infinity";
        uint32_t j = 0;
        while (j < n - i && lower(s[i + j]) == inf[j]) j++;
        if (j == n - i) {
            bits = is32 ? 0x7F800000u : 0x7FF0000000000000ull;
            goto done;
        }
    }
    bool hex = n - i > 2 && s[i] == '0' && lower(s[i + 1]) == 'x';
    if (hex) i += 2;
    BN(r, PARSE_LIMBS);
    BN(q, PARSE_LIMBS);
    bn_set(&r, 0);
    bool point = false, any = false, sticky = false;
    int nd = 0;             /* significant digits kept in r */
    long dexp = 0;          /* value = r x base^dexp (hex: 2^dexp) */
    for (; i < n; i++) {
        int c = s[i];
        if (c == '_') continue;
        if (c == '.' && !point) { point = true; continue; }
        if (!is_digit(c, hex)) break;
        any = true;
        int dv = c <= '9' ? c - '0' : lower(c) - 'a' + 10;
        if (nd == 0 && dv == 0) {                /* a leading zero */
            if (point) dexp -= hex ? 4 : 1;
            continue;
        }
        if (nd < (hex ? 15 : PARSE_DIGITS)) {    /* hex: 60 bits are plenty */
            bn_mul_add(&r, hex ? 16u : 10u, (uint32_t)dv);
            nd++;
            if (point) dexp -= hex ? 4 : 1;
        } else {
            sticky = sticky || dv != 0;
            if (!point) dexp += hex ? 4 : 1;
        }
    }
    if (!any) return FP_SYNTAX;
    if (i < n || hex) {                           /* an exponent; hex needs one */
        if (i == n || lower(s[i]) != (hex ? 'p' : 'e')) return FP_SYNTAX;
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        if (i == n) return FP_SYNTAX;
        long x = 0;
        for (; i < n; i++) {
            if (s[i] == '_') continue;
            if (s[i] < '0' || s[i] > '9') return FP_SYNTAX;
            if (x < 100000) x = x * 10 + (s[i] - '0');   /* beyond, the value is 0 or Inf anyway */
        }
        dexp += eneg ? -x : x;
    }
    if (!r.n) goto done;                          /* zero */
    if (sticky) {                                 /* the digits dropped: one more, a 1 */
        bn_mul_add(&r, hex ? 16u : 10u, 1);
        dexp -= hex ? 4 : 1;
        nd++;
    }
    int e2 = 0;
    if (hex) {
        bn_set(&q, 1);
        e2 = (int)(dexp < -2000 ? -2000 : dexp > 2000 ? 2000 : dexp);
    } else {
        /* value = r x 10^dexp, 10^(nd + dexp - 1) <= value < 10^(nd + dexp) */
        long mag = nd + dexp;
        if (mag > (is32 ? 39 : 309)) { bits = is32 ? 0x7F800000u : 0x7FF0000000000000ull; goto range; }
        if (mag < (is32 ? -46 : -324)) goto done;  /* below half the least subnormal: 0 */
        bn_set(&q, 1);
        if (dexp > 0) bn_mul_pow10(&r, (int)dexp);
        else bn_mul_pow10(&q, (int)-dexp);
    }
    if (!round_ratio(&r, &q, e2, is32, &bits)) goto range;
done:
    if (is32) {
        uint32_t w = (uint32_t)bits | (neg ? 0x80000000u : 0u);
        float f;
        memcpy(&f, &w, sizeof f);
        *out = f;
    } else {
        bits |= neg ? 1ull << 63 : 0u;
        memcpy(out, &bits, sizeof *out);
    }
    return FP_OK;
range:
    bits |= neg ? (is32 ? 0x80000000ull : 1ull << 63) : 0u;
    if (is32) {
        uint32_t w = (uint32_t)bits;
        float f;
        memcpy(&f, &w, sizeof f);
        *out = f;
    } else {
        memcpy(out, &bits, sizeof *out);
    }
    return FP_RANGE;
}

#else
typedef int gogo_no_float_decimal;   /* ISO C: no empty translation unit */
#endif /* GOGO_FLOAT */
