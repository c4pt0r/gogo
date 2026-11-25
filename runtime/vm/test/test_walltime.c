#include "test.h"
#include "platform.h"
#include "actor.h"
#include <string.h>

extern void platform_unix_set_clock(uint64_t (*fn)(void));

static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
static Vm wv;   /* only its wall clock is used */

/* The wall clock (platform.h): set, advancing with the monotonic clock,
 * forgotten by ms <= 0. */
TEST(walltime_set_and_advance) {
    platform_unix_set_clock(fake_clock);
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
    platform_unix_set_clock(NULL);
}

/* The floor: a time before the build is refused and changes nothing. */
TEST(walltime_floor) {
    platform_unix_set_clock(fake_clock);
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
    platform_unix_set_clock(NULL);
}

/* Each Vm keeps its own clock; vm_init starts it unknown. */
TEST(walltime_per_vm) {
    static Vm other;
    platform_unix_set_clock(fake_clock);
    fake_now = 10;
    memset(&other, 0, sizeof other);
    CHECK_EQ(vm_set_unix_time_ms(&wv, GOGO_BUILD_UNIX_MS + 5000LL), 1);
    CHECK(vm_unix_time_ms(&other) == 0);
    CHECK_EQ(vm_set_unix_time_ms(&other, GOGO_BUILD_UNIX_MS + 9000LL), 1);
    fake_now = 20;
    CHECK(vm_unix_time_ms(&wv) == GOGO_BUILD_UNIX_MS + 5010LL);
    CHECK(vm_unix_time_ms(&other) == GOGO_BUILD_UNIX_MS + 9010LL);
    vm_set_unix_time_ms(&wv, 0);
    CHECK(vm_unix_time_ms(&other) == GOGO_BUILD_UNIX_MS + 9010LL);
    platform_unix_set_clock(NULL);
}

TEST(random_varies) {
    uint32_t a = vm_random(&wv), b = vm_random(&wv), c = vm_random(&wv);
    CHECK(a != b || b != c);
}
