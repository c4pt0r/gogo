#include "test.h"
#include "platform.h"

extern void platform_unix_set_clock(uint64_t (*fn)(void));

static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }

/* The wall clock (platform.h): set, advancing with the monotonic clock,
 * forgotten by ms <= 0. */
TEST(walltime_set_and_advance) {
    platform_unix_set_clock(fake_clock);
    fake_now = 1000;
    platform_set_unix_time_ms((GOGO_BUILD_UNIX_MS + 100000LL));
    CHECK(platform_unix_time_ms() == (GOGO_BUILD_UNIX_MS + 100000LL));
    fake_now = 1750;
    CHECK(platform_unix_time_ms() == (GOGO_BUILD_UNIX_MS + 100750LL));
    platform_set_unix_time_ms((GOGO_BUILD_UNIX_MS + 200000LL));        /* a new set replaces the old offset */
    CHECK(platform_unix_time_ms() == (GOGO_BUILD_UNIX_MS + 200000LL));
    fake_now = 3000;
    CHECK(platform_unix_time_ms() == (GOGO_BUILD_UNIX_MS + 201250LL));
    platform_set_unix_time_ms(0);                      /* forget: unknown is 0 */
    CHECK(platform_unix_time_ms() == 0);
    fake_now = 9000;
    CHECK(platform_unix_time_ms() == 0);
    platform_set_unix_time_ms((GOGO_BUILD_UNIX_MS + 200000LL));
    platform_set_unix_time_ms(-5);                     /* negative forgets too */
    CHECK(platform_unix_time_ms() == 0);
    platform_unix_set_clock(NULL);
}

/* The floor: a time before the build is refused and changes nothing. */
TEST(walltime_floor) {
    platform_unix_set_clock(fake_clock);
    fake_now = 0;
    platform_set_unix_time_ms(0);
    CHECK_EQ(platform_set_unix_time_ms(GOGO_BUILD_UNIX_MS - 1), 0);
    CHECK(platform_unix_time_ms() == 0);
    CHECK_EQ(platform_set_unix_time_ms(GOGO_BUILD_UNIX_MS), 1);
    CHECK(platform_unix_time_ms() == GOGO_BUILD_UNIX_MS);
    CHECK_EQ(platform_set_unix_time_ms(1000), 0);          /* still the old time */
    CHECK(platform_unix_time_ms() == GOGO_BUILD_UNIX_MS);
    CHECK_EQ(platform_set_unix_time_ms(0), 1);             /* forgetting is always allowed */
    CHECK(platform_unix_time_ms() == 0);
    platform_unix_set_clock(NULL);
}

TEST(random_varies) {
    uint32_t a = platform_random(), b = platform_random(), c = platform_random();
    CHECK(a != b || b != c);
}
