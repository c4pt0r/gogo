/* Event ring: FIFO order, index wraparound, overflow policy. */
#include "test.h"
#include <errno.h>
#include "config.h"
#include "evq.h"
#include "platform.h"

static PortEvent ev(int32_t n) {
    PortEvent e;
    e.port = 1; e.gen = 2; e.a = n; e.b = -n; e.time_ms = (uint32_t)n * 3u;
    return e;
}

TEST(evq_fifo) {
    PortEvent e, o;
    evq_reset();
    CHECK(!evq_pop(&o));
    for (int32_t i = 0; i < 10; i++) { e = ev(i); CHECK(evq_push(&e)); }
    for (int32_t i = 0; i < 10; i++) {
        CHECK(evq_pop(&o));
        CHECK_EQ(o.a, i); CHECK_EQ(o.b, -i); CHECK_EQ(o.port, 1); CHECK_EQ(o.gen, 2);
        CHECK_EQ(o.time_ms, (uint32_t)i * 3u);
    }
    CHECK(!evq_pop(&o));
    CHECK_EQ(evq_dropped(), 0);
}

TEST(evq_wraparound) {
    PortEvent e, o;
    evq_reset();
    for (int32_t i = 0; i < (int32_t)(10 * GOGO_EVQ_CAP); i++) {
        e = ev(i);
        CHECK(evq_push(&e));
        CHECK(evq_pop(&o));
        CHECK_EQ(o.a, i);
    }
    CHECK(!evq_pop(&o));
    CHECK_EQ(evq_dropped(), 0);
}

TEST(evq_overflow) {
    PortEvent e, o;
    evq_reset();
    for (int32_t i = 0; i < (int32_t)GOGO_EVQ_CAP + 5; i++) {
        e = ev(i);
        CHECK_EQ(evq_push(&e), i < (int32_t)GOGO_EVQ_CAP);
    }
    CHECK_EQ(evq_dropped(), 5);
    for (int32_t i = 0; i < (int32_t)GOGO_EVQ_CAP; i++) {
        CHECK(evq_pop(&o));
        CHECK_EQ(o.a, i);
    }
    CHECK(!evq_pop(&o));
    evq_reset();
    CHECK_EQ(evq_dropped(), 0);
}

extern void platform_unix_set_clock(uint64_t (*fn)(void));

TEST(notify_latches_wait) {
    platform_unix_set_clock(0);
    uint64_t t0 = platform_now_ms();
    platform_notify();
    platform_wait(5000);
    CHECK(platform_now_ms() - t0 < 100);
    t0 = platform_now_ms();          /* latch was consumed: waits again */
    platform_wait(30);
    CHECK(platform_now_ms() - t0 >= 25);
}

/* platform_notify may run from a signal handler: it must not clobber the
 * errno of the code it interrupted. */
TEST(notify_preserves_errno) {
    errno = 1234;
    platform_notify();
    CHECK_EQ(errno, 1234);
    platform_wait(0);                /* consume the latch */
}
