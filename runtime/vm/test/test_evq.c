/* Event ring: FIFO order, index wraparound, overflow policy. */
#include "test.h"
#include "test_host.h"
#include <errno.h>
#include "config.h"
#include "evq.h"
#include "actor.h"
#include "platform_unix.h"

static Vm qv, qv2;   /* only their rings are used */

static PortEvent ev(int32_t n) {
    PortEvent e;
    e.port = 1; e.gen = 2; e.a = n; e.b = -n; e.time_ms = (uint32_t)n * 3u;
    return e;
}

TEST(evq_fifo) {
    PortEvent e, o;
    evq_reset(&qv);
    CHECK(!evq_pop(&qv, &o));
    for (int32_t i = 0; i < 10; i++) { e = ev(i); CHECK(evq_push(&qv, &e)); }
    for (int32_t i = 0; i < 10; i++) {
        CHECK(evq_pop(&qv, &o));
        CHECK_EQ(o.a, i); CHECK_EQ(o.b, -i); CHECK_EQ(o.port, 1); CHECK_EQ(o.gen, 2);
        CHECK_EQ(o.time_ms, (uint32_t)i * 3u);
    }
    CHECK(!evq_pop(&qv, &o));
    CHECK_EQ(evq_dropped(&qv), 0);
}

TEST(evq_wraparound) {
    PortEvent e, o;
    evq_reset(&qv);
    for (int32_t i = 0; i < (int32_t)(10 * GOGO_EVQ_CAP); i++) {
        e = ev(i);
        CHECK(evq_push(&qv, &e));
        CHECK(evq_pop(&qv, &o));
        CHECK_EQ(o.a, i);
    }
    CHECK(!evq_pop(&qv, &o));
    CHECK_EQ(evq_dropped(&qv), 0);
}

TEST(evq_overflow) {
    PortEvent e, o;
    evq_reset(&qv);
    for (int32_t i = 0; i < (int32_t)GOGO_EVQ_CAP + 5; i++) {
        e = ev(i);
        CHECK_EQ(evq_push(&qv, &e), i < (int32_t)GOGO_EVQ_CAP);
    }
    CHECK_EQ(evq_dropped(&qv), 5);
    for (int32_t i = 0; i < (int32_t)GOGO_EVQ_CAP; i++) {
        CHECK(evq_pop(&qv, &o));
        CHECK_EQ(o.a, i);
    }
    CHECK(!evq_pop(&qv, &o));
    evq_reset(&qv);
    CHECK_EQ(evq_dropped(&qv), 0);
}

/* Each Vm has its own ring: events pushed to one are never seen by the
 * other, and a full ring drops only its own. */
TEST(two_rings) {
    PortEvent e, o;
    evq_reset(&qv);
    evq_reset(&qv2);
    for (int32_t i = 0; i < 5; i++) { e = ev(i); CHECK(evq_push(&qv, &e)); }
    CHECK(!evq_empty(&qv));
    CHECK(evq_empty(&qv2));
    CHECK(!evq_pop(&qv2, &o));
    e = ev(100);
    CHECK(evq_push(&qv2, &e));
    for (int32_t i = 0; i < 5; i++) { CHECK(evq_pop(&qv, &o)); CHECK_EQ(o.a, i); }
    CHECK(!evq_pop(&qv, &o));
    CHECK(evq_pop(&qv2, &o));
    CHECK_EQ(o.a, 100);
    CHECK(!evq_pop(&qv2, &o));
    for (int32_t i = 0; i < (int32_t)GOGO_EVQ_CAP + 3; i++) { e = ev(i); (void)evq_push(&qv, &e); }
    CHECK_EQ(evq_dropped(&qv), 3);
    CHECK_EQ(evq_dropped(&qv2), 0);
    e = ev(7);
    CHECK(evq_push(&qv2, &e));                /* qv full does not fill qv2 */
    CHECK(evq_pop(&qv2, &o));
    CHECK_EQ(o.a, 7);
    CHECK(evq_empty(&qv2));
    evq_reset(&qv);
}


TEST(notify_latches_wait) {
    test_host_set_clock(0);
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
