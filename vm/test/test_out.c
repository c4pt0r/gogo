#include "test.h"
#include "out.h"
#include <string.h>

static char cap[64];
static size_t cap_n;

static void capture(const char *s, size_t n) {
    memcpy(cap + cap_n, s, n);
    cap_n += n;
}

static void expect_int(int32_t v, const char *want) {
    Out o;
    cap_n = 0;
    out_init(&o, capture);
    out_int(&o, v);
    out_flush(&o);
    CHECK_EQ(cap_n, strlen(want));
    CHECK(cap_n == strlen(want) && memcmp(cap, want, cap_n) == 0);
}

static void expect_int64(int64_t v, const char *want) {
    Out o;
    cap_n = 0;
    out_init(&o, capture);
    out_int64(&o, v);
    out_flush(&o);
    CHECK_EQ(cap_n, strlen(want));
    CHECK(cap_n == strlen(want) && memcmp(cap, want, cap_n) == 0);
}

TEST(out_int_extremes) {
    expect_int(INT32_MIN, "-2147483648");
    expect_int(INT32_MAX, "2147483647");
    expect_int(0, "0");
    expect_int(-1, "-1");
    expect_int(10, "10");
    expect_int(-1000000000, "-1000000000");
}

TEST(out_int64_extremes) {
    expect_int64(INT64_MIN, "-9223372036854775808");
    expect_int64(INT64_MAX, "9223372036854775807");
    expect_int64(0, "0");
    expect_int64(-4294967296LL, "-4294967296");
}
