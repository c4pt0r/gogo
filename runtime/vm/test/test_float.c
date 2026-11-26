/* Float -> decimal (floats.c, floatlib.c) against Go's strconv and println
 * over the corpus tools/floatcorpus generates (float_corpus.h), and the
 * K_FLOAT box. */
#include "test.h"
#include "floats.h"

#if GOGO_FLOAT

#include "out.h"
#include "actor.h"
#include "../../floatlib/floatlib.h"
#include <string.h>
#include "float_corpus.h"

static char cap_buf[128];
static size_t cap_n;

static void capture(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    CHECK_EQ(stream, 1);
    if (cap_n + n <= sizeof cap_buf) memcpy(cap_buf + cap_n, s, n);
    cap_n += n;
}
static const gogo_host cap_host = { .write = capture };
static Vm cv = { .host = &cap_host };

static double value_of(const struct FloatCase *c) {
    if (c->is32) {
        uint32_t u = (uint32_t)c->bits;
        float f;
        memcpy(&f, &u, sizeof f);
        return f;
    }
    double d;
    memcpy(&d, &c->bits, sizeof d);
    return d;
}

static void report(const struct FloatCase *c, const char *got, size_t n) {
    fprintf(stderr, "  bits %016llx is32 %d fmt %c prec %d: got \"%.*s\", want \"%s\"\n",
            (unsigned long long)c->bits, c->is32, c->fmt, c->prec, (int)n, got, c->want);
}

TEST(float_format_matches_go) {
    static char got[2048];
    size_t bad = 0, n = sizeof float_corpus / sizeof float_corpus[0];
    for (size_t i = 0; i < n; i++) {
        const struct FloatCase *c = &float_corpus[i];
        double v = value_of(c);
        uint32_t len = floatlib_format(NULL, v, c->is32, c->fmt, c->prec);
        CHECK(len < sizeof got);
        if (len >= sizeof got) continue;
        CHECK_EQ(floatlib_format(got, v, c->is32, c->fmt, c->prec), len);
        if (len != strlen(c->want) || memcmp(got, c->want, len) != 0) {
            if (bad++ < 10) report(c, got, len);
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(n > 1000);
}

/* println: out_float on the box, for every 'g' -1 entry. */
TEST(float_println_matches_go) {
    size_t bad = 0;
    for (size_t i = 0; i < sizeof float_corpus / sizeof float_corpus[0]; i++) {
        const struct FloatCase *c = &float_corpus[i];
        if (c->fmt != 'g' || c->prec != -1) continue;
        uint32_t obj[3];
        obj[0] = hdr_make(c->is32 ? 2u : 3u, K_FLOAT);
        obj[1] = (uint32_t)c->bits;
        obj[2] = (uint32_t)(c->bits >> 32);
        Out o;
        cap_n = 0;
        out_init(&o, &cv, 1);
        out_float(&o, obj);
        out_flush(&o);
        if (cap_n != strlen(c->want) || memcmp(cap_buf, c->want, cap_n) != 0) {
            if (bad++ < 10) report(c, cap_buf, cap_n);
        }
    }
    CHECK_EQ(bad, 0);
}

/* The digit engine's modes directly: shortest, n digits, n places. */
TEST(float_digits_modes) {
    char d[32];
    int dp, nd;
    nd = float_digits(0.1, false, FD_SHORTEST, 0, d, (int)sizeof d, &dp);
    CHECK(nd == 1 && d[0] == '1' && dp == 0);
    nd = float_digits(0.1, true, FD_SHORTEST, 0, d, (int)sizeof d, &dp);   /* float32(0.1) */
    CHECK(nd == 1 && d[0] == '1' && dp == 0);
    nd = float_digits(0.1, false, FD_DIGITS, 20, d, (int)sizeof d, &dp);
    CHECK(nd == 20 && memcmp(d, "10000000000000000555", 20) == 0 && dp == 0);
    nd = float_digits(9.5, false, FD_DIGITS, 1, d, (int)sizeof d, &dp);    /* half to even: 10 */
    CHECK(nd == 1 && d[0] == '1' && dp == 2);
    nd = float_digits(0.5, false, FD_FRAC, 0, d, (int)sizeof d, &dp);      /* rounds to 0 */
    CHECK_EQ(nd, 0);
    nd = float_digits(0.0004, false, FD_FRAC, 2, d, (int)sizeof d, &dp);
    CHECK_EQ(nd, 0);
    nd = float_digits(1e300, false, FD_DIGITS, 3, d, (int)sizeof d, &dp);
    CHECK(nd == 1 && d[0] == '1' && dp == 301);
}

#endif
