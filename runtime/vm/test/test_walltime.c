#include "test.h"
#include "test_host.h"
#include "platform_unix.h"
#include "actor.h"
#include <string.h>
#include <stdlib.h>


static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
static Vm wv = { .host = &test_host };   /* only its host and wall clock are used */

/* The wall clock (host.h): set, advancing with the monotonic clock,
 * forgotten by ms <= 0. */
TEST(walltime_set_and_advance) {
    test_host_set_clock(fake_clock);
    fake_now = 1000;
    vm_set_unix_time_ms(&wv, (GOGO_BUILD_UNIX_MS + 100000LL));
    CHECK(vm_unix_time_ms(&wv) == (GOGO_BUILD_UNIX_MS + 100000LL));
    fake_now = 1750;
    CHECK(vm_unix_time_ms(&wv) == (GOGO_BUILD_UNIX_MS + 100750LL));
    vm_set_unix_time_ms(&wv, (GOGO_BUILD_UNIX_MS + 200000LL));        /* a new set replaces the old offset */
    CHECK(vm_unix_time_ms(&wv) == (GOGO_BUILD_UNIX_MS + 200000LL));
    fake_now = 3000;
    CHECK(vm_unix_time_ms(&wv) == (GOGO_BUILD_UNIX_MS + 201250LL));
    vm_set_unix_time_ms(&wv, 0);                      /* forget: unknown is 0 */
    CHECK(vm_unix_time_ms(&wv) == 0);
    fake_now = 9000;
    CHECK(vm_unix_time_ms(&wv) == 0);
    vm_set_unix_time_ms(&wv, (GOGO_BUILD_UNIX_MS + 200000LL));
    vm_set_unix_time_ms(&wv, -5);                     /* negative forgets too */
    CHECK(vm_unix_time_ms(&wv) == 0);
    test_host_set_clock(NULL);
}

/* The floor: a time before the build is refused and changes nothing. */
TEST(walltime_floor) {
    test_host_set_clock(fake_clock);
    fake_now = 0;
    vm_set_unix_time_ms(&wv, 0);
    CHECK_EQ(vm_set_unix_time_ms(&wv, GOGO_BUILD_UNIX_MS - 1), 0);
    CHECK(vm_unix_time_ms(&wv) == 0);
    CHECK_EQ(vm_set_unix_time_ms(&wv, GOGO_BUILD_UNIX_MS), 1);
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS);
    CHECK_EQ(vm_set_unix_time_ms(&wv, 1000), 0);          /* still the old time */
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS);
    CHECK_EQ(vm_set_unix_time_ms(&wv, 0), 1);             /* forgetting is always allowed */
    CHECK(vm_unix_time_ms(&wv) == 0);
    test_host_set_clock(NULL);
}

/* Each Vm keeps its own clock; vm_init starts it unknown. */
TEST(walltime_per_vm) {
    static Vm other;
    test_host_set_clock(fake_clock);
    fake_now = 10;
    memset(&other, 0, sizeof other);
    other.host = &test_host;
    CHECK_EQ(vm_set_unix_time_ms(&wv, GOGO_BUILD_UNIX_MS + 5000LL), 1);
    CHECK(vm_unix_time_ms(&other) == 0);
    CHECK_EQ(vm_set_unix_time_ms(&other, GOGO_BUILD_UNIX_MS + 9000LL), 1);
    fake_now = 20;
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS + 5010LL);
    CHECK(vm_unix_time_ms(&other) == GOGO_BUILD_UNIX_MS + 9010LL);
    vm_set_unix_time_ms(&wv, 0);
    CHECK(vm_unix_time_ms(&other) == GOGO_BUILD_UNIX_MS + 9010LL);
    test_host_set_clock(NULL);
}

TEST(random_varies) {
    uint32_t a = vm_random(&wv), b = vm_random(&wv), c = vm_random(&wv);
    CHECK(a != b || b != c);
}

/* A new time base (vm_start's t0) moves vm_now, not the wall clock. */
TEST(walltime_keeps_across_t0) {
    test_host_set_clock(fake_clock);
    fake_now = 500;
    wv.t0 = 0;
    CHECK_EQ(vm_set_unix_time_ms(&wv, GOGO_BUILD_UNIX_MS + 1000LL), 1);
    wv.t0 += vm_now(&wv);
    CHECK(vm_now(&wv) == 0);
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS + 1000LL);
    fake_now = 800;
    CHECK(vm_now(&wv) == 300);
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS + 1300LL);
    wv.t0 = 0;
    vm_set_unix_time_ms(&wv, 0);
    test_host_set_clock(NULL);
}

/* An RTC host (unix_time_ms / set_unix_time_ms) owns the wall clock; the
 * floor still guards a set. */
static int64_t rtc_ms;
static int rtc_sets;
static int64_t rtc_get(void *ctx) { CHECK(ctx == &rtc_ms); return rtc_ms; }
static int rtc_set(void *ctx, int64_t ms) { CHECK(ctx == &rtc_ms); rtc_sets++; rtc_ms = ms; return 1; }

TEST(walltime_rtc_host) {
    gogo_host h = test_host;
    h.unix_time_ms = rtc_get;
    h.set_unix_time_ms = rtc_set;
    Vm *v = calloc(1, sizeof *v);
    v->host = &h;
    v->host_ctx = &rtc_ms;
    rtc_ms = GOGO_BUILD_UNIX_MS + 42;
    rtc_sets = 0;
    CHECK(vm_unix_time_ms(v) == GOGO_BUILD_UNIX_MS + 42);
    CHECK_EQ(vm_set_unix_time_ms(v, GOGO_BUILD_UNIX_MS - 1), 0);   /* the floor: the host never sees it */
    CHECK_EQ(rtc_sets, 0);
    CHECK_EQ(vm_set_unix_time_ms(v, GOGO_BUILD_UNIX_MS + 7), 1);
    CHECK_EQ(rtc_sets, 1);
    CHECK(vm_unix_time_ms(v) == GOGO_BUILD_UNIX_MS + 7);
    CHECK(!v->wall_known);                                       /* the Vm's own clock is unused */
    free(v);
}

/* host->random when given; NULL: the Vm's own mixer, state per Vm. */
static uint32_t fixed_random(void *ctx) { (void)ctx; return 0x12345678u; }

TEST(random_host_and_fallback) {
    gogo_host h = test_host;
    Vm *v = calloc(1, sizeof *v);
    v->host = &h;
    h.random = fixed_random;
    CHECK_EQ(vm_random(v), 0x12345678u);
    CHECK_EQ(v->rand_x, 0u);
    h.random = NULL;
    uint32_t a = vm_random(v);
    CHECK_EQ(v->rand_x, a);
    uint32_t b = vm_random(v);
    CHECK(a != b);
    CHECK_EQ(v->rand_x, b);
    free(v);
}
